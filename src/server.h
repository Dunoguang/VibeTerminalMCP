#pragma once
#include "terminal.h"
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace mcp {

using json = nlohmann::json;

// MCP 协议层：JSON-RPC 2.0 请求分发（2025-06-18 主协议）
class TerminalMCPServer {
public:
    TerminalMCPServer();

    // 进度回调: 执行长任务期间发出 notifications/progress（HTTP SSE 流用）
    using ProgressCb = std::function<void(const json& notification)>;

    // 处理一行 JSON-RPC 请求，返回响应（通知返回 nullopt）
    // 返回的 json 已是完整响应帧（含 id），可直接序列化输出
    std::optional<json> handle_request(const json& req, ProgressCb progress = nullptr);

    // 工具列表（tools/list 内容）
    const json& tools() const { return tools_; }

private:
    json tools_;  // tools/list 返回的 tools 数组
    TerminalManager terminal_manager_;

    // 各方法处理器
    json handle_initialize(const json& params);
    json handle_list_tools(const json& params);
    json handle_call_tool(const json& params, ProgressCb progress);
    json handle_ping();
    json handle_discover();  // 2026-07-28 预留

    // 工具实现
    json tool_execute_command(const json& args, ProgressCb progress);
    json tool_terminal_new(const json& args);
    json tool_terminal_add_cmd(const json& args);
    json tool_terminal_line(const json& args);
    json tool_terminal_last(const json& args);
    json tool_terminal_wait(const json& args);
    json tool_terminal_timeout(const json& args);
    json tool_terminal_kill(const json& args);
    json tool_terminal_list(const json& args);
    json tool_terminal_resize(const json& args);
    json tool_get_tools(const json& args, ProgressCb progress);

    // JSON-RPC 错误构造
    static json error(int code, const std::string& message, json data = nullptr);
};

} // namespace mcp
