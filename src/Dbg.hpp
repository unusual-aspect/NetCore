#pragma once

#include "NetDefaults.hpp"
#include "UtcTime.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

// [HH:MM:SS:mmm] path@fn[line] : text  — padded to DEBUG_LINE_WIDTH before ':'.
#define DEBUG_LINE_WIDTH 100

namespace netdbg {

inline std::string currentTime() {
    return utc::hms_ms();
}

// Structured line used on the wire path: TIME=<utc> IP=… OP=… MSG=…
// Bodies are redacted unless setVerbose(true) was called (--verbose).
// Atomic: integration tests run several ClientApp threads in one process.
inline std::atomic<bool>& verboseLogging() {
    static std::atomic<bool> verbose{false};
    return verbose;
}

inline void setVerbose(bool enabled) {
    verboseLogging().store(enabled, std::memory_order_relaxed);
}

inline bool isVerbose() {
    return verboseLogging().load(std::memory_order_relaxed);
}

inline std::string redactMessage(std::string_view message) {
    if (message.empty() || isVerbose()) {
        return std::string(message);
    }
    if (message == "<empty>" || message == "Ok" || message == "Goodbye") {
        return std::string(message);
    }
    return "<redacted len=" + std::to_string(message.size()) + ">";
}

inline std::string event(std::string_view operation, std::string_view ip, std::string_view message = {}) {
    std::string line = "TIME=";
    line.append(utc::hms_ms()).append(" UTC");
    line.append(" IP=").append(ip.empty() ? "unknown" : ip);
    line.append(" OP=").append(operation);
    if (!message.empty()) {
        line.append(" MSG=").append(redactMessage(message));
    }
    return line;
}

inline std::string nativePath(std::string file) {
#ifdef _WIN32
    // MSVC __FILE__ is `\`, some TUs still give `/` — normalize before compare.
    for (char& ch : file) {
        if (ch == '/') {
            ch = '\\';
        }
    }
#endif
    return file;
}

// __FILE__ is absolute when DBG lives in a header; keep the suffix after the shared prefix with this file.
inline std::string shortFile(std::string file) {
    file = nativePath(std::move(file));
    const std::string this_file = nativePath(__FILE__);

    // Walk until the paths diverge; keep from the last matching slash-ish char.
    std::size_t shared_prefix = 0;
    const std::size_t shorter =
        file.size() < this_file.size() ? file.size() : this_file.size();
    for (std::size_t index = 0; index < shorter; ++index) {
        if (file[index] != this_file[index]) {
            break;
        }
        shared_prefix = index;
    }
    if (shared_prefix != 0) {
        return ".." + file.substr(shared_prefix);
    }
    return file;
}

// __func__ on a lambda is operator()/operator (); swap in __FUNCTION__ and drop <lambda…>.
inline std::string lambdaFilter(std::string cpp_function, const std::string& msvc_function) {
    auto operator_pos = cpp_function.find("operator ()");
    if (operator_pos == std::string::npos) {
        operator_pos = cpp_function.find("operator()");
    }
    if (operator_pos != std::string::npos) {
        const auto operator_len = cpp_function[operator_pos + 8] == ' ' ? 11u : 10u;
        cpp_function.replace(operator_pos, operator_len, msvc_function);
    }

    // Drop "<lambda_...>" so the log shows the enclosing function, not the lambda type.
    const auto angle_pos = cpp_function.find('<');
    const auto colon_pos = cpp_function.find("::");
    if (angle_pos != std::string::npos) {
        std::string value = cpp_function.substr(0, angle_pos >= 2 ? angle_pos - 2 : 0);
        if (colon_pos != std::string::npos && colon_pos + 2 < value.size()) {
            value = value.substr(colon_pos + 2);
        }
        return value;
    }
    return cpp_function;
}

inline std::mutex& logMutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::ofstream& logFile() {
    static std::ofstream file;
    return file;
}

inline std::string& logPath() {
    static std::string path;
    return path;
}

// Relative to cwd (logs/<file>) so home directories never land in retained DBG.
inline std::string logPathForDisplay() {
    const std::filesystem::path full{logPath()};
    if (full.empty()) {
        return {};
    }
    std::error_code error;
    const auto cwd = std::filesystem::current_path(error);
    if (!error) {
        const auto rel = std::filesystem::relative(full, cwd, error);
        if (!error && !rel.empty()) {
            const auto generic = rel.generic_string();
            if (generic != "." && generic != ".." && generic.find("../") != 0) {
                return generic;
            }
        }
    }
    return (std::filesystem::path("logs") / full.filename()).generic_string();
}

inline std::string& runId() {
    static std::string id;
    return id;
}

inline std::string& logRole() {
    static std::string role;
    return role;
}

inline std::filesystem::path& logDirectory() {
    static std::filesystem::path dir;
    return dir;
}

inline int& logPid() {
    static int pid = 0;
    return pid;
}

inline std::uint64_t& logBytesWritten() {
    static std::uint64_t bytes = 0;
    return bytes;
}

inline std::string& logDayUtc() {
    static std::string day;
    return day;
}

// Keep newest kDbgLogMaxFiles matching "{role}-*.log" under logs/.
inline void pruneDbgLogFilesLocked() {
    if (logRole().empty() || logDirectory().empty() || kDbgLogMaxFiles == 0) {
        return;
    }

    struct Entry {
        std::filesystem::path path;
        std::filesystem::file_time_type mtime;
    };
    std::vector<Entry> entries;
    std::error_code error;
    for (const auto& item : std::filesystem::directory_iterator(logDirectory(), error)) {
        if (error || !item.is_regular_file(error)) {
            continue;
        }
        const auto name = item.path().filename().string();
        const std::string prefix = logRole() + "-";
        const std::string suffix = ".log";
        if (name.size() <= prefix.size() + suffix.size()) {
            continue;
        }
        if (name.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }
        if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
            continue;
        }
        entries.push_back(Entry{item.path(), item.last_write_time(error)});
    }

    if (entries.size() <= kDbgLogMaxFiles) {
        return;
    }

    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.mtime > b.mtime;
    });
    for (std::size_t index = kDbgLogMaxFiles; index < entries.size(); ++index) {
        std::filesystem::remove(entries[index].path, error);
    }
}

// Opens a new segment. Caller holds logMutex().
inline bool openNextDbgFileLocked() {
    const std::string id = utc::run_id();
    runId() = id;
    const auto path =
        logDirectory() / (logRole() + "-" + id + "-" + std::to_string(logPid()) + ".log");
    const std::string path_str = path.string();

    if (logFile().is_open()) {
        logFile().close();
    }
    logFile().open(path, std::ios::out | std::ios::trunc);
    if (!logFile().is_open()) {
        logPath() = path_str;
        logBytesWritten() = 0;
        return false;
    }
    logPath() = path_str;
    logBytesWritten() = 0;
    logDayUtc() = utc::date_id();
    pruneDbgLogFilesLocked();
    return true;
}

inline void maybeRotateDbgFileLocked() {
    if (!logFile().is_open()) {
        return;
    }
    const std::string today = utc::date_id();
    const bool new_day = !logDayUtc().empty() && logDayUtc() != today;
    const bool size_hit = logBytesWritten() >= kDbgLogMaxBytes;
    if (!new_day && !size_hit) {
        return;
    }
    logFile() << "[" << currentTime() << "] DBG log rotating ("
              << (new_day ? "new UTC day " + today : "size limit " + std::to_string(kDbgLogMaxBytes) + " bytes")
              << ")\n";
    logFile().flush();
    logFile().close();
    if (!openNextDbgFileLocked()) {
        std::cerr << "Cannot rotate DBG log to '" << logPath()
                  << "' — further DBG stays on stderr only.\n";
    }
}

// One active file under <cwd>/logs/{role}-{UTC stamp}-{pid}.log
// Rotates each UTC calendar day (and at kDbgLogMaxBytes within a day).
// Keeps newest kDbgLogMaxFiles for this role.
// Returns false if the logs/ folder or first file cannot be created — callers should exit.
inline bool openLog(std::string_view role, const char* /*argv0*/ = nullptr) {
#ifdef _WIN32
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif

    std::error_code error;
    const auto log_dir = std::filesystem::current_path(error) / "logs";
    if (error) {
        std::cerr << "Cannot resolve working directory for logs/ — " << error.message() << '\n';
        return false;
    }

    std::filesystem::create_directories(log_dir, error);
    if (error || !std::filesystem::is_directory(log_dir)) {
        std::cerr << "Cannot create log folder '" << log_dir.string() << "' — "
                  << (error ? error.message() : "not a directory") << '\n';
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(logMutex());
        logRole() = std::string(role);
        logDirectory() = log_dir;
        logPid() = pid;
        if (!openNextDbgFileLocked()) {
            std::cerr << "Cannot write log file '" << logPath()
                      << "' — refusing to run without retained DBG.\n";
            return false;
        }
    }
    return true;
}

inline void printDbg(std::string file, std::string function_name, std::string_view text = {}) {
    std::string line = "[" + currentTime() + "] ";
    line += shortFile(std::move(file));
    line += "@";
    line += function_name;

    if (!text.empty()) {
        // Pad so the ": text" column lines up across files.
        if (line.size() < DEBUG_LINE_WIDTH) {
            line.append(DEBUG_LINE_WIDTH - line.size(), ' ');
        }
        line += ": ";
        line.append(text);
    }

    std::lock_guard<std::mutex> lock(logMutex());
    std::cerr << line << '\n';
    if (logFile().is_open()) {
        logFile() << line << '\n';
        logFile().flush();
        logBytesWritten() += static_cast<std::uint64_t>(line.size()) + 1u;
        maybeRotateDbgFileLocked();
    }
}

} // namespace netdbg

#define STR_FN_LN \
    netdbg::lambdaFilter(__func__, __FUNCTION__) + "[" + std::to_string(__LINE__) + "]"

#define DBG(...) netdbg::printDbg(__FILE__, STR_FN_LN, __VA_ARGS__)
