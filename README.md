# DXZoom

Full-screen magnification for Windows using the Desktop Duplication API. Hold Win+Scroll (or Shift+Scroll) to smoothly zoom the entire desktop up to 10× — the viewport follows your cursor naturally with fluid, pointer-centered tracking. No stepped increments, no jarring jumps. Native C++17, pure Win32, Direct3D 11.

## Why This Fork Exists

This is a fork of [SmoothZoom](https://github.com/original/smoothzoom), which used the Windows Magnification API. The Magnification API is legacy technology — Microsoft recommends the Desktop Duplication API or `Windows.Graphics.Capture` for new screen-capture applications. The Magnification API also imposed hard constraints: binaries must have `UIAccess="true"`, must be code-signed, and must run from secure folders like `Program Files`. The Desktop Duplication API removes all of these requirements.

The tradeoff is that Desktop Duplication requires building the capture, rendering, input-passthrough, and cursor-drawing pipeline from scratch — things the Magnification API provided for free. This fork implements that pipeline.

## Current Status

**Phase 0 — Capture and Render Spike** (planned). Implementation has not yet started.

## Features

- **Scroll-gesture zoom** — Hold Win (or Shift) and scroll to zoom in/out continuously
- **Pointer-centered viewport tracking** — The magnified view follows your cursor proportionally across the desktop
- **Keyboard shortcuts** — Win+Plus/Minus for animated step zoom, Win+Esc to reset
- **Animated transitions** — Ease-out zoom animations with retargeting and scroll-interrupts-animation
- **Temporary toggle** — Hold Ctrl+Alt to peek at zoom/unzoom, release to restore
- **Settings persistence** — JSON config file with hot-reload
- **System tray icon** — Right-click for settings window and exit
- **Crash recovery** — Exception handler and dirty-shutdown detection

## What's NOT Included (Out of Scope)

This fork focuses on single-monitor pointer-based zoom. The following features from the original SmoothZoom are **explicitly out of scope**:

- Multi-monitor support
- Color inversion / color effect shaders
- Keyboard focus following (UI Automation)
- Text cursor / caret following (UI Automation)

## Controls

| Shortcut | Action |
|----------|--------|
| Modifier + Scroll wheel | Zoom in/out (continuous, configurable: Win [default] / Shift / Ctrl / Alt) |
| Win + Plus / Minus | Zoom in/out (animated step) |
| Win + Esc | Reset to 1× (animated) |
| Ctrl+Alt (hold) | Temporary toggle (peek at zoom/unzoom) |
| Win+Ctrl+M | Toggle zoom on/off |
| Tray icon (right-click) | Settings and exit |

## Build Requirements

- **Windows 10 version 2004+** (build 19041) for `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`
- **Visual Studio 2022** with C++ desktop workload and Windows SDK
- **CMake 3.20+**
- **x64 only**
- **No code signing or UIAccess required** — the Desktop Duplication API does not need them

## How to Build and Run

```powershell
# Build (from x64 Native Tools Command Prompt for VS 2022)
scripts\build.bat

# Run
.\build\Debug\DXZoom.exe
```

### Unit Tests

```
cd build
ctest -C Debug
```

Tests cover pure logic components (ZoomController, ViewportTracker, WinKeyManager, ModifierUtils) with no Win32 API dependencies — safe to run on any machine including CI.

## Architecture Overview

Nine components across four layers, running on two threads:

```
Input Layer:   InputInterceptor · WinKeyManager
Logic Layer:   ZoomController · ViewportTracker · RenderLoop
Output Layer:  DDABridge · CursorRenderer
Support Layer: SettingsManager · TrayUI
```

| Thread | Responsibility |
|--------|---------------|
| **Main** | Message pump, low-level hooks, TrayUI, app lifecycle |
| **Render** | Desktop Duplication capture, D3D11 rendering, frame ticks via `DwmFlush()` |

Communication between threads uses atomics, SeqLock, lock-free queues, and atomic pointer swap — no mutexes on the render hot path.

See `CLAUDE.md` and `docs/` for detailed design documentation.

## Known Limitations

- **DRM-protected content appears black** — Netflix in Edge, some games with anti-cheat, and other HDCP-protected content will render as black in the magnified view. The Magnification API handled this transparently because it operated inside DWM compositing. Desktop Duplication captures post-compositing and is subject to HDCP restrictions. This is a known limitation of the Desktop Duplication API.
- **Secure desktop inaccessible** — Desktop Duplication cannot capture Ctrl+Alt+Delete, UAC prompts, or the lock screen (same limitation as the Magnification API).
- **Single-monitor only** — Multi-monitor support is out of scope for this fork.

## Phase Plan

| Phase | Name | Key Delivery | Status |
|-------|------|-------------|--------|
| 0 | Capture and Render Spike | Hardcoded 2× zoom overlay, 60fps capture, self-capture exclusion validation | Planned |
| 1 | Input Passthrough and Cursor | Click-through overlay, software cursor rendering | Planned |
| 2 | Scroll-Gesture Zoom | Win+Scroll zoom, proportional viewport tracking, Start Menu suppression | Planned |
| 3 | Keyboard, Toggle, Settings, Polish | Animated transitions, temporary toggle, settings UI, tray, crash recovery | Planned |
