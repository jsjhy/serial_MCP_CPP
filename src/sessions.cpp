// Session registry: private log directories isolate rotation/cleanup. Device I/O never holds the registry lock.
// shared_ptr keeps objects alive across removal, shutdown and MCP requests; stopped cores cannot reopen.
#include "sessions.hpp"

namespace sm
{
namespace
{
Config session_config(Config config, const std::string& id)
{
    config.log_dir /= widen(id);
    return config;
}
} // namespace

SerialSession::SerialSession(std::string identity, Config settings)
    : id(std::move(identity)),
      config(session_config(std::move(settings), id)),
      log(config),
      core(log, config.baud)
{
}

SerialSession::~SerialSession()
{
    stop();
}

void SerialSession::stop()
{
    std::lock_guard lock(stop_mutex_);
    log.release_producers();
    core.stop();
    log.stop();
}

SessionManager::SessionManager(Config config)
    : config_(std::move(config))
{
    create();
}

SessionManager::~SessionManager()
{
    stop();
}

std::shared_ptr<SerialSession> SessionManager::create()
{
    std::lock_guard lock(mutex_);
    if (stopping_)
    {
        throw std::runtime_error("serial manager stopped");
    }
    if (sessions_.size() >= limit)
    {
        throw std::runtime_error("serial session limit is 16");
    }
    auto id = "serial-" + std::to_string(next_++);
    auto session = std::make_shared<SerialSession>(id, config_);
    session->core.mcp(mcp_);
    sessions_.emplace(id, session);
    return session;
}

std::shared_ptr<SerialSession> SessionManager::get(const std::string& id)
{
    std::lock_guard lock(mutex_);
    if (stopping_)
    {
        throw std::runtime_error("serial manager stopped");
    }
    if (id.empty())
    {
        if (sessions_.size() != 1)
        {
            throw std::runtime_error(
                "multiple serial sessions; specify serial_id (use list_sessions)"
            );
        }
        return sessions_.begin()->second;
    }
    auto i = sessions_.find(id);
    if (i == sessions_.end())
    {
        throw std::runtime_error("unknown serial_id: " + id);
    }
    return i->second;
}

std::vector<std::shared_ptr<SerialSession>> SessionManager::list()
{
    std::lock_guard lock(mutex_);
    std::vector<std::shared_ptr<SerialSession>> result;
    for (const auto& [id, session] : sessions_)
    {
        result.push_back(session);
    }
    return result;
}

void SessionManager::remove(const std::string& id)
{
    std::shared_ptr<SerialSession> removed;
    {
        std::lock_guard lock(mutex_);
        if (stopping_)
        {
            throw std::runtime_error("serial manager stopped");
        }
        auto i = sessions_.find(id);
        if (i == sessions_.end())
        {
            throw std::runtime_error("unknown serial_id: " + id);
        }
        if (sessions_.size() == 1)
        {
            throw std::runtime_error("keep at least one serial session; use close to disconnect");
        }
        removed = std::move(i->second);
        sessions_.erase(i);
    }
    removed->stop();
    if (!removed->log.error().empty())
    {
        throw std::runtime_error("removed " + id + "; log flush failed: " + removed->log.error());
    }
}

void SessionManager::mcp(std::string status)
{
    std::lock_guard lock(mutex_);
    mcp_ = std::move(status);
    for (auto& [id, session] : sessions_)
    {
        session->core.mcp(mcp_);
    }
}

void SessionManager::release_producers()
{
    std::lock_guard lock(mutex_);
    stopping_ = true;
    for (auto& [id, session] : sessions_)
    {
        session->log.release_producers();
    }
}

void SessionManager::stop()
{
    release_producers();
    auto snapshot = list();
    for (auto& session : snapshot)
    {
        session->stop();
    }
}
} // namespace sm
