#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mcp {

// 命令执行结果
struct ProcessResult {
    int exit_code = -1;        // 正常退出: WEXITSTATUS；超时/信号击杀: 128+sig
    std::string stdout_data;
    std::string stderr_data;
    bool timed_out = false;    // 超时被 killpg 击杀
    bool killed = false;       // 被信号击杀（非超时路径）
    bool stdout_truncated = false;
    bool stderr_truncated = false;
    int64_t duration_ms = 0;
};

// 同步执行命令（M1: 串行，无任务池）
// command: 交给 /bin/bash -c 执行
// timeout_ms: >0 看门狗超时击杀进程组；<=0 不限时
// env: KEY=VALUE 列表，execve 前覆盖环境
// cwd: 空 = 继承当前目录
// max_output: 单流截断上限（字节），超出置 truncated 标记但继续 drain 防管道阻塞
ProcessResult run_command(const std::string& command,
                          int64_t timeout_ms = 30000,
                          const std::vector<std::string>& env = {},
                          const std::string& cwd = "",
                          size_t max_output = 64 * 1024);

} // namespace mcp
