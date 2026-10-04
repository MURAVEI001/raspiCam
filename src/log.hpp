#pragma once
#include <cstdio>
#include <ctime>
#include <cstdarg>
#include <mutex>

namespace mjpeg {

enum class LogLevel { DBG, INFO, WARN, ERR };

inline const char* level_str(LogLevel l) {
    switch (l) {
        case LogLevel::DBG:  return "DBG ";
        case LogLevel::INFO: return "INFO";
        case LogLevel::WARN: return "WARN";
        case LogLevel::ERR:  return "ERR ";
    }
    return "???";
}

inline std::mutex& log_mutex() {
    static std::mutex m;
    return m;
}

inline void log(LogLevel lvl, const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(log_mutex());
    char tbuf[32];
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tm);

    std::fprintf(stderr, "[%s][%s] ", tbuf, level_str(lvl));
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

} // namespace mjpeg

#define LOG_DBG(...)  ::mjpeg::log(::mjpeg::LogLevel::DBG,  __VA_ARGS__)
#define LOG_INFO(...) ::mjpeg::log(::mjpeg::LogLevel::INFO, __VA_ARGS__)
#define LOG_WARN(...) ::mjpeg::log(::mjpeg::LogLevel::WARN, __VA_ARGS__)
#define LOG_ERR(...)  ::mjpeg::log(::mjpeg::LogLevel::ERR,  __VA_ARGS__)