#pragma once
#include <cstdio>
#include <cstdarg>

namespace core {

enum class LogLevel { Debug, Info, Warn, Error };

namespace detail {
inline const char* levelTag(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return "DBG";
        case LogLevel::Info:  return "INF";
        case LogLevel::Warn:  return "WRN";
        case LogLevel::Error: return "ERR";
    }
    return "???";
}

inline void logV(LogLevel l, const char* fmt, va_list args) {
#ifdef NDEBUG
    if (l == LogLevel::Debug) return;
#endif
    ::fprintf(stderr, "[%s] ", levelTag(l));
    ::vfprintf(stderr, fmt, args);
    ::fprintf(stderr, "\n");
}
} // namespace detail

inline void logDebug(const char* fmt, ...) { va_list a; va_start(a, fmt); detail::logV(LogLevel::Debug, fmt, a); va_end(a); }
inline void logInfo (const char* fmt, ...) { va_list a; va_start(a, fmt); detail::logV(LogLevel::Info,  fmt, a); va_end(a); }
inline void logWarn (const char* fmt, ...) { va_list a; va_start(a, fmt); detail::logV(LogLevel::Warn,  fmt, a); va_end(a); }
inline void logError(const char* fmt, ...) { va_list a; va_start(a, fmt); detail::logV(LogLevel::Error, fmt, a); va_end(a); }

} // namespace core
