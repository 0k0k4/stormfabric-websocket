// StormFabric mod: WebSocket client (ws:// and wss://) for addon and microcontroller scripts.
// Global table `websocket` in addon/mission VMs and microcontroller VMs. Polling API: a Lua function
// cannot be retained by a native mod, so scripts call websocket.poll() from onTick.
// No game address: only host->api and the Windows API.
#include "sw_mod_sdk.h"
#include "ws_registry.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#define WS_MOD_VERSION "0.3.0"

namespace {

const SwModHost* g_host = nullptr;
wsc::Registry* g_registry = nullptr; // heap: never destroyed inside DllMain
std::atomic<uint64_t> g_nextOwner{1};
std::atomic<int> g_addonVms{0};
bool g_hasGetGlobal = false;

const char kConfigKey[] = "websocket.cfg";
const char kOwnerField[] = "__owner";

void Log(const std::string& message) {
    if (g_host) g_host->log(g_host->modId, message.c_str());
}

const SwLuaApi* Lua() { return g_host->api->lua; }
const SwLuaExtrasApi* X() { return g_host->api->lua->extras; }

bool IsType(void* L, int index, SwLuaType type) {
    SwLuaType t = SW_LUA_TYPE_NONE;
    return X()->getType(g_host, L, index, &t) == SW_LUA_OK && t == type;
}

bool IsAbsent(void* L, int index) {
    if (index > Lua()->getTop(L)) return true;
    return IsType(L, index, SW_LUA_TYPE_NIL);
}

// Copies a Lua string argument byte for byte (binary and embedded NUL preserved).
bool ArgBytes(void* L, int index, std::string* out, size_t maxBytes) {
    if (!IsType(L, index, SW_LUA_TYPE_STRING)) return false;
    char probe[1];
    size_t length = 0;
    const SwLuaStatus s = X()->copyString(g_host, L, index, probe, sizeof(probe), &length);
    if ((s != SW_LUA_OK && s != SW_LUA_BUFFER_TOO_SMALL) || length > maxBytes) return false;
    out->assign(length + 1, '\0');
    if (X()->copyString(g_host, L, index, &(*out)[0], out->size(), &length) != SW_LUA_OK) return false;
    out->resize(length);
    return true;
}

// Positive integer id, or 0.
uint64_t ArgId(void* L, int index) {
    if (!IsType(L, index, SW_LUA_TYPE_NUMBER) && !IsType(L, index, SW_LUA_TYPE_INTEGER)) return 0;
    const double v = Lua()->argNumber(L, index, 0);
    if (!(v >= 1 && v <= 9007199254740992.0) || v != (double)(uint64_t)v) return 0;
    return (uint64_t)v;
}

bool PushString(void* L, const std::string& s) {
    return X()->pushString(g_host, L, s.data(), s.size()) == SW_LUA_OK;
}

// Lua integer (prints as "1", not "1.0").
bool PushInt(void* L, int64_t v) { return X()->pushInteger(g_host, L, v) == SW_LUA_OK; }

// Identifies the calling script VM without keeping its state: each VM has its own
// `websocket` table, which receives a numeric token on first use. 0 when unavailable.
uint64_t Owner(void* L) {
    const SwLuaExtrasApi* x = X();
    if (!g_hasGetGlobal || !Lua()->checkStack(L, 3)) return 0;
    const int top = Lua()->getTop(L);
    uint64_t owner = 0;
    if (x->getGlobal(g_host, L, "websocket") == SW_LUA_OK && IsType(L, -1, SW_LUA_TYPE_TABLE)) {
        if (x->getField(g_host, L, -1, kOwnerField) == SW_LUA_OK &&
            (IsType(L, -1, SW_LUA_TYPE_NUMBER) || IsType(L, -1, SW_LUA_TYPE_INTEGER))) {
            // argNumber only accepts positive indices (negative ones return the default).
            const double v = Lua()->argNumber(L, Lua()->getTop(L), 0);
            if (v >= 1) owner = (uint64_t)v;
        }
        x->pop(g_host, L, Lua()->getTop(L) - top - 1); // keep the table only
        if (!owner) {
            const uint64_t fresh = g_nextOwner++;
            if (x->pushInteger(g_host, L, (int64_t)fresh) == SW_LUA_OK &&
                x->setField(g_host, L, -2, kOwnerField) == SW_LUA_OK)
                owner = fresh;
        }
    }
    const int extra = Lua()->getTop(L) - top;
    if (extra > 0) x->pop(g_host, L, extra);
    return owner;
}

const char* StateName(wsc::State s) {
    switch (s) {
    case wsc::State::Connecting: return "connecting";
    case wsc::State::Open: return "open";
    case wsc::State::Closing: return "closing";
    default: return "closed";
    }
}

// ---- Lua functions --------------------------------------------------------------------

// websocket.connect(url [, options]) -> id | nil, error
int LuaConnect(void* L, void*) {
    std::string url, options, error;
    uint64_t id = 0;
    if (!ArgBytes(L, 1, &url, 4096)) error = "connect: url must be a string";
    else if (!IsAbsent(L, 2) && !ArgBytes(L, 2, &options, 16384)) error = "connect: options must be a string";
    else id = g_registry->Connect(Owner(L), url, options, &error);
    if (!Lua()->checkStack(L, 2)) return 0;
    if (id) return PushInt(L, (int64_t)id) ? 1 : 0;
    if (X()->pushNil(g_host, L) != SW_LUA_OK) return 0;
    return PushString(L, error) ? 2 : 1; // count what was pushed: the stack stays balanced
}

// websocket.send(id, data [, binary]) -> bool
int LuaSend(void* L, void*) {
    const uint64_t id = ArgId(L, 1);
    std::string data;
    const size_t max = g_registry->GetLimits().maxPendingSendBytes;
    bool ok = id && ArgBytes(L, 2, &data, max);
    if (ok) ok = g_registry->Send(Owner(L), id, data, Lua()->argBool(L, 3, false));
    if (!Lua()->checkStack(L, 1)) return 0;
    Lua()->pushBool(L, ok);
    return 1;
}

// Pushes one event as: id, type, data, extra.
int PushEvent(void* L, uint64_t id, const wsc::Event& e) {
    // On a failed push the values already pushed are counted, so the stack stays balanced.
    if (!Lua()->checkStack(L, 4) || !PushInt(L, (int64_t)id)) return 0;
    switch (e.type) {
    case wsc::EventType::Open:
        return PushString(L, "open") ? 2 : 1;
    case wsc::EventType::Message:
        if (!PushString(L, "message")) return 1;
        if (!PushString(L, e.data)) return 2;
        Lua()->pushBool(L, e.binary);
        return 4;
    case wsc::EventType::Close:
        if (!PushString(L, "close")) return 1;
        if (!PushString(L, e.data)) return 2;
        return PushInt(L, e.code) ? 4 : 3;
    default:
        if (!PushString(L, "error")) return 1;
        if (!PushString(L, e.data)) return 2;
        return PushInt(L, e.code) ? 4 : 3;
    }
}

// websocket.poll([id]) -> id, type, data, extra | nil
int LuaPoll(void* L, void*) {
    uint64_t id = 0;
    if (!IsAbsent(L, 1)) {
        id = ArgId(L, 1);
        if (!id) return 0;
    }
    uint64_t from = 0;
    wsc::Event e;
    if (!g_registry->Poll(Owner(L), id, &from, &e)) return 0;
    return PushEvent(L, from, e);
}

// websocket.receive(id) -> data, binary | nil
int LuaReceive(void* L, void*) {
    const uint64_t id = ArgId(L, 1);
    wsc::Event e;
    if (!id || !g_registry->PollMessage(Owner(L), id, &e)) return 0;
    if (!Lua()->checkStack(L, 2) || !PushString(L, e.data)) return 0;
    Lua()->pushBool(L, e.binary);
    return 2;
}

// websocket.state(id) -> "connecting" | "open" | "closing" | "closed"
int LuaState(void* L, void*) {
    const uint64_t id = ArgId(L, 1);
    wsc::State s = wsc::State::Closed;
    if (id) g_registry->GetState(Owner(L), id, &s);
    if (!Lua()->checkStack(L, 1)) return 0;
    return PushString(L, StateName(s)) ? 1 : 0;
}

// websocket.close(id [, code [, reason]]) -> bool
int LuaClose(void* L, void*) {
    const uint64_t id = ArgId(L, 1);
    int code = 1000;
    std::string reason;
    bool ok = id != 0;
    if (ok && !IsAbsent(L, 2)) {
        const double c = Lua()->argNumber(L, 2, -1);
        ok = c >= 0 && c <= 65535 && c == (double)(int)c;
        code = (int)c;
    }
    if (ok && !IsAbsent(L, 3)) ok = ArgBytes(L, 3, &reason, 123);
    if (ok) ok = g_registry->Close(Owner(L), id, code, reason);
    if (!Lua()->checkStack(L, 1)) return 0;
    Lua()->pushBool(L, ok);
    return 1;
}

// websocket.pending(id) -> number of queued events
int LuaPending(void* L, void*) {
    const uint64_t id = ArgId(L, 1);
    const size_t n = id ? g_registry->Pending(Owner(L), id) : 0;
    if (!Lua()->checkStack(L, 1)) return 0;
    return PushInt(L, (int64_t)n) ? 1 : 0;
}

// websocket.version() -> string
int LuaVersion(void* L, void*) {
    if (!Lua()->checkStack(L, 1)) return 0;
    return PushString(L, WS_MOD_VERSION) ? 1 : 0;
}

const SwLuaFunction kFunctions[] = {
    {"connect", &LuaConnect, nullptr}, {"send", &LuaSend, nullptr},       {"poll", &LuaPoll, nullptr},
    {"receive", &LuaReceive, nullptr}, {"state", &LuaState, nullptr},     {"close", &LuaClose, nullptr},
    {"pending", &LuaPending, nullptr}, {"version", &LuaVersion, nullptr},
};

// ---- Lifecycle --------------------------------------------------------------------------

void LoadConfig(const SwModHost* host) {
    const SwStorageApi* storage = host->api->storage;
    wsc::Limits limits;
    if (!storage || storage->structSize < offsetof(SwStorageApi, writeConfig) + sizeof(storage->writeConfig) ||
        !(host->api->runtime->capabilities() & SW_CAP_MOD_STORAGE)) {
        Log("storage unavailable: default configuration");
        g_registry->Configure(limits);
        return;
    }
    std::vector<uint8_t> buffer(64 * 1024);
    size_t size = 0;
    const SwStorageStatus s = storage->readConfig(host, kConfigKey, buffer.data(), buffer.size(), &size);
    if (s == SW_STORAGE_NOT_FOUND) {
        const std::string text = wsc::DefaultConfigText();
        storage->writeConfig(host, kConfigKey, (const uint8_t*)text.data(), text.size());
        Log("wrote default websocket.cfg");
    } else if (s == SW_STORAGE_OK) {
        std::string error;
        if (!wsc::ParseConfig(std::string((const char*)buffer.data(), size), &limits, &error)) {
            Log("websocket.cfg ignored: " + error);
            limits = wsc::Limits();
        }
    } else {
        Log("websocket.cfg unreadable (status " + std::to_string((int)s) + "): defaults");
    }
    g_registry->Configure(limits);
    std::string hosts;
    for (const std::string& h : limits.allowHosts) hosts += (hosts.empty() ? "" : ",") + h;
    Log("config: allow_hosts=" + hosts + " max_connections=" + std::to_string(limits.maxConnections) +
        " idle_timeout=" + std::to_string((int)limits.idleSeconds) +
        " microcontrollers=" + (limits.microcontrollers ? "1" : "0"));
}

void OnSweep(void*) {
    if (g_registry) g_registry->Sweep();
}

// No addon VM left = the world was closed: drop every connection.
void OnAddonVm(const SwEvent* event, void*) {
    if (!event || !g_registry) return;
    if (event->type == SW_EVENT_ADDON_LUA_CREATED) {
        ++g_addonVms;
    } else if (event->type == SW_EVENT_ADDON_LUA_DESTROYED) {
        int n = g_addonVms.load();
        while (n > 0 && !g_addonVms.compare_exchange_weak(n, n - 1)) {}
        if (n == 1 && g_registry->Count()) {
            Log("last addon VM destroyed: closing connections");
            g_registry->CloseAll();
        }
    }
}

// Windows binds a DLL request by base name to an already loaded module. A mod DLL named
// like a System32 DLL (websocket.dll, winhttp.dll...) therefore replaces it for the whole
// process: WinHTTP then fails with ERROR_NOT_ENOUGH_MEMORY. Returns the clashing name.
std::string ShadowedSystemDll() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ShadowedSystemDll), &self))
        return std::string();
    wchar_t path[MAX_PATH], system[MAX_PATH];
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    const UINT s = GetSystemDirectoryW(system, MAX_PATH);
    if (!n || n >= MAX_PATH || !s || s >= MAX_PATH) return std::string();
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    const std::wstring candidate = std::wstring(system) + L"\\" + name;
    if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) return std::string();
    char narrow[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow), nullptr, nullptr);
    return narrow;
}

} // namespace

SW_MOD_EXPORT bool SwModInit(const SwModHost* host) {
    if (!host || host->sdkVersion < 2 || !host->api || host->api->abiVersion != SW_ENGINE_API_VERSION ||
        !host->api->lua || !host->api->runtime) return false;
    const SwLuaApi* lua = host->api->lua;
    const SwRuntimeApi* runtime = host->api->runtime;
    // Strings need extras->pushString/copyString (byte exact); getGlobal is optional (VM owner).
    if (lua->structSize < offsetof(SwLuaApi, extras) + sizeof(lua->extras) || !lua->extras ||
        lua->extras->structSize < offsetof(SwLuaExtrasApi, copyString) + sizeof(lua->extras->copyString)) {
        host->log(host->modId, "Lua extras API missing: disabled");
        return false;
    }
    const std::string shadowed = ShadowedSystemDll();
    if (!shadowed.empty()) {
        host->log(host->modId, (shadowed + " has the name of a Windows DLL and would replace it in the game "
                                "(WinHTTP needs the real one): rename the mod DLL. Disabled.").c_str());
        return false;
    }
    const uint64_t caps = runtime->capabilities();
    g_host = host;
    g_hasGetGlobal = lua->extras->structSize >= offsetof(SwLuaExtrasApi, getGlobal) + sizeof(lua->extras->getGlobal);
    if (!g_hasGetGlobal) Log("extras->getGlobal unavailable: scripts share one connection owner");
    g_registry = new wsc::Registry(nullptr, &Log);
    LoadConfig(host);
    const size_t count = sizeof(kFunctions) / sizeof(kFunctions[0]);

    // Addon/mission scripts (script.lua): newer reviewed build only.
    bool addon = false;
    if (lua->structSize >= offsetof(SwLuaApi, registerAddonLibrary) + sizeof(lua->registerAddonLibrary) &&
        (caps & SW_CAP_LUA_ADDON)) {
        addon = lua->registerAddonLibrary(host, "websocket", kFunctions, count);
        if (!addon) Log("registerAddonLibrary(websocket) failed");
    } else {
        Log("addon Lua VMs unavailable on this game build: no addon library");
    }
    // Microcontroller Lua: both reviewed builds; can be turned off in websocket.cfg.
    bool mc = false;
    if (g_registry->GetLimits().microcontrollers) {
        mc = lua->registerLibrary(host, "websocket", kFunctions, count);
        if (!mc) Log("registerLibrary(websocket) failed: no microcontroller library");
    } else {
        Log("microcontroller library disabled (microcontrollers=0)");
    }
    if (!addon && !mc) {
        Log("no Lua library registered: disabled");
        delete g_registry;
        g_registry = nullptr;
        g_host = nullptr;
        return false;
    }
    Log(std::string("libraries: addon=") + (addon ? "yes" : "no") + " microcontroller=" + (mc ? "yes" : "no"));
    SwHandle handle = 0;
    if (runtime->subscribe(host, SW_EVENT_MASK(SW_EVENT_ADDON_LUA_CREATED) | SW_EVENT_MASK(SW_EVENT_ADDON_LUA_DESTROYED),
                           &OnAddonVm, nullptr, &handle) != SW_OK)
        Log("addon VM events unavailable: relying on idle_timeout");
    if (!(caps & SW_CAP_MAIN_THREAD_TASKS) || runtime->schedule(host, 1.0, 1.0, &OnSweep, nullptr, &handle) != SW_OK)
        Log("scheduled sweep unavailable: idle connections are released on connect");
    Log("websocket " WS_MOD_VERSION " ready");
    return true;
}

SW_MOD_EXPORT void SwModUnload(void) {
    // Callbacks are already revoked: no Lua call or task can reach the registry now.
    if (g_registry) {
        g_registry->CloseAll();
        delete g_registry;
        g_registry = nullptr;
    }
    g_host = nullptr;
}
