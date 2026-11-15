// Offline tests of the websocket mod (registry + Lua bindings) through a fake StormFabric
// host: a minimal Lua stack with per-VM globals, a fake runtime and a fake storage.
// Needs tests/echo_server.py running.   mod_test.exe [--port 8765]
#include "sw_mod_sdk.h"
#include "../src/ws_client.h"
#include "../src/ws_registry.h"

#include <windows.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

// ---- Fake Lua -------------------------------------------------------------------------

struct Value;
using Table = std::map<std::string, Value>;
struct Value {
    SwLuaType type = SW_LUA_TYPE_NIL;
    bool b = false;
    double n = 0;
    std::string s;
    std::shared_ptr<Table> t;
    SwLuaCallback fn = nullptr;
};
static Value Nil() { return Value(); }
static Value Num(double n) { Value v; v.type = SW_LUA_TYPE_NUMBER; v.n = n; return v; }
static Value Int(double n) { Value v; v.type = SW_LUA_TYPE_INTEGER; v.n = n; return v; }
static Value Bool(bool b) { Value v; v.type = SW_LUA_TYPE_BOOLEAN; v.b = b; return v; }
static Value Str(const std::string& s) { Value v; v.type = SW_LUA_TYPE_STRING; v.s = s; return v; }

struct Vm {
    std::vector<Value> stack;
    Table globals;
    std::string lib = "websocket"; // global name of the library table in this VM
};

static Value* At(void* L, int index) {
    Vm* vm = (Vm*)L;
    const int size = (int)vm->stack.size();
    const int i = index > 0 ? index - 1 : size + index;
    if (index == 0 || i < 0 || i >= size) return nullptr;
    return &vm->stack[(size_t)i];
}
static void Push(void* L, Value v) { ((Vm*)L)->stack.push_back(std::move(v)); }

// Same rules as the loader (src/native_lua.cpp ApiArg*): positive indices only, a negative
// or zero index returns the default; argBool: nil -> false, non-boolean -> true.
static int F_getTop(void* L) { return (int)((Vm*)L)->stack.size(); }
static double F_argNumber(void* L, int i, double d) {
    Value* v = i > 0 ? At(L, i) : nullptr;
    return v && (v->type == SW_LUA_TYPE_NUMBER || v->type == SW_LUA_TYPE_INTEGER) ? v->n : d;
}
static bool F_argBool(void* L, int i, bool d) {
    Value* v = i > 0 ? At(L, i) : nullptr;
    if (!v) return d;
    if (v->type == SW_LUA_TYPE_NIL) return false;
    return v->type == SW_LUA_TYPE_BOOLEAN ? v->b : true;
}
static size_t F_argString(void* L, int i, char* out, size_t cap) {
    Value* v = i > 0 ? At(L, i) : nullptr;
    if (!v || v->type != SW_LUA_TYPE_STRING || !cap) return 0;
    const size_t n = v->s.size() < cap - 1 ? v->s.size() : cap - 1;
    memcpy(out, v->s.data(), n);
    out[n] = 0;
    return n;
}
static bool F_checkStack(void*, int n) { return n >= 0 && n < 100; }
static void F_pushNumber(void* L, double n) { Push(L, Num(n)); }
static void F_pushBool(void* L, bool b) { Push(L, Bool(b)); }

static SwLuaStatus X_pushString(const SwModHost*, void* L, const char* b, size_t n) {
    if (!b && n) return SW_LUA_INVALID_ARGUMENT;
    Push(L, Str(std::string(b ? b : "", n)));
    return SW_LUA_OK;
}
static SwLuaStatus X_pushNil(const SwModHost*, void* L) { Push(L, Nil()); return SW_LUA_OK; }
static SwLuaStatus X_pushInteger(const SwModHost*, void* L, int64_t v) { Push(L, Int((double)v)); return SW_LUA_OK; }
static SwLuaStatus X_createTable(const SwModHost*, void* L, uint32_t) {
    Value v;
    v.type = SW_LUA_TYPE_TABLE;
    v.t = std::make_shared<Table>();
    Push(L, v);
    return SW_LUA_OK;
}
static SwLuaStatus X_getField(const SwModHost*, void* L, int i, const char* k) {
    Value* v = At(L, i);
    if (!v) return SW_LUA_OUT_OF_RANGE;
    if (v->type != SW_LUA_TYPE_TABLE) return SW_LUA_TYPE_MISMATCH;
    auto it = v->t->find(k);
    Push(L, it == v->t->end() ? Nil() : it->second);
    return SW_LUA_OK;
}
static SwLuaStatus X_setField(const SwModHost*, void* L, int i, const char* k) {
    Value* v = At(L, i);
    Vm* vm = (Vm*)L;
    if (!v || vm->stack.empty()) return SW_LUA_OUT_OF_RANGE;
    if (v->type != SW_LUA_TYPE_TABLE) return SW_LUA_TYPE_MISMATCH;
    (*v->t)[k] = vm->stack.back();
    vm->stack.pop_back();
    return SW_LUA_OK;
}
static SwLuaStatus X_getType(const SwModHost*, void* L, int i, SwLuaType* t) {
    Value* v = At(L, i);
    *t = v ? v->type : SW_LUA_TYPE_NONE;
    return SW_LUA_OK;
}
static SwLuaStatus X_pop(const SwModHost*, void* L, int n) {
    Vm* vm = (Vm*)L;
    if (n < 0 || n > (int)vm->stack.size()) return SW_LUA_OUT_OF_RANGE;
    vm->stack.resize(vm->stack.size() - (size_t)n);
    return SW_LUA_OK;
}
static SwLuaStatus X_raiseError(const SwModHost*, void*, const char*, size_t) { return SW_LUA_UNSUPPORTED; }
static SwLuaStatus X_copyString(const SwModHost*, void* L, int i, char* out, size_t cap, size_t* len) {
    Value* v = At(L, i);
    if (!v) return SW_LUA_OUT_OF_RANGE;
    if (v->type != SW_LUA_TYPE_STRING) return SW_LUA_TYPE_MISMATCH;
    *len = v->s.size();
    if (cap) {
        const size_t n = v->s.size() < cap - 1 ? v->s.size() : cap - 1;
        memcpy(out, v->s.data(), n);
        out[n] = 0;
    }
    return v->s.size() < cap ? SW_LUA_OK : SW_LUA_BUFFER_TOO_SMALL;
}
static SwLuaStatus X_getGlobal(const SwModHost*, void* L, const char* name) {
    Vm* vm = (Vm*)L;
    auto it = vm->globals.find(name);
    Push(L, it == vm->globals.end() ? Nil() : it->second);
    return SW_LUA_OK;
}
static SwLuaStatus X_call(const SwModHost*, void*, int, int, char*, size_t) { return SW_LUA_UNSUPPORTED; }

static SwLuaExtrasApi g_extras = {sizeof(SwLuaExtrasApi), X_pushString, X_pushNil, X_pushInteger, X_createTable,
                                  X_getField, X_setField, X_getType, X_pop,
                                  X_raiseError, X_copyString, X_getGlobal, X_call};

static std::vector<SwLuaFunction> g_library;
static std::string g_libraryName;
static bool F_registerAddonLibrary(const SwModHost*, const char* name, const SwLuaFunction* f, size_t n) {
    g_libraryName = name;
    g_library.assign(f, f + n);
    return true;
}
static std::vector<SwLuaFunction> g_mcLibrary;
static std::string g_mcLibraryName;
static bool F_registerLibrary(const SwModHost*, const char* name, const SwLuaFunction* f, size_t n) {
    g_mcLibraryName = name;
    g_mcLibrary.assign(f, f + n);
    return true;
}
static bool F_enableQol(const SwModHost*) { return true; }
static void F_disableQol(const SwModHost*) {}

static SwLuaApi g_lua = {sizeof(SwLuaApi), F_enableQol, F_disableQol, F_registerLibrary, F_getTop, F_argNumber,
                         F_argBool, F_argString, F_checkStack, F_pushNumber, F_pushBool, nullptr, &g_extras,
                         F_registerAddonLibrary};

// ---- Fake runtime / storage / host ---------------------------------------------------

static uint64_t g_caps = SW_CAP_LUA_ADDON | SW_CAP_MAIN_THREAD_TASKS | SW_CAP_MOD_STORAGE;
static SwEventCallback g_eventCb = nullptr;
static SwTaskCallback g_taskCb = nullptr;
static uint64_t R_caps() { return g_caps; }
static SwResult R_subscribe(const SwModHost*, uint64_t, SwEventCallback cb, void*, SwHandle* h) {
    g_eventCb = cb;
    *h = 1;
    return SW_OK;
}
static SwResult R_unsubscribe(const SwModHost*, SwHandle) { return SW_OK; }
static SwResult R_run(const SwModHost*, SwTaskCallback, void*, SwHandle*) { return SW_OK; }
static SwResult R_schedule(const SwModHost*, double, double, SwTaskCallback cb, void*, SwHandle* h) {
    g_taskCb = cb;
    *h = 2;
    return SW_OK;
}
static SwResult R_cancel(const SwModHost*, SwHandle) { return SW_OK; }
static double R_time() { return 0; }
static bool R_isGame() { return true; }
static SwResult R_simTime(double*) { return SW_UNAVAILABLE; }
static SwRuntimeApi g_runtime = {offsetof(SwRuntimeApi, subscribeSimulationTick), R_caps, R_subscribe, R_unsubscribe,
                                 R_run, R_schedule, R_cancel, R_time, R_isGame, R_simTime};

static std::map<std::string, std::string> g_storage;
static SwStorageStatus S_read(const SwModHost*, const char* key, uint8_t* buf, size_t cap, size_t* size) {
    auto it = g_storage.find(key);
    if (it == g_storage.end()) { *size = 0; return SW_STORAGE_NOT_FOUND; }
    *size = it->second.size();
    if (cap < it->second.size()) return SW_STORAGE_BUFFER_TOO_SMALL;
    memcpy(buf, it->second.data(), it->second.size());
    return SW_STORAGE_OK;
}
static SwStorageStatus S_write(const SwModHost*, const char* key, const uint8_t* b, size_t n) {
    g_storage[key].assign((const char*)b, n);
    return SW_STORAGE_OK;
}
static SwStorageApi g_storageApi = {offsetof(SwStorageApi, readData), S_read, S_write, nullptr, nullptr};

static std::vector<std::string> g_logs;
static void H_log(const char*, const char* m) {
    printf("      log: %s\n", m);
    g_logs.push_back(m);
}

static SwEngineApi g_api = {SW_ENGINE_API_VERSION, sizeof(SwEngineApi), nullptr, &g_lua, nullptr, &g_runtime,
                            nullptr, nullptr, &g_storageApi, nullptr, nullptr};
static SwModHost g_hostStruct = {SW_MOD_SDK_VERSION, nullptr, "websocket", "WebSocket", "test", "test", ".",
                                 nullptr, nullptr, H_log, &g_api};

extern "C" bool SwModInit(const SwModHost* host);
extern "C" void SwModUnload(void);

// A new addon VM (or microcontroller VM when mc): the loader installs the library table
// registered for that kind of VM before the script runs.
static std::unique_ptr<Vm> NewVm(bool mc = false) {
    auto vm = std::make_unique<Vm>();
    const auto& functions = mc ? g_mcLibrary : g_library;
    vm->lib = mc ? g_mcLibraryName : g_libraryName;
    Value table;
    table.type = SW_LUA_TYPE_TABLE;
    table.t = std::make_shared<Table>();
    for (const SwLuaFunction& f : functions) {
        Value fn;
        fn.type = SW_LUA_TYPE_FUNCTION;
        fn.fn = f.callback;
        (*table.t)[f.name] = fn;
    }
    vm->globals[vm->lib] = table;
    return vm;
}

// ---- Test helpers ---------------------------------------------------------------------

static int g_failed = 0, g_passed = 0, g_port = 8765;
static bool g_ok = true;
#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("    FAIL line %d: %s -- ", __LINE__, #cond);            \
            printf(__VA_ARGS__);                                             \
            printf("\n");                                                    \
            g_ok = false;                                                    \
        }                                                                    \
    } while (0)

// Calls websocket.<name>(args...) and returns its results. Checks the stack contract:
// arguments untouched, exactly `returned` values pushed on top.
static std::vector<Value> Call(Vm& vm, const char* name, std::vector<Value> args) {
    Value& table = vm.globals[vm.lib];
    auto it = table.t ? table.t->find(name) : Table::iterator();
    if (!table.t || it == table.t->end()) {
        CHECK(false, "no function %s", name);
        return {};
    }
    vm.stack = args;
    const int n = it->second.fn(&vm, nullptr);
    CHECK(n >= 0 && vm.stack.size() == args.size() + (size_t)n, "%s: stack %zu args %zu results %d", name,
          vm.stack.size(), args.size(), n);
    for (size_t i = 0; i < args.size() && i < vm.stack.size(); ++i)
        CHECK(vm.stack[i].type == args[i].type, "%s: argument %zu modified", name, i + 1);
    std::vector<Value> out;
    if (n > 0 && vm.stack.size() >= (size_t)n) out.assign(vm.stack.end() - n, vm.stack.end());
    vm.stack.clear();
    return out;
}

static std::string Url(const char* path = "/") { return "ws://127.0.0.1:" + std::to_string(g_port) + path; }

// Polls until an event of `type` arrives for the VM (optionally for one id).
static std::vector<Value> WaitEvent(Vm& vm, const char* type, double id = 0, double seconds = 10) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds((int)(seconds * 1000));
    while (std::chrono::steady_clock::now() < end) {
        auto r = Call(vm, "poll", id ? std::vector<Value>{Num(id)} : std::vector<Value>{});
        if (r.size() >= 2) {
            if (r[1].s == type) return r;
            printf("      (event %s %s)\n", r[1].s.c_str(), r.size() > 2 ? r[2].s.c_str() : "");
        }
        Sleep(5);
    }
    return {};
}

static bool Init(const char* config) {
    g_storage.clear();
    if (config) g_storage["websocket.cfg"] = config;
    g_logs.clear();
    return SwModInit(&g_hostStruct);
}

static void Run(const char* name, void (*fn)()) {
    printf("[ %s ]\n", name);
    g_ok = true;
    fn();
    printf("    %s\n", g_ok ? "ok" : "FAILED");
    (g_ok ? g_passed : g_failed)++;
}

static bool Logged(const char* needle) {
    for (const std::string& l : g_logs)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}

// ---- Tests --------------------------------------------------------------------------------

static void TestRegistryHelpers() {
    using namespace wsc;
    CHECK(UrlHost("wss://User:pw@Example.COM:8443/x?y") == "example.com", "%s", UrlHost("wss://User:pw@Example.COM:8443/x?y").c_str());
    CHECK(UrlHost("ws://[::1]:80/") == "[::1]", "ipv6");
    CHECK(UrlHost("ws://host") == "host", "bare host");
    CHECK(HostAllowed({"*"}, "a.b"), "*");
    CHECK(HostAllowed({"*.example.com"}, "api.example.com") && !HostAllowed({"*.example.com"}, "example.com") &&
              !HostAllowed({"*.example.com"}, "badexample.com"), "suffix");
    CHECK(!HostAllowed({}, "a") && !HostAllowed({"*"}, ""), "empty");
    Limits l;
    std::string err;
    CHECK(ParseConfig(DefaultConfigText(), &l, &err), "defaults parse: %s", err.c_str());
    CHECK(ParseConfig("allow_hosts= A.com , *.b.org\nmax_connections=2 # c\n\nidle_timeout=1.5\r\n", &l, &err) &&
              l.allowHosts.size() == 2 && l.allowHosts[0] == "a.com" && l.maxConnections == 2 && l.idleSeconds == 1.5,
          "custom parse: %s", err.c_str());
    CHECK(!ParseConfig("max_connections=0", &l, &err) && !ParseConfig("nope=1", &l, &err) &&
              !ParseConfig("max_message_bytes=2000000", &l, &err) && !ParseConfig("junk", &l, &err), "bad configs");
}

static void ResetLibraries() {
    g_library.clear();
    g_libraryName.clear();
    g_mcLibrary.clear();
    g_mcLibraryName.clear();
}

static void TestInitChecks() {
    // Older build: no addon VMs, the microcontroller library alone is enough.
    ResetLibraries();
    g_caps = SW_CAP_MAIN_THREAD_TASKS | SW_CAP_MOD_STORAGE;
    CHECK(Init(nullptr), "init without SW_CAP_LUA_ADDON (microcontrollers only)");
    CHECK(g_library.empty() && g_mcLibrary.size() == 8, "mc only (%zu, %zu)", g_library.size(), g_mcLibrary.size());
    CHECK(Logged("libraries: addon=no microcontroller=yes"), "libraries logged");
    SwModUnload();
    ResetLibraries();
    CHECK(!Init("microcontrollers=0\n"), "nothing to register: refused");
    g_caps = SW_CAP_LUA_ADDON | SW_CAP_MAIN_THREAD_TASKS | SW_CAP_MOD_STORAGE;

    const uint32_t saved = g_extras.structSize;
    g_extras.structSize = offsetof(SwLuaExtrasApi, copyString);
    CHECK(!Init(nullptr), "refused without extras->copyString");
    g_extras.structSize = offsetof(SwLuaExtrasApi, getGlobal);
    CHECK(Init(nullptr), "init without extras->getGlobal (shared owner)");
    CHECK(Logged("share one connection owner"), "shared owner logged");
    SwModUnload();
    g_extras.structSize = saved;

    ResetLibraries();
    CHECK(Init("microcontrollers=0\n"), "addon only");
    CHECK(g_library.size() == 8 && g_mcLibrary.empty(), "microcontrollers=0 respected");
    SwModUnload();

    ResetLibraries();
    CHECK(Init(nullptr), "init");
    CHECK(g_libraryName == "websocket" && g_library.size() == 8, "library %s (%zu)", g_libraryName.c_str(), g_library.size());
    CHECK(g_mcLibraryName == "websocket" && g_mcLibrary.size() == 8, "mc library %s (%zu)", g_mcLibraryName.c_str(), g_mcLibrary.size());
    CHECK(g_storage.count("websocket.cfg") && g_storage["websocket.cfg"] == wsc::DefaultConfigText(), "default config written");
    CHECK(g_eventCb && g_taskCb, "subscribed + scheduled");
    SwModUnload();
}

static void TestBasics() {
    CHECK(Init(nullptr), "init");
    auto vm = NewVm();
    auto r = Call(*vm, "version", {});
    CHECK(r.size() == 1 && r[0].type == SW_LUA_TYPE_STRING && !r[0].s.empty(), "version");
    r = Call(*vm, "connect", {Num(42)});
    CHECK(r.size() == 2 && r[0].type == SW_LUA_TYPE_NIL && r[1].s.find("string") != std::string::npos, "connect(42)");
    r = Call(*vm, "connect", {Str("http://127.0.0.1/")});
    CHECK(r.size() == 2 && r[0].type == SW_LUA_TYPE_NIL, "connect(http)");
    r = Call(*vm, "connect", {Str(Url()), Str("bogus=1")});
    CHECK(r.size() == 2 && r[0].type == SW_LUA_TYPE_NIL && r[1].s.find("unknown option") != std::string::npos, "bad options: %s",
          r.size() > 1 ? r[1].s.c_str() : "");
    r = Call(*vm, "send", {Num(999), Str("x")});
    CHECK(r.size() == 1 && r[0].type == SW_LUA_TYPE_BOOLEAN && !r[0].b, "send unknown id");
    r = Call(*vm, "state", {Str("nope")});
    CHECK(r.size() == 1 && r[0].s == "closed", "state bad id");
    r = Call(*vm, "poll", {});
    CHECK(r.empty(), "poll nothing");
    r = Call(*vm, "poll", {Num(-1)});
    CHECK(r.empty(), "poll bad id");

    r = Call(*vm, "connect", {Str(Url()), Nil()});
    CHECK(r.size() == 1 && r[0].type == SW_LUA_TYPE_INTEGER && r[0].n >= 1, "connect returns an integer id");
    const double id = r.empty() ? 0 : r[0].n;
    CHECK(Logged("connect #") && Logged("ws://127.0.0.1:"), "connection logged");
    r = Call(*vm, "state", {Num(id)});
    CHECK(r.size() == 1 && (r[0].s == "connecting" || r[0].s == "open"), "state %s", r.empty() ? "" : r[0].s.c_str());
    r = WaitEvent(*vm, "open");
    CHECK(r.size() == 2 && r[0].n == id && r[0].type == SW_LUA_TYPE_INTEGER, "open event");
    // Regression (seen in game): the owner token must be read back on later calls, so a
    // poll from another callback of the same VM keeps the connection alive.
    const double token = (*vm->globals["websocket"].t)["__owner"].n;
    Call(*vm, "state", {Num(id)});
    CHECK((*vm->globals["websocket"].t)["__owner"].n == token, "owner token stable across calls");
    CHECK(Call(*vm, "state", {Num(id)})[0].s == "open", "state open");

    r = Call(*vm, "send", {Num(id), Str("hi")});
    CHECK(r.size() == 1 && r[0].b, "send");
    r = WaitEvent(*vm, "message", id);
    CHECK(r.size() == 4 && r[2].s == "hi" && r[3].type == SW_LUA_TYPE_BOOLEAN && !r[3].b, "echo text");

    std::string bin("a\0b\xff", 4);
    r = Call(*vm, "send", {Int(id), Str(bin), Bool(true)});
    CHECK(r.size() == 1 && r[0].b, "send binary (integer id)");
    r = WaitEvent(*vm, "message");
    CHECK(r.size() == 4 && r[2].s == bin && r[3].b, "echo binary");

    Call(*vm, "send", {Num(id), Str("one")});
    Call(*vm, "send", {Num(id), Str("two")});
    Sleep(200);
    r = Call(*vm, "pending", {Num(id)});
    CHECK(r.size() == 1 && r[0].n == 2, "pending 2 (%g)", r.empty() ? -1.0 : r[0].n);
    r = Call(*vm, "receive", {Num(id)});
    CHECK(r.size() == 2 && r[0].s == "one" && !r[1].b, "receive one");
    r = Call(*vm, "receive", {Num(id)});
    CHECK(r.size() == 2 && r[0].s == "two", "receive two");
    r = Call(*vm, "receive", {Num(id)});
    CHECK(r.empty(), "receive nothing");

    CHECK(!Call(*vm, "close", {Num(id), Num(1005)})[0].b, "reserved code refused");
    CHECK(!Call(*vm, "close", {Num(id), Num(70000)})[0].b, "out of range code refused");
    CHECK(!Call(*vm, "close", {Num(id), Num(1000), Str(std::string(124, 'x'))})[0].b, "long reason refused");
    r = Call(*vm, "close", {Num(id), Num(4000), Str("bye")});
    CHECK(r.size() == 1 && r[0].b, "close");
    r = WaitEvent(*vm, "close", id);
    CHECK(r.size() == 4 && r[0].n == id && r[3].n == 4000, "close event code %g", r.size() > 3 ? r[3].n : -1.0);
    CHECK(Call(*vm, "state", {Num(id)})[0].s == "closed", "closed");
    CHECK(Call(*vm, "pending", {Num(id)})[0].n == 0, "released");
    SwModUnload();
    CHECK(wsc::Connection::LiveThreads() == 0, "threads %d", wsc::Connection::LiveThreads());
}

static void TestOwners() {
    CHECK(Init(nullptr), "init");
    auto a = NewVm(), b = NewVm();
    auto r = Call(*a, "connect", {Str(Url())});
    const double id = r.size() == 1 ? r[0].n : 0;
    CHECK(id > 0, "connect from A");
    auto& tableA = *a->globals["websocket"].t;
    CHECK(tableA.count("__owner") && tableA["__owner"].n >= 1, "owner token set on A");
    CHECK(!WaitEvent(*a, "open").empty(), "A open");
    Call(*a, "send", {Num(id), Str("for A")});
    Sleep(200);
    CHECK(Call(*b, "poll", {}).empty(), "B sees no event");
    CHECK(Call(*b, "poll", {Num(id)}).empty(), "B cannot poll A's id");
    CHECK(Call(*b, "state", {Num(id)})[0].s == "closed", "B sees A's id as closed");
    CHECK(!Call(*b, "send", {Num(id), Str("x")})[0].b, "B cannot send on A's id");
    CHECK(!Call(*b, "close", {Num(id)})[0].b, "B cannot close A's id");
    auto& tableB = *b->globals["websocket"].t;
    CHECK(tableB.count("__owner") && tableB["__owner"].n != tableA["__owner"].n, "distinct owners");
    r = WaitEvent(*a, "message");
    CHECK(r.size() == 4 && r[2].s == "for A", "A still gets its message");
    // A script that removed the global falls back to the shared owner 0.
    auto c = NewVm();
    Value fn = (*c->globals["websocket"].t)["connect"];
    c->globals.erase("websocket");
    c->stack = {Str(Url())};
    const int n = fn.fn(c.get(), nullptr);
    CHECK(n == 1 && c->stack.size() == 2, "connect without global table works (%d)", n);
    SwModUnload();
}

static void TestMicrocontroller() {
    CHECK(Init(nullptr), "init");
    auto mc = NewVm(true), mc2 = NewVm(true), addon = NewVm(false);
    auto r = Call(*mc, "connect", {Str(Url())});
    const double id = r.size() == 1 ? r[0].n : 0;
    CHECK(id > 0, "connect from a microcontroller");
    r = WaitEvent(*mc, "open");
    CHECK(r.size() == 2 && r[0].n == id, "open in the microcontroller VM");
    CHECK(Call(*mc, "send", {Num(id), Str("from mc")})[0].b, "send");
    r = WaitEvent(*mc, "message", id);
    CHECK(r.size() == 4 && r[2].s == "from mc", "echo in the microcontroller VM");
    // Every microcontroller and addon VM is a separate owner.
    CHECK(Call(*mc2, "poll", {}).empty() && Call(*addon, "poll", {}).empty(), "other VMs see nothing");
    CHECK(Call(*addon, "state", {Num(id)})[0].s == "closed", "addon cannot reach the mc connection");
    CHECK(Call(*mc2, "state", {Num(id)})[0].s == "closed", "another mc cannot reach it");
    // A vehicle despawn destroys the VM silently: the idle sweep releases the connection.
    SwModUnload();
    CHECK(wsc::Connection::LiveThreads() == 0, "threads %d", wsc::Connection::LiveThreads());
}

static void TestConfigLimits() {
    CHECK(Init("allow_hosts=example.com\n"), "init");
    auto vm = NewVm();
    auto r = Call(*vm, "connect", {Str(Url())});
    CHECK(r.size() == 2 && r[0].type == SW_LUA_TYPE_NIL && r[1].s.find("not allowed") != std::string::npos, "host refused");
    CHECK(Logged("refused ws://127.0.0.1:"), "refusal logged");
    SwModUnload();

    CHECK(Init("max_connections=2\nlog_connections=0\n"), "init");
    vm = NewVm();
    CHECK(Call(*vm, "connect", {Str(Url())}).size() == 1, "1st");
    CHECK(Call(*vm, "connect", {Str(Url())}).size() == 1, "2nd");
    r = Call(*vm, "connect", {Str(Url())});
    CHECK(r.size() == 2 && r[1].s.find("too many") != std::string::npos, "3rd refused");
    CHECK(!Logged("connect #"), "log_connections=0 respected");
    SwModUnload();

    CHECK(Init("max_connections=oops\n"), "init with a bad config still works");
    CHECK(Logged("websocket.cfg ignored"), "bad config reported");
    SwModUnload();
}

static void TestIdleAndWorldEnd() {
    CHECK(Init("idle_timeout=1\n"), "init");
    auto vm = NewVm();
    auto r = Call(*vm, "connect", {Str(Url())});
    const double id = r.size() == 1 ? r[0].n : 0;
    CHECK(!WaitEvent(*vm, "open").empty(), "open");
    Sleep(1500);
    g_taskCb(nullptr); // the loader's 1 s sweep task
    CHECK(Call(*vm, "state", {Num(id)})[0].s == "closed", "idle connection released");
    CHECK(Logged("released idle"), "idle release logged");

    SwEvent created = {sizeof(SwEvent), SW_EVENT_ADDON_LUA_CREATED};
    SwEvent destroyed = {sizeof(SwEvent), SW_EVENT_ADDON_LUA_DESTROYED};
    g_eventCb(&created, nullptr);
    g_eventCb(&created, nullptr);
    r = Call(*vm, "connect", {Str(Url())});
    const double id2 = r.size() == 1 ? r[0].n : 0;
    CHECK(id2 > 0, "connect");
    g_eventCb(&destroyed, nullptr);
    CHECK(Call(*vm, "state", {Num(id2)})[0].s != "closed", "one VM left: kept");
    g_eventCb(&destroyed, nullptr);
    CHECK(Call(*vm, "state", {Num(id2)})[0].s == "closed", "no VM left: closed");
    g_eventCb(&destroyed, nullptr); // unbalanced (mod loaded mid-game): ignored
    SwModUnload();
}

static void TestUnloadWithTraffic() {
    CHECK(Init(nullptr), "init");
    auto vm = NewVm();
    for (int i = 0; i < 10; ++i) Call(*vm, "connect", {Str(Url())});
    Call(*vm, "connect", {Str("ws://10.255.255.1/")}); // still connecting
    Sleep(300);
    for (double id = 1; id < 200; ++id) Call(*vm, "send", {Num(id), Str(std::string(50000, 'z'))});
    const auto start = std::chrono::steady_clock::now();
    SwModUnload();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    CHECK(ms < 3000, "unload took %.0f ms", ms);
    CHECK(wsc::Connection::LiveThreads() == 0, "threads %d", wsc::Connection::LiveThreads());
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--port") && i + 1 < argc) g_port = atoi(argv[++i]);
    Run("registry helpers", TestRegistryHelpers);
    Run("init checks", TestInitChecks);
    Run("lua api", TestBasics);
    Run("owner isolation", TestOwners);
    Run("microcontroller library", TestMicrocontroller);
    Run("config limits", TestConfigLimits);
    Run("idle sweep and world end", TestIdleAndWorldEnd);
    Run("unload with traffic", TestUnloadWithTraffic);
    printf("\nlive threads at exit: %d\n%d passed, %d failed\n", wsc::Connection::LiveThreads(), g_passed, g_failed);
    return g_failed || wsc::Connection::LiveThreads() ? 1 : 0;
}
