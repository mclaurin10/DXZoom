# SmoothZoom-DDA

Full-screen magnification for Windows, inspired by macOS Accessibility Zoom. Hold Win+Scroll to smoothly zoom the entire desktop up to 10× — the viewport follows your cursor naturally with fluid, pointer-centered tracking. Native C++17, pure Win32, using the DXGI Desktop Duplication API and Direct3D 11.

This is a fork of [SmoothZoom](https://github.com/mclaurin10/SmoothZoom). The original project used the Windows Magnification API (`MagSetFullscreenTransform`, `MagSetInputTransform`, etc.). This fork replaces the Magnification API with the Desktop Duplication API (`IDXGIOutputDuplication`) and a Direct3D 11 rendering pipeline.

## Why This Fork Exists

The Windows Magnification API is legacy. Microsoft recommends the Desktop Duplication API or `Windows.Graphics.Capture` for new screen-capture applications. The Magnification API also imposes hard constraints: the binary must have `UIAccess="true"`, must be code-signed, and must be installed in a secure folder like `Program Files`. The Desktop Duplication API removes all of these requirements. The tradeoff is that Desktop Duplication requires building the entire capture, rendering, input-passthrough, and cursor-drawing pipeline from scratch — things the Magnification API provided for free.

## Key Features

- **Scroll-gesture zoom** — Hold Win and scroll to zoom in/out continuously
- **Pointer-centered viewport tracking** — The magnified view follows your cursor proportionally across the desktop
- **Keyboard shortcuts** — Win+Plus/Minus for animated step zoom, Win+Esc to reset
- **Animated transitions** — Ease-out zoom animations with retargeting and scroll-interrupts-animation
- **Temporary toggle** — Hold Ctrl+Alt to peek at zoom/unzoom, release to restore
- **Settings persistence** — JSON config file with hot-reload
- **System tray icon** — Right-click for settings window and exit
- **Crash recovery** — Crash kills the overlay window and the desktop returns to normal (inherently safe)

## Controls

| Shortcut | Action |
|----------|--------|
| Win + Scroll wheel | Zoom in/out (continuous) |
| Win + Plus / Minus | Zoom in/out (animated step) |
| Win + Esc | Reset to 1× (animated) |
| Ctrl+Alt (hold) | Temporary toggle (peek at zoom/unzoom) |
| Tray icon (right-click) | Settings and exit |

## Out of Scope

These features are not implemented in this fork:

- Multi-monitor support
- Color inversion / color effect shaders
- Keyboard focus following (UIA FocusMonitor)
- Text cursor / caret following (UIA CaretMonitor)

## Architecture Overview

Eight components across three layers, running on two threads:

```
Input Layer:   InputInterceptor · WinKeyManager
Logic Layer:   ZoomController · ViewportTracker · RenderLoop
Output Layer:  DDABridge (+ software cursor renderer)
Support Layer: SettingsManager · TrayUI
```

| Thread | Responsibility |
|--------|---------------|
| **Main** | Message pump, low-level hooks, TrayUI, app lifecycle |
| **Render** | Desktop Duplication capture, D3D11 rendering, DwmFlush() sync |

**DDABridge** replaces the original MagBridge. It encapsulates the Desktop Duplication capture pipeline and Direct3D 11 rendering pipeline, exposing the same `setTransform(float zoom, int xOff, int yOff)` interface so all upstream components are unchanged.

The overlay window uses `WS_EX_TRANSPARENT | WS_EX_LAYERED` for full input passthrough — all clicks, drags, and hovers land on the desktop beneath. At zoom 1.0×, the overlay is hidden and capture is paused (zero GPU cost).

## Build Requirements

- **Windows 10 version 2004+** (build 19041) for `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`
- **Visual Studio 2022** with C++ desktop workload and Windows SDK
- **CMake 3.20+**
- **x64 only**
- No UIAccess manifest required
- No code signing required
- No secure folder installation required

## How to Build and Run

```powershell
# Build (from x64 Native Tools Command Prompt for VS 2022)
scripts\build.bat

# Run
.\build\bin\Debug\SmoothZoom.exe
```

### Unit Tests

```
cd build
ctest -C Debug
```

Tests cover pure logic components (ZoomController, ViewportTracker, WinKeyManager) with no Win32 API dependencies.

## Known Limitations

1. **DRM-protected content** (Netflix in Edge, some games with anti-cheat) renders as black in the magnified view. The Magnification API handled this transparently because it operated inside DWM compositing. Desktop Duplication captures post-compositing and is subject to HDCP restrictions.
2. **Secure desktop inaccessible** — `IDXGIOutputDuplication` cannot capture Ctrl+Alt+Delete, UAC prompts, or the lock screen.
3. **Single-monitor only** in this fork.
