#pragma once
#include <string>
#include <format>
#include <cstdint>

namespace mcp {

// 日志级别
enum class LogLevel { TRACE = 0, DEBUG, INFO, WARN, ERROR };

// 初始化日志（从环境变量 SHELL_MCP_LOG 读级别: trace/debug/info/warn/error）
void init_logging();
LogLevel log_level();
void log_msg(LogLevel lv, const std::string& msg);

// 便捷宏: 全部输出到 stderr（MCP stdio 铁律: stdout 只允许协议帧）
#define LOG_TRACE(...) ::mcp::log_msg(::mcp::LogLevel::TRACE, std::format(__VA_ARGS__))
#define LOG_DEBUG(...) ::mcp::log_msg(::mcp::LogLevel::DEBUG, std::format(__VA_ARGS__))
#define LOG_INFO(...)  ::mcp::log_msg(::mcp::LogLevel::INFO,  std::format(__VA_ARGS__))
#define LOG_WARN(...)  ::mcp::log_msg(::mcp::LogLevel::WARN,  std::format(__VA_ARGS__))
#define LOG_ERROR(...) ::mcp::log_msg(::mcp::LogLevel::ERROR, std::format(__VA_ARGS__))

// 毫秒时间戳（monotonic）
int64_t now_ms();

} // namespace mcp
