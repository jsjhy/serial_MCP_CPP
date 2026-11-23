#include "core.hpp"

namespace sm
{
void simulated_serial_log_test(Config config)
{
    config.log_dir /= "simulated-serial";
    config.rotate = 1;
    LogWriter log(config);
    const std::string time = "12:34:56.000";
    const std::string first = "[" + time + "] ready\n";
    const std::string second = "[" + time + "] sensor: \xe4\xb8\xad\n";
    auto read_files = [&]
    {
        std::vector<std::string> contents;
        if (fs::exists(config.log_dir))
        {
            for (const auto& entry : fs::directory_iterator(config.log_dir))
            {
                std::ifstream file(entry.path(), std::ios::binary);
                contents.emplace_back(
                    std::istreambuf_iterator<char>(file),
                    std::istreambuf_iterator<char>()
                );
            }
        }
        return contents;
    };

    // Feed synthetic RX chunks directly to the real logger. Split both the row and its UTF-8.
    log.push({Direction::Rx, "ready\r\nsensor: \xe4", time});
    auto deadline = Clock::now() + std::chrono::seconds(3);
    auto first_cursor = log.committed();
    while (first_cursor.line != 1 || read_files() != std::vector<std::string>{first})
    {
        if (Clock::now() >= deadline)
        {
            throw std::runtime_error("simulated RX flush must expose only the complete row");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        first_cursor = log.committed();
    }
    log.push({Direction::Rx, "\xb8\xad\r", time});
    log.push({Direction::Rx, "\n", time});
    log.stop();

    const auto second_cursor = log.committed();
    if (first_cursor.filename.empty() || second_cursor.filename.empty() ||
        first_cursor.filename == second_cursor.filename || second_cursor.line != 1)
    {
        throw std::runtime_error("rotation must reset the committed row number in the next file");
    }
    auto contents = read_files();
    std::sort(contents.begin(), contents.end());
    if (contents != std::vector<std::string>{first, second})
    {
        throw std::runtime_error("simulated RX rotation must preserve complete UTF-8 rows");
    }
}
} // namespace sm
