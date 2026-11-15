// WebSocket client core (ws:// and wss://) on top of WinHTTP.
// Independent of the StormFabric SDK so it can be tested outside the game.
// Thread model: every public method is thread-safe and never blocks on the network.
// Each connection owns one receive thread (handshake + receive loop) and one send thread.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wsc {

enum class State { Connecting, Open, Closing, Closed };
enum class EventType { Open, Message, Close, Error };

struct Event {
    EventType type = EventType::Error;
    std::string data;    // message payload, close reason or error text
    bool binary = false; // Message only
    int code = 0;        // Close: close code (1005 when none); Error: Win32/WinHTTP code or HTTP status
};

// auto: WinHTTP automatic proxy, then a direct connection if that fails for a non-network
// reason. direct: never a proxy. system: the WinHTTP proxy configured with netsh.
enum class Proxy { Auto, Direct, System };

struct Options {
    std::string protocol;              // Sec-WebSocket-Protocol (optional)
    Proxy proxy = Proxy::Auto;
    std::function<void(const std::string&)> log; // diagnostics (any thread), optional
    std::vector<std::string> headers;  // "Name: value", no CR/LF
    bool insecure = false;             // wss: accept invalid/self-signed certificates
    uint32_t timeoutMs = 10000;        // DNS/connect/handshake timeout (not applied once open)
    size_t maxMessageBytes = 1048576;  // incoming message limit (close 1009 above)
    size_t maxQueuedEvents = 256;      // incoming events kept before dropping messages
    size_t maxPendingSendBytes = 4u * 1048576; // outgoing bytes waiting to be sent
};

// Parses "key=value;key=value". Keys: protocol, header (repeatable), insecure, timeout
// (seconds), proxy (auto|direct|system). Unknown keys and malformed values are errors.
bool ParseOptions(const std::string& text, Options* out, std::string* error);

class Connection {
public:
    // Validates the URL (ws/wss only) and starts connecting in the background. Returns
    // nullptr with *error set when the URL or options are invalid; network failures are
    // reported later as an Error event followed by a Close event.
    static std::unique_ptr<Connection> Create(const std::string& url, const Options& options,
                                              std::string* error);
    ~Connection(); // cancels everything and joins both threads

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // Queues a message. False when not open, closing, or over maxPendingSendBytes.
    bool Send(const std::string& data, bool binary);
    // Starts the closing handshake (code 1000..4999, reason <= 123 bytes). False when
    // already closing/closed. A connection still connecting is cancelled.
    bool Close(int code, const std::string& reason);
    // Pops the next event. False when none is pending.
    bool Poll(Event* out);
    // Pops the oldest Message event, leaving other events queued in order.
    bool PollMessage(Event* out);
    size_t Pending() const;
    State GetState() const { return state_.load(); }
    const std::string& Url() const { return url_; }

    // Number of worker threads alive in the process (diagnostics/tests).
    static int LiveThreads();

private:
    Connection() = default;
    void ReceiveThread();
    void SendThread();
    bool Handshake();
    void ReceiveLoop();
    void PushEvent(Event event, bool always);
    void Fail(int code, const std::string& text);
    void FinishClosed(int code, const std::string& reason);
    void CloseHandle(void** handle);
    void* WebSocketHandle();

    struct Outgoing {
        std::string data;
        bool binary = false;
        bool isClose = false;
        int code = 0;
    };

    std::string url_;
    Options options_;
    bool secure_ = false;
    std::wstring host_, path_;
    uint16_t port_ = 0;

    std::atomic<State> state_{State::Connecting};
    std::atomic<bool> stop_{false};

    mutable std::mutex eventsMutex_;
    std::deque<Event> events_;
    bool overflowReported_ = false;
    bool closedReported_ = false;

    std::mutex sendQueueMutex_;
    std::condition_variable sendCv_;
    std::deque<Outgoing> sendQueue_;
    size_t pendingSendBytes_ = 0;
    bool receiveDone_ = false;
    bool closeRequested_ = false;
    int closeCode_ = 1000;
    std::string closeReason_;

    std::mutex handleMutex_; // guards the handle values below
    void* session_ = nullptr;
    void* connect_ = nullptr;
    void* request_ = nullptr;
    void* socket_ = nullptr;
    std::mutex wireSendMutex_; // one WinHTTP send (data or close frame) at a time

    std::thread receiveThread_;
    std::thread sendThread_;
};

} // namespace wsc
