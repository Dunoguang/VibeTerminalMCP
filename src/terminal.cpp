#include "terminal.h"
#include "util.h"

#include <pty.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <random>
#include <thread>

extern "C" char** environ;  // 全局环境指针 (C 符号)

namespace mcp {

namespace {
constexpr size_t kMaxBuffer = 64 * 1024;   // 环形缓冲上限
constexpr int kPollMs = 100;                // 读线程轮询间隔
constexpr int kMaxWaitMs = 115000;          // 最大等待时间

std::string random_id() {
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 15);
    const char* hex = "0123456789abcdef";
    std::string id(16, '0');
    for (auto& c : id) c = hex[dist(rd)];
    return id;
}

std::string random_str() {
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 35);
    const char* chars = "0123456789abcdefghijklmnopqrstuvwxyz";
    std::string s(8, '0');
    for (auto& c : s) c = chars[dist(rd)];
    return s;
}

} // namespace

// ---------- TerminalSession ----------
TerminalSession::TerminalSession(std::string id, int rows, int cols,
                                  std::string cwd, std::vector<std::string> env)
    : id_(std::move(id)), rows_(rows), cols_(cols),
      cwd_(std::move(cwd)), env_(std::move(env)) {}

TerminalSession::~TerminalSession() { close(); }

bool TerminalSession::start() {
    // 显式构建交互式 termios (无 tty 的父进程下 forkpty 默认值不可靠, ISIG 可能没开)
    struct termios tio{};
    tio.c_iflag = ICRNL | IXON | IUTF8 | BRKINT | ISTRIP;
    tio.c_oflag = OPOST | ONLCR;
    tio.c_cflag = B38400 | CS8 | CREAD;
    tio.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN;
    tio.c_cc[VINTR] = 0x03;    // ^C
    tio.c_cc[VQUIT] = 0x1c;    // ^\
    tio.c_cc[VERASE] = 0x7f;   // DEL
    tio.c_cc[VKILL] = 0x15;    // ^U
    tio.c_cc[VEOF] = 0x04;     // ^D
    tio.c_cc[VSTART] = 0x11;   // ^Q
    tio.c_cc[VSTOP] = 0x13;    // ^S
    struct winsize ws = {.ws_row = (unsigned short)rows_,
                         .ws_col = (unsigned short)cols_};
    int master;
    pid_t pid = forkpty(&master, nullptr, &tio, &ws);
    if (pid < 0) {
        LOG_ERROR("forkpty failed: {}", strerror(errno));
        return false;
    }
    if (pid == 0) {
        // 子进程
        if (!cwd_.empty() && chdir(cwd_.c_str()) != 0) {
            // chdir 失败也继续
        }
        // 构建 envp (environ 副本 + 覆盖 + TERM)
        std::vector<std::string> env_store;
        for (size_t i = 0; environ[i]; ++i) env_store.emplace_back(environ[i]);
        // 去重并覆盖
        auto upsert = [&](const std::string& kv) {
            auto eq = kv.find('=');
            std::string key = (eq == std::string::npos) ? kv : kv.substr(0, eq);
            env_store.erase(std::remove_if(env_store.begin(), env_store.end(),
                [&](const std::string& s) {
                    return s.compare(0, key.size(), key) == 0 &&
                           s.size() > key.size() && s[key.size()] == '=';
                }), env_store.end());
            env_store.push_back(kv);
        };
        if (getenv("TERM")) upsert(std::string("TERM=") + getenv("TERM"));
        else upsert("TERM=xterm-256color");
        for (const auto& kv : env_) upsert(kv);
        std::vector<char*> envp;
        for (auto& s : env_store) envp.push_back(s.data());
        envp.push_back(nullptr);

        // 重置信号处置: exec 会保留 SIG_IGN (nohup 启动的服务器继承了 SIGINT=IGN),
        // 不重置则 bash/sleep 全部忽略 SIGINT, Ctrl+C 永远无效
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGHUP, SIG_DFL);

        // bash 交互模式 (需要 pty)
        const char* argv[] = {"bash", "--noprofile", "--norc", "-i", nullptr};
        execve("/bin/bash", const_cast<char* const*>(argv), envp.data());
        _exit(127);
    }

    master_fd_ = master;
    pid_ = pid;
    alive_ = true;
    last_activity_ = now_ms();

    // 读线程
    reader_ = std::thread(&TerminalSession::reader_loop, this);

    LOG_INFO("terminal {} created (pid={}, {}x{})", id_, pid, rows_, cols_);
    return true;
}

void TerminalSession::reader_loop() {
    while (alive_.load()) {
        struct pollfd fd = {master_fd_, POLLIN, 0};
        int pr = poll(&fd, 1, kPollMs);
        if (pr < 0) {
            if (errno != EINTR) break;
            continue;
        }
        if (pr > 0 && (fd.revents & (POLLIN | POLLHUP))) {
            char buf[8192];
            ssize_t n = read(master_fd_, buf, sizeof(buf));
            if (n > 0) {
                append_output(buf, (size_t)n);
                last_activity_ = now_ms();
            } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                // EOF 或致命错误 → 结束
                break;
            }
        }
    }
    alive_ = false;
    // 通知 wait 线程
    buf_cv_.notify_all();
}

void TerminalSession::append_output(const char* data, size_t n) {
    std::lock_guard lock(buf_mutex_);
    buffer_.append(data, n);
    // 裁剪缓冲: 超过 kMaxBuffer 时从头部删除到最近换行
    if (buffer_.size() > kMaxBuffer) {
        size_t excess = buffer_.size() - kMaxBuffer;
        // 找到第一个换行后的位置
        auto nl = buffer_.find('\n', excess);
        if (nl == std::string::npos) {
            // 无换行则删到超过部分
            buffer_.erase(0, excess);
        } else {
            buffer_.erase(0, nl + 1);
        }
        // 调整 last_read_pos_
        if (last_read_pos_ > buffer_.size()) last_read_pos_ = 0;
    }
    buf_cv_.notify_all();
}

void TerminalSession::write(const std::string& data) {
    if (!alive_ || master_fd_ < 0) return;
    // 控制字符映射: \x03 Ctrl+C → SIGINT, \x1a Ctrl+Z → SIGTSTP, \x1c Ctrl+\ → SIGQUIT
    // 直接发信号给前台进程组 (tty ldisc 在无控制终端服务器下不可靠)
    std::string rest;
    bool has_ctrl = false;
    for (char c : data) {
        if (c == '\x03') { signal_fg(SIGINT); has_ctrl = true; }
        else if (c == '\x1a') { signal_fg(SIGTSTP); has_ctrl = true; }
        else if (c == '\x1c') { signal_fg(SIGQUIT); has_ctrl = true; }
        else rest.push_back(c);
    }
    if (!rest.empty()) ::write(master_fd_, rest.data(), rest.size());
    if (has_ctrl) {
        // 信号后稍等, 让 bash 更新 $? (前台命令退出 + 收尸)
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    last_activity_ = now_ms();
}

std::string TerminalSession::read_all() {
    std::lock_guard lock(buf_mutex_);
    return buffer_;
}

std::string TerminalSession::read_last() {
    std::lock_guard lock(buf_mutex_);
    size_t pos = last_read_pos_;
    last_read_pos_ = buffer_.size();
    return buffer_.substr(pos);
}

std::string TerminalSession::read_lines(int a, int b) {
    std::lock_guard lock(buf_mutex_);
    if (a < 1) a = 1;
    // 第 1 行的偏移 = 0; 找到第 a 行开头
    size_t start = 0;
    int line = 1;
    for (size_t i = 0; i < buffer_.size() && line < a; ++i) {
        if (buffer_[i] == '\n') { ++line; start = i + 1; }
    }
    if (line < a) return "(no output)";
    // 找到第 b 行结尾
    size_t end = buffer_.size();
    line = a;
    for (size_t i = start; i < buffer_.size() && line <= b; ++i) {
        if (buffer_[i] == '\n') { ++line; end = i + 1; }
    }
    if (end <= start) return "(no output)";
    return buffer_.substr(start, end - start);
}

bool TerminalSession::wait_marker(int64_t timeout_ms, std::string& out, int* exit_code) {
    if (timeout_ms <= 0 || timeout_ms > kMaxWaitMs) timeout_ms = kMaxWaitMs;
    // 重试机制: marker 可能在命令中断瞬间被 bash 吃掉, 最多 3 次尝试
    constexpr int kMaxAttempts = 3;
    int64_t per_attempt = timeout_ms / kMaxAttempts;
    if (per_attempt < 1000) per_attempt = timeout_ms;  // 短超时不细分
    bool found = false;
    int ec = -1;

    for (int attempt = 0; attempt < kMaxAttempts && !found; ++attempt) {
        std::string marker = "__DONE_" + random_str() + "__";
        std::string marker_exit = marker + ":";  // 检测 marker: 后跟数字

        // 发送 marker 命令 (先 \u0015 Ctrl+U 清残留输入行, 防 ^C 后残留文本吞掉 \n)
        // 注意: "\x15echo" 会被解析为 \x15e (十六进制转义吞 e), 必须用 \u0015
        write("\u0015" + std::string("echo ") + marker + ":$?\n");

        size_t init_len;
        {
            std::lock_guard lock(buf_mutex_);
            init_len = buffer_.size();
        }
        int64_t deadline = now_ms() + per_attempt;

        while (now_ms() < deadline && alive_) {
            std::string buf;
            {
                std::unique_lock lock(buf_mutex_);
                // 无论是否有新输出都检测 (wait_for 只是节流); 修复: 输出稳定时
                // wait_for 超时 continue 导致检测永远不执行
                buf_cv_.wait_for(lock, std::chrono::milliseconds(200),
                                 [&] { return buffer_.size() != init_len || !alive_; });
                buf = buffer_;
            }
            // 在 buf 中查找 marker_exit + 数字 (从 init_len 之后)
            auto pos = buf.find(marker_exit, init_len);

            if (pos != std::string::npos) {
                // 提取退出码: marker_exit 后的数字
                std::string after = buf.substr(pos + marker_exit.size());
                try {
                    size_t epos = 0;
                    ec = std::stoi(after, &epos);
                    if (epos > 0) found = true;
                } catch (...) {
                    // 不是数字, 继续等
                }
            }
            if (found) break;
            init_len = buf.size();  // 更新检测点
        }
    }

    // 收集新输出
    {
        std::lock_guard lock(buf_mutex_);
        out = buffer_.substr((buffer_.size() > 4096) ? buffer_.size() - 4096 : 0);
        // 截断到 4KB
        if (out.size() > 4096) out = out.substr(out.size() - 4096);
    }
    if (exit_code) *exit_code = ec;
    return found;
}

bool TerminalSession::wait_prompt(int64_t timeout_ms, std::string& out) {
    // 简单 prompt 检测: 输出尾部是否有以 $ # > ❯ 结尾的行
    if (timeout_ms <= 0 || timeout_ms > kMaxWaitMs) timeout_ms = kMaxWaitMs;
    size_t init_len;
    {
        std::lock_guard lock(buf_mutex_);
        init_len = buffer_.size();
    }
    int64_t deadline = now_ms() + timeout_ms;
    bool found = false;

    while (now_ms() < deadline && alive_) {
        std::string buf;
        {
            std::unique_lock lock(buf_mutex_);
            buf_cv_.wait_for(lock, std::chrono::milliseconds(200),
                             [&] { return buffer_.size() != init_len || !alive_; });
            buf = buffer_;
        }
        // 取最后 1024 字节找提示符 (先剥离 ANSI, 防转义序列干扰尾部字符检测)
        std::string tail = strip_ansi(buf.substr(buf.size() < 1024 ? 0 : buf.size() - 1024));
        auto nl = tail.rfind('\n');
        std::string last_line = (nl == std::string::npos) ? tail : tail.substr(nl + 1);
        // 常见提示符结尾: $ # > ❯ % (trim 后)
        last_line.erase(last_line.find_last_not_of(" \t\r\n") + 1);
        if (!last_line.empty()) {
            char lastc = last_line.back();
            if (lastc == '$' || lastc == '#' || lastc == '>' || lastc == ':' || lastc == '%') {
                found = true;
                break;
            }
        }
    }
    {
        std::lock_guard lock(buf_mutex_);
        out = buffer_.substr(buffer_.size() < 4096 ? 0 : buffer_.size() - 4096);
        if (out.size() > 4096) out = out.substr(out.size() - 4096);
    }
    return found;
}

bool TerminalSession::signal_fg(int sig) {
    // 从 /proc/<bash_pid>/stat 读前台进程组 tpgid (字段 8, bash 持有控制终端)
    std::ifstream f("/proc/" + std::to_string(pid_) + "/stat");
    std::string line;
    if (!std::getline(f, line)) {
        LOG_WARN("signal_fg: cannot read /proc/{}/stat", pid_);
        return false;
    }
    auto rp = line.rfind(')');
    if (rp == std::string::npos) return false;
    std::istringstream ss(line.substr(rp + 1));
    char state;
    int ppid, pgrp, session, tty_nr, tpgid;
    if (!(ss >> state >> ppid >> pgrp >> session >> tty_nr >> tpgid)) return false;
    if (tpgid <= 0) {
        LOG_WARN("signal_fg: no foreground process group (tpgid={})", tpgid);
        return false;
    }
    LOG_INFO("signal_fg: SIG{} to pgid {} (bash pid {})", sig, tpgid, pid_);
    if (kill(-tpgid, sig) != 0) {
        LOG_WARN("signal_fg: kill(-{}, {}) failed: {}", tpgid, sig, strerror(errno));
        return false;
    }
    return true;
}

void TerminalSession::resize(int rows, int cols) {
    if (master_fd_ < 0) return;
    struct winsize ws = {.ws_row = (unsigned short)rows,
                         .ws_col = (unsigned short)cols};
    ioctl(master_fd_, TIOCSWINSZ, &ws);
    rows_ = rows; cols_ = cols;
}

void TerminalSession::close() {
    alive_ = false;
    buf_cv_.notify_all();
    if (master_fd_ >= 0) {
        ::write(master_fd_, "exit\n", 5);
        // 给 bash 一点时间优雅退出
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ::close(master_fd_);
        master_fd_ = -1;
    }
    if (pid_ > 0) {
        kill(pid_, SIGTERM);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        waitpid(pid_, nullptr, WNOHANG);
        pid_ = -1;
    }
    if (reader_.joinable()) reader_.join();
    LOG_INFO("terminal {} closed", id_);
}

// ---------- TerminalManager ----------
std::string TerminalManager::create(int rows, int cols, const std::string& cwd,
                                     const std::vector<std::string>& env) {
    auto id = random_id();
    auto session = std::make_shared<TerminalSession>(id, rows, cols, cwd, env);
    if (!session->start()) return "";
    std::lock_guard lock(m_);
    sessions_[id] = session;
    return id;
}

std::shared_ptr<TerminalSession> TerminalManager::get(const std::string& id) {
    std::lock_guard lock(m_);
    auto it = sessions_.find(id);
    if (it != sessions_.end()) return it->second;
    return nullptr;
}

void TerminalManager::kill(const std::string& id) {
    std::shared_ptr<TerminalSession> session;
    {
        std::lock_guard lock(m_);
        auto it = sessions_.find(id);
        if (it != sessions_.end()) {
            session = it->second;
            sessions_.erase(it);
        }
    }
    if (session) session->close();
}

std::vector<std::shared_ptr<TerminalSession>> TerminalManager::list() {
    std::lock_guard lock(m_);
    std::vector<std::shared_ptr<TerminalSession>> ret;
    for (auto& [id, s] : sessions_) ret.push_back(s);
    return ret;
}

void TerminalManager::cleanup_idle(int64_t idle_ms) {
    auto now = now_ms();
    std::lock_guard lock(m_);
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (now - it->second->last_activity_ms() > idle_ms) {
            it->second->close();
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace mcp
