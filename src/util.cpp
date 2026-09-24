#include "util.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>

namespace mcp {

namespace {
LogLevel g_level = LogLevel::INFO;
std::mutex g_log_mutex;

LogLevel parse_level(const char* s) {
    if (!s) return LogLevel::INFO;
    std::string v(s);
    if (v == "trace") return LogLevel::TRACE;
    if (v == "debug") return LogLevel::DEBUG;
    if (v == "info")  return LogLevel::INFO;
    if (v == "warn" || v == "warning") return LogLevel::WARN;
    if (v == "error") return LogLevel::ERROR;
    return LogLevel::INFO;
}

const char* level_name(LogLevel lv) {
    switch (lv) {
        case LogLevel::TRACE: return "TRACE";
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERROR: return "ERROR";
    }
    return "?";
}
} // namespace

void init_logging() {
    g_level = parse_level(std::getenv("SHELL_MCP_LOG"));
}

LogLevel log_level() { return g_level; }

void log_msg(LogLevel lv, const std::string& msg) {
    if (lv < g_level) return;
    std::lock_guard lock(g_log_mutex);
    std::cerr << std::format("[{} {}] {}\n", level_name(lv), now_ms(), msg);
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------
// output hygiene helpers
// ---------------------------------------------------------------
namespace {
const char* const kRepl = "\xEF\xBF\xBD";   // U+FFFD
} // namespace

std::string strip_ansi(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    const std::size_t n = in.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned char c = (unsigned char)in[i];
        if (c == 0x1B) {
            i++;
            if (i >= n) break;
            const unsigned char d = (unsigned char)in[i];
            if (d == '[') {
                i++;
                while (i < n) {
                    const unsigned char e = (unsigned char)in[i];
                    i++;
                    if (e >= 0x40 && e <= 0x7E) break;
                }
            } else if (d == ']') {
                i++;
                while (i < n) {
                    const unsigned char e = (unsigned char)in[i];
                    if (e == 0x07) { i++; break; }
                    if (e == 0x1B && i + 1 < n && in[i + 1] == '\\') { i += 2; break; }
                    i++;
                }
            } else if (d == 'P' || d == '^' || d == '_' || d == 'X') {
                i++;
                while (i < n) {
                    if ((unsigned char)in[i] == 0x1B && i + 1 < n && in[i + 1] == '\\') { i += 2; break; }
                    i++;
                }
            } else if (d >= 0x20 && d <= 0x2F) {
                i++;
                while (i < n && (unsigned char)in[i] >= 0x20 && (unsigned char)in[i] <= 0x2F) i++;
                if (i < n) i++;
            } else {
                i++;
            }
            continue;
        }
        if (c == 0x9B) {
            i++;
            while (i < n) {
                const unsigned char e = (unsigned char)in[i];
                i++;
                if (e >= 0x40 && e <= 0x7E) break;
            }
            continue;
        }
        if (c < 0x20 && c != '\n' && c != '\t' && c != '\r') { i++; continue; }
        if (c == 0x7F) { i++; continue; }
        out.push_back((char)c);
        i++;
    }
    return out;
}

std::string sanitize_utf8(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    const std::size_t n = in.size();
    std::size_t i = 0;
    bool pending = false;
    auto flush = [&]() {
        if (pending) { out += kRepl; pending = false; }
    };
    while (i < n) {
        const unsigned char c = (unsigned char)in[i];
        std::size_t len = 0;
        if (c < 0x80) len = 1;
        else if (c >= 0xC2 && c <= 0xDF) len = 2;
        else if (c >= 0xE0 && c <= 0xEF) len = 3;
        else if (c >= 0xF0 && c <= 0xF4) len = 4;
        bool ok = (len > 0) && (i + len <= n);
        if (ok && len > 1) {
            const unsigned char c1 = (unsigned char)in[i + 1];
            if (c1 < 0x80 || c1 > 0xBF) ok = false;
            if (ok && len >= 3) {
                const unsigned char c2 = (unsigned char)in[i + 2];
                if (c2 < 0x80 || c2 > 0xBF) ok = false;
                if (ok && c == 0xE0 && c1 < 0xA0) ok = false;
                if (ok && c == 0xED && c1 > 0x9F) ok = false;
                if (ok && c == 0xF0 && c1 < 0x90) ok = false;
                if (ok && c == 0xF4 && c1 > 0x8F) ok = false;
            }
            if (ok && len == 4) {
                const unsigned char c3 = (unsigned char)in[i + 3];
                if (c3 < 0x80 || c3 > 0xBF) ok = false;
            }
        }
        if (ok) {
            flush();
            out.append(in, i, len);
            i += len;
        } else {
            pending = true;
            i++;
        }
    }
    flush();
    return out;
}

std::string sanitize_output(const std::string& in) {
    return sanitize_utf8(strip_ansi(in));
}

bool clip_tail_utf8(std::string& s, std::size_t max_bytes) {
    if (s.size() <= max_bytes) return false;
    std::size_t pos = s.size() - max_bytes;
    while (pos < s.size() && ((unsigned char)s[pos] & 0xC0) == 0x80) pos++;
    s.erase(0, pos);
    return true;
}

std::string safe_dump(const nlohmann::json& j, int indent) {
    try {
        return j.dump(indent, ' ', false, nlohmann::json::error_handler_t::replace);
    } catch (const std::exception&) {
        return std::string("{\"error\":\"dump failed\"}");
    }
}

// ---- audit log ----
namespace {
std::mutex g_audit_mutex;
std::string g_audit_path;   // empty = auto (exe dir or env)

std::string now_str() {
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm;
    localtime_r(&t, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

std::string resolve_audit_path() {
    if (!g_audit_path.empty()) return g_audit_path;
    const char* env = std::getenv("SHELL_MCP_AUDIT_LOG");
    if (env && *env) return std::string(env);
    std::error_code ec;
    auto dir = exe_dir();
    if (!dir.empty()) return (dir / "audit.log").string();
    return "audit.log";
}
} // namespace

std::filesystem::path exe_dir() {
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec || p.empty()) return std::filesystem::current_path(ec);
    return p.parent_path();
}

void set_audit_path(const std::string& path) {
    std::lock_guard lock(g_audit_mutex);
    g_audit_path = path;
}

std::string audit_path() {
    std::lock_guard lock(g_audit_mutex);
    return resolve_audit_path();
}

void audit_log(const std::string& tool, const std::string& reason,
               const std::string& args_summary, const std::string& status) {
    const std::string path = audit_path();
    std::lock_guard lock(g_audit_mutex);
    std::ofstream f(path, std::ios::app);
    if (!f) return;
    f << "[" << now_str() << "] " << tool << " | reason=" << reason
      << " | args=" << args_summary << " | status=" << status << std::endl;
}

void audit_log(const std::string& tool, const std::string& reason,
               const nlohmann::json& args, const std::string& status) {
    std::string summary = safe_dump(args);
    if (summary.size() > 200) summary = summary.substr(0, 200) + "...";
    audit_log(tool, reason, summary, status);
}

} // namespace mcp
