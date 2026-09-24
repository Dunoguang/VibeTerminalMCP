# Shell MCP Server (C++) 设计文档 v0.1

## 1. 目标
替代 /root/shell-mcp（Python 单文件版），解决三大执行引擎缺陷：
1. **超时处理能力差**：原版 communicate()/read() 无超时，命令可跑死请求
   → 进程组级超时击杀 + 看门狗
2. **无多任务管理**：请求串行、无状态、无取消
   → TaskManager 任务池 + 状态机 + 查询/取消/结果留存
3. **无会话留存**：每条命令独立进程，cwd/env 不保留
   → 持久 shell 会话（pty），cd/export 状态天然留存

## 2. 技术选型
- C++23（-std=c++23，g++ 16.1.1 / clang 22.1.8 完整支持）/ CMake ≥3.16 / nlohmann-json 3.12（系统包，无 vendoring）
- 手写 stdio JSON-RPC 2.0（MCP stdio transport），协议面小不引 SDK
- 零额外运行时依赖；SSH 接口留桩（纯本地，后续 libssh）
- 构建：CMake + make，产物单二进制 `shell-mcp-server`

## 3. 架构

```
            ┌──────────────────────────────────────────┐
            │ main.cpp  stdio 主循环                    │
            │ stdin  ── JSON-RPC 行 ──▶ Dispatcher      │
            │ stdout ◀── 响应 ──────────┘               │
            └──────────────────┬───────────────────────┘
                               ▼
            TerminalMCPServer (协议层, server.cpp)
             ├─ initialize / tools/list / tools/call / ping
             ├─ protocolVersion 协商 (2024-11-05 ↔ 2025-06-18)
             └─ 参数 JSON Schema 校验
                               ▼
            ExecutionEngine (执行引擎)
             ├─ TaskManager   任务池/状态机/结果留存/取消
             ├─ ProcessRunner fork+setpgid+killpg+双管道+poll
             ├─ SessionManager 持久 pty bash 会话
             └─ OutputBuffer  输出截断(64KB)+truncated 标记
```

## 4. 线程模型（C++23 并发）
- 主线程：stdin 阻塞 readline → 逐行 JSON-RPC → 同步处理 → stdout 响应
- 任务执行：每任务一个 std::jthread（自带 stop_token，task_cancel 直接
  request_stop() 优雅中断；超时击杀走 killpg 兜底）
- 任务线程内：fork → 子进程 setpgid → exec bash -c → 父进程 poll 双管道
  （循环检查 deadline，超时 killpg(-pgid, SIGKILL) + waitpid 收尸）
- 共享状态：全局 mutex 保护 task map / session map；SIGCHLD 不装 handler，
- 错误处理：std::expected 返回（无异常路径）；日志 std::format；
  参数校验用 concepts + 手写 JSON Schema 检查
  统一 waitpid(WNOHANG) 轮询收尸，避免信号竞态

## 5. 任务状态机
```
pending ──▶ running ──▶ done
              │  ├──▶ failed (exit≠0)
              │  ├──▶ timeout (看门狗击杀)
              │  └──▶ killed (task_cancel / SIGKILL)
              └── cancelled (排队中被取消)
```
- 结果留存：task map 内存保留（默认 100 条 LRU 淘汰），落盘后续版
- execute_command 语义：
  - timeout_ms>0：同步等待至多 timeout_ms，超时击杀并返回 partial output + status=timeout + task_id
  - timeout_ms=0：立即返回 task_id（纯异步），后台执行，task_status 查询

## 6. 进程执行细节 (ProcessRunner)
- fork() → 子进程 setpgid(0,0)（bash 再 spawn 的后代继承 pgid）→ exec /bin/bash -c
- 父进程：pipe2(O_CLOEXEC) 双管道，fcntl 非阻塞 + poll() 边读边存
- 超时：poll 循环检查 deadline；超时 killpg(-pgid, SIGKILL)；waitpid 收尸
- 输出：stdout/stderr 分开存，单流超 64KB 截断置 truncated 标记
- 环境：env 参数以 KEY=VALUE 前缀注入命令（bash -c "K=V cmd" 语义），
  cwd 用 chdir 后再 exec（避免 bash -c 里 cd 的引用问题）

## 7. 会话管理 (SessionManager)
- session_create(name) → forkpty() 起 bash --noprofile --norc（pty 行缓冲，
  避免管道块缓冲导致输出滞留；支持交互式程序）
- session_exec(session_id, cmd) → 写 stdin：`cmd; echo "__DONE_$RANDOM__:$?"`，
  读 pty 直到匹配标记行 → 返回标记前全部输出 + exit code
- 同一会话串行（per-session mutex），不同会话并行
- cd/export 等内建命令状态天然留存（同一 bash 进程）
- session_kill / session_list / session_delete；空闲 30min 自动回收（可配）
- 注意：pty 下 bash 会回显输入命令，返回前剔除回显行（标记行前第 1 行）

## 8. 工具 Schema（MCP tools/list 完整定义）
| 工具 | 参数 | 说明 |
|---|---|---|
| execute_command | command:string*, timeout_ms:int=30000, env:object, cwd:string, session:string(预留) | 同步执行，超时击杀返回 partial |
| task_status | task_id:string* | 查询任务状态/输出/退出码 |
| task_cancel | task_id:string* | 取消任务（TERM→2s→KILL） |
| task_list | — | 列出全部任务及状态 |
| session_create | name:string* | 创建持久会话，返回 session_id |
| session_exec | session_id:string*, command:string*, timeout_ms:int=60000 | 会话内执行（cwd 留存） |
| session_list | — | 列出会话与活动时间 |
| session_kill | session_id:string* | 结束会话 |

每个工具完整 JSON Schema 见 §10（实现时逐个录入）。

## 9. 目录结构
```
/root/github/VibeTerminalMCP/
├── CMakeLists.txt
├── DESIGN.md
├── README.md
├── src/
│   ├── main.cpp          # stdio 主循环
│   ├── server.h/.cpp     # 协议层 + 工具分发 + schema
│   ├── task_manager.h/.cpp
│   ├── process_runner.h/.cpp
│   ├── session.h/.cpp    # pty 持久会话
│   └── util.h/.cpp       # JSON 辅助 / 日志(stderr) / 时间
└── tests/
    ├── smoke.sh          # 冒烟：initialize→tools/list→execute
    └── test_runner.py    # 子进程/超时/会话场景测试
```

## 10. 日志规范
- 全部日志走 **stderr**（MCP stdio 规范：stdout 只允许协议帧）
- 级别：TRACE/DEBUG/INFO/WARN/ERROR，环境变量 SHELL_MCP_LOG=debug 控制
- 每请求打点：method, duration_ms, status

## 11. 里程碑
- M1 骨架：CMake + main 循环 + initialize/tools/list/ping + execute_command
  同步版（超时击杀 + 输出截断）→ 可被客户端直连
- M2 多任务：TaskManager 全套（异步/状态/取消/列表/结果留存）
- M3 会话：pty 持久 shell 全套
- M4 打磨：参数校验、日志分级、README、冒烟测试、接入现有客户端验证
- ✅ 已完成（M1.5）：Streamable HTTP 传输（2025-06-18 规范）——单端点 /mcp，
  POST 单 JSON 响应 + 202 通知 + Mcp-Session-Id 会话 + MCP-Protocol-Version 头校验
  + Origin 防 DNS rebinding + OPTIONS CORS；GET SSE 暂 405（M2 进度通知时启用）

## 12. 风险与对策
- C++23 特性仅限 g++ 16/clang 22 可用（本项目固定自家容器工具链，无兼容负担）
- pty 回显/乱序 → 标记法 + 回显剔除；备选降级纯管道（块缓冲可接受）
- bash 子进程逃逸 pgid（setsid 的程序如 nohup）→ 文档注明：timeout 语义为
  "尽力而为杀进程组"，逃逸进程后续版 cgroup 方案
- 大输出内存 → 64KB 截断默认值可配
