// Shared boundary: Windows handle RAII, UTF-8/UTF-16 conversion, command encoding and startup configuration.
// OS paths use Windows UTF-16; protocol/log text uses UTF-8. UI text positions use UTF-16 units.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <richedit.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <bcrypt.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <utility>
#include <cstdio>
#include <cctype>
#include <vector>

namespace sm
{
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// Noncopyable owning handle. Shared serial connections retain it with shared_ptr until in-flight I/O releases it.
struct Handle
{
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;

    explicit Handle(HANDLE h)
        : value(h)
    {
    }

    ~Handle()
    {
        reset();
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    Handle(Handle&& other) noexcept
        : value(std::exchange(other.value, INVALID_HANDLE_VALUE))
    {
    }

    Handle& operator=(Handle&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            value = std::exchange(other.value, INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    explicit operator bool() const
    {
        return value && value != INVALID_HANDLE_VALUE;
    }

    void reset(HANDLE h = INVALID_HANDLE_VALUE)
    {
        if (*this)
        {
            CloseHandle(value);
        }
        value = h;
    }
};

std::wstring widen(const std::string& value);
std::string utf8(const std::wstring& value);
std::string win_error(DWORD code = GetLastError());
std::string stamp(bool date = false);
std::string trim(std::string value);
std::string hex_text(const std::string& bytes);
std::string parse_hex(const std::string& text);
std::string decode_escapes(const std::string& text);
std::string command_bytes(const std::wstring& text, bool hex, const std::string& eol);
fs::path executable();
std::wstring quote_arg(const std::wstring& arg);
std::vector<std::string> ports();

struct Palette
{
    COLORREF fg;
    COLORREF bg;
    COLORREF dim;
    explicit Palette(const std::string& theme);
    static COLORREF ansi(unsigned index);
};

// Configuration is a startup value snapshot. main reloads port/baud; mutable Config is not shared across threads.
struct Config
{
    std::string port;
    std::string mode = "both";
    std::string theme = "dark";
    std::string eol = "crlf";
    DWORD baud = 115200;
    unsigned mcp_port = 8010;
    unsigned keep_days = 30;
    unsigned flush_ms = 100;
    unsigned flush_bytes = 4096;
    unsigned max_lines = 500;
    uint64_t rotate = 10 * 1024 * 1024;
    bool hex = false;
    bool watch = false;
    fs::path log_dir = L"logs";
    fs::path config_path;
};

Config read_config(const fs::path& path, Config base = {});
Config options(const std::vector<std::wstring>& args);
} // namespace sm
