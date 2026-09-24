#!/usr/bin/env python3
"""End-to-end self test for VibeTerminalMCP.

Usage:
    python3 tools/mcp_selftest.py [port] [host]

Default endpoint: http://127.0.0.1:8001/mcp

Checks
  1. initialize + tools/call round trip
  2. ANSI escape sequences are stripped from terminal output
  3. invalid UTF-8 bytes come back as U+FFFD instead of HTTP 500
  4. oversized output is clipped to the last 4KB and marked with
     the ===4KB...=== prefix

Exit code 0 = all checks passed.
"""
import json
import sys
import time
import urllib.error
import urllib.request

PORT = sys.argv[1] if len(sys.argv) > 1 else "8001"
HOST = sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1"
BASE = "http://%s:%s/mcp" % (HOST, PORT)
MARKER = "===4KB" + "\u622a\u65ad" + "==="

SESSION = None
FAILS = []


def call(body, timeout=30):
    """POST one JSON-RPC message, return the parsed response (SSE aware)."""
    global SESSION
    req = urllib.request.Request(
        BASE, data=json.dumps(body).encode(), method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Accept", "application/json, text/event-stream")
    if SESSION:
        req.add_header("Mcp-Session-Id", SESSION)
    try:
        resp = urllib.request.urlopen(req, timeout=timeout)
    except urllib.error.HTTPError as e:
        print("  HTTP error %s: %s" % (e.code, e.read()[:200]))
        return None
    except Exception as e:                       # noqa: BLE001
        print("  transport error: %r" % (e,))
        return None
    sid = resp.headers.get("mcp-session-id")
    if sid:
        SESSION = sid
    raw = resp.read().decode("utf-8", "replace")
    for line in raw.splitlines():
        if line.startswith("data: "):
            return json.loads(line[6:])
    return json.loads(raw) if raw.strip() else raw


def tool(name, args, timeout=30):
    """Call one MCP tool, return its first text content."""
    res = call({"jsonrpc": "2.0", "id": 9, "method": "tools/call",
                "params": {"name": name, "arguments": args}}, timeout)
    if res is None or "result" not in res:
        print("  tool %s failed: %s" % (name, json.dumps(res)[:200]))
        return None
    return res["result"]["content"][0]["text"]


def check(name, ok, extra=""):
    print("  %-32s %s %s" % (name, "PASS" if ok else "FAIL", extra))
    if not ok:
        FAILS.append(name)


def main():
    call({"jsonrpc": "2.0", "id": 1, "method": "initialize",
          "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                     "clientInfo": {"name": "mcp-selftest", "version": "1"}}})
    check("initialize (session id)", SESSION is not None)
    if SESSION is None:
        return 1
    call({"jsonrpc": "2.0", "method": "notifications/initialized"})

    sid = tool("terminal_new",
               {"reason": "selftest", "rows": 40, "cols": 120})
    check("terminal_new", bool(sid), (sid or "")[:16])
    if not sid:
        return 1

    # --- 1. ANSI strip + invalid UTF-8 repair ---------------------------
    tool("terminal_add_cmd", {
        "session_id": sid, "auto_enter": True, "reason": "selftest-binary",
        "input": "printf 'B:\\xab\\x32\\xff:\\n'; "
                 "printf 'C:\\033[31mRED\\033[0m done\\n'"})
    out = tool("terminal_wait", {"session_id": sid, "reason": "selftest-wait",
                                 "timeout_ms": 8000})
    check("wait returns content", out is not None)
    if out is not None:
        check("no ANSI escapes left", "\x1b" not in out)
        check("invalid bytes -> U+FFFD", "\ufffd" in out)
        check("plain text preserved", "done" in out)

    # --- 2. 4KB tail clip marker ---------------------------------------
    tool("terminal_add_cmd", {
        "session_id": sid, "auto_enter": True, "reason": "selftest-fill",
        "input": "for i in $(seq 1 400); do "
                 "echo L$i-abcdefghijklmnopqrstuvwxyz; done"})
    time.sleep(3)
    big = tool("terminal_line", {"session_id": sid, "start_line": 1,
                                 "end_line": -1, "reason": "selftest-line"})
    check("oversized read returns", big is not None)
    if big is not None:
        size = len(big.encode())
        check("starts with truncation marker", big.startswith(MARKER))
        check("clipped to about 4KB", size <= 4200, "%d bytes" % size)

    tool("terminal_kill", {"session_id": sid, "reason": "selftest-kill"})
    print("")
    if FAILS:
        print("RESULT: FAIL (%d) -> %s" % (len(FAILS), ", ".join(FAILS)))
        return 1
    print("RESULT: ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
