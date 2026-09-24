// Unit tests for the output hygiene helpers in src/util.cpp
//
// Build & run:
//   g++ -std=c++23 -Isrc -o /tmp/test_util tools/test_util.cpp src/util.cpp
//   /tmp/test_util
#include "util.h"

#include <cstdio>
#include <string>

using namespace mcp;

static int fails = 0;

static void check(const char* name, bool ok) {
    printf("%-28s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

int main() {
    // strip_ansi: colors, bracketed paste markers
    std::string colored = "\x1b[31mRED\x1b[0m plain \x1b[?2004h";
    check("strip_ansi", strip_ansi(colored) == "RED plain ");

    // sanitize_utf8: invalid byte runs collapse into one U+FFFD each
    std::string bad;
    bad.push_back((char)0xAB);
    bad.push_back((char)0x32);
    bad.push_back((char)0xFF);
    bad += "ok";
    std::string fixed = sanitize_utf8(bad);
    check("sanitize_utf8 valid out", sanitize_utf8(fixed) == fixed);
    check("sanitize_utf8 collapsed", fixed.size() == 3 + 1 + 3 + 2);

    // valid multi byte stays untouched (U+4E2D)
    std::string zh = "\xE4\xB8\xAD";
    check("keep valid utf8", sanitize_utf8(zh) == zh);

    // truncated sequence gets repaired
    std::string cut = "A\xE4\xB8";
    check("fix truncated seq", sanitize_utf8(cut).size() == 1 + 3);

    // clip_tail_utf8: flag + boundary safety
    std::string big = "0123456789" + zh + zh + zh;
    std::string cp = big;
    check("clip flag true", clip_tail_utf8(cp, 11));
    check("clip utf8 boundary", sanitize_utf8(cp) == cp);
    std::string small = "abc";
    check("clip no-op", clip_tail_utf8(small, 11) == false);

    // safe_dump: strict dump throws on invalid utf8, safe_dump does not
    nlohmann::json j;
    j["t"] = bad;
    bool threw = false;
    try {
        (void)j.dump();
    } catch (...) {
        threw = true;
    }
    check("strict dump throws", threw);
    check("safe_dump ok", safe_dump(j).find("ok") != std::string::npos);

    printf(fails ? "RESULT: FAIL(%d)\n" : "RESULT: ALL PASS\n", fails);
    return fails ? 1 : 0;
}
