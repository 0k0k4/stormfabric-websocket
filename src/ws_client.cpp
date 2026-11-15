// WebSocket client core on top of WinHTTP (Windows 8+). See ws_client.h.
#include "ws_client.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <cstring>

#pragma comment(lib, "winhttp.lib")

namespace wsc {
namespace {

std::atomic<int> g_liveThreads{0};

struct ThreadCounter {
    ThreadCounter() { ++g_liveThreads; }
    ~ThreadCounter() { --g_liveThreads; }
};

constexpr size_t kReceiveChunk = 64 * 1024;
constexpr size_t kMaxCloseReason = 123;
constexpr auto kCloseTimeout = std::chrono::seconds(5);

std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string Narrow(const wchar_t* w, size_t len) {
    if (!w || !len) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, (int)len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, (int)len, &s[0], n, nullptr, nullptr);
    return s;
}

std::string ErrorText(DWORD code) {
    wchar_t* buffer = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS;
    HMODULE module = nullptr;
    if (code >= WINHTTP_ERROR_BASE && code <= WINHTTP_ERROR_LAST) {
        module = GetModuleHandleW(L"winhttp.dll");
        flags |= FORMAT_MESSAGE_FROM_HMODULE;
    } else {
        flags |= FORMAT_MESSAGE_FROM_SYSTEM;
    }
    const DWORD len = FormatMessageW(flags, module, code, 0, (LPWSTR)&buffer, 0, nullptr);
    std::string text = len ? Narrow(buffer, len) : std::string();
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '.'))
        text.pop_back();
    if (text.empty()) text = "error";
    return text + " (" + std::to_string(code) + ")";
}

bool StartsWithNoCase(const std::string& s, const char* prefix) {
    const size_t n = strlen(prefix);
    return s.size() >= n && _strnicmp(s.c_str(), prefix, n) == 0;
}

bool HasControlChars(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7f) return true;
    return false;
}

bool ValidHeader(const std::string& h) {
    const size_t colon = h.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    for (size_t i = 0; i < colon; ++i) {
        const unsigned char c = (unsigned char)h[i];
        if (c <= 0x20 || c >= 0x7f) return false; // token characters only
    }
    for (size_t i = colon + 1; i < h.size(); ++i) {
        const unsigned char c = (unsigned char)h[i];
        if ((c < 0x20 && c != '\t') || c == 0x7f) return false; // no CR/LF injection
    }
    return true;
}

bool ValidSendCloseCode(int code) { return code == 1000 || (code >= 3000 && code <= 4999); }

bool ValidEchoCloseCode(int code) {
    return (code >= 1000 && code <= 1003) || (code >= 1007 && code <= 1014) || (code >= 3000 && code <= 4999);
}

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

} // namespace

bool ParseOptions(const std::string& text, Options* out, std::string* error) {
    Options result = *out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find(';', pos);
        if (end == std::string::npos) end = text.size();
        const std::string item = text.substr(pos, end - pos);
        pos = end + 1;
        if (Trim(item).empty()) {
            if (end == text.size()) break;
            continue;
        }
        const size_t eq = item.find('=');
        if (eq == std::string::npos) {
            if (error) *error = "option without '=': " + Trim(item);
            return false;
        }
        const std::string key = Trim(item.substr(0, eq));
        const std::string value = item.substr(eq + 1);
        if (key == "protocol") {
            const std::string v = Trim(value);
            if (v.empty() || HasControlChars(v)) {
                if (error) *error = "invalid protocol";
                return false;
            }
            result.protocol = v;
        } else if (key == "header") {
            const std::string v = Trim(value);
            if (!ValidHeader(v)) {
                if (error) *error = "invalid header: " + v;
                return false;
            }
            result.headers.push_back(v);
        } else if (key == "insecure") {
            const std::string v = Trim(value);
            if (v == "1" || v == "true") result.insecure = true;
            else if (v == "0" || v == "false") result.insecure = false;
            else {
                if (error) *error = "insecure must be 0 or 1";
                return false;
            }
        } else if (key == "proxy") {
            const std::string v = Trim(value);
            if (v == "auto") result.proxy = Proxy::Auto;
            else if (v == "direct") result.proxy = Proxy::Direct;
            else if (v == "system") result.proxy = Proxy::System;
            else {
                if (error) *error = "proxy must be auto, direct or system";
                return false;
            }
        } else if (key == "timeout") {
            char* endp = nullptr;
            const std::string v = Trim(value);
            const double seconds = strtod(v.c_str(), &endp);
            if (v.empty() || *endp || !(seconds >= 1 && seconds <= 120)) {
                if (error) *error = "timeout must be 1..120 seconds";
                return false;
            }
            result.timeoutMs = (uint32_t)(seconds * 1000);
        } else {
            if (error) *error = "unknown option: " + key;
            return false;
        }
        if (end == text.size()) break;
    }
    *out = result;
    return true;
}

int Connection::LiveThreads() { return g_liveThreads.load(); }

std::unique_ptr<Connection> Connection::Create(const std::string& url, const Options& options,
                                               std::string* error) {
    auto fail = [&](const char* text) -> std::unique_ptr<Connection> {
        if (error) *error = text;
        return nullptr;
    };
    if (url.size() > 4096 || HasControlChars(url)) return fail("invalid url");
    bool secure;
    std::string httpUrl;
    if (StartsWithNoCase(url, "ws://")) {
        secure = false;
        httpUrl = "http://" + url.substr(5);
    } else if (StartsWithNoCase(url, "wss://")) {
        secure = true;
        httpUrl = "https://" + url.substr(6);
    } else {
        return fail("url must start with ws:// or wss://");
    }
    for (const std::string& h : options.headers)
        if (!ValidHeader(h)) return fail("invalid header");
    if (HasControlChars(options.protocol)) return fail("invalid protocol");

    const std::wstring wide = Widen(httpUrl);
    if (wide.empty()) return fail("url is not valid UTF-8");
    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = (DWORD)-1;
    parts.dwUrlPathLength = (DWORD)-1;
    parts.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wide.c_str(), (DWORD)wide.size(), 0, &parts) || !parts.dwHostNameLength)
        return fail("invalid url");

    std::unique_ptr<Connection> c(new Connection());
    c->url_ = url;
    c->options_ = options;
    c->secure_ = secure;
    c->host_.assign(parts.lpszHostName, parts.dwHostNameLength);
    c->port_ = parts.nPort;
    c->path_.assign(parts.lpszUrlPath ? parts.lpszUrlPath : L"", parts.dwUrlPathLength);
    if (parts.lpszExtraInfo && parts.dwExtraInfoLength) {
        std::wstring extra(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        const size_t hash = extra.find(L'#'); // fragments are never sent
        if (hash != std::wstring::npos) extra.resize(hash);
        c->path_ += extra;
    }
    if (c->path_.empty()) c->path_ = L"/";
    c->receiveThread_ = std::thread(&Connection::ReceiveThread, c.get());
    c->sendThread_ = std::thread(&Connection::SendThread, c.get());
    return c;
}

Connection::~Connection() {
    stop_ = true;
    {
        std::lock_guard<std::mutex> lock(sendQueueMutex_);
        sendCv_.notify_all();
    }
    {
        // Closing a handle cancels the blocking WinHTTP call that uses it.
        std::lock_guard<std::mutex> lock(handleMutex_);
        if (request_) { WinHttpCloseHandle(request_); request_ = nullptr; }
        if (socket_) { WinHttpCloseHandle(socket_); socket_ = nullptr; }
    }
    if (receiveThread_.joinable()) receiveThread_.join();
    if (sendThread_.joinable()) sendThread_.join();
    CloseHandle(&connect_);
    CloseHandle(&session_);
}

void Connection::CloseHandle(void** handle) {
    std::lock_guard<std::mutex> lock(handleMutex_);
    if (*handle) {
        WinHttpCloseHandle(*handle);
        *handle = nullptr;
    }
}

void* Connection::WebSocketHandle() {
    std::lock_guard<std::mutex> lock(handleMutex_);
    return socket_;
}

bool Connection::Send(const std::string& data, bool binary) {
    if (state_.load() != State::Open || stop_) return false;
    std::lock_guard<std::mutex> lock(sendQueueMutex_);
    if (data.size() > options_.maxPendingSendBytes - pendingSendBytes_ ||
        pendingSendBytes_ > options_.maxPendingSendBytes) return false;
    Outgoing item;
    item.data = data;
    item.binary = binary;
    pendingSendBytes_ += data.size();
    sendQueue_.push_back(std::move(item));
    sendCv_.notify_all();
    return true;
}

bool Connection::Close(int code, const std::string& reason) {
    if (!ValidSendCloseCode(code) || reason.size() > kMaxCloseReason) return false;
    {
        std::lock_guard<std::mutex> lock(sendQueueMutex_);
        if (closeRequested_) return false;
        closeRequested_ = true;
        closeCode_ = code;
        closeReason_ = reason;
    }
    State expected = State::Connecting;
    if (state_.compare_exchange_strong(expected, State::Closing)) {
        // Cancel the handshake; the receive thread reports the requested Close event.
        std::lock_guard<std::mutex> lock(handleMutex_);
        if (request_) { WinHttpCloseHandle(request_); request_ = nullptr; }
        return true;
    }
    expected = State::Open;
    if (!state_.compare_exchange_strong(expected, State::Closing)) return false;
    std::lock_guard<std::mutex> lock(sendQueueMutex_);
    Outgoing item;
    item.isClose = true;
    item.code = code;
    item.data = reason;
    sendQueue_.push_back(std::move(item));
    sendCv_.notify_all();
    return true;
}

bool Connection::Poll(Event* out) {
    std::lock_guard<std::mutex> lock(eventsMutex_);
    if (events_.empty()) return false;
    *out = std::move(events_.front());
    events_.pop_front();
    if (events_.size() < options_.maxQueuedEvents / 2) overflowReported_ = false;
    return true;
}

bool Connection::PollMessage(Event* out) {
    std::lock_guard<std::mutex> lock(eventsMutex_);
    for (auto it = events_.begin(); it != events_.end(); ++it) {
        if (it->type != EventType::Message) continue;
        *out = std::move(*it);
        events_.erase(it);
        if (events_.size() < options_.maxQueuedEvents / 2) overflowReported_ = false;
        return true;
    }
    return false;
}

size_t Connection::Pending() const {
    std::lock_guard<std::mutex> lock(eventsMutex_);
    return events_.size();
}

void Connection::PushEvent(Event event, bool always) {
    std::lock_guard<std::mutex> lock(eventsMutex_);
    if (!always && events_.size() >= options_.maxQueuedEvents) {
        if (!overflowReported_) {
            overflowReported_ = true;
            Event e;
            e.type = EventType::Error;
            e.code = 0;
            e.data = "event queue full: incoming messages dropped";
            events_.push_back(std::move(e));
        }
        return;
    }
    events_.push_back(std::move(event));
}

void Connection::Fail(int code, const std::string& text) {
    Event e;
    e.type = EventType::Error;
    e.code = code;
    e.data = text;
    PushEvent(std::move(e), true);
}

void Connection::FinishClosed(int code, const std::string& reason) {
    state_ = State::Closed;
    {
        std::lock_guard<std::mutex> lock(eventsMutex_);
        if (closedReported_) return;
        closedReported_ = true;
    }
    Event e;
    e.type = EventType::Close;
    e.code = code;
    e.data = reason;
    PushEvent(std::move(e), true);
}

void Connection::ReceiveThread() {
    ThreadCounter counter;
    if (Handshake()) ReceiveLoop();
    CloseHandle(&request_);
    CloseHandle(&socket_);
    if (state_.load() != State::Closed) FinishClosed(1006, std::string());
    std::lock_guard<std::mutex> lock(sendQueueMutex_);
    receiveDone_ = true;
    sendCv_.notify_all();
}

bool Connection::Handshake() {
    // Stores a freshly created handle unless the connection was cancelled meanwhile.
    auto keep = [&](void** slot, HINTERNET h) -> bool {
        if (!h) return false;
        std::lock_guard<std::mutex> lock(handleMutex_);
        if (stop_ || state_.load() != State::Connecting) {
            WinHttpCloseHandle(h);
            return false;
        }
        *slot = h;
        return true;
    };
    auto cancelledOr = [&](DWORD err, const char* what) -> bool {
        if (stop_) return false;
        if (state_.load() == State::Closing) {
            // Close() during the handshake: report the requested close.
            int code;
            std::string reason;
            {
                std::lock_guard<std::mutex> lock(sendQueueMutex_);
                code = closeCode_;
                reason = closeReason_;
            }
            FinishClosed(code, reason);
            return false;
        }
        Fail((int)err, std::string(what) + ": " + ErrorText(err));
        return false;
    };

    // One attempt up to the HTTP response, with the given proxy mode. Returns 0, or the
    // error with *stage set; handles of a failed attempt are closed.
    auto attempt = [&](DWORD access, const char** stage) -> DWORD {
        auto failed = [&](DWORD err, const char* what) -> DWORD {
            *stage = what;
            CloseHandle(&request_);
            CloseHandle(&connect_);
            CloseHandle(&session_);
            return err ? err : ERROR_INTERNAL_ERROR;
        };
        HINTERNET session = WinHttpOpen(L"StormFabric-WebSocket/0.2", access, WINHTTP_NO_PROXY_NAME,
                                        WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return failed(GetLastError(), "WinHttpOpen");
        if (!keep(&session_, session)) return failed(ERROR_WINHTTP_OPERATION_CANCELLED, "cancelled");
        const int t = (int)options_.timeoutMs;
        WinHttpSetTimeouts(session, t, t, t, t);

        HINTERNET connect = WinHttpConnect(session, host_.c_str(), port_, 0);
        if (!connect) return failed(GetLastError(), "connect");
        if (!keep(&connect_, connect)) return failed(ERROR_WINHTTP_OPERATION_CANCELLED, "cancelled");

        HINTERNET request = WinHttpOpenRequest(connect, L"GET", path_.c_str(), nullptr, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES, secure_ ? WINHTTP_FLAG_SECURE : 0);
        if (!request) return failed(GetLastError(), "open request");
        if (!keep(&request_, request)) return failed(ERROR_WINHTTP_OPERATION_CANCELLED, "cancelled");

        if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
            return failed(GetLastError(), "websocket upgrade option");
        if (secure_ && options_.insecure) {
            DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                          SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
        }
        std::string headers;
        if (!options_.protocol.empty()) headers += "Sec-WebSocket-Protocol: " + options_.protocol + "\r\n";
        for (const std::string& h : options_.headers) headers += h + "\r\n";
        if (!headers.empty()) {
            const std::wstring w = Widen(headers);
            if (w.empty() || !WinHttpAddRequestHeaders(request, w.c_str(), (DWORD)w.size(), WINHTTP_ADDREQ_FLAG_ADD))
                return failed(w.empty() ? ERROR_INVALID_PARAMETER : GetLastError(), "request headers");
        }
        if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
            return failed(GetLastError(), "send request");
        if (!WinHttpReceiveResponse(request, nullptr)) return failed(GetLastError(), "receive response");
        return 0;
    };

    // Errors that a different proxy mode cannot fix.
    auto definitive = [](DWORD err) {
        return err == ERROR_WINHTTP_CANNOT_CONNECT || err == ERROR_WINHTTP_NAME_NOT_RESOLVED ||
               err == ERROR_WINHTTP_TIMEOUT || err == ERROR_WINHTTP_SECURE_FAILURE ||
               err == ERROR_WINHTTP_OPERATION_CANCELLED;
    };
    struct Mode { DWORD access; const char* name; };
    std::vector<Mode> modes;
    if (options_.proxy == Proxy::Direct) modes = {{WINHTTP_ACCESS_TYPE_NO_PROXY, "direct"}};
    else if (options_.proxy == Proxy::System) modes = {{WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, "system proxy"}};
    else modes = {{WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, "auto proxy"}, {WINHTTP_ACCESS_TYPE_NO_PROXY, "direct"}};

    std::string history;
    DWORD err = 0;
    const char* stage = "";
    for (size_t i = 0; i < modes.size(); ++i) {
        err = attempt(modes[i].access, &stage);
        if (!err) {
            if (!history.empty() && options_.log) options_.log("connected " + std::string(modes[i].name) + " after: " + history);
            break;
        }
        const std::string line = std::string(modes[i].name) + " " + stage + ": " + ErrorText(err);
        if (options_.log) options_.log("attempt failed: " + line);
        history += (history.empty() ? "" : "; ") + line;
        if (stop_ || state_.load() != State::Connecting || definitive(err)) break;
    }
    if (err) {
        if (stop_ || state_.load() == State::Closing) return cancelledOr(err, "");
        Fail((int)err, history);
        return false;
    }
    HINTERNET request;
    {
        std::lock_guard<std::mutex> lock(handleMutex_);
        request = request_;
    }
    if (!request) return cancelledOr(ERROR_WINHTTP_OPERATION_CANCELLED, "cancelled");

    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
        return cancelledOr(GetLastError(), "query status");
    if (status != 101) {
        if (stop_ || state_.load() == State::Closing) return cancelledOr(0, "");
        Fail((int)status, "server refused the upgrade: HTTP " + std::to_string(status));
        return false;
    }

    // An open socket can stay silent for a long time: no receive timeout from here on.
    DWORD infinite = 0;
    WinHttpSetOption(request, WINHTTP_OPTION_RECEIVE_TIMEOUT, &infinite, sizeof(infinite));
    HINTERNET socket = WinHttpWebSocketCompleteUpgrade(request, 0);
    if (!socket) return cancelledOr(GetLastError(), "complete upgrade");
    if (!keep(&socket_, socket)) return cancelledOr(ERROR_WINHTTP_OPERATION_CANCELLED, "cancelled");
    CloseHandle(&request_);

    State expected = State::Connecting;
    if (!state_.compare_exchange_strong(expected, State::Open)) return cancelledOr(0, "");
    Event e;
    e.type = EventType::Open;
    PushEvent(std::move(e), true);
    return true;
}

void Connection::ReceiveLoop() {
    std::vector<char> buffer(kReceiveChunk);
    std::string message;
    bool inMessage = false, messageBinary = false, tooBig = false;
    for (;;) {
        HINTERNET socket = WebSocketHandle();
        if (!socket || stop_) return;
        DWORD read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        const DWORD err = WinHttpWebSocketReceive(socket, buffer.data(), (DWORD)buffer.size(), &read, &type);
        if (err != NO_ERROR) {
            if (!stop_ && state_.load() != State::Closing) Fail((int)err, "receive: " + ErrorText(err));
            FinishClosed(1006, state_.load() == State::Closing ? "close handshake not completed" : "");
            return;
        }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            USHORT code = 0;
            char reason[kMaxCloseReason + 2] = {};
            DWORD reasonLen = 0;
            if (WinHttpWebSocketQueryCloseStatus(socket, &code, reason, sizeof(reason), &reasonLen) != NO_ERROR) {
                code = 1005;
                reasonLen = 0;
            }
            State expected = State::Open;
            if (state_.compare_exchange_strong(expected, State::Closing)) {
                // Server-initiated close: answer with a close frame.
                std::lock_guard<std::mutex> wire(wireSendMutex_);
                WinHttpWebSocketClose(socket, ValidEchoCloseCode(code) ? code : 1000, nullptr, 0);
            }
            FinishClosed(code, std::string(reason, reasonLen));
            return;
        }
        if (tooBig) continue; // discard until the server answers our close frame

        const bool binary = type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE ||
                            type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE;
        const bool final = type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE ||
                           type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
        if (!inMessage) {
            inMessage = true;
            messageBinary = binary;
            message.clear();
        }
        if (message.size() + read > options_.maxMessageBytes) {
            tooBig = true;
            message.clear();
            message.shrink_to_fit();
            Fail(1009, "incoming message exceeds " + std::to_string(options_.maxMessageBytes) + " bytes");
            State expected = State::Open;
            if (state_.compare_exchange_strong(expected, State::Closing)) {
                static const char kReason[] = "message too big";
                std::lock_guard<std::mutex> wire(wireSendMutex_);
                WinHttpWebSocketShutdown(socket, 1009, (PVOID)kReason, (DWORD)(sizeof(kReason) - 1));
            }
            continue;
        }
        message.append(buffer.data(), read);
        if (final) {
            Event e;
            e.type = EventType::Message;
            e.binary = messageBinary;
            e.data = std::move(message);
            PushEvent(std::move(e), false);
            message = std::string();
            inMessage = false;
        }
    }
}

void Connection::SendThread() {
    ThreadCounter counter;
    std::unique_lock<std::mutex> lock(sendQueueMutex_);
    for (;;) {
        sendCv_.wait(lock, [&] {
            return stop_ || receiveDone_ || (!sendQueue_.empty() && state_.load() != State::Connecting);
        });
        if (stop_ || receiveDone_) return;
        Outgoing item = std::move(sendQueue_.front());
        sendQueue_.pop_front();
        if (!item.isClose) pendingSendBytes_ -= item.data.size();
        lock.unlock();

        HINTERNET socket = WebSocketHandle();
        if (!socket) {
            lock.lock();
            continue;
        }
        if (item.isClose) {
            DWORD err;
            {
                std::lock_guard<std::mutex> wire(wireSendMutex_);
                err = WinHttpWebSocketShutdown(socket, (USHORT)item.code, item.data.empty() ? nullptr : &item.data[0],
                                               (DWORD)item.data.size());
            }
            lock.lock();
            // Wait for the server's close frame; force the socket closed if it never comes.
            const bool done = err == NO_ERROR &&
                              sendCv_.wait_for(lock, kCloseTimeout, [&] { return stop_ || receiveDone_; });
            if (!done) {
                lock.unlock();
                CloseHandle(&socket_);
                lock.lock();
            }
            sendQueue_.clear();
            pendingSendBytes_ = 0;
            return;
        }
        DWORD err;
        {
            std::lock_guard<std::mutex> wire(wireSendMutex_);
            err = WinHttpWebSocketSend(socket,
                                       item.binary ? WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE
                                                   : WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                       item.data.empty() ? nullptr : &item.data[0], (DWORD)item.data.size());
        }
        if (err != NO_ERROR) {
            if (!stop_ && state_.load() == State::Open) Fail((int)err, "send: " + ErrorText(err));
            CloseHandle(&socket_); // the receive thread ends and reports the close
        }
        lock.lock();
    }
}

} // namespace wsc
