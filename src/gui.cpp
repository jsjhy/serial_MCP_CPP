// WinAPI GUI: the UI thread owns windows, fonts and UTF-16 selections. Serial actions use Core's task queue.
// RX patches precede the draft. Incoming output scrolls to the bottom. No custom shortcuts.
#include "ui.hpp"
#include "resource.h"
#include <cassert>
#include <commdlg.h>
#include <psapi.h>
#include <cstring>
#include <fstream>
#include <tlhelp32.h>

namespace sm
{
namespace
{
constexpr int PORT = 101;
constexpr int BAUD = 102;
constexpr int CONNECTION = 103;
constexpr int REFRESH = 105;
constexpr int EOL = 107;
constexpr int CLEAR = 108;
constexpr int TERM = 109;
constexpr int FILTER_APPLY = 110;
constexpr int FILTER_ENABLE = 113;
constexpr int FIND_NEXT = 114;
constexpr int LOG_TOGGLE = 120;
constexpr int LOG_RELOAD = 121;
constexpr int LOG_MATCH_CASE = 122;
constexpr int FIND_PREVIOUS = 116;

std::wstring get_text(HWND h)
{
    int n = GetWindowTextLengthW(h);
    std::wstring s(size_t(n) + 1, 0);
    n = GetWindowTextW(h, s.data(), int(s.size()));
    s.resize(size_t(n));
    return s;
}

std::wstring rich_text(HWND h)
{
    std::wstring s(size_t(GetWindowTextLengthW(h)) + 2, 0);
    GETTEXTEX request{};
    request.cb = DWORD(s.size() * sizeof(wchar_t));
    request.codepage = 1200;
    auto n = SendMessageW(
        h,
        EM_GETTEXTEX,
        reinterpret_cast<WPARAM>(&request),
        reinterpret_cast<LPARAM>(s.data())
    );
    s.resize(size_t(std::max<LRESULT>(n, 0)));
    return s;
}

void text(HWND h, const std::wstring& s)
{
    if (get_text(h) != s)
    {
        SetWindowTextW(h, s.c_str());
    }
}

CHARRANGE selection(HWND h)
{
    CHARRANGE r{};
    SendMessageW(h, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&r));
    return r;
}

void select(HWND h, LONG begin, LONG end)
{
    CHARRANGE r{begin, end};
    SendMessageW(h, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&r));
}

void color(HWND h, COLORREF value)
{
    CHARFORMATW f{};
    f.cbSize = sizeof f;
    f.dwMask = CFM_COLOR;
    f.crTextColor = value;
    SendMessageW(h, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&f));
}

// Keep the native resize/title-bar behavior without a forced dark frame.
// Windows 11 supports DWMWA_BORDER_COLOR (34); older systems may ignore it.
void window_frame(HWND window)
{
    BOOL dark = FALSE;
    DwmSetWindowAttribute(window, 20, &dark, sizeof dark);
    COLORREF border = 0xFFFFFFFE; // DWMWA_COLOR_NONE: suppress the outer border stroke.
    DwmSetWindowAttribute(window, 34, &border, sizeof border);
    int corner = 2; // DWMWCP_ROUND; preserve the standard Windows 11 corners.
    DwmSetWindowAttribute(window, 33, &corner, sizeof corner);
}

// Icons loaded with LR_SHARED/LoadIcon belong to the module, not the window or a GDI owner.
void window_icons(WNDCLASSEXW& window_class)
{
    window_class.hIcon = LoadIconW(window_class.hInstance, MAKEINTRESOURCEW(IDI_APP));
    window_class.hIconSm = static_cast<HICON>(LoadImageW(
        window_class.hInstance,
        MAKEINTRESOURCEW(IDI_APP),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON),
        LR_SHARED
    ));
    if (!window_class.hIcon || !window_class.hIconSm)
    {
        throw std::runtime_error("application icon resource could not be loaded");
    }
}

// EM_STREAMIN consumes this stack-owned source synchronously; no callback survives the call.
struct RtfStream
{
    std::string text;
    size_t offset = 0;

    static DWORD CALLBACK read(DWORD_PTR cookie, LPBYTE buffer, LONG capacity, LONG* copied)
    {
        auto& source = *reinterpret_cast<RtfStream*>(cookie);
        size_t count = std::min(size_t(std::max(capacity, 0L)), source.text.size() - source.offset);
        std::memcpy(buffer, source.text.data() + source.offset, count);
        source.offset += count;
        *copied = LONG(count);
        return 0;
    }
};

// Restore painting even if formatting/allocation throws; never leave a window redraw-disabled.
struct RedrawGuard
{
    HWND window;

    explicit RedrawGuard(HWND target)
        : window(target)
    {
        SendMessageW(window, WM_SETREDRAW, FALSE, 0);
    }

    ~RedrawGuard()
    {
        SendMessageW(window, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(window, nullptr, FALSE);
    }
};

struct Font
{
    HFONT value = nullptr;

    ~Font()
    {
        if (value)
        {
            DeleteObject(value);
        }
    }
};

struct Module
{
    HMODULE value = nullptr;

    ~Module()
    {
        if (value)
        {
            FreeLibrary(value);
        }
    }
};

struct App
{
    Core& core;
    LogWriter& log;
    Restart& restart;
    const std::string serial_id; // Device-session identity; unrelated to HTTP MCP-Session-Id.
    Config config;
    Palette palette;
    Display display;
    Module rich;
    Font normal;
    Font mono;
    HWND window = nullptr;
    HWND terminal = nullptr;
    HWND archive = nullptr;
    HWND archive_toggle = nullptr;
    HWND archive_reload = nullptr;
    HWND archive_match_case = nullptr;
    bool archive_open = false;
    std::optional<std::future<std::wstring>> archive_job;
    HWND include_text = nullptr;
    HWND exclude_text = nullptr;
    HWND filter_apply = nullptr;
    HWND search = nullptr;
    HWND find_next = nullptr;
    HWND find_previous = nullptr;
    HWND filter_enable = nullptr;
    std::wstring applied_include;
    std::wstring applied_exclude;
    bool filter_enabled = true; // GUI-thread state; toggling retains the applied predicate.
    HWND port = nullptr;
    HWND baud = nullptr;
    HWND status = nullptr;
    HWND log_status = nullptr;
    HWND mcp = nullptr;
    std::vector<std::pair<int, HWND>> buttons;
    std::vector<std::string> available;
    bool known = false;
    bool pending_send = false;
    std::optional<std::future<std::string>>
        connection_job; // UI-owned receipt; blocks duplicate clicks.
    long boundary = 0;
    uint64_t last_display_dropped = 0;
    std::vector<std::pair<std::future<std::string>, bool>> jobs;
    uint64_t scan = 0;
    std::string eol;

    App(Core& c, LogWriter& l, Restart& r, Config cfg, std::string id = {})
        : core(c),
          log(l),
          restart(r),
          serial_id(std::move(id)),
          config(std::move(cfg)),
          palette(config.theme),
          display(palette),
          eol(config.eol)
    {
    }

    ~App()
    {
        if (window && IsWindow(window))
        {
            DestroyWindow(window);
        }
    }

    HWND control(const wchar_t* kind, const wchar_t* caption, DWORD style, int id)
    {
        HWND h = CreateWindowExW(
            0,
            kind,
            caption,
            WS_CHILD | WS_VISIBLE | style,
            0,
            0,
            100,
            28,
            window,
            reinterpret_cast<HMENU>(INT_PTR(id)),
            GetModuleHandleW(nullptr),
            nullptr
        );
        if (!h)
        {
            throw std::runtime_error(win_error());
        }
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(normal.value), 0);
        return h;
    }

    void create()
    {
        rich.value = LoadLibraryW(L"Msftedit.dll");
        if (!rich.value)
        {
            throw std::runtime_error(win_error());
        }
        normal.value = CreateFontW(
            -16,
            0,
            0,
            0,
            400,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            0,
            0,
            CLEARTYPE_QUALITY,
            0,
            L"Segoe UI"
        );
        mono.value = CreateFontW(
            -17,
            0,
            0,
            0,
            400,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            0,
            0,
            CLEARTYPE_QUALITY,
            0,
            L"Cascadia Mono"
        );
        if (!normal.value || !mono.value)
        {
            throw std::runtime_error("font creation failed");
        }
        port = control(L"COMBOBOX", L"", CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP, PORT);
        baud = control(L"COMBOBOX", L"115200", CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP, BAUD);
        for (auto v :
            {L"9600", L"19200", L"38400", L"57600", L"115200", L"230400", L"460800", L"921600"})
        {
            SendMessageW(baud, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(v));
        }
        auto s = core.state();
        text(port, widen(s.port));
        text(baud, std::to_wstring(s.baud));
        for (auto [id, name] : std::vector<std::pair<int, const wchar_t*>>{{CONNECTION, L"Connect"},
                 {REFRESH, L"Refresh"},
                 {EOL, L"EOL: CRLF"},
                 {CLEAR, L"Clear"}})
        {
            buttons.emplace_back(id, control(L"BUTTON", name, BS_PUSHBUTTON | WS_TABSTOP, id));
        }
        filter_enable = control(L"BUTTON", L"Enable", BS_AUTOCHECKBOX | WS_TABSTOP, FILTER_ENABLE);
        SendMessageW(filter_enable, BM_SETCHECK, BST_CHECKED, 0);
        include_text = control(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 112);
        exclude_text = control(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 111);
        for (const auto& [edit, hint] :
            {std::pair{include_text, L"Include"}, std::pair{exclude_text, L"Exclude"}})
        {
            SendMessageW(edit, EM_SETLIMITTEXT, 256, 0);
            SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(hint));
        }
        filter_apply = control(L"BUTTON", L"Apply", BS_PUSHBUTTON | WS_TABSTOP, FILTER_APPLY);
        search = control(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 115);
        SendMessageW(search, EM_SETLIMITTEXT, 256, 0);
        SendMessageW(search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search log"));
        find_previous = control(L"BUTTON", L"Previous", BS_PUSHBUTTON | WS_TABSTOP, FIND_PREVIOUS);
        find_next = control(L"BUTTON", L"Next", BS_PUSHBUTTON | WS_TABSTOP, FIND_NEXT);
        status = control(L"STATIC", L"", SS_LEFTNOWORDWRAP, 0);
        log_status = control(L"STATIC", L"", SS_LEFTNOWORDWRAP, 0);
        mcp = control(L"STATIC", L"", SS_LEFTNOWORDWRAP, 0);
        terminal = control(
            MSFTEDIT_CLASS,
            L"",
            ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL | WS_VSCROLL |
                WS_HSCROLL | WS_TABSTOP,
            TERM
        );
        SendMessageW(terminal, WM_SETFONT, reinterpret_cast<WPARAM>(mono.value), 0);
        SendMessageW(terminal, EM_EXLIMITTEXT, 0, 16 * 1024 * 1024);
        SendMessageW(terminal, EM_SETBKGNDCOLOR, 0, palette.bg);
        SendMessageW(terminal, EM_SETTARGETDEVICE, 0, 1);
        color(terminal, palette.fg);
        if (!SetWindowSubclass(terminal, edit_proc, 1, reinterpret_cast<DWORD_PTR>(this)))
        {
            throw std::runtime_error("RichEdit subclass failed");
        }
        archive_toggle = control(L"BUTTON", L">", BS_PUSHBUTTON | WS_TABSTOP, LOG_TOGGLE);
        archive_reload = control(L"BUTTON", L"Reload log", BS_PUSHBUTTON | WS_TABSTOP, LOG_RELOAD);
        archive_match_case =
            control(L"BUTTON", L"Match case", BS_AUTOCHECKBOX | WS_TABSTOP, LOG_MATCH_CASE);
        SendMessageW(archive_match_case, BM_SETCHECK, BST_CHECKED, 0);
        archive = control(
            MSFTEDIT_CLASS,
            L"",
            ES_MULTILINE | ES_READONLY | ES_NOHIDESEL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL |
                WS_TABSTOP,
            0
        );
        SendMessageW(archive, WM_SETFONT, reinterpret_cast<WPARAM>(mono.value), 0);
        SendMessageW(archive, EM_EXLIMITTEXT, 0, LONG_MAX);
        SendMessageW(archive, EM_SETTARGETDEVICE, 0, 1);
        SendMessageW(archive, EM_SETBKGNDCOLOR, 0, palette.bg);
        CHARFORMATW archive_format{};
        archive_format.cbSize = sizeof archive_format;
        archive_format.dwMask = CFM_COLOR;
        archive_format.crTextColor = palette.fg;
        SendMessageW(
            archive,
            EM_SETCHARFORMAT,
            SCF_DEFAULT,
            reinterpret_cast<LPARAM>(&archive_format)
        );
        SetTimer(window, 1, 33, nullptr);
        layout();
        scan_ports();
        labels();
    }

    void layout()
    {
        if (!terminal)
        {
            return;
        }
        RECT r{};
        GetClientRect(window, &r);
        int w = r.right, h = r.bottom;
        MoveWindow(port, 12, 12, 180, 300, TRUE);
        MoveWindow(baud, 202, 12, 110, 300, TRUE);
        int x = 322;
        for (auto [id, b] : buttons)
        {
            int width = id == CONNECTION ? 104 : 92;
            MoveWindow(b, x, 10, width, 30, TRUE);
            x += width + 8;
        }
        MoveWindow(status, 12, 50, w - 24, 24, TRUE);
        MoveWindow(log_status, 12, 76, w - 24, 24, TRUE);
        const int top = 104;
        const int panel = archive_open ? w / 2 : w - 12;
        const int left = panel + 4;
        const int right = w - 12;
        MoveWindow(terminal, 12, top, std::max(panel - 46, 20), std::max(h - top - 42, 20), TRUE);
        MoveWindow(archive_toggle, panel - 30, top, 26, 26, TRUE);
        MoveWindow(archive_reload, left, top, 104, 26, TRUE);
        MoveWindow(filter_enable, left + 110, top, 78, 26, TRUE);
        MoveWindow(archive_match_case, left + 190, top, 110, 26, TRUE);
        const int filter_width = std::max((right - left - 88) / 2, 20);
        MoveWindow(include_text, left, top + 30, filter_width, 26, TRUE);
        MoveWindow(exclude_text, left + filter_width + 4, top + 30, filter_width, 26, TRUE);
        MoveWindow(filter_apply, right - 76, top + 30, 76, 26, TRUE);
        MoveWindow(search, left, top + 60, std::max(right - left - 150, 20), 26, TRUE);
        MoveWindow(find_previous, right - 144, top + 60, 76, 26, TRUE);
        MoveWindow(find_next, right - 64, top + 60, 64, 26, TRUE);
        MoveWindow(
            archive,
            left,
            top + 90,
            std::max(right - left, 20),
            std::max(h - top - 132, 20),
            TRUE
        );
        for (HWND child : {archive,
                 archive_reload,
                 archive_match_case,
                 filter_enable,
                 exclude_text,
                 include_text,
                 filter_apply,
                 search,
                 find_previous,
                 find_next})
        {
            ShowWindow(child, archive_open ? SW_SHOW : SW_HIDE);
        }
        MoveWindow(mcp, 12, h - 32, w - 24, 24, TRUE);
    }

    std::wstring draft()
    {
        auto s = rich_text(terminal);
        return s.substr(std::min<size_t>(size_t(boundary), s.size()));
    }

    void draft(const std::wstring& s)
    {
        select(terminal, boundary, -1);
        color(terminal, palette.fg);
        SendMessageW(terminal, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(s.c_str()));
        SendMessageW(terminal, EM_EMPTYUNDOBUFFER, 0, 0);
    }

    void scan_ports()
    {
        try
        {
            auto list = ports();
            known = true;
            if (list != available)
            {
                auto selected = get_text(port);
                SendMessageW(port, CB_RESETCONTENT, 0, 0);
                for (auto& p : list)
                {
                    SendMessageW(port, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(widen(p).c_str()));
                }
                text(port, selected.empty() && !list.empty() ? widen(list[0]) : selected);
                available = std::move(list);
            }
        }
        catch (const std::exception& e)
        {
            known = false;
            core.notice(e.what());
        }
    }

    void labels()
    {
        auto s = core.state();
        bool exists = std::any_of(
            available.begin(),
            available.end(),
            [&](auto& p)
            {
                return _stricmp(p.c_str(), s.port.c_str()) == 0;
            }
        );
        std::string state = s.connected        ? "CONNECTED"
                            : known && !exists ? "NOT_FOUND"
                                               : "DISCONNECTED";
        text(
            status,
            widen(s.port + " @ " + std::to_string(s.baud) + " | " + state + " " + s.notice)
        );
        const auto cursor = log.committed();
        const auto log_position =
            cursor.line ? cursor.filename + ":" + std::to_string(cursor.line) : "pending";
        text(
            log_status,
            widen(
                "log:" + log_position + " | rx:" + std::to_string(s.rx) +
                "B tx:" + std::to_string(s.tx) + "B dropped:" + std::to_string(s.dropped) +
                " | display skipped:" + std::to_string(s.display_dropped) +
                (log.error().empty() ? "" : " | log error: " + log.error())
            )
        );
        text(
            mcp,
            widen(
                std::string("Live | MCP") +
                (serial_id.empty() ? std::string{} : " | serial_id: " + serial_id) + " | " + s.mcp
            )
        );
        for (auto [id, b] : buttons)
        {
            if (id == CONNECTION)
            {
                text(b, s.connected ? L"Disconnect" : L"Connect");
                EnableWindow(b, !connection_job.has_value());
            }
            if (id == EOL)
            {
                auto label = eol;
                std::transform(
                    label.begin(),
                    label.end(),
                    label.begin(),
                    [](unsigned char c)
                    {
                        return char(std::toupper(c));
                    }
                );
                text(b, widen("EOL: " + label));
            }
        }
    }

    // Insert an entire colored batch in one RichEdit operation rather than one operation per run.
    // Device text is escaped; it cannot inject RTF. Unicode uses signed UTF-16 RTF code units.
    void insert_pieces(const std::vector<Piece>& pieces)
    {
        if (pieces.empty())
        {
            return;
        }
        std::vector<COLORREF> colors;
        std::map<COLORREF, size_t> indices;
        for (auto& piece : pieces)
        {
            if (!indices.count(piece.color))
            {
                colors.push_back(piece.color);
                indices[piece.color] = colors.size();
            }
        }
        LOGFONTW font{};
        GetObjectW(mono.value, sizeof font, &font);
        int half_points = std::max(
            1,
            MulDiv(std::abs(font.lfHeight), 144, std::max(int(GetDpiForWindow(terminal)), 96))
        );
        RtfStream source;
        source.text =
            "{\\rtf1\\ansi\\uc1\\deff0{\\fonttbl{\\f0\\fmodern Cascadia Mono;}}{\\colortbl;";
        for (auto value : colors)
        {
            source.text += "\\red" + std::to_string(GetRValue(value)) + "\\green" +
                           std::to_string(GetGValue(value)) + "\\blue" +
                           std::to_string(GetBValue(value)) + ";";
        }
        source.text += "}\\f0\\fs" + std::to_string(half_points) + " ";
        for (auto& piece : pieces)
        {
            source.text += "\\cf" + std::to_string(indices.at(piece.color)) + " ";
            for (wchar_t unit : piece.text)
            {
                if (unit == L'\r' || unit == L'\n')
                {
                    source.text += "\\par ";
                }
                else if (unit == L'\t')
                {
                    source.text += "\\tab ";
                }
                else if (unit == L'\\' || unit == L'{' || unit == L'}')
                {
                    source.text += '\\';
                    source.text += char(unit);
                }
                else if (unit >= 32 && unit < 127)
                {
                    source.text += char(unit);
                }
                else
                {
                    int value = int(unit);
                    if (value >= 32768)
                    {
                        value -= 65536;
                    }
                    source.text += "\\u" + std::to_string(value) + "?";
                }
            }
        }
        source.text += '}';
        EDITSTREAM stream{};
        stream.dwCookie = reinterpret_cast<DWORD_PTR>(&source);
        stream.pfnCallback = RtfStream::read;
        SendMessageW(
            terminal,
            EM_STREAMIN,
            SF_RTF | SFF_SELECTION,
            reinterpret_cast<LPARAM>(&stream)
        );
        if (stream.dwError)
        {
            throw std::runtime_error("RichEdit batch insertion failed");
        }
    }

    void apply(Patch patch)
    {
        if (patch.pieces.empty() && !patch.erase)
        {
            return;
        }
        auto old = boundary;
        auto chosen = selection(terminal);
        RedrawGuard redraw(terminal);
        long remove = std::min(old, patch.erase);
        select(terminal, old - remove, old);
        boundary -= remove;
        // A partial row may become hidden without producing any replacement text.
        if (patch.pieces.empty())
        {
            SendMessageW(terminal, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
        }
        else
        {
            insert_pieces(patch.pieces);
        }
        for (auto& piece : patch.pieces)
        {
            boundary += long(piece.text.size());
        }
        color(terminal, palette.fg);
        long delta = boundary - old;
        auto shift = [&](long n)
        {
            return n >= old ? n + delta : std::min(n, boundary);
        };
        select(terminal, shift(chosen.cpMin), shift(chosen.cpMax));
        // RichEdit has wrapping disabled. Its line indices include the active input row.
        const auto last_line = SendMessageW(terminal, EM_EXLINEFROMCHAR, 0, boundary);
        const auto excess = last_line + 1 - long(config.max_lines);
        long trim = 0;
        if (excess > 0)
        {
            trim = long(SendMessageW(terminal, EM_LINEINDEX, excess, 0));
        }
        const long budget = long(config.max_lines) * 1024;
        if (boundary - trim > budget)
        {
            const auto line = SendMessageW(terminal, EM_EXLINEFROMCHAR, 0, boundary - budget / 2);
            trim = std::max(trim, long(SendMessageW(terminal, EM_LINEINDEX, line, 0)));
        }
        if (trim > 0 && trim <= boundary)
        {
            auto saved = selection(terminal);
            select(terminal, 0, trim);
            SendMessageW(terminal, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
            boundary -= trim;
            select(terminal, std::max(saved.cpMin - trim, 0L), std::max(saved.cpMax - trim, 0L));
        }
        SendMessageW(terminal, EM_EMPTYUNDOBUFFER, 0, 0);
        SendMessageW(terminal, WM_VSCROLL, SB_BOTTOM, 0);
    }

    void clear()
    {
        auto s = draft();
        SetWindowTextW(terminal, L"");
        boundary = 0;
        display.reset();
        draft(s);
    }

    void send()
    {
        if (pending_send)
        {
            return;
        }
        try
        {
            auto s = draft();
            if (s.size() > 65536)
            {
                throw std::runtime_error("command exceeds 65536 UTF-16 units");
            }
            auto bytes = command_bytes(s, false, eol);
            if (bytes.empty())
            {
                return;
            }
            if (!core.state().connected)
            {
                throw std::runtime_error("not connected; choose a port and Connect");
            }
            pending_send = true;
            jobs.emplace_back(
                core.submit(
                    [c = &core, bytes]
                    {
                        return c->send(bytes);
                    }
                ),
                true
            );
        }
        catch (const std::exception& e)
        {
            core.notice(e.what());
        }
    }

    void set_archive_text(const wchar_t* contents)
    {
        SetWindowTextW(archive, contents);
        select(archive, 0, -1);
        color(archive, palette.fg);
        select(archive, 0, 0);
    }

    void load_archive()
    {
        if (!archive_open || archive_job)
        {
            return;
        }
        const auto cursor = log.committed();
        if (cursor.filename.empty())
        {
            set_archive_text(L"No log has been written yet.");
            return;
        }
        const auto path = log.directory() / widen(cursor.filename);
        const auto include = filter_enabled ? applied_include : std::wstring{};
        const auto exclude = filter_enabled ? applied_exclude : std::wstring{};
        const bool match_case = SendMessageW(archive_match_case, BM_GETCHECK, 0, 0) == BST_CHECKED;
        // Capture the committed row count before reading so a concurrent append cannot
        // introduce an incomplete row. The worker owns no window or session references.
        archive_job.emplace(
            std::async(
                std::launch::async,
                [path, cursor, include, exclude, match_case]
                {
                    std::ifstream file(path, std::ios::binary);
                    if (!file)
                    {
                        return std::wstring(L"Unable to read the log file.");
                    }
                    std::wstring output;
                    std::string row;
                    for (uint64_t i = 0; i < cursor.line && std::getline(file, row); ++i)
                    {
                        const auto line = widen(row);
                        const auto matches = [&](const std::wstring& pattern)
                        {
                            if (pattern.size() > line.size())
                            {
                                return false;
                            }
                            if (match_case)
                            {
                                return line.find(pattern) != std::wstring::npos;
                            }
                            return FindStringOrdinal(
                                       FIND_FROMSTART,
                                       line.data(),
                                       int(line.size()),
                                       pattern.data(),
                                       int(pattern.size()),
                                       TRUE
                                   ) >= 0;
                        };
                        if ((include.empty() || matches(include)) &&
                            (exclude.empty() || !matches(exclude)))
                        {
                            output += line;
                            output += L'\n';
                        }
                    }
                    if (file.bad())
                    {
                        return std::wstring(L"Unable to read the log file.");
                    }
                    return output;
                }
            )
        );
        EnableWindow(archive_reload, FALSE);
        EnableWindow(archive_match_case, FALSE);
        EnableWindow(filter_apply, FALSE);
        EnableWindow(filter_enable, FALSE);
    }

    void find_output(bool forward)
    {
        auto query = get_text(search);
        if (query.empty())
        {
            SetFocus(search);
            return;
        }
        const auto selected = selection(archive);
        const long boundary = long(GetWindowTextLengthW(archive));
        FINDTEXTEXW match{};
        match.lpstrText = query.data();
        if (forward)
        {
            match.chrg = {std::min(selected.cpMax, boundary), boundary};
        }
        else
        {
            match.chrg = {std::min(selected.cpMin, boundary), 0};
        }
        const auto result = SendMessageW(
            archive,
            EM_FINDTEXTEXW,
            (forward ? FR_DOWN : 0) |
                (SendMessageW(archive_match_case, BM_GETCHECK, 0, 0) == BST_CHECKED ? FR_MATCHCASE
                                                                                    : 0),
            reinterpret_cast<LPARAM>(&match)
        );
        if (result < 0)
        {
            MessageBoxW(window, L"No further match in this direction.", L"Search", MB_OK);
            return;
        }
        SetFocus(archive);
        select(archive, match.chrgText.cpMin, match.chrgText.cpMax);
        SendMessageW(archive, EM_SCROLLCARET, 0, 0);
    }

    void command(int id)
    {
        try
        {
            if (id == LOG_TOGGLE)
            {
                archive_open = !archive_open;
                SetWindowTextW(archive_toggle, archive_open ? L"<" : L">");
                layout();
                load_archive();
                return;
            }
            if (id == LOG_RELOAD || id == LOG_MATCH_CASE)
            {
                load_archive();
                return;
            }
            if (id == FIND_NEXT || id == FIND_PREVIOUS)
            {
                find_output(id == FIND_NEXT);
                return;
            }
            if (id == FILTER_ENABLE || id == FILTER_APPLY)
            {
                if (id == FILTER_APPLY)
                {
                    applied_include = get_text(include_text);
                    applied_exclude = get_text(exclude_text);
                }
                filter_enabled = SendMessageW(filter_enable, BM_GETCHECK, 0, 0) == BST_CHECKED;
                load_archive();
            }
            else if (id == CONNECTION)
            {
                if (connection_job)
                {
                    return;
                }
                // Choose the action from this session's actual state, including MCP changes.
                // The UI never blocks on device I/O and disables the button until completion.
                bool disconnect = core.state().connected;
                std::string name;
                DWORD speed = 0;
                if (!disconnect)
                {
                    name = utf8(get_text(port));
                    auto value = std::stoul(get_text(baud));
                    if (name.empty() || value == 0 || value > 4000000)
                    {
                        throw std::runtime_error("invalid port or baud");
                    }
                    speed = DWORD(value);
                }
                connection_job.emplace(core.submit(
                    [c = &core, disconnect, name, speed]
                    {
                        if (disconnect)
                        {
                            c->close();
                            return std::string{};
                        }
                        return c->open(name, speed);
                    }
                ));
            }
            else if (id == REFRESH)
            {
                scan_ports();
            }
            else if (id == EOL)
            {
                eol = eol == "crlf" ? "none" : eol == "none" ? "cr" : eol == "cr" ? "lf" : "crlf";
            }
            else if (id == CLEAR)
            {
                clear();
            }
        }
        catch (const std::exception& e)
        {
            core.notice(e.what());
        }
        labels();
        SetFocus(terminal);
    }

    void tick()
    {
        if (archive_job &&
            archive_job->wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            std::wstring contents;
            try
            {
                contents = archive_job->get();
            }
            catch (const std::exception&)
            {
                contents = L"Unable to load the log file.";
            }
            archive_job.reset();
            set_archive_text(contents.c_str());
            EnableWindow(archive_reload, TRUE);
            EnableWindow(archive_match_case, TRUE);
            EnableWindow(filter_apply, TRUE);
            EnableWindow(filter_enable, TRUE);
        }

        if (restart.ready())
        {
            PostMessageW(GetAncestor(window, GA_ROOT), WM_CLOSE, 0, 0);
            return;
        }
        if (connection_job &&
            connection_job->wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
        {
            auto error = connection_job->get();
            connection_job.reset();
            if (!error.empty())
            {
                core.notice(error);
            }
        }
        for (auto i = jobs.begin(); i != jobs.end();)
        {
            if (i->first.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
            {
                auto result = i->first.get();
                if (i->second)
                {
                    pending_send = false;
                    if (result.empty())
                    {
                        draft(L"");
                        core.notice("written to serial driver (device execution not confirmed)");
                    }
                }
                if (!result.empty())
                {
                    core.notice(result);
                }
                i = jobs.erase(i);
            }
            else
            {
                ++i;
            }
        }
        if (!IsWindowVisible(window) || IsIconic(GetAncestor(window, GA_ROOT)))
        {
            return;
        }
        if (GetTickCount64() - scan >= 1000)
        {
            scan = GetTickCount64();
            scan_ports();
        }
        labels();
        Patch batch;
        auto state = core.state();
        if (state.display_dropped != last_display_dropped)
        {
            batch.erase = display.reset();
            batch.pieces.push_back({L"\r[display backlog skipped; see file log]\r", palette.dim});
            last_display_dropped = state.display_dropped;
        }
        // A fixed byte budget bounds decoding and the RTF payload even for a single huge record.
        for (auto& record : core.drain(64, 32 * 1024))
        {
            if (record.dir != Direction::Status)
            {
                batch.append(display.feed(record, false));
            }
        }
        apply(std::move(batch));
    }

    static LRESULT CALLBACK
    edit_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR context)
    {
        auto& a = *reinterpret_cast<App*>(context);
        if (m == WM_NCDESTROY)
        {
            RemoveWindowSubclass(h, edit_proc, 1);
            return DefSubclassProc(h, m, w, l);
        }
        if (m != WM_KEYDOWN && m != WM_CHAR && m != WM_CONTEXTMENU && m != WM_PASTE &&
            m != WM_CUT && m != WM_CLEAR && m != WM_UNDO)
        {
            return DefSubclassProc(h, m, w, l);
        }
        auto r = selection(h);
        if (m == WM_CONTEXTMENU)
        {
            if (r.cpMin != r.cpMax)
            {
                SendMessageW(h, WM_COPY, 0, 0);
            }
            return 0;
        }
        if (m == WM_KEYDOWN)
        {
            if (w == VK_RETURN)
            {
                a.send();
                return 0;
            }
            if (w == VK_DELETE && (r.cpMin < a.boundary || a.pending_send))
            {
                return 0;
            }
        }
        if (m == WM_CHAR)
        {
            if (w == 13 || w == 127 ||
                (w < 32 && w != 1 && w != 3 && w != 8 && w != 9 && w != 22 && w != 24 && w != 26))
            {
                return 0;
            }
            if (a.pending_send && w != 3)
            {
                return 0;
            }
            if (w == 8 && (r.cpMin < a.boundary || (r.cpMin == a.boundary && r.cpMin == r.cpMax)))
            {
                return 0;
            }
            if (w >= 32 && r.cpMin < a.boundary)
            {
                select(h, -1, -1);
            }
        }
        if (m == WM_PASTE || m == WM_CUT || m == WM_CLEAR || m == WM_UNDO)
        {
            if (a.pending_send || m == WM_UNDO)
            {
                return 0;
            }
            if (r.cpMin < a.boundary)
            {
                if (m == WM_PASTE)
                {
                    select(h, -1, -1);
                }
                else
                {
                    return 0;
                }
            }
        }
        return DefSubclassProc(h, m, w, l);
    }
};

LRESULT CALLBACK window_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCCREATE)
    {
        auto* a = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        a->window = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(a));
        return TRUE;
    }
    if (m == WM_GETMINMAXINFO)
    {
        auto* p = reinterpret_cast<MINMAXINFO*>(l);
        p->ptMinTrackSize = {1040, 480};
        return 0;
    }
    if (m == WM_CLOSE)
    {
        DestroyWindow(h);
        return 0;
    }
    if (m == WM_DESTROY)
    {
        KillTimer(h, 1);
        if (!(GetWindowLongPtrW(h, GWL_STYLE) & WS_CHILD))
        {
            PostQuitMessage(0);
        }
        return 0;
    }
    auto* a = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!a)
    {
        return DefWindowProcW(h, m, w, l);
    }
    try
    {
        if (m == WM_TIMER)
        {
            a->tick();
            return 0;
        }
        if (m == WM_SIZE)
        {
            a->layout();
            return 0;
        }
        if (m == WM_COMMAND && HIWORD(w) == BN_CLICKED)
        {
            a->command(LOWORD(w));
            return 0;
        }
    }
    catch (const std::exception& e)
    {
        a->core.notice(e.what());
    }
    return DefWindowProcW(h, m, w, l);
}

void make_window(App& a, bool visible, HWND parent = nullptr)
{
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX cc{sizeof cc, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&cc);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"SerialMcpCppWindow";
    window_icons(wc);
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        throw std::runtime_error(win_error());
    }
    HWND h = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"Serial Monitor C++ — WinAPI",
        (parent ? WS_CHILD : WS_OVERLAPPEDWINDOW) | WS_CLIPCHILDREN,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1260,
        800,
        parent,
        nullptr,
        wc.hInstance,
        &a
    );
    if (!h)
    {
        throw std::runtime_error(win_error());
    }
    a.create();
    if (!parent)
    {
        window_frame(h);
    }
    if (visible)
    {
        ShowWindow(h, SW_SHOW);
        UpdateWindow(h);
        SetFocus(a.terminal);
    }
}
} // namespace

// The workspace owns tabs and child windows. Each Panel destroys App before releasing its session reference.
// Background pages retain UI state without rendering; bounded RX events and file logging continue independently.
namespace
{
struct Workspace
{
    struct Panel
    {
        std::shared_ptr<SerialSession> session;
        std::unique_ptr<App> app;
    };

    SessionManager& sessions;
    Restart& restart;
    Config config;
    HWND window = nullptr;
    HWND tabs = nullptr;
    // Owned until all child controls are destroyed by the Workspace destructor.
    Font tab_font;
    std::map<std::string, Panel> panels;
    std::vector<std::string> keys;
    std::string active;
    LRESULT hot_caption = HTNOWHERE;
    LRESULT pressed_caption = HTNOWHERE;

    Workspace(SessionManager& manager, Restart& restarter, Config settings)
        : sessions(manager),
          restart(restarter),
          config(std::move(settings))
    {
    }

    ~Workspace()
    {
        if (window && IsWindow(window))
        {
            DestroyWindow(window);
        }
    }

    int scaled(int pixels) const
    {
        return MulDiv(pixels, std::max(int(GetDpiForWindow(window)), 96), 96);
    }

    int header_height() const
    {
        return scaled(40);
    }

    // The client owns this compact row. Keep a zero glass margin so GDI child tabs stay visible.
    void caption_frame()
    {
        MARGINS margins{};
        DwmExtendFrameIntoClientArea(window, &margins);
    }

    LRESULT caption_hit_test(LPARAM position)
    {
        POINT point{SHORT(LOWORD(position)), SHORT(HIWORD(position))};
        ScreenToClient(window, &point);
        RECT area{};
        GetClientRect(window, &area);
        if (!IsZoomed(window))
        {
            int border = scaled(6);
            bool left = point.x < border;
            bool right = point.x >= area.right - border;
            bool top = point.y < border;
            bool bottom = point.y >= area.bottom - border;
            if (top && left)
            {
                return HTTOPLEFT;
            }
            if (top && right)
            {
                return HTTOPRIGHT;
            }
            if (bottom && left)
            {
                return HTBOTTOMLEFT;
            }
            if (bottom && right)
            {
                return HTBOTTOMRIGHT;
            }
            if (left)
            {
                return HTLEFT;
            }
            if (right)
            {
                return HTRIGHT;
            }
            if (top)
            {
                return HTTOP;
            }
            if (bottom)
            {
                return HTBOTTOM;
            }
        }
        if (point.y < header_height())
        {
            if (point.x >= area.right - scaled(46))
            {
                return HTCLOSE;
            }
            if (point.x >= area.right - scaled(92))
            {
                return HTMAXBUTTON;
            }
            if (point.x >= area.right - scaled(138))
            {
                return HTMINBUTTON;
            }
            if (point.x < scaled(36))
            {
                return HTSYSMENU;
            }
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    // Paint compact caption glyphs with stock GDI objects. Native HT* hit codes still
    // delegate dragging, system menus and caption-button commands to DefWindowProc.
    void paint_caption()
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT header{};
        GetClientRect(window, &header);
        header.bottom = header_height();
        SetDCBrushColor(dc, RGB(232, 236, 241));
        FillRect(dc, &header, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        HICON icon = reinterpret_cast<HICON>(GetClassLongPtrW(window, GCLP_HICONSM));
        int size = scaled(16);
        DrawIconEx(
            dc,
            scaled(10),
            (header_height() - size) / 2,
            icon,
            size,
            size,
            0,
            nullptr,
            DI_NORMAL
        );
        int saved = SaveDC(dc);
        int width = scaled(46);
        int center_y = header_height() / 2;
        for (int i = 0; i < 3; ++i)
        {
            LRESULT hit = i == 0 ? HTMINBUTTON : i == 1 ? HTMAXBUTTON : HTCLOSE;
            RECT button{header.right - (3 - i) * width,
                0,
                header.right - (2 - i) * width,
                header_height()};
            bool hot = hot_caption == hit;
            if (hot)
            {
                SelectObject(dc, GetStockObject(DC_BRUSH));
                SetDCBrushColor(dc, hit == HTCLOSE ? RGB(196, 43, 28) : RGB(230, 230, 230));
                FillRect(dc, &button, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            }
            SelectObject(dc, GetStockObject(DC_PEN));
            SetDCPenColor(dc, hot && hit == HTCLOSE ? RGB(255, 255, 255) : RGB(30, 30, 30));
            int center_x = (button.left + button.right) / 2;
            int radius = scaled(5);
            if (hit == HTMINBUTTON)
            {
                MoveToEx(dc, center_x - radius, center_y, nullptr);
                LineTo(dc, center_x + radius + 1, center_y);
            }
            else if (hit == HTMAXBUTTON)
            {
                SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
                if (IsZoomed(window))
                {
                    Rectangle(
                        dc,
                        center_x - radius + 2,
                        center_y - radius,
                        center_x + radius + 1,
                        center_y + radius - 1
                    );
                    Rectangle(
                        dc,
                        center_x - radius,
                        center_y - radius + 2,
                        center_x + radius - 1,
                        center_y + radius + 1
                    );
                }
                else
                {
                    Rectangle(
                        dc,
                        center_x - radius,
                        center_y - radius,
                        center_x + radius + 1,
                        center_y + radius + 1
                    );
                }
            }
            else
            {
                MoveToEx(dc, center_x - radius, center_y - radius, nullptr);
                LineTo(dc, center_x + radius + 1, center_y + radius + 1);
                MoveToEx(dc, center_x + radius, center_y - radius, nullptr);
                LineTo(dc, center_x - radius - 1, center_y + radius + 1);
            }
        }
        if (saved)
        {
            RestoreDC(dc, saved);
        }
        EndPaint(window, &paint);
    }

    void caption_hover(LRESULT hit)
    {
        if (hot_caption == hit)
        {
            return;
        }
        hot_caption = hit;
        RECT row{};
        GetClientRect(window, &row);
        row.bottom = header_height();
        InvalidateRect(window, &row, FALSE);
    }

    // Registry snapshots release the manager lock. Only the GUI thread creates, destroys and selects controls.
    void sync()
    {
        auto previous = active;
        auto snapshot = sessions.list();
        std::vector<std::string> next;
        for (auto& session : snapshot)
        {
            next.push_back(session->id);
            if (!panels.count(session->id))
            {
                auto app = std::make_unique<App>(
                    session->core,
                    session->log,
                    restart,
                    config,
                    session->id
                );
                make_window(*app, false, window);
                panels.emplace(session->id, Panel{session, std::move(app)});
            }
        }
        for (auto i = panels.begin(); i != panels.end();)
        {
            if (std::find(next.begin(), next.end(), i->first) == next.end())
            {
                i = panels.erase(i);
            }
            else
            {
                ++i;
            }
        }
        bool changed = next != keys;
        if (changed)
        {
            keys = std::move(next);
            TabCtrl_DeleteAllItems(tabs);
            for (size_t i = 0; i < keys.size(); ++i)
            {
                auto label = widen(keys[i]);
                TCITEMW item{};
                item.mask = TCIF_TEXT;
                item.pszText = label.data();
                TabCtrl_InsertItem(tabs, int(i), &item);
            }
            TCITEMW plus{};
            plus.mask = TCIF_TEXT;
            plus.pszText = const_cast<wchar_t*>(L"+");
            TabCtrl_InsertItem(tabs, int(keys.size()), &plus);
        }
        if (!panels.count(active) && !keys.empty())
        {
            active = keys.front();
        }
        for (size_t i = 0; i < keys.size(); ++i)
        {
            auto state = panels.at(keys[i]).session->core.state();
            auto label = widen(
                keys[i] + (state.port.empty() ? "" : " " + state.port) +
                (state.connected ? " [ON]    X" : " [OFF]    X")
            );
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = label.data();
            TabCtrl_SetItem(tabs, int(i), &item);
            if (keys[i] == active)
            {
                TabCtrl_SetCurSel(tabs, int(i));
            }
        }
        // Status updates do not relayout RichEdit, preserving scrolling/selections and avoiding periodic flicker.
        if (changed || previous != active)
        {
            layout();
        }
    }

    void layout()
    {
        if (!tabs)
        {
            return;
        }
        RECT area{};
        GetClientRect(window, &area);
        RECT caption_buttons{};
        int reserved = scaled(150);
        if (SUCCEEDED(DwmGetWindowAttribute(window, 5, &caption_buttons, sizeof caption_buttons)))
        {
            reserved = std::max(reserved, int(caption_buttons.right - caption_buttons.left));
        }
        // Leave a draggable gap before the system buttons even with many overflowing tabs.
        int left = scaled(36);
        int width = std::max(int(area.right) - left - reserved - scaled(48), scaled(100));
        MoveWindow(tabs, left, scaled(3), width, header_height() - scaled(3), TRUE);
        for (auto& [id, panel] : panels)
        {
            MoveWindow(
                panel.app->window,
                0,
                header_height(),
                area.right,
                std::max(area.bottom - header_height(), 0L),
                TRUE
            );
            ShowWindow(panel.app->window, id == active ? SW_SHOWNA : SW_HIDE);
        }
    }

    // The close icon and hit target share the same client-coordinate rectangle.
    // Scale the reserved icon width with the tab control DPI, not the terminal font.
    RECT close_rect(int index)
    {
        RECT rectangle{};
        if (!TabCtrl_GetItemRect(tabs, index, &rectangle))
        {
            throw std::runtime_error("cannot locate tab");
        }
        int width = MulDiv(26, int(GetDpiForWindow(tabs)), 96);
        rectangle.left = std::max(rectangle.left, rectangle.right - std::max(width, 26));
        return rectangle;
    }

    void new_tab()
    {
        active = sessions.create()->id;
        sync();
        SetFocus(panels.at(active).app->terminal);
    }

    void close_tab(size_t index)
    {
        if (index >= keys.size())
        {
            return;
        }
        // Copy the ID before sync rebuilds keys; never route a close through the active tab.
        auto id = keys[index];
        sessions.remove(id);
        sync();
        SetFocus(panels.at(active).app->terminal);
    }

    // Native tabs retain selection, keyboard navigation and overflow hit testing.
    // Paint the full strip ourselves to avoid classic borders and use rounded Edge-style tabs.
    // Use stock GDI objects and restore the borrowed DC; no per-frame GDI allocations.
    void draw_tab(const DRAWITEMSTRUCT& item)
    {
        size_t index = size_t(item.itemID);
        if (item.hwndItem != tabs || index > keys.size())
        {
            return;
        }
        bool plus = index == keys.size();
        bool enabled = plus ? keys.size() < SessionManager::limit : keys.size() > 1;
        std::wstring label = L"+";
        if (!plus)
        {
            auto state = panels.at(keys[index]).session->core.state();
            label = widen(
                keys[index] + (state.port.empty() ? "" : " " + state.port) +
                (state.connected ? " [ON]" : " [OFF]")
            );
        }
        int saved = SaveDC(item.hDC);
        if (!saved)
        {
            return;
        }
        bool selected = !plus && (item.itemState & ODS_SELECTED);
        RECT shape = item.rcItem;
        shape.left += scaled(2);
        shape.right -= scaled(2);
        shape.top += scaled(2);
        if (selected)
        {
            RECT strip{};
            GetClientRect(tabs, &strip);
            shape.bottom = strip.bottom;
        }
        SelectObject(item.hDC, GetStockObject(DC_BRUSH));
        SelectObject(item.hDC, GetStockObject(DC_PEN));
        COLORREF background = selected ? RGB(255, 255, 255) : RGB(232, 236, 241);
        SetDCBrushColor(item.hDC, background);
        SetDCPenColor(item.hDC, background);
        RoundRect(
            item.hDC,
            shape.left,
            shape.top,
            shape.right,
            shape.bottom,
            scaled(12),
            scaled(12)
        );
        if (selected)
        {
            // Square off the bottom half so the active tab joins the white content.
            RECT base{shape.left, (shape.top + shape.bottom) / 2, shape.right, shape.bottom};
            FillRect(item.hDC, &base, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        }
        else if (!plus)
        {
            SetDCPenColor(item.hDC, RGB(194, 200, 208));
            MoveToEx(item.hDC, shape.right, shape.top + scaled(9), nullptr);
            LineTo(item.hDC, shape.right, shape.bottom - scaled(9));
        }
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, GetSysColor(COLOR_BTNTEXT));
        SelectObject(item.hDC, tab_font.value);
        RECT caption = item.rcItem;
        if (plus)
        {
            SetTextColor(item.hDC, GetSysColor(enabled ? COLOR_BTNTEXT : COLOR_GRAYTEXT));
            DrawTextW(
                item.hDC,
                label.c_str(),
                -1,
                &caption,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE
            );
        }
        else
        {
            RECT close = close_rect(int(index));
            caption.left += 10;
            caption.right = close.left;
            DrawTextW(
                item.hDC,
                label.c_str(),
                -1,
                &caption,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX
            );
            int x = (close.left + close.right) / 2;
            int y = (close.top + close.bottom) / 2;
            int radius = std::max(3, MulDiv(3, int(GetDpiForWindow(tabs)), 96));
            SelectObject(item.hDC, GetStockObject(DC_PEN));
            SetDCPenColor(item.hDC, GetSysColor(enabled ? COLOR_BTNTEXT : COLOR_GRAYTEXT));
            MoveToEx(item.hDC, x - radius, y - radius, nullptr);
            LineTo(item.hDC, x + radius + 1, y + radius + 1);
            MoveToEx(item.hDC, x + radius, y - radius, nullptr);
            LineTo(item.hDC, x - radius - 1, y + radius + 1);
        }
        RestoreDC(item.hDC, saved);
    }

    static LRESULT CALLBACK
    tab_proc(HWND h, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR context)
    {
        auto& self = *reinterpret_cast<Workspace*>(context);
        if (message == WM_NCDESTROY)
        {
            RemoveWindowSubclass(h, tab_proc, 2);
            return DefSubclassProc(h, message, w, l);
        }
        if (message == WM_ERASEBKGND)
        {
            return 1;
        }
        if (message == WM_PAINT)
        {
            // BeginPaint clips to this child only. Stock objects are borrowed and
            // the DC is restored by draw_tab; no brushes or fonts accumulate.
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(h, &paint);
            RECT area{};
            GetClientRect(h, &area);
            SetDCBrushColor(dc, RGB(232, 236, 241));
            FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            for (int index = 0; index < TabCtrl_GetItemCount(h); ++index)
            {
                DRAWITEMSTRUCT item{};
                item.hwndItem = h;
                item.hDC = dc;
                item.itemID = UINT(index);
                item.itemState = index == TabCtrl_GetCurSel(h) ? ODS_SELECTED : 0;
                if (TabCtrl_GetItemRect(h, index, &item.rcItem))
                {
                    self.draw_tab(item);
                }
            }
            EndPaint(h, &paint);
            return 0;
        }
        if (message == WM_NCHITTEST)
        {
            POINT point{SHORT(LOWORD(l)), SHORT(HIWORD(l))};
            POINT parent_point = point;
            ScreenToClient(self.window, &parent_point);
            ScreenToClient(h, &point);
            TCHITTESTINFO hit{};
            hit.pt = point;
            if ((!IsZoomed(self.window) && parent_point.y < self.scaled(6)) ||
                TabCtrl_HitTest(h, &hit) < 0)
            {
                return HTTRANSPARENT;
            }
        }
        if (message == WM_LBUTTONDOWN)
        {
            TCHITTESTINFO hit{};
            hit.pt = {SHORT(LOWORD(l)), SHORT(HIWORD(l))};
            int index = TabCtrl_HitTest(h, &hit);
            try
            {
                if (index >= 0 && size_t(index) == self.keys.size())
                {
                    self.new_tab();
                    return 0;
                }
                if (index >= 0 && size_t(index) < self.keys.size())
                {
                    auto close = self.close_rect(index);
                    if (PtInRect(&close, hit.pt))
                    {
                        self.close_tab(size_t(index));
                        return 0;
                    }
                }
            }
            catch (const std::exception& error)
            {
                if (self.panels.count(self.active))
                {
                    self.panels.at(self.active).session->core.notice(error.what());
                }
                return 0;
            }
        }
        return DefSubclassProc(h, message, w, l);
    }

    void select_tab()
    {
        int index = TabCtrl_GetCurSel(tabs);
        if (index >= 0 && size_t(index) == keys.size())
        {
            if (keys.size() < SessionManager::limit)
            {
                new_tab();
            }
            else
            {
                // Arrow-key navigation must not leave the add item selected at the limit.
                for (size_t i = 0; i < keys.size(); ++i)
                {
                    if (keys[i] == active)
                    {
                        TabCtrl_SetCurSel(tabs, int(i));
                    }
                }
            }
            return;
        }
        if (index >= 0 && size_t(index) < keys.size())
        {
            active = keys[size_t(index)];
            layout();
            SetFocus(panels.at(active).app->terminal);
        }
    }

    // TaskDialog owns its controls and modal loop; serial/log workers continue.
    // Only the fixed project URL is opened, never arbitrary notification text.
    static HRESULT CALLBACK about_event(HWND dialog, UINT event, WPARAM, LPARAM, LONG_PTR)
    {
        if (event == TDN_HYPERLINK_CLICKED)
        {
            auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
                dialog,
                L"open",
                L"https://github.com/jsjhy/serial_MCP_CPP.git",
                nullptr,
                nullptr,
                SW_SHOWNORMAL
            ));
            if (result <= 32)
            {
                MessageBoxW(
                    dialog,
                    L"Unable to open the project link in the default browser.",
                    L"Open project link",
                    MB_OK | MB_ICONERROR
                );
            }
        }
        return S_OK;
    }

    void about()
    {
        TASKDIALOGCONFIG dialog{};
        dialog.cbSize = sizeof dialog;
        dialog.hwndParent = window;
        dialog.hInstance = GetModuleHandleW(nullptr);
        dialog.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_USE_HICON_MAIN | TDF_ALLOW_DIALOG_CANCELLATION;
        dialog.dwCommonButtons = TDCBF_OK_BUTTON;
        dialog.pszWindowTitle = L"About Serial Monitor C++";
        dialog.pszMainInstruction = L"Serial Monitor C++";
        // The class owns this shared icon; the dialog only borrows it.
        dialog.hMainIcon = reinterpret_cast<HICON>(GetClassLongPtrW(window, GCLP_HICON));
        dialog.pszContent = L"Version 1.0.0\n\n"
                            L"Native WinAPI multi-port serial monitor\n"
                            L"Up to 16 independent serial sessions\n"
                            L"MCP: Streamable HTTP (2025-03-26)\n\n"
                            L"<a href=\"https://github.com/jsjhy/serial_MCP_CPP.git\">"
                            L"https://github.com/jsjhy/serial_MCP_CPP.git</a>";
        dialog.pfCallback = about_event;
        if (FAILED(TaskDialogIndirect(&dialog, nullptr, nullptr, nullptr)))
        {
            throw std::runtime_error("cannot create About dialog");
        }
    }

    static LRESULT CALLBACK procedure(HWND h, UINT message, WPARAM w, LPARAM l)
    {
        if (message == WM_NCCREATE)
        {
            auto* self =
                static_cast<Workspace*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            self->window = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            return TRUE;
        }
        auto* self = reinterpret_cast<Workspace*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (!self)
        {
            return DefWindowProcW(h, message, w, l);
        }
        if (message == WM_NCMOUSEMOVE || message == WM_NCMOUSELEAVE)
        {
            LRESULT next = message == WM_NCMOUSELEAVE ? HTNOWHERE : LRESULT(w);
            self->caption_hover(next);
            if (message == WM_NCMOUSEMOVE)
            {
                TRACKMOUSEEVENT tracking{sizeof tracking, TME_LEAVE | TME_NONCLIENT, h, 0};
                TrackMouseEvent(&tracking);
            }
        }
        // This client-painted caption owns its mouse messages. DWM's hidden
        // native button geometry must not consume input for our custom row.
        try
        {
            switch (message)
            {
                case WM_NCLBUTTONDOWN:
                    if (w == HTMINBUTTON || w == HTMAXBUTTON || w == HTCLOSE)
                    {
                        self->pressed_caption = LRESULT(w);
                        self->caption_hover(LRESULT(w));
                        SetCapture(h);
                        return 0;
                    }
                    break;
                case WM_MOUSEMOVE:
                    if (self->pressed_caption != HTNOWHERE)
                    {
                        POINT cursor{};
                        GetCursorPos(&cursor);
                        self->caption_hover(self->caption_hit_test(MAKELPARAM(cursor.x, cursor.y)));
                        return 0;
                    }
                    break;
                case WM_LBUTTONUP:
                case WM_NCLBUTTONUP:
                    if (self->pressed_caption != HTNOWHERE)
                    {
                        POINT cursor{};
                        GetCursorPos(&cursor);
                        LRESULT hit = self->caption_hit_test(MAKELPARAM(cursor.x, cursor.y));
                        LRESULT pressed = self->pressed_caption;
                        self->pressed_caption = HTNOWHERE;
                        ReleaseCapture();
                        self->caption_hover(hit);
                        if (hit == pressed)
                        {
                            WPARAM command = pressed == HTCLOSE       ? SC_CLOSE
                                             : pressed == HTMINBUTTON ? SC_MINIMIZE
                                             : IsZoomed(h)            ? SC_RESTORE
                                                                      : SC_MAXIMIZE;
                            PostMessageW(h, WM_SYSCOMMAND, command, 0);
                        }
                        return 0;
                    }
                    break;
                case WM_CAPTURECHANGED:
                case WM_CANCELMODE:
                    self->pressed_caption = HTNOWHERE;
                    self->caption_hover(HTNOWHERE);
                    break;
                case WM_NCCALCSIZE:
                {
                    auto* area = w ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(l)->rgrc[0]
                                   : reinterpret_cast<RECT*>(l);
                    if (IsZoomed(h))
                    {
                        // Maximized WS_THICKFRAME windows extend their invisible resize border
                        // beyond the work area; keep the client row inside that work area.
                        UINT dpi = GetDpiForWindow(h);
                        int x = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) +
                                GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                        int y = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) +
                                GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                        InflateRect(area, -x, -y);
                    }
                    return 0;
                }
                case WM_NCHITTEST:
                    return self->caption_hit_test(l);
                case WM_ACTIVATE:
                case WM_DWMCOMPOSITIONCHANGED:
                    self->caption_frame();
                    break;
                case WM_PAINT:
                    self->paint_caption();
                    return 0;
                case WM_SYSCOMMAND:
                    if ((w & 0xFFF0) == IDM_ABOUT)
                    {
                        self->about();
                        return 0;
                    }
                    break;
                case WM_GETMINMAXINFO:
                    reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize = {1100, 540};
                    return 0;
                case WM_SIZE:
                {
                    self->layout();
                    RECT row{};
                    GetClientRect(h, &row);
                    row.bottom = self->header_height();
                    InvalidateRect(h, &row, FALSE);
                    return 0;
                }
                case WM_TIMER:
                    if (self->restart.ready())
                    {
                        PostMessageW(h, WM_CLOSE, 0, 0);
                    }
                    else
                    {
                        self->sync();
                    }
                    return 0;
                case WM_NOTIFY:
                    if (reinterpret_cast<NMHDR*>(l)->hwndFrom == self->tabs &&
                        reinterpret_cast<NMHDR*>(l)->code == TCN_SELCHANGE)
                    {
                        self->select_tab();
                    }
                    return 0;
                case WM_DRAWITEM:
                    self->draw_tab(*reinterpret_cast<DRAWITEMSTRUCT*>(l));
                    return TRUE;
                case WM_CLOSE:
                    DestroyWindow(h);
                    return 0;
                case WM_DESTROY:
                    KillTimer(h, 2);
                    PostQuitMessage(0);
                    return 0;
            }
        }
        catch (const std::exception& error)
        {
            if (self->panels.count(self->active))
            {
                self->panels.at(self->active).session->core.notice(error.what());
            }
        }
        return DefWindowProcW(h, message, w, l);
    }

    void create(bool visible)
    {
        SetProcessDPIAware();
        INITCOMMONCONTROLSEX controls{sizeof controls, ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = procedure;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"SerialMcpCppWorkspace";
        window_icons(wc);
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            throw std::runtime_error(win_error());
        }
        window = CreateWindowExW(
            0,
            wc.lpszClassName,
            L"Serial Monitor C++ — Multi-port",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            1260,
            840,
            nullptr,
            nullptr,
            wc.hInstance,
            this
        );
        if (!window)
        {
            throw std::runtime_error(win_error());
        }
        // The system menu is owned and destroyed by the native top-level window.
        HMENU menu = GetSystemMenu(window, FALSE);
        if (!menu ||
            !InsertMenuW(menu, SC_CLOSE, MF_BYCOMMAND | MF_STRING, IDM_ABOUT, L"&About...") ||
            !InsertMenuW(menu, SC_CLOSE, MF_BYCOMMAND | MF_SEPARATOR, 0, nullptr))
        {
            throw std::runtime_error("system menu initialization failed");
        }
        tabs = CreateWindowExW(
            0,
            WC_TABCONTROLW,
            L"",
            WS_CHILD | WS_VISIBLE | TCS_SINGLELINE | TCS_OWNERDRAWFIXED | TCS_FLATBUTTONS,
            0,
            0,
            100,
            30,
            window,
            nullptr,
            wc.hInstance,
            nullptr
        );
        if (!tabs || !SetWindowSubclass(tabs, tab_proc, 2, reinterpret_cast<DWORD_PTR>(this)))
        {
            throw std::runtime_error("tab control initialization failed");
        }
        tab_font.value = CreateFontW(
            -scaled(13),
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            0,
            0,
            CLEARTYPE_QUALITY,
            0,
            L"Segoe UI"
        );
        if (!tab_font.value)
        {
            throw std::runtime_error("tab font initialization failed");
        }
        SendMessageW(tabs, WM_SETFONT, reinterpret_cast<WPARAM>(tab_font.value), FALSE);
        TabCtrl_SetItemSize(tabs, scaled(200), scaled(32));
        window_frame(window);
        caption_frame();
        SetWindowPos(
            window,
            nullptr,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED
        );
        sync();
        SetTimer(window, 2, 250, nullptr);
        if (visible)
        {
            ShowWindow(window, SW_SHOW);
            UpdateWindow(window);
            select_tab();
        }
    }
};
} // namespace

int gui(SessionManager& sessions, Restart& restart, Config config)
{
    Workspace workspace(sessions, restart, std::move(config));
    workspace.create(true);
    MSG msg{};
    for (;;)
    {
        auto n = GetMessageW(&msg, nullptr, 0, 0);
        if (n == -1)
        {
            throw std::runtime_error(win_error());
        }
        if (!n)
        {
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

// Synthetic RX enters Core::publish and real log/UI paths, but never opens a serial device.
// Repeated samples distinguish queue warm-up/allocator retention from sustained resource growth.
int receive_stress_test()
{
    auto temporary = fs::weakly_canonical(fs::temp_directory_path());
    auto directory =
        temporary / (L"serial_receive_stress_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                        std::to_wstring(GetTickCount64()));
    // Cleanup may recurse only into a fresh, verified direct child of the temporary directory.
    if (fs::weakly_canonical(directory).parent_path() != temporary ||
        !fs::create_directory(directory))
    {
        throw std::runtime_error("could not create an isolated stress log directory");
    }
    Config config;
    config.log_dir = directory;
    config.keep_days = 0;
    config.rotate = 64 * 1024 * 1024;
    SessionManager manager(config);
    manager.create();
    manager.create();
    auto sessions = manager.list();
    Restart restart({executable().wstring()});
    unsigned duration = 18;
    wchar_t requested[32]{};
    if (GetEnvironmentVariableW(L"SERIAL_MCP_STRESS_SECONDS", requested, 32))
    {
        duration = unsigned(std::clamp(std::stoul(requested), 5UL, 300UL));
    }
    std::atomic<bool> stop = false;
    std::atomic<uint64_t> injected = 0;
    std::thread producer;
    double worst = 0;
    {
        Workspace workspace(manager, restart, config);
        workspace.create(true);
        std::string payload;
        while (payload.size() < 4096)
        {
            payload += "\x1b[32m[load] ";
            payload += std::string(72, 'x');
            payload += " 中文😀\x1b[0m\r\n";
        }
        payload.resize(4096);
        producer = std::thread(
            [&]
            {
                while (!stop)
                {
                    for (auto& session : sessions)
                    {
                        if (stop)
                        {
                            break;
                        }
                        session->core.publish(Direction::Rx, payload);
                        injected += payload.size();
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
        );
        auto start = Clock::now();
        auto next = start;
        try
        {
            while (Clock::now() - start < std::chrono::seconds(duration))
            {
                auto began = Clock::now();
                MSG message{};
                // Dispatch one message before rechecking deadlines; a busy queue cannot starve cleanup.
                if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                {
                    if (message.message == WM_QUIT)
                    {
                        break;
                    }
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                else
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                worst = std::max(
                    worst,
                    std::chrono::duration<double, std::milli>(Clock::now() - began).count()
                );
                if (Clock::now() >= next)
                {
                    PROCESS_MEMORY_COUNTERS_EX memory{};
                    memory.cb = sizeof memory;
                    K32GetProcessMemoryInfo(
                        GetCurrentProcess(),
                        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                        sizeof memory
                    );
                    unsigned threads = 0;
                    {
                        Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
                        THREADENTRY32 entry{};
                        entry.dwSize = sizeof entry;
                        if (snapshot && Thread32First(snapshot.value, &entry))
                        {
                            do
                            {
                                if (entry.th32OwnerProcessID == GetCurrentProcessId())
                                {
                                    ++threads;
                                }
                            } while (Thread32Next(snapshot.value, &entry));
                        }
                    }
                    DWORD handles = 0;
                    GetProcessHandleCount(GetCurrentProcess(), &handles);
                    std::printf(
                        "RX_STRESS seconds=%.1f injected=%llu private=%llu working=%llu handles=%lu gdi=%lu user=%lu worst_message_ms=%.2f threads=%u mode=%s\n",
                        std::chrono::duration<double>(Clock::now() - start).count(),
                        static_cast<unsigned long long>(injected.load()),
                        static_cast<unsigned long long>(memory.PrivateUsage),
                        static_cast<unsigned long long>(memory.WorkingSetSize),
                        handles,
                        GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS),
                        GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS),
                        worst,
                        threads,
                        "text"
                    );
                    std::fflush(stdout);
                    next = Clock::now() + std::chrono::seconds(1);
                }
            }
        }
        catch (...)
        {
            stop = true;
            manager.release_producers();
            producer.join();
            manager.stop();
            throw;
        }
        stop = true;
        manager.release_producers();
        producer.join();
        manager.stop();
        for (auto& session : sessions)
        {
            if (!session->log.error().empty())
            {
                throw std::runtime_error(session->log.error());
            }
        }
    }
    MSG message{};
    while (PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE))
    {
    }
    fs::remove_all(directory);
    std::printf(
        "RX_STRESS complete injected=%llu worst_message_ms=%.2f; synthetic data, not real serial or a long-run leak proof\n",
        static_cast<unsigned long long>(injected.load()),
        worst
    );
    return 0;
}

void native_tests(Core& core, LogWriter& log, Restart& restart)
{
    auto require = [](bool ok, const char* s)
    {
        if (!ok)
        {
            throw std::runtime_error(s);
        }
    };
    // Hidden workspaces test tab isolation and remote-removal synchronization without clipboard or real serial access.
    {
        Config config;
        SessionManager manager(config);
        auto first = manager.get();
        auto second = manager.create();
        {
            Workspace workspace(manager, restart, config);
            workspace.create(false);
            RECT outer{}, client{}, tab_bounds{};
            GetWindowRect(workspace.window, &outer);
            GetClientRect(workspace.window, &client);
            GetWindowRect(workspace.tabs, &tab_bounds);
            POINT tab_origin{tab_bounds.left, tab_bounds.top};
            ScreenToClient(workspace.window, &tab_origin);
            require(client.bottom == outer.bottom - outer.top, "caption uses the client row");
            require(tab_origin.y < workspace.header_height(), "tabs share the caption row");
            POINT drag{outer.right - workspace.scaled(175), outer.top + workspace.scaled(20)};
            require(
                workspace.caption_hit_test(MAKELPARAM(drag.x, drag.y)) == HTCAPTION,
                "compact caption preserves window dragging"
            );
            auto& a = *workspace.panels.at(first->id).app;
            auto& b = *workspace.panels.at(second->id).app;
            a.draft(L"draft A中文");
            b.draft(L"draft B😀");
            a.apply(a.display.feed({Direction::Rx, "only A", stamp()}, false));
            workspace.active = second->id;
            workspace.sync();
            require(
                a.draft() == L"draft A中文" && b.draft() == L"draft B😀",
                "tab draft isolation"
            );
            require(
                rich_text(b.terminal).find(L"only A") == std::wstring::npos,
                "tab output isolation"
            );
            // Exercise real mouse hit targets, including closing an inactive tab.
            auto click = [&](const RECT& rectangle)
            {
                int x = (rectangle.left + rectangle.right) / 2;
                int y = (rectangle.top + rectangle.bottom) / 2;
                SendMessageW(workspace.tabs, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
                SendMessageW(workspace.tabs, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
            };
            RECT plus{};
            TabCtrl_GetItemRect(workspace.tabs, int(workspace.keys.size()), &plus);
            click(plus);
            require(workspace.panels.size() == 3, "plus opens a new tab");
            auto third = workspace.active;
            require(third != first->id && third != second->id, "plus selects the new tab");
            auto first_index = std::find(workspace.keys.begin(), workspace.keys.end(), first->id);
            click(workspace.close_rect(int(first_index - workspace.keys.begin())));
            require(
                workspace.panels.size() == 2 && workspace.active == third,
                "inactive tab X closes its own session without switching the active tab"
            );
            auto third_index = std::find(workspace.keys.begin(), workspace.keys.end(), third);
            click(workspace.close_rect(int(third_index - workspace.keys.begin())));
            require(
                workspace.panels.size() == 1 && workspace.active == second->id,
                "removed tab switches to surviving session"
            );
            require(b.draft() == L"draft B😀", "removing another tab preserves draft");
        }
        MSG message{};
        while (PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE))
        {
        }
    }
    auto cycle = [&](bool check)
    {
        App a(core, log, restart, Config{});
        make_window(a, false);
        a.draft(L"help中文😀");
        a.apply(a.display.feed({Direction::Rx, "prompt中文😀", stamp()}, false));
        if (check)
        {
            a.set_archive_text(L"Log preview");
            select(a.archive, 0, -1);
            CHARFORMATW archive_format{};
            archive_format.cbSize = sizeof archive_format;
            SendMessageW(
                a.archive,
                EM_GETCHARFORMAT,
                SCF_SELECTION,
                reinterpret_cast<LPARAM>(&archive_format)
            );
            require(
                rich_text(a.archive) == L"Log preview" &&
                    (archive_format.dwMask & CFM_COLOR) != 0 &&
                    (archive_format.dwEffects & CFE_AUTOCOLOR) == 0 &&
                    archive_format.crTextColor == a.palette.fg,
                "log preview uses the theme foreground after replacing its contents"
            );
            require(a.draft() == L"help中文😀", "native draft preservation");
            auto all = rich_text(a.terminal);
            select(a.terminal, a.boundary, a.boundary);
            SendMessageW(a.terminal, WM_CHAR, 8, 0);
            require(rich_text(a.terminal) == all, "native prompt protection");
            select(a.terminal, 0, 1);
            SendMessageW(a.terminal, WM_CLEAR, 0, 0);
            require(rich_text(a.terminal) == all, "native history protection");
            CHARFORMATW format{};
            format.cbSize = sizeof format;
            SendMessageW(
                a.terminal,
                EM_GETCHARFORMAT,
                SCF_SELECTION,
                reinterpret_cast<LPARAM>(&format)
            );
            require(format.crTextColor == a.palette.dim, "native dim timestamp");
            a.clear();
            require(a.boundary == 0 && a.draft() == L"help中文😀", "clear preserves draft");
            a.command(EOL);
            require(a.eol == "none", "line ending button");
            a.clear();
            Patch batch;
            batch.append(a.display.feed({Direction::Rx, "\\rtf1{literal}中", stamp()}, false));
            batch.append(a.display.feed({Direction::Rx, "😀\nnext", stamp()}, false));
            a.apply(std::move(batch));
            require(a.draft() == L"help中文😀", "RTF batch preserves the input draft");
            require(
                rich_text(a.terminal).find(L"\\rtf1{literal}中😀") != std::wstring::npos,
                "RTF batch escapes device syntax and preserves UTF-16"
            );
            a.clear();
            a.display.visible = [](const std::wstring& row)
            {
                return row.find(L"hide") == std::wstring::npos;
            };
            a.apply(a.display.feed({Direction::Rx, "partial ", stamp()}, false));
            a.apply(a.display.feed({Direction::Rx, "hide\n", stamp()}, false));
            require(
                a.boundary == 0 && rich_text(a.terminal) == a.draft() && a.draft() == L"help中文😀",
                "erase-only filter patch removes the stale last row"
            );
        }
        // App destroys the window; remove the test thread's WM_QUIT before the next cycle.
    };
    auto cleanup = []()
    {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE))
        {
        }
    };
    for (int i = 0; i < 5; ++i)
    {
        cycle(i == 0);
        cleanup();
    }
    HANDLE process = GetCurrentProcess();
    DWORD handles_before = 0, handles_after = 0;
    GetProcessHandleCount(process, &handles_before);
    DWORD gdi = GetGuiResources(process, GR_GDIOBJECTS),
          user = GetGuiResources(process, GR_USEROBJECTS);
    for (int i = 0; i < 100; ++i)
    {
        cycle(false);
        cleanup();
    }
    GetProcessHandleCount(process, &handles_after);
    DWORD gdi_after = GetGuiResources(process, GR_GDIOBJECTS),
          user_after = GetGuiResources(process, GR_USEROBJECTS);
    std::printf(
        "100 native windows: GDI %lu -> %lu, USER %lu -> %lu, handles %lu -> %lu\n",
        gdi,
        gdi_after,
        user,
        user_after,
        handles_before,
        handles_after
    );
    require(
        gdi_after <= gdi + 2 && user_after <= user + 2 && handles_after <= handles_before + 4,
        "native resource growth"
    );
}
} // namespace sm
