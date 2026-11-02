// WinAPI GUI: the UI thread owns windows, fonts and UTF-16 selections. Serial actions use Core's task queue.
// RX patches precede the draft. Selecting history pauses display updates, not file logging. No custom shortcuts.
#include "ui.hpp"
#include "resource.h"
#include <cassert>

namespace sm
{
namespace
{
constexpr int PORT = 101;
constexpr int BAUD = 102;
constexpr int CONNECTION = 103;
constexpr int REFRESH = 105;
constexpr int MODE = 106;
constexpr int EOL = 107;
constexpr int CLEAR = 108;
constexpr int TERM = 109;

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
    std::vector<std::pair<std::future<std::string>, bool>> jobs;
    uint64_t scan = 0;
    bool hex;
    std::string eol;

    App(Core& c, LogWriter& l, Restart& r, Config cfg, std::string id = {})
        : core(c),
          log(l),
          restart(r),
          serial_id(std::move(id)),
          config(std::move(cfg)),
          palette(config.theme),
          display(palette),
          hex(config.hex),
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
                 {MODE, L"ASCII"},
                 {EOL, L"EOL: CRLF"},
                 {CLEAR, L"Clear"}})
        {
            buttons.emplace_back(id, control(L"BUTTON", name, BS_PUSHBUTTON | WS_TABSTOP, id));
        }
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
        MoveWindow(terminal, 12, 106, w - 24, std::max(h - 148, 20), TRUE);
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
        text(
            log_status,
            widen(
                "log:" + log.filename() + " | input/view:" + (hex ? "hex" : "ascii") +
                " | rx:" + std::to_string(s.rx) + "B tx:" + std::to_string(s.tx) +
                "B dropped:" + std::to_string(s.dropped) +
                (log.error().empty() ? "" : " | log error: " + log.error())
            )
        );
        text(
            mcp,
            widen(
                "MCP" + (serial_id.empty() ? std::string{} : " | serial_id: " + serial_id) + " | " +
                s.mcp
            )
        );
        for (auto [id, b] : buttons)
        {
            if (id == CONNECTION)
            {
                text(b, s.connected ? L"Disconnect" : L"Connect");
                EnableWindow(b, !connection_job.has_value());
            }
            if (id == MODE)
            {
                text(b, hex ? L"HEX" : L"ASCII");
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

    void apply(Patch patch)
    {
        if (patch.pieces.empty() && !patch.erase)
        {
            return;
        }
        auto old = boundary;
        auto chosen = selection(terminal);
        POINT scroll{};
        SendMessageW(terminal, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
        bool follow = chosen.cpMin >= old;
        SendMessageW(terminal, WM_SETREDRAW, FALSE, 0);
        long remove = std::min(old, patch.erase);
        select(terminal, old - remove, old);
        boundary -= remove;
        for (auto& p : patch.pieces)
        {
            color(terminal, p.color);
            SendMessageW(terminal, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(p.text.c_str()));
            boundary += long(p.text.size());
        }
        color(terminal, palette.fg);
        long delta = boundary - old;
        auto shift = [&](long n)
        {
            return n >= old ? n + delta : std::min(n, boundary);
        };
        select(terminal, shift(chosen.cpMin), shift(chosen.cpMax));
        long budget = long(config.max_lines) * 1024;
        if (boundary > budget)
        {
            auto all = rich_text(terminal);
            auto at = all.find(L'\r', size_t(boundary - budget / 2));
            if (at != std::wstring::npos && at < size_t(boundary))
            {
                auto saved = selection(terminal);
                select(terminal, 0, long(at + 1));
                SendMessageW(terminal, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
                boundary -= long(at + 1);
                select(
                    terminal,
                    std::max(saved.cpMin - long(at + 1), 0L),
                    std::max(saved.cpMax - long(at + 1), 0L)
                );
            }
        }
        SendMessageW(terminal, EM_EMPTYUNDOBUFFER, 0, 0);
        if (follow)
        {
            SendMessageW(terminal, EM_SCROLLCARET, 0, 0);
        }
        else
        {
            SendMessageW(terminal, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
        }
        SendMessageW(terminal, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(terminal, nullptr, FALSE);
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
            auto bytes = command_bytes(s, hex, eol);
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

    void command(int id)
    {
        try
        {
            if (id == CONNECTION)
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
            else if (id == MODE)
            {
                hex = !hex;
                display.reset();
                apply({0, {{L"\r", palette.fg}}});
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
        if (GetTickCount64() - scan >= 1000)
        {
            scan = GetTickCount64();
            scan_ports();
        }
        labels();
        auto chosen = selection(terminal);
        if (chosen.cpMin != chosen.cpMax && chosen.cpMin < boundary)
        {
            return;
        }
        for (auto& r : core.drain())
        {
            if (r.dir != Direction::Status)
            {
                apply(display.feed(r, hex));
            }
        }
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
// Background pages consume their own events and retain partial lines, drafts, selections and scroll positions.
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
    std::map<std::string, Panel> panels;
    std::vector<std::string> keys;
    std::string active;

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
        MoveWindow(tabs, 8, 8, std::max(area.right - 16L, 100L), 30, TRUE);
        for (auto& [id, panel] : panels)
        {
            MoveWindow(panel.app->window, 0, 44, area.right, std::max(area.bottom - 44L, 0L), TRUE);
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

    // Native tabs provide borders/scrolling; owner drawing reserves a distinct X area.
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
        FillRect(
            item.hDC,
            &item.rcItem,
            GetSysColorBrush((item.itemState & ODS_SELECTED) ? COLOR_WINDOW : COLOR_BTNFACE)
        );
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, GetSysColor(COLOR_BTNTEXT));
        SelectObject(item.hDC, GetStockObject(DEFAULT_GUI_FONT));
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

    // This modal dialog is owned by the workspace; Windows frees its controls and menu resources.
    // The modal message loop keeps existing UI timers and serial/log workers running.
    void about()
    {
        MSGBOXPARAMSW parameters{};
        parameters.cbSize = sizeof parameters;
        parameters.hwndOwner = window;
        parameters.hInstance = GetModuleHandleW(nullptr);
        parameters.lpszCaption = L"About Serial Monitor C++";
        parameters.lpszText = L"Serial Monitor C++\n"
                              L"Version 1.0.0\n\n"
                              L"Native WinAPI multi-port serial monitor\n"
                              L"Up to 16 independent serial sessions\n"
                              L"MCP: Streamable HTTP (2025-03-26)\n\n"
                              L"https://github.com/jsjhy/serial_MCP_CPP.git";
        parameters.dwStyle = MB_OK | MB_USERICON;
        parameters.lpszIcon = MAKEINTRESOURCEW(IDI_APP);
        if (!MessageBoxIndirectW(&parameters))
        {
            throw std::runtime_error(win_error());
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
        try
        {
            switch (message)
            {
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
                    self->layout();
                    return 0;
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
            WS_CHILD | WS_VISIBLE | TCS_SINGLELINE | TCS_OWNERDRAWFIXED,
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
        SendMessageW(
            tabs,
            WM_SETFONT,
            reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),
            0
        );
        sync();
        SetTimer(window, 2, 250, nullptr);
        window_frame(window);
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
            a.command(MODE);
            require(a.hex && !b.hex, "tab input/view mode isolation");
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
            a.command(MODE);
            require(a.hex, "mode button");
            a.command(EOL);
            require(a.eol == "none", "line ending button");
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
