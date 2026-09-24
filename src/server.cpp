#include "server.h"
#include "terminal.h"
#include "util.h"

#include <utility>
#include <optional>
#include <thread>
#include <chrono>
#include <string>

namespace mcp {

namespace {
// 本项目支持的协议版本
constexpr const char* kProtocolVersion = "2025-06-18";
const std::vector<std::string> kSupportedVersions = {"2025-11-25", "2025-06-18", "2024-11-05"};

// max bytes of one terminal read returned to the client
constexpr std::size_t kMaxOutputBytes = 4096;
// truncation marker = "===4KB" + U+622A U+65AD + "===" + LF
const std::string kTruncMarker = "===4KB\xe6\x88\xaa\xe6\x96\xad===\n";

// sanitize UTF-8/ANSI, then clip tail and mark truncation
std::string finalize_output(std::string out) {
    out = sanitize_output(std::move(out));
    if (clip_tail_utf8(out, kMaxOutputBytes)) {
        out.insert(0, kTruncMarker);
    }
    return out;
}



json tool_schema_terminal_new() {
    return {
        {"name", "terminal_new"},
        {"title", "New Terminal"},
        {"description", "创建持久终端会话 (pty + bash 交互)。返回 session_id。可指定尺寸/工作目录/环境变量。"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"rows", {{"type", "integer"}, {"default", 24}, {"description", "pty 行数"}}},
                {"cols", {{"type", "integer"}, {"default", 80}, {"description", "pty 列数"}}},
                {"cwd", {{"type", "string"}, {"description", "工作目录 (默认继承)"}}},
                {"env", {{"type", "object"}, {"description", "环境变量覆盖 (默认 TERM=xterm-256color)"}}},
                {"reason", {{"type", "string"}, {"description", "操作理由（必填，用于审计）"}}},
            }},
            {"required", {"reason"}},
        }},
    };
}

json tool_schema_terminal_add_cmd() {
    return {
        {"name", "terminal_add_cmd"},
        {"title", "Terminal Add Command"},
        {"description", "向终端会话输入字符串 (支持控制字符, 不自动回车; auto_enter=true 时追加 \n)"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"session_id", {{"type", "string"}, {"description", "终端会话 ID"}}},
                {"input", {{"type", "string"}, {"description", "要输入的字符串 (支持 \u001b[...] 等控制字符)"}}},
                {"auto_enter", {{"type", "boolean"}, {"default", false}, {"description", "自动追加回车"}}},
                {"reason", {{"type", "string"}, {"description", "操作理由（必填，用于审计）"}}},
            }},
            {"required", {"session_id", "input", "reason"}},
        }},
    };
}

json tool_schema_terminal_line() {
    return {
        {"name", "terminal_line"},
        {"title", "Terminal Lines"},
        {"description", "获取终端输出第 a-b 行内容 (1-based, 最多 4KB)"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"session_id", {{"type", "string"}, {"description", "终端会话 ID"}}},
                {"start_line", {{"type", "integer"}, {"default", 1}, {"description", "起始行 (1-based)"}}},
                {"end_line", {{"type", "integer"}, {"default", -1}, {"description", "结束行 (-1=到最后)"}}},
                {"reason", {{"type", "string"}, {"description", "操作理由（必填，用于审计）"}}},
            }},
            {"required", {"session_id", "reason"}},
        }},
    };
}

json tool_schema_terminal_last() {
    return {
        {"name", "terminal_last"},
        {"title", "Terminal Last Output"},
        {"description", "获取终端自上次查询以来的新输出 (最多 4KB)"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"session_id", {{"type", "string"}, {"description", "终端会话 ID"}}},
                {"reason", {{"type", "string"}, {"description", "操作理由（必填，用于审计）"}}},
            }},
            {"required", {"session_id", "reason"}},
        }},
    };
}

json tool_schema_terminal_wait() {
    return {
        {"name", "terminal_wait"},
        {"title", "Terminal Wait"},
        {"description", "等待终端命令执行完 (marker 模式: 发 echo 标记等退出码; prompt 模式: 检测提示符)。最多 115s"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"session_id", {{"type", "string"}, {"description", "终端会话 ID"}}},
                {"timeout_ms", {{"type", "integer"}, {"default", 115000}, {"description", "最大等待 (最多 115000)"}}},
                {"mode", {{"type", "string"}, {"enum", {"marker", "prompt"}}, {"default", "marker"}, {"description", "完成检测模式"}}},
                {"reason", {{"type", "string"}, {"description", "操作理由（必填，用于审计）"}}},
            }},
            {"required", {"session_id", "reason"}},
        }},
    };
}

json tool_schema_terminal_timeout() {
    return {
        {"name", "terminal_timeout"},
        {"title", "Terminal Timeout"},
        {"description", "等待终端 n 秒后返回当前输出 (不判断完成状态, 最多 115s)"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"session_id", {{"type", "string"}, {"description", "终端会话 ID"}}},
                {"wait_ms", {{"type", "integer"}, {"default", 5000}, {"description", "等待毫秒 (最多 115000)"}}},
                {"reason", {{"type", "string"}, {"description", "操作理由（必填，用于审计）"}}},
            }},
            {"required", {"session_id", "reason"}},
        }},
    };
}

json tool_schema_terminal_kill() {
    return {
        {"name", "terminal_kill"},
        {"title", "Kill Terminal"},
        {"description", "关闭终端会话"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"session_id", {{"type", "string"}, {"description", "终端会话 ID"}}},
                {"reason", {{"type", "string"}, {"description", "操作理由（必填，用于审计）"}}},
            }},
            {"required", {"session_id", "reason"}},
        }},
    };
}

json tool_schema_terminal_list() {
    return {
        {"name", "terminal_list"},
        {"title", "List Terminals"},
        {"description", "列出终端会话 (ID/存活时间/最后一行)"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"reason", {{"type", "string"}, {"description", "操作理由（必填，用于审计）"}}},
            }},
            {"required", {"reason"}},
        }},
    };
}

json tool_schema_terminal_resize() {
    return {
        {"name", "terminal_resize"},
        {"title", "Resize Terminal"},
        {"description", "调整终端会话 pty 尺寸"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"session_id", {{"type", "string"}, {"description", "终端会话 ID"}}},
                {"rows", {{"type", "integer"}, {"description", "行数"}}},
                {"cols", {{"type", "integer"}, {"description", "列数"}}},
                {"reason", {{"type", "string"}, {"description", "操作理由（必填，用于审计）"}}},
            }},
            {"required", {"session_id", "rows", "cols", "reason"}},
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
    tools_.push_back(tool_schema_terminal_new());
    tools_.push_back(tool_schema_terminal_add_cmd());
    tools_.push_back(tool_schema_terminal_line());
    tools_.push_back(tool_schema_terminal_last());
    tools_.push_back(tool_schema_terminal_wait());
    tools_.push_back(tool_schema_terminal_timeout());
    tools_.push_back(tool_schema_terminal_kill());
    tools_.push_back(tool_schema_terminal_list());
    tools_.push_back(tool_schema_terminal_resize());
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
        {"serverInfo", {{"name", "vibeterminalmcp"}, {"title", "VibeTerminalMCP (C++)"}, {"version", "0.1.0"}}},
    };
}

json TerminalMCPServer::handle_list_tools(const json& params) {
    (void)params;
    return {{"tools", tools_}};
}

json TerminalMCPServer::handle_call_tool(const json& params, ProgressCb progress) {
    std::string name = params.value("name", "");
    json args = params.value("arguments", json::object());

    // 审计：检查 reason（所有工具调用必填）
    std::string reason = args.value("reason", "");
    if (reason.empty()) {
        audit_log(name, "MISSING_REASON", args, "rejected");
        json err = {{"content", json::array({{{"type", "text"}, {"text", "审计拒绝: 必须提供 reason 参数（操作理由）"}}})}, {"isError", true}};
        return err;
    }

    // 记录审计日志（执行前）
    audit_log(name, reason, args, "executing");

    if (name == "terminal_new") return tool_terminal_new(args);
    if (name == "terminal_add_cmd") return tool_terminal_add_cmd(args);
    if (name == "terminal_line") return tool_terminal_line(args);
    if (name == "terminal_last") return tool_terminal_last(args);
    if (name == "terminal_wait") return tool_terminal_wait(args);
    if (name == "terminal_timeout") return tool_terminal_timeout(args);
    if (name == "terminal_kill") return tool_terminal_kill(args);
    if (name == "terminal_list") return tool_terminal_list(args);
    if (name == "terminal_resize") return tool_terminal_resize(args);
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


json TerminalMCPServer::tool_terminal_new(const json& args) {
    int rows = args.value("rows", 24);
    int cols = args.value("cols", 80);
    std::string cwd = args.value("cwd", "");
    std::vector<std::string> env;
    if (args.contains("env") && args["env"].is_object()) {
        for (auto& [k, v] : args["env"].items()) env.push_back(k + "=" + v.get<std::string>());
    }
    std::string sid = terminal_manager_.create(rows, cols, cwd, env);
    if (sid.empty()) {
        return {{"content", json::array({{{"type", "text"}, {"text", "Failed to create terminal"}}})},
                {"isError", true}};
    }
    LOG_INFO("terminal_new: {} ({}x{})", sid, rows, cols);
    std::string reason = args.value("reason", "");
    audit_log("terminal_new", reason, args, sid.empty() ? "failed" : "ok");
    return {{"content", json::array({{{"type", "text"}, {"text", sid}}})}, {"isError", sid.empty()}};
}

json TerminalMCPServer::tool_terminal_add_cmd(const json& args) {
    auto session = terminal_manager_.get(args.value("session_id", ""));
    if (!session) {
        return {{"content", json::array({{{"type", "text"}, {"text", "Session not found"}}})}, {"isError", true}};
    }
    std::string input = args.value("input", "");
    bool auto_enter = args.value("auto_enter", false);
    if (auto_enter) input += "\n";
    session->write(input);
    std::string reason = args.value("reason", "");
    audit_log("terminal_add_cmd", reason, args, "ok");
    return {{"content", json::array({{{"type", "text"}, {"text", "ok"}}})}, {"isError", false}};
}

json TerminalMCPServer::tool_terminal_line(const json& args) {
    auto session = terminal_manager_.get(args.value("session_id", ""));
    if (!session) {
        return {{"content", json::array({{{"type", "text"}, {"text", "Session not found"}}})}, {"isError", true}};
    }
    int a = args.value("start_line", 1);
    int b = args.value("end_line", -1);
    std::string out;
    if (b < a) {
        out = session->read_all();
    } else {
        out = session->read_lines(a, b);
    }
    out = finalize_output(std::move(out));
    audit_log("terminal_line", args.value("reason", ""), args, "ok");
    return {{"content", json::array({{{"type", "text"}, {"text", out}}})}, {"isError", false}};
}

json TerminalMCPServer::tool_terminal_last(const json& args) {
    auto session = terminal_manager_.get(args.value("session_id", ""));
    if (!session) {
        return {{"content", json::array({{{"type", "text"}, {"text", "Session not found"}}})}, {"isError", true}};
    }
    std::string out = session->read_last();
    out = finalize_output(std::move(out));
    audit_log("terminal_line", args.value("reason", ""), args, "ok");
    return {{"content", json::array({{{"type", "text"}, {"text", out}}})}, {"isError", false}};
}

json TerminalMCPServer::tool_terminal_wait(const json& args) {
    auto session = terminal_manager_.get(args.value("session_id", ""));
    if (!session) {
        return {{"content", json::array({{{"type", "text"}, {"text", "Session not found"}}})}, {"isError", true}};
    }
    int64_t timeout_ms = args.value("timeout_ms", 115000);
    std::string mode = args.value("mode", "marker");
    std::string out;
    bool ok;
    int ec = -1;
    if (mode == "prompt") {
        ok = session->wait_prompt(timeout_ms, out);
    } else {
        ok = session->wait_marker(timeout_ms, out, &ec);
    }
    out = finalize_output(std::move(out));
    json result = {
        {"content", json::array({{{"type", "text"}, {"text", out}}})},
        {"isError", false},
        {"structuredContent", {
            {"ready", ok},
            {"exitCode", ec},
            {"mode", mode},
        }},
    };
    audit_log("terminal_wait", args.value("reason", ""), args, safe_dump(result));
    return result;
}

json TerminalMCPServer::tool_terminal_timeout(const json& args) {
    auto session = terminal_manager_.get(args.value("session_id", ""));
    if (!session) {
        return {{"content", json::array({{{"type", "text"}, {"text", "Session not found"}}})}, {"isError", true}};
    }
    int64_t wait_ms = args.value("wait_ms", 5000);
    if (wait_ms > 115000) wait_ms = 115000;
    std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
    std::string out = session->read_last();
    out = finalize_output(std::move(out));
    audit_log("terminal_line", args.value("reason", ""), args, "ok");
    return {{"content", json::array({{{"type", "text"}, {"text", out}}})}, {"isError", false}};
}

json TerminalMCPServer::tool_terminal_kill(const json& args) {
    std::string sid = args.value("session_id", "");
    terminal_manager_.kill(sid);
    audit_log("terminal_kill", args.value("reason", ""), args, "closed");
    return {{"content", json::array({{{"type", "text"}, {"text", "closed"}}})}, {"isError", false}};
}

json TerminalMCPServer::tool_terminal_list(const json& args) {
    (void)args;
    auto sessions = terminal_manager_.list();
    json arr = json::array();
    for (auto& s : sessions) {
        std::string all = s->read_all();
        std::string last_line;
        auto nl = all.rfind('\n');
        last_line = (nl == std::string::npos) ? all : all.substr(nl + 1);
        if (last_line.size() > 200) last_line = last_line.substr(last_line.size() - 200);
        last_line = sanitize_output(last_line);
        arr.push_back({
            {"id", s->id()},
            {"pid", s->pid()},
            {"alive", s->alive()},
            {"idle_ms", now_ms() - s->last_activity_ms()},
            {"last_line", last_line},
        });
    }
    audit_log("terminal_list", args.value("reason", ""), args, "ok");
    return {{"content", json::array({{{"type", "text"}, {"text", safe_dump(arr, 2)}}})}, {"isError", false}};
}

json TerminalMCPServer::tool_terminal_resize(const json& args) {
    auto session = terminal_manager_.get(args.value("session_id", ""));
    if (!session) {
        return {{"content", json::array({{{"type", "text"}, {"text", "Session not found"}}})}, {"isError", true}};
    }
    int rows = args.value("rows", 24);
    int cols = args.value("cols", 80);
    session->resize(rows, cols);
    audit_log("terminal_resize", args.value("reason", ""), args, "ok");
    return {{"content", json::array({{{"type", "text"}, {"text", "ok"}}})}, {"isError", false}};
}

json TerminalMCPServer::tool_get_tools(const json& args, ProgressCb progress) {
    (void)args; (void)progress;
    json text = tools_.dump(2);
    audit_log("get_tools", args.value("reason", ""), args, "ok");
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
