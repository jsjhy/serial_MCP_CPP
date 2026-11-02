// Process assembly handles the hidden restart helper before creating the session manager, shared MCP and GUI tabs.
// Shutdown order: config watcher, network workers, serial workers, then log-tail flush. No threads are detached.
#include "ui.hpp"

namespace sm
{
int application(std::vector<std::wstring> args)
{
    if (args.size() > 1 && args[1] == L"--internal-restart-helper")
    {
        return Restart::helper(args);
    }
    if (std::find(args.begin(), args.end(), L"--self-test") != args.end())
    {
        return self_test();
    }
    Config cfg = options(args);
    SessionManager sessions(cfg);
    auto primary = sessions.get();
    auto& core = primary->core;
    Restart restart(args);
    Mcp server(sessions, restart, cfg.mcp_port);
    auto selected = cfg.port;
    if (selected.empty())
    {
        auto list = ports();
        if (list.size() == 1)
        {
            selected = list[0];
        }
    }
    if (!selected.empty())
    {
        core.open(selected, cfg.baud);
    }
    bool use_mcp = cfg.mode == "both" || cfg.mode == "mcp";
    if (use_mcp)
    {
        try
        {
            server.start();
        }
        catch (const std::exception& e)
        {
            server.stop();
            sessions.mcp("failed: " + std::string(e.what()));
            if (cfg.mode == "mcp")
            {
                throw;
            }
        }
    }
    std::atomic<bool> stop_watch = false;
    std::thread watch;
    if (cfg.watch && !cfg.config_path.empty())
    {
        watch = std::thread(
            [&]
            {
                std::error_code ec;
                auto last = fs::last_write_time(cfg.config_path, ec);
                while (!stop_watch)
                {
                    for (int i = 0; i < 25 && !stop_watch; ++i)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    }
                    if (stop_watch)
                    {
                        break;
                    }
                    auto now = fs::last_write_time(cfg.config_path, ec);
                    if (now != last)
                    {
                        last = now;
                        try
                        {
                            auto next = read_config(cfg.config_path);
                            // Reload affects only the startup session. A removed session's Core rejects reopening.
                            if (!next.port.empty())
                            {
                                core.open(next.port, next.baud);
                            }
                        }
                        catch (const std::exception& e)
                        {
                            core.notice(e.what());
                        }
                    }
                }
            }
        );
    }
    int result = 0;
    try
    {
        if (cfg.mode == "mcp")
        {
            while (!restart.ready())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        else
        {
            result = gui(sessions, restart, cfg);
        }
    }
    catch (...)
    {
        sessions.release_producers();
        stop_watch = true;
        if (watch.joinable())
        {
            watch.join();
        }
        server.stop();
        sessions.stop();
        throw;
    }
    sessions.release_producers();
    stop_watch = true;
    if (watch.joinable())
    {
        watch.join();
    }
    server.stop();
    sessions.stop();
    for (auto& session : sessions.list())
    {
        if (!session->log.error().empty())
        {
            throw std::runtime_error(session->id + ": " + session->log.error());
        }
    }
    return result;
}
} // namespace sm

static int entry(std::vector<std::wstring> args)
{
    bool testing = std::find(args.begin(), args.end(), L"--self-test") != args.end();
    bool helper = args.size() > 1 && args[1] == L"--internal-restart-helper";
    try
    {
        return sm::application(std::move(args));
    }
    catch (const std::exception& e)
    {
        if (testing)
        {
            std::fprintf(stderr, "%s\n", e.what());
        }
        else if (!helper)
        {
            MessageBoxW(
                nullptr,
                sm::widen(e.what()).c_str(),
                L"Serial Monitor C++",
                MB_OK | MB_ICONERROR
            );
        }
        return 1;
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int count = 0;
    wchar_t** raw = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!raw)
    {
        return 1;
    }
    std::vector<std::wstring> args(raw, raw + count);
    LocalFree(raw);
    return entry(std::move(args));
}
