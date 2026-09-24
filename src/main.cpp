#include "server.h"
#include "transport.h"
#include "util.h"

#include <iostream>
#include <memory>
#include <string>

using namespace mcp;

int main(int argc, char* argv[]) {
    init_logging();

    std::string mode = "stdio";
    std::string host = "127.0.0.1";
    int port = 8000;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            return (i + 1 < argc) ? std::string(argv[++i]) : std::string();
        };
        if (arg == "--mode" || arg == "-m") mode = next();
        else if (arg == "--host") host = next();
        else if (arg == "--port" || arg == "-p") port = std::stoi(next());
        else if (arg == "--audit-log") set_audit_path(next());
        else if (arg == "--help" || arg == "-h") {
            std::cerr << "Usage: vibeterminalmcp-server [--mode stdio|http] "
                         "[--host HOST] [--port PORT] [--audit-log PATH]\n"
                         "  --audit-log  audit log path "
                         "(default: <exe dir>/audit.log; env "
                         "SHELL_MCP_AUDIT_LOG also honored)\n";
            return 0;
        }
    }

    TerminalMCPServer server;

    std::unique_ptr<Transport> transport;
    if (mode == "http" || mode == "sse") {
        transport = make_http_transport(server, host, port);
    } else {
        transport = make_stdio_transport(server);
    }

    LOG_INFO("vibeterminalmcp-server (C++) starting, mode={} protocol=2025-06-18", mode);
    LOG_INFO("audit log: {}", audit_path());
    return transport->run();
}
