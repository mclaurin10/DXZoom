# DXZoom — Technical Risks and Mitigations

**Version:** 1.0
**Status:** Draft
**Last Updated:** March 2026
**Prerequisites:** CLAUDE.md, .claude/rules/*

---

## 1. How to Read This Document

Each risk is assigned a unique identifier (DR-01 through DR-18) and rated on two dimensions:

**Likelihood** — how probable is it that this risk materializes?
- **Low:** Unlikely given current evidence, but not impossible.
- **Medium:** A realistic possibility that should be planned for.
- **High:** Expected to occur based on known platform behavior or prior experience.

**Impact** — if the risk materializes, how severe is the consequence?
- **Low:** A minor inconvenience. A workaround exists and no user-visible degradation occurs.
- **Medium:** A noticeable degradation in one feature area. The core experience still functions.
- **High:** A major feature is broken or seriously degraded. The product is shippable but diminished.
- **Critical:** The technical approach is invalidated. A fundamental redesign is required before the project can proceed.

**First Exposed** indicates the delivery phase where the risk is first testable. Risks exposed in Phase 0 are the most important to resolve early — that's why Phase 0 exists.

The prefix "DR" distinguishes these from the original SmoothZoom risks (R-01 through R-22). Where a risk carries over from the original project, the original ID is noted for traceability.

---

## 2. Category A — Desktop Duplication and Rendering Risks

These risks are specific to the DXGI Desktop Duplication capture pipeline and the Direct3D 11 rendering pipeline that replaces the Magnification API.

---

### DR-01: Overlay Self-Capture Recursion

| Attribute | Value |
|-----------|-------|
| Likelihood | **Low** |
| Impact | **Critical** |
| First Exposed | Phase 0 |

**Description:** The DXZoom overlay window is a full-screen topmost `HWND` that renders the magnified desktop. `IDXGIOutputDuplication` captures the composited desktop framebuffer — which includes the overlay window itself. If the overlay appears in the captured texture, the result is infinite recursion: the overlay renders itself rendering itself, producing a hall-of-mirrors effect or a completely corrupted display.

**Current Evidence:** `SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)` was introduced in Windows 10 version 2004 (build 19041) and is designed to exclude a window from Desktop Duplication and `Windows.Graphics.Capture` output. OBS Studio and other screen-capture tools use this API to exclude their own preview windows. The API is well-documented and widely used.

**Mitigation:**
1. Call `SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)` immediately after creating the overlay window, before the first frame is captured.
2. Phase 0 exit criterion E0.2 directly validates this: verify the overlay window itself is not visible in the captured/magnified content.
3. If `SetWindowDisplayAffinity` returns `FALSE`, log the error and abort initialization — do not proceed with a visible-in-capture overlay.

**Risk Gate:** If E0.2 fails (the overlay is visible in the captured frame despite `WDA_EXCLUDEFROMCAPTURE`):
- Investigate whether the failure is driver-specific or OS-version-specific.
- Test `WDA_MONITOR` as an alternative (excludes from all capture, including screen sharing — more aggressive but guaranteed to work).
- If neither `WDA` flag works, evaluate `Windows.Graphics.Capture` with `IGraphicsCaptureItemInterop::CreateForMonitor()` which has built-in window exclusion capabilities.
- If no capture API can exclude the overlay, the project's fundamental approach is invalidated and requires a different rendering strategy.

---

### DR-02: DXGI_ERROR_ACCESS_LOST Recovery Reliability

| Attribute | Value |
|-----------|-------|
| Likelihood | **High** |
| Impact | **High** |
| First Exposed | Phase 0 |

**Description:** `IDXGIOutputDuplication::AcquireNextFrame()` returns `DXGI_ERROR_ACCESS_LOST` in several normal operating scenarios: display resolution change, desktop switch (Ctrl+Alt+Delete, UAC prompt, lock screen), display mode change (fullscreen game launch/exit, HDR toggle), and monitor connect/disconnect. This is not an error — it's the API's way of signaling that the duplication surface is stale and must be recreated. However, recreation is not instant: it requires releasing the old duplication object, potentially re-creating the D3D11 device and swap chain (if resolution changed), and acquiring a new duplication object. If recovery is slow, unreliable, or leaves stale state, the user sees a frozen or black overlay.

**Current Evidence:** This is the normal operating model for Desktop Duplication. Every application that uses the API must handle this. Microsoft's own Desktop Duplication sample code demonstrates the recreation flow. The risk is not whether recovery is possible — it's whether it's fast and reliable enough to be invisible to the user.

**Mitigation:**
1. Implement a structured recovery sequence: release duplication → release swap chain and render target views → re-query output dimensions → recreate swap chain → recreate duplication → resume capture.
2. Retry with exponential backoff: 0ms, 50ms, 100ms, 200ms, 500ms. Five attempts. If all fail, set an error flag and stop capture.
3. During recovery, hide the overlay (`ShowWindow(SW_HIDE)`) so the user sees the normal desktop rather than a frozen magnified frame. Re-show once capture resumes.
4. Phase 0 exit criterion E0.5 validates this: change display resolution while running, verify capture resumes within 1 second.

**Contingency:** If recovery is consistently slow (>1 second) or fails on certain hardware: implement a "soft reset" where DXZoom resets to 1.0× zoom on `ACCESS_LOST`, then requires the user to re-engage zoom. This is a degraded experience but avoids showing stale frames.

---

### DR-03: Capture-to-Display Latency

| Attribute | Value |
|-----------|-------|
| Likelihood | **Medium** |
| Impact | **High** |
| First Exposed | Phase 0 |

**Description:** The Desktop Duplication pipeline adds inherent latency that the Magnification API did not have. The Magnification API operated inside DWM compositing — it told DWM "scale the desktop by 2×" and DWM did it in the same compositing pass. Desktop Duplication captures the *result* of DWM compositing, then DXZoom renders it to the overlay, which DWM composites *again*. This means the magnified view is always at least one frame behind the actual desktop content. If the pipeline is poorly structured, the delay could be two or more frames.

At 60Hz, one frame of latency is 16.67ms. This is tolerable — the user won't perceive a 16ms delay in magnified content. Two frames (33ms) is noticeable on fast-moving content (video, animations, cursor movement). Three or more frames would make the tool feel sluggish.

**Mitigation:**
1. Call `AcquireNextFrame()` with a timeout of 0ms (non-blocking). If no new frame is available, re-render the last captured texture at the current zoom/offset (the desktop hasn't changed, so no visual difference). This avoids blocking the render loop waiting for the next desktop frame.
2. Structure the render loop to minimize the gap between `AcquireNextFrame` and `Present`: acquire → update shader constants → draw → present → `DwmFlush`. No heavy computation between acquire and present.
3. Phase 0 exit criterion E0.3 validates this: verify the visual update appears within one frame of desktop content change.
4. For objective measurement, use `DwmGetCompositionTimingInfo` to compare the captured frame's present timestamp with the overlay's present timestamp.

**Contingency:** If latency is consistently two frames: accept it. Two-frame latency (33ms at 60Hz) is the expected cost of the capture-then-render approach. The Magnification API avoided this by operating inside DWM, but that option is no longer available. If latency exceeds two frames: investigate whether the swap chain present mode, buffer count, or `DwmFlush` timing can be adjusted. `DXGI_SWAP_EFFECT_FLIP_DISCARD` with a single back buffer minimizes queuing.

---

### DR-04: Overlay Z-Order Conflicts

| Attribute | Value |
|-----------|-------|
| Likelihood | **Medium** |
| Impact | **Medium** |
| First Exposed | Phase 0 |

**Description:** The overlay window must be the topmost window on the desktop — above all application windows, taskbar, and other overlays. The window is created with `WS_EX_TOPMOST`, but other applications also use topmost windows: game overlays (Steam, Discord, GeForce Experience), screen-recording tools (OBS), accessibility tools, notification popups, and volume/brightness OSD. If another topmost window is placed above the DXZoom overlay, it appears unmagnified in front of the magnified desktop — a jarring visual discontinuity.

The Magnification API didn't have this problem because it operated within DWM compositing, not as a separate window layer.

**Mitigation:**
1. Create the overlay with `WS_EX_TOPMOST | WS_EX_NOACTIVATE`. The `WS_EX_NOACTIVATE` prevents the overlay from stealing focus when shown.
2. Periodically call `SetWindowPos(overlayHwnd, HWND_TOPMOST, ...)` in the render loop (once per second, not per frame) to reassert topmost status if another window has claimed it.
3. Accept that some overlays (notably game overlays and hardware OSD) may appear above the magnified view. This is a cosmetic issue, not a functional one — the magnifier still works.

**Contingency:** If Z-order fights are persistent and disruptive: investigate using `HWND_TOP` with a `WS_EX_TOOLWINDOW` style, or using the `SetWindowBand` undocumented API (used by the Windows shell for above-topmost positioning). These are fragile approaches and should only be used as a last resort.

---

### DR-05: D3D11 Device Lost or Removed

| Attribute | Value |
|-----------|-------|
| Likelihood | **Low** |
| Impact | **High** |
| First Exposed | Phase 0 |

**Description:** The D3D11 device can be lost (`DXGI_ERROR_DEVICE_REMOVED` or `DXGI_ERROR_DEVICE_RESET`) due to driver crashes, driver updates, GPU timeout detection and recovery (TDR), or hardware failure. When this happens, all D3D11 resources (textures, buffers, shaders, swap chain) become invalid and must be recreated from scratch.

**Current Evidence:** Device loss is rare during normal desktop use but does occur. Driver updates while the application is running are a common trigger. TDR events (GPU hangs exceeding the Windows timeout, typically 2 seconds) are another.

**Mitigation:**
1. Check `HRESULT` from `Present()` for `DXGI_ERROR_DEVICE_REMOVED` and `DXGI_ERROR_DEVICE_RESET`. Also check `ID3D11Device::GetDeviceRemovedReason()` when any D3D11 call fails unexpectedly.
2. On device loss: tear down all resources, recreate the D3D11 device, recreate the swap chain, reload shaders, recreate the Desktop Duplication object, and resume.
3. This is a superset of the `DXGI_ERROR_ACCESS_LOST` recovery (DR-02). Structure the recovery code so that device-loss recovery calls the access-lost recovery as a subset.
4. During recovery, hide the overlay. Show a brief notification if recovery takes more than 1 second.

**Contingency:** If device recreation fails (e.g., the GPU is in an unrecoverable state): exit gracefully. Log the failure, hide the overlay, and terminate. The desktop returns to normal since the overlay is gone.

---

### DR-06: DRM and HDCP Content Appears Black

| Attribute | Value |
|-----------|-------|
| Likelihood | **High** |
| Impact | **Low** |
| First Exposed | Phase 0 |

**Description:** Desktop Duplication captures the composited desktop framebuffer, but HDCP-protected content (Netflix in Edge/Chrome, Hulu, Disney+, some games with anti-cheat) is excluded from the capture. These regions appear as solid black in the captured texture. When magnified, the user sees a large black rectangle where the protected content should be.

The Magnification API did not have this limitation because it operated within the DWM compositing pipeline before HDCP enforcement.

**Current Evidence:** This is a well-documented limitation of Desktop Duplication. Every screen-capture tool (OBS, ShareX, Windows Game Bar) exhibits the same behavior with HDCP content. There is no workaround — HDCP enforcement is by design.

**Mitigation:**
1. Document this as a known limitation in the README and any user-facing help text.
2. Do not attempt to work around HDCP enforcement — this would violate DRM protections and potentially legal requirements.
3. The magnifier still functions correctly for all non-protected content, which is the vast majority of desktop use.

**Contingency:** None. This is a platform constraint, not a bug.

---

## 3. Category B — Input and Overlay Interaction Risks

---

### DR-07: Overlay Click-Through Reliability

| Attribute | Value |
|-----------|-------|
| Likelihood | **Medium** |
| Impact | **High** |
| First Exposed | Phase 1 |

**Description:** The overlay window uses `WS_EX_TRANSPARENT | WS_EX_LAYERED` to be fully input-transparent. All pointer input should pass through to the desktop beneath. However, there are known edge cases where `WS_EX_TRANSPARENT` windows can still intercept input: drag-and-drop operations may not initiate correctly if the overlay processes `WM_NCHITTEST`; some touch input and stylus input may behave differently with layered windows; and `WM_MOUSEACTIVATE` handling can interfere with click targeting.

**Mitigation:**
1. Ensure the overlay's window procedure returns `HTTRANSPARENT` from `WM_NCHITTEST` as a defense-in-depth measure, in addition to the `WS_EX_TRANSPARENT` style.
2. Phase 1 exit criteria E1.1 through E1.5 validate click, drag, hover, tooltip, and right-click behavior comprehensively.
3. Test with diverse input devices: standard mouse, Precision Touchpad, touch screen (if available), stylus.
4. Test drag-and-drop specifically: drag files between Explorer windows, drag text in a browser, drag window title bars.

**Contingency:** If specific input types (touch, stylus) don't pass through reliably: investigate whether the overlay needs to actively forward those events via `SendInput` or `PostMessage`. This is a significant complexity increase and should only be pursued if the affected input type is common enough to matter.

---

### DR-08: Software Cursor Rendering Fidelity

| Attribute | Value |
|-----------|-------|
| Likelihood | **Medium** |
| Impact | **Medium** |
| First Exposed | Phase 1 |

**Description:** DXZoom must render a software cursor because the captured desktop texture does not include the cursor layer. This requires querying `GetCursorInfo()` every frame to get the current cursor handle, position, and visibility, then drawing the correct cursor bitmap at the magnified position. There are several fidelity risks:

1. **Hotspot offset:** Each cursor icon has a hotspot (the pixel that represents the exact click point). `GetIconInfo()` provides the hotspot coordinates. If the rendered cursor is offset from the hotspot, clicks will appear to land at the wrong position relative to the visible cursor — even though input passthrough delivers them correctly to the desktop.
2. **Animated cursors:** The busy/working cursor (`IDC_WAIT`, `IDC_APPSTARTING`) is animated. Rendering the correct animation frame requires tracking the cursor animation state or using `DrawIconEx` which handles animation automatically if called per-frame.
3. **High-DPI cursors:** On high-DPI displays, cursor bitmaps may be larger than expected. The rendered cursor must match the size the user expects at their DPI settings.
4. **Cursor visibility transitions:** When the cursor is hidden by an application (e.g., during video playback, in fullscreen games), `GetCursorInfo()` reports `CURSOR_SUPPRESSED` or the cursor is not visible. DXZoom must not render a phantom cursor in these cases.

**Mitigation:**
1. Use `GetCursorInfo()` for position and visibility, `GetIconInfo()` for hotspot offset and bitmap handles.
2. Render using `DrawIconEx` if using GDI compositing on the overlay DC, or extract the cursor bitmap via `GetIconInfo` and render it as a D3D11 texture if using full GPU rendering.
3. Subtract the hotspot offset from the rendered position so the cursor's active point aligns with where input will land.
4. Test with all standard system cursors: arrow, I-beam, hand, crosshair, resize (all directions), move, busy, working-in-background, forbidden.
5. Test cursor visibility: move cursor into a fullscreen video player, verify no phantom cursor renders.

**Contingency:** If animated cursor rendering is problematic: render the static version of animated cursors. This is a minor visual regression — the busy spinner appears as a static hourglass. Functional behavior is unaffected.

---

### DR-09: 1.0× Show/Hide Transition Artifacts

| Attribute | Value |
|-----------|-------|
| Likelihood | **Medium** |
| Impact | **Medium** |
| First Exposed | Phase 2 |

**Description:** At zoom 1.0×, the overlay is hidden (`ShowWindow(SW_HIDE)`) and capture is paused. When zoom transitions above 1.0×, the overlay must appear and capture must resume. If this transition is not visually seamless, the user sees a flash, pop, black frame, or momentary desktop flicker when first engaging zoom. This would feel unpolished and jarring.

The transition involves several steps that must happen in the right order and ideally within a single frame:
1. Resume Desktop Duplication capture (may need to recreate the duplication object if it was released while paused).
2. Acquire the first frame.
3. Render the first magnified frame to the overlay's back buffer.
4. Show the overlay window (`ShowWindow(SW_SHOW)`).
5. Present the back buffer.

If the overlay becomes visible before the first magnified frame is rendered, the user briefly sees a stale or black overlay.

**Mitigation:**
1. **Pre-render before show:** Acquire the first frame and render it to the back buffer *before* calling `ShowWindow(SW_SHOW)`. The overlay becomes visible already containing the correct magnified content.
2. Keep the `IDXGIOutputDuplication` object alive during the 1.0× pause rather than releasing it. This avoids the recreation latency. Only release it if `DXGI_ERROR_ACCESS_LOST` occurs or if memory pressure is a concern.
3. At 1.0× pause, continue holding the last captured desktop texture in GPU memory. When zoom re-engages, the first frame can render from the cached texture while a fresh capture is acquired.
4. Phase 2 exit criterion E2.9 validates this: "Transition from 1.0× to zoomed is visually seamless (no flash, pop, or frame skip)."

**Contingency:** If the pre-render approach doesn't fully eliminate the flash: use a brief (50ms) fade-in on the overlay window via `SetLayeredWindowAttributes` with a rapidly increasing alpha. This masks any initialization artifact behind a smooth opacity transition.

---

## 4. Category C — Input Interception Risks

These risks carry over from the original SmoothZoom project. The Desktop Duplication API change does not affect them — they concern the global low-level hooks, which are unchanged.

---

### DR-10: Hook Deregistration Under System Load

*(Carries over from original R-05)*

| Attribute | Value |
|-----------|-------|
| Likelihood | **High** |
| Impact | **High** |
| First Exposed | Phase 2 |

**Description:** Windows enforces a timeout on low-level hook callbacks. If the hook procedure does not return within approximately 300ms (configurable via the `LowLevelHooksTimeout` registry value), Windows silently removes the hook. Once removed, DXZoom stops intercepting scroll events — the modifier+scroll gesture silently stops working. The user has no feedback that anything has gone wrong.

**Contributing factors:** Main thread message pump stalls (COM calls, dialog boxes, Windows Update notifications, antivirus interference), system load spikes, or a bug that adds latency to the hook callback.

**Mitigation:**
1. Hook callbacks must be absolutely minimal: read the event struct, perform one or two atomic writes, return immediately. No computation, no I/O, no allocation, no COM, no debug output.
2. Hook health watchdog timer on the Main thread checks hook handle validity every 5 seconds. If a hook has been silently unregistered, re-install it immediately and log a warning.
3. If re-registration fails: set an error flag. Scroll-gesture zoom is temporarily unavailable. Display a tray notification (Phase 3).

**Contingency:** If hooks are deregistered frequently (multiple times per session): investigate whether the main thread's message pump has latency spikes. Profile with Event Tracing for Windows (ETW) to identify the stalling message. If the root cause is unavoidable (e.g., a system-level COM call), consider moving the message pump to a dedicated thread with no other responsibilities.

---

### DR-11: Start Menu Suppression Failure

*(Carries over from original R-06)*

| Attribute | Value |
|-----------|-------|
| Likelihood | **Medium** |
| Impact | **Medium** |
| First Exposed | Phase 2 |

**Description:** When the user holds Win to zoom and then releases it, the Start Menu should NOT open. WinKeyManager suppresses it by injecting a dummy `VK_CONTROL` press/release via `SendInput` before the Win key-up reaches the shell. If the injection fails or is blocked (by security software, anti-cheat, or a `SendInput` filter), the Start Menu opens after every zoom session — an infuriating interaction that makes the tool unusable with Win as the modifier.

**Mitigation:**
1. The injection technique is well-proven — PowerToys, AutoHotkey, and many games use the same approach.
2. Test with common security software (Windows Defender, CrowdStrike, Bitdefender) to verify `SendInput` is not blocked.
3. Verify the injection timing: the `VK_CONTROL` must arrive before the Win key-up is processed by the shell. Inject synchronously from the keyboard hook's Win key-up handler.

**Contingency:** If `SendInput` injection is blocked by security software in a specific environment: recommend the user switch the modifier to Shift, Ctrl, or Alt (configurable in Phase 3 settings), which do not have Start Menu side effects.

---

### DR-12: Modifier Key Conflicts with Other Applications

*(Carries over from original R-07)*

| Attribute | Value |
|-----------|-------|
| Likelihood | **Medium** |
| Impact | **Medium** |
| First Exposed | Phase 2 |

**Description:** The modifier key (Win, Shift, Ctrl, or Alt) + scroll wheel combination may conflict with other applications. For example:
- Win+Scroll: Some virtual desktop managers or tiling window managers use this combination.
- Ctrl+Scroll: Most browsers and many editors use this for zoom. If DXZoom consumes the scroll event, browser zoom breaks.
- Shift+Scroll: Some applications use this for horizontal scrolling. If DXZoom consumes the event, horizontal scroll breaks in those applications.
- Alt+Scroll: Less commonly used but may conflict with specific accessibility tools.

**Mitigation:**
1. Win is the default modifier because it has the fewest conflicts with application-level shortcuts. The Start Menu side effect is handled by WinKeyManager.
2. Shift is the recommended alternative for users who prefer a simpler key without Start Menu concerns, with the understanding that Shift+Scroll horizontal scrolling in some applications will be consumed while DXZoom is active.
3. When DXZoom consumes a scroll event (modifier held), it returns `1` from the hook to prevent the event from reaching applications. When the modifier is NOT held, scroll events pass through unconditionally — no interference.
4. In Phase 3, the modifier key is user-configurable, so users can pick the combination that doesn't conflict with their workflow.

**Contingency:** If a specific modifier conflicts with a critical application the user can't work without: the user changes the modifier in settings. If all four modifiers conflict: this is an unusual edge case that may require DXZoom to support a two-key modifier combination (e.g., Ctrl+Alt+Scroll), which is not currently in scope but architecturally simple to add.

---

### DR-13: Touchpad Scroll Event Variations

*(Carries over from original R-08)*

| Attribute | Value |
|-----------|-------|
| Likelihood | **Medium** |
| Impact | **Low** |
| First Exposed | Phase 2 |

**Description:** Precision Touchpads (required on Windows 10 laptops since 2015) generate `WM_MOUSEWHEEL` events with fine-grained deltas — often much smaller than the traditional 120-unit notch of a mouse wheel. Legacy (non-precision) touchpads may generate different event patterns, including extremely large deltas or bursts of events.

**Mitigation:**
1. ZoomController's logarithmic scroll-to-zoom model normalizes scroll deltas against `WHEEL_DELTA` (120). Fine-grained deltas from Precision Touchpads produce proportionally smaller zoom increments — which is actually the desired behavior (smoother zoom on touchpad).
2. The scroll accumulator is additive (atomic add in the hook, atomic exchange in the render loop), so it naturally coalesces multiple small deltas into a single zoom step per frame.
3. Test with both a discrete mouse wheel and a Precision Touchpad (if available).

**Contingency:** If touchpad zoom is too sensitive or not sensitive enough: add a "scroll sensitivity" multiplier to settings (Phase 3). This scales the raw scroll delta before it reaches ZoomController.

---

## 5. Category D — Recovery and Resilience Risks

---

### DR-14: Secure Desktop and UAC Prompt Disruption

*(Carries over from original R-15)*

| Attribute | Value |
|-----------|-------|
| Likelihood | **High** |
| Impact | **Low** |
| First Exposed | Phase 2 |

**Description:** `IDXGIOutputDuplication` cannot capture the secure desktop (Ctrl+Alt+Delete, UAC prompts, lock screen). When the system switches to the secure desktop, `AcquireNextFrame()` returns `DXGI_ERROR_ACCESS_LOST`. The overlay should be hidden during this period. When the system returns to the normal desktop, capture must resume and the overlay should reappear at its previous zoom state.

This is functionally identical to the original SmoothZoom's behavior — the Magnification API also couldn't operate on the secure desktop. The difference is that the Magnification API silently stopped working, whereas Desktop Duplication actively signals the failure via `ACCESS_LOST`, making recovery more straightforward.

**Mitigation:**
1. DR-02 (ACCESS_LOST recovery) handles this case. The overlay is hidden during recovery and reappears when capture resumes.
2. Preserve zoom state across the secure desktop transition. When the user returns, they should see their magnified view, not a reset to 1.0×.
3. Detect secure desktop transitions via `WM_WTSSESSION_CHANGE` with `WTS_SESSION_LOCK`/`WTS_SESSION_UNLOCK` to differentiate from resolution changes (which also trigger `ACCESS_LOST`).

**Contingency:** None needed. This is a platform limitation. Handle it gracefully.

---

### DR-15: Display Configuration Change While Zoomed

*(Carries over from original R-16, simplified — single monitor only)*

| Attribute | Value |
|-----------|-------|
| Likelihood | **Medium** |
| Impact | **Medium** |
| First Exposed | Phase 2 |

**Description:** If the user changes display resolution or DPI scaling while DXZoom is actively zoomed, the viewport offset and proportional mapping calculations may reference stale screen geometry. The overlay window size may no longer match the display. The captured texture dimensions change. If any of these are not updated, the magnified view will be mispositioned, stretched, or cropped.

Desktop Duplication signals this via `DXGI_ERROR_ACCESS_LOST`, so the capture pipeline will naturally re-initialize. The risk is that the overlay window, viewport math, and shader constants are not also updated to match the new display geometry.

**Mitigation:**
1. On `DXGI_ERROR_ACCESS_LOST` recovery, re-query display dimensions via `GetSystemMetrics(SM_CXSCREEN)` / `GetSystemMetrics(SM_CYSCREEN)` or `EnumDisplayMonitors`.
2. Resize the overlay window to match the new display size.
3. Update ViewportTracker's screen dimensions. Re-clamp the current viewport offset to the new valid range.
4. Recreate the swap chain with the new dimensions.
5. Register for `WM_DISPLAYCHANGE` and `WM_DPICHANGED` on the main thread's message window as a supplementary detection mechanism (in addition to the `ACCESS_LOST` signal from the capture pipeline).

**Contingency:** If edge cases prove difficult (e.g., resolution changes mid-frame causing a race between the render loop and the main thread): implement a conservative fallback that resets zoom to 1.0× on any display configuration change. Inelegant but safe.

---

### DR-16: Sleep, Hibernate, and Fast User Switching

*(Carries over from original R-21)*

| Attribute | Value |
|-----------|-------|
| Likelihood | **High** |
| Impact | **Low** |
| First Exposed | Phase 2 |

**Description:** When the system enters sleep or hibernate, the display is powered off and the DWM compositing pipeline is suspended. `IDXGIOutputDuplication` will return `DXGI_ERROR_ACCESS_LOST` on the next `AcquireNextFrame()` call after resume. The display configuration may have changed during sleep (laptop docked/undocked, external monitor connected/disconnected).

**Mitigation:**
1. Handle `WM_POWERBROADCAST` messages with `PBT_APMRESUMEAUTOMATIC` and `PBT_APMSUSPEND` to detect sleep/resume cycles.
2. On resume: the DR-02 recovery sequence handles the `ACCESS_LOST` naturally. Additionally, re-verify hook installation (hooks may need re-registration after resume).
3. Handle `WM_WTSSESSION_CHANGE` with `WTS_SESSION_LOCK`/`WTS_SESSION_UNLOCK` for screen lock and user switching.
4. Preserve zoom state across sleep/resume. The user should return to their magnified view.

**Contingency:** If the Desktop Duplication object enters an unrecoverable state after resume: perform a full teardown and reinitialization of DDABridge (device, swap chain, duplication object, shaders). This is heavier but handles any stale state.

---

## 6. Category E — Performance Risks

---

### DR-17: High CPU/GPU Usage at High Refresh Rates

*(Carries over from original R-18, with Desktop Duplication specifics)*

| Attribute | Value |
|-----------|-------|
| Likelihood | **Medium** |
| Impact | **Medium** |
| First Exposed | Phase 0 |

**Description:** The Render Thread calls `DwmFlush()` once per VSync. On a 60Hz display, the render loop runs 60 times per second. On 144Hz or 240Hz, it runs 144–240 times per second. Each iteration involves Desktop Duplication frame acquisition, texture copy, shader execution, and swap chain present — significantly more GPU work per frame than the Magnification API's single function call.

**Performance budget per frame at 60Hz:**
- `AcquireNextFrame()`: ~0.1ms (GPU-to-GPU texture handoff)
- Shader execution (scale + offset): ~0.2ms (a single fullscreen quad with texture sampling)
- Cursor rendering: ~0.05ms
- `Present()`: ~0.1ms
- `DwmFlush()`: blocks until VSync (not counted — it's idle wait time)

Total active work per frame should be well under 1ms. The risk is that implementation inefficiencies (unnecessary texture copies, excessive state changes, CPU-side pixel processing) push this higher.

**Mitigation:**
1. Phase 0 exit criterion E0.4 validates this: framerate stays at display refresh rate with <5% GPU utilization on mid-range hardware.
2. Avoid CPU-side texture copies. The captured desktop texture should stay on the GPU — use shader resource views to sample it directly.
3. When zoomed but the pointer is stationary and no animation is running: if `AcquireNextFrame()` returns `DXGI_OUTDUPL_FRAME_INFO` with `LastPresentTime == 0` (desktop hasn't changed), skip the shader execution and present. Just call `DwmFlush()` to maintain timing.
4. At 1.0× zoom: hide the overlay and pause capture entirely. Zero GPU cost.

**Contingency:** If GPU utilization exceeds targets on 240Hz displays: implement adaptive pacing where the capture/render loop runs at a capped rate (e.g., 60Hz) during idle periods, ramping to full VSync rate only during active zoom changes or pointer movement.

---

### DR-18: Viewport Instability at High Zoom Levels

*(Carries over from original R-19)*

| Attribute | Value |
|-----------|-------|
| Likelihood | **Medium** |
| Impact | **Medium** |
| First Exposed | Phase 2 |

**Description:** At very high zoom levels (>5×), the viewport covers a small portion of the screen. Small pointer movements cause large viewport displacement. At 10× zoom, the viewport is 1/10th of the screen in each dimension — a 10-pixel pointer movement maps to a 100-pixel viewport shift. This can feel jittery or oversensitive.

**Mitigation:**
1. ViewportTracker's proportional mapping naturally handles this — the mapping is continuous and proportional, so the viewport tracks the pointer smoothly regardless of zoom level.
2. The deadzone (micro-jitter suppression) prevents sub-pixel pointer noise from causing visible viewport movement.
3. Any damping or smoothing must not introduce perceptible lag during slow, deliberate movements.

**Contingency:** If high-zoom usability is poor despite the deadzone: add a user-configurable "tracking sensitivity" slider to settings (Phase 3). This scales the viewport tracking speed relative to pointer movement.

---

## 7. Category F — Environmental and Compatibility Risks

---

### DR-19: Anti-Cheat and Security Software Conflicts

*(Carries over from original R-20, slightly different profile — no UIAccess, but global hooks + SendInput remain)*

| Attribute | Value |
|-----------|-------|
| Likelihood | **Medium** |
| Impact | **Medium** |
| First Exposed | Phase 2 |

**Description:** Some anti-cheat engines (EAC, BattlEye, Vanguard) and enterprise security products monitor or restrict processes that install global hooks or inject keystrokes via `SendInput`. DXZoom uses both. The Desktop Duplication API itself may also trigger security software alerts — it is used by screen-capture malware, and some endpoint protection tools monitor for `DuplicateOutput` calls.

Note: DXZoom does NOT require UIAccess (unlike the original SmoothZoom), which reduces its security surface and makes it less likely to be flagged.

**Mitigation:**
1. Detect when hooks cannot be installed (`SetWindowsHookEx` returns `NULL`) and display a clear message.
2. DXZoom does not need to function while a fullscreen game is running (games are the primary anti-cheat context). If anti-cheat blocks hooks, DXZoom becomes dormant until the game exits.
3. Document known interactions with popular security products.

**Contingency:** If a specific security product blocks DXZoom: recommend the user add DXZoom to the allowlist, or use it only outside of protected game sessions.

---

### DR-20: Keyboard Layout and Virtual Key Code Variations

*(Carries over from original R-22)*

| Attribute | Value |
|-----------|-------|
| Likelihood | **Medium** |
| Impact | **Low** |
| First Exposed | Phase 3 |

**Description:** The keyboard shortcuts `Win+Plus` and `Win+Minus` use virtual key codes `VK_OEM_PLUS` and `VK_OEM_MINUS`. On non-US keyboard layouts, these physical keys may map to different virtual key codes.

**Mitigation:**
1. Accept multiple virtual key codes: `VK_OEM_PLUS` and `VK_ADD` (numpad) for zoom-in; `VK_OEM_MINUS` and `VK_SUBTRACT` (numpad) for zoom-out.
2. In Phase 3, keyboard shortcuts are user-configurable.

**Contingency:** If specific layouts are problematic: add layout detection. This is a polish item — scroll-gesture zoom (the primary interaction) is unaffected by keyboard layout.

---

### DR-21: Floating-Point Accumulation Error Over Long Sessions

*(Carries over from original R-17)*

| Attribute | Value |
|-----------|-------|
| Likelihood | **Medium** |
| Impact | **Low** |
| First Exposed | Phase 2 |

**Description:** Repeated multiplicative zoom changes can accumulate floating-point rounding error. Zooming in from 1.0× to 5.0× and back might land at 0.9997× or 1.0003× instead of exactly 1.0×.

**Mitigation:**
1. Use `double` precision for internal zoom calculations. Convert to `float` only at the DDABridge API boundary.
2. Snap to exactly 1.0× when within epsilon (0.005).
3. Snap to the configured maximum (10.0×) when within epsilon.

**Contingency:** None needed. Double precision with epsilon snapping makes this negligible.

---

## 8. Risk Summary Matrix

| ID | Risk | Likelihood | Impact | First Phase | Category |
|----|------|-----------|--------|-------------|----------|
| DR-01 | Overlay self-capture recursion | Low | Critical | 0 | DDA/Rendering |
| DR-02 | DXGI_ERROR_ACCESS_LOST recovery | High | High | 0 | DDA/Rendering |
| DR-03 | Capture-to-display latency | Medium | High | 0 | DDA/Rendering |
| DR-04 | Overlay Z-order conflicts | Medium | Medium | 0 | DDA/Rendering |
| DR-05 | D3D11 device lost or removed | Low | High | 0 | DDA/Rendering |
| DR-06 | DRM/HDCP content appears black | High | Low | 0 | DDA/Rendering |
| DR-07 | Overlay click-through reliability | Medium | High | 1 | Input/Overlay |
| DR-08 | Software cursor rendering fidelity | Medium | Medium | 1 | Input/Overlay |
| DR-09 | 1.0× show/hide transition artifacts | Medium | Medium | 2 | Input/Overlay |
| DR-10 | Hook deregistration under load | High | High | 2 | Input |
| DR-11 | Start Menu suppression failure | Medium | Medium | 2 | Input |
| DR-12 | Modifier key conflicts with apps | Medium | Medium | 2 | Input |
| DR-13 | Touchpad scroll event variations | Medium | Low | 2 | Input |
| DR-14 | Secure desktop / UAC disruption | High | Low | 2 | Recovery |
| DR-15 | Display config change while zoomed | Medium | Medium | 2 | Recovery |
| DR-16 | Sleep, hibernate, user switching | High | Low | 2 | Recovery |
| DR-17 | High CPU/GPU at high refresh rates | Medium | Medium | 0 | Performance |
| DR-18 | Viewport instability at high zoom | Medium | Medium | 2 | Performance |
| DR-19 | Anti-cheat / security software | Medium | Medium | 2 | Environment |
| DR-20 | Keyboard layout / VK variations | Medium | Low | 3 | Environment |
| DR-21 | Floating-point accumulation error | Medium | Low | 2 | Recovery |

---

## 9. Top Five Risks by Priority

**1. DR-01 (Overlay Self-Capture Recursion) — Low likelihood, Critical impact.** If `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` doesn't work, the entire Desktop Duplication approach is blocked. This is why it's the Phase 0 risk gate. Likelihood is low because the API is well-documented and widely used, but the consequence of failure is project-pivoting.

**2. DR-02 (ACCESS_LOST Recovery) — High likelihood, High impact.** This WILL happen during normal use (resolution changes, UAC prompts, sleep/resume). The recovery must be fast (<1 second), reliable (no leaked resources, no stale state), and invisible to the user (overlay hides and reappears cleanly). A slow or unreliable recovery makes the tool feel broken.

**3. DR-10 (Hook Deregistration) — High likelihood, High impact.** Carried over from the original project and unchanged. Hooks will be silently removed under load. The watchdog timer is the primary mitigation. Keep hook callbacks absolutely minimal.

**4. DR-03 (Capture-to-Display Latency) — Medium likelihood, High impact.** The Desktop Duplication pipeline inherently adds latency compared to the Magnification API. One frame of latency is acceptable. Two frames is noticeable but tolerable. More than two frames makes the tool feel sluggish. The pipeline structure (acquire → render → present → DwmFlush with no blocking steps between acquire and present) is the primary mitigation.

**5. DR-07 (Overlay Click-Through Reliability) — Medium likelihood, High impact.** If clicks don't reliably pass through the overlay, the magnifier is unusable — the user can see elements but can't interact with them. `WS_EX_TRANSPARENT` is the standard approach and works for most input types, but drag-and-drop and touch input may have edge cases that require investigation.

---

## 10. Risks Eliminated by the Fork

The following original SmoothZoom risks (R-xx) no longer apply to DXZoom:

| Original ID | Risk | Why Eliminated |
|-------------|------|---------------|
| R-01 | Magnification API deprecation | DXZoom uses Desktop Duplication, not the Magnification API |
| R-02 | Float-precision zoom quantization | DXZoom controls the shader — zoom precision is whatever the shader computes |
| R-03 | MagSetFullscreenTransform latency | No Magnification API calls |
| R-04 | MagSetInputTransform desynchronization | No input transform — overlay is click-through, no coordinate remapping |
| R-09 | Inconsistent UIA focus events | Focus following is out of scope |
| R-10 | Caret position detection failures | Caret following is out of scope |
| R-11 | UIA callback latency | No UIA thread |
| R-12 | UIAccess code signing complexity | Desktop Duplication does not require UIAccess |
| R-14 | Crash leaving screen magnified | With DDA, a crash kills the overlay and the desktop returns to normal — inherently safe |
