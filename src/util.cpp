#include "util.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>

namespace mcp {

namespace {
LogLevel g_level = LogLevel::INFO;
std::mutex g_log_mutex;

LogLevel parse_level(const char* s) {
    if (!s) return LogLevel::INFO;
    std::string v(s);
    if (v == "trace") return LogLevel::TRACE;
    if (v == "debug") return LogLevel::DEBUG;
    if (v == "info")  return LogLevel::INFO;
    if (v == "warn" || v == "warning") return LogLevel::WARN;
    if (v == "error") return LogLevel::ERROR;
    return LogLevel::INFO;
}

const char* level_name(LogLevel lv) {
    switch (lv) {
        case LogLevel::TRACE: return "TRACE";
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERROR: return "ERROR";
    }
    return "?";
}
} // namespace

void init_logging() {
    g_level = parse_level(std::getenv("SHELL_MCP_LOG"));
}

LogLevel log_level() { return g_level; }

void log_msg(LogLevel lv, const std::string& msg) {
    if (lv < g_level) return;
    std::lock_guard lock(g_log_mutex);
    // stderr: MCP stdio 规范允许任意日志；stdout 严禁
    std::cerr << std::format("[{} {}] {}\n", level_name(lv), now_ms(), msg);
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace mcp
