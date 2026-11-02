# touch2track

Turn your laptop touchpad into a **virtual trackpoint**.

Touch and hold anywhere on the touchpad, then nudge your finger a few millimetres away from where it landed. The cursor keeps gliding in that direction, faster the further you push, and stops when you lift your finger. No more swiping across the pad again and again to cross a big screen.

touch2track is a small Windows tray app written in C++ (Win32). It reads raw finger positions from Windows Precision Touchpads, needs no driver, and has no runtime dependencies.

## Features

- **Joystick mode by default**: the point where your finger first lands is the centre; the offset from it sets direction and speed.
- **Tunable response**: dead zone, full-deflection radius, maximum speed, speed-curve exponent and update rate are all configurable.
- **Normal touchpad behaviour is preserved**: clicks, tap-to-click, two-finger scrolling and multi-finger gestures keep working. Only single-finger cursor movement is replaced.
- **Palm rejection aware**: contacts the touchpad flags as palms are ignored, so a resting palm does not switch the joystick off or drive the cursor.
- **Hotkey toggle** (`Ctrl+Alt+J` by default) and a tray icon to switch back to the regular touchpad at any time.
- **External mouse takes priority**: while a USB or Bluetooth mouse is moving, the joystick steps aside.
- **Idle-friendly**: no timers run while no finger is on the pad.
- **Portable across touchpads**: finger offsets are measured in millimetres from the pad's reported physical size, so it feels the same on pads of different sizes.

## Requirements

- Windows 10 or 11
- A **Windows Precision Touchpad**. Most laptops from the last several years have one. You can check under *Settings → Bluetooth & devices → Touchpad*; Precision Touchpads expose the full gesture options there.

## Install

touch2track has no installer. Either:

- build it from source (see [Building](#building)), or
- download `touch2track.exe` from the **Artifacts** of a successful run of the *build* workflow under the repository's *Actions* tab.

Run `touch2track.exe`; a `config.ini` is created next to it on first start.

## Usage

1. Run `touch2track.exe`. A tray icon appears: **green ring with a dot = joystick on**, **grey ring = off**.
2. Touch and hold the touchpad, then nudge your finger. The first touch point is the centre.
3. Press `Ctrl+Alt+J` (or left-click the tray icon) to switch between joystick mode and the normal touchpad.

Right-click the tray icon for the menu:

| Item | What it does |
|---|---|
| Joystick mode | Toggle on/off |
| Speed | Slow / Medium / Fast preset (multiplies `max_speed`) |
| Restart as administrator | Needed to control elevated windows, see [Limitations](#limitations) |
| Start with Windows | Launch at sign-in as a standard user |
| Start with Windows as administrator | Registers a scheduled task that launches elevated at sign-in without a UAC prompt. Only available when the app is installed in a protected folder, see [Limitations](#limitations) |
| Open config.ini / Reload config | Edit and apply settings without restarting |

## Configuration

`config.ini` is created next to the executable on first run. If that folder is read-only (for example under `C:\Program Files`), it is created in `%APPDATA%\touch2track\` instead. Use *Reload config* in the tray menu to apply changes. Invalid or out-of-range values are replaced by the default or clamped; they never stop the app.

```ini
[joystick]
; Finger offset around the starting point that is ignored (mm). Filters hand tremor.
deadzone_mm=0.8
; Finger offset that counts as full deflection (mm).
max_radius_mm=12
; Cursor speed at full deflection, in pixels per second (before the speed preset).
max_speed=450
; Speed curve exponent: 1 = linear, higher = finer control near the centre.
gamma=1.5
; A lone finger must rest this long (ms) before the joystick starts moving.
; Prevents two-finger scrolling from nudging the cursor.
touch_delay_ms=70
; A real mouse moved this recently (ms) => the joystick pauses. 0 disables.
mouse_hold_ms=150
; No touchpad report for this long (ms) => treat the finger as lifted. 0 disables.
release_timeout_ms=500
; How often the cursor is updated (ms, 1-50).
send_interval_ms=4
; 0 = slow (x0.6), 1 = medium (x1.0), 2 = fast (x1.6). Also set from the tray menu.
speed_preset=1

[hotkey]
; Modifiers: ctrl, alt, shift, win. Key: a letter, digit, F1-F24, space, tab, ...
toggle=ctrl+alt+j
```

### Tuning tips

- **Cursor drifts while the finger rests**: increase `deadzone_mm`.
- **Too fast or too slow**: change `max_speed`, or pick a preset from the tray menu.
- **Want finer control for small pushes**: increase `gamma` (for example 2.0).
- **Smoother motion**: lower `send_interval_ms`. The distance moved is computed from real elapsed time, so the interval changes smoothness and CPU use, not speed. The cursor is updated on a high-resolution timer whose real period is a little longer than requested: on the machine used for development 4 ms gave about 4.3 ms and 1 ms about 1.5 ms (roughly 650 updates per second), so values below 2 give diminishing returns.
- **Cursor creeps during two-finger scrolling**: increase `touch_delay_ms`.

## Limitations

- **Precision Touchpads only.** Touchpads running legacy vendor-only drivers are not detected. If none is found the app says so in a tray notification and starts working as soon as one is connected.
- **Elevated windows.** Windows blocks input hooks from reaching windows that run as administrator (Task Manager, installers, and so on) unless touch2track is elevated too. Use *Restart as administrator* for the current session. The app never requests administrator rights on its own.
- **Administrator autostart needs a protected install folder.** *Start with Windows as administrator* is only offered when `touch2track.exe` lives under `Program Files` or the Windows folder. Otherwise any program running as you could replace the executable and be elevated at your next sign-in. Copy the exe to e.g. `C:\Program Files\touch2track\` first. The task is registered with the Task Scheduler API; remove it from the same menu item or by deleting the `touch2track` task in Task Scheduler.
- **Pointer speed.** Cursor motion is sent as relative mouse input, so Windows' pointer speed and *Enhance pointer precision* settings still apply on top of `max_speed`.
- **Touchpad edges.** If you touch very close to an edge, there is less room to push in that direction. Start nearer the middle.
- **External mouse detection** compares device paths to tell the touchpad's own mouse collection from real mice. It may not match every touchpad model. If the joystick never activates, set `mouse_hold_ms=0` to disable the mouse-priority feature and please open an issue.
- Only the touchpad models listed under [Tested hardware](#tested-hardware) have been verified; reports from other pads are welcome.

### Tested hardware

| Laptop | Touchpad | Notes |
|---|---|---|
| Acer Nitro 5 | ELAN Precision Touchpad (hybrid reports, palm confidence, up to 5 contacts) | Everything above verified |

## Building

Requires CMake 3.16+ and a C++17 compiler (MSVC or MinGW-w64). The Windows SDK / MinGW headers must include the HID and Task Scheduler headers (all current ones do).

MinGW-w64 with Ninja:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++
cmake --build build
ctest --test-dir build --output-on-failure
```

Visual Studio (Developer Command Prompt):

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The executable is `touch2track.exe` inside the build directory. Both toolchains produce a self-contained exe (static runtime). Pass `-DTOUCH2TRACK_BUILD_TESTS=OFF` to skip the tests.

## How it works

The code is split into a small platform-independent core ([`src/core.h`](src/core.h): frame assembly, speed curve, config parsing) and the Win32 shell ([`src/touch2track.cpp`](src/touch2track.cpp)).

1. **Reading the touchpad.** The app registers for Raw Input on the touchpad HID collection (usage page `0x0D`, usage `0x05`) and decodes each report with the HID parser: contact ID, tip switch, confidence, X/Y converted to millimetres using the device's reported physical range.
2. **Assembling frames.** A Precision Touchpad describes one scan of the pad as one or more reports. In "hybrid" mode each report carries a few contacts, only the first report holds the real contact count (the others say `0`), and all share the same scan time. Frames are therefore assembled per device by scan time, not by the count field alone. Palms are filtered out here.
3. **Suppressing the built-in cursor.** A low-level mouse hook (`WH_MOUSE_LL`), running on its own thread so nothing can delay it, discards non-injected `WM_MOUSEMOVE` events while exactly one finger is down. With zero or two or more fingers nothing is swallowed, so clicks, scrolling and gestures are untouched. Because Windows silently drops hooks that time out, the hook is re-installed after resume/unlock and periodically.
4. **Moving the cursor.** A worker thread sleeps until a single finger lands, then runs on a high-resolution timer: it takes the vector from the first-touch origin to the current finger position, applies the dead zone and speed curve, and injects the movement with `SendInput`. Fractional pixels are accumulated so slow speeds stay smooth.
5. **Staying safe.** A short touch delay avoids reacting to the first finger of a two-finger gesture. Raw input from real mice pauses the joystick, while input injected by the app itself is ignored. If the pad stops reporting while a finger is "down" (pad disabled, sleep, lock screen), the joystick releases by itself.

## Development

- `ctest` runs the unit tests in [`tests/core_test.cpp`](tests/core_test.cpp). They replay real hybrid-mode touchpad reports ([`tests/fixtures/touchpad_frames.txt`](tests/fixtures/touchpad_frames.txt)) through the frame assembler, including two- to five-finger frames, a lift frame and a palm contact, and cover the config and hotkey parsers and the speed curve.
- `touch2track.exe --task-status` exits with code 0 if the administrator startup task exists (handy for scripts).

## Uninstall

Turn off both *Start with Windows* options in the tray menu, choose *Exit*, then delete the folder (and `%APPDATA%\touch2track` if it exists). The app only writes `config.ini`, a `touch2track` entry in `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` (if enabled), and a scheduled task named `touch2track` (if enabled).

## License

[MIT](LICENSE)
