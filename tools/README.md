# tools / 验证脚本

回归用的小工具，改完 `src/` 后跑一遍即可。

## 端到端自测（需要服务在跑）

```bash
python3 tools/mcp_selftest.py [port] [host]   # 默认 8001 / 127.0.0.1
```

检查项：

1. `initialize` + `tools/call` 往返正常（能拿到 session id）
2. 终端输出里的 ANSI 转义已剥离（颜色、`[?2004h` 等）
3. 非法 UTF-8 字节被替换成 `U+FFFD`，**不再触发 HTTP 500**
4. 超长输出被裁到尾部 4KB，且以 `===4KB截断===` 开头

输出 `RESULT: ALL PASS` 且退出码为 0 才算通过。

## 单元测试（不需要服务）

只测 `src/util.cpp` 里的输出清洗函数，不依赖网络：

```bash
g++ -std=c++23 -Isrc -o /tmp/test_util tools/test_util.cpp src/util.cpp
/tmp/test_util
```

覆盖：`strip_ansi`、`sanitize_utf8`（非法字节折叠 / 合法多字节保留 /
截断序列修复）、`clip_tail_utf8`（UTF-8 边界对齐）、`safe_dump`
（对照组：严格 `dump()` 必须抛异常，`safe_dump` 不能抛）。

## 注意

- 脚本会创建并销毁自己的 pty 会话，不会影响其它会话。
- 容器 `/tmp` 是 tmpfs，重启即清空 —— 脚本请从仓库目录运行。
