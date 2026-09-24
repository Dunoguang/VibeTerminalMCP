#include "transport.h"
#include "server.h"
#include "util.h"

#include "httplib.h"

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mcp {

namespace {
constexpr const char* kEndpoint = "/mcp";
const std::vector<std::string> kSupportedVersions = {"2025-11-25", "2025-06-18", "2024-11-05"};
constexpr int kSseHeartbeatSec = 30;  // GET 流心跳间隔（SSE 注释行）

std::string random_session_id() {
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 15);
    const char* hex = "0123456789abcdef";
    std::string id(32, '0');
    for (auto& c : id) c = hex[dist(rd)];
    return id;
}

// SSE 帧: 标准格式 event: <type> + data: <payload>
std::string sse_frame(const std::string& event_type, const std::string& data) {
    return "event: " + event_type + "\ndata: " + data + "\n\n";
}

// 老式 HTTP+SSE (2024-11-05): event: endpoint + data: <messages-url>
std::string sse_endpoint_frame(const std::string& url) {
    return "event: endpoint\ndata: " + url + "\n\n";
}

// 客户端 Accept 是否含 text/event-stream
bool wants_sse(const httplib::Request& req) {
    return req.get_header_value("Accept").find("text/event-stream") != std::string::npos;
}
} // namespace

// 单个 SSE 流: 事件队列 + 阻塞 serve（httplib provider 线程）
class SseStream : public std::enable_shared_from_this<SseStream> {
public:
    struct Event {
        std::string event_type = "message";  // SSE event 类型
        std::string data;                    // data 载荷
        bool is_response = false;            // 最终响应: 发送后关流
    };

    // 推事件（通知/响应, event: message）→ events_ 队列 (JSON-RPC 消息)
    void push(std::string data, bool is_response = false) {
        {
            std::lock_guard lock(m_);
            events_.push_back({"message", std::move(data), is_response});
        }
        cv_.notify_all();
    }

    // 推自定义事件 (connected/ping 等, 对齐原版 shell-mcp) — 预组帧入队
    void push_custom(std::string event_type, std::string data, bool is_response = false) {
        std::string frame = sse_frame(event_type, data);
        {
            std::lock_guard lock(m_);
            if (is_response) {
                events_.push_back({"message", std::move(data), true});
            } else {
                custom_frames_.push_back(std::move(frame));
            }
        }
        cv_.notify_all();
    }

    // 推送老式 endpoint 事件 (event: endpoint, 非 JSON-RPC)
    void push_endpoint(const std::string& url) {
        {
            std::lock_guard lock(m_);
            endpoints_.push_back(url);
        }
        cv_.notify_all();
    }

    void close() {
        {
            std::lock_guard lock(m_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    bool closed() {
        std::lock_guard lock(m_);
        return closed_;
    }

    // 在 httplib content provider 线程阻塞服务; 返回 false 结束响应
    // heartbeat: GET 长连接流启用（30s 注释行保活）
    bool serve(httplib::DataSink& sink, bool heartbeat) {
        // 初始注释行: 立即 flush 响应头 (httplib 缓冲, 否则客户端收不到 200)
        if (!sink.write(":\n", 2)) return false;
        // 发送顺序对齐原版: connected(custom) → endpoint → server_info(events)
        for (;;) {
            std::string custom_frame;
            {
                std::lock_guard lock(m_);
                if (!custom_frames_.empty()) {
                    custom_frame = std::move(custom_frames_.front());
                    custom_frames_.pop_front();
                }
            }
            if (!custom_frame.empty()) {
                if (!sink.write(custom_frame.data(), custom_frame.size())) return false;
                continue;
            }
            std::string ep;
            {
                std::lock_guard lock(m_);
                if (!endpoints_.empty()) {
                    ep = std::move(endpoints_.front());
                    endpoints_.pop_front();
                }
            }
            if (!ep.empty()) {
                std::string f = sse_endpoint_frame(ep);
                if (!sink.write(f.data(), f.size())) return false;
                continue;
            }
            Event evt;
            {
                std::unique_lock lock(m_);
                if (heartbeat) {
                    if (!cv_.wait_for(lock, std::chrono::seconds(kSseHeartbeatSec),
                                      [&] { return !events_.empty() || closed_; })) {
                        // 心跳: SSE 注释行 (客户端必须忽略, 非 JSON-RPC 事件会解析炸)
                        if (!sink.write(":\n", 2)) return false;
                        continue;
                    }
                } else {
                    cv_.wait(lock, [&] { return !events_.empty() || closed_; });
                }
                if (events_.empty()) {
                    if (closed_) return false;
                    continue;
                }
                evt = std::move(events_.front());
                events_.pop_front();
            }
            std::string frame = sse_frame(evt.event_type, evt.data);
            if (!sink.write(frame.data(), frame.size())) {
                LOG_WARN("sse client disconnected");
                return false;
            }
            if (evt.is_response) return false;  // 最终响应后关流
        }
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Event> events_;
    std::deque<std::string> endpoints_;   // 老式 endpoint 事件 (先发)
    std::deque<std::string> custom_frames_;  // 自定义帧队列 (connected/ping 等)
    bool closed_ = false;
};

class HttpTransport : public Transport {
public:
    HttpTransport(TerminalMCPServer& server, std::string host, int port)
        : server_(server), host_(std::move(host)), port_(port) {}

    int run() override {
        httplib::Server svr;

        auto post_handler = [this](const httplib::Request& req, httplib::Response& res) {
            handle_post(req, res);
        };
        auto get_handler = [this](const httplib::Request& req, httplib::Response& res) {
            handle_get(req, res);
        };
        // 标准端点 /mcp + 根路径兼容（部分客户端只填 host:port）
        svr.Post(kEndpoint, post_handler);
        svr.Get(kEndpoint, get_handler);
        svr.Post("/", post_handler);
        svr.Get("/", get_handler);
        // 老式 HTTP+SSE (2024-11-05) 兼容: /message (原版端点) + /messages + /sse
        svr.Post("/message", post_handler);
        svr.Get("/message", get_handler);
        svr.Post("/messages", post_handler);
        svr.Get("/messages", get_handler);
        svr.Post("/sse", post_handler);
        svr.Get("/sse", get_handler);

        svr.Options(kEndpoint, [](const httplib::Request&, httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS, DELETE");
            res.set_header("Access-Control-Allow-Headers",
                           "Content-Type, Authorization, Mcp-Session-Id, MCP-Protocol-Version, Last-Event-ID");
            res.set_header("Access-Control-Max-Age", "86400");
            res.status = 204;
        });

        svr.Delete(kEndpoint, [](const httplib::Request&, httplib::Response& res) {
            res.status = 405;
            res.set_content("Session termination not supported", "text/plain");
        });

        // 全量请求日志: 定位客户端实际请求的路径/头
        svr.set_pre_routing_handler([](const httplib::Request& req, httplib::Response&) {
            LOG_INFO("REQ {} {} session={} accept={}",
                     req.method, req.path,
                     req.get_header_value("Mcp-Session-Id"),
                     req.get_header_value("Accept"));
            return httplib::Server::HandlerResponse::Unhandled;
        });

        svr.set_exception_handler([](const httplib::Request& req, httplib::Response& res,
                                     std::exception_ptr ep) {
            try {
                if (ep) std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                LOG_ERROR("httplib exception on {} {}: {}", req.method, req.path, e.what());
            } catch (...) {
                LOG_ERROR("httplib unknown exception on {} {}", req.method, req.path);
            }
            res.status = 500;
            res.set_content("Internal Server Error", "text/plain");
        });

        svr.set_error_handler([](const httplib::Request& req, httplib::Response& res) {
            LOG_WARN("HTTP {} for {} {}", res.status, req.method, req.path);
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

    struct HttpSession {
        bool initialized = true;
        std::vector<std::weak_ptr<SseStream>> get_streams;  // GET SSE 流
    };
    std::mutex sessions_mutex_;
    std::unordered_map<std::string, HttpSession> sessions_;
    // 老式 SSE: GET 流按 endpoint sessionId 注册, POST /message?sessionId=xxx
    // 的响应同时推送到该流 (原版 shell-mcp 广播机制)
    std::unordered_map<std::string, std::weak_ptr<SseStream>> sse_by_query_session_;

    // ---------- 公共前置检查 ----------
    bool check_origin(const httplib::Request& req, httplib::Response& res) {
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
                    return false;
                }
            }
        }
        return true;
    }

    bool check_version(const httplib::Request& req, httplib::Response& res) {
        if (req.has_header("MCP-Protocol-Version")) {
            std::string pv = req.get_header_value("MCP-Protocol-Version");
            bool ok = std::find(kSupportedVersions.begin(), kSupportedVersions.end(), pv) != kSupportedVersions.end();
            if (!ok) {
                LOG_WARN("unsupported protocol version header: {}", pv);
                res.status = 400;
                res.set_content("Unsupported MCP-Protocol-Version", "text/plain");
                return false;
            }
        }
        return true;
    }

    // 会话校验: 无 session 头 → 宽松放行（对齐官方 SDK require_session_id=false）;
    // 带了但未知/已终止 → 404（规范: session 终止后 MUST 404, 客户端会重新 initialize）
    bool check_session(const httplib::Request& req, httplib::Response& res) {
        std::string sid = req.get_header_value("Mcp-Session-Id");
        if (sid.empty()) return true;  // 无 session 不强制
        std::lock_guard lock(sessions_mutex_);
        if (!sessions_.count(sid)) {
            LOG_WARN("unknown session id: {}, 404", sid);
            res.status = 404;
            res.set_content("Session not found", "text/plain");
            return false;
        }
        return true;
    }

    // 注册 GET SSE 流到会话（流关闭时自动移除）
    void register_get_stream(const std::string& sid, const std::shared_ptr<SseStream>& stream) {
        std::lock_guard lock(sessions_mutex_);
        auto& s = sessions_[sid];
        s.get_streams.erase(
            std::remove_if(s.get_streams.begin(), s.get_streams.end(),
                           [](const std::weak_ptr<SseStream>& w) { return w.expired(); }),
            s.get_streams.end());
        s.get_streams.push_back(stream);
    }

    // ---------- POST /mcp ----------
    void handle_post(const httplib::Request& req, httplib::Response& res) {
        if (!check_origin(req, res)) return;
        if (!check_version(req, res)) return;

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
            if (!check_session(req, res)) return;
        }

        // 通知: 204 No Content (对齐原版 shell-mcp)
        if (is_notification) {
            LOG_DEBUG("http notification: {} -> 204", method);
            res.status = 204;
            return;
        }

        // SSE 响应流: 客户端想要 SSE 且请求带 progressToken（订阅进度）
        std::string progress_token;
        if (req_json.contains("params") && req_json["params"].contains("_meta") &&
            req_json["params"]["_meta"].contains("progressToken")) {
            progress_token = req_json["params"]["_meta"]["progressToken"].get<std::string>();
        }
        if (wants_sse(req) && !progress_token.empty() && method != "initialize") {
            handle_post_sse(req, res, req_json, method, progress_token);
            return;
        }

        // 单 JSON 响应 + 双通道推送 (老式 SSE: 响应同时发到 GET 流)
        auto resp = server_.handle_request(req_json);
        if (method == "initialize" && resp.has_value() && !resp->contains("error")) {
            std::string sid = random_session_id();
            {
                std::lock_guard lock(sessions_mutex_);
                sessions_[sid] = HttpSession{};
            }
            res.set_header("Mcp-Session-Id", sid);
            LOG_INFO("session created: {} ({})", sid, req_json["id"].dump());
        }
        // 推送响应到 endpoint sessionId 关联的 GET 流 (对齐原版广播机制)
        if (resp.has_value()) {
            std::string qsid = req.get_header_value("Mcp-Session-Id");
            if (qsid.empty()) {
                // 从 query 取 sessionId (/message?sessionId=xxx)
                auto qpos = req.target.find("sessionId=");
                if (qpos != std::string::npos) {
                    qsid = req.target.substr(qpos + 10);
                    auto amp = qsid.find('&');
                    if (amp != std::string::npos) qsid = qsid.substr(0, amp);
                }
            }
            if (!qsid.empty()) {
                std::shared_ptr<SseStream> stream;
                {
                    std::lock_guard lock(sessions_mutex_);
                    auto it = sse_by_query_session_.find(qsid);
                    if (it != sse_by_query_session_.end()) stream = it->second.lock();
                }
                if (stream && !stream->closed()) {
                    stream->push(safe_dump(*resp));
                    LOG_DEBUG("response pushed to sse stream for query session {}", qsid);
                }
            }
        }
        res.status = 200;
        res.set_content(resp ? safe_dump(*resp) : "{}", "application/json");
    }

    // POST SSE 响应流: progress 通知(可多个) + 最终响应, 响应后关流
    void handle_post_sse(const httplib::Request& req, httplib::Response& res,
                         const json& req_json, const std::string& method,
                         const std::string& progress_token) {
        LOG_INFO("sse response stream for {} (token={})", method, progress_token);
        auto stream = std::make_shared<SseStream>();

        // 执行线程: 跑请求, 进度推流, 最终响应推流后关
        std::thread([this, req_json, stream, progress_token, method] {
            try {
                // progress 回调 → notifications/progress
                TerminalMCPServer::ProgressCb cb = [stream, progress_token](const json& p) {
                    json note = {
                        {"jsonrpc", "2.0"},
                        {"method", "notifications/progress"},
                        {"params", {
                            {"progressToken", progress_token},
                            {"progress", p.value("progress", 0.0)},
                            {"total", p.value("total", json(nullptr))},
                            {"message", p.value("message", "")},
                        }},
                    };
                    // total 为 null 时移除（规范: total 可选）
                    if (note["params"]["total"].is_null()) note["params"].erase("total");
                    stream->push(safe_dump(note));
                };
                auto resp = server_.handle_request(req_json, cb);
                if (resp.has_value()) {
                    stream->push(safe_dump(*resp), true /* is_response: 发完关流 */);
                } else {
                    stream->close();
                }
            } catch (const std::exception& e) {
                LOG_ERROR("sse exec thread error: {}", e.what());
                json err = {{"jsonrpc", "2.0"}, {"id", nullptr},
                            {"error", {{"code", -32603}, {"message", std::string("Internal error: ") + e.what()}}}};
                stream->push(safe_dump(err), true);
            }
        }).detach();

        // 会话发放（initialize 不走 SSE 分支, 无需处理）
        (void)method;

        res.set_header("Cache-Control", "no-cache");
        res.set_header("X-Accel-Buffering", "no");
        res.set_header("Connection", "keep-alive");
        res.set_content_provider(
            "text/event-stream",
            [stream](size_t, httplib::DataSink& sink) -> bool {
                return stream->serve(sink, /*heartbeat=*/false);
            });
    }

    // ---------- GET /mcp: SSE 长连接流（2025-06-18 兼容; 服务端→客户端推送） ----------
    // 铁律: 必须 Accept: text/event-stream + 有效 session, 否则快速失败——
    // 开永不结束的流会让客户端 HTTP 请求永久挂起
    void handle_get(const httplib::Request& req, httplib::Response& res) {
        if (!wants_sse(req)) {
            res.status = 405;
            res.set_content("SSE stream requires Accept: text/event-stream", "text/plain");
            return;
        }
        if (!check_origin(req, res)) return;
        std::string sid = req.get_header_value("Mcp-Session-Id");
        // 无 session: SSE 流只发 endpoint 事件 (老式握手), 其他一律不发
        // server_info/connected 等消息可能让客户端报错 (未知 id 响应/非 JSON-RPC)
        if (sid.empty()) {
            auto stream = std::make_shared<SseStream>();
            std::string qsid = random_session_id();
            stream->push_endpoint("/message?sessionId=" + qsid);
            {
                std::lock_guard lock(sessions_mutex_);
                sse_by_query_session_[qsid] = stream;
            }
            LOG_INFO("sse stream registered for query session {}", qsid);
            res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
            res.set_header("Pragma", "no-cache");
            res.set_header("X-Accel-Buffering", "no");
            res.set_content_provider(
                "text/event-stream",
                [stream](size_t, httplib::DataSink& sink) -> bool {
                    bool ok = stream->serve(sink, /*heartbeat=*/true);
                    stream->close();
                    return ok;
                });
            return;
        }
        if (!check_session(req, res)) return;

        auto stream = std::make_shared<SseStream>();
        register_get_stream(sid, stream);
        LOG_INFO("get sse stream registered for session {}", sid);

        res.set_header("Cache-Control", "no-cache");
        res.set_header("X-Accel-Buffering", "no");
        res.set_header("Connection", "keep-alive");
        res.set_content_provider(
            "text/event-stream",
            [stream](size_t, httplib::DataSink& sink) -> bool {
                bool ok = stream->serve(sink, /*heartbeat=*/true);
                stream->close();
                return ok;
            });
    }
};

std::unique_ptr<Transport> make_http_transport(TerminalMCPServer& server,
                                               const std::string& host,
                                               int port) {
    return std::make_unique<HttpTransport>(server, host, port);
}

} // namespace mcp
