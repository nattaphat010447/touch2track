# touch2track

Turn your laptop touchpad into a **virtual trackpoint**.

Touch and hold anywhere on the touchpad, then nudge your finger a few millimetres away from where it landed. The cursor keeps gliding in that direction, faster the further you push, and stops when you lift your finger. No more swiping across the pad again and again to cross a big screen.

touch2track is a small, dependency-free Windows tray app written in C++ (Win32). It reads raw finger positions from Windows Precision Touchpads and does not need a driver.

## Features

- **Joystick mode by default**: the point where your finger first lands is the centre; the offset from it sets direction and speed.
- **Tunable response**: dead zone, full-deflection radius, maximum speed, speed-curve exponent and update rate are all configurable.
- **Normal touchpad behaviour is preserved**: clicks, tap-to-click, two-finger scrolling and multi-finger gestures keep working. Only single-finger cursor movement is replaced.
- **Hotkey toggle** (`Ctrl+Alt+J` by default) and a tray icon to switch back to the regular touchpad at any time.
- **External mouse takes priority**: while a USB or Bluetooth mouse is moving, the joystick steps aside.
- **Portable**: a single executable plus an auto-generated `config.ini`. Physical finger offsets are measured in millimetres, so it feels the same across touchpads of different sizes.

## Requirements

- Windows 10 or 11
- A **Windows Precision Touchpad**. Most laptops from the last several years have one. You can check under *Settings → Bluetooth & devices → Touchpad*; Precision Touchpads expose the full gesture options there.

## Install

touch2track has no installer. Build it from source (see [Building](#building)) and run `touch2track.exe`.

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
| Start with Windows as administrator | Registers a scheduled task that launches elevated at sign-in without a UAC prompt |
| Open config.ini / Reload config | Edit and apply settings without restarting |

## Configuration

`config.ini` is created next to the executable on first run. Use *Reload config* in the tray menu to apply changes.

```ini
[joystick]
deadzone_mm=0.8        ; finger offset ignored around the centre (mm), filters hand tremor
max_radius_mm=12       ; finger offset that counts as full deflection (mm)
max_speed=450          ; pixels per second at full deflection, before the speed preset
gamma=1.5              ; speed curve exponent: 1 = linear, higher = finer control near the centre
touch_delay_ms=70      ; a lone finger must rest this long before the joystick starts moving
mouse_hold_ms=150      ; a real mouse moved this recently => the joystick pauses
send_interval_ms=4     ; how often the cursor is updated (1-50 ms)
speed_preset=1         ; 0 = slow (x0.6), 1 = medium (x1.0), 2 = fast (x1.6)

[hotkey]
toggle=ctrl+alt+j      ; modifiers: ctrl, alt, shift, win; key: a letter, digit or F1-F24
```

### Tuning tips

- **Cursor drifts while the finger rests**: increase `deadzone_mm`.
- **Too fast or too slow**: change `max_speed`, or pick a preset from the tray menu.
- **Want finer control for small pushes**: increase `gamma` (for example 2.0).
- **Smoother motion**: lower `send_interval_ms`. The distance moved is computed from real elapsed time, so the interval changes smoothness and CPU use, not speed. `1` gives the smoothest result; the default `4` is a good balance.
- **Cursor creeps during two-finger scrolling**: increase `touch_delay_ms`.

## Limitations

- **Precision Touchpads only.** Touchpads running legacy vendor-only drivers are not detected; the app tells you at startup.
- **Elevated windows.** Windows blocks input hooks from reaching windows that run as administrator (Task Manager, installers, and so on) unless touch2track is elevated too. Use *Restart as administrator*, or *Start with Windows as administrator* to make it permanent. The app does not request administrator rights by default.
- **Pointer speed.** Cursor motion is sent as relative mouse input, so Windows' pointer speed and *Enhance pointer precision* settings still apply on top of `max_speed`.
- **Touchpad edges.** If you touch very close to an edge, there is less room to push in that direction. Start nearer the middle.
- **External mouse detection** compares device paths to tell the touchpad's own mouse collection from real mice. It may not match every touchpad model. If the joystick never activates, set `mouse_hold_ms=0` to disable the mouse-priority feature and please open an issue.

## Building

Requires CMake 3.16+, a C++17 compiler, and the Windows SDK or MinGW-w64.

MinGW-w64 with Ninja:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++
cmake --build build
```

Visual Studio (Developer Command Prompt):

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The executable is `touch2track.exe` inside the build directory (MinGW builds are statically linked).

## How it works

1. **Reading the touchpad.** The app registers for Raw Input on the touchpad HID collection (usage page `0x0D`, usage `0x05`) and decodes each report with the HID parser: contact count, tip switch, contact ID, and X/Y converted to millimetres using the device's reported physical range. Both full and "hybrid" report layouts are handled.
2. **Suppressing the built-in cursor.** A low-level mouse hook (`WH_MOUSE_LL`) discards non-injected `WM_MOUSEMOVE` events while exactly one finger is down. With zero or two or more fingers nothing is swallowed, so clicks, scrolling and gestures are untouched.
3. **Moving the cursor.** A worker thread on a high-resolution timer takes the vector from the first-touch origin to the current finger position, applies the dead zone and speed curve, and injects the movement with `SendInput`. Fractional pixels are accumulated so slow speeds stay smooth.
4. **Playing nicely with other input.** A short touch delay avoids reacting to the first finger of a two-finger gesture. Raw input from real mice pauses the joystick, while input injected by the app itself is ignored.

## Uninstall

Turn off both *Start with Windows* options in the tray menu, choose *Exit*, then delete the folder. The app only writes `config.ini` next to the executable, a `touch2track` entry in `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` (if enabled), and a scheduled task named `touch2track` (if enabled).

## License

[MIT](LICENSE)
