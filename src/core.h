// core.h - platform-independent logic of touch2track (no Win32 types), so it can be unit tested.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

namespace t2t {

// ---------------------------------------------------------------- touch frames

struct Contact {
    int id = 0;
    bool tip = false;        // finger is touching the surface
    bool confident = true;   // false = the touchpad thinks this is a palm
    double mx = 0, my = 0;   // position in millimetres from the pad's top-left corner
};

// Windows Precision Touchpads describe a "frame" (one scan of the pad) as one or more HID
// reports. In hybrid mode every report carries just a few contacts; only the FIRST report of a
// frame has the real Contact Count, the following ones report 0. All reports of one frame share
// the same scan time. A frame with no fingers left on the pad is a single report with count 0.
class FrameAssembler {
public:
    // Feed the contacts decoded from one report. Returns true when a complete frame is ready
    // (read it with frame()). Incomplete frames that are overtaken by a newer scan are dropped.
    bool push(bool hasScan, uint32_t scan, bool hasCount, uint32_t count, const std::vector<Contact>& contacts) {
        bool startNew;
        if (hasScan) startNew = !haveScan_ || scan != scan_;
        else if (hasCount) startNew = count > 0 || !open_;
        else startNew = true;                         // no way to group reports: each one is a frame
        if (!startNew && !open_) return false;        // stray duplicate of a frame we already delivered
        if (startNew) {
            cur_.clear();
            open_ = true;
            haveScan_ = hasScan;
            scan_ = scan;
            expected_ = (hasCount && count > 0) ? count : contacts.size();
        }
        size_t room = expected_ > cur_.size() ? expected_ - cur_.size() : 0;
        for (size_t i = 0; i < contacts.size() && i < room; i++) cur_.push_back(contacts[i]);
        if (cur_.size() >= expected_) {
            done_ = cur_;
            open_ = false;
            return true;
        }
        return false;
    }

    const std::vector<Contact>& frame() const { return done_; }

    void reset() {
        cur_.clear();
        done_.clear();
        open_ = haveScan_ = false;
        scan_ = 0;
        expected_ = 0;
    }

private:
    std::vector<Contact> cur_, done_;
    bool open_ = false, haveScan_ = false;
    uint32_t scan_ = 0;
    size_t expected_ = 0;
};

// Fingers that count for the joystick: touching, and not rejected as a palm.
inline int countFingers(const std::vector<Contact>& frame, const Contact** only = nullptr) {
    int n = 0;
    const Contact* last = nullptr;
    for (const auto& c : frame)
        if (c.tip && c.confident) { n++; last = &c; }
    if (only) *only = last;
    return n;
}

// ---------------------------------------------------------------- tuning

struct Tuning {
    double deadzoneMm = 0.8;         // finger offset that is ignored (hand tremor)
    double maxRadiusMm = 12.0;       // finger offset that counts as full deflection
    double maxSpeed = 450.0;         // pixels/second at full deflection, before the preset
    double gamma = 1.5;              // speed curve exponent (1 = linear)
    double touchDelayMs = 70;        // a lone finger must rest this long before the joystick starts
    double mouseHoldMs = 150;        // a real mouse moved this recently => it wins
    double releaseTimeoutMs = 500;   // no touchpad report for this long => treat the finger as lifted (0 = off)
    int sendIntervalMs = 4;          // cursor update period
    int preset = 1;                  // 0 slow, 1 medium, 2 fast
};
static const double kPresetMul[3] = {0.6, 1.0, 1.6};

// Cursor speed in pixels/second for a finger offset of r millimetres from the origin.
inline double speedPxPerSec(double r, const Tuning& t) {
    if (r <= t.deadzoneMm) return 0;
    double k = std::fmin(1.0, (r - t.deadzoneMm) / (t.maxRadiusMm - t.deadzoneMm));
    return t.maxSpeed * kPresetMul[t.preset] * std::pow(k, t.gamma);
}

// ---------------------------------------------------------------- config text parsing

inline std::wstring trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::iswspace(s[b])) b++;
    while (e > b && std::iswspace(s[e - 1])) e--;
    return s.substr(b, e - b);
}

// GetPrivateProfileString keeps inline comments ("0.8   ; note"); remove them and surrounding blanks.
inline std::wstring cleanValue(const std::wstring& raw) {
    size_t c = raw.find(L';');
    return trim(c == std::wstring::npos ? raw : raw.substr(0, c));
}

// Parses a number; anything unusable (empty, text, NaN/inf, trailing junk) yields `def`.
// Valid numbers are clamped into [lo, hi].
inline double parseNumber(const std::wstring& raw, double def, double lo, double hi) {
    std::wstring s = cleanValue(raw);
    if (s.empty()) return def;
    wchar_t* end = nullptr;
    double v = std::wcstod(s.c_str(), &end);
    if (end == s.c_str() || *end != L'\0' || !std::isfinite(v)) return def;
    return std::min(hi, std::max(lo, v));
}

struct Hotkey {
    bool ctrl = false, alt = false, shift = false, win = false;
    unsigned vk = 0;   // Windows virtual-key code
    bool valid() const { return vk != 0 && (ctrl || alt || shift || win); }
};

// "ctrl+alt+j", "Ctrl + Shift + F9", "win+space" ... Requires at least one modifier and a key.
inline bool parseHotkey(const std::wstring& raw, Hotkey& out) {
    Hotkey h;
    std::wstring s = cleanValue(raw);
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t e = s.find(L'+', pos);
        if (e == std::wstring::npos) e = s.size();
        std::wstring t = trim(s.substr(pos, e - pos));
        for (auto& ch : t) ch = (wchar_t)std::towlower(ch);
        if (t == L"ctrl" || t == L"control") h.ctrl = true;
        else if (t == L"alt") h.alt = true;
        else if (t == L"shift") h.shift = true;
        else if (t == L"win") h.win = true;
        else if (t == L"space") h.vk = 0x20;
        else if (t == L"tab") h.vk = 0x09;
        else if (t == L"enter" || t == L"return") h.vk = 0x0D;
        else if (t == L"esc" || t == L"escape") h.vk = 0x1B;
        else if (t == L"insert") h.vk = 0x2D;
        else if (t == L"delete" || t == L"del") h.vk = 0x2E;
        else if (t == L"home") h.vk = 0x24;
        else if (t == L"end") h.vk = 0x23;
        else if (t == L"pageup") h.vk = 0x21;
        else if (t == L"pagedown") h.vk = 0x22;
        else if (t.size() == 1 && std::iswalnum(t[0]) && t[0] < 128) h.vk = (unsigned)std::towupper(t[0]);
        else if (t.size() >= 2 && t[0] == L'f') {
            wchar_t* end = nullptr;
            long n = std::wcstol(t.c_str() + 1, &end, 10);
            if (*end == L'\0' && n >= 1 && n <= 24) h.vk = 0x70 + (unsigned)n - 1;   // VK_F1 = 0x70
            else return false;
        } else {
            return false;   // unknown token
        }
        pos = e + 1;
    }
    if (!h.valid()) return false;
    out = h;
    return true;
}

}  // namespace t2t
