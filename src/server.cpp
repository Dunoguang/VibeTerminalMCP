#include "server.h"
#include "process_runner.h"
#include "util.h"

#include <optional>
#include <string>

namespace mcp {

namespace {
// 本项目支持的协议版本
constexpr const char* kProtocolVersion = "2025-06-18";
const std::vector<std::string> kSupportedVersions = {"2025-06-18", "2024-11-05"};

constexpr int64_t kDefaultTimeoutMs = 30000;

json tool_schema_execute_command() {
    return {
        {"name", "execute_command"},
        {"title", "Execute Command"},
        {"description", "在本地主机上执行命令（bash -c）。支持超时击杀进程组、环境变量注入、工作目录。输出超过 64KB 截断。"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"command", {{"type", "string"}, {"description", "要执行的命令"}}},
                {"timeout_ms", {{"type", "integer"}, {"description", "超时毫秒（<=0 不限时，默认 30000）。超时后击杀整个进程组并返回部分输出"}, {"default", kDefaultTimeoutMs}}},
                {"env", {{"type", "object"}, {"description", "环境变量覆盖（KEY=VALUE）"}}},
                {"cwd", {{"type", "string"}, {"description", "工作目录（默认继承服务器目录）"}}},
                {"force_execute", {{"type", "boolean"}, {"description", "预留：强制执行（当前无黑名单过滤）"}}},
            }},
            {"required", {"command"}},
        }},
    };
}

json tool_schema_get_tools() {
    return {
        {"name", "get_tools"},
        {"title", "Get Tools"},
        {"description", "获取服务器支持的所有工具列表（含完整 JSON Schema）"},
        {"inputSchema", {{"type", "object"}, {"properties", json::object()}}},
    };
}
} // namespace

TerminalMCPServer::TerminalMCPServer() {
    tools_ = json::array();
    tools_.push_back(tool_schema_execute_command());
    tools_.push_back(tool_schema_get_tools());
}

std::optional<json> TerminalMCPServer::handle_request(const json& req, ProgressCb progress) {
    int64_t t0 = now_ms();
    std::string method = req.value("method", "");

    // 通知（无 id）→ 处理但无响应
    bool is_notification = !req.contains("id");

    try {
        json result;
        if (method == "initialize") {
            result = handle_initialize(req.value("params", json::object()));
        } else if (method == "tools/list") {
            result = handle_list_tools(req.value("params", json::object()));
        } else if (method == "tools/call") {
            result = handle_call_tool(req.value("params", json::object()), progress);
        } else if (method == "ping") {
            result = handle_ping();
        } else if (method == "server/discover") {  // 2026-07-28 预留
            result = handle_discover();
        } else if (method == "notifications/initialized" || method == "notifications/cancelled") {
            // 通知：记录日志即可，无响应
            LOG_DEBUG("notification: {} (ignored, no response)", method);
            return std::nullopt;
        } else {
            json err = error(-32601, "Method not found: " + method);
            if (is_notification) return std::nullopt;
            err["id"] = req.value("id", json(nullptr));
            LOG_WARN("method not found: {} ({}ms)", method, now_ms() - t0);
            return err;
        }

        if (is_notification) return std::nullopt;

        json resp = {
            {"jsonrpc", "2.0"},
            {"id", req["id"]},
            {"result", std::move(result)},
        };
        LOG_DEBUG("{} -> {}ms", method, now_ms() - t0);
        return resp;
    } catch (const json::exception& e) {
        LOG_ERROR("json error in {}: {}", method, e.what());
        if (is_notification) return std::nullopt;
        json err = error(-32602, std::string("Invalid params: ") + e.what());
        err["id"] = req.value("id", json(nullptr));
        return err;
    } catch (const std::exception& e) {
        LOG_ERROR("exception in {}: {}", method, e.what());
        if (is_notification) return std::nullopt;
        json err = error(-32603, std::string("Internal error: ") + e.what());
        err["id"] = req.value("id", json(nullptr));
        return err;
    }
}

json TerminalMCPServer::handle_initialize(const json& params) {
    std::string client_version = params.value("protocolVersion", "");
    LOG_INFO("initialize from {} {} (protocolVersion={})",
             params.value("clientInfo", json::object()).value("name", "?"),
             params.value("clientInfo", json::object()).value("version", "?"),
             client_version);

    // 版本协商：客户端发的版本支持则回同版，否则回本项目支持的最新版
    std::string negotiated = client_version;
    bool known = std::find(kSupportedVersions.begin(), kSupportedVersions.end(), client_version) != kSupportedVersions.end();
    if (!known) negotiated = kProtocolVersion;

    return {
        {"protocolVersion", negotiated},
        {"capabilities", {{"tools", {{"listChanged", false}}}}},
        {"serverInfo", {{"name", "shell-mcp-server"}, {"title", "Shell MCP Server (C++)"}, {"version", "0.1.0"}}},
    };
}

json TerminalMCPServer::handle_list_tools(const json& params) {
    (void)params;
    return {{"tools", tools_}};
}

json TerminalMCPServer::handle_call_tool(const json& params, ProgressCb progress) {
    std::string name = params.value("name", "");
    json args = params.value("arguments", json::object());

    if (name == "execute_command") {
        return tool_execute_command(args, progress);
    }
    if (name == "get_tools") {
        return tool_get_tools(args, progress);
    }
    throw std::runtime_error("Unknown tool: " + name);
}

json TerminalMCPServer::handle_ping() {
    return json::object();  // 空 result {}
}

// 2026-07-28 预留：广告支持的协议版本
json TerminalMCPServer::handle_discover() {
    return {{"protocolVersions", kSupportedVersions}};
}

json TerminalMCPServer::tool_execute_command(const json& args, ProgressCb progress) {
    if (!args.contains("command") || !args["command"].is_string()) {
        return {{"content", json::array({{
            {"type", "text"},
            {"text", "Missing required parameter: command"},
        }})}, {"isError", true}};
    }

    std::string command = args["command"].get<std::string>();
    int64_t timeout_ms = args.value("timeout_ms", kDefaultTimeoutMs);
    std::string cwd = args.value("cwd", "");
    std::vector<std::string> env;
    if (args.contains("env") && args["env"].is_object()) {
        for (auto& [k, v] : args["env"].items()) {
            env.push_back(k + "=" + v.get<std::string>());
        }
    }

    LOG_INFO("execute_command: timeout={}ms cmd={}", timeout_ms, command.substr(0, 200));

    // 阶段进度（SSE 流客户端可见; 无 progressToken 时 progress 为空, 跳过）
    auto emit = [&](double p, const std::string& msg) {
        if (progress) progress(json{{"progress", p}, {"total", 1.0}, {"message", msg}});
    };
    emit(0.0, "starting: " + command.substr(0, 80));
    ProcessResult r = run_command(command, timeout_ms, env, cwd);
    emit(1.0, r.timed_out ? "timed out, process group killed"
                          : "completed (exit " + std::to_string(r.exit_code) + ")");

    // 结果文本
    std::string text;
    if (r.timed_out) {
        text += "[TIMEOUT] command exceeded " + std::to_string(timeout_ms) + "ms, process group killed\n";
    }
    if (r.stdout_data.empty() && r.stderr_data.empty() && !r.timed_out) {
        text = "(no output)";
    } else {
        if (!r.stdout_data.empty()) text += r.stdout_data;
        if (!r.stderr_data.empty()) {
            if (!r.stdout_data.empty()) text += "\n";
            text += r.stderr_data;
        }
    }
    if (r.stdout_truncated || r.stderr_truncated) text += "\n[TRUNCATED] output exceeded limit";

    json result = {
        {"content", json::array({{
            {"type", "text"},
            {"text", text},
        }})},
        {"isError", r.exit_code != 0},
        {"structuredContent", {
            {"exitCode", r.exit_code},
            {"timedOut", r.timed_out},
            {"killed", r.killed},
            {"stdoutTruncated", r.stdout_truncated},
            {"stderrTruncated", r.stderr_truncated},
            {"durationMs", r.duration_ms},
        }},
    };
    LOG_DEBUG("execute_command done: exit={} dur={}ms out={}B err={}B",
              r.exit_code, r.duration_ms, r.stdout_data.size(), r.stderr_data.size());
    return result;
}

json TerminalMCPServer::tool_get_tools(const json& args, ProgressCb progress) {
    (void)args; (void)progress;
    json text = tools_.dump(2);
    return {
        {"content", json::array({{
            {"type", "text"},
            {"text", text},
        }})},
        {"isError", false},
    };
}

json TerminalMCPServer::error(int code, const std::string& message, json data) {
    json e = {
        {"jsonrpc", "2.0"},
        {"error", {{"code", code}, {"message", message}}},
    };
    if (!data.is_null()) e["error"]["data"] = std::move(data);
    return e;
}

} // namespace mcp
