#include "transport.h"
#include "server.h"
#include "util.h"

#include <iostream>
#include <memory>
#include <optional>
#include <string>

namespace mcp {

class StdioTransport : public Transport {
public:
    explicit StdioTransport(TerminalMCPServer& server) : server_(server) {}

    int run() override {
        LOG_INFO("stdio transport: reading JSON-RPC from stdin...");
        std::string line;
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
                std::cout << safe_dump(err) << '\n' << std::flush;
                continue;
            }

            auto resp = server_.handle_request(req);
            if (resp.has_value()) {
                // stdout 只允许协议帧；单行输出
                std::cout << safe_dump(*resp) << '\n' << std::flush;
            }
        }
        LOG_INFO("stdin closed (EOF), shutting down");
        return 0;
    }

    void shutdown() override {}

private:
    TerminalMCPServer& server_;
};

std::unique_ptr<Transport> make_stdio_transport(TerminalMCPServer& server) {
    return std::make_unique<StdioTransport>(server);
}

} // namespace mcp
