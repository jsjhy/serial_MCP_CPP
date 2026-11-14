// Single-port core: its GUI page and MCP requests share a send queue. Receipts confirm driver acceptance only.
// Owners join all workers. Connections/events use snapshots; windows never own serial handles.
#pragma once
#include "text.hpp"

namespace sm
{
struct LogCursor
{
    std::string filename;
    uint64_t line = 0; // One-based last flushed row; zero denotes no row.
};

class LogWriter
{
    Config config_;
    size_t queue_limit_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable space_;
    std::deque<Record> queue_;
    size_t bytes_ = 0;
    bool stopping_ = false;
    bool accepting_ = true;
    std::thread worker_;
    std::string filename_;
    LogCursor committed_;
    std::string error_;
    void loop();

public:
    explicit LogWriter(Config c, size_t queue_limit = 16 * 1024 * 1024);

    ~LogWriter()
    {
        stop();
    }

    void push(Record r);
    void
    release_producers(); // Wake backpressured producers before core shutdown; the log thread drains already queued records.
    void stop();
    std::string filename();
    LogCursor committed();

    const fs::path& directory() const
    {
        return config_.log_dir;
    }

    std::string error();
};

struct CoreState
{
    std::string port;
    std::string notice;
    std::string mcp = "disabled";
    DWORD baud = 115200;
    bool connected = false;
    uint64_t rx = 0;
    uint64_t tx = 0;
    uint64_t dropped = 0;
    uint64_t display_dropped = 0; // UI queue eviction count, independent of file logging.
};

class Core
{
    // Synthetic receive benchmark may exercise the real bounded publication path without hardware.
    friend int receive_stress_test();
    friend int self_test();

    struct Connection
    {
        Handle port;

        explicit Connection(HANDLE h)
            : port(h)
        {
        }

        ~Connection()
        {
            if (port)
            {
                CancelIoEx(port.value, nullptr);
            }
        }
    };

    LogWriter& log_;
    std::mutex mutex_;
    std::mutex operation_;
    std::mutex jobs_mutex_;
    std::condition_variable jobs_cv_;
    CoreState state_;
    std::shared_ptr<Connection> connection_;
    uint64_t generation_ = 0;
    bool reconnect_ = false;
    std::deque<Record> events_;
    std::deque<Record> history_;
    size_t event_bytes_ = 0;
    size_t history_bytes_ = 0;
    std::deque<std::function<void()>> jobs_;
    std::atomic<bool> stopping_ = false;
    std::thread reader_;
    std::thread writer_;
    void reader();
    void worker();
    void publish(Direction d, std::string bytes);
    std::shared_ptr<Connection> create_connection(const std::string& port, DWORD baud);

public:
    // baud initializes disconnected state without opening a device. The logger must outlive this core.
    explicit Core(LogWriter& log, DWORD baud = 115200);

    ~Core()
    {
        stop();
    }

    void stop();
    std::string open(
        const std::string& port,
        DWORD baud
    ); // Empty means success; failures retain the attempted configuration.
    void close();
    std::future<std::string> submit(std::function<std::string()> job);
    std::string send(const std::string& bytes);
    std::string signals(bool dtr, bool rts);
    CoreState state();
    // Both record count and raw bytes are bounded; oversized records continue on the next call.
    // This consumes only the display queue, leaving log/history records unchanged.
    std::vector<Record> drain(size_t limit = 64, size_t byte_limit = 32 * 1024);
    std::vector<Record> history(size_t limit);
    void notice(std::string text);
    void mcp(std::string text);
};
} // namespace sm
