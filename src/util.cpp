#include "util.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

namespace {
std::mutex g_audit_mutex;
const std::string kAuditPath = "/root/github/shell-mcp-cpp/audit.log";

std::string now_str() {
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm;
    localtime_r(&t, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}
}

namespace mcp {

void audit_log(const std::string& tool, const std::string& reason,
               const std::string& args_summary, const std::string& status) {
    std::lock_guard lock(g_audit_mutex);
    std::ofstream f(kAuditPath, std::ios::app);
    if (!f) return;
    f << "[" << now_str() << "] " << tool << " | reason=" << reason
      << " | args=" << args_summary << " | status=" << status << std::endl;
}

void audit_log(const std::string& tool, const std::string& reason,
               const nlohmann::json& args, const std::string& status) {
    std::string summary = args.dump();
    if (summary.size() > 200) summary = summary.substr(0, 200) + "...";
    audit_log(tool, reason, summary, status);
}

} // namespace mcp

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

} // namespace

namespace {
std::mutex g_audit_mutex;
const std::string kAuditPath = "/root/github/shell-mcp-cpp/audit.log";

std::string now_str() {
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm;
    localtime_r(&t, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}
}

namespace mcp {

void audit_log(const std::string& tool, const std::string& reason,
               const std::string& args_summary, const std::string& status) {
    std::lock_guard lock(g_audit_mutex);
    std::ofstream f(kAuditPath, std::ios::app);
    if (!f) return;
    f << "[" << now_str() << "] " << tool << " | reason=" << reason
      << " | args=" << args_summary << " | status=" << status << std::endl;
}

void audit_log(const std::string& tool, const std::string& reason,
               const nlohmann::json& args, const std::string& status) {
    std::string summary = args.dump();
    if (summary.size() > 200) summary = summary.substr(0, 200) + "...";
    audit_log(tool, reason, summary, status);
}

} // namespace mcp mcp
