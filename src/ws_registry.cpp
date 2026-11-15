#include "ws_registry.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace wsc {
namespace {

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

std::string Lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool ParseSize(const std::string& v, size_t lo, size_t hi, size_t* out) {
    char* end = nullptr;
    const unsigned long long n = strtoull(v.c_str(), &end, 10);
    if (v.empty() || *end || n < lo || n > hi) return false;
    *out = (size_t)n;
    return true;
}

// Scheme + host + port only: paths and queries may carry tokens.
std::string SafeUrl(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return "?";
    size_t end = url.find_first_of("/?#", scheme + 3);
    std::string authority = url.substr(scheme + 3, end == std::string::npos ? std::string::npos : end - scheme - 3);
    const size_t at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);
    return url.substr(0, scheme + 3) + authority;
}

} // namespace

std::string DefaultConfigText() {
    return "# websocket mod configuration (edit with the game closed)\n"
           "# allow_hosts: comma-separated list: * (any), host, *.domain\n"
           "allow_hosts=*\n"
           "max_connections=32\n"
           "idle_timeout=30\n"
           "max_message_bytes=1048576\n"
           "max_queued_events=256\n"
           "max_pending_send_bytes=4194304\n"
           "log_connections=1\n"
           "# microcontrollers: also give `websocket` to microcontroller Lua (workshop vehicles\n"
           "# included). 0 = addon scripts only.\n"
           "microcontrollers=1\n";
}

bool ParseConfig(const std::string& text, Limits* out, std::string* error) {
    Limits result = *out;
    size_t pos = 0;
    int lineNo = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        ++lineNo;
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        line = Trim(line);
        if (line.empty()) continue;
        const size_t eq = line.find('=');
        auto bad = [&](const char* what) {
            if (error) *error = "line " + std::to_string(lineNo) + ": " + what;
            return false;
        };
        if (eq == std::string::npos) return bad("expected key=value");
        const std::string key = Trim(line.substr(0, eq));
        const std::string value = Trim(line.substr(eq + 1));
        if (key == "allow_hosts") {
            result.allowHosts.clear();
            size_t p = 0;
            while (p <= value.size()) {
                size_t comma = value.find(',', p);
                if (comma == std::string::npos) comma = value.size();
                const std::string h = Lower(Trim(value.substr(p, comma - p)));
                if (!h.empty()) result.allowHosts.push_back(h);
                p = comma + 1;
            }
        } else if (key == "max_connections") {
            if (!ParseSize(value, 1, 256, &result.maxConnections)) return bad("max_connections: 1..256");
        } else if (key == "idle_timeout") {
            char* e = nullptr;
            const double s = strtod(value.c_str(), &e);
            if (value.empty() || *e || !(s >= 1 && s <= 86400)) return bad("idle_timeout: 1..86400 seconds");
            result.idleSeconds = s;
        } else if (key == "max_message_bytes") {
            if (!ParseSize(value, 1, 1048576, &result.maxMessageBytes)) return bad("max_message_bytes: 1..1048576");
        } else if (key == "max_queued_events") {
            if (!ParseSize(value, 1, 65536, &result.maxQueuedEvents)) return bad("max_queued_events: 1..65536");
        } else if (key == "max_pending_send_bytes") {
            if (!ParseSize(value, 1, 64u * 1048576, &result.maxPendingSendBytes))
                return bad("max_pending_send_bytes: 1..67108864");
        } else if (key == "log_connections") {
            if (value == "1") result.logConnections = true;
            else if (value == "0") result.logConnections = false;
            else return bad("log_connections: 0 or 1");
        } else if (key == "microcontrollers") {
            if (value == "1") result.microcontrollers = true;
            else if (value == "0") result.microcontrollers = false;
            else return bad("microcontrollers: 0 or 1");
        } else {
            return bad(("unknown key: " + key).c_str());
        }
    }
    *out = result;
    return true;
}

std::string UrlHost(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return std::string();
    const size_t end = url.find_first_of("/?#", scheme + 3);
    std::string authority = url.substr(scheme + 3, end == std::string::npos ? std::string::npos : end - scheme - 3);
    const size_t at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);
    if (!authority.empty() && authority[0] == '[') {
        const size_t close = authority.find(']');
        return Lower(close == std::string::npos ? authority : authority.substr(0, close + 1));
    }
    const size_t colon = authority.find(':');
    if (colon != std::string::npos) authority.resize(colon);
    return Lower(authority);
}

bool HostAllowed(const std::vector<std::string>& patterns, const std::string& host) {
    if (host.empty()) return false;
    for (const std::string& p : patterns) {
        if (p == "*" || p == host) return true;
        if (p.size() > 2 && p[0] == '*' && p[1] == '.') {
            const std::string suffix = p.substr(1); // ".domain"
            if (host.size() > suffix.size() && host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0)
                return true;
        }
    }
    return false;
}

Registry::Registry(Clock clock, Logger log) : clock_(std::move(clock)), log_(std::move(log)) {}

Registry::~Registry() { CloseAll(); }

double Registry::Now() const {
    if (clock_) return clock_();
    return (double)GetTickCount64() / 1000.0;
}

void Registry::Configure(const Limits& limits) {
    std::lock_guard<std::mutex> lock(mutex_);
    limits_ = limits;
}

Limits Registry::GetLimits() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return limits_;
}

Registry::Entry* Registry::Find(uint64_t owner, uint64_t id) {
    auto it = entries_.find(id);
    if (it == entries_.end() || it->second.owner != owner) return nullptr;
    it->second.lastTouched = Now();
    return &it->second;
}

void Registry::Destroy(std::vector<std::unique_ptr<Connection>>& dead) {
    dead.clear(); // joins the worker threads, outside the registry lock
}

uint64_t Registry::Connect(uint64_t owner, const std::string& url, const std::string& optionsText,
                           std::string* error) {
    Sweep();
    Limits limits = GetLimits();
    Options options;
    options.maxMessageBytes = limits.maxMessageBytes;
    options.maxQueuedEvents = limits.maxQueuedEvents;
    options.maxPendingSendBytes = limits.maxPendingSendBytes;
    if (!ParseOptions(optionsText, &options, error)) return 0;
    if (log_) {
        Logger log = log_;
        const std::string where = SafeUrl(url);
        options.log = [log, where](const std::string& m) { log(where + ": " + m); };
    }
    const std::string host = UrlHost(url);
    if (!HostAllowed(limits.allowHosts, host)) {
        if (error) *error = "host not allowed by websocket.cfg: " + host;
        if (log_) log_("refused " + SafeUrl(url) + " (allow_hosts)");
        return 0;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (entries_.size() >= limits_.maxConnections) {
            if (error) *error = "too many connections (max " + std::to_string(limits_.maxConnections) + ")";
            return 0;
        }
    }
    std::unique_ptr<Connection> c = Connection::Create(url, options, error);
    if (!c) return 0;
    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (entries_.size() < limits_.maxConnections) { // may have raced with another connect
            id = nextId_++;
            Entry& e = entries_[id];
            e.owner = owner;
            e.connection = std::move(c);
            e.lastTouched = Now();
        }
    }
    if (!id) {
        if (error) *error = "too many connections (max " + std::to_string(limits.maxConnections) + ")";
        c.reset(); // outside the lock
        return 0;
    }
    if (limits.logConnections && log_)
        log_("connect #" + std::to_string(id) + " -> " + SafeUrl(url) + (options.insecure ? " (insecure)" : ""));
    return id;
}

bool Registry::Send(uint64_t owner, uint64_t id, const std::string& data, bool binary) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* e = Find(owner, id);
    return e && e->connection->Send(data, binary);
}

bool Registry::Close(uint64_t owner, uint64_t id, int code, const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* e = Find(owner, id);
    return e && e->connection->Close(code, reason);
}

bool Registry::GetState(uint64_t owner, uint64_t id, State* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* e = Find(owner, id);
    if (!e) return false;
    *out = e->connection->GetState();
    return true;
}

size_t Registry::Pending(uint64_t owner, uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* e = Find(owner, id);
    return e ? e->connection->Pending() : 0;
}

bool Registry::Poll(uint64_t owner, uint64_t id, uint64_t* outId, Event* out) {
    std::vector<std::unique_ptr<Connection>> dead;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const double now = Now();
        auto release = entries_.end();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->second.owner != owner || (id && it->first != id)) continue;
            it->second.lastTouched = now;
            if (found) continue; // keep touching the owner's other connections
            if (!it->second.connection->Poll(out)) continue;
            found = true;
            if (outId) *outId = it->first;
            if (out->type == EventType::Close) release = it;
        }
        if (release != entries_.end()) {
            dead.push_back(std::move(release->second.connection));
            entries_.erase(release);
        }
    }
    Destroy(dead);
    return found;
}

bool Registry::PollMessage(uint64_t owner, uint64_t id, Event* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* e = Find(owner, id);
    return e && e->connection->PollMessage(out);
}

size_t Registry::Sweep() {
    std::vector<std::unique_ptr<Connection>> dead;
    std::vector<uint64_t> idle;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const double now = Now();
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (now - it->second.lastTouched > limits_.idleSeconds) {
                idle.push_back(it->first);
                dead.push_back(std::move(it->second.connection));
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (log_)
        for (uint64_t i : idle) log_("released idle connection #" + std::to_string(i));
    const size_t n = dead.size();
    Destroy(dead);
    return n;
}

void Registry::CloseOwner(uint64_t owner) {
    std::vector<std::unique_ptr<Connection>> dead;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->second.owner == owner) {
                dead.push_back(std::move(it->second.connection));
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }
    Destroy(dead);
}

void Registry::CloseAll() {
    std::vector<std::unique_ptr<Connection>> dead;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& kv : entries_) dead.push_back(std::move(kv.second.connection));
        entries_.clear();
    }
    if (!dead.empty() && log_) log_("closing " + std::to_string(dead.size()) + " connection(s)");
    Destroy(dead);
}

size_t Registry::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

} // namespace wsc
