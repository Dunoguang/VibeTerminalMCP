# VibeTerminalMCP

A tiny C++23 MCP server that hands an AI agent **persistent interactive
terminal sessions** (pty + bash) instead of one-shot shell commands.

一个极小的 C++23 MCP 服务器：给 AI agent 提供**持久化的交互式终端会话**
（pty + bash），而不是一次性的命令执行。

- Transports: stdio / Streamable HTTP (`/mcp`) / legacy SSE (`/sse`)
- Zero heavy deps: `httplib.h` (vendored) + nlohmann/json
- ~1900 lines of C++, builds in seconds on a phone
- Output hygiene built in: ANSI stripped, invalid UTF-8 repaired,
  truncation is explicit

## 特性 / Features

| | |
|---|---|
| Persistent sessions | pty 会话跨多次工具调用存活，`cd`/`export`/REPL 状态都保留 |
| 9 small tools | 见下表；没有"执行任意命令"的后门 |
| Output hygiene | 剥离 ANSI 转义、非法 UTF-8 折叠成 U+FFFD、超长输出显式标注 |
| Audit trail | 每个工具调用必须带 `reason`，写入 `audit.log` |
| No silent data loss | 截断时在正文开头插入 `===4KB截断===` |

## Tools / 工具

| Tool | Key args | Description |
|---|---|---|
| `terminal_new` | `rows` `cols` `cwd` `env` | 创建 pty 会话，返回 `session_id` |
| `terminal_add_cmd` | `session_id` `input` `auto_enter` | 向 pty 写入（`auto_enter=true` 才回车） |
| `terminal_line` | `session_id` `start_line` `end_line` | 按行号读缓冲 |
| `terminal_last` | `session_id` | 读上次读取之后的新输出 |
| `terminal_wait` | `session_id` `timeout_ms` `mode` | 等命令结束（`marker` / `prompt`），返回 exitCode |
| `terminal_timeout` | `session_id` `wait_ms` | 纯等待后读尾部输出 |
| `terminal_kill` | `session_id` | 关闭会话 |
| `terminal_list` | - | 列出会话（pid / alive / idle_ms / last_line） |
| `get_tools` | `reason` | 返回全部工具的 JSON Schema |

只用 `terminal_*` 系列工具，没有直接执行任意命令的后门；
所有写操作都先进入 pty，因此交互式程序（编辑器、REPL、
需要 tty 的工具）都能正常工作。

## Build / 构建

```bash
# 依赖：C++23 编译器 + nlohmann/json（仅头文件）
git clone https://github.com/Dunoguang/VibeTerminalMCP
cd VibeTerminalMCP
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

产物：`build/shell-mcp-server`（单文件，1.2 MB）。

## Run / 运行

stdio（给本地 MCP 客户端直接拉起）：

```bash
./build/shell-mcp-server --mode stdio
```

HTTP（Streamable HTTP + 老式 SSE）：

```bash
./build/shell-mcp-server --mode http --host 127.0.0.1 --port 8001
# endpoint: http://127.0.0.1:8001/mcp   (SSE: /sse, /message)
```

命令行参数 / CLI options：

| Flag | Default | 说明 |
|---|---|---|
| `--mode stdio\|http` | `stdio` | 传输方式 |
| `--host HOST` | `127.0.0.1` | HTTP 监听地址 |
| `--port PORT` | `8000` | HTTP 监听端口 |
| `--audit-log PATH` | `<程序所在目录>/audit.log` | 审计日志路径，不写死任何绝对路径 |

审计日志默认跟随可执行文件所在目录（通过 `/proc/self/exe` 解析，解析失败时退回当前目录），因此整个程序拿到哪儿都能跑，没有任何编译期写死的路径。

systemd（附件模式，日志 append 到 `server.log`）：

```ini
[Unit]
Description=Shell MCP Server (C++)
After=network.target

[Service]
Type=simple
User=root
WorkingDirectory=/root/github/VibeTerminalMCP
ExecStart=/root/github/VibeTerminalMCP/build/shell-mcp-server \
  --mode http --host 127.0.0.1 --port 8001
Restart=on-failure
RestartSec=2
Environment=SHELL_MCP_LOG=info
StandardOutput=append:/root/github/VibeTerminalMCP/server.log
StandardError=append:/root/github/VibeTerminalMCP/server.log

[Install]
WantedBy=multi-user.target
```

想固定审计日志位置，就在 ExecStart 末尾追加
`--audit-log /path/to/audit.log`；不加则写在程序目录。

客户端配置（通用 MCP JSON）：

```json
{
  "mcpServers": {
    "VibeTerminalMCP": {
      "type": "http",
      "url": "http://127.0.0.1:8001/mcp"
    }
  }
}
```

## Output contract / 输出契约

终端输出在离开服务器之前会经过三道处理，
让 agent 拿到的永远是**合法、干净、可判定**的文本：

1. **ANSI 剥离**：ESC 的 CSI / OSC / DCS / SOS / PM / APC
   以及 C1 CSI 全部移除，其它不可打印控制字符丢弃
   （保留 `\n` `\t` `\r`）。
2. **UTF-8 修复**：非法字节序列（含 overlong、代理区、
   截断的多字节字符）折叠成一个 `U+FFFD`，
   因此**任何二进制输出都不会再让整个调用失败**。
3. **显式截断**：单次读取最多返回 4096 字节尾部；
   发生截断时，正文开头插入 `===4KB截断===` 标记。

这三条是踩坑换来的：在修复前，只要 pty 缓冲里混进一个
非 UTF-8 字节，JSON 序列化就会抛异常，客户端只能收到
HTTP 500 与一段 SDK 堆栈，看起来像"服务挂了"。

## 会话语义 / Session semantics

- 会话存活在服务器进程内存里，**服务重启即全部丢失**
  （设计如此：不做持久化，客户端重新 `terminal_new` 即可）。
- `terminal_wait` 默认用 marker 模式：注入
  `echo __DONE_xxx__:$?` 探测命令结束并解析退出码。
- 每个工具调用都必须带 `reason` 参数，否则直接拒绝。
  审计记录写在程序所在目录的 `audit.log`（一行一次调用），
  可用 `--audit-log PATH` 或环境变量 `SHELL_MCP_AUDIT_LOG` 覆盖。

## 布局 / Layout

```
src/main.cpp            命令行解析、启动 transport
src/server.{h,cpp}      JSON-RPC 方法分发 + 9 个工具实现
src/terminal.{h,cpp}    pty 会话（forkpty / 环形缓冲 / marker 等待）
src/http_transport.cpp  Streamable HTTP (/mcp) + SSE (/sse, /message)
src/stdio_transport.cpp stdio 模式
src/util.{h,cpp}        日志、审计、输出清洗（ANSI / UTF-8 / 截断）
third_party/httplib.h   单头 HTTP 库
DESIGN.md PROTOCOL.md   设计与协议笔记
```

## 已知取舍 / Known trade-offs

- 会话不持久化：重启丢会话（按设计）。
- 只有"拉"模式：`terminal_wait` 上限 115 秒，没有流式推送。
- 日志不轮转：`audit.log` / `server.log` 只增不减，长跑请自行 logrotate。
- 没有"raw 执行"模式：命令会经过 pty 回显。

## License

MIT
