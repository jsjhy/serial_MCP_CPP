// Value conversion/config parsing creates no threads. TOML supports project scalar keys and [gui], plus legacy [tui].
// Unknown file keys are ignored; invalid known types/ranges fail. CLI overrides files, which override defaults.
#include "common.hpp"
#include "json.hpp"
#include <cctype>
#include <utility>

namespace sm
{
std::wstring widen(const std::string& s)
{
    if (s.empty())
    {
        return {};
    }
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0);
    if (!n)
    {
        throw std::runtime_error("invalid UTF-8");
    }
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string utf8(const std::wstring& w)
{
    if (w.empty())
    {
        return {};
    }
    int n = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        w.data(),
        int(w.size()),
        nullptr,
        0,
        nullptr,
        nullptr
    );
    if (!n)
    {
        throw std::runtime_error("invalid UTF-16");
    }
    std::string s(n, 0);
    WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        w.data(),
        int(w.size()),
        s.data(),
        n,
        nullptr,
        nullptr
    );
    return s;
}

std::string win_error(DWORD code)
{
    wchar_t* p = nullptr;
    FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
        reinterpret_cast<wchar_t*>(&p),
        0,
        nullptr
    );
    std::string s = p ? utf8(p) : "Windows error";
    if (p)
    {
        LocalFree(p);
    }
    return trim(s) + " (os error " + std::to_string(code) + ")";
}

std::string trim(std::string s)
{
    auto ws = [](unsigned char c)
    {
        return std::isspace(c);
    };
    while (!s.empty() && ws(s.back()))
    {
        s.pop_back();
    }
    auto i = std::find_if_not(s.begin(), s.end(), ws);
    s.erase(s.begin(), i);
    return s;
}

std::string stamp(bool date)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[64];
    if (date)
    {
        std::snprintf(
            b,
            sizeof b,
            "%04u-%02u-%02u %02u:%02u:%02u.%03u",
            t.wYear,
            t.wMonth,
            t.wDay,
            t.wHour,
            t.wMinute,
            t.wSecond,
            t.wMilliseconds
        );
    }
    else
    {
        std::snprintf(
            b,
            sizeof b,
            "%02u:%02u:%02u.%03u",
            t.wHour,
            t.wMinute,
            t.wSecond,
            t.wMilliseconds
        );
    }
    return b;
}

std::string hex_text(const std::string& bytes)
{
    static const char* d = "0123456789ABCDEF";
    std::string out;
    for (unsigned char b : bytes)
    {
        if (!out.empty())
        {
            out += ' ';
        }
        out += d[b >> 4];
        out += d[b & 15];
    }
    return out;
}

std::string parse_hex(const std::string& text)
{
    std::istringstream in(text);
    std::string token, all, out;
    while (in >> token)
    {
        if (token.starts_with("0x"))
        {
            token.erase(0, 2);
        }
        all += token;
    }
    if (all.size() % 2)
    {
        throw std::runtime_error("odd number of hex digits");
    }
    auto digit = [](char c) -> int
    {
        if (c >= '0' && c <= '9')
        {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f')
        {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F')
        {
            return c - 'A' + 10;
        }
        throw std::runtime_error("invalid hex character");
    };
    for (size_t i = 0; i < all.size(); i += 2)
    {
        out += char(digit(all[i]) * 16 + digit(all[i + 1]));
    }
    return out;
}

std::string decode_escapes(const std::string& s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] != '\\' || i + 1 == s.size())
        {
            o += s[i];
            continue;
        }
        char c = s[i + 1];
        if (c == 'r' || c == 'n' || c == 't' || c == '0' || c == '\\')
        {
            ++i;
            o += c == 'r' ? '\r' : c == 'n' ? '\n' : c == 't' ? '\t' : c == '0' ? '\0' : '\\';
        }
        else if ((c == 'x' || c == 'X') && i + 3 < s.size())
        {
            try
            {
                auto b = parse_hex(s.substr(i + 2, 2));
                o += b;
                i += 3;
            }
            catch (...)
            {
                o += s[i];
            }
        }
        else
        {
            o += s[i];
        }
    }
    return o;
}

std::string command_bytes(const std::wstring& s, bool hex, const std::string& eol)
{
    if (hex)
    {
        return parse_hex(utf8(s));
    }
    auto b = decode_escapes(utf8(s));
    if (!b.empty() && (b.back() == '\r' || b.back() == '\n'))
    {
        return b;
    }
    if (eol == "crlf")
    {
        b += "\r\n";
    }
    else if (eol == "cr")
    {
        b += "\r";
    }
    else if (eol == "lf")
    {
        b += "\n";
    }
    return b;
}

fs::path executable()
{
    std::vector<wchar_t> b(32768);
    auto n = GetModuleFileNameW(nullptr, b.data(), DWORD(b.size()));
    if (!n || n == b.size())
    {
        throw std::runtime_error("executable path unavailable");
    }
    return std::wstring(b.data(), n);
}

std::wstring quote_arg(const std::wstring& s)
{
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : s)
    {
        if (c == '\\')
        {
            ++slashes;
            continue;
        }
        out.append(slashes * (c == '"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == '"')
        {
            out += L'\\';
        }
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L"\"";
}

std::vector<std::string> ports()
{
    std::vector<wchar_t> b(32768);
    DWORD n = 0;
    while (!(n = QueryDosDeviceW(nullptr, b.data(), DWORD(b.size()))))
    {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || b.size() > 1024 * 1024)
        {
            throw std::runtime_error(win_error());
        }
        b.resize(b.size() * 2);
    }
    std::vector<std::string> out;
    for (const wchar_t* p = b.data(); *p; p += wcslen(p) + 1)
    {
        std::wstring s = p;
        if (s.size() > 3 && s.starts_with(L"COM") &&
            std::all_of(
                s.begin() + 3,
                s.end(),
                [](wchar_t c)
                {
                    return c >= '0' && c <= '9';
                }
            ))
        {
            out.push_back(utf8(s));
        }
    }
    std::sort(
        out.begin(),
        out.end(),
        [](auto& a, auto& b)
        {
            return a.size() == b.size() ? a < b : a.size() < b.size();
        }
    );
    return out;
}

Palette::Palette(const std::string& s)
{
    fg = RGB(220, 224, 232);
    bg = RGB(20, 23, 29);
    if (s == "light")
    {
        fg = RGB(32, 36, 42);
        bg = RGB(250, 250, 250);
    }
    else if (s == "nord")
    {
        fg = RGB(216, 222, 233);
        bg = RGB(46, 52, 64);
    }
    else if (s == "solarized")
    {
        fg = RGB(131, 148, 150);
        bg = RGB(0, 43, 54);
    }
    dim =
        RGB((GetRValue(fg) + GetRValue(bg)) / 2,
            (GetGValue(fg) + GetGValue(bg)) / 2,
            (GetBValue(fg) + GetBValue(bg)) / 2);
}

COLORREF Palette::ansi(unsigned n)
{
    if (n >= 232)
    {
        unsigned v = 8 + 10 * (std::min(n, 255u) - 232);
        return RGB(v, v, v);
    }
    if (n >= 16)
    {
        n -= 16;
        auto c = [](unsigned v)
        {
            return v ? 55 + v * 40 : 0;
        };
        return RGB(c(n / 36), c(n / 6 % 6), c(n % 6));
    }
    static COLORREF c[] = {RGB(35, 35, 35),
        RGB(235, 105, 105),
        RGB(110, 200, 140),
        RGB(225, 195, 110),
        RGB(115, 165, 235),
        RGB(195, 140, 220),
        RGB(110, 200, 215),
        RGB(220, 220, 220),
        RGB(128, 128, 128),
        RGB(255, 135, 135),
        RGB(135, 235, 160),
        RGB(245, 225, 140),
        RGB(140, 190, 255),
        RGB(225, 165, 245),
        RGB(140, 230, 245),
        RGB(255, 255, 255)};
    return c[n % 16];
}

namespace
{
uint64_t number(const std::string& s, uint64_t lo, uint64_t hi)
{
    if (s.empty() || s[0] == '-')
    {
        throw std::runtime_error("invalid unsigned value");
    }
    size_t n = 0;
    auto v = std::stoull(s, &n);
    if (n != s.size() || v < lo || v > hi)
    {
        throw std::runtime_error("numeric setting out of range");
    }
    return v;
}

void setting(Config& c, std::string k, std::string s, bool file)
{
    auto string = [&]()
    {
        if (file)
        {
            auto j = Json::parse(s);
            if (!j.is_string())
            {
                throw std::runtime_error("expected string: " + k);
            }
            return j.text();
        }
        return s;
    };
    auto boolean = [&]()
    {
        if (s == "true")
        {
            return true;
        }
        if (s == "false")
        {
            return false;
        }
        throw std::runtime_error("expected boolean: " + k);
    };
    if (k == "port")
    {
        c.port = string();
    }
    else if (k == "baud")
    {
        c.baud = DWORD(number(s, 1, 4000000));
    }
    else if (k == "mode")
    {
        c.mode = string();
        if (c.mode != "both" && c.mode != "gui" && c.mode != "mcp")
        {
            throw std::runtime_error("invalid mode");
        }
    }
    else if (k == "log_dir")
    {
        c.log_dir = widen(string());
    }
    else if (k == "mcp_port")
    {
        c.mcp_port = unsigned(number(s, 1, 65535));
    }
    else if (k == "log_rotate")
    {
        c.rotate = number(s, 1024, 1ull << 40);
    }
    else if (k == "log_keep_days")
    {
        c.keep_days = unsigned(number(s, 0, 36500));
    }
    else if (k == "log_flush_bytes")
    {
        c.flush_bytes = unsigned(number(s, 1, 16 * 1024 * 1024));
    }
    else if (k == "log_flush_ms")
    {
        c.flush_ms = unsigned(number(s, 1, 60000));
    }
    else if (k == "theme")
    {
        c.theme = string();
        if (c.theme != "dark" && c.theme != "light" && c.theme != "nord" && c.theme != "solarized")
        {
            throw std::runtime_error("invalid theme");
        }
    }
    else if (k == "line_ending")
    {
        c.eol = string();
        if (c.eol != "none" && c.eol != "cr" && c.eol != "lf" && c.eol != "crlf")
        {
            throw std::runtime_error("invalid line ending");
        }
    }
    else if (k == "input_hex" || k == "hex_view")
    {
        c.hex = boolean();
    }
    else if (k == "max_lines")
    {
        c.max_lines = unsigned(number(s, 50, 2000));
    }
    else if (!file)
    {
        throw std::runtime_error("unknown option: " + k);
    }
}
} // namespace

Config read_config(const fs::path& p, Config c)
{
    std::ifstream in(p);
    if (!in)
    {
        throw std::runtime_error("cannot read config: " + utf8(p.wstring()));
    }
    std::string line, section;
    unsigned no = 0;
    while (std::getline(in, line))
    {
        ++no;
        bool quoted = false, escape = false;
        size_t end = line.size();
        for (size_t i = 0; i < line.size(); ++i)
        {
            char ch = line[i];
            if (ch == '"' && !escape)
            {
                quoted = !quoted;
            }
            if (ch == '#' && !quoted)
            {
                end = i;
                break;
            }
            escape = ch == '\\' && !escape;
        }
        line = trim(line.substr(0, end));
        if (line.empty())
        {
            continue;
        }
        if (line.front() == '[')
        {
            if (line.back() != ']')
            {
                throw std::runtime_error("invalid TOML section");
            }
            section = line.substr(1, line.size() - 2);
            continue;
        }
        auto pos = line.find('=');
        if (pos == std::string::npos)
        {
            throw std::runtime_error("invalid config line " + std::to_string(no));
        }
        if (!section.empty() && section != "gui" && section != "tui")
        {
            continue;
        }
        auto key = trim(line.substr(0, pos));
        auto val = trim(line.substr(pos + 1));
        setting(c, key, val, true);
    }
    c.config_path = p;
    return c;
}

Config options(const std::vector<std::wstring>& a)
{
    Config c;
    fs::path path = executable().parent_path() / L"config.toml";
    bool explicit_config = false;
    for (size_t i = 1; i < a.size(); ++i)
    {
        if (a[i] == L"--config")
        {
            if (++i == a.size())
            {
                throw std::runtime_error("missing config path");
            }
            path = a[i];
            explicit_config = true;
        }
    }
    if (explicit_config || fs::exists(path))
    {
        c = read_config(path);
    }
    for (size_t i = 1; i < a.size(); ++i)
    {
        auto key = utf8(a[i]);
        if (key == "--config")
        {
            ++i;
            continue;
        }
        if (key == "--watch-config")
        {
            c.watch = true;
            continue;
        }
        if (key == "--help")
        {
            throw std::runtime_error(
                "Options: --port COM8 --baud 115200 --mode both|gui|mcp --config file.toml --mcp-port 8010 --log-dir logs --watch-config"
            );
        }
        if (key == "-p")
        {
            key = "--port";
        }
        if (key == "-b")
        {
            key = "--baud";
        }
        if (!key.starts_with("--") || ++i == a.size())
        {
            throw std::runtime_error("invalid argument");
        }
        key.erase(0, 2);
        std::replace(key.begin(), key.end(), '-', '_');
        setting(c, key, utf8(a[i]), false);
    }
    return c;
}
} // namespace sm
