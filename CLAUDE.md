# SmoothZoom-DDA

**Current Status:** Phase 0 — Capture and Render Spike (planned). Implementation has not yet started.

Native C++17 Win32 full-screen magnifier for Windows. Hold Win+Scroll to smoothly zoom up to 10× with continuous viewport tracking of the pointer. Uses the Desktop Duplication API (`IDXGIOutputDuplication`) and Direct3D 11 rendering.

This is a fork of SmoothZoom that replaces the Windows Magnification API with the Desktop Duplication API. The Magnification API is legacy technology and imposes hard constraints (UIAccess, code signing, secure folder installation). The Desktop Duplication API removes these constraints but requires building the capture, rendering, input-passthrough, and cursor-drawing pipeline from scratch.

## Architecture

Single-process, four layers, nine components:

- **Input Layer:** InputInterceptor (hooks), WinKeyManager (Win key + Start Menu suppression)
- **Logic Layer:** ZoomController (zoom state/animation), ViewportTracker (offset, pointer-only), RenderLoop (frame tick engine)
- **Output Layer:** DDABridge (Desktop Duplication capture + D3D11 rendering), CursorRenderer (software cursor drawing, integrated into DDABridge)
- **Support Layer:** SettingsManager (config.json + snapshots), TrayUI (tray icon + settings window)

## Threading Model — Two Threads

**Main Thread:** Message pump, low-level hooks, TrayUI, app lifecycle.
**Render Thread:** Desktop Duplication capture, D3D11 rendering, frame ticks via `DwmFlush()`.

**Note:** The UIA Thread from the original SmoothZoom is removed. Focus and caret following are out of scope for this fork.

### Threading Invariants

These are the rules most likely to cause subtle, hard-to-diagnose bugs if violated:

1. **Hook callbacks must be minimal.** Read event, update an atomic or post a message, return. No computation, no I/O, no allocation. The system silently deregisters hooks that exceed ~300ms.
2. **Render thread: no heap allocation, no mutexes, no I/O on the per-frame hot path.** All data comes from pre-allocated shared state via atomics or SeqLock.
3. **DDABridge is the only component that touches DXGI or Direct3D 11.** No other component may include DXGI/D3D11 headers or call DXGI/D3D11 functions. This isolates future migrations.
4. **At zoom 1.0×, the overlay is hidden and capture is paused.** Zero GPU cost when not zoomed. This preserves the behavioral contract: at 1.0×, SmoothZoom-DDA is perceptually invisible.
5. **Settings use atomic pointer swap.** Immutable snapshot struct, copy-on-write. Readers never lock.

### Shared State Mechanisms

- **Atomics:** modifier key state, scroll delta accumulator (exchange-with-0), toggle state, timestamps
- **SeqLock:** pointer position (small struct, infrequent writes, frequent reads)
- **Lock-free queue:** keyboard commands (Main → Render)
- **Atomic pointer swap:** settings snapshots (Main → All)

**Note:** Focus rectangle and caret rectangle SeqLocks from the original design are removed (out of scope).

## Component Changes from Original SmoothZoom

### Replaced: MagBridge → DDABridge

MagBridge encapsulated all Magnification API calls. It is replaced by **DDABridge**, which encapsulates the Desktop Duplication capture pipeline and Direct3D 11 rendering pipeline. DDABridge exposes the same `setTransform(float zoom, int xOff, int yOff)` interface so that all upstream components — ZoomController, ViewportTracker, RenderLoop — are unchanged.

#### DDABridge Internals

**Capture Pipeline:**
- `IDXGIOutputDuplication` on the primary monitor
- Acquires desktop frames as GPU textures each frame
- Handles `DXGI_ERROR_ACCESS_LOST` by re-creating the duplication object (happens on resolution change, desktop switch, UAC prompt)

**Rendering Pipeline:**
- Full-screen borderless topmost `HWND` with a D3D11 swap chain
- A vertex/pixel shader scales and offsets the captured texture according to the current zoom level and viewport offset
- Bilinear filtering for image smoothing ON (default), nearest-neighbor for smoothing OFF (configurable via settings, unlike the Magnification API)
- Frame pacing: `DwmFlush()` synchronization, same as the original design

**Overlay Self-Capture Exclusion:**
- The overlay window must not appear in the captured framebuffer (would cause infinite recursion)
- Use `SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)` (available Win 10 2004+)
- This is a Phase 0 risk gate — if exclusion fails, evaluate `Windows.Graphics.Capture` with `IGraphicsCaptureItemInterop` as a fallback

**Software Cursor Rendering:**
- Queries `GetCursorInfo()` each frame
- Draws the current cursor icon at the correct magnified position on the overlay surface
- Hides the system cursor in the overlay region when zoom > 1.0×

**The 1.0× State:**
- At zoom level 1.0×, the overlay window is **hidden** (`ShowWindow(hwnd, SW_HIDE)`)
- Desktop Duplication capture is paused
- GPU cost is zero
- This preserves the behavioral contract: at 1.0×, SmoothZoom-DDA is perceptually invisible with no visual artifact, no performance impact, and no input interference
- When zoom transitions above 1.0×, the overlay materializes and capture begins
- This transition must be visually seamless

### New: CursorRenderer

Integrated into DDABridge. Queries `GetCursorInfo()` each frame and draws the current cursor icon at the correct magnified position. The system cursor is hidden when the overlay is visible (zoom > 1.0×).

### Unchanged Components

- **InputInterceptor** — global low-level hooks (`WH_MOUSE_LL`, `WH_KEYBOARD_LL`), scroll consumption, pointer position tracking, hook health watchdog
- **WinKeyManager** — Win key state machine, Start Menu suppression via `SendInput` Ctrl injection
- **ZoomController** — zoom state, logarithmic scroll-to-zoom model, `SCROLL_DIRECT` / `ANIMATING` / `TOGGLING` / `IDLE` modes, soft-approach bounds clamping, target retargeting
- **ViewportTracker** — proportional mapping, edge clamping, deadzone. **Note:** With focus/caret following out of scope, ViewportTracker is pointer-only. No `determineActiveSource()` priority arbitration, no SeqLock focus/caret rectangles.
- **RenderLoop** — frame-tick loop on the Render Thread, `DwmFlush` sync, drains scroll accumulator and keyboard command queue, calls ZoomController, calls ViewportTracker, calls DDABridge. Identical structure, just calls `ddaBridge.setTransform()` instead of `magBridge.setTransform()`.
- **SettingsManager** — `config.json` in `%AppData%\SmoothZoom\`, immutable snapshot with atomic pointer swap
- **TrayUI** — system tray icon, context menu, settings window

### Removed Components

- **FocusMonitor** — out of scope
- **CaretMonitor** — out of scope
- **UIA Thread** — no longer needed since both UIA components are removed

## Input Passthrough Model

The overlay window uses `WS_EX_TRANSPARENT | WS_EX_LAYERED` to be fully input-transparent. All pointer input passes through to the desktop beneath. No coordinate remapping or `SendInput` forwarding is needed. Scroll interception for zoom is handled by the global low-level mouse hook, same as the original design. This is the same input model used by OBS, screen-recording tools, and game overlays.

## Hard Constraints

Violating these causes silent failure or a broken build. Non-negotiable.

1. **x64 only.** Desktop Duplication API is x64.
2. **Windows 10 version 2004+** (build 19041) minimum for `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`.
3. **PerMonitorV2 DPI awareness** in manifest. Without it, coordinates are virtualized and viewport math breaks on mixed-DPI setups.
4. **Secure desktop inaccessible.** Desktop Duplication API doesn't work on Ctrl+Alt+Delete, UAC, lock screen.
5. **No external frameworks.** Pure Win32 + C++17 + MSVC. Only optional external dep: nlohmann/json (header-only).
6. **Single-monitor only.** Multi-monitor support is out of scope.
7. **DRM content limitation.** DRM/HDCP-protected content (Netflix, some games) will appear black in the magnified view. This is a known limitation of the Desktop Duplication API. Document it but do not attempt to work around it.

**Note:** UIAccess and code signing are **NOT required** for the Desktop Duplication API.

## Crash Behavior Difference

With the Magnification API, a crash left the screen stuck at the magnified level (very bad). With Desktop Duplication, a crash kills the overlay window and the desktop returns to normal (acceptable). An unhandled exception handler is still installed for clean state logging, but the failure mode is inherently safer.

## Phased Delivery — Follow Strictly

Do not implement later-phase features prematurely. Each phase produces a runnable, testable build.

| Phase | Name | Key Delivery | Status |
|-------|------|-------------|--------|
| 0 | Capture and Render Spike | Hardcoded 2× zoom overlay, 60fps capture, self-capture exclusion validation, `DXGI_ERROR_ACCESS_LOST` recovery | Planned |
| 1 | Input Passthrough and Cursor | Click-through overlay, software cursor rendering | Planned |
| 2 | Scroll-Gesture Zoom | Win+Scroll zoom, proportional viewport tracking, Start Menu suppression, 1.0× hide-overlay behavior | Planned |
| 3 | Keyboard, Toggle, Settings, Polish | Win+Plus/Minus/Esc with ease-out, temporary toggle, settings UI, tray icon, crash recovery | Planned |

### Phase 0 — Capture and Render Spike

Minimal harness. Create `IDXGIOutputDuplication` on the primary monitor, acquire frames in a loop, render them scaled to a hardcoded 2× on a full-screen borderless topmost window via D3D11. No input handling, no zoom control.

**Validates:**
- Can sustain 60fps capture + render
- End-to-end latency from desktop change to overlay pixel
- Overlay self-capture exclusion via `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` — **risk gate**: if the overlay appears in the captured frame, must find an alternative (WDA_MONITOR, Graphics.Capture, or compositing tricks)
- No visual artifacts at the 2× scale (tearing, color shift, misalignment)

**Exit criteria:**
- E0.1: Overlay displays a 2× magnified view of the desktop, updating in real time
- E0.2: The overlay window itself is not visible in the captured/magnified content (no infinite recursion)
- E0.3: Visual update appears within one frame of desktop content change
- E0.4: Framerate stays at display refresh rate (60fps on 60Hz) with <5% GPU utilization on mid-range hardware
- E0.5: `DXGI_ERROR_ACCESS_LOST` recovery works: change display resolution while running, capture resumes within 1 second

**Risk gate:** If E0.2 fails (self-capture exclusion doesn't work), evaluate `Windows.Graphics.Capture` with monitor capture + border suppression as an alternative capture backend before proceeding.

### Phase 1 — Input Passthrough and Cursor

Make the overlay click-through. Implement software cursor rendering. Still hardcoded 2× zoom.

**Validates:**
- Clicks, drags, hovers, and tooltips land correctly on the desktop beneath the overlay
- Software cursor tracks the correct magnified position
- Correct cursor icon (arrow, I-beam, hand, resize, busy) updates in real time

**Exit criteria:**
- E1.1: With overlay at 2×, clicking a desktop button activates it
- E1.2: Dragging a window title bar moves the window correctly
- E1.3: Hover states (button highlights, hyperlink cursor changes, tooltips) work
- E1.4: Software cursor displays at the correct magnified position with the correct icon
- E1.5: Right-click context menus appear at the correct position

### Phase 2 — Scroll-Gesture Zoom (Core Value)

Wire up InputInterceptor, WinKeyManager, ZoomController, ViewportTracker, and RenderLoop to DDABridge. This is the equivalent of the original Phase 1 — the core product.

Also implements the 1.0× hide-overlay behavior: overlay hidden and capture paused at 1.0×, overlay shown and capture started on zoom > 1.0×.

**Exit criteria:**
- E2.1: Hold Win + scroll up: screen zooms in smoothly centered on pointer
- E2.2: Hold Win + scroll down: screen zooms out. At 1.0×, overlay disappears, further scroll-down has no effect
- E2.3: Zoom cannot exceed 10.0×, decelerates into the bound
- E2.4: Release Win after scrolling: Start Menu does NOT open
- E2.5: Win press-and-release without scroll: Start Menu opens normally
- E2.6: While zoomed, move pointer: viewport glides proportionally
- E2.7: While zoomed, click a button: correct button activates
- E2.8: At 1.0×, no visual artifact, no performance impact, no input latency
- E2.9: Transition from 1.0× to zoomed is visually seamless (no flash, pop, or frame skip)

### Phase 3 — Keyboard, Toggle, Settings, and Polish

Combines the original Phases 2, 4, and 5. Adds keyboard-driven zoom with animated transitions, temporary toggle (hold-to-peek), settings UI, tray icon, config.json persistence, and hardening (`DXGI_ERROR_ACCESS_LOST` recovery, hook robustness watchdog).

**Note:** Color inversion, multi-monitor support, and UIA-based focus/caret following are **out of scope** and will not be implemented.

## Component Boundaries

Each component has a single responsibility. No component reaches into another's internals. Communication only through shared state or defined interfaces.

- **DDABridge** is the only file that `#include`s DXGI/D3D11 headers. This isolates future migrations. No other component should include `<dxgi.h>`, `<dxgi1_2.h>`, `<d3d11.h>`, or call any DXGI/D3D11 functions.
- **WinKeyManager** is factored out of InputInterceptor for testability of the Win key state machine.
- **InputInterceptor** never consumes keyboard events (only observes). It consumes scroll events only when the modifier is held.
- **ViewportTracker** is pointer-only. No `determineActiveSource()` priority arbitration. No focus/caret tracking.
- **ZoomController** has four modes: IDLE, SCROLL_DIRECT, ANIMATING, TOGGLING. Uses `double` internally for zoom math, converts to `float` only at the API boundary. Snaps to 1.0× within epsilon 0.005.

## What's Out of Scope

These features from the original SmoothZoom are **explicitly excluded** from this fork:

- Multi-monitor support
- Color inversion / color effect shaders (`MagSetFullscreenColorEffect` had no Desktop Duplication equivalent without custom shaders, which is out of scope)
- Keyboard focus following (UIA FocusMonitor)
- Text cursor / caret following (UIA CaretMonitor)
- The entire UIA thread and its two components
- `Windows.Graphics.Capture` as the capture backend (may be evaluated later if Phase 0 self-capture exclusion fails, but is not the current path)

## Build Requirements

- C++17, MSVC (VS 2022+), CMake, x64 only.
- Link: `d3d11.lib`, `dxgi.lib`, `Dwmapi.lib`, `User32.lib`, `Shell32.lib`, `Ole32.lib`, `OleAut32.lib`, `Wtsapi32.lib`, `Hid.lib`, `Advapi32.lib`, `Comctl32.lib`.
- **No longer needed:** `Magnification.lib`, `UIAutomationCore.lib`
- **No UIAccess, no code signing, no secure folder requirement.** The Desktop Duplication API does not need them.
- Build: `scripts\build.bat` or CMake directly.
