# MCP 协议实现要点（依据官方 spec 2025-06-18 / 2026-07-28）

来源：/root/github/mcp-spec（modelcontextprotocol/specification，官方）

## 1. stdio 传输（两版相同，铁律）
- 客户端以子进程启动服务端；服务端 stdin 读、stdout 写 JSON-RPC
- **帧格式：一行一个 JSON 消息，newline 分隔，消息内 MUST NOT 含内嵌换行**
- **stdout 只能写合法 MCP 消息**（响应 + 通知），严禁写请求/日志
- **stderr 随意写日志**（客户端可能转发/忽略，stderr 不代表错误）
- 客户端只写请求和通知，不写响应
- 关闭：客户端关 stdin → 服务端读 EOF 退出；服务端也可自行退出
- 帧格式与 LSP 不同：无 Content-Length 头，纯 newline-delimited

## 2. 生命周期（2025-06-18 主流版，initialize 握手）
1. 客户端发 initialize: {protocolVersion, capabilities, clientInfo}
2. 服务端回: {protocolVersion, capabilities:{tools:{},...}, serverInfo:{name,version}}
3. 客户端发 notifications/initialized
4. 版本协商：客户端发它支持的最新版；服务端支持→回同版本；
   不支持→回自己支持的最新版（客户端不支持则断开）
5. 关闭：关 stdin → EOF；SIGTERM → SIGKILL

## 3. JSON-RPC 2.0 消息
- 请求带 id；通知无 id（notifications/*）
- 响应: result 或 error{code,message,data?}
- 错误码: -32700 Parse error / -32601 Method not found(能力不支持)
          / -32602 Invalid params / -32603 Internal error

## 4. 工具
- tools/list → {tools:[{name, title?, description, inputSchema}]} 支持分页 cursor/nextCursor
- tools/call {name, arguments} → {content:[{type:text|image|audio|resource_link|resource}], isError:bool}
- 2025-06-18 新增可选 title 字段

## 5. 通知/工具方法（2025-06-18）
- ping → 立即回空 result {}（保活）
- notifications/cancelled {requestId, reason}（取消 in-flight 请求，双方可发）
- notifications/progress {progressToken, progress, total?, message?}
  → 客户端在请求 _meta.progressToken 订阅，服务端回报进度
- logging: notifications/message（服务端→客户端结构化日志，可选）

## 6. 2026-07-28 下一代（重大变化，仅预留）
- **移除 initialize/initialized/ping/logging/setLevel**，MCP 变无状态
- 每请求 _meta.protocolVersion 声明版本；不支持回 -32022 UnsupportedProtocolVersionError
  + data.supported 列表
- 新增 server/discover（服务端 MUST 实现）广告支持版本
- 结果带 resultType: "complete" | "inputRequired"（MRTR 多轮交互）
- subscriptions/listen 替代 resources/subscribe；-32002 改为 -32602

## 7. 本项目决策（shell-mcp-cpp）
- **主协议：2025-06-18**（Claude Desktop/Cursor/Cherry Studio 等主流客户端均此版本）
- 接受 initialize 的 2024-11-05 / 2025-06-18，响应回 2025-06-18
- 实现 ping / progress（长任务进度）/ notifications/cancelled 接收（映射 task_cancel）
- 日志全部 stderr；stdout 只写单行 JSON（json 序列化天然转义换行）
- 预留 server/discover 响应（若客户端发来，回支持的版本列表）——低成本高兼容
