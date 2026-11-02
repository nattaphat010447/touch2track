// touch2track - turn a Windows Precision Touchpad into a virtual trackpoint (joystick).
//
// Touch and hold, then nudge your finger away from where it first landed: the cursor keeps
// gliding in that direction, faster the further you push. Lift the finger to stop.
//
// How it works:
//   1. Raw Input (HID usage page 0x0D / usage 0x05) delivers the finger positions.
//   2. A low-level mouse hook swallows the touchpad's own cursor movement while exactly one
//      finger is down. Clicks, two-finger scroll and multi-finger gestures are untouched.
//   3. A worker thread turns the finger's offset from its origin into a velocity and moves
//      the cursor with SendInput.
//
// Controlled from a tray icon and a global hotkey (default Ctrl+Alt+J); settings live in
// config.ini next to the executable.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

static const wchar_t* kAppName = L"touch2track";

// ---------------------------------------------------------------- config
struct Config {
    double deadzoneMm = 0.8;    // finger offset that is ignored (hand tremor)
    double maxRadiusMm = 12.0;  // finger offset that counts as full deflection
    double maxSpeed = 450.0;    // pixels/second at full deflection (before the preset multiplier)
    double gamma = 1.5;         // speed curve exponent (1 = linear)
    double touchDelayMs = 70;   // single finger must rest this long before the joystick starts
                                // (stops two-finger scrolling from nudging the cursor)
    double mouseHoldMs = 150;   // a real mouse moved this recently => it wins over the joystick
    int sendIntervalMs = 4;     // how often the cursor is updated (1-50 ms)
    int preset = 1;             // 0 slow, 1 medium, 2 fast
    std::wstring hotkeyText = L"ctrl+alt+j";
    UINT hotMods = MOD_CONTROL | MOD_ALT;
    UINT hotVk = 'J';
};
static const double kPresetMul[3] = {0.6, 1.0, 1.6};

static Config g_cfg;
static std::atomic<int> g_mouseHold{150};   // copy of Config::mouseHoldMs for the hook
static std::atomic<int> g_intervalMs{4};    // copy of Config::sendIntervalMs for the worker
static CRITICAL_SECTION g_cs;               // guards g_cfg and the joystick state
static std::wstring g_iniPath;

static double iniDouble(const wchar_t* key, double def) {
    wchar_t buf[64];
    GetPrivateProfileStringW(L"joystick", key, L"", buf, 64, g_iniPath.c_str());
    return buf[0] ? wcstod(buf, nullptr) : def;
}

static bool parseHotkey(const std::wstring& text, UINT& mods, UINT& vk) {
    mods = 0; vk = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t e = text.find(L'+', pos);
        if (e == std::wstring::npos) e = text.size();
        std::wstring t = text.substr(pos, e - pos);
        for (auto& ch : t) ch = (wchar_t)towlower(ch);
        if (t == L"ctrl" || t == L"control") mods |= MOD_CONTROL;
        else if (t == L"alt") mods |= MOD_ALT;
        else if (t == L"shift") mods |= MOD_SHIFT;
        else if (t == L"win") mods |= MOD_WIN;
        else if (t.size() == 1) vk = (UINT)towupper(t[0]);
        else if (t.size() >= 2 && t[0] == L'f') {
            int n = _wtoi(t.c_str() + 1);
            if (n >= 1 && n <= 24) vk = VK_F1 + n - 1;
        }
        pos = e + 1;
    }
    return mods && vk;
}

static void loadConfig() {
    Config c;
    c.deadzoneMm = iniDouble(L"deadzone_mm", c.deadzoneMm);
    c.maxRadiusMm = iniDouble(L"max_radius_mm", c.maxRadiusMm);
    c.maxSpeed = iniDouble(L"max_speed", c.maxSpeed);
    c.gamma = iniDouble(L"gamma", c.gamma);
    c.touchDelayMs = iniDouble(L"touch_delay_ms", c.touchDelayMs);
    c.mouseHoldMs = iniDouble(L"mouse_hold_ms", c.mouseHoldMs);
    c.sendIntervalMs = (int)iniDouble(L"send_interval_ms", c.sendIntervalMs);
    if (c.sendIntervalMs < 1) c.sendIntervalMs = 1;
    if (c.sendIntervalMs > 50) c.sendIntervalMs = 50;
    c.preset = (int)iniDouble(L"speed_preset", c.preset);
    if (c.preset < 0 || c.preset > 2) c.preset = 1;
    if (c.maxRadiusMm <= c.deadzoneMm + 0.5) c.maxRadiusMm = c.deadzoneMm + 0.5;
    wchar_t hk[64];
    GetPrivateProfileStringW(L"hotkey", L"toggle", c.hotkeyText.c_str(), hk, 64, g_iniPath.c_str());
    UINT m, v;
    if (parseHotkey(hk, m, v)) { c.hotkeyText = hk; c.hotMods = m; c.hotVk = v; }
    EnterCriticalSection(&g_cs);
    g_cfg = c;
    g_mouseHold = (int)c.mouseHoldMs;
    g_intervalMs = c.sendIntervalMs;
    LeaveCriticalSection(&g_cs);
}

static void writeDefaultConfig() {
    if (GetFileAttributesW(g_iniPath.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    auto w = [](const wchar_t* s, const wchar_t* k, const wchar_t* v) {
        WritePrivateProfileStringW(s, k, v, g_iniPath.c_str());
    };
    w(L"joystick", L"deadzone_mm", L"0.8");
    w(L"joystick", L"max_radius_mm", L"12");
    w(L"joystick", L"max_speed", L"450");
    w(L"joystick", L"gamma", L"1.5");
    w(L"joystick", L"touch_delay_ms", L"70");
    w(L"joystick", L"mouse_hold_ms", L"150");
    w(L"joystick", L"send_interval_ms", L"4");
    w(L"joystick", L"speed_preset", L"1");
    w(L"hotkey", L"toggle", L"ctrl+alt+j");
}

// ---------------------------------------------------------------- HID touchpad
struct Link {
    USHORT link = 0;
    LONG xMin = 0, xMax = 1, yMin = 0, yMax = 1;
    double xMm = 0, yMm = 0;   // physical size of the pad along each axis
};
struct Device {
    std::vector<unsigned char> ppBuf;
    PHIDP_PREPARSED_DATA pp = nullptr;
    std::vector<Link> links;   // one per finger slot in the report
    bool hasCount = false;
    USHORT countLink = 0;
};
struct Contact { int id; bool tip; double mx, my; };

static std::map<HANDLE, Device> g_devs;
static std::vector<Contact> g_frame;
static ULONG g_expected = 0;

static std::atomic<bool> g_enabled{true};
static std::atomic<int> g_tips{0};
static std::atomic<ULONGLONG> g_lastTouch{0};
static std::atomic<ULONGLONG> g_lastRawMouse{0};  // last movement of a real (non-touchpad) mouse
static std::vector<std::wstring> g_tpKeys;        // touchpad device-path prefixes, to tell its mouse
                                                  // collection apart from external mice

// Joystick state (guarded by g_cs)
static bool g_haveOne = false;
static ULONGLONG g_singleSince = 0;  // when the current single-finger touch began
static int g_trackId = -1;
static double g_ox = 0, g_oy = 0, g_cx = 0, g_cy = 0;  // origin and current finger position (mm)

static double unitToMm(ULONG units, LONG exp, double phys) {
    exp &= 0xF;  // UnitsExp is a signed nibble (0xE = -2)
    if (exp > 7) exp -= 16;
    double f = std::pow(10.0, (double)exp);
    if ((units & 0xF) == 1 && ((units >> 4) & 0xF) == 1) return phys * f * 10.0;   // cm
    if ((units & 0xF) == 3 && ((units >> 4) & 0xF) == 1) return phys * f * 25.4;   // inch
    return phys * f;
}

static Device* getDevice(HANDLE h) {
    auto it = g_devs.find(h);
    if (it != g_devs.end()) return it->second.pp ? &it->second : nullptr;
    Device d;
    UINT size = 0;
    GetRawInputDeviceInfoW(h, RIDI_PREPARSEDDATA, nullptr, &size);
    if (size) {
        d.ppBuf.resize(size);
        if (GetRawInputDeviceInfoW(h, RIDI_PREPARSEDDATA, d.ppBuf.data(), &size) != (UINT)-1) {
            d.pp = (PHIDP_PREPARSED_DATA)d.ppBuf.data();
            HIDP_CAPS caps;
            USHORT n = 0;
            std::vector<HIDP_VALUE_CAPS> vc;
            if (HidP_GetCaps(d.pp, &caps) == HIDP_STATUS_SUCCESS) {
                n = caps.NumberInputValueCaps;
                vc.resize(n);
                if (!n || HidP_GetValueCaps(HidP_Input, vc.data(), &n, d.pp) != HIDP_STATUS_SUCCESS) d.pp = nullptr;
            } else d.pp = nullptr;
            if (d.pp) {
                std::map<USHORT, Link> lm;
                for (USHORT i = 0; i < n; i++) {
                    auto& c = vc[i];
                    if (c.IsRange) continue;
                    USHORT u = c.NotRange.Usage;
                    if (c.UsagePage == 0x01 && (u == 0x30 || u == 0x31)) {        // X / Y
                        Link& l = lm[c.LinkCollection];
                        l.link = c.LinkCollection;
                        double mm = unitToMm(c.Units, c.UnitsExp, (double)(c.PhysicalMax - c.PhysicalMin));
                        if (u == 0x30) { l.xMin = c.LogicalMin; l.xMax = c.LogicalMax; l.xMm = mm; }
                        else           { l.yMin = c.LogicalMin; l.yMax = c.LogicalMax; l.yMm = mm; }
                    } else if (c.UsagePage == 0x0D && u == 0x54) {                // contact count
                        d.hasCount = true; d.countLink = c.LinkCollection;
                    }
                }
                for (auto& kv : lm) if (kv.second.xMm > 0 && kv.second.yMm > 0) d.links.push_back(kv.second);
                if (d.links.empty()) d.pp = nullptr;
            }
        }
    }
    auto& slot = g_devs[h];
    slot = std::move(d);
    // The buffer moved with the struct, so the preparsed-data pointer must be re-pointed.
    if (slot.pp) slot.pp = (PHIDP_PREPARSED_DATA)slot.ppBuf.data();
    return slot.pp ? &slot : nullptr;
}

static void resetJoystick() {
    g_haveOne = false; g_trackId = -1;
}

static void handleFrame() {
    int tips = 0;
    for (auto& c : g_frame) if (c.tip) tips++;
    g_tips = tips;
    if (tips) g_lastTouch = GetTickCount64();
    EnterCriticalSection(&g_cs);
    if (tips == 1) {
        for (auto& c : g_frame) if (c.tip) {
            if (!g_haveOne || c.id != g_trackId) {   // new single-finger touch: this is the origin
                g_ox = c.mx; g_oy = c.my; g_trackId = c.id;
                g_singleSince = GetTickCount64();
            }
            g_cx = c.mx; g_cy = c.my; g_haveOne = true;
        }
    } else {
        resetJoystick();
    }
    LeaveCriticalSection(&g_cs);
}

static void onHid(RAWINPUT* ri) {
    Device* d = getDevice(ri->header.hDevice);
    if (!d) return;
    RAWHID& h = ri->data.hid;
    for (DWORD r = 0; r < h.dwCount; r++) {
        PCHAR rep = (PCHAR)(h.bRawData + r * h.dwSizeHid);
        ULONG size = h.dwSizeHid, count = 0;
        // Hybrid mode: only the first report of a frame carries the contact count; the
        // following reports carry just the remaining fingers.
        bool first = d->hasCount &&
            HidP_GetUsageValue(HidP_Input, 0x0D, d->countLink, 0x54, &count, d->pp, rep, size) == HIDP_STATUS_SUCCESS;
        if (first) { g_frame.clear(); g_expected = count; }
        bool any = false;
        for (auto& l : d->links) {
            ULONG x, y, id = 0;
            if (HidP_GetUsageValue(HidP_Input, 0x01, l.link, 0x30, &x, d->pp, rep, size) != HIDP_STATUS_SUCCESS) continue;
            if (HidP_GetUsageValue(HidP_Input, 0x01, l.link, 0x31, &y, d->pp, rep, size) != HIDP_STATUS_SUCCESS) continue;
            HidP_GetUsageValue(HidP_Input, 0x0D, l.link, 0x51, &id, d->pp, rep, size);
            USAGE us[16]; ULONG ul = 16;
            bool tip = false;
            if (HidP_GetUsages(HidP_Input, 0x0D, l.link, us, &ul, d->pp, rep, size) == HIDP_STATUS_SUCCESS)
                for (ULONG k = 0; k < ul; k++) if (us[k] == 0x42) tip = true;   // tip switch
            Contact c;
            c.id = (int)id; c.tip = tip;
            c.mx = l.xMax > l.xMin ? (double)((LONG)x - l.xMin) / (l.xMax - l.xMin) * l.xMm : 0;
            c.my = l.yMax > l.yMin ? (double)((LONG)y - l.yMin) / (l.yMax - l.yMin) * l.yMm : 0;
            g_frame.push_back(c);
            any = true;
        }
        if (!any && !first) continue;
        if (g_frame.size() >= g_expected) handleFrame();
    }
}

// ---------------------------------------------------------------- mouse hook
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

// ---------------------------------------------------------------- worker thread
static std::atomic<bool> g_quit{false};

static DWORD WINAPI MoverThread(LPVOID) {
    HANDLE t = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    if (!t) {
        t = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        timeBeginPeriod(1);
    }
    LARGE_INTEGER freq, last;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    int interval = 0;
    double accx = 0, accy = 0;   // sub-pixel remainder carried between updates
    while (!g_quit) {
        int want = g_intervalMs;
        if (want != interval) {  // (re)arm the timer when the interval changes
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)want * 10000;
            SetWaitableTimer(t, &due, want, nullptr, nullptr, FALSE);
            interval = want;
        }
        WaitForSingleObject(t, 50);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = (double)(now.QuadPart - last.QuadPart) / (double)freq.QuadPart;
        last = now;
        if (dt > 0.05) dt = 0.05;

        EnterCriticalSection(&g_cs);
        Config c = g_cfg;
        ULONGLONG tick = GetTickCount64();
        bool active = g_enabled && g_haveOne &&
                      tick - g_singleSince >= (ULONGLONG)c.touchDelayMs &&   // not a second finger still landing
                      tick - g_lastRawMouse >= (ULONGLONG)c.mouseHoldMs;     // real mouse has priority
        double dx = g_cx - g_ox, dy = g_cy - g_oy;
        LeaveCriticalSection(&g_cs);
        if (!active) { accx = accy = 0; continue; }

        double r = std::sqrt(dx * dx + dy * dy);
        if (r < c.deadzoneMm) { accx = accy = 0; continue; }
        double k = std::fmin(1.0, (r - c.deadzoneMm) / (c.maxRadiusMm - c.deadzoneMm));
        double v = c.maxSpeed * kPresetMul[c.preset] * std::pow(k, c.gamma);
        accx += dx / r * v * dt;
        accy += dy / r * v * dt;
        int ix = (int)accx, iy = (int)accy;   // truncate toward zero, keep the fraction for next time
        if (!ix && !iy) continue;
        accx -= ix; accy -= iy;
        INPUT in = {};
        in.type = INPUT_MOUSE;
        in.mi.dx = ix; in.mi.dy = iy;
        in.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &in, sizeof(in));
    }
    CloseHandle(t);
    return 0;
}

// ---------------------------------------------------------------- elevation / autostart helpers
enum { WM_TRAY = WM_APP + 1, ID_TOGGLE = 1, ID_SPEED0 = 10, ID_AUTOSTART = 20, ID_RELOAD = 21, ID_OPENCFG = 22,
       ID_RELAUNCH_ADMIN = 23, ID_TASK = 24, ID_EXIT = 99 };
static const wchar_t* kTaskName = L"touch2track";
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunName = L"touch2track";

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

static int runHidden(const std::wstring& cmd) {
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    std::wstring c = cmd;
    if (!CreateProcessW(nullptr, &c[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)code;
}

static bool taskExists() {
    return runHidden(std::wstring(L"schtasks.exe /query /tn ") + kTaskName) == 0;
}

static std::wstring xmlEsc(const std::wstring& s) {
    std::wstring o;
    for (wchar_t ch : s) {
        if (ch == L'&') o += L"&amp;"; else if (ch == L'<') o += L"&lt;"; else if (ch == L'>') o += L"&gt;"; else o += ch;
    }
    return o;
}

static void removeRunKey() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_WRITE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, kRunName);
        RegCloseKey(k);
    }
}

// Must run elevated: registers a logon task with highest privileges so the app starts as
// administrator without a UAC prompt every time.
static bool taskInstall() {
    wchar_t exe[MAX_PATH], user[256] = L"", dom[256] = L"", tmp[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    DWORD un = 256;
    GetUserNameW(user, &un);
    GetEnvironmentVariableW(L"USERDOMAIN", dom, 256);
    std::wstring who = std::wstring(dom) + L"\\" + user;
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
        L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + xmlEsc(who) + L"</UserId></LogonTrigger></Triggers>"
        L"<Principals><Principal id=\"Author\"><UserId>" + xmlEsc(who) + L"</UserId>"
        L"<LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
        L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
        L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><StartWhenAvailable>true</StartWhenAvailable><Enabled>true</Enabled></Settings>"
        L"<Actions Context=\"Author\"><Exec><Command>" + xmlEsc(exe) + L"</Command></Exec></Actions></Task>";
    GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"touch2track_task.xml";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w;
    WORD bom = 0xFEFF;
    WriteFile(f, &bom, 2, &w, nullptr);
    WriteFile(f, xml.c_str(), (DWORD)(xml.size() * sizeof(wchar_t)), &w, nullptr);
    CloseHandle(f);
    int rc = runHidden(std::wstring(L"schtasks.exe /create /tn ") + kTaskName + L" /xml \"" + path + L"\" /f");
    DeleteFileW(path.c_str());
    if (rc == 0) removeRunKey();  // avoid starting twice (a non-elevated copy could win the mutex)
    return rc == 0;
}

static void taskRemove() {
    runHidden(std::wstring(L"schtasks.exe /delete /tn ") + kTaskName + L" /f");
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
    if (wait && sei.hProcess) WaitForSingleObject(sei.hProcess, 30000);
    if (sei.hProcess) CloseHandle(sei.hProcess);
    return true;
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

// ---------------------------------------------------------------- tray
static HWND g_hwnd;
static NOTIFYICONDATAW g_nid;
static HICON g_iconOn, g_iconOff;
static UINT g_taskbarCreated;
static HHOOK g_hook;

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
    tip += g_cfg.hotkeyText + L")";
    if (isElevated()) tip += L" [admin]";
    wcsncpy(g_nid.szTip, tip.c_str(), 127);
    g_nid.szTip[127] = 0;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &g_nid);
}

static void balloon(const wchar_t* text) {
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_WARNING;
    wcsncpy(n.szInfoTitle, kAppName, 63);
    wcsncpy(n.szInfo, text, 255);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

static void setEnabled(bool on) {
    g_enabled = on;
    EnterCriticalSection(&g_cs);
    resetJoystick();
    LeaveCriticalSection(&g_cs);
    updateTray(false);
}

static void registerHotkey() {
    UnregisterHotKey(g_hwnd, 1);
    EnterCriticalSection(&g_cs);
    UINT m = g_cfg.hotMods, v = g_cfg.hotVk;
    LeaveCriticalSection(&g_cs);
    if (!RegisterHotKey(g_hwnd, 1, m | MOD_NOREPEAT, v))
        balloon(L"Could not register the hotkey (it may be in use by another program). Change it in config.ini.");
}

static void showMenu() {
    HMENU m = CreatePopupMenu();
    std::wstring t = L"Joystick mode  (" + g_cfg.hotkeyText + L")";
    AppendMenuW(m, MF_STRING | (g_enabled ? MF_CHECKED : 0), ID_TOGGLE, t.c_str());
    HMENU sp = CreatePopupMenu();
    const wchar_t* names[3] = {L"Slow", L"Medium", L"Fast"};
    for (int i = 0; i < 3; i++)
        AppendMenuW(sp, MF_STRING | (g_cfg.preset == i ? MF_CHECKED : 0), ID_SPEED0 + i, names[i]);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sp, L"Speed");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    bool elevated = isElevated(), task = taskExists();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0,
                elevated ? L"Running as administrator" : L"Running as a standard user (admin windows are not covered)");
    if (!elevated) AppendMenuW(m, MF_STRING, ID_RELAUNCH_ADMIN, L"Restart as administrator");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (task ? MF_GRAYED : 0) | (autostartOn() ? MF_CHECKED : 0), ID_AUTOSTART,
                L"Start with Windows");
    AppendMenuW(m, MF_STRING | (task ? MF_CHECKED : 0), ID_TASK, L"Start with Windows as administrator (no UAC prompt)");
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

// True if this raw-mouse device is the touchpad's own mouse collection rather than a real mouse.
static bool isTouchpadMouse(HANDLE h) {
    static std::map<HANDLE, bool> cache;
    auto it = cache.find(h);
    if (it != cache.end()) return it->second;
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
    return cache[h] = tp;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INPUT: {
        UINT size = 0;
        GetRawInputData((HRAWINPUT)lp, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
        std::vector<unsigned char> buf(size);
        if (size && GetRawInputData((HRAWINPUT)lp, RID_INPUT, buf.data(), &size, sizeof(RAWINPUTHEADER)) == size) {
            auto* ri = (RAWINPUT*)buf.data();
            if (ri->header.dwType == RIM_TYPEHID) onHid(ri);
            // hDevice == NULL means injected input (our own SendInput), which must not count as a real mouse.
            else if (ri->header.dwType == RIM_TYPEMOUSE && ri->header.hDevice &&
                     (ri->data.mouse.lLastX || ri->data.mouse.lLastY) &&
                     !(ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) && !isTouchpadMouse(ri->header.hDevice))
                g_lastRawMouse = GetTickCount64();
        }
        break;
    }
    case WM_HOTKEY:
        setEnabled(!g_enabled);
        return 0;
    case WM_TRAY:
        if (lp == WM_LBUTTONUP) setEnabled(!g_enabled);
        else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) showMenu();
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == ID_TOGGLE) setEnabled(!g_enabled);
        else if (id >= ID_SPEED0 && id < ID_SPEED0 + 3) {
            EnterCriticalSection(&g_cs);
            g_cfg.preset = id - ID_SPEED0;
            LeaveCriticalSection(&g_cs);
            wchar_t b[8];
            _itow(id - ID_SPEED0, b, 10);
            WritePrivateProfileStringW(L"joystick", L"speed_preset", b, g_iniPath.c_str());
        } else if (id == ID_AUTOSTART) setAutostart(!autostartOn());
        else if (id == ID_RELAUNCH_ADMIN) {
            if (runElevated(L"--relaunch", false)) DestroyWindow(hwnd);  // the new copy waits for our mutex
        } else if (id == ID_TASK) {
            bool on = !taskExists();
            if (isElevated()) { if (on) taskInstall(); else taskRemove(); }
            else runElevated(on ? L"--task-on" : L"--task-off", true);
            if (on && !taskExists()) balloon(L"Could not create the administrator startup task.");
        }
        else if (id == ID_OPENCFG) ShellExecuteW(nullptr, L"open", g_iniPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else if (id == ID_RELOAD) { loadConfig(); registerHotkey(); updateTray(false); }
        else if (id == ID_EXIT) DestroyWindow(hwnd);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        if (msg == g_taskbarCreated) updateTray(true);  // Explorer restarted: re-add the icon
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool hasPrecisionTouchpad() {
    UINT n = 0;
    GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST));
    std::vector<RAWINPUTDEVICELIST> list(n);
    if (!n || GetRawInputDeviceList(list.data(), &n, sizeof(RAWINPUTDEVICELIST)) == (UINT)-1) return false;
    bool found = false;
    for (auto& e : list) {
        if (e.dwType != RIM_TYPEHID) continue;
        RID_DEVICE_INFO info; info.cbSize = sizeof(info); UINT sz = sizeof(info);
        if (GetRawInputDeviceInfoW(e.hDevice, RIDI_DEVICEINFO, &info, &sz) == (UINT)-1) continue;
        if (info.hid.usUsagePage == 0x0D && info.hid.usUsage == 0x05) {
            found = true;
            // Device paths look like \\?\hid#elan0xxx&col01#...; keep the part before "&col" so the
            // touchpad's mouse collection (&col02) can be recognised later.
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
    }
    return found;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int) {
    std::wstring args = cmdLine ? cmdLine : L"";
    // Helper modes: do one job and exit (launched elevated from the tray menu).
    if (args.find(L"--task-on") != std::wstring::npos) return isElevated() && taskInstall() ? 0 : 5;
    if (args.find(L"--task-off") != std::wstring::npos) { if (!isElevated()) return 5; taskRemove(); return 0; }

    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Local\\touch2track.singleton");
    DWORD err = GetLastError();
    if (!mtx && err == ERROR_ACCESS_DENIED) return 0;  // an elevated copy owns the mutex and we cannot open it
    if (err == ERROR_ALREADY_EXISTS) {
        // --relaunch: wait for the old copy (which is shutting down) to release the mutex.
        if (args.find(L"--relaunch") == std::wstring::npos) return 0;
        DWORD w = WaitForSingleObject(mtx, 8000);
        if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) return 0;
    }

    SetProcessDPIAware();
    InitializeCriticalSection(&g_cs);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_iniPath = exe;
    g_iniPath = g_iniPath.substr(0, g_iniPath.find_last_of(L"\\/") + 1) + L"config.ini";
    writeDefaultConfig();
    loadConfig();

    if (!hasPrecisionTouchpad()) {
        MessageBoxW(nullptr,
            L"No Windows Precision Touchpad was found on this computer.\n"
            L"touch2track only supports Precision Touchpads (see Settings > Bluetooth & devices > Touchpad).",
            kAppName, MB_OK | MB_ICONWARNING);
        return 1;
    }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.lpszClassName = L"touch2track.window";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    g_iconOn = makeIcon(RGB(60, 200, 90), true);
    g_iconOff = makeIcon(RGB(140, 140, 140), false);
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uCallbackMessage = WM_TRAY;
    updateTray(true);
    registerHotkey();

    RAWINPUTDEVICE rid[2] = {{0x0D, 0x05, RIDEV_INPUTSINK, g_hwnd},   // touchpad
                             {0x01, 0x02, RIDEV_INPUTSINK, g_hwnd}};   // mice (so a real mouse can take priority)
    if (!RegisterRawInputDevices(rid, 2, sizeof(rid[0]))) {
        MessageBoxW(nullptr, L"Failed to register for raw input.", kAppName, MB_OK | MB_ICONERROR);
        return 2;
    }
    g_hook = SetWindowsHookExW(WH_MOUSE_LL, LLMouse, hInst, 0);
    if (!g_hook) {
        MessageBoxW(nullptr, L"Failed to install the mouse hook.", kAppName, MB_OK | MB_ICONERROR);
        return 3;
    }
    HANDLE th = CreateThread(nullptr, 0, MoverThread, nullptr, 0, nullptr);
    SetThreadPriority(th, THREAD_PRIORITY_ABOVE_NORMAL);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }

    g_quit = true;
    WaitForSingleObject(th, 500);
    UnhookWindowsHookEx(g_hook);
    UnregisterHotKey(g_hwnd, 1);
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    ReleaseMutex(mtx);
    return 0;
}
