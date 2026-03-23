# SmoothZoom-DDA — Project-Wide Constraints

## API Prohibition

Do NOT call any Magnification API functions: `MagInitialize`, `MagUninitialize`, `MagSetFullscreenTransform`, `MagGetFullscreenTransform`, `MagSetInputTransform`, `MagSetFullscreenColorEffect`, `MagShowSystemCursor`. Do NOT `#include <Magnification.h>`. Do NOT link `Magnification.lib`. The Magnification API is not used in this fork.

## Scope Prohibition

Do NOT implement:
- Multi-monitor support
- Color inversion / color effect shaders
- Keyboard focus following (FocusMonitor / UIA)
- Text cursor / caret following (CaretMonitor / UIA)
- A UIA thread

These are permanently out of scope for this fork.

## Framework Prohibition

Do NOT add external frameworks: no Qt, no Electron, no WPF, no Dear ImGui. Pure Win32 + Direct3D 11 + C++17 + MSVC only. nlohmann/json (header-only, vendored) is the sole allowed dependency.

## Component Isolation

**DDABridge is the only component that touches DXGI or Direct3D 11.** No other component should `#include` D3D/DXGI headers or call D3D/DXGI functions. This isolates the capture/render pipeline to one file.

Maintain the `setTransform(float zoom, int xOff, int yOff)` interface on DDABridge so upstream components (ZoomController, ViewportTracker, RenderLoop) are API-agnostic.

## Architecture — Eight Components, Three Layers

```
Input:   InputInterceptor · WinKeyManager
Logic:   ZoomController · ViewportTracker · RenderLoop
Output:  DDABridge (+ software cursor renderer)
Support: SettingsManager · TrayUI
```

Each component has a single responsibility. Do not merge components or move responsibilities across boundaries.

## Threading Model — Two Threads, Strict Affinity

| Thread | Owns | Key Constraint |
|--------|------|----------------|
| **Main** | Message pump, hook callbacks (WH_MOUSE_LL, WH_KEYBOARD_LL), TrayUI | Hook callbacks must be minimal: read struct, atomic write, return. No computation, no I/O, no COM, no blocking. |
| **Render** | RenderLoop frame tick, DDABridge (D3D11 device, swap chain, Desktop Duplication capture), DwmFlush() | No heap allocation, no mutex, no I/O, no blocking calls except DwmFlush(). |

Do NOT add a third thread. There is no UIA thread.

**Thread communication uses only:** atomics, SeqLock (small structs), lock-free queue (commands), copy-on-write with atomic pointer swap (settings). No `std::mutex`, no `CRITICAL_SECTION`, no `WaitForSingleObject` on any hot path.

## Overlay Window Rules

- At zoom 1.0×, the overlay MUST be hidden (`ShowWindow(hwnd, SW_HIDE)`) and Desktop Duplication capture MUST be paused. Zero GPU cost when not zoomed.
- The overlay window MUST use `SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)` to prevent self-capture recursion.
- The overlay window MUST use `WS_EX_TRANSPARENT | WS_EX_LAYERED` for input passthrough. No coordinate remapping, no `SendInput` forwarding.

## Cross-Cutting Invariants

1. **No heap allocation on hot paths.** This applies to both hook callbacks (Main thread) and `frameTick()` (Render thread). No `new`, `malloc`, `std::vector::push_back`, `std::string` construction, or STL container mutation.

2. **No per-frame logging.** Log only on state transitions (zoom started/ended, error detected, hook re-registered).

3. **`GetCursorPos()` for pointer position in RenderLoop, not SharedState.** The low-level mouse hook's `WM_MOUSEMOVE` is unreliable under fullscreen magnification. `GetCursorPos()` is a fast shared-memory read (~1µs).

4. **Use `double` for internal zoom math, `float` only at the API boundary.** Snap to exactly 1.0 when within epsilon (0.005).

5. **Integer division in viewport math is a bug.** `screenW / zoom` must be floating-point division.

6. **Settings use atomic pointer swap.** Immutable snapshot struct, copy-on-write. Readers never lock.

## Build Constraints

- **C++17 / MSVC / x64 only.**
- **Minimum OS: Windows 10 version 2004** (build 19041) for `WDA_EXCLUDEFROMCAPTURE`.
- **No UIAccess manifest required.** No code signing required. No secure folder installation required.
- **Build:** `scripts\build.bat` or CMake directly.
- **Unit tests** cover pure logic (ZoomController, ViewportTracker, WinKeyManager). Run via `ctest -C Debug`.

## Phase Gating — Do NOT Implement Early

| Feature | Earliest Phase |
|---------|---------------|
| Desktop Duplication capture + D3D11 rendering (hardcoded 2×) | 0 |
| Click-through overlay, software cursor rendering | 1 |
| Scroll-gesture zoom, WinKeyManager, ZoomController, ViewportTracker, RenderLoop, 1.0× hide-overlay | 2 |
| Keyboard shortcuts, animation, temporary toggle, SettingsManager, TrayUI, config.json, hardening | 3 |

If you find yourself writing code for a later-phase feature, stop and check the phase. Stub interfaces are acceptable if a current-phase component needs the signature for compilation, but do not implement the logic.
