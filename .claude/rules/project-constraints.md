# SmoothZoom-DDA — Project-Wide Constraints

## Current Phase: Phase 0 (Capture and Render Spike)

Phase 0 validates the Desktop Duplication capture pipeline and Direct3D 11 rendering with a hardcoded 2× zoom overlay. No input handling, no zoom control, no component integration.

**Phase 0 scope:**
- Create `IDXGIOutputDuplication` on the primary monitor
- Acquire desktop frames in a loop
- Render scaled to a hardcoded 2× on a full-screen borderless topmost window via D3D11
- Validate self-capture exclusion via `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`
- Validate `DXGI_ERROR_ACCESS_LOST` recovery (change resolution while running)
- Validate 60fps sustained capture + render with <5% GPU utilization

**Exit criteria:** E0.1 through E0.5 (see CLAUDE.md §Phase 0)

**Risk gate:** If self-capture exclusion fails (E0.2), evaluate `Windows.Graphics.Capture` as an alternative capture backend before proceeding to Phase 1.

## Phase Gating — Do NOT Implement Early

| Feature | Earliest Phase | Key Indicator |
|---------|---------------|---------------|
| Input passthrough (click-through overlay) | 1 | `WS_EX_TRANSPARENT \| WS_EX_LAYERED` on overlay window |
| Software cursor rendering | 1 | `GetCursorInfo()` per-frame in DDABridge |
| InputInterceptor, WinKeyManager, hook installation | 2 | Scroll consumption, modifier key tracking |
| ZoomController SCROLL_DIRECT mode | 2 | Logarithmic scroll-to-zoom, soft-approach bounds clamping |
| ViewportTracker pointer-only tracking | 2 | Proportional mapping, edge clamping, deadzone |
| RenderLoop frame tick integration | 2 | Drains scroll accumulator, calls DDABridge |
| 1.0× hide-overlay behavior | 2 | `ShowWindow(overlayHwnd, SW_HIDE)` at zoom 1.0× |
| Keyboard shortcuts (Win+Plus/Minus/Esc) | 3 | ZoomController ANIMATING mode, ease-out interpolation |
| Temporary toggle (Ctrl+Alt hold-to-peek) | 3 | ZoomController TOGGLING mode |
| SettingsManager, TrayUI, config.json | 3 | AC-2.9.*, configurable modifier keys |
| Crash recovery (`SetUnhandledExceptionFilter`) | 3 | Sentinel file, exception handler |

**Out of scope (will NOT be implemented):**
- UIA thread, FocusMonitor, CaretMonitor
- ViewportTracker multi-source priority arbitration (`determineActiveSource()`)
- Color inversion (no Desktop Duplication equivalent without custom shaders, which is out of scope)
- Multi-monitor support
- Conflict detection with native Magnifier (Magnification API specific)

If you find yourself writing code for a feature in the right column, stop and check the phase. Stub interfaces are acceptable if a current-phase component needs the signature for compilation, but do not implement the logic.

## Architecture — Nine Components, Four Layers

```
Input:   InputInterceptor · WinKeyManager
Logic:   ZoomController · ViewportTracker · RenderLoop
Output:  DDABridge (includes CursorRenderer)
Support: SettingsManager(P3) · TrayUI(P3)
```

Each component has a single responsibility. Do not merge components or move responsibilities across boundaries. If a new behavior doesn't fit an existing component, that's a design conversation, not a reason to stuff it into the nearest file.

## Threading Model — Two Threads, Strict Affinity

| Thread | Owns | Key Constraint |
|--------|------|----------------|
| **Main** | Message pump, hook callbacks (WH_MOUSE_LL, WH_KEYBOARD_LL), TrayUI | Hook callbacks must be minimal: read struct, atomic write, return. No computation, no I/O, no COM, no blocking. |
| **Render** | RenderLoop frame tick, all DDABridge calls (capture + D3D11 rendering) | No heap allocation, no mutex, no I/O, no blocking calls except DwmFlush(). |

**Note:** The UIA Thread from the original SmoothZoom is removed. Focus and caret following are out of scope.

**Thread communication uses only:** atomics, SeqLock (small structs), lock-free queue (commands), copy-on-write with atomic pointer swap (settings). No `std::mutex`, no `CRITICAL_SECTION`, no `WaitForSingleObject` on any hot path.

## Cross-Cutting Invariants

1. **No DXGI/D3D11 includes outside DDABridge.** All DXGI/D3D11 calls go through DDABridge's public interface. This isolates future migrations to one file. No other component should include `<dxgi.h>`, `<dxgi1_2.h>`, `<d3d11.h>`, or call any DXGI/D3D11 functions.

2. **No heap allocation on hot paths.** This applies to both hook callbacks (Main thread) and `frameTick()` (Render thread). No `new`, `malloc`, `std::vector::push_back`, `std::string` construction, or STL container mutation.

3. **No per-frame logging.** Log only on state transitions (zoom started/ended, error detected, hook re-registered, `DXGI_ERROR_ACCESS_LOST` recovery). At 144Hz, per-frame logging is 144 writes/second.

4. **`GetCursorPos()` for pointer position in RenderLoop, not SharedState.** The low-level mouse hook's `WM_MOUSEMOVE` is unreliable under fullscreen magnification. `GetCursorPos()` is a fast shared-memory read (~1µs) that always returns the true position.

5. **Use `double` for internal zoom math, `float` only at the API boundary.** Prevents floating-point accumulation drift over long sessions. Snap to exactly 1.0 when within epsilon (0.005).

6. **Integer division in viewport math is a bug.** `screenW / zoom` must be floating-point division. Integer truncation causes click-offset errors that worsen at higher zoom levels.

7. **At zoom 1.0×, overlay is hidden and capture is paused.** Zero GPU cost when not zoomed. This preserves the behavioral contract: at 1.0×, SmoothZoom-DDA is perceptually invisible.

## Build Constraints

- **C++17 / MSVC / x64 only.** No external frameworks, no Boost, no Qt. Win32 API + STL only. nlohmann/json (header-only, vendored in third_party/) is the sole exception.
- **Windows 10 version 2004+** (build 19041) minimum for `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`.
- **PerMonitorV2 DPI awareness** in manifest. Without it, coordinates are virtualized and viewport math breaks on mixed-DPI setups.
- **No UIAccess, no code signing, no secure folder requirement.** The Desktop Duplication API does not need them (unlike the Magnification API).
- **Build:** `scripts\build.bat` or CMake directly.
- **Link:** `d3d11.lib`, `dxgi.lib`, `Dwmapi.lib`, `User32.lib`, `Shell32.lib`, `Ole32.lib`, `OleAut32.lib`, `Wtsapi32.lib`, `Hid.lib`, `Advapi32.lib`, `Comctl32.lib`.
- **No longer needed:** `Magnification.lib`, `UIAutomationCore.lib`
- **Unit tests** cover pure logic (ZoomController, ViewportTracker, WinKeyManager) with no Win32 API dependencies. Run via `ctest -C Debug` from the build directory.
