#include "core/log.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace gm {
namespace {

std::atomic<int> g_level{static_cast<int>(LogLevel::Info)};
std::mutex g_mutex;

LogLevel clamp_level(LogLevel level) {
    if (level < LogLevel::Trace) {
        return LogLevel::Trace;
    }
    if (level > LogLevel::Off) {
        return LogLevel::Off;
    }
    return level;
}

} // namespace

const char* log_level_name(LogLevel level) {
    switch (level) {
    case LogLevel::Trace:
        return "trace";
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warn:
        return "warn";
    case LogLevel::Error:
        return "error";
    case LogLevel::Off:
        return "off";
    }
    return "?";
}

void set_log_level(LogLevel level) {
    g_level.store(static_cast<int>(clamp_level(level)), std::memory_order_relaxed);
}

LogLevel log_level() {
    return static_cast<LogLevel>(g_level.load(std::memory_order_relaxed));
}

bool log_enabled(LogLevel level) {
    return static_cast<int>(level) >= g_level.load(std::memory_order_relaxed);
}

void set_log_level_from_env() {
    const char* env = std::getenv("GM_LOG");
    if (env == nullptr || *env == '\0') {
        return;
    }
    std::string_view value{env};
    for (LogLevel level : {LogLevel::Trace, LogLevel::Debug, LogLevel::Info,
                           LogLevel::Warn, LogLevel::Error, LogLevel::Off}) {
        if (value == log_level_name(level)) {
            set_log_level(level);
            return;
        }
    }
}

void log_message(LogLevel level, std::string_view category, std::string_view message) {
    if (!log_enabled(level)) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    std::fprintf(stderr, "[%s] %.*s: %.*s\n", log_level_name(level),
                 static_cast<int>(category.size()), category.data(),
                 static_cast<int>(message.size()), message.data());
    std::fflush(stderr);
}

} // namespace gm
