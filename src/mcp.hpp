// Streamable HTTP MCP and restart coordination. Network threads resolve sessions through SessionManager, never HWNDs.
// One HTTP connection per request with JSON responses and no SSE push. Sessions have count and idle-time limits.
#pragma once
#include "sessions.hpp"
#include "json.hpp"
#include <set>

namespace sm
{
class Restart
{
    std::vector<std::wstring> args_;
    std::mutex mutex_;
    uint64_t deadline_ = 0;

public:
    Restart(std::vector<std::wstring> args)
        : args_(std::move(args))
    {
    }

    std::string request();
    bool ready();
    static int helper(const std::vector<std::wstring>& args);
};

class Mcp
{
    struct Session
    {
        std::string name;
        bool initialized = false;
        Clock::time_point seen = Clock::now();
    };

    SessionManager& serials_;
    Restart& restart_;
    unsigned port_;
    SOCKET listen_ = INVALID_SOCKET;
    std::atomic<bool> stopping_ = false;
    bool winsock_ = false;
    std::thread accept_;
    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<SOCKET> queued_;
    std::set<SOCKET> active_;
    std::map<std::string, Session> sessions_;
    void accept_loop();
    void worker();
    void client(SOCKET socket);
    void publish();
    void expire();
    Json dispatch(const Json& request, const std::string& session);
    Json tools();
    Json call(const std::string& name, const Json& args);

public:
    Mcp(SessionManager& serials, Restart& restart, unsigned port)
        : serials_(serials),
          restart_(restart),
          port_(port)
    {
    }

    ~Mcp()
    {
        stop();
    }

    void start();
    void stop();
};
} // namespace sm
