// Windows OVERLAPPED I/O owns one event per operation. On timeout, cancel and wait before releasing OVERLAPPED.
// operation_ serializes port changes. Blocking I/O holds no state lock. A full log queue backpressures producers.
#include "core.hpp"

namespace sm
{
LogWriter::LogWriter(Config c, size_t limit)
    : config_(std::move(c)),
      queue_limit_(std::max<size_t>(limit, 1024))
{
    worker_ = std::thread(
        [this]
        {
            loop();
        }
    );
}

// Bound both bytes and record count: empty payloads still consume metadata memory.
void LogWriter::push(Record r)
{
    std::unique_lock lock(mutex_);
    space_.wait(
        lock,
        [&]
        {
            return stopping_ || !accepting_ || (bytes_ < queue_limit_ && queue_.size() < 4096);
        }
    );
    if (stopping_ || !accepting_)
    {
        return;
    }
    bytes_ += r.data.size();
    queue_.push_back(std::move(r));
    cv_.notify_one();
}

void LogWriter::release_producers()
{
    {
        std::lock_guard lock(mutex_);
        accepting_ = false;
    }
    space_.notify_all();
}

void LogWriter::stop()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    space_.notify_all();
    if (worker_.joinable())
    {
        worker_.join();
    }
}

std::string LogWriter::filename()
{
    std::lock_guard lock(mutex_);
    return filename_.empty() ? "pending" : filename_;
}

LogCursor LogWriter::committed()
{
    std::lock_guard lock(mutex_);
    return committed_;
}

std::string LogWriter::error()
{
    std::lock_guard lock(mutex_);
    return error_;
}

void LogWriter::loop()
{
    std::ofstream file;
    uint64_t written = 0, sequence = 0;
    std::string pending;
    uint64_t rows = 0;
    bool line = false;
    size_t complete = 0; // Byte count through the last complete UTF-8 log row.
    std::optional<Direction> direction;
    TextParser parser(RGB(255, 255, 255));
    std::string time;
    auto last = Clock::now();
    auto emit = [&](const std::wstring& s, COLORREF)
    {
        if (s == L"\b")
        {
            return;
        }
        if (!line)
        {
            pending += "[" + time + "] ";
            if (direction == Direction::Tx)
            {
                pending += "TX: ";
            }
            if (direction == Direction::Status)
            {
                pending += "STATUS: ";
            }
            line = true;
        }
        if (s == L"\r")
        {
            pending += '\n';
            complete = pending.size();
            line = false;
        }
        else
        {
            pending += utf8(s);
        }
    };
    auto flush = [&]
    {
        if (!complete)
        {
            return;
        }
        try
        {
            fs::create_directories(config_.log_dir);
            if (!file.is_open() || written >= config_.rotate)
            {
                if (file.is_open())
                {
                    file.flush();
                    file.close();
                }
                std::string name =
                    "serial_" + stamp(true) + "_" + std::to_string(++sequence) + ".log";
                std::replace(name.begin(), name.end(), ':', '-');
                std::replace(name.begin(), name.end(), ' ', '_');
                while (fs::exists(config_.log_dir / widen(name)))
                {
                    name =
                        name.substr(0, name.rfind('_') + 1) + std::to_string(++sequence) + ".log";
                }
                rows = 0;
                file.clear();
                file.open(config_.log_dir / widen(name), std::ios::binary | std::ios::app);
                if (!file)
                {
                    throw std::runtime_error("cannot create log file");
                }
                written = uint64_t(file.tellp());
                {
                    std::lock_guard lock(mutex_);
                    filename_ = name;
                }
                if (config_.keep_days)
                {
                    std::error_code ec;
                    for (auto& e : fs::directory_iterator(config_.log_dir, ec))
                    {
                        auto n = e.path().filename().string();
                        if (!n.starts_with("serial_") || e.path().extension() != L".log")
                        {
                            continue;
                        }
                        auto age = fs::file_time_type::clock::now() - e.last_write_time(ec);
                        if (!ec && age > std::chrono::hours(24ull * config_.keep_days))
                        {
                            fs::remove(e.path(), ec);
                        }
                    }
                }
            }
            file.write(pending.data(), std::streamsize(complete));
            file.flush();
            if (!file)
            {
                throw std::runtime_error("log write/flush failed");
            }
            written += complete;
            rows += std::count(pending.begin(), pending.begin() + complete, '\n');
            pending.erase(0, complete);
            complete = 0;
            {
                std::lock_guard lock(mutex_);
                committed_ = {filename_, rows};
                error_.clear();
            }
        }
        catch (const std::exception& e)
        {
            std::lock_guard lock(mutex_);
            error_ = e.what();
            if (file.is_open())
            {
                file.close();
            }
        }
    };
    for (;;)
    {
        std::deque<Record> batch;
        bool end;
        {
            std::unique_lock lock(mutex_);
            cv_.wait_for(
                lock,
                std::chrono::milliseconds(config_.flush_ms),
                [&]
                {
                    return stopping_ || !queue_.empty();
                }
            );
            end = stopping_;
            // Bound formatted data on disk failure; retain pending output and retry so backpressure reaches producers.
            if (complete < queue_limit_)
            {
                size_t batch_bytes = 0;
                for (unsigned i = 0; i < 64 && !queue_.empty(); ++i)
                {
                    if (!batch.empty() && batch_bytes + queue_.front().data.size() > 256 * 1024)
                    {
                        break;
                    }
                    batch_bytes += queue_.front().data.size();
                    bytes_ -= queue_.front().data.size();
                    batch.push_back(std::move(queue_.front()));
                    queue_.pop_front();
                }
                space_.notify_all();
            }
            end = end && queue_.empty();
        }
        for (auto& r : batch)
        {
            if (direction != r.dir)
            {
                parser.finish(emit);
                if (line)
                {
                    emit(L"\r", RGB(255, 255, 255));
                }
                parser = TextParser(RGB(255, 255, 255));
                direction = r.dir;
            }
            time = r.time;
            parser.feed(r.data, emit);
            if (r.dir == Direction::Status)
            {
                parser.finish(emit);
                if (line)
                {
                    emit(L"\r", RGB(255, 255, 255));
                }
            }
        }
        if (complete >= config_.flush_bytes ||
            Clock::now() - last >= std::chrono::milliseconds(config_.flush_ms) || end)
        {
            flush();
            last = Clock::now();
        }
        if (end)
        {
            parser.finish(emit);
            if (line)
            {
                emit(L"\r", RGB(255, 255, 255));
            }
            flush();
            break;
        }
        // Do not wait forever on persistent shutdown disk errors. Report failure without claiming the tail is durable.
        {
            std::lock_guard lock(mutex_);
            if (stopping_ && !error_.empty() && complete >= queue_limit_)
            {
                break;
            }
        }
        if (complete >= queue_limit_)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.flush_ms));
        }
    }
}

Core::Core(LogWriter& l, DWORD baud)
    : log_(l)
{
    state_.baud = baud;
    try
    {
        reader_ = std::thread(
            [this]
            {
                reader();
            }
        );
        writer_ = std::thread(
            [this]
            {
                worker();
            }
        );
    }
    catch (...)
    {
        stop();
        throw;
    }
}

CoreState Core::state()
{
    std::lock_guard lock(mutex_);
    return state_;
}

void Core::notice(std::string s)
{
    std::lock_guard lock(mutex_);
    state_.notice = std::move(s);
}

void Core::mcp(std::string s)
{
    std::lock_guard lock(mutex_);
    state_.mcp = std::move(s);
}

void Core::publish(Direction dir, std::string bytes)
{
    Record r{dir, std::move(bytes), stamp(true)};
    log_.push(r);
    std::lock_guard lock(mutex_);
    if (dir == Direction::Rx)
    {
        state_.rx += r.data.size();
    }
    if (dir == Direction::Tx)
    {
        state_.tx += r.data.size();
    }
    history_bytes_ += r.data.size();
    history_.push_back(r);
    while (history_.size() > 4096 || history_bytes_ > 4 * 1024 * 1024)
    {
        history_bytes_ -= history_.front().data.size();
        history_.pop_front();
        ++state_.dropped;
    }
    event_bytes_ += r.data.size();
    events_.push_back(std::move(r));
    while (events_.size() > 1024 || event_bytes_ > 4 * 1024 * 1024)
    {
        event_bytes_ -= events_.front().data.size();
        events_.pop_front();
        ++state_.display_dropped;
    }
}

std::vector<Record> Core::drain(size_t count, size_t byte_limit)
{
    std::lock_guard lock(mutex_);
    std::vector<Record> out;
    while (count-- && byte_limit && !events_.empty())
    {
        auto& front = events_.front();
        size_t take = std::min(byte_limit, front.data.size());
        event_bytes_ -= take;
        byte_limit -= take;
        if (take == front.data.size())
        {
            out.push_back(std::move(front));
            events_.pop_front();
        }
        else
        {
            out.push_back({front.dir, front.data.substr(0, take), front.time});
            front.data.erase(0, take);
        }
    }
    return out;
}

std::vector<Record> Core::history(size_t n)
{
    std::lock_guard lock(mutex_);
    n = std::min(n, history_.size());
    return {history_.end() - std::ptrdiff_t(n), history_.end()};
}

std::shared_ptr<Core::Connection> Core::create_connection(const std::string& name, DWORD baud)
{
    std::wstring path = widen(name);
    if (!path.starts_with(L"\\\\.\\"))
    {
        path = L"\\\\.\\" + path;
    }
    Handle h(CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        nullptr
    ));
    if (!h && (GetLastError() == 2 || GetLastError() == 3 || GetLastError() == 161))
    {
        h.reset(CreateFileW(
            widen(name).c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr
        ));
    }
    if (!h)
    {
        throw std::runtime_error(win_error());
    }
    DCB d{};
    d.DCBlength = sizeof d;
    if (!GetCommState(h.value, &d))
    {
        throw std::runtime_error(win_error());
    }
    d.BaudRate = baud;
    d.ByteSize = 8;
    d.Parity = NOPARITY;
    d.StopBits = ONESTOPBIT;
    d.fBinary = TRUE;
    d.fParity = FALSE;
    d.fOutxCtsFlow = FALSE;
    d.fOutxDsrFlow = FALSE;
    d.fOutX = FALSE;
    d.fInX = FALSE;
    d.fDtrControl = DTR_CONTROL_ENABLE;
    d.fRtsControl = RTS_CONTROL_ENABLE;
    d.fAbortOnError = FALSE;
    if (!SetCommState(h.value, &d))
    {
        throw std::runtime_error(win_error());
    }
    COMMTIMEOUTS t{};
    t.ReadIntervalTimeout = MAXDWORD;
    t.ReadTotalTimeoutConstant = 50;
    t.WriteTotalTimeoutConstant = 3000;
    if (!SetCommTimeouts(h.value, &t))
    {
        throw std::runtime_error(win_error());
    }
    SetupComm(h.value, 65536, 65536);
    return std::make_shared<Connection>(std::exchange(h.value, INVALID_HANDLE_VALUE));
}

std::string Core::open(const std::string& name, DWORD baud)
{
    // Serialize open/close. Publish desired settings before releasing the old connection; failures do not roll back.
    if (name.empty() || name.size() > 256 ||
        std::any_of(
            name.begin(),
            name.end(),
            [](unsigned char c)
            {
                return c < 32;
            }
        ))
    {
        return "invalid port name";
    }
    std::lock_guard operation(operation_);
    if (stopping_)
    {
        return "serial session stopped";
    }
    std::shared_ptr<Connection> old;
    {
        std::lock_guard lock(mutex_);
        ++generation_;
        reconnect_ = false;
        old = std::exchange(connection_, {});
        state_.connected = false;
        state_.port = name;
        state_.baud = baud;
    }
    if (old)
    {
        CancelIoEx(old->port.value, nullptr);
    }
    // Wait for readers/writers to release the old handle before reopening the same exclusive device.
    for (unsigned i = 0; old && old.use_count() > 1 && i < 200 && !stopping_; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    old.reset();
    std::string result;
    try
    {
        auto p = create_connection(name, baud);
        std::lock_guard lock(mutex_);
        connection_ = std::move(p);
        state_.connected = true;
        reconnect_ = true;
        state_.notice = "connected";
    }
    catch (const std::exception& e)
    {
        result = "open " + name + " failed: " + e.what();
        notice(result);
    }
    publish(
        Direction::Status,
        result.empty() ? "opened " + name + " @ " + std::to_string(baud) : result
    );
    return result;
}

void Core::close()
{
    std::lock_guard operation(operation_);
    std::shared_ptr<Connection> old;
    {
        std::lock_guard lock(mutex_);
        ++generation_;
        reconnect_ = false;
        old = std::exchange(connection_, {});
        state_.connected = false;
        state_.notice = "disconnected";
    }
    if (old)
    {
        CancelIoEx(old->port.value, nullptr);
    }
}

// Destroy stack-owned OVERLAPPED only after completion/cancellation. Return bytes completed by the driver.
static DWORD io(HANDLE port, bool write, void* data, DWORD size)
{
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event)
    {
        throw std::runtime_error(win_error());
    }
    OVERLAPPED op{};
    op.hEvent = event.value;
    DWORD count = 0;
    BOOL ok =
        write ? WriteFile(port, data, size, &count, &op) : ReadFile(port, data, size, &count, &op);
    if (!ok)
    {
        DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING)
        {
            throw std::runtime_error(win_error(error));
        }
        DWORD wait = WaitForSingleObject(event.value, write ? 3200 : 200);
        if (wait != WAIT_OBJECT_0)
        {
            CancelIoEx(port, &op);
            GetOverlappedResult(port, &op, &count, TRUE);
            if (!write && wait == WAIT_TIMEOUT)
            {
                return 0;
            }
            throw std::runtime_error("serial I/O timeout");
        }
        if (!GetOverlappedResult(port, &op, &count, FALSE))
        {
            throw std::runtime_error(win_error());
        }
    }
    return count;
}

std::string Core::send(const std::string& bytes)
{
    // Empty means WriteFile completed fully, not device execution. Never retry partial failures and duplicate commands.
    std::shared_ptr<Connection> p;
    {
        std::lock_guard lock(mutex_);
        p = connection_;
    }
    if (!p)
    {
        return "not connected";
    }
    try
    {
        size_t at = 0;
        while (at < bytes.size())
        {
            auto n =
                io(p->port.value,
                    true,
                    const_cast<char*>(bytes.data() + at),
                    DWORD(std::min<size_t>(bytes.size() - at, 65536)));
            if (!n)
            {
                throw std::runtime_error("serial write returned zero");
            }
            at += n;
        }
        publish(Direction::Tx, bytes);
        return {};
    }
    catch (const std::exception& e)
    {
        notice(e.what());
        return e.what();
    }
}

std::string Core::signals(bool dtr, bool rts)
{
    std::shared_ptr<Connection> p;
    {
        std::lock_guard lock(mutex_);
        p = connection_;
    }
    if (!p)
    {
        return "not connected";
    }
    if (!EscapeCommFunction(p->port.value, dtr ? SETDTR : CLRDTR) ||
        !EscapeCommFunction(p->port.value, rts ? SETRTS : CLRRTS))
    {
        return win_error();
    }
    return {};
}

std::future<std::string> Core::submit(std::function<std::string()> job)
{
    // Tasks/receipts own their data. Bound the queue to 64; shutdown receipts report stopped without touching windows.
    auto promise = std::make_shared<std::promise<std::string>>();
    auto f = promise->get_future();
    std::lock_guard lock(jobs_mutex_);
    if (stopping_ || jobs_.size() >= 64)
    {
        promise->set_value("serial task queue full or stopped");
        return f;
    }
    jobs_.push_back(
        [this, promise, job = std::move(job)]
        {
            try
            {
                promise->set_value(stopping_ ? "serial core stopped" : job());
            }
            catch (const std::exception& e)
            {
                promise->set_value(e.what());
            }
        }
    );
    jobs_cv_.notify_one();
    return f;
}

void Core::worker()
{
    for (;;)
    {
        std::function<void()> job;
        {
            std::unique_lock lock(jobs_mutex_);
            jobs_cv_.wait(
                lock,
                [&]
                {
                    return stopping_ || !jobs_.empty();
                }
            );
            if (jobs_.empty() && stopping_)
            {
                break;
            }
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        job();
    }
}

void Core::reader()
{
    unsigned backoff = 300;
    for (;;)
    {
        if (stopping_)
        {
            return;
        }
        std::shared_ptr<Connection> p;
        bool retry;
        uint64_t gen;
        CoreState s;
        {
            std::lock_guard lock(mutex_);
            p = connection_;
            retry = reconnect_;
            gen = generation_;
            s = state_;
        }
        if (!p)
        {
            if (!retry)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            for (unsigned ms = 0; ms < backoff && !stopping_; ms += 20)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                std::lock_guard lock(mutex_);
                if (generation_ != gen)
                {
                    break;
                }
            }
            if (stopping_)
            {
                return;
            }
            std::lock_guard operation(operation_);
            {
                std::lock_guard lock(mutex_);
                if (generation_ != gen || !reconnect_)
                {
                    continue;
                }
            }
            try
            {
                auto next = create_connection(s.port, s.baud);
                std::lock_guard lock(mutex_);
                if (generation_ == gen)
                {
                    connection_ = std::move(next);
                    state_.connected = true;
                    state_.notice = "reconnected";
                    backoff = 300;
                }
            }
            catch (...)
            {
                backoff = std::min(backoff * 2, 3000u);
            }
            continue;
        }
        try
        {
            char buffer[4096];
            DWORD n = io(p->port.value, false, buffer, sizeof buffer);
            if (n)
            {
                publish(Direction::Rx, std::string(buffer, n));
            }
        }
        catch (const std::exception& e)
        {
            std::lock_guard lock(mutex_);
            if (connection_ == p)
            {
                connection_.reset();
                state_.connected = false;
                state_.notice = "read error: " + std::string(e.what());
            }
        }
    }
}

void Core::stop()
{
    if (stopping_.exchange(true))
    {
        return;
    }
    log_.release_producers();
    close();
    jobs_cv_.notify_all();
    if (reader_.joinable())
    {
        reader_.join();
    }
    if (writer_.joinable())
    {
        writer_.join();
    }
}
} // namespace sm
