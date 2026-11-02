// HTTP bounds body/header sizes, concurrency and timeouts. stop releases sockets, sessions and worker threads.
// DELETE ends a logical session; closing TCP does not. Successful notifications return an empty 202 response.
#include "mcp.hpp"

namespace sm
{
namespace
{
struct Socket
{
    SOCKET value = INVALID_SOCKET;

    explicit Socket(SOCKET s)
        : value(s)
    {
    }

    ~Socket()
    {
        if (value != INVALID_SOCKET)
        {
            closesocket(value);
        }
    }
};

void launch(const std::vector<std::wstring>& args, DWORD flags)
{
    std::wstring command = quote_arg(executable().wstring());
    for (auto& a : args)
    {
        command += L" " + quote_arg(a);
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(
            executable().c_str(),
            command.data(),
            nullptr,
            nullptr,
            FALSE,
            flags,
            nullptr,
            nullptr,
            &si,
            &pi
        ))
    {
        throw std::runtime_error(win_error());
    }
    Handle process(pi.hProcess), thread(pi.hThread);
}

std::string session_id()
{
    unsigned char bytes[24];
    if (BCryptGenRandom(nullptr, bytes, sizeof bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
    {
        throw std::runtime_error("secure random unavailable");
    }
    std::string s;
    static const char* d = "0123456789abcdef";
    for (auto b : bytes)
    {
        s += d[b >> 4];
        s += d[b & 15];
    }
    return s;
}

std::string lower(std::string s)
{
    for (auto& c : s)
    {
        c = char(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

void transmit(SOCKET s, std::string bytes)
{
    size_t at = 0;
    while (at < bytes.size())
    {
        int n = ::send(s, bytes.data() + at, int(std::min<size_t>(bytes.size() - at, 65536)), 0);
        if (n <= 0)
        {
            return;
        }
        at += size_t(n);
    }
}

void response(SOCKET s, int status, const std::string& body = "", const std::string& session = "")
{
    std::string reason = status == 200   ? "OK"
                         : status == 202 ? "Accepted"
                         : status == 400 ? "Bad Request"
                         : status == 403 ? "Forbidden"
                         : status == 404 ? "Not Found"
                         : status == 405 ? "Method Not Allowed"
                         : status == 406 ? "Not Acceptable"
                         : status == 413 ? "Content Too Large"
                         : status == 415 ? "Unsupported Media Type"
                         : status == 503 ? "Service Unavailable"
                                         : "Error";
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason +
                       "\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) +
                       "\r\n";
    if (!body.empty())
    {
        head += "Content-Type: application/json; charset=utf-8\r\n";
    }
    if (!session.empty())
    {
        head += "Mcp-Session-Id: " + session + "\r\n";
    }
    if (status == 405)
    {
        head += "Allow: POST, DELETE\r\n";
    }
    transmit(s, head + "\r\n" + body);
}

Json error(const Json& id, int code, const std::string& text)
{
    return Json::object(
        {{"jsonrpc", "2.0"},
            {"id", id},
            {"error", Json::object({{"code", code}, {"message", text}})}}
    );
}

Json property(const char* type)
{
    return Json::object({{"type", type}});
}
} // namespace

std::string Restart::request()
{
    std::lock_guard lock(mutex_);
    if (deadline_)
    {
        return "restart already scheduled";
    }
    try
    {
        std::vector<std::wstring> args{L"--internal-restart-helper",
            std::to_wstring(GetCurrentProcessId())};
        args.insert(args.end(), args_.begin() + 1, args_.end());
        launch(args, CREATE_NO_WINDOW);
        deadline_ = GetTickCount64() + 1000;
        return "restart scheduled; reconnect MCP after restart";
    }
    catch (const std::exception& e)
    {
        return "error: " + std::string(e.what());
    }
}

bool Restart::ready()
{
    std::lock_guard lock(mutex_);
    return deadline_ && GetTickCount64() >= deadline_;
}

int Restart::helper(const std::vector<std::wstring>& a)
{
    if (a.size() < 3)
    {
        return 1;
    }
    try
    {
        size_t n = 0;
        unsigned long long pid = std::stoull(a[2], &n);
        if (n != a[2].size() || pid == 0 || pid > MAXDWORD)
        {
            return 1;
        }
        Handle process(OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid)));
        if (!process || WaitForSingleObject(process.value, 30000) != WAIT_OBJECT_0)
        {
            return 1;
        }
        std::vector<std::wstring> args(a.begin() + 3, a.end());
        launch(args, 0);
        return 0;
    }
    catch (...)
    {
        return 1;
    }
}

void Mcp::start()
{
    WSADATA w{};
    if (WSAStartup(MAKEWORD(2, 2), &w))
    {
        throw std::runtime_error("WSAStartup failed");
    }
    winsock_ = true;
    listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_ == INVALID_SOCKET)
    {
        throw std::runtime_error("socket creation failed");
    }
    BOOL exclusive = TRUE;
    setsockopt(
        listen_,
        SOL_SOCKET,
        SO_EXCLUSIVEADDRUSE,
        reinterpret_cast<char*>(&exclusive),
        sizeof exclusive
    );
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(u_short(port_));
    if (bind(listen_, reinterpret_cast<sockaddr*>(&address), sizeof address) ||
        ::listen(listen_, 16))
    {
        throw std::runtime_error("MCP listen failed: " + std::to_string(WSAGetLastError()));
    }
    publish();
    for (unsigned i = 0; i < 4; ++i)
    {
        workers_.emplace_back(
            [this]
            {
                worker();
            }
        );
    }
    accept_ = std::thread(
        [this]
        {
            accept_loop();
        }
    );
}

void Mcp::stop()
{
    if (stopping_.exchange(true))
    {
        return;
    }
    if (listen_ != INVALID_SOCKET)
    {
        shutdown(listen_, SD_BOTH);
    }
    {
        std::lock_guard lock(mutex_);
        for (auto s : active_)
        {
            shutdown(s, SD_BOTH);
        }
        for (auto s : queued_)
        {
            closesocket(s);
        }
        queued_.clear();
    }
    cv_.notify_all();
    if (accept_.joinable())
    {
        accept_.join();
    }
    if (listen_ != INVALID_SOCKET)
    {
        closesocket(listen_);
        listen_ = INVALID_SOCKET;
    }
    for (auto& t : workers_)
    {
        if (t.joinable())
        {
            t.join();
        }
    }
    {
        std::lock_guard lock(mutex_);
        sessions_.clear();
    }
    if (winsock_)
    {
        WSACleanup();
        winsock_ = false;
    }
    serials_.mcp("stopped");
}

void Mcp::expire()
{
    std::lock_guard lock(mutex_);
    auto now = Clock::now();
    for (auto i = sessions_.begin(); i != sessions_.end();)
    {
        if (now - i->second.seen > std::chrono::minutes(5))
        {
            i = sessions_.erase(i);
        }
        else
        {
            ++i;
        }
    }
}

void Mcp::publish()
{
    std::lock_guard lock(mutex_);
    size_t n = 0;
    std::string names;
    for (auto& [_, s] : sessions_)
    {
        if (s.initialized)
        {
            ++n;
            if (!names.empty())
            {
                names += ", ";
            }
            names += s.name;
        }
    }
    serials_.mcp(
        (n ? "CONNECTED | sessions:" + std::to_string(n) + " | " + names
           : "LISTENING | waiting for client") +
        " | http://127.0.0.1:" + std::to_string(port_) + "/mcp"
    );
}

void Mcp::accept_loop()
{
    while (!stopping_)
    {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(listen_, &set);
        timeval wait{0, 200000};
        int ready = select(0, &set, nullptr, nullptr, &wait);
        if (ready > 0 && !stopping_)
        {
            SOCKET s = accept(listen_, nullptr, nullptr);
            if (s != INVALID_SOCKET)
            {
                DWORD timeout = 2000;
                setsockopt(
                    s,
                    SOL_SOCKET,
                    SO_RCVTIMEO,
                    reinterpret_cast<char*>(&timeout),
                    sizeof timeout
                );
                setsockopt(
                    s,
                    SOL_SOCKET,
                    SO_SNDTIMEO,
                    reinterpret_cast<char*>(&timeout),
                    sizeof timeout
                );
                std::lock_guard lock(mutex_);
                if (queued_.size() < 64 && !stopping_)
                {
                    queued_.push_back(s);
                    cv_.notify_one();
                }
                else
                {
                    closesocket(s);
                }
            }
        }
        expire();
        publish();
    }
}

void Mcp::worker()
{
    for (;;)
    {
        SOCKET s;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(
                lock,
                [&]
                {
                    return stopping_ || !queued_.empty();
                }
            );
            if (stopping_)
            {
                return;
            }
            s = queued_.front();
            queued_.pop_front();
            active_.insert(s);
        }
        {
            Socket owner(s);
            try
            {
                client(s);
            }
            catch (...)
            {
                response(s, 400);
            }
        }
        {
            std::lock_guard lock(mutex_);
            active_.erase(s);
        }
    }
}

void Mcp::client(SOCKET s)
{
    std::string bytes;
    char buf[4096];
    auto deadline = Clock::now() + std::chrono::seconds(5);
    size_t end;
    auto receive = [&]()
    {
        if (Clock::now() > deadline)
        {
            return false;
        }
        int n = recv(s, buf, sizeof buf, 0);
        if (n <= 0)
        {
            return false;
        }
        bytes.append(buf, size_t(n));
        return true;
    };
    while ((end = bytes.find("\r\n\r\n")) == std::string::npos)
    {
        if (bytes.size() > 16384)
        {
            response(s, 413);
            return;
        }
        if (!receive())
        {
            return;
        }
    }
    if (end > 16384)
    {
        response(s, 413);
        return;
    }
    std::istringstream head(bytes.substr(0, end));
    std::string method, path, version;
    head >> method >> path >> version;
    std::string line;
    std::getline(head, line);
    std::map<std::string, std::string> headers;
    while (std::getline(head, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        auto colon = line.find(':');
        if (colon == std::string::npos)
        {
            response(s, 400);
            return;
        }
        auto key = lower(trim(line.substr(0, colon)));
        if (headers.count(key))
        {
            response(s, 400);
            return;
        }
        headers[key] = trim(line.substr(colon + 1));
    }
    if (path != "/mcp")
    {
        response(s, 404);
        return;
    }
    if (headers.count("origin"))
    {
        auto origin = headers["origin"];
        if (origin != "http://127.0.0.1:" + std::to_string(port_) &&
            origin != "http://localhost:" + std::to_string(port_))
        {
            response(s, 403);
            return;
        }
    }
    if (method == "GET")
    {
        response(s, 405);
        return;
    }
    std::string sid = headers["mcp-session-id"];
    if (method == "DELETE")
    {
        bool removed;
        {
            std::lock_guard lock(mutex_);
            removed = sessions_.erase(sid) != 0;
        }
        publish();
        response(s, removed ? 202 : 404);
        return;
    }
    if (method != "POST")
    {
        response(s, 405);
        return;
    }
    if (headers["content-type"].find("application/json") == std::string::npos)
    {
        response(s, 415);
        return;
    }
    if (headers["accept"].find("application/json") == std::string::npos)
    {
        response(s, 406);
        return;
    }
    if (headers.count("transfer-encoding") || !headers.count("content-length"))
    {
        response(s, 400);
        return;
    }
    size_t count = 0;
    try
    {
        size_t used;
        count = std::stoull(headers["content-length"], &used);
        if (used != headers["content-length"].size())
        {
            throw std::runtime_error("length");
        }
    }
    catch (...)
    {
        response(s, 400);
        return;
    }
    if (count > 1024 * 1024)
    {
        response(s, 413);
        return;
    }
    while (bytes.size() < end + 4 + count)
    {
        if (!receive())
        {
            return;
        }
    }
    Json request;
    try
    {
        request = Json::parse(bytes.substr(end + 4, count));
    }
    catch (...)
    {
        response(s, 400, error({}, -32700, "Parse error").dump());
        return;
    }
    if (request.is_object() && request.at("method").text() == "initialize")
    {
        if (!request.has("id") || request.at("jsonrpc").text() != "2.0")
        {
            response(s, 400);
            return;
        }
        {
            std::lock_guard lock(mutex_);
            if (sessions_.size() >= 128)
            {
                response(s, 503);
                return;
            }
            sid = session_id();
            auto name = request.at("params").at("clientInfo").at("name").text("unknown");
            name.erase(
                std::remove_if(
                    name.begin(),
                    name.end(),
                    [](unsigned char c)
                    {
                        return c < 32;
                    }
                ),
                name.end()
            );
            if (name.size() > 80)
            {
                name = "client";
            }
            sessions_[sid] = {name, false, Clock::now()};
        }
        Json info = Json::object(
            {{"protocolVersion", "2025-03-26"},
                {"capabilities", Json::object({{"tools", Json::object({})}})},
                {"serverInfo", Json::object({{"name", "serial-mcp-cpp"}, {"version", "1.0.0"}})},
                {"instructions",
                    "Use list_sessions and serial_id when multiple serial sessions exist. restart disconnects all clients."}}
        );
        response(
            s,
            200,
            Json::object({{"jsonrpc", "2.0"}, {"id", request.at("id")}, {"result", info}}).dump(),
            sid
        );
        return;
    }
    {
        std::lock_guard lock(mutex_);
        auto i = sessions_.find(sid);
        if (i == sessions_.end())
        {
            response(s, sid.empty() ? 400 : 404);
            return;
        }
        i->second.seen = Clock::now();
    }
    if (request.is_array())
    {
        auto& a = std::get<Json::Array>(request.value);
        if (a.empty() || a.size() > 32)
        {
            response(s, 400);
            return;
        }
        Json::Array results;
        size_t budget = 0;
        // Bound aggregate batch responses so a 1 MiB request cannot expand into unbounded history snapshots.
        for (auto& item : a)
        {
            auto r =
                budget > 4 * 1024 * 1024
                    ? error(item.at("id"), -32000, "Batch response limit; use smaller requests")
                    : dispatch(item, sid);
            if (item.has("id"))
            {
                budget += r.dump().size();
                results.push_back(std::move(r));
            }
        }
        response(s, results.empty() ? 202 : 200, results.empty() ? "" : Json(results).dump());
        return;
    }
    auto result = dispatch(request, sid);
    publish();
    response(s, request.has("id") ? 200 : 202, request.has("id") ? result.dump() : "");
}

Json Mcp::dispatch(const Json& req, const std::string& sid)
{
    auto id = req.at("id");
    if (!req.is_object() || req.at("jsonrpc").text() != "2.0")
    {
        return error(id, -32600, "Invalid Request");
    }
    auto method = req.at("method").text();
    if (method == "notifications/initialized")
    {
        std::lock_guard lock(mutex_);
        sessions_[sid].initialized = true;
        return {};
    }
    if (method == "notifications/cancelled" || (!req.has("id") && req.has("result")))
    {
        return {};
    }
    Json result;
    try
    {
        if (method == "ping")
        {
            result = Json::object({});
        }
        else if (method == "tools/list")
        {
            result = Json::object({{"tools", tools()}});
        }
        else if (method == "tools/call")
        {
            auto& p = req.at("params");
            result = call(p.at("name").text(), p.at("arguments"));
        }
        else
        {
            return error(id, -32601, "Method not found");
        }
    }
    catch (const std::exception& e)
    {
        return error(id, -32602, e.what());
    }
    return Json::object({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
}

Json Mcp::tools()
{
    auto tool =
        [](const char* name, const char* desc, Json::Object props, Json::Array required = {})
    {
        return Json::object(
            {{"name", name},
                {"description", desc},
                {"inputSchema",
                    Json::object(
                        {{"type", "object"},
                            {"properties", Json(props)},
                            {"required", Json(required)},
                            {"additionalProperties", false}}
                    )}}
        );
    };
    Json::Array result{tool("list_ports", "List available serial ports.", {}),
        tool(
            "list_sessions",
            "List serial session IDs, connection state and separate log paths.",
            {}
        ),
        tool(
            "create_session",
            "Create an empty serial session (maximum 16); use open afterwards.",
            {}
        ),
        tool(
            "remove_session",
            "Stop and remove one serial session; keep at least one.",
            {{"serial_id", property("string")}},
            {"serial_id"}
        ),
        tool(
            "open",
            "Open a serial session. Specify serial_id when more than one exists.",
            {{"port", property("string")}, {"baud", property("integer")}},
            {"port", "baud"}
        ),
        tool("close", "Disconnect one serial session without removing it.", {}),
        tool(
            "send",
            "Write ASCII escapes or hex bytes; no implicit newline. Driver success does not confirm device execution.",
            {{"ascii", property("string")}, {"hex", property("string")}}
        ),
        tool(
            "read",
            "Read a non-destructive snapshot of one serial session's recent RX bytes.",
            {{"limit", property("integer")}, {"ascii", property("boolean")}}
        ),
        tool(
            "set_dtr_rts",
            "Set one serial session's DTR and RTS output levels.",
            {{"dtr", property("boolean")}, {"rts", property("boolean")}},
            {"dtr", "rts"}
        ),
        tool(
            "export_log",
            "Return one serial session's in-memory record summaries.",
            {{"limit", property("integer")}}
        ),
        tool(
            "restart",
            "Restart the whole application; runtime-added sessions are not restored.",
            {}
        )};
    // serial_id routes device operations; HTTP MCP-Session-Id identifies only the client protocol session.
    for (auto& item : result)
    {
        auto name = item.at("name").text();
        if (name == "open" || name == "close" || name == "send" || name == "read" ||
            name == "set_dtr_rts" || name == "export_log")
        {
            auto& fields = std::get<Json::Object>(item.value);
            auto& schema = std::get<Json::Object>(fields["inputSchema"].value);
            auto& properties = std::get<Json::Object>(schema["properties"].value);
            properties["serial_id"] = property("string");
        }
    }
    return Json(std::move(result));
}

Json Mcp::call(const std::string& name, const Json& a)
{
    if (!a.is_object())
    {
        throw std::runtime_error("arguments must be an object");
    }
    std::string out;
    bool failed = false;
    try
    {
        if (a.has("serial_id") &&
            (!a.at("serial_id").is_string() || a.at("serial_id").text().empty()))
        {
            throw std::runtime_error("serial_id must be a nonempty string");
        }
        // Retain the session throughout the request; removal stops resources without leaving a dangling Core.
        std::shared_ptr<SerialSession> serial;
        if (name == "open" || name == "close" || name == "send" || name == "read" ||
            name == "set_dtr_rts" || name == "export_log")
        {
            serial = serials_.get(a.at("serial_id").text());
        }
        auto describe = [](const std::shared_ptr<SerialSession>& session)
        {
            auto state = session->core.state();
            return Json::object(
                {{"serial_id", session->id},
                    {"port", state.port},
                    {"baud", int64_t(state.baud)},
                    {"connected", state.connected},
                    {"rx_bytes", int64_t(state.rx)},
                    {"tx_bytes", int64_t(state.tx)},
                    {"notice", state.notice},
                    {"log_dir", utf8(session->config.log_dir.wstring())},
                    {"log_file", session->log.filename()},
                    {"log_error", session->log.error()}}
            );
        };
        if (name == "list_sessions")
        {
            Json::Array list;
            for (auto& session : serials_.list())
            {
                list.push_back(describe(session));
            }
            out = Json::object({{"sessions", Json(std::move(list))}}).dump();
        }
        else if (name == "create_session")
        {
            out = describe(serials_.create()).dump();
        }
        else if (name == "remove_session")
        {
            auto id = a.at("serial_id").text();
            if (id.empty())
            {
                throw std::runtime_error("serial_id is required");
            }
            serials_.remove(id);
            out = "removed " + id;
        }
        else if (name == "list_ports")
        {
            auto list = ports();
            out = list.empty() ? "no serial ports found" : "available: ";
            for (size_t i = 0; i < list.size(); ++i)
            {
                if (i)
                {
                    out += ", ";
                }
                out += list[i];
            }
        }
        else if (name == "open")
        {
            auto port = a.at("port").text();
            auto baud = a.at("baud").integer(-1);
            if (port.empty() || baud < 0 || baud > 4000000)
            {
                throw std::runtime_error("invalid port or baud");
            }
            out = serial->core.open(port, baud ? DWORD(baud) : 115200);
            failed = !out.empty();
            if (!failed)
            {
                out = "opened";
            }
        }
        else if (name == "close")
        {
            serial->core.close();
            out = "closed";
        }
        else if (name == "send")
        {
            if (a.has("ascii") && !a.at("ascii").is_string())
            {
                throw std::runtime_error("ascii must be a string");
            }
            if (a.has("hex") && !a.at("hex").is_string() &&
                !std::holds_alternative<std::nullptr_t>(a.at("hex").value))
            {
                throw std::runtime_error("hex must be a string or null");
            }
            auto h = trim(a.at("hex").text());
            auto bytes = h.empty() ? decode_escapes(a.at("ascii").text()) : parse_hex(h);
            auto job = serial->core.submit(
                [serial, bytes]
                {
                    return serial->core.send(bytes);
                }
            );
            out = job.get();
            failed = !out.empty();
            if (!failed)
            {
                out = "written " + std::to_string(bytes.size()) +
                      " bytes to serial port (device execution not confirmed)";
            }
        }
        else if (name == "set_dtr_rts")
        {
            if (!std::holds_alternative<bool>(a.at("dtr").value) ||
                !std::holds_alternative<bool>(a.at("rts").value))
            {
                throw std::runtime_error("dtr and rts must be boolean");
            }
            out = serial->core.signals(a.at("dtr").boolean(), a.at("rts").boolean());
            failed = !out.empty();
            if (!failed)
            {
                out = "ok";
            }
        }
        else if (name == "read" || name == "export_log")
        {
            size_t limit = size_t(std::clamp<int64_t>(a.at("limit").integer(20), 1, 4096));
            auto records = serial->core.history(limit);
            for (auto& r : records)
            {
                if (name == "read" && r.dir != Direction::Rx)
                {
                    continue;
                }
                out += "[" + r.time + "] ";
                if (name == "export_log")
                {
                    out += (r.dir == Direction::Rx      ? "RX"
                               : r.dir == Direction::Tx ? "TX"
                                                        : "STATUS") +
                           std::string(": ") + std::to_string(r.data.size()) + " bytes\n";
                }
                else if (a.at("ascii").boolean())
                {
                    for (unsigned char c : r.data)
                    {
                        out += c >= 32 && c < 127 ? char(c) : '.';
                    }
                    out += '\n';
                }
                else
                {
                    out += hex_text(r.data) + "\n";
                }
                if (out.size() > 2 * 1024 * 1024)
                {
                    out += "[output truncated; use a smaller limit]\n";
                    break;
                }
            }
            if (out.empty())
            {
                out = "no data";
            }
        }
        else if (name == "restart")
        {
            out = restart_.request();
            failed = out.starts_with("error:");
        }
        else
        {
            throw std::runtime_error("unknown tool");
        }
    }
    catch (const std::exception& e)
    {
        out = e.what();
        failed = true;
    }
    if (failed && !out.starts_with("error:"))
    {
        out = "error: " + out;
    }
    return Json::object(
        {{"content", Json::array({Json::object({{"type", "text"}, {"text", out}})})},
            {"isError", failed}}
    );
}
} // namespace sm
