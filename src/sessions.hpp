// Ownership boundary: each SerialSession owns its log thread, Core read/write threads and bounded buffers.
// SessionManager owns the registry and MCP status; GUI/MCP take shared_ptr snapshots.
// Removal unlinks first, then stops I/O/threads outside the registry lock. Retained references cannot reopen it.
#pragma once
#include "core.hpp"

namespace sm
{
struct SerialSession
{
    const std::string
        id; // Monotonic within the process, never reused; unrelated to HTTP MCP session IDs.
    const Config
        config;    // Configuration snapshot; log_dir includes this session's private subdirectory.
    LogWriter log; // Construct before core and destroy after core.
    Core core;

    SerialSession(std::string identity, Config settings);
    ~SerialSession();

    // Repeat/concurrent calls are safe; joins all owned threads before returning. Do not call from those threads.
    void stop();

private:
    std::mutex
        stop_mutex_; // Serializes this session's shutdown without locking other registry operations.
};

class SessionManager
{
    Config config_;
    std::mutex
        mutex_; // Protects registry, IDs and global MCP text. Never hold while joining threads.
    std::map<std::string, std::shared_ptr<SerialSession>> sessions_;
    uint64_t next_ = 1;
    bool stopping_ = false;
    std::string mcp_ = "disabled";

public:
    static constexpr size_t limit =
        16; // Three threads per session; bounds total thread and buffer budgets.
    explicit SessionManager(Config config);
    ~SessionManager();

    // Create an empty session without device I/O. Throw on failure; existing sessions remain unchanged.
    std::shared_ptr<SerialSession> create();
    // An empty ID supports legacy single-session calls. Multiple sessions require explicit routing to prevent misdelivery.
    std::shared_ptr<SerialSession> get(const std::string& id = "");
    std::vector<std::shared_ptr<SerialSession>> list();
    // Keep at least one session; reclaim resources before returning. Log flush failures throw after removal.
    void remove(const std::string& id);
    void mcp(std::string status);
    // Shutdown first rejects new create/lookup requests and releases log backpressure before stopping other producers.
    void release_producers();
    void stop();
};
} // namespace sm
