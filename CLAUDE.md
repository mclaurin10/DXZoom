# SmoothZoom-DDA

**Current Status:** Phase 0 — Capture and Render Spike (starting).

Fork of SmoothZoom. Replaces the Windows Magnification API with the DXGI Desktop Duplication API (`IDXGIOutputDuplication`) and a Direct3D 11 rendering pipeline. Native C++17 Win32 full-screen magnifier for Windows. Hold Win+Scroll to smoothly zoom up to 10× with continuous pointer-based viewport tracking.

## Architecture

Single-process, three layers, eight components:

- **Input Layer:** InputInterceptor (hooks), WinKeyManager (Win key + Start Menu suppression)
- **Logic Layer:** ZoomController (zoom state/animation), ViewportTracker (offset, pointer-only), RenderLoop (frame tick engine)
- **Output Layer:** DDABridge (Desktop Duplication capture + D3D11 rendering + software cursor renderer)
- **Support Layer:** SettingsManager (config.json + snapshots), TrayUI (tray icon + settings window)

### Component Status

| Component | Status | Notes |
|-----------|--------|-------|
| **DDABridge** | **New** (replaces MagBridge) | Sole DXGI/D3D11 touchpoint. Same `setTransform(float, int, int)` interface. |
| **Software cursor renderer** | **New** (inside DDABridge) | `GetCursorInfo()` each frame, draws cursor icon at magnified position. |
| InputInterceptor | Carries over unchanged | Global hooks, scroll consumption, pointer tracking, hook watchdog. |
| WinKeyManager | Carries over unchanged | Win key state machine, Start Menu suppression. |
| ZoomController | Carries over unchanged | SCROLL_DIRECT / ANIMATING / TOGGLING / IDLE modes. |
| ViewportTracker | Carries over (simplified) | Pointer-only. No `determineActiveSource()`, no focus/caret SeqLock. |
| RenderLoop | Carries over unchanged | Calls `ddaBridge.setTransform()` instead of `magBridge.setTransform()`. |
| SettingsManager | Carries over unchanged | config.json in `%AppData%\SmoothZoom\`, atomic pointer swap. |
| TrayUI | Carries over unchanged | System tray icon, context menu, settings window. |
| ~~FocusMonitor~~ | **Removed** | Out of scope. |
| ~~CaretMonitor~~ | **Removed** | Out of scope. |

## DDABridge — Component Contract

DDABridge is the **only** component that touches DXGI or Direct3D 11. No other component should `#include` D3D/DXGI headers or call D3D/DXGI functions. This isolates the capture/render pipeline to one file.

**Public interface:** `setTransform(float zoom, int xOff, int yOff)` — identical to the old MagBridge. All upstream components (ZoomController, ViewportTracker, RenderLoop) are API-agnostic.

### DDABridge Internals

- **Capture:** `IDXGIOutputDuplication` on the primary monitor. Acquires desktop frames as GPU textures. Handles `DXGI_ERROR_ACCESS_LOST` by re-creating the duplication object (happens on resolution change, desktop switch, UAC prompt).
- **Rendering:** Full-screen borderless topmost HWND with a D3D11 swap chain. A vertex/pixel shader scales and offsets the captured texture according to the current zoom level and viewport offset. Bilinear filtering for image smoothing ON (default), nearest-neighbor for smoothing OFF.
- **Software cursor:** Queries `GetCursorInfo()` each frame, draws the current cursor icon at the correct magnified position on the overlay surface. Hides the system cursor in the overlay region when zoom > 1.0×.
- **Self-capture exclusion:** `SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)` (Win 10 2004+). This is a Phase 0 risk gate — if exclusion fails, evaluate `Windows.Graphics.Capture` with `IGraphicsCaptureItemInterop` as a fallback.
- **Frame pacing:** `DwmFlush()` synchronization, same as the original design.

## Overlay Window Strategy

The overlay window is a full-screen borderless topmost HWND.

- **Input passthrough:** Uses `WS_EX_TRANSPARENT | WS_EX_LAYERED` to be fully input-transparent. All pointer input passes through to the desktop beneath. No coordinate remapping or `SendInput` forwarding is needed. Scroll interception for zoom is handled by the global low-level mouse hook, same as the original design.
- **At 1.0×:** The overlay window is hidden (`ShowWindow(hwnd, SW_HIDE)`). Desktop Duplication capture is paused. GPU cost is zero. This preserves the behavioral contract: at 1.0×, SmoothZoom is perceptually invisible with no visual artifact, no performance impact, and no input interference.
- **Transition from 1.0× to zoomed:** The overlay materializes and capture begins. This transition must be visually seamless (no flash, pop, or frame skip).

## Threading Model — Two Threads

| Thread | Owns | Key Constraint |
|--------|------|----------------|
| **Main** | Message pump, hook callbacks (WH_MOUSE_LL, WH_KEYBOARD_LL), TrayUI, app lifecycle | Hook callbacks must be minimal: read struct, atomic write, return. No computation, no I/O, no COM, no blocking. |
| **Render** | RenderLoop frame tick, DDABridge (D3D11 device, swap chain, Desktop Duplication), DwmFlush() | No heap allocation, no mutex, no I/O, no blocking calls except DwmFlush(). |

There is **no UIA thread**. FocusMonitor and CaretMonitor are removed (out of scope).

### Threading Invariants

1. **Hook callbacks must be minimal.** Read event, update an atomic or post a message, return. No computation, no I/O, no allocation. The system silently deregisters hooks that exceed ~300ms.
2. **Render thread: no heap allocation, no mutexes, no I/O on the per-frame hot path.** All data comes from pre-allocated shared state via atomics or SeqLock.
3. **No per-frame logging.** Log only on state transitions (zoom started/ended, error detected, hook re-registered). At 144Hz, per-frame logging is 144 writes/second.
4. **`GetCursorPos()` for pointer position in RenderLoop, not SharedState.** The low-level mouse hook's `WM_MOUSEMOVE` is unreliable under fullscreen magnification. `GetCursorPos()` is a fast shared-memory read (~1µs) that always returns the true position.
5. **Use `double` for internal zoom math, `float` only at the API boundary.** Prevents floating-point accumulation drift over long sessions. Snap to exactly 1.0 when within epsilon (0.005).
6. **Integer division in viewport math is a bug.** `screenW / zoom` must be floating-point division. Integer truncation causes click-offset errors that worsen at higher zoom levels.
7. **Settings use atomic pointer swap.** Immutable snapshot struct, copy-on-write. Readers never lock.

### Shared State Mechanisms

- **Atomics:** modifier key state, scroll delta accumulator (exchange-with-0), toggle state, timestamps
- **SeqLock:** pointer position (small struct, infrequent writes, frequent reads)
- **Lock-free queue:** keyboard commands (Main → Render)
- **Atomic pointer swap:** settings snapshots (Main → All)

No focus rectangle or caret rectangle shared state (those components are removed).

## Hard Constraints

1. **x64 only.** Desktop Duplication API is x64.
2. **Windows 10 version 2004+** (build 19041) minimum for `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`.
3. **PerMonitorV2 DPI awareness** in manifest. Without it, coordinates are virtualized and viewport math breaks.
4. **No external frameworks.** Pure Win32 + D3D11 + C++17 + MSVC. Only optional external dep: nlohmann/json (header-only).
5. **Secure desktop inaccessible.** `IDXGIOutputDuplication` cannot capture Ctrl+Alt+Delete, UAC, lock screen.
6. **DRM/HDCP-protected content** will appear black in the magnified view. This is a known limitation — do not attempt to work around it.
7. **Single-monitor only** in this fork.
8. **UIAccess and code signing are NOT required.** The Desktop Duplication API does not need them.

## Crash Behavior

With Desktop Duplication, a crash kills the overlay window and the desktop returns to normal (acceptable). This is inherently safer than the Magnification API, where a crash left the screen stuck at the magnified level. An unhandled exception handler is still installed for clean state logging, but the failure mode is benign.

## Known Issues & Debugging

1. **DRM-protected content** (Netflix in Edge, some games with anti-cheat) renders as black. Desktop Duplication captures post-compositing and is subject to HDCP restrictions.
2. **Secure desktop inaccessible**, same as the original.
3. **Self-capture recursion:** If the overlay appears in the captured frame, you get infinite recursion. Verify `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` is working. If it fails, evaluate `Windows.Graphics.Capture` as a fallback.
4. **`DXGI_ERROR_ACCESS_LOST`:** Expected on resolution change, desktop switch, UAC prompt. DDABridge must re-create the `IDXGIOutputDuplication` object. Log the event, recover within 1 second.

## Phased Delivery — Follow Strictly

Do not implement later-phase features prematurely. Each phase produces a runnable, testable build.

| Phase | Name | Key Delivery |
|-------|------|-------------|
| 0 | Capture and Render Spike | Hardcoded 2× magnified overlay via DDA + D3D11, self-capture exclusion risk gate |
| 1 | Input Passthrough and Cursor | Click-through overlay, software cursor rendering |
| 2 | Scroll-Gesture Zoom (Core Value) | InputInterceptor, WinKeyManager, ZoomController, ViewportTracker, RenderLoop wired to DDABridge. 1.0× hide-overlay behavior. |
| 3 | Keyboard Shortcuts, Animation, Toggle, Settings, Polish | Keyboard zoom, animated transitions, hold-to-peek, settings UI, tray icon, config.json, hardening |

### Phase 0 — Capture and Render Spike

Minimal harness. Create `IDXGIOutputDuplication` on the primary monitor, acquire frames in a loop, render them scaled to a hardcoded 2× on a full-screen borderless topmost window via D3D11. No input handling, no zoom control.

**Exit criteria:**
- E0.1: Overlay displays a 2× magnified view of the desktop, updating in real time
- E0.2: The overlay window itself is not visible in the captured/magnified content (no infinite recursion)
- E0.3: Visual update appears within one frame of desktop content change
- E0.4: Framerate stays at display refresh rate (60fps on 60Hz) with <5% GPU utilization on mid-range hardware
- E0.5: `DXGI_ERROR_ACCESS_LOST` recovery works: change display resolution while running, capture resumes within 1 second

**Risk gate:** If E0.2 fails (self-capture exclusion doesn't work), evaluate `Windows.Graphics.Capture` with monitor capture + border suppression as an alternative capture backend before proceeding.

### Phase 1 — Input Passthrough and Cursor

Make the overlay click-through. Implement software cursor rendering. Still hardcoded 2× zoom.

**Exit criteria:**
- E1.1: With overlay at 2×, clicking a desktop button activates it
- E1.2: Dragging a window title bar moves the window correctly
- E1.3: Hover states (button highlights, hyperlink cursor changes, tooltips) work
- E1.4: Software cursor displays at the correct magnified position with the correct icon
- E1.5: Right-click context menus appear at the correct position

### Phase 2 — Scroll-Gesture Zoom (Core Value)

Wire up InputInterceptor, WinKeyManager, ZoomController, ViewportTracker, and RenderLoop to DDABridge. Also implements the 1.0× hide-overlay behavior.

**ACs covered:** AC-2.1.01–AC-2.1.18, AC-2.2.01–AC-2.2.03 + AC-2.2.10, AC-2.3.01–AC-2.3.13, AC-2.4.01–AC-2.4.13.

**Exit criteria:**
- E2.1: Hold Win + scroll up: screen zooms in smoothly centered on pointer
- E2.2: Hold Win + scroll down: screen zooms out. At 1.0×, overlay disappears, further scroll-down has no effect
- E2.3: Zoom cannot exceed 10.0×, decelerates into the bound
- E2.4: Release Win after scrolling: Start Menu does NOT open
- E2.5: Win press-and-release without scroll: Start Menu opens normally
- E2.6: While zoomed, move pointer: viewport glides proportionally
- E2.7: While zoomed, click a button: correct button activates
- E2.8: At 1.0×, no visual artifact, no performance impact, no input latency
- E2.9: Transition from 1.0× to zoomed is visually seamless

### Phase 3 — Keyboard Shortcuts, Animation, Toggle, Settings, and Polish

Combines keyboard-driven zoom with animated transitions, temporary toggle, settings UI, tray icon, config.json persistence, and hardening.

**ACs covered:** AC-2.2.04–AC-2.2.10, AC-2.8.01–AC-2.8.10, AC-2.7.01–AC-2.7.12, AC-2.9.01–AC-2.9.19, AC-ERR.03–AC-ERR.04.

## Component Boundaries

Each component has a single responsibility. No component reaches into another's internals. Communication only through shared state or defined interfaces.

- **DDABridge** is the only file that includes DXGI/D3D11 headers. No other component calls DXGI or D3D functions.
- **WinKeyManager** is factored out of InputInterceptor for testability of the Win key state machine.
- **InputInterceptor** never consumes keyboard events (only observes). It consumes scroll events only when the modifier is held.
- **ViewportTracker** is pointer-only in this fork. No multi-source priority arbitration, no `determineActiveSource()`.
- **ZoomController** has four modes: IDLE, SCROLL_DIRECT, ANIMATING, TOGGLING. Uses `double` internally for zoom math, converts to `float` only at the API boundary. Snaps to 1.0× within epsilon 0.005.

## Build Requirements

- C++17, MSVC (VS 2022+), CMake, x64 only.
- Link: `d3d11.lib`, `dxgi.lib`, `d3dcompiler.lib`, `Dwmapi.lib`, `User32.lib`, `Shell32.lib`, `Ole32.lib`, `OleAut32.lib`, `Wtsapi32.lib`, `Hid.lib`, `Advapi32.lib`, `Comctl32.lib`.
- No code signing required. No UIAccess manifest required. No secure folder installation required.
- Unit tests: `ctest -C Debug` from the build directory. Cover pure logic (ZoomController, ViewportTracker, WinKeyManager).
