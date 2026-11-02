// touch2track - turn a Windows Precision Touchpad into a virtual trackpoint (joystick).
//
// Touch and hold, then nudge your finger away from where it first landed: the cursor keeps
// gliding in that direction, faster the further you push. Lift the finger to stop.
//
// Threads:
//   UI thread      tray icon, hotkey, Raw Input (touchpad + mice), power/session events
//   hook thread    WH_MOUSE_LL hook that swallows the touchpad's own cursor movement while exactly
//                  one finger is down. Never blocked by anything else, because Windows silently
//                  removes a low-level hook that is too slow.
//   worker thread  turns the finger's offset from its origin into velocity and moves the cursor
//                  with SendInput. Sleeps (no timer, no wake-ups) while no finger is down.
//
// Controlled from a tray icon and a global hotkey (default Ctrl+Alt+J); settings live in
// config.ini next to the executable (or in %APPDATA%\touch2track if that folder is read-only).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <objbase.h>
#include <oleauto.h>
#include <shellapi.h>
#include <taskschd.h>
#include <wtsapi32.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <atomic>
#include <map>
#include <string>
#include <vector>

#include "core.h"

static const wchar_t* kAppName = L"touch2track";
static const wchar_t* kWndClass = L"touch2track.window";
static const wchar_t* kTaskName = L"touch2track";
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunName = L"touch2track";

enum {
    WM_TRAY = WM_APP + 1,
    WM_TASK_DONE = WM_APP + 2,
    WM_HOOK_REINSTALL = WM_APP + 10,   // posted to the hook thread
    ID_TOGGLE = 1, ID_SPEED0 = 10, ID_AUTOSTART = 20, ID_RELOAD = 21, ID_OPENCFG = 22,
    ID_RELAUNCH_ADMIN = 23, ID_TASK = 24, ID_EXIT = 99
};
#ifndef WM_INPUT_DEVICE_CHANGE
#define WM_INPUT_DEVICE_CHANGE 0x00FE
#endif

// ---------------------------------------------------------------- shared state
static CRITICAL_SECTION g_cs;             // guards g_tun and the joystick state below
static t2t::Tuning g_tun;
static std::wstring g_iniPath;
static std::wstring g_hotkeyText = L"ctrl+alt+j";
static t2t::Hotkey g_hotkey = [] { t2t::Hotkey h; h.ctrl = h.alt = true; h.vk = 'J'; return h; }();

static std::atomic<bool> g_enabled{true};
static std::atomic<bool> g_quit{false};
static std::atomic<int> g_tips{0};                  // fingers currently on the pad (palms excluded)
static std::atomic<int> g_mouseHold{150};           // copy of Tuning::mouseHoldMs for the hook
static std::atomic<ULONGLONG> g_lastTouch{0};       // last frame with a finger down
static std::atomic<ULONGLONG> g_lastReport{0};      // last HID report of any kind
static std::atomic<ULONGLONG> g_lastRawMouse{0};    // last movement of a real (non-touchpad) mouse

// Joystick state (guarded by g_cs)
static bool g_haveOne = false;
static int g_trackId = -1;
static ULONGLONG g_singleSince = 0;                 // when the current single-finger touch began
static double g_ox = 0, g_oy = 0, g_cx = 0, g_cy = 0;   // origin and current finger position (mm)

static HANDLE g_wake;                               // wakes the worker thread
static bool g_elevated = false;
static UINT g_msgShow;                              // "already running" message between instances

static void signalWorker() { SetEvent(g_wake); }

// ---------------------------------------------------------------- config
static std::wstring readIni(const wchar_t* section, const wchar_t* key) {
    wchar_t buf[256];
    GetPrivateProfileStringW(section, key, L"", buf, 256, g_iniPath.c_str());
    return buf;
}

static void loadConfig() {
    using namespace t2t;
    auto num = [](const wchar_t* key, double def, double lo, double hi) {
        return parseNumber(readIni(L"joystick", key), def, lo, hi);
    };
    Tuning t;
    t.deadzoneMm = num(L"deadzone_mm", t.deadzoneMm, 0, 10);
    t.maxRadiusMm = num(L"max_radius_mm", t.maxRadiusMm, 1, 60);
    if (t.maxRadiusMm < t.deadzoneMm + 0.5) t.maxRadiusMm = t.deadzoneMm + 0.5;
    t.maxSpeed = num(L"max_speed", t.maxSpeed, 10, 5000);
    t.gamma = num(L"gamma", t.gamma, 0.5, 4);
    t.touchDelayMs = num(L"touch_delay_ms", t.touchDelayMs, 0, 1000);
    t.mouseHoldMs = num(L"mouse_hold_ms", t.mouseHoldMs, 0, 2000);
    t.releaseTimeoutMs = num(L"release_timeout_ms", t.releaseTimeoutMs, 0, 10000);
    if (t.releaseTimeoutMs > 0 && t.releaseTimeoutMs < 100) t.releaseTimeoutMs = 100;
    t.sendIntervalMs = (int)num(L"send_interval_ms", t.sendIntervalMs, 1, 50);
    t.preset = (int)num(L"speed_preset", t.preset, 0, 2);

    Hotkey hk;
    std::wstring hkText = cleanValue(readIni(L"hotkey", L"toggle"));
    bool hkOk = parseHotkey(hkText, hk);

    EnterCriticalSection(&g_cs);
    g_tun = t;
    g_mouseHold = (int)t.mouseHoldMs;
    LeaveCriticalSection(&g_cs);
    if (hkOk) { g_hotkey = hk; g_hotkeyText = hkText; }
    signalWorker();   // the update interval may have changed
}

static const char kDefaultConfig[] =
    "; touch2track settings. Lines starting with ';' are comments.\r\n"
    "; Use \"Reload config\" in the tray menu after editing.\r\n"
    "\r\n"
    "[joystick]\r\n"
    "; Finger offset around the starting point that is ignored (mm). Filters hand tremor.\r\n"
    "deadzone_mm=0.8\r\n"
    "; Finger offset that counts as full deflection (mm).\r\n"
    "max_radius_mm=12\r\n"
    "; Cursor speed at full deflection, in pixels per second (before the speed preset).\r\n"
    "max_speed=450\r\n"
    "; Speed curve exponent: 1 = linear, higher = finer control near the centre.\r\n"
    "gamma=1.5\r\n"
    "; A lone finger must rest this long (ms) before the joystick starts moving.\r\n"
    "; Prevents two-finger scrolling from nudging the cursor.\r\n"
    "touch_delay_ms=70\r\n"
    "; A real mouse moved this recently (ms) => the joystick pauses. 0 disables.\r\n"
    "mouse_hold_ms=150\r\n"
    "; No touchpad report for this long (ms) => treat the finger as lifted. 0 disables.\r\n"
    "release_timeout_ms=500\r\n"
    "; How often the cursor is updated (ms, 1-50).\r\n"
    "send_interval_ms=4\r\n"
    "; 0 = slow (x0.6), 1 = medium (x1.0), 2 = fast (x1.6). Also set from the tray menu.\r\n"
    "speed_preset=1\r\n"
    "\r\n"
    "[hotkey]\r\n"
    "; Modifiers: ctrl, alt, shift, win. Key: a letter, digit, F1-F24, space, tab, ...\r\n"
    "toggle=ctrl+alt+j\r\n";

static bool fileExists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Creates `path` with the default template. Fails (returns false) if the folder is not writable.
static bool writeDefaultConfig(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    BOOL ok = WriteFile(f, kDefaultConfig, (DWORD)(sizeof(kDefaultConfig) - 1), &w, nullptr);
    CloseHandle(f);
    return ok != FALSE;
}

// config.ini lives next to the exe; if that folder is read-only (e.g. Program Files) it falls
// back to %APPDATA%\touch2track\config.ini.
static std::wstring resolveConfigPath() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    std::wstring beside = dir + L"config.ini";
    if (fileExists(beside) || writeDefaultConfig(beside)) return beside;
    wchar_t appdata[MAX_PATH];
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) {
        std::wstring d = std::wstring(appdata) + L"\\touch2track";
        CreateDirectoryW(d.c_str(), nullptr);
        std::wstring p = d + L"\\config.ini";
        if (fileExists(p) || writeDefaultConfig(p)) return p;
    }
    return beside;   // nothing is writable: defaults will be used, edits cannot be saved
}

// ---------------------------------------------------------------- HID touchpad
struct Link {                       // one finger slot of a report
    USHORT link = 0;
    LONG xMin = 0, xMax = 0, yMin = 0, yMax = 0;
    int xBits = 32, yBits = 32;
    bool xSigned = false, ySigned = false;
    double xMm = 0, yMm = 0;
    bool hasX = false, hasY = false, hasConf = false;
};
struct Device {
    std::vector<unsigned char> ppBuf;
    PHIDP_PREPARSED_DATA pp = nullptr;      // null = not a usable touchpad
    std::vector<Link> links;
    bool hasCount = false, hasScan = false;
    USHORT countLink = 0, scanLink = 0;
    t2t::FrameAssembler fa;
    std::vector<t2t::Contact> scratch;
};
static std::map<HANDLE, Device> g_devs;                 // UI thread only
static std::map<HANDLE, bool> g_mouseIsTouchpad;        // UI thread only
static std::vector<std::wstring> g_tpKeys;              // touchpad device-path prefixes (UI thread only)

template <class Cap, class F>
static void eachUsage(const Cap& c, F f) {
    if (c.IsRange) { for (ULONG u = c.Range.UsageMin; u <= c.Range.UsageMax; u++) f((USHORT)u); }
    else f(c.NotRange.Usage);
}

// Physical size in mm from a HID unit/exponent pair; 0 if the unit is not cm or inch.
static double unitToMm(ULONG units, LONG exp, double phys) {
    exp &= 0xF;  // UnitsExp is a signed nibble (0xE = -2)
    if (exp > 7) exp -= 16;
    double f = std::pow(10.0, (double)exp);
    if ((units & 0xF) == 1 && ((units >> 4) & 0xF) == 1) return phys * f * 10.0;   // SI linear, cm
    if ((units & 0xF) == 3 && ((units >> 4) & 0xF) == 1) return phys * f * 25.4;   // English linear, inch
    return 0;
}

static LONG signExtend(ULONG v, int bits, bool isSigned) {
    if (!isSigned || bits >= 32 || bits <= 0) return (LONG)v;
    ULONG m = 1UL << (bits - 1);
    return (LONG)((v ^ m) - m);
}

static Device* getDevice(HANDLE h) {
    auto it = g_devs.find(h);
    if (it != g_devs.end()) return it->second.pp ? &it->second : nullptr;
    Device& d = g_devs[h];   // built in place, so pp keeps pointing into ppBuf
    UINT size = 0;
    GetRawInputDeviceInfoW(h, RIDI_PREPARSEDDATA, nullptr, &size);
    if (!size) return nullptr;
    d.ppBuf.resize(size);
    if (GetRawInputDeviceInfoW(h, RIDI_PREPARSEDDATA, d.ppBuf.data(), &size) == (UINT)-1) return nullptr;
    PHIDP_PREPARSED_DATA pp = (PHIDP_PREPARSED_DATA)d.ppBuf.data();
    HIDP_CAPS caps;
    if (HidP_GetCaps(pp, &caps) != HIDP_STATUS_SUCCESS) return nullptr;
    std::vector<HIDP_VALUE_CAPS> vc(caps.NumberInputValueCaps);
    USHORT nv = caps.NumberInputValueCaps;
    if (!nv || HidP_GetValueCaps(HidP_Input, vc.data(), &nv, pp) != HIDP_STATUS_SUCCESS) return nullptr;
    std::vector<HIDP_BUTTON_CAPS> bc(caps.NumberInputButtonCaps);
    USHORT nb = caps.NumberInputButtonCaps;
    if (nb && HidP_GetButtonCaps(HidP_Input, bc.data(), &nb, pp) != HIDP_STATUS_SUCCESS) nb = 0;

    std::map<USHORT, Link> lm;
    for (USHORT i = 0; i < nv; i++) {
        const HIDP_VALUE_CAPS& c = vc[i];
        eachUsage(c, [&](USHORT u) {
            if (c.UsagePage == 0x01 && (u == 0x30 || u == 0x31)) {            // X / Y
                Link& l = lm[c.LinkCollection];
                l.link = c.LinkCollection;
                double mm = unitToMm(c.Units, c.UnitsExp, (double)(c.PhysicalMax - c.PhysicalMin));
                if (u == 0x30) { l.hasX = true; l.xMin = c.LogicalMin; l.xMax = c.LogicalMax; l.xMm = mm; l.xBits = c.BitSize; l.xSigned = c.LogicalMin < 0; }
                else           { l.hasY = true; l.yMin = c.LogicalMin; l.yMax = c.LogicalMax; l.yMm = mm; l.yBits = c.BitSize; l.ySigned = c.LogicalMin < 0; }
            } else if (c.UsagePage == 0x0D && u == 0x54) {                     // contact count
                d.hasCount = true; d.countLink = c.LinkCollection;
            } else if (c.UsagePage == 0x0D && u == 0x56) {                     // scan time
                d.hasScan = true; d.scanLink = c.LinkCollection;
            }
        });
    }
    for (USHORT i = 0; i < nb; i++) {
        const HIDP_BUTTON_CAPS& c = bc[i];
        eachUsage(c, [&](USHORT u) {
            if (c.UsagePage == 0x0D && u == 0x47) lm[c.LinkCollection].hasConf = true;   // confidence (palm rejection)
        });
    }
    for (auto& kv : lm) {
        Link l = kv.second;
        if (!l.hasX || !l.hasY || l.xMax <= l.xMin || l.yMax <= l.yMin) continue;
        // No usable physical size: assume the typical ~31.5 device units per millimetre.
        if (l.xMm <= 0) l.xMm = (double)(l.xMax - l.xMin) / 31.5;
        if (l.yMm <= 0) l.yMm = (double)(l.yMax - l.yMin) / 31.5;
        d.links.push_back(l);
    }
    if (d.links.empty()) return nullptr;
    d.pp = pp;
    return &d;
}

static void resetJoystickLocked() { g_haveOne = false; g_trackId = -1; }

static void resetInputState() {
    EnterCriticalSection(&g_cs);
    resetJoystickLocked();
    LeaveCriticalSection(&g_cs);
    g_tips = 0;
    for (auto& kv : g_devs) kv.second.fa.reset();
    signalWorker();
}

static void handleFrame(const std::vector<t2t::Contact>& frame) {
    const t2t::Contact* one = nullptr;
    int tips = t2t::countFingers(frame, &one);
    g_tips = tips;
    ULONGLONG now = GetTickCount64();
    if (tips) g_lastTouch = now;
    EnterCriticalSection(&g_cs);
    bool before = g_haveOne;
    if (tips == 1) {
        if (!g_haveOne || one->id != g_trackId) {        // new single-finger touch: this is the origin
            g_ox = one->mx; g_oy = one->my; g_trackId = one->id;
            g_singleSince = now;
        }
        g_cx = one->mx; g_cy = one->my; g_haveOne = true;
    } else {
        resetJoystickLocked();
    }
    bool after = g_haveOne;
    LeaveCriticalSection(&g_cs);
    if (before != after) signalWorker();
}

static void onHid(RAWINPUT* ri) {
    Device* d = getDevice(ri->header.hDevice);
    if (!d) return;
    RAWHID& h = ri->data.hid;
    for (DWORD r = 0; r < h.dwCount; r++) {
        PCHAR rep = (PCHAR)(h.bRawData + r * h.dwSizeHid);
        ULONG size = h.dwSizeHid;
        ULONG count = 0, scan = 0;
        bool hasCount = d->hasCount &&
            HidP_GetUsageValue(HidP_Input, 0x0D, d->countLink, 0x54, &count, d->pp, rep, size) == HIDP_STATUS_SUCCESS;
        bool hasScan = d->hasScan &&
            HidP_GetUsageValue(HidP_Input, 0x0D, d->scanLink, 0x56, &scan, d->pp, rep, size) == HIDP_STATUS_SUCCESS;
        d->scratch.clear();
        for (const Link& l : d->links) {
            ULONG x, y, id = 0;
            if (HidP_GetUsageValue(HidP_Input, 0x01, l.link, 0x30, &x, d->pp, rep, size) != HIDP_STATUS_SUCCESS) continue;
            if (HidP_GetUsageValue(HidP_Input, 0x01, l.link, 0x31, &y, d->pp, rep, size) != HIDP_STATUS_SUCCESS) continue;
            HidP_GetUsageValue(HidP_Input, 0x0D, l.link, 0x51, &id, d->pp, rep, size);
            USAGE us[16]; ULONG ul = 16;
            bool tip = false, conf = !l.hasConf;   // pads without a confidence usage never report palms
            if (HidP_GetUsages(HidP_Input, 0x0D, l.link, us, &ul, d->pp, rep, size) == HIDP_STATUS_SUCCESS)
                for (ULONG k = 0; k < ul; k++) {
                    if (us[k] == 0x42) tip = true;       // tip switch
                    if (us[k] == 0x47) conf = true;      // confidence
                }
            LONG xs = signExtend(x, l.xBits, l.xSigned), ys = signExtend(y, l.yBits, l.ySigned);
            t2t::Contact c;
            c.id = (int)id; c.tip = tip; c.confident = conf;
            c.mx = (double)(xs - l.xMin) / (l.xMax - l.xMin) * l.xMm;
            c.my = (double)(ys - l.yMin) / (l.yMax - l.yMin) * l.yMm;
            d->scratch.push_back(c);
        }
        if (!hasCount && !hasScan && d->scratch.empty()) continue;   // not a touch report
        g_lastReport = GetTickCount64();
        if (d->fa.push(hasScan, scan, hasCount, count, d->scratch)) handleFrame(d->fa.frame());
    }
}

// Recomputes the touchpad device-path prefixes; returns true if a Precision Touchpad is present.
static bool refreshTouchpads() {
    g_tpKeys.clear();
    g_mouseIsTouchpad.clear();
    UINT n = 0;
    GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST));
    std::vector<RAWINPUTDEVICELIST> list(n);
    if (!n || GetRawInputDeviceList(list.data(), &n, sizeof(RAWINPUTDEVICELIST)) == (UINT)-1) return false;
    bool found = false;
    for (auto& e : list) {
        if (e.dwType != RIM_TYPEHID) continue;
        RID_DEVICE_INFO info; info.cbSize = sizeof(info); UINT sz = sizeof(info);
        if (GetRawInputDeviceInfoW(e.hDevice, RIDI_DEVICEINFO, &info, &sz) == (UINT)-1) continue;
        if (info.hid.usUsagePage != 0x0D || info.hid.usUsage != 0x05) continue;
        found = true;
        // Device paths look like \\?\hid#elan0xxx&col01#...; keep the part before "&col" so the
        // touchpad's own mouse collection (&col02) can be told apart from real mice.
        UINT len = 0;
        GetRawInputDeviceInfoW(e.hDevice, RIDI_DEVICENAME, nullptr, &len);
        if (!len) continue;
        std::wstring name(len, L'\0');
        if (GetRawInputDeviceInfoW(e.hDevice, RIDI_DEVICENAME, &name[0], &len) == (UINT)-1) continue;
        name.resize(wcslen(name.c_str()));
        for (auto& ch : name) ch = (wchar_t)towlower(ch);
        size_t p = name.find(L"&col");
        if (p != std::wstring::npos) g_tpKeys.push_back(name.substr(0, p));
    }
    return found;
}

// True if this raw-mouse device is the touchpad's own mouse collection rather than a real mouse.
static bool isTouchpadMouse(HANDLE h) {
    auto it = g_mouseIsTouchpad.find(h);
    if (it != g_mouseIsTouchpad.end()) return it->second;
    bool tp = false;
    UINT n = 0;
    GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, nullptr, &n);
    if (n) {
        std::wstring name(n, L'\0');
        if (GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, &name[0], &n) != (UINT)-1) {
            for (auto& ch : name) ch = (wchar_t)towlower(ch);
            for (auto& k : g_tpKeys) if (!k.empty() && name.compare(0, k.size(), k) == 0) tp = true;
        }
    }
    return g_mouseIsTouchpad[h] = tp;
}

// ---------------------------------------------------------------- mouse hook thread
static LRESULT CALLBACK LLMouse(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && wParam == WM_MOUSEMOVE && g_enabled) {
        auto* ms = (MSLLHOOKSTRUCT*)lParam;
        ULONGLONG now = GetTickCount64();
        // A real mouse is moving: let everything through so it keeps priority.
        bool mouseBusy = now - g_lastRawMouse < (ULONGLONG)g_mouseHold;
        if (!(ms->flags & LLMHF_INJECTED) && g_tips == 1 && now - g_lastTouch < 60 && !mouseBusy)
            return 1;  // swallow the touchpad's own movement (one finger only; two fingers = scroll/gestures)
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

static DWORD g_hookThreadId;
static HANDLE g_hookReady;
static std::atomic<bool> g_hookOk{false};

static DWORD WINAPI HookThread(LPVOID) {
    MSG m;
    PeekMessageW(&m, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // create the message queue
    HINSTANCE inst = GetModuleHandleW(nullptr);
    HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, LLMouse, inst, 0);
    g_hookOk = hook != nullptr;
    SetEvent(g_hookReady);
    if (!hook) return 1;
    // Windows removes a low-level hook without notice if it ever times out (and after resume or
    // unlock it can be stale), so it is re-installed on those events and every few minutes.
    UINT_PTR timer = SetTimer(nullptr, 0, 5 * 60 * 1000, nullptr);
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        bool reinstall = m.message == WM_HOOK_REINSTALL || (m.message == WM_TIMER && m.wParam == timer && g_tips == 0);
        if (reinstall) {
            if (hook) UnhookWindowsHookEx(hook);
            hook = SetWindowsHookExW(WH_MOUSE_LL, LLMouse, inst, 0);
        } else if (!hook && m.message == WM_TIMER) {
            hook = SetWindowsHookExW(WH_MOUSE_LL, LLMouse, inst, 0);
        } else {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    KillTimer(nullptr, timer);
    if (hook) UnhookWindowsHookEx(hook);
    return 0;
}

static void reinstallHook() {
    if (g_hookThreadId) PostThreadMessageW(g_hookThreadId, WM_HOOK_REINSTALL, 0, 0);
}

// ---------------------------------------------------------------- worker thread
struct Snapshot {
    bool enabled, haveOne;
    double ox, oy, cx, cy;
    ULONGLONG singleSince;
    t2t::Tuning t;
};

static Snapshot takeSnapshot() {
    Snapshot s;
    EnterCriticalSection(&g_cs);
    s.enabled = g_enabled;
    s.haveOne = g_haveOne;
    s.ox = g_ox; s.oy = g_oy; s.cx = g_cx; s.cy = g_cy;
    s.singleSince = g_singleSince;
    s.t = g_tun;   // plain numbers only: no allocation here
    LeaveCriticalSection(&g_cs);
    return s;
}

static DWORD WINAPI WorkerThread(LPVOID) {
    // High-resolution timers work in a window-less process; the classic timer + timeBeginPeriod
    // does not on current Windows 11 builds.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    bool coarse = timer == nullptr;
    if (coarse) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    HANDLE waits[2] = {g_wake, timer};
    LARGE_INTEGER freq, last;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    bool armed = false;
    int armedInterval = 0;
    double accx = 0, accy = 0;   // sub-pixel remainder carried between updates

    auto arm = [&](int ms) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)ms * 10000;
        if (coarse && !armed) timeBeginPeriod(1);
        SetWaitableTimer(timer, &due, ms, nullptr, nullptr, FALSE);
        armed = true;
        armedInterval = ms;
    };
    auto disarm = [&]() {
        if (!armed) return;
        CancelWaitableTimer(timer);
        if (coarse) timeEndPeriod(1);
        armed = false;
    };

    while (!g_quit) {
        // Idle: block on the wake event only, so there are no timer wake-ups without a finger down.
        WaitForMultipleObjects(2, waits, FALSE, armed ? 100 : INFINITE);
        if (g_quit) break;
        Snapshot s = takeSnapshot();
        bool want = s.enabled && s.haveOne;
        if (want && !armed) { arm(s.t.sendIntervalMs); QueryPerformanceCounter(&last); accx = accy = 0; continue; }
        if (!want) { disarm(); accx = accy = 0; continue; }
        if (s.t.sendIntervalMs != armedInterval) arm(s.t.sendIntervalMs);

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = (double)(now.QuadPart - last.QuadPart) / (double)freq.QuadPart;
        last = now;
        if (dt > 0.05) dt = 0.05;

        ULONGLONG tick = GetTickCount64();
        // The finger's lift was never seen (pad disabled, sleep, lost report): stop by ourselves.
        if (s.t.releaseTimeoutMs > 0 && tick - g_lastReport > (ULONGLONG)s.t.releaseTimeoutMs) {
            EnterCriticalSection(&g_cs);
            resetJoystickLocked();
            LeaveCriticalSection(&g_cs);
            g_tips = 0;
            continue;
        }
        if (tick - s.singleSince < (ULONGLONG)s.t.touchDelayMs) continue;        // maybe a second finger is landing
        if (tick - g_lastRawMouse < (ULONGLONG)s.t.mouseHoldMs) continue;        // a real mouse has priority

        double dx = s.cx - s.ox, dy = s.cy - s.oy;
        double r = std::sqrt(dx * dx + dy * dy);
        double v = t2t::speedPxPerSec(r, s.t);
        if (v <= 0) { accx = accy = 0; continue; }
        accx += dx / r * v * dt;
        accy += dy / r * v * dt;
        int ix = (int)accx, iy = (int)accy;   // truncate toward zero, keep the fraction for next time
        if (!ix && !iy) continue;
        accx -= ix; accy -= iy;
        INPUT in = {};
        in.type = INPUT_MOUSE;
        in.mi.dx = ix; in.mi.dy = iy;
        in.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &in, sizeof(in));   // fails only when blocked (elevated window / secure desktop): nothing to do
    }
    disarm();
    CloseHandle(timer);
    return 0;
}

// ---------------------------------------------------------------- elevation, autostart, scheduled task
static bool isElevated() {
    HANDLE tok;
    TOKEN_ELEVATION e = {};
    DWORD n = 0;
    bool r = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        if (GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &n)) r = e.TokenIsElevated != 0;
        CloseHandle(tok);
    }
    return r;
}

static void removeRunKey() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_WRITE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, kRunName);
        RegCloseKey(k);
    }
}

static bool autostartOn() {
    HKEY k;
    bool r = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        r = RegQueryValueExW(k, kRunName, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
        RegCloseKey(k);
    }
    return r;
}

static void setAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_WRITE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring v = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(k, kRunName, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kRunName);
    }
    RegCloseKey(k);
}

// An elevated autostart must not point at a file that a standard user (or any program running as
// them) can replace. Only folders under Program Files or Windows qualify.
static bool inProtectedFolder() {
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    std::wstring p = exe;
    for (auto& ch : p) ch = (wchar_t)towlower(ch);
    const wchar_t* vars[] = {L"ProgramFiles", L"ProgramW6432", L"ProgramFiles(x86)", L"SystemRoot"};
    for (const wchar_t* v : vars) {
        wchar_t buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(v, buf, MAX_PATH);
        if (!n || n >= MAX_PATH) continue;
        std::wstring base = buf;
        for (auto& ch : base) ch = (wchar_t)towlower(ch);
        if (!base.empty() && base.back() != L'\\') base += L'\\';
        if (p.compare(0, base.size(), base) == 0) return true;
    }
    return false;
}

static const wchar_t* kAdminTaskRefusal =
    L"\"Start with Windows as administrator\" is only available when touch2track is installed in a folder "
    L"that standard users cannot write to, for example C:\\Program Files\\touch2track.\n\n"
    L"Otherwise any program running as you could replace touch2track.exe and gain administrator "
    L"rights at your next sign-in.\n\n"
    L"You can still use \"Restart as administrator\" for the current session.";

// Task Scheduler is used through its COM API (no temp files, no schtasks.exe).
static const GUID kClsidTaskScheduler = {0x0F87369F, 0xA4E5, 0x4CFC, {0xBD, 0x3E, 0x73, 0xE6, 0x15, 0x45, 0x72, 0xDD}};
static const GUID kIidTaskService = {0x2FABA4C7, 0x4DA9, 0x4013, {0x96, 0x97, 0x20, 0xCC, 0x3F, 0xD4, 0x0F, 0x85}};

struct TaskFolder {
    ITaskService* svc = nullptr;
    ITaskFolder* folder = nullptr;
    ~TaskFolder() { if (folder) folder->Release(); if (svc) svc->Release(); }
    bool open() {
        if (FAILED(CoCreateInstance(kClsidTaskScheduler, nullptr, CLSCTX_INPROC_SERVER, kIidTaskService, (void**)&svc))) return false;
        VARIANT e;
        VariantInit(&e);
        if (FAILED(svc->Connect(e, e, e, e))) return false;
        BSTR root = SysAllocString(L"\\");
        HRESULT hr = svc->GetFolder(root, &folder);
        SysFreeString(root);
        return SUCCEEDED(hr) && folder;
    }
};

static bool taskExists() {
    TaskFolder tf;
    if (!tf.open()) return false;
    BSTR name = SysAllocString(kTaskName);
    IRegisteredTask* t = nullptr;
    HRESULT hr = tf.folder->GetTask(name, &t);
    SysFreeString(name);
    if (SUCCEEDED(hr) && t) { t->Release(); return true; }
    return false;
}

static std::wstring xmlEsc(const std::wstring& s) {
    std::wstring o;
    for (wchar_t ch : s) {
        if (ch == L'&') o += L"&amp;"; else if (ch == L'<') o += L"&lt;"; else if (ch == L'>') o += L"&gt;"; else o += ch;
    }
    return o;
}

// Must run elevated: registers a logon task with highest privileges so the app starts as
// administrator without a UAC prompt every time.
static bool taskInstall() {
    wchar_t exe[MAX_PATH], user[256] = L"", dom[256] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    DWORD un = 256;
    GetUserNameW(user, &un);
    GetEnvironmentVariableW(L"USERDOMAIN", dom, 256);
    std::wstring who = xmlEsc(std::wstring(dom) + L"\\" + user);
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
        L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + who + L"</UserId></LogonTrigger></Triggers>"
        L"<Principals><Principal id=\"Author\"><UserId>" + who + L"</UserId>"
        L"<LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
        L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
        L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><StartWhenAvailable>true</StartWhenAvailable><Enabled>true</Enabled></Settings>"
        L"<Actions Context=\"Author\"><Exec><Command>" + xmlEsc(exe) + L"</Command></Exec></Actions></Task>";
    TaskFolder tf;
    if (!tf.open()) return false;
    BSTR name = SysAllocString(kTaskName);
    BSTR body = SysAllocString(xml.c_str());
    VARIANT e;
    VariantInit(&e);
    IRegisteredTask* reg = nullptr;
    HRESULT hr = tf.folder->RegisterTask(name, body, TASK_CREATE_OR_UPDATE, e, e, TASK_LOGON_INTERACTIVE_TOKEN, e, &reg);
    SysFreeString(name);
    SysFreeString(body);
    if (reg) reg->Release();
    if (FAILED(hr)) return false;
    removeRunKey();   // avoid starting twice (a non-elevated copy could win the single-instance mutex)
    return true;
}

static bool taskRemove() {
    TaskFolder tf;
    if (!tf.open()) return false;
    BSTR name = SysAllocString(kTaskName);
    HRESULT hr = tf.folder->DeleteTask(name, 0);
    SysFreeString(name);
    return SUCCEEDED(hr);
}

// Re-run this executable elevated (UAC) with the given arguments. Returns false if the user
// declined the prompt.
static bool runElevated(const wchar_t* args, bool wait) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = args;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return false;
    if (wait && sei.hProcess) WaitForSingleObject(sei.hProcess, 60000);
    if (sei.hProcess) CloseHandle(sei.hProcess);
    return true;
}

struct TaskJob { HWND hwnd; bool enable; };

static DWORD WINAPI TaskJobThread(LPVOID p) {   // keeps the UI thread free while the UAC prompt is open
    TaskJob* job = (TaskJob*)p;
    bool ok = runElevated(job->enable ? L"--task-on" : L"--task-off", true);
    PostMessageW(job->hwnd, WM_TASK_DONE, ok, job->enable);
    delete job;
    return 0;
}

// ---------------------------------------------------------------- tray
static HWND g_hwnd;
static NOTIFYICONDATAW g_nid;
static HICON g_iconOn, g_iconOff;
static UINT g_taskbarCreated;
static std::atomic<bool> g_taskBusy{false};

static HICON makeIcon(COLORREF ring, bool dot) {
    int s = GetSystemMetrics(SM_CXSMICON);
    if (s < 16) s = 16;
    HDC dc = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP color = CreateCompatibleBitmap(dc, s, s);
    HBITMAP mask = CreateBitmap(s, s, 1, 1, nullptr);
    ReleaseDC(nullptr, dc);
    int m = std::max(1, s / 10), pw = std::max(2, s / 8);
    // Colour bitmap: a ring, plus a centre dot when enabled.
    HGDIOBJ old = SelectObject(mem, color);
    HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
    RECT rc = {0, 0, s, s};
    FillRect(mem, &rc, bg);
    DeleteObject(bg);
    HPEN pen = CreatePen(PS_SOLID, pw, ring);
    HGDIOBJ op = SelectObject(mem, pen);
    HGDIOBJ ob = SelectObject(mem, GetStockObject(NULL_BRUSH));
    Ellipse(mem, m, m, s - m, s - m);
    if (dot) {
        HBRUSH fb = CreateSolidBrush(ring);
        SelectObject(mem, fb);
        SelectObject(mem, GetStockObject(NULL_PEN));
        int c0 = s / 2, rr = std::max(2, s / 5);
        Ellipse(mem, c0 - rr, c0 - rr, c0 + rr, c0 + rr);
        SelectObject(mem, GetStockObject(NULL_BRUSH));
        DeleteObject(fb);
    }
    SelectObject(mem, op); SelectObject(mem, ob);
    DeleteObject(pen);
    // Mask bitmap: 0 = opaque, 1 = transparent.
    SelectObject(mem, mask);
    FillRect(mem, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
    SelectObject(mem, GetStockObject(BLACK_BRUSH));
    SelectObject(mem, GetStockObject(BLACK_PEN));
    Ellipse(mem, m - 1, m - 1, s - m + 1, s - m + 1);
    SelectObject(mem, old);
    DeleteDC(mem);
    ICONINFO ii = {TRUE, 0, 0, mask, color};
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(color); DeleteObject(mask);
    return ic;
}

static void updateTray(bool add) {
    g_nid.hIcon = g_enabled ? g_iconOn : g_iconOff;
    std::wstring tip = std::wstring(kAppName) + (g_enabled ? L": on (" : L": off (");
    tip += g_hotkeyText + L")";
    if (g_elevated) tip += L" [admin]";
    lstrcpynW(g_nid.szTip, tip.c_str(), 128);
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &g_nid);
}

static void balloon(const wchar_t* text, DWORD icon = NIIF_WARNING) {
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = icon;
    lstrcpynW(n.szInfoTitle, kAppName, 64);
    lstrcpynW(n.szInfo, text, 256);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

static void setEnabled(bool on) {
    g_enabled = on;
    resetInputState();
    updateTray(false);
}

static void registerHotkey() {
    UnregisterHotKey(g_hwnd, 1);
    UINT mods = MOD_NOREPEAT | (g_hotkey.ctrl ? MOD_CONTROL : 0) | (g_hotkey.alt ? MOD_ALT : 0) |
                (g_hotkey.shift ? MOD_SHIFT : 0) | (g_hotkey.win ? MOD_WIN : 0);
    if (!RegisterHotKey(g_hwnd, 1, mods, g_hotkey.vk))
        balloon(L"Could not register the hotkey (it may be in use by another program). Change it in config.ini.");
}

static void showMenu() {
    HMENU m = CreatePopupMenu();
    t2t::Tuning t;
    EnterCriticalSection(&g_cs);
    t = g_tun;
    LeaveCriticalSection(&g_cs);
    std::wstring title = L"Joystick mode  (" + g_hotkeyText + L")";
    AppendMenuW(m, MF_STRING | (g_enabled ? MF_CHECKED : 0), ID_TOGGLE, title.c_str());
    HMENU sp = CreatePopupMenu();
    const wchar_t* names[3] = {L"Slow", L"Medium", L"Fast"};
    for (int i = 0; i < 3; i++)
        AppendMenuW(sp, MF_STRING | (t.preset == i ? MF_CHECKED : 0), ID_SPEED0 + i, names[i]);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sp, L"Speed");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    bool task = taskExists();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0,
                g_elevated ? L"Running as administrator" : L"Running as a standard user (admin windows are not covered)");
    if (!g_elevated) AppendMenuW(m, MF_STRING, ID_RELAUNCH_ADMIN, L"Restart as administrator");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (task ? MF_GRAYED : 0) | (autostartOn() ? MF_CHECKED : 0), ID_AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_STRING | (task ? MF_CHECKED : 0) | (g_taskBusy ? MF_GRAYED : 0), ID_TASK,
                L"Start with Windows as administrator (no UAC prompt)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_OPENCFG, L"Open config.ini");
    AppendMenuW(m, MF_STRING, ID_RELOAD, L"Reload config");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);  // also destroys the submenu
}

static void openConfig() {
    HINSTANCE r = ShellExecuteW(nullptr, L"edit", g_iniPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + g_iniPath + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

static void onTaskToggle() {
    if (g_taskBusy) return;
    bool enable = !taskExists();
    if (enable && !inProtectedFolder()) {
        MessageBoxW(g_hwnd, kAdminTaskRefusal, kAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (g_elevated) {
        bool ok = enable ? taskInstall() : taskRemove();
        if (!ok) balloon(enable ? L"Could not create the administrator startup task." : L"Could not remove the administrator startup task.");
        return;
    }
    g_taskBusy = true;
    TaskJob* job = new TaskJob{g_hwnd, enable};
    HANDLE th = CreateThread(nullptr, 0, TaskJobThread, job, 0, nullptr);
    if (th) CloseHandle(th);
    else { delete job; g_taskBusy = false; }
}

static std::vector<unsigned char> g_rawBuf(1024);   // reused for every WM_INPUT (UI thread only)

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INPUT: {
        UINT size = (UINT)g_rawBuf.size();
        UINT got = GetRawInputData((HRAWINPUT)lp, RID_INPUT, g_rawBuf.data(), &size, sizeof(RAWINPUTHEADER));
        if (got == (UINT)-1 && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            g_rawBuf.resize(size + 64);
            size = (UINT)g_rawBuf.size();
            got = GetRawInputData((HRAWINPUT)lp, RID_INPUT, g_rawBuf.data(), &size, sizeof(RAWINPUTHEADER));
        }
        if (got != (UINT)-1) {
            auto* ri = (RAWINPUT*)g_rawBuf.data();
            if (ri->header.dwType == RIM_TYPEHID) onHid(ri);
            // hDevice == NULL means injected input (our own SendInput), which must not count as a real mouse.
            else if (ri->header.dwType == RIM_TYPEMOUSE && ri->header.hDevice &&
                     (ri->data.mouse.lLastX || ri->data.mouse.lLastY) &&
                     !(ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) && !isTouchpadMouse(ri->header.hDevice))
                g_lastRawMouse = GetTickCount64();
        }
        break;   // DefWindowProc must still see WM_INPUT
    }
    case WM_INPUT_DEVICE_CHANGE:
        if (wp == 2 /*GIDC_REMOVAL*/) {
            g_devs.erase((HANDLE)lp);
            resetInputState();
        }
        refreshTouchpads();
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMSUSPEND) resetInputState();
        else if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) { resetInputState(); reinstallHook(); }
        return TRUE;
    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_LOCK) resetInputState();
        else if (wp == WTS_SESSION_UNLOCK) { resetInputState(); reinstallHook(); }
        return 0;
    case WM_HOTKEY:
        setEnabled(!g_enabled);
        return 0;
    case WM_TRAY:
        if (lp == WM_LBUTTONUP) setEnabled(!g_enabled);
        else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) showMenu();
        return 0;
    case WM_TASK_DONE:
        g_taskBusy = false;
        if (!wp) balloon(L"The administrator prompt was cancelled or failed.");
        else if (lp && !taskExists()) balloon(L"Could not create the administrator startup task.");
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == ID_TOGGLE) setEnabled(!g_enabled);
        else if (id >= ID_SPEED0 && id < ID_SPEED0 + 3) {
            EnterCriticalSection(&g_cs);
            g_tun.preset = id - ID_SPEED0;
            LeaveCriticalSection(&g_cs);
            WritePrivateProfileStringW(L"joystick", L"speed_preset", std::to_wstring(id - ID_SPEED0).c_str(), g_iniPath.c_str());
        } else if (id == ID_AUTOSTART) setAutostart(!autostartOn());
        else if (id == ID_RELAUNCH_ADMIN) {
            if (runElevated(L"--relaunch", false)) DestroyWindow(hwnd);   // the new copy waits for our mutex
        } else if (id == ID_TASK) onTaskToggle();
        else if (id == ID_OPENCFG) openConfig();
        else if (id == ID_RELOAD) { loadConfig(); registerHotkey(); updateTray(false); }
        else if (id == ID_EXIT) DestroyWindow(hwnd);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        if (msg == g_taskbarCreated) updateTray(true);                                   // Explorer restarted
        else if (msg == g_msgShow) { balloon(L"touch2track is already running. Use its tray icon.", NIIF_INFO); return 0; }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void optOutOfThrottling() {
    // Keep Windows from putting this tiny background process into efficiency mode / ignoring its
    // timer resolution; either would make the cursor stutter.
    typedef BOOL(WINAPI * SetProcInfo)(HANDLE, int, LPVOID, DWORD);
    auto fn = (SetProcInfo)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation");
    if (!fn) return;
    PROCESS_POWER_THROTTLING_STATE st = {};
    st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    st.StateMask = 0;
    fn(GetCurrentProcess(), 4 /*ProcessPowerThrottling*/, &st, sizeof(st));
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int) {
    std::wstring args = cmdLine ? cmdLine : L"";
    g_elevated = isElevated();

    // Helper modes: do one job and exit (--task-on/--task-off are launched elevated from the tray menu).
    if (args.find(L"--task-status") != std::wstring::npos) {   // exit code 0 = the startup task exists
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool exists = taskExists();
        CoUninitialize();
        return exists ? 0 : 1;
    }
    bool taskOn = args.find(L"--task-on") != std::wstring::npos;
    bool taskOff = args.find(L"--task-off") != std::wstring::npos;
    if (taskOn || taskOff) {
        if (!g_elevated) return 5;
        if (taskOn && !inProtectedFolder()) return 6;
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool ok = taskOn ? taskInstall() : taskRemove();
        CoUninitialize();
        return ok ? 0 : 7;
    }

    g_msgShow = RegisterWindowMessageW(L"touch2track.show");
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Local\\touch2track.singleton");
    DWORD err = GetLastError();
    if (!mtx || err == ERROR_ALREADY_EXISTS) {
        bool relaunch = args.find(L"--relaunch") != std::wstring::npos;
        if (mtx && relaunch) {
            // The old copy is shutting down: wait for it to release the mutex, then take over.
            DWORD w = WaitForSingleObject(mtx, 8000);
            if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) return 0;
        } else {
            // Already running (possibly elevated, in which case we could not even open its mutex): tell it.
            HWND other = FindWindowW(kWndClass, nullptr);
            if (other) PostMessageW(other, g_msgShow, 0, 0);
            if (mtx) CloseHandle(mtx);
            return 0;
        }
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    InitializeCriticalSection(&g_cs);
    optOutOfThrottling();
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);   // auto-reset
    g_hookReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_iniPath = resolveConfigPath();
    loadConfig();
    bool haveTouchpad = refreshTouchpads();

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.lpszClassName = kWndClass;
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(0, kWndClass, L"", 0, 0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_hwnd, g_msgShow, MSGFLT_ALLOW, nullptr);   // let a non-elevated copy reach an elevated one

    g_iconOn = makeIcon(RGB(60, 200, 90), true);
    g_iconOff = makeIcon(RGB(140, 140, 140), false);
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uCallbackMessage = WM_TRAY;
    updateTray(true);
    registerHotkey();
    WTSRegisterSessionNotification(g_hwnd, NOTIFY_FOR_THIS_SESSION);

    int exitCode = 0;
    RAWINPUTDEVICE rid[2] = {{0x0D, 0x05, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, g_hwnd},   // touchpad (+ arrival/removal)
                             {0x01, 0x02, RIDEV_INPUTSINK, g_hwnd}};                    // mice (so a real mouse can take priority)
    HANDLE hookThread = nullptr, workerThread = nullptr;
    if (!RegisterRawInputDevices(rid, 2, sizeof(rid[0]))) {
        MessageBoxW(nullptr, L"Failed to register for raw input.", kAppName, MB_OK | MB_ICONERROR);
        exitCode = 2;
    } else {
        hookThread = CreateThread(nullptr, 0, HookThread, nullptr, 0, &g_hookThreadId);
        if (hookThread) WaitForSingleObject(g_hookReady, 5000);
        if (!hookThread || !g_hookOk) {
            MessageBoxW(nullptr, L"Failed to install the mouse hook.", kAppName, MB_OK | MB_ICONERROR);
            exitCode = 3;
        }
    }
    if (!exitCode) {
        workerThread = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
        SetThreadPriority(workerThread, THREAD_PRIORITY_ABOVE_NORMAL);
        if (!haveTouchpad)
            balloon(L"No Windows Precision Touchpad found. touch2track will start working when one is connected.");
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }

    g_quit = true;
    signalWorker();
    if (workerThread) { WaitForSingleObject(workerThread, 1000); CloseHandle(workerThread); }
    if (hookThread) {
        PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
        WaitForSingleObject(hookThread, 1000);
        CloseHandle(hookThread);
    }
    WTSUnRegisterSessionNotification(g_hwnd);
    UnregisterHotKey(g_hwnd, 1);
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    DestroyIcon(g_iconOn);
    DestroyIcon(g_iconOff);
    CoUninitialize();
    ReleaseMutex(mtx);
    CloseHandle(mtx);
    return exitCode;
}
