// Connection registry: numeric ids, per-owner isolation, limits, idle collection.
// Independent of the StormFabric SDK. Thread-safe; never blocks on the network.
#pragma once
#include "ws_client.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace wsc {

struct Limits {
    size_t maxConnections = 32;
    double idleSeconds = 30;        // a connection nobody touched for this long is closed
    size_t maxMessageBytes = 1048576;
    size_t maxQueuedEvents = 256;
    size_t maxPendingSendBytes = 4u * 1048576;
    std::vector<std::string> allowHosts{"*"}; // "*", "host", "*.domain"
    bool logConnections = true;
    bool microcontrollers = true;   // also expose the library to microcontroller Lua
};

// "key=value" lines; '#' starts a comment. Unknown keys are errors.
bool ParseConfig(const std::string& text, Limits* out, std::string* error);
std::string DefaultConfigText();

// "ws://user@Host:1234/path" -> "host" (lowercase, brackets kept for IPv6).
std::string UrlHost(const std::string& url);
bool HostAllowed(const std::vector<std::string>& patterns, const std::string& host);

class Registry {
public:
    using Clock = std::function<double()>;           // seconds, monotonic
    using Logger = std::function<void(const std::string&)>;

    explicit Registry(Clock clock = nullptr, Logger log = nullptr);
    ~Registry();

    void Configure(const Limits& limits);
    Limits GetLimits() const;

    // Owner: opaque token of the calling script VM (0 = unknown, shared).
    // Returns the new id (> 0), or 0 with *error set.
    uint64_t Connect(uint64_t owner, const std::string& url, const std::string& optionsText, std::string* error);
    bool Send(uint64_t owner, uint64_t id, const std::string& data, bool binary);
    bool Close(uint64_t owner, uint64_t id, int code, const std::string& reason);
    // False when the id is unknown (never existed, other owner, or already released).
    bool GetState(uint64_t owner, uint64_t id, State* out);
    size_t Pending(uint64_t owner, uint64_t id);
    // Next event of one connection (id != 0) or of any connection of the owner (id == 0),
    // oldest connection first. A connection is released once its Close event is returned.
    bool Poll(uint64_t owner, uint64_t id, uint64_t* outId, Event* out);
    // Next Message of one connection; other events stay queued in order.
    bool PollMessage(uint64_t owner, uint64_t id, Event* out);

    // Closes idle connections and releases closed ones nobody polls. Returns how many
    // were released. Call periodically (any thread).
    size_t Sweep();
    // Closes every connection of the owner / all connections (joins their threads).
    void CloseOwner(uint64_t owner);
    void CloseAll();
    size_t Count() const;

private:
    struct Entry {
        uint64_t owner = 0;
        std::unique_ptr<Connection> connection;
        double lastTouched = 0;
    };
    double Now() const;
    Entry* Find(uint64_t owner, uint64_t id); // requires mutex_; touches the entry
    static void Destroy(std::vector<std::unique_ptr<Connection>>& dead);

    Clock clock_;
    Logger log_;
    mutable std::mutex mutex_;
    Limits limits_;
    std::map<uint64_t, Entry> entries_; // ordered by id = creation order
    uint64_t nextId_ = 1;
};

} // namespace wsc
