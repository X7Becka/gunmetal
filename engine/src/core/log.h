#pragma once

#include <sstream>
#include <string>
#include <string_view>

namespace gm {

enum class LogLevel : int {
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error,
    Off,
};

const char* log_level_name(LogLevel level);

void set_log_level(LogLevel level);
LogLevel log_level();
bool log_enabled(LogLevel level);

/// Read `GM_LOG` (trace/debug/info/warn/error/off) once at startup.
void set_log_level_from_env();

/// Write one line to stderr: `[level] category: message`.
void log_message(LogLevel level, std::string_view category, std::string_view message);

template <class... Args>
void logf(LogLevel level, std::string_view category, Args&&... args) {
    if (!log_enabled(level)) {
        return;
    }
    std::ostringstream os;
    (os << ... << args);
    log_message(level, category, os.str());
}

} // namespace gm
