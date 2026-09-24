#!/bin/bash
# VibeTerminalMCP 管理脚本 (start/stop/restart/status)
# 用法: ./start_mcp.sh [start|stop|restart|status]

DIR="/root/github/VibeTerminalMCP"
BIN="$DIR/build/shell-mcp-server"
PID_FILE="$DIR/server.pid"
LOG="$DIR/server.log"
HOST="127.0.0.1"
PORT="${MCP_PORT:-8001}"

[ -x "$BIN" ] || { echo "ERROR: $BIN 不存在，先 cmake --build build"; exit 1; }

start() {
    if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
        echo "已在运行 (PID $(cat "$PID_FILE"), port $PORT)"
        return 0
    fi
    nohup "$BIN" --mode http --host "$HOST" --port "$PORT" >> "$LOG" 2>&1 &
    echo $! > "$PID_FILE"
    sleep 0.5
    if kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
        echo "已启动: http://$HOST:$PORT/mcp (PID $(cat "$PID_FILE"), 日志 $LOG)"
    else
        echo "启动失败，看日志: $LOG"; exit 1
    fi
}

stop() {
    if [ -f "$PID_FILE" ]; then
        PID=$(cat "$PID_FILE")
        kill "$PID" 2>/dev/null
        for i in 1 2 3 4 5; do kill -0 "$PID" 2>/dev/null || break; sleep 0.5; done
        kill -0 "$PID" 2>/dev/null && kill -9 "$PID"
        rm -f "$PID_FILE"
        echo "已停止 (PID $PID)"
    else
        echo "未在运行"
    fi
}

status() {
    if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
        echo "运行中: PID $(cat "$PID_FILE"), http://$HOST:$PORT/mcp"
    else
        echo "未运行"
    fi
}

case "${1:-start}" in
    start)   start ;;
    stop)    stop ;;
    restart) stop; start ;;
    status)  status ;;
    *) echo "用法: $0 [start|stop|restart|status]"; exit 1 ;;
esac
