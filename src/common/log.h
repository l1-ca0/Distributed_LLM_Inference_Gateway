#pragma once

// Lightweight structured logging with levels and timestamps.
//
// Usage:
//   LOG_INFO("gateway", "started on port %d", port);
//   LOG_DEBUG("swim", "ping %s seq=%lu", peer_id.c_str(), seq);
//   LOG_WARN("lb", "replica %s circuit open", id.c_str());
//   LOG_ERROR("replica", "generate failed: %s", err.c_str());
//
// Control:
//   Log::SetLevel(LogLevel::WARN);   // suppress DEBUG and INFO
//   Log::SetLevel(LogLevel::DEBUG);  // show everything
//   Log::SetQuiet(true);             // suppress all output (for clean test output)

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <atomic>

namespace llmgateway {

enum class LogLevel { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3, NONE = 4 };

class Log {
public:
    static void SetLevel(LogLevel level) { level_.store(level); }
    static LogLevel GetLevel() { return level_.load(); }
    static void SetQuiet(bool quiet) { quiet_.store(quiet); }

    static void Write(LogLevel level, const char* component, const char* fmt, ...) {
        if (quiet_.load()) return;
        if (level < level_.load()) return;

        // Timestamp (milliseconds since program start).
        static auto start = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();

        // Level prefix.
        const char* prefix = "";
        switch (level) {
            case LogLevel::DEBUG: prefix = "DBG"; break;
            case LogLevel::INFO:  prefix = "INF"; break;
            case LogLevel::WARN:  prefix = "WRN"; break;
            case LogLevel::ERROR: prefix = "ERR"; break;
            default: break;
        }

        // Format: [001234ms] INF [component] message
        fprintf(stderr, "[%06ldms] %s [%s] ", static_cast<long>(ms), prefix, component);

        va_list args;
        va_start(args, fmt);
        vfprintf(stderr, fmt, args);
        va_end(args);

        fprintf(stderr, "\n");
    }

private:
    static inline std::atomic<LogLevel> level_{LogLevel::INFO};
    static inline std::atomic<bool> quiet_{false};
};

}  // namespace llmgateway

#define LOG_DEBUG(component, ...) \
    llmgateway::Log::Write(llmgateway::LogLevel::DEBUG, component, __VA_ARGS__)
#define LOG_INFO(component, ...) \
    llmgateway::Log::Write(llmgateway::LogLevel::INFO, component, __VA_ARGS__)
#define LOG_WARN(component, ...) \
    llmgateway::Log::Write(llmgateway::LogLevel::WARN, component, __VA_ARGS__)
#define LOG_ERROR(component, ...) \
    llmgateway::Log::Write(llmgateway::LogLevel::ERROR, component, __VA_ARGS__)
