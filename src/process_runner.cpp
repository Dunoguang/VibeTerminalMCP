#include "process_runner.h"
#include "util.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <optional>

namespace mcp {

extern "C" char** environ;  // 全局环境指针（C 符号，无名字修饰）

namespace {

// 从 fd 非阻塞读取，追加到 out；超过 max_output 置 truncated
void drain_fd(int fd, std::string& out, bool& truncated, size_t max_output) {
    char buf[8192];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            if (!truncated) {
                if (out.size() + static_cast<size_t>(n) <= max_output) {
                    out.append(buf, static_cast<size_t>(n));
                } else {
                    size_t room = max_output - out.size();
                    if (room > 0) out.append(buf, room);
                    truncated = true;
                }
            }
            // truncated 后继续读（drain），否则子进程写满管道会阻塞
            continue;
        }
        if (n == 0) return;  // EOF
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        if (errno == EINTR) continue;
        LOG_WARN("read fd {} error: {}", fd, strerror(errno));
        return;
    }
}

} // namespace

ProcessResult run_command(const std::string& command,
                          int64_t timeout_ms,
                          const std::vector<std::string>& env,
                          const std::string& cwd,
                          size_t max_output) {
    ProcessResult res;
    int64_t t0 = now_ms();

    int out_pipe[2], err_pipe[2];
    if (pipe2(out_pipe, O_CLOEXEC) != 0 || pipe2(err_pipe, O_CLOEXEC) != 0) {
        res.stderr_data = std::string("pipe() failed: ") + strerror(errno);
        return res;
    }

    pid_t pid = fork();
    if (pid < 0) {
        res.stderr_data = std::string("fork() failed: ") + strerror(errno);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        return res;
    }

    if (pid == 0) {
        // ---- 子进程 ----
        // 自成进程组：后代（bash 再 spawn 的）继承 pgid，超时可整组击杀
        if (setpgid(0, 0) != 0) _exit(127);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);

        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);

        // 构建 envp：environ 副本 + 覆盖
        std::vector<std::string> env_store;
        size_t n = 0;
        while (environ[n]) ++n;
        env_store.reserve(n + env.size() + 1);
        for (size_t i = 0; i < n; ++i) env_store.emplace_back(environ[i]);
        // 去重：新 env 覆盖同名旧变量
        for (const auto& kv : env) {
            auto eq = kv.find('=');
            std::string key = (eq == std::string::npos) ? kv : kv.substr(0, eq);
            env_store.erase(std::remove_if(env_store.begin(), env_store.end(),
                [&](const std::string& s) {
                    return s.compare(0, key.size(), key) == 0 &&
                           s.size() > key.size() && s[key.size()] == '=';
                }), env_store.end());
            env_store.push_back(kv);
        }
        std::vector<char*> envp;
        envp.reserve(env_store.size() + 1);
        for (auto& s : env_store) envp.push_back(s.data());
        envp.push_back(nullptr);

        std::string cmd_copy = command;  // execve argv 需要非 const char*
        const char* bash_argv[] = {"bash", "-c", cmd_copy.c_str(), nullptr};
        execve("/bin/bash", const_cast<char* const*>(bash_argv), envp.data());
        _exit(127);  // exec 失败
    }

    // ---- 父进程 ----
    close(out_pipe[1]); close(err_pipe[1]);
    // 父进程也把子进程加入自己的 pgid 管理视角（setpgid 竞态防护：子进程可能还没执行 setpgid）
    setpgid(pid, pid);

    // 非阻塞读
    fcntl(out_pipe[0], F_SETFL, fcntl(out_pipe[0], F_GETFL) | O_NONBLOCK);
    fcntl(err_pipe[0], F_SETFL, fcntl(err_pipe[0], F_GETFL) | O_NONBLOCK);

    int64_t deadline = (timeout_ms > 0) ? t0 + timeout_ms : 0;
    bool out_eof = false, err_eof = false;
    int status = 0;
    bool reaped = false;

    while (!out_eof || !err_eof) {
        // 看门狗检查
        if (deadline > 0) {
            int64_t remain = deadline - now_ms();
            if (remain <= 0) {
                res.timed_out = true;
                LOG_INFO("timeout {}ms, killpg({}) for cmd: {}", timeout_ms, pid, command.substr(0, 120));
                killpg(pid, SIGKILL);
                // 击杀后继续 drain 直到 EOF，然后收尸
            }
        }

        struct pollfd fds[2] = {
            {out_pipe[0], POLLIN | POLLHUP, 0},
            {err_pipe[0], POLLIN | POLLHUP, 0},
        };
        int timeout = 100;  // 100ms 轮询，用于看门狗检查
        int pr = poll(fds, 2, timeout);
        if (pr < 0) {
            if (errno == EINTR) continue;
            LOG_WARN("poll failed: {}", strerror(errno));
            break;
        }
        if (pr > 0) {
            if (fds[0].revents & (POLLIN | POLLHUP)) {
                drain_fd(out_pipe[0], res.stdout_data, res.stdout_truncated, max_output);
                if (fds[0].revents & POLLHUP) { out_eof = true; }
                else {
                    // POLLIN 读完可能已 EOF，再查一次
                    char c;
                    if (read(out_pipe[0], &c, 0) == 0 && errno == 0) {}
                    // 简化：POLLHUP 之后自然收尾
                }
            }
            if (fds[1].revents & (POLLIN | POLLHUP)) {
                drain_fd(err_pipe[0], res.stderr_data, res.stderr_truncated, max_output);
                if (fds[1].revents & POLLHUP) { err_eof = true; }
            }
        }

        // 收尸：非阻塞检查
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            reaped = true;
            if (WIFEXITED(status)) {
                res.exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                int sig = WTERMSIG(status);
                res.exit_code = 128 + sig;
                if (!res.timed_out) res.killed = true;
            }
            // 已收尸，但管道可能还有数据（子进程的后代还在写）——继续循环直到 EOF
            if (out_eof && err_eof) break;
        }
    }

    // 循环退出后兜底收尸
    if (!reaped) {
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) res.exit_code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            res.exit_code = 128 + sig;
            if (!res.timed_out) res.killed = true;
        }
    }

    close(out_pipe[0]); close(err_pipe[0]);
    res.duration_ms = now_ms() - t0;
    return res;
}

} // namespace mcp
