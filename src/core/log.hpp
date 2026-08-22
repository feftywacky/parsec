#pragma once
#include <cstdarg>
#include <cstdint>
#include <cstdio>

#include "core/time.hpp"

// Lightweight, allocation-free levelled logger for the engine thread (docs/07 Phase 1 file
// list). No std::string, no iostreams, no heap: every call formats into a fixed stack buffer
// and writes with a single fwrite/fprintf, so it is safe to sprinkle through the engine's
// event loop without violating the "no allocation on the market-data path" rule (docs/02 §1).
// It is a diagnostic tool, not a tracing framework — callers still own not calling it every
// tick for something that happens every tick.
//
// SECURITY (docs/06 §5): log lines must never contain key material. No private keys, no
// keystore passphrases, no raw ECDSA signatures, no mnemonic/seed bytes. If a value could be
// used to move funds or unlock the keystore, it does not belong in a log line, full stop.
namespace pc {

enum class LogLevel : uint8_t { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4 };

namespace detail {

// Function-local static avoids a global constructor (unsequenced init order across TUs would
// otherwise be a real hazard for something read from the engine thread at startup).
inline LogLevel& min_level_ref() noexcept {
    static LogLevel level = LogLevel::Info;
    return level;
}

inline const char* level_name(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:
            return "TRACE";
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warn:
            return "WARN";
        case LogLevel::Error:
            return "ERROR";
    }
    return "?";
}

// printf-style formatting into a fixed buffer (vsnprintf truncates rather than growing), then
// one fprintf to stderr. stderr is unbuffered-by-default on most libcs and kept separate from
// any stdout the app writes for its own console output (docs/07 Phase 1's "prints a live book
// ... to stdout" milestone).
inline void write_line(LogLevel level, const char* fmt, va_list args) noexcept {
    char buf[512];
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    std::fprintf(stderr, "[%llu][%s] %s\n", static_cast<unsigned long long>(unix_ms()),
                 level_name(level), buf);
}

}  // namespace detail

// Sets the minimum level that actually gets formatted/written; anything below it is a no-op
// branch, so callers can leave PC_LOG_TRACE/DEBUG calls in hot paths without measurable cost
// once the level is raised above them at startup.
inline void set_log_level(LogLevel level) noexcept {
    detail::min_level_ref() = level;
}

inline LogLevel log_level() noexcept {
    return detail::min_level_ref();
}

// [[gnu::format]] would be nice here but this header must stay portable (MSVC is not out of
// scope per docs/05 §6), so callers get the checking their compiler's -Wformat gives them via
// the vsnprintf-style call.
inline void log_line(LogLevel level, const char* fmt, ...) noexcept {
    if (level < detail::min_level_ref())
        return;
    va_list args;
    va_start(args, fmt);
    detail::write_line(level, fmt, args);
    va_end(args);
}

}  // namespace pc

#define PC_LOG_TRACE(...) ::pc::log_line(::pc::LogLevel::Trace, __VA_ARGS__)
#define PC_LOG_DEBUG(...) ::pc::log_line(::pc::LogLevel::Debug, __VA_ARGS__)
#define PC_LOG_INFO(...) ::pc::log_line(::pc::LogLevel::Info, __VA_ARGS__)
#define PC_LOG_WARN(...) ::pc::log_line(::pc::LogLevel::Warn, __VA_ARGS__)
#define PC_LOG_ERROR(...) ::pc::log_line(::pc::LogLevel::Error, __VA_ARGS__)
