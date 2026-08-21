#include "server.h"
#include "util.h"

#include <iostream>
#include <optional>
#include <string>

using namespace mcp;

int main() {
    init_logging();
    TerminalMCPServer server;

    LOG_INFO("shell-mcp-server (C++) starting, protocol 2025-06-18, reading JSON-RPC from stdin...");

    std::string line;
    // MCP stdio: 一行一个 JSON-RPC 消息（newline-delimited，无 Content-Length）
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        if (line.find_first_not_of(" \t\r\n") == std::string::npos) continue;

        json req;
        try {
            req = json::parse(line);
        } catch (const json::parse_error& e) {
            LOG_WARN("parse error: {}", e.what());
            json err = {
                {"jsonrpc", "2.0"},
                {"id", nullptr},
                {"error", {{"code", -32700}, {"message", std::string("Parse error: ") + e.what()}}},
            };
            std::cout << err.dump() << '\n' << std::flush;
            continue;
        }

        std::optional<json> resp = server.handle_request(req);
        if (resp.has_value()) {
            // stdout 只允许协议帧；单行输出（消息内换行由 JSON 转义）
            std::cout << resp->dump() << '\n' << std::flush;
        }
    }

    LOG_INFO("stdin closed (EOF), shutting down");
    return 0;
}
