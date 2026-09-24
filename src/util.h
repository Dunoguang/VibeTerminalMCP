#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <nlohmann/json.hpp>
#include <string>

namespace mcp {

// log levels
enum class LogLevel { TRACE = 0, DEBUG, INFO, WARN, ERROR };

// init logging from SHELL_MCP_LOG env (trace/debug/info/warn/error)
void init_logging();
LogLevel log_level();
void log_msg(LogLevel lv, const std::string& msg);

// all logging goes to stderr (MCP stdio rule: stdout = protocol only)
#define LOG_TRACE(...) ::mcp::log_msg(::mcp::LogLevel::TRACE, std::format(__VA_ARGS__))
#define LOG_DEBUG(...) ::mcp::log_msg(::mcp::LogLevel::DEBUG, std::format(__VA_ARGS__))
#define LOG_INFO(...)  ::mcp::log_msg(::mcp::LogLevel::INFO,  std::format(__VA_ARGS__))
#define LOG_WARN(...)  ::mcp::log_msg(::mcp::LogLevel::WARN,  std::format(__VA_ARGS__))
#define LOG_ERROR(...) ::mcp::log_msg(::mcp::LogLevel::ERROR, std::format(__VA_ARGS__))

// audit log
void audit_log(const std::string& tool, const std::string& reason,
               const std::string& args_summary, const std::string& status);
void audit_log(const std::string& tool, const std::string& reason,
               const nlohmann::json& args, const std::string& status);

// monotonic milliseconds
int64_t now_ms();

// directory holding this executable (/proc/self/exe), falls back to cwd
std::filesystem::path exe_dir();
// resolved audit log path (default: <exe_dir>/audit.log)
std::string audit_path();
// override audit log path (CLI --audit-log); empty = auto
void set_audit_path(const std::string& path);

// ---- output hygiene ----
// drop ANSI/VT escape sequences + non printable control chars
std::string strip_ansi(const std::string& in);
// replace invalid UTF-8 byte runs with U+FFFD (never throws)
std::string sanitize_utf8(const std::string& in);
// strip_ansi + sanitize_utf8: call before putting text into JSON
std::string sanitize_output(const std::string& in);
// keep only the last max_bytes bytes, aligned to a UTF-8 boundary;
// returns true when the string was actually truncated
bool clip_tail_utf8(std::string& s, std::size_t max_bytes);
// json::dump that replaces invalid UTF-8 instead of throwing
std::string safe_dump(const nlohmann::json& j, int indent = -1);

} // namespace mcp
