#include "transport.h"
#include "server.h"
#include "util.h"

#include "httplib.h"

#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>

namespace mcp {

namespace {
constexpr const char* kEndpoint = "/mcp";
const std::vector<std::string> kSupportedVersions = {"2025-06-18", "2024-11-05"};

std::string random_session_id() {
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 15);
    const char* hex = "0123456789abcdef";
    std::string id(32, '0');
    for (auto& c : id) c = hex[dist(rd)];
    return id;
}
} // namespace

class HttpTransport : public Transport {
public:
    HttpTransport(TerminalMCPServer& server, std::string host, int port)
        : server_(server), host_(std::move(host)), port_(port) {}

    int run() override {
        httplib::Server svr;

        // POST /mcp: 客户端发 JSON-RPC 消息
        svr.Post(kEndpoint, [this](const httplib::Request& req, httplib::Response& res) {
            handle_post(req, res);
        });

        // GET /mcp: SSE 流（M2 进度通知时启用；规范允许 405）
        svr.Get(kEndpoint, [](const httplib::Request&, httplib::Response& res) {
            res.status = 405;
            res.set_content("SSE stream not supported yet", "text/plain");
        });

        // OPTIONS /mcp: CORS 预检
        svr.Options(kEndpoint, [](const httplib::Request&, httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS, DELETE");
            res.set_header("Access-Control-Allow-Headers",
                           "Content-Type, Authorization, Mcp-Session-Id, MCP-Protocol-Version, Last-Event-ID");
            res.set_header("Access-Control-Max-Age", "86400");
            res.status = 204;
        });

        // DELETE /mcp: 客户端终止会话（规范允许 405）
        svr.Delete(kEndpoint, [](const httplib::Request&, httplib::Response& res) {
            res.status = 405;
            res.set_content("Session termination not supported", "text/plain");
        });

        svr.set_error_handler([](const httplib::Request&, httplib::Response& res) {
            if (res.status == 404) res.set_content("Not Found", "text/plain");
        });

        LOG_INFO("http transport: listening on http://{}:{}{}", host_, port_, kEndpoint);
        if (!svr.listen(host_.c_str(), port_)) {
            LOG_ERROR("failed to listen on {}:{}", host_, port_);
            return 1;
        }
        return 0;
    }

    void shutdown() override {}

private:
    TerminalMCPServer& server_;
    std::string host_;
    int port_;

    std::mutex sessions_mutex_;
    std::unordered_map<std::string, bool> sessions_;  // session_id -> initialized

    void handle_post(const httplib::Request& req, httplib::Response& res) {
        // 安全: DNS rebinding 防护——Origin 存在时校验其 host 与 Host 头一致
        if (req.has_header("Origin")) {
            std::string origin = req.get_header_value("Origin");
            std::string host = req.get_header_value("Host");
            if (!origin.empty() && origin.find("://") != std::string::npos) {
                std::string origin_host = origin.substr(origin.find("://") + 3);
                auto slash = origin_host.find('/');
                if (slash != std::string::npos) origin_host = origin_host.substr(0, slash);
                if (origin_host != host) {
                    LOG_WARN("origin mismatch: {} vs host {}", origin_host, host);
                    res.status = 403;
                    res.set_content("Origin not allowed", "text/plain");
                    return;
                }
            }
        }

        // MCP-Protocol-Version 头校验（无头默认 2025-03-26 之前的客户端，宽松接受）
        if (req.has_header("MCP-Protocol-Version")) {
            std::string pv = req.get_header_value("MCP-Protocol-Version");
            bool ok = std::find(kSupportedVersions.begin(), kSupportedVersions.end(), pv) != kSupportedVersions.end();
            if (!ok) {
                LOG_WARN("unsupported protocol version header: {}", pv);
                res.status = 400;
                res.set_content("Unsupported MCP-Protocol-Version", "text/plain");
                return;
            }
        }

        // 解析 body
        json req_json;
        try {
            req_json = json::parse(req.body);
        } catch (const json::parse_error& e) {
            LOG_WARN("http parse error: {}", e.what());
            res.status = 400;
            res.set_header("Content-Type", "application/json");
            res.set_content(json{{"jsonrpc", "2.0"}, {"id", nullptr},
                                 {"error", {{"code", -32700}, {"message", std::string("Parse error: ") + e.what()}}}}
                                .dump(), "application/json");
            return;
        }

        std::string method = req_json.value("method", "");
        bool is_notification = !req_json.contains("id");

        // 会话管理: 除 initialize 外必须有有效 Mcp-Session-Id
        if (method != "initialize") {
            std::string sid = req.get_header_value("Mcp-Session-Id");
            std::lock_guard lock(sessions_mutex_);
            if (sid.empty() || !sessions_.count(sid)) {
                LOG_WARN("no valid session for method {}", method);
                res.status = 404;  // 客户端收到 404 会重新 initialize
                res.set_content("Session not found", "text/plain");
                return;
            }
        }

        // 通知: 202 Accepted 无 body
        if (is_notification) {
            LOG_DEBUG("http notification: {} -> 202", method);
            res.status = 202;
            return;
        }

        // 请求: 处理并返回单 JSON 响应
        auto resp = server_.handle_request(req_json);

        // initialize 成功 → 发放会话
        if (method == "initialize" && resp.has_value() && !resp->contains("error")) {
            std::string sid = random_session_id();
            {
                std::lock_guard lock(sessions_mutex_);
                sessions_[sid] = true;
            }
            res.set_header("Mcp-Session-Id", sid);
            LOG_INFO("session created: {} ({})", sid, req_json["id"].dump());
        }

        res.set_header("Content-Type", "application/json");
        res.status = 200;
        res.set_content(resp ? resp->dump() : "{}", "application/json");
    }
};

std::unique_ptr<Transport> make_http_transport(TerminalMCPServer& server,
                                               const std::string& host,
                                               int port) {
    return std::make_unique<HttpTransport>(server, host, port);
}

} // namespace mcp
