#pragma once
#include <memory>
#include <string>

namespace mcp {

class TerminalMCPServer;

// 传输抽象：stdio / streamable-http 共享协议层
class Transport {
public:
    virtual ~Transport() = default;
    // 阻塞运行，返回进程退出码
    virtual int run() = 0;
    virtual void shutdown() = 0;
};

std::unique_ptr<Transport> make_stdio_transport(TerminalMCPServer& server);
std::unique_ptr<Transport> make_http_transport(TerminalMCPServer& server,
                                               const std::string& host,
                                               int port);

} // namespace mcp
