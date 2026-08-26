#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace lab::core {

enum class LogLevel { Debug, Info, Warning, Error };

class Logger {
public:
    static Logger& instance();

    void setMinimumLevel(LogLevel level) noexcept;
    bool setLogFile(const std::filesystem::path& path);
    void log(LogLevel level, const std::string& component, const std::string& message);

private:
    Logger() = default;

    std::mutex mutex_;
    std::ofstream file_;
    LogLevel minimumLevel_{LogLevel::Info};
};

}  // namespace lab::core

