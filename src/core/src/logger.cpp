#include "lab/core/logger.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace lab::core {
namespace {

const char* levelName(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warning:
        return "WARNING";
    case LogLevel::Error:
        return "ERROR";
    }
    return "UNKNOWN";
}

std::string currentTime() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  now.time_since_epoch()) %
                              1000;
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    std::ostringstream stream;
    stream << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << '.'
           << std::setfill('0') << std::setw(3) << milliseconds.count();
    return stream.str();
}

}  // namespace

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::setMinimumLevel(LogLevel level) noexcept {
    std::scoped_lock lock(mutex_);
    minimumLevel_ = level;
}

bool Logger::setLogFile(const std::filesystem::path& path) {
    std::scoped_lock lock(mutex_);
    file_.close();
    file_.open(path, std::ios::app);
    return file_.is_open();
}

void Logger::log(
    LogLevel level,
    const std::string& component,
    const std::string& message) {
    std::scoped_lock lock(mutex_);
    if (level < minimumLevel_) {
        return;
    }
    const auto line = currentTime() + " [" + levelName(level) + "] [" +
                      component + "] " + message;
    std::clog << line << '\n';
    if (file_.is_open()) {
        file_ << line << '\n';
        file_.flush();
    }
}

}  // namespace lab::core

