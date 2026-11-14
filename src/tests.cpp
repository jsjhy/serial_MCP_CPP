// Self-tests use memory, temporary files, invalid device names and hidden native windows, never real serial I/O.
// Assertions throw and return a failing exit code even with NDEBUG. Temporary logs are removed after testing.
#include "ui.hpp"

namespace sm
{
#ifdef SERIAL_MCP_SIMULATED_TESTS
void simulated_serial_log_test(Config config);
#endif

int self_test()
{
    unsigned checks = 0;
    auto check = [&](bool ok, const char* message)
    {
        ++checks;
        if (!ok)
        {
            throw std::runtime_error(message);
        }
    };
    auto rejects = [&](const std::string& json)
    {
        try
        {
            Json::parse(json);
            return false;
        }
        catch (...)
        {
            return true;
        }
    };
    auto j = Json::parse(
        R"({"text":"中文\ud83d\ude00","id":9007199254740993,"ok":true,"items":[null,-1.5e2]})"
    );
    check(j.at("text").text() == "中文😀", "JSON Unicode surrogate pair");
    check(Json::parse(j.dump()).at("id").dump() == "9007199254740993", "JSON integer id precision");
    for (auto s : {"",
             "[1,]",
             "{\"a\":1,\"a\":2}",
             "01",
             "\"\\ud800\"",
             "{\"a\":true}garbage",
             "[",
             "\"\\x00\""})
    {
        check(rejects(s), "invalid JSON accepted");
    }
    check(parse_hex("0x01 ff 00") == std::string("\x01\xff\0", 3), "hex decoding");
    check(command_bytes(L"help", false, "crlf") == "help\r\n", "default CRLF");
    check(command_bytes(L"", false, "cr") == "\r", "empty Enter");
    check(
        command_bytes(L"01 ff", true, "crlf") == std::string("\x01\xff", 2),
        "hex is exact bytes"
    );
    check(decode_escapes("help\\r\\n") == "help\r\n", "ASCII escape decoding");
    check(utf8(widen("中文😀")) == "中文😀", "UTF-8 round trip");
    Palette palette("dark");
    Display filtered(palette);
    filtered.visible = [](const std::wstring& body)
    {
        return body.find(L"ERROR") != std::wstring::npos;
    };
    auto hidden = filtered.feed({Direction::Rx, "noise\nER", "12:00"}, false);
    check(hidden.pieces.empty(), "filter hides nonmatching complete and partial lines");
    auto shown = filtered.feed({Direction::Rx, "ROR\x1b[31m detail", "12:01"}, false);
    check(!shown.pieces.empty(), "filter matches decoded line across packets");
    auto removed = filtered.feed({Direction::Rx, "\nnoise\n", "12:02"}, false);
    check(removed.erase > 0, "filtered partial snapshot is replaced on completion");
    filtered.visible = [](const std::wstring& body)
    {
        return body.find(L"noise") == std::wstring::npos;
    };
    filtered.reset();
    check(
        filtered.feed({Direction::Rx, "noise\n", "12:03"}, false).pieces.empty(),
        "exclude predicate survives parser reset"
    );
    Display display(palette);
    auto first = display.feed({Direction::Rx, "中😀", "12:34:56.789"}, false);
    long size = 0;
    for (auto& p : first.pieces)
    {
        size += long(p.text.size());
    }
    auto second = display.feed({Direction::Rx, "!\n", "12:34:56.999"}, false);
    check(second.erase == size, "partial snapshot UTF-16 replacement");
    std::wstring visible;
    for (auto& p : second.pieces)
    {
        visible += p.text;
    }
    check(visible.find(L"中😀!") != std::wstring::npos, "partial line assembly");
    check(first.pieces.front().color == palette.dim, "dim timestamp");
    // Composing patches must equal sequential UTF-16 replacements, including split Unicode/ANSI.
    Display sequential(palette);
    Display batched(palette);
    Patch combined;
    std::wstring expected;
    for (auto bytes : {std::string("prefix "),
             std::string("\xe4\xb8", 2),
             std::string("\xad", 1) + "\x1b[32mOK",
             std::string("\nnext😀")})
    {
        Record record{Direction::Rx, bytes, "12:34:56.789"};
        auto patch = sequential.feed(record, false);
        expected.resize(expected.size() - size_t(patch.erase));
        for (auto& piece : patch.pieces)
        {
            expected += piece.text;
        }
        combined.append(batched.feed(record, false));
    }
    std::wstring actual;
    for (auto& piece : combined.pieces)
    {
        actual += piece.text;
    }
    check(
        combined.erase == 0 && actual == expected,
        "batched patch composition preserves split text"
    );
    TextParser parser(palette.fg);
    std::wstring decoded;
    COLORREF received = 0;
    auto emit = [&](const std::wstring& s, COLORREF c)
    {
        decoded += s;
        received = c;
    };
    parser.feed(std::string("\xe4\xb8", 2), emit);
    parser.feed(std::string("\xad", 1) + "\x1b[3", emit);
    parser.feed("2mOK\r", emit);
    parser.feed("\n", emit);
    check(decoded == L"中OK\r", "split UTF-8/ANSI and CRLF");
    check(received == Palette::ansi(2), "ANSI color state");
    auto dir =
        fs::temp_directory_path() / (L"serial_cpp_test_" + std::to_wstring(GetCurrentProcessId()) +
                                        L"_" + std::to_wstring(GetTickCount64()));
    Config cfg;
    cfg.log_dir = dir;
    cfg.flush_ms = 10;
    cfg.flush_bytes = 1;
    cfg.rotate = 1024 * 1024;
    cfg.keep_days = 0;
    {
        LogWriter log(cfg);
        Core core(log);
        Restart restart({executable().wstring()});
        check(!core.send("help").empty(), "disconnected send must fail");
        check(!core.open("COM_CPP_SELF_TEST_INVALID", 115200).empty(), "invalid port must fail");
        auto f = core.submit(
            [&]
            {
                return core.send("help");
            }
        );
        check(!f.get().empty(), "queued send failure");
        // A huge record must not bypass a UI tick's byte budget; history remains unsplit.
        core.publish(Direction::Rx, std::string(128 * 1024, 'q'));
        auto display_batch = core.drain(64, 8192);
        size_t batch_bytes = 0;
        for (auto& record : display_batch)
        {
            batch_bytes += record.data.size();
        }
        check(batch_bytes <= 8192, "display drain byte budget");
        check(
            core.history(1).front().data.size() == 128 * 1024,
            "UI slicing preserves MCP history"
        );
        for (int i = 0; i < 1030; ++i)
        {
            core.publish(Direction::Rx, "");
        }
        check(
            core.events_.size() <= 1024 && core.event_bytes_ <= 4 * 1024 * 1024 &&
                core.state().display_dropped > 0,
            "display queue bounded even for empty records"
        );
        native_tests(core, log, restart);
        ++checks;
        log.push({Direction::Rx, std::string("\xe4\xb8", 2), stamp(true)});
        log.push({Direction::Rx, std::string("\xad", 1) + "\x1b[31mRED\x1b[0m\r", stamp(true)});
        log.push({Direction::Rx, "\nTAIL", stamp(true)});
        core.stop();
        log.stop();
        check(log.error().empty(), "log write error");
        std::string content;
        for (auto& e : fs::directory_iterator(dir))
        {
            std::ifstream in(e.path(), std::ios::binary);
            content.append(std::istreambuf_iterator<char>(in), {});
        }
        check(content.find("中RED") != std::string::npos, "readable UTF-8 log");
        check(content.find("TAIL") != std::string::npos, "shutdown flushes log tail");
        check(content.find('\x1b') == std::string::npos, "log must not contain ANSI");
        check(content.find("RX:") == std::string::npos, "log omits RX prefix");
    }
    // Multi-session tests use invalid ports and in-memory tasks; they do not measure real multi-device throughput.
    {
        Config multiple = cfg;
        multiple.baud = 230400;
        SessionManager manager(multiple);
        auto first = manager.get();
        auto second = manager.create();
        check(
            first->core.state().baud == 230400 && second->core.state().baud == 230400,
            "new sessions inherit configured default baud without opening devices"
        );
        check(first->config.log_dir != second->config.log_dir, "separate log directories");
        bool ambiguous = false;
        try
        {
            manager.get();
        }
        catch (const std::exception&)
        {
            ambiguous = true;
        }
        check(ambiguous, "multiple sessions require an explicit serial_id");
        first->core.open("COM_CPP_SESSION_A_INVALID", 9600);
        second->core.open("COM_CPP_SESSION_B_INVALID", 115200);
        check(
            first->core.state().baud == 9600 && second->core.state().baud == 115200,
            "independent port/baud state"
        );
        auto records = first->core.history(4096);
        check(
            std::none_of(
                records.begin(),
                records.end(),
                [](const Record& record)
                {
                    return record.data.find("SESSION_B") != std::string::npos;
                }
            ),
            "histories must not mix"
        );
        // B must complete while A's queue is blocked. Release A before asserting to avoid deadlock on failure.
        std::promise<void> release;
        auto gate = release.get_future().share();
        auto blocked = first->core.submit(
            [gate]
            {
                gate.wait();
                return std::string{};
            }
        );
        auto independent = second->core.submit(
            []
            {
                return std::string("independent");
            }
        );
        bool ready = independent.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        release.set_value();
        blocked.get();
        check(ready && independent.get() == "independent", "independent serial task workers");
        manager.remove(second->id);
        check(
            !second->core.open("COM_CPP_RETIRED_INVALID", 115200).empty(),
            "retained references cannot reopen removed session"
        );
        check(
            !second->core
                .submit(
                    []
                    {
                        return std::string{};
                    }
                )
                .get()
                .empty(),
            "removed session rejects new jobs"
        );
        check(manager.get() == first, "single-session legacy routing restored");
        for (size_t i = 1; i < SessionManager::limit; ++i)
        {
            manager.create();
        }
        bool bounded = false;
        try
        {
            manager.create();
        }
        catch (const std::exception&)
        {
            bounded = true;
        }
        check(
            bounded && manager.list().size() == SessionManager::limit,
            "bounded serial session count"
        );
        for (auto& session : manager.list())
        {
            if (session != first)
            {
                manager.remove(session->id);
            }
        }
        // Keep one session after repeated creation/removal. Handle counts are not a substitute for long-running leak tests.
        DWORD before = 0, after = 0;
        GetProcessHandleCount(GetCurrentProcess(), &before);
        for (int i = 0; i < 30; ++i)
        {
            auto session = manager.create();
            manager.remove(session->id);
        }
        GetProcessHandleCount(GetCurrentProcess(), &after);
        check(after <= before + 4, "serial session thread/handle reclamation");
        std::printf("30 serial sessions: handles %lu -> %lu\n", before, after);
        manager.stop();
        check(first->log.error().empty(), "independent session log flush");
        check(fs::exists(first->config.log_dir), "session log directory created");
    }
    // Persistent disk errors must release backpressured producers so core shutdown cannot hang on a full log queue.
    auto bad = dir / L"not_a_directory";
    {
        std::ofstream file(bad);
        file << "file";
    }
    {
        Config failed = cfg;
        failed.log_dir = bad;
        LogWriter log(failed, 1024);
        Core core(log);
        log.push({Direction::Rx, std::string(2048, 'x'), stamp(true)});
        auto writer = std::async(
            std::launch::async,
            [&]
            {
                for (int i = 0; i < 3; ++i)
                {
                    log.push({Direction::Rx, std::string(2048, 'y'), stamp(true)});
                }
            }
        );
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        core.stop();
        log.stop();
        check(
            writer.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
            "log backpressure must release during shutdown"
        );
        writer.get();
        check(!log.error().empty(), "disk failure must be reported");
    }
#ifdef SERIAL_MCP_SIMULATED_TESTS
    simulated_serial_log_test(cfg);
#endif
    fs::remove_all(dir);
    std::printf(
        "PASS: %u assertions, codecs, JSON, streaming text, log tail, disconnected I/O, native controls/resources\n",
        checks
    );
    return 0;
}
} // namespace sm
