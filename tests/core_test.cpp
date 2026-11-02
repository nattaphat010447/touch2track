// Unit tests for src/core.h. Usage: core_test <path to tests/fixtures/touchpad_frames.txt>
#include "../src/core.h"

#include <cstdio>
#include <fstream>
#include <sstream>

using namespace t2t;

static int g_failed = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            g_failed++;                                                          \
        }                                                                        \
    } while (0)

struct Report { uint32_t scan; uint32_t count; Contact c; };

static std::vector<Report> loadFixture(const char* path) {
    std::vector<Report> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        Report r{};
        int tip, conf;
        long x, y;
        if (!(ss >> r.scan >> r.count >> r.c.id >> tip >> conf >> x >> y)) continue;
        r.c.tip = tip != 0;
        r.c.confident = conf != 0;
        r.c.mx = x / 31.5;   // device units -> mm for this pad
        r.c.my = y / 31.5;
        out.push_back(r);
    }
    return out;
}

static void testFrames(const char* fixture) {
    auto reports = loadFixture(fixture);
    CHECK(reports.size() > 50);

    // Expected: consecutive reports with the same scan time form one frame; its size is the first
    // report's count, or 1 for a lift frame (count 0).
    std::vector<size_t> expected;
    for (size_t i = 0; i < reports.size();) {
        size_t j = i;
        while (j < reports.size() && reports[j].scan == reports[i].scan) j++;
        expected.push_back(j - i);
        CHECK(j - i == (reports[i].count ? reports[i].count : 1));   // fixture sanity
        i = j;
    }

    FrameAssembler fa;
    std::vector<size_t> got;
    std::vector<int> fingers;
    for (const auto& r : reports) {
        if (fa.push(true, r.scan, true, r.count, {r.c})) {
            got.push_back(fa.frame().size());
            fingers.push_back(countFingers(fa.frame()));
        }
    }
    CHECK(got == expected);

    // The previous behaviour treated every report with a readable count as a new frame, so a
    // two-finger frame was seen as one finger. Make sure multi-finger frames are really seen.
    bool sawTwo = false, sawThree = false, sawFive = false, sawPalm = false;
    for (size_t i = 0; i < got.size(); i++) {
        if (got[i] == 2 && fingers[i] == 2) sawTwo = true;
        if (got[i] == 3 && fingers[i] == 3) sawThree = true;
        if (got[i] == 5) sawFive = true;
        if (got[i] == 4 && fingers[i] == 3) sawPalm = true;   // 4 contacts, one of them a palm
    }
    CHECK(sawTwo);
    CHECK(sawThree);
    CHECK(sawFive);
    CHECK(sawPalm);
}

static void testAssemblerEdgeCases() {
    FrameAssembler fa;
    Contact a; a.id = 0; a.tip = true; a.mx = 10; a.my = 10;
    Contact b = a; b.id = 1;

    // Hybrid two-finger frame: count only in the first report.
    CHECK(!fa.push(true, 100, true, 2, {a}));
    CHECK(fa.push(true, 100, true, 0, {b}));
    CHECK(fa.frame().size() == 2);

    // A duplicate report of the delivered frame is ignored.
    CHECK(!fa.push(true, 100, true, 0, {b}));

    // Incomplete frame overtaken by the next scan is dropped.
    CHECK(!fa.push(true, 110, true, 3, {a}));
    CHECK(fa.push(true, 120, true, 1, {a}));
    CHECK(fa.frame().size() == 1);

    // Lift frame: count 0 with one contact whose tip is up.
    Contact up = a; up.tip = false;
    CHECK(fa.push(true, 130, true, 0, {up}));
    CHECK(fa.frame().size() == 1 && countFingers(fa.frame()) == 0);

    // Full (non-hybrid) report: 5 slots, only `count` of them valid.
    std::vector<Contact> slots(5, b);
    slots[0] = a;
    CHECK(fa.push(true, 140, true, 1, slots));
    CHECK(fa.frame().size() == 1);

    // No scan time and no count: every report is its own frame (bounded, no accumulation).
    for (int i = 0; i < 1000; i++) {
        CHECK(fa.push(false, 0, false, 0, {a, b}));
        CHECK(fa.frame().size() == 2);
    }

    // No scan time but a count: count > 0 starts a frame, count 0 continues it.
    fa.reset();
    CHECK(!fa.push(false, 0, true, 2, {a}));
    CHECK(fa.push(false, 0, true, 0, {b}));
    CHECK(fa.frame().size() == 2);

    // Palm handling.
    Contact palm = a; palm.confident = false;
    CHECK(countFingers({a, palm}) == 1);
    CHECK(countFingers({palm}) == 0);
    const Contact* only = nullptr;
    CHECK(countFingers({a, palm}, &only) == 1 && only && only->confident);
}

static void testParsing() {
    CHECK(parseNumber(L"0.8", 1, 0, 10) == 0.8);
    CHECK(parseNumber(L"  2.5   ; comment", 1, 0, 10) == 2.5);
    CHECK(parseNumber(L"abc", 7, 0, 10) == 7);
    CHECK(parseNumber(L"", 7, 0, 10) == 7);
    CHECK(parseNumber(L"1.5 mm", 7, 0, 10) == 7);          // trailing junk -> default
    CHECK(parseNumber(L"-5", 7, 0, 10) == 0);              // clamped
    CHECK(parseNumber(L"99999", 7, 0, 10) == 10);
    CHECK(parseNumber(L"nan", 7, 0, 10) == 7);
    CHECK(parseNumber(L"inf", 7, 0, 10) == 7);

    Hotkey h;
    CHECK(parseHotkey(L"ctrl+alt+j", h) && h.ctrl && h.alt && !h.shift && !h.win && h.vk == 'J');
    CHECK(parseHotkey(L"Ctrl + Shift + F9   ; toggle", h) && h.ctrl && h.shift && h.vk == 0x78);
    CHECK(parseHotkey(L"win+space", h) && h.win && h.vk == 0x20);
    CHECK(parseHotkey(L"alt+1", h) && h.vk == '1');
    CHECK(!parseHotkey(L"j", h));                // needs a modifier
    CHECK(!parseHotkey(L"ctrl+alt", h));         // needs a key
    CHECK(!parseHotkey(L"ctrl+alt+banana", h));
    CHECK(!parseHotkey(L"ctrl+f99", h));
    CHECK(!parseHotkey(L"", h));
}

static void testSpeed() {
    Tuning t;
    CHECK(speedPxPerSec(0.0, t) == 0);
    CHECK(speedPxPerSec(t.deadzoneMm, t) == 0);
    double prev = 0;
    for (double r = t.deadzoneMm + 0.1; r < 20; r += 0.1) {
        double v = speedPxPerSec(r, t);
        CHECK(v >= prev);
        prev = v;
    }
    CHECK(std::fabs(speedPxPerSec(t.maxRadiusMm, t) - t.maxSpeed * kPresetMul[t.preset]) < 1e-9);
    CHECK(std::fabs(speedPxPerSec(100, t) - t.maxSpeed * kPresetMul[t.preset]) < 1e-9);   // saturates
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: core_test <touchpad_frames.txt>\n");
        return 2;
    }
    testFrames(argv[1]);
    testAssemblerEdgeCases();
    testParsing();
    testSpeed();
    if (g_failed) {
        std::printf("%d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
