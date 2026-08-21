#pragma once
#include <atomic>
#include <condition_variable>
#include <thread>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mcp {

// 持久终端会话 (pty + bash 交互 shell)
// 读线程: poll pty → 环形缓冲 (保留最近 64KB); 写: pty write
// 命令完成检测: marker 模式 (echo __DONE_rand__:$?) 优先, prompt 模式备选
class TerminalSession : public std::enable_shared_from_this<TerminalSession> {
public:
    TerminalSession(std::string id, int rows, int cols,
                    std::string cwd, std::vector<std::string> env);
    ~TerminalSession();

    // forkpty + exec bash --noprofile --norc -i (TERM=xterm-256color)
    bool start();

    // 写输入到 pty (支持控制字符, 不自动回车)
    void write(const std::string& data);

    // 全部缓冲输出
    std::string read_all();
    // 自上次 read_last() 以来的新输出 (记录偏移)
    std::string read_last();
    // 按行提取 [a, b] (1-based, 基于当前缓冲)
    std::string read_lines(int a, int b);

    // marker 模式等待: 发送 "echo __DONE_<rand>__:$?", 等待输出中出现
    // "__DONE_<rand>__:<exitcode>"; 返回是否成功, out 收集新输出
    bool wait_marker(int64_t timeout_ms, std::string& out, int* exit_code);
    // prompt 模式等待: 检测输出尾部提示符特征
    bool wait_prompt(int64_t timeout_ms, std::string& out);

    // 调整 pty 尺寸
    void resize(int rows, int cols);

    void close();
    bool alive() const { return alive_.load(); }
    const std::string& id() const { return id_; }
    int64_t last_activity_ms() const { return last_activity_.load(); }
    pid_t pid() const { return pid_; }

private:
    void reader_loop();
    void append_output(const char* data, size_t n);

    std::string id_;
    int rows_, cols_;
    std::string cwd_;
    std::vector<std::string> env_;

    int master_fd_ = -1;       // pty 主端
    pid_t pid_ = -1;
    std::atomic<bool> alive_{false};
    std::atomic<int64_t> last_activity_{0};
    std::thread reader_;

    std::mutex buf_mutex_;
    std::string buffer_;          // 输出缓冲
    size_t last_read_pos_ = 0;    // read_last() 的偏移
    std::condition_variable buf_cv_;
};

// 终端会话管理器
class TerminalManager {
public:
    // 创建会话, 返回 session_id (失败返回空串)
    std::string create(int rows, int cols, const std::string& cwd,
                       const std::vector<std::string>& env);
    std::shared_ptr<TerminalSession> get(const std::string& id);
    void kill(const std::string& id);
    std::vector<std::shared_ptr<TerminalSession>> list();
    void cleanup_idle(int64_t idle_ms);  // 空闲超时清理

private:
    std::mutex m_;
    std::unordered_map<std::string, std::shared_ptr<TerminalSession>> sessions_;
};

} // namespace mcp
