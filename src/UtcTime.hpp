#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

// UTC helpers. Store timestamps are unix-epoch nanoseconds (same value on every host).
namespace utc {

inline std::chrono::system_clock::time_point now() {
    return std::chrono::system_clock::now();
}

inline std::int64_t now_ns() {
    using namespace std::chrono;
    // Unix epoch ns — same number on every host, stored in SQLite ts.
    return duration_cast<nanoseconds>(now().time_since_epoch()).count();
}

inline std::tm to_tm(std::time_t unix_time) {
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &unix_time);
#else
    gmtime_r(&unix_time, &utc);
#endif
    return utc;  // always UTC, never localtime
}

// DBG clock: HH:MM:SS:mmm from the same UTC instant as now_ns().
inline std::string hms_ms(std::chrono::system_clock::time_point time_point = now()) {
    const auto unix_time = std::chrono::system_clock::to_time_t(time_point);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(time_point.time_since_epoch()) % 1000;
    const std::tm utc = to_tm(unix_time);

    std::ostringstream stream;
    stream << std::put_time(&utc, "%H:%M:%S") << ':'
           << std::setfill('0') << std::setw(3) << milliseconds.count();
    return stream.str();
}

// Filename-safe UTC stamp for one process run: 20260812-115602-123
inline std::string run_id(std::chrono::system_clock::time_point time_point = now()) {
    const auto unix_time = std::chrono::system_clock::to_time_t(time_point);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(time_point.time_since_epoch()) % 1000;
    const std::tm utc = to_tm(unix_time);

    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y%m%d-%H%M%S") << '-'
           << std::setfill('0') << std::setw(3) << milliseconds.count();
    return stream.str();
}

// UTC calendar day for DBG daily rotation: 20260812
inline std::string date_id(std::chrono::system_clock::time_point time_point = now()) {
    const auto unix_time = std::chrono::system_clock::to_time_t(time_point);
    const std::tm utc = to_tm(unix_time);
    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y%m%d");
    return stream.str();
}

} // namespace utc
