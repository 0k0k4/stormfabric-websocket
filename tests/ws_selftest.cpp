// Offline tests for ws_client against tests/echo_server.py.
//   ws_selftest.exe [--port 8765] [--tls-port 8766] [--online]
#include "../src/ws_client.h"

#include <windows.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

using namespace wsc;
using Clock = std::chrono::steady_clock;

static int g_failed = 0, g_passed = 0;
static int g_port = 8765, g_tlsPort = 8766;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("    FAIL line %d: %s -- ", __LINE__, #cond);          \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
            ok = false;                                                    \
        }                                                                  \
    } while (0)

static std::string Url(const char* path = "/") { return "ws://127.0.0.1:" + std::to_string(g_port) + path; }
static std::string TlsUrl() { return "wss://localhost:" + std::to_string(g_tlsPort) + "/"; }

static const char* TypeName(EventType t) {
    switch (t) {
    case EventType::Open: return "open";
    case EventType::Message: return "message";
    case EventType::Close: return "close";
    default: return "error";
    }
}

// Waits for the next event (up to `seconds`).
static bool Next(Connection& c, Event* e, double seconds = 10) {
    const auto end = Clock::now() + std::chrono::milliseconds((int)(seconds * 1000));
    while (Clock::now() < end) {
        if (c.Poll(e)) return true;
        Sleep(2);
    }
    return false;
}

// Waits for an event of the given type, skipping others. Returns false on timeout.
static bool WaitFor(Connection& c, EventType type, Event* e, double seconds = 10) {
    const auto end = Clock::now() + std::chrono::milliseconds((int)(seconds * 1000));
    while (Clock::now() < end) {
        if (c.Poll(e)) {
            if (e->type == type) return true;
            if (e->type != EventType::Message)
                printf("    (skipped %s %d %.100s)\n", TypeName(e->type), e->code, e->data.c_str());
            continue;
        }
        Sleep(2);
    }
    return false;
}

static std::unique_ptr<Connection> OpenOk(const std::string& url, const Options& o, bool& ok) {
    std::string err;
    auto c = Connection::Create(url, o, &err);
    CHECK(c != nullptr, "create failed: %s", err.c_str());
    if (!c) return nullptr;
    Event e;
    const bool got = Next(*c, &e);
    CHECK(got && e.type == EventType::Open, "expected open, got %s %d %s", got ? TypeName(e.type) : "timeout",
          e.code, e.data.c_str());
    return (got && e.type == EventType::Open) ? std::move(c) : nullptr;
}

static void Run(const char* name, bool (*fn)()) {
    printf("[ %s ]\n", name);
    const auto start = Clock::now();
    const bool ok = fn();
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    printf("    %s (%.0f ms)\n", ok ? "ok" : "FAILED", ms);
    (ok ? g_passed : g_failed)++;
}

// ---------------------------------------------------------------------------------------

static bool TestOptions() {
    bool ok = true;
    Options o;
    std::string err;
    CHECK(ParseOptions("protocol=chat; header=X-Test: a;b=c ;insecure=1;timeout=2.5", &o, &err) == false,
          "header value with ';' splits into an unknown option");
    o = Options();
    CHECK(ParseOptions("protocol=chat;header=X-Test: abc;header=X-Other: 1;insecure=1;timeout=2.5", &o, &err),
          "%s", err.c_str());
    CHECK(o.protocol == "chat" && o.headers.size() == 2 && o.headers[0] == "X-Test: abc" && o.insecure &&
              o.timeoutMs == 2500, "parsed values");
    Options p;
    CHECK(ParseOptions("", &p, &err) && ParseOptions(";;", &p, &err), "empty strings");
    CHECK(!ParseOptions("bogus=1", &p, &err), "unknown key");
    CHECK(!ParseOptions("timeout=0", &p, &err) && !ParseOptions("timeout=x", &p, &err), "bad timeout");
    CHECK(!ParseOptions("header=NoColon", &p, &err), "header without colon");
    CHECK(!ParseOptions("header=Bad Name: x", &p, &err), "header name with space");
    CHECK(!ParseOptions("insecure=yes", &p, &err), "bad bool");
    Options q;
    CHECK(q.proxy == Proxy::Auto, "default proxy auto");
    CHECK(ParseOptions("proxy=direct", &q, &err) && q.proxy == Proxy::Direct, "proxy=direct");
    CHECK(ParseOptions("proxy=system", &q, &err) && q.proxy == Proxy::System, "proxy=system");
    CHECK(!ParseOptions("proxy=socks", &q, &err), "bad proxy");
    return ok;
}

static bool TestProxyModes() {
    bool ok = true;
    const char* modes[] = {"proxy=auto", "proxy=direct", "proxy=system"};
    for (const char* m : modes) {
        Options o;
        std::string err;
        std::vector<std::string> logs;
        o.log = [&logs](const std::string& s) { logs.push_back(s); };
        CHECK(ParseOptions(m, &o, &err), "%s", err.c_str());
        auto c = OpenOk(Url(), o, ok);
        if (!c) continue;
        CHECK(c->Send(m, false), "send");
        Event e;
        CHECK(WaitFor(*c, EventType::Message, &e) && e.data == m, "%s echo", m);
        CHECK(logs.empty(), "%s: no failed attempt logged (%zu)", m, logs.size());
    }
    // A definitive network error is not retried with another proxy mode.
    Options o;
    o.timeoutMs = 3000;
    std::vector<std::string> logs;
    o.log = [&logs](const std::string& s) { logs.push_back(s); };
    std::string err;
    auto c = Connection::Create("ws://127.0.0.1:1/", o, &err);
    Event e;
    CHECK(c && WaitFor(*c, EventType::Close, &e), "refused closes");
    CHECK(logs.size() == 1, "one attempt for a refused connection (%zu)", logs.size());
    return ok;
}

static bool TestBadUrls() {
    bool ok = true;
    std::string err;
    Options o;
    CHECK(!Connection::Create("http://127.0.0.1/", o, &err), "http refused");
    CHECK(!Connection::Create("ftp://x", o, &err), "ftp refused");
    CHECK(!Connection::Create("ws://", o, &err), "empty host refused");
    CHECK(!Connection::Create("ws://a\r\nb/", o, &err), "control chars refused");
    o.headers.push_back("X: a\r\nInjected: 1");
    CHECK(!Connection::Create(Url(), o, &err), "header injection refused");
    CHECK(Connection::LiveThreads() == 0, "no thread started (%d)", Connection::LiveThreads());
    return ok;
}

static bool TestEchoText() {
    bool ok = true;
    auto c = OpenOk(Url(), Options(), ok);
    if (!c) return false;
    CHECK(c->GetState() == State::Open, "state open");
    const std::string utf8 = "h\xC3\xA9llo \xE2\x9C\x93";
    CHECK(c->Send(utf8, false), "send");
    Event e;
    CHECK(WaitFor(*c, EventType::Message, &e) && e.data == utf8 && !e.binary, "echo text: %s", e.data.c_str());
    CHECK(c->Send("", false), "send empty");
    CHECK(WaitFor(*c, EventType::Message, &e) && e.data.empty(), "echo empty");
    return ok;
}

static bool TestEchoBinary() {
    bool ok = true;
    auto c = OpenOk(Url(), Options(), ok);
    if (!c) return false;
    std::string bin;
    for (int i = 0; i < 300; ++i) bin.push_back((char)(i & 0xff)); // includes NUL bytes
    CHECK(c->Send(bin, true), "send");
    Event e;
    CHECK(WaitFor(*c, EventType::Message, &e) && e.data == bin && e.binary, "echo binary (%zu bytes)", e.data.size());
    return ok;
}

static bool TestBigMessages() {
    bool ok = true;
    auto c = OpenOk(Url(), Options(), ok);
    if (!c) return false;
    std::string big(1048576, 'x');
    big[0] = 'B';
    big[big.size() - 1] = 'E';
    CHECK(c->Send(big, false), "send 1 MiB");
    Event e;
    CHECK(WaitFor(*c, EventType::Message, &e, 20) && e.data == big, "echo 1 MiB (%zu)", e.data.size());
    CHECK(c->Send("big:1000000", false), "request fragmented 1 MB");
    CHECK(WaitFor(*c, EventType::Message, &e, 20) && e.data.size() == 1000000, "fragmented (%zu)", e.data.size());
    for (int i = 0; i < 100; ++i) CHECK(c->Send("m" + std::to_string(i), false), "burst send %d", i);
    for (int i = 0; i < 100; ++i) {
        const bool got = WaitFor(*c, EventType::Message, &e);
        CHECK(got && e.data == "m" + std::to_string(i), "order %d: %s", i, e.data.c_str());
        if (!got) break;
    }
    return ok;
}

static bool TestTooBig() {
    bool ok = true;
    auto c = OpenOk(Url(), Options(), ok);
    if (!c) return false;
    CHECK(c->Send("big:2000000", false), "request 2 MB");
    Event e;
    CHECK(Next(*c, &e, 20) && e.type == EventType::Error && e.code == 1009, "error 1009: %s %d", TypeName(e.type), e.code);
    CHECK(WaitFor(*c, EventType::Close, &e, 20), "close event");
    CHECK(c->GetState() == State::Closed, "closed");
    return ok;
}

static bool TestServerClose() {
    bool ok = true;
    auto c = OpenOk(Url(), Options(), ok);
    if (!c) return false;
    CHECK(c->Send("close-me", false), "send");
    Event e;
    CHECK(Next(*c, &e) && e.type == EventType::Close && e.code == 4001 && e.data == "bye", "close 4001 bye: %s %d %s",
          TypeName(e.type), e.code, e.data.c_str());
    CHECK(c->GetState() == State::Closed, "closed");
    CHECK(!c->Send("late", false), "send after close refused");
    CHECK(!c->Close(1000, ""), "close after close refused");
    return ok;
}

static bool TestClientClose() {
    bool ok = true;
    auto c = OpenOk(Url(), Options(), ok);
    if (!c) return false;
    CHECK(!c->Close(1005, ""), "reserved code refused");
    CHECK(!c->Close(1000, std::string(124, 'r')), "long reason refused");
    CHECK(c->Close(1000, "done"), "close");
    CHECK(c->GetState() == State::Closing || c->GetState() == State::Closed, "closing");
    CHECK(!c->Send("x", false), "send while closing refused");
    Event e;
    CHECK(Next(*c, &e) && e.type == EventType::Close && e.code == 1000, "close 1000: %s %d %s", TypeName(e.type),
          e.code, e.data.c_str());
    CHECK(c->GetState() == State::Closed, "closed");
    return ok;
}

static bool ExpectError(const std::string& url, const Options& o, const char* what, int* codeOut = nullptr) {
    bool ok = true;
    std::string err;
    auto c = Connection::Create(url, o, &err);
    CHECK(c != nullptr, "create: %s", err.c_str());
    if (!c) return false;
    Event e;
    const bool got = Next(*c, &e, 30);
    CHECK(got && e.type == EventType::Error, "%s: expected error, got %s", what, got ? TypeName(e.type) : "timeout");
    if (got) printf("    %s -> %d %s\n", what, e.code, e.data.c_str());
    if (codeOut) *codeOut = e.code;
    CHECK(Next(*c, &e) && e.type == EventType::Close && e.code == 1006, "%s: close 1006", what);
    return ok;
}

static bool TestConnectFailures() {
    bool ok = true;
    Options fast;
    fast.timeoutMs = 3000;
    ok &= ExpectError("ws://127.0.0.1:1/", fast, "refused");
    ok &= ExpectError("ws://does-not-exist.invalid/", fast, "dns");
    int code = 0;
    ok &= ExpectError(Url("/reject"), fast, "http 403", &code);
    CHECK(code == 403, "status 403 (%d)", code);
    return ok;
}

static bool TestProtocolAndHeaders() {
    bool ok = true;
    Options o;
    std::string err;
    CHECK(ParseOptions("protocol=chat;header=X-Test: abc", &o, &err), "%s", err.c_str());
    auto c = OpenOk(Url(), o, ok);
    if (!c) return false;
    CHECK(c->Send("headers?", false), "send");
    Event e;
    CHECK(WaitFor(*c, EventType::Message, &e) && e.data == "abc|chat", "got %s", e.data.c_str());
    return ok;
}

static bool TestTls() {
    bool ok = true;
    Options strict;
    strict.timeoutMs = 5000;
    ok &= ExpectError(TlsUrl(), strict, "self-signed refused");
    Options insecure;
    insecure.insecure = true;
    auto c = OpenOk(TlsUrl(), insecure, ok);
    if (!c) return false;
    CHECK(c->Send("over tls", false), "send");
    Event e;
    CHECK(WaitFor(*c, EventType::Message, &e) && e.data == "over tls", "tls echo");
    std::string big(300000, 'z');
    CHECK(c->Send(big, true), "send big over tls");
    CHECK(WaitFor(*c, EventType::Message, &e, 20) && e.data == big, "tls big echo");
    return ok;
}

static bool TestIdleBeyondTimeout() {
    bool ok = true;
    Options o;
    o.timeoutMs = 1000;
    auto c = OpenOk(Url(), o, ok);
    if (!c) return false;
    Sleep(2500);
    Event e;
    CHECK(!c->Poll(&e), "no event while idle (%s %d %s)", TypeName(e.type), e.code, e.data.c_str());
    CHECK(c->GetState() == State::Open, "still open");
    CHECK(c->Send("after idle", false), "send");
    CHECK(WaitFor(*c, EventType::Message, &e) && e.data == "after idle", "echo after idle");
    return ok;
}

static bool TestQueueOverflow() {
    bool ok = true;
    Options o;
    o.maxQueuedEvents = 16;
    auto c = OpenOk(Url(), o, ok);
    if (!c) return false;
    CHECK(c->Send("flood:100", false), "send");
    Sleep(500);
    CHECK(c->Pending() <= 17, "bounded queue (%zu)", c->Pending());
    int messages = 0, errors = 0;
    Event e;
    while (c->Poll(&e)) {
        if (e.type == EventType::Message) ++messages;
        if (e.type == EventType::Error) ++errors;
    }
    CHECK(messages == 16 && errors == 1, "16 messages + 1 overflow error (%d, %d)", messages, errors);
    CHECK(c->Send("still alive", false), "send after overflow");
    CHECK(WaitFor(*c, EventType::Message, &e) && e.data == "still alive", "echo after overflow");
    return ok;
}

static bool TestSendLimit() {
    bool ok = true;
    Options o;
    o.maxPendingSendBytes = 1000;
    auto c = OpenOk(Url(), o, ok);
    if (!c) return false;
    CHECK(!c->Send(std::string(1001, 'a'), false), "over limit refused");
    CHECK(c->Send(std::string(1000, 'a'), false), "at limit accepted");
    Event e;
    CHECK(WaitFor(*c, EventType::Message, &e) && e.data.size() == 1000, "echo");
    return ok;
}

static bool TestCancel() {
    bool ok = true;
    // Non-routable address: the TCP connect hangs until cancelled.
    Options o;
    o.timeoutMs = 60000;
    std::string err;
    {
        auto c = Connection::Create("ws://10.255.255.1/", o, &err);
        CHECK(c != nullptr, "create");
        Sleep(300);
        const auto start = Clock::now();
        c.reset();
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        CHECK(ms < 3000, "destructor while connecting took %.0f ms", ms);
    }
    {
        auto c = Connection::Create("ws://10.255.255.1/", o, &err);
        Sleep(300);
        const auto start = Clock::now();
        CHECK(c->Close(4000, "abort"), "close while connecting");
        Event e;
        CHECK(Next(*c, &e, 5) && e.type == EventType::Close && e.code == 4000 && e.data == "abort",
              "close event: %s %d %s", TypeName(e.type), e.code, e.data.c_str());
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        CHECK(ms < 3000, "cancel took %.0f ms", ms);
    }
    {
        // Destroying open connections with pending traffic.
        Options fast;
        std::unique_ptr<Connection> many[32];
        for (auto& m : many) m = Connection::Create(Url(), fast, &err);
        Sleep(500);
        for (auto& m : many) if (m) m->Send(std::string(100000, 'q'), true);
        const auto start = Clock::now();
        for (auto& m : many) m.reset();
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        CHECK(ms < 3000, "destroying 32 connections took %.0f ms", ms);
    }
    return ok;
}

static bool TestOnline() {
    bool ok = true;
    auto c = OpenOk("wss://echo.websocket.org/", Options(), ok);
    if (!c) return false;
    CHECK(c->Send("stormfabric-online-test", false), "send");
    Event e;
    bool found = false;
    const auto end = Clock::now() + std::chrono::seconds(10);
    while (!found && Clock::now() < end && WaitFor(*c, EventType::Message, &e, 10))
        found = e.data == "stormfabric-online-test"; // the server sends a greeting first
    CHECK(found, "public echo");
    CHECK(c->Close(1000, ""), "close");
    CHECK(WaitFor(*c, EventType::Close, &e), "close event");
    return ok;
}

int main(int argc, char** argv) {
    bool online = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) g_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tls-port") && i + 1 < argc) g_tlsPort = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--online")) online = true;
    }
    Run("options parsing", TestOptions);
    Run("invalid urls", TestBadUrls);
    Run("text echo", TestEchoText);
    Run("binary echo", TestEchoBinary);
    Run("big and fragmented messages", TestBigMessages);
    Run("message over limit", TestTooBig);
    Run("server close", TestServerClose);
    Run("client close", TestClientClose);
    Run("connection failures", TestConnectFailures);
    Run("subprotocol and headers", TestProtocolAndHeaders);
    Run("proxy modes", TestProxyModes);
    Run("wss", TestTls);
    Run("idle beyond timeout", TestIdleBeyondTimeout);
    Run("event queue overflow", TestQueueOverflow);
    Run("send limit", TestSendLimit);
    Run("cancel and destroy", TestCancel);
    if (online) Run("public wss echo", TestOnline);

    const auto end = Clock::now() + std::chrono::seconds(2);
    while (Connection::LiveThreads() && Clock::now() < end) Sleep(10);
    printf("\nlive threads at exit: %d\n", Connection::LiveThreads());
    if (Connection::LiveThreads()) ++g_failed;
    printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
