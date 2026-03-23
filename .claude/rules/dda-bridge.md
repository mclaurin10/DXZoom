---
paths:
  - "**/DDABridge*"
---
# DDABridge — Desktop Duplication API + Direct3D 11 Rendering

## RULE: API Isolation Boundary

**No file other than DDABridge.cpp/h may `#include` DXGI or Direct3D 11 headers, or call any DXGI/D3D11 functions.** This is absolute. If you are writing code in any other component that needs capture or rendering behavior, it must go through DDABridge's public interface. This isolation bounds future migrations to this single file.

**Forbidden headers outside DDABridge:**
- `<dxgi.h>`, `<dxgi1_2.h>`, `<dxgi1_3.h>`, `<dxgi1_4.h>`, `<dxgi1_5.h>`, `<dxgi1_6.h>`
- `<d3d11.h>`, `<d3d11_1.h>`, `<d3d11_2.h>`, `<d3d11_3.h>`, `<d3d11_4.h>`
- `<d3dcommon.h>`, `<d3dcompiler.h>`

## DDABridge Interface Contract

DDABridge exposes the same `setTransform(float zoom, int xOff, int yOff)` interface as MagBridge did. This ensures all upstream components — ZoomController, ViewportTracker, RenderLoop — are unchanged.

```cpp
class DDABridge {
public:
    bool initialize();                         // Create D3D device, swap chain, duplication object
    void shutdown();                           // Release all resources, hide overlay
    void setTransform(float zoom, int xOff, int yOff);  // Update zoom/offset, render frame
    bool hasError() const;                     // Check for persistent errors
};
```

## Threading Constraints

All DDABridge methods must be called from the **Render Thread**. This includes `initialize()`, `shutdown()`, and `setTransform()`. The D3D11 device context and swap chain are not thread-safe. Violating this causes undefined behavior.

## Capture Pipeline — IDXGIOutputDuplication

**Initialization:**
1. Create `ID3D11Device` and `ID3D11DeviceContext`
2. Get `IDXGIDevice` from the D3D device
3. Get `IDXGIAdapter` from the DXGI device
4. Get `IDXGIOutput` for the primary monitor (output 0)
5. Query `IDXGIOutput1` and call `DuplicateOutput()` to get `IDXGIOutputDuplication`

**Per-Frame Capture:**
1. Call `AcquireNextFrame()` on the duplication object
2. Get the desktop texture from the `IDXGIResource`
3. Render the texture to the overlay (see Rendering Pipeline below)
4. Call `ReleaseFrame()`

**Error Recovery: DXGI_ERROR_ACCESS_LOST**

`AcquireNextFrame()` returns `DXGI_ERROR_ACCESS_LOST` when:
- Display resolution changes
- User switches to secure desktop (Ctrl+Alt+Delete, UAC prompt)
- Display mode changes (fullscreen app transitions, HDR toggle)
- Monitor configuration changes

**Recovery steps:**
1. Release the current `IDXGIOutputDuplication` object
2. Release and recreate the D3D device and swap chain (resolution may have changed)
3. Recreate `IDXGIOutputDuplication` from the new output
4. Resume capture

Implement a retry limit (e.g., 5 attempts with 200ms delays) to avoid infinite loops if the desktop is inaccessible (secure desktop). After retries are exhausted, set an error flag and stop capture until the next `setTransform()` call above 1.0×.

## Rendering Pipeline — Direct3D 11

**Overlay Window:**
- Full-screen borderless topmost `HWND`
- Style: `WS_POPUP | WS_VISIBLE`
- Extended style: `WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE`
- **Self-capture exclusion:** Call `SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)` immediately after creating the window. This prevents the overlay from appearing in the captured desktop framebuffer (Phase 0 risk gate — if this fails, the project must evaluate `Windows.Graphics.Capture` as an alternative).

**D3D11 Swap Chain:**
- Create `IDXGISwapChain` with `DXGI_SWAP_EFFECT_FLIP_DISCARD`
- Format: `DXGI_FORMAT_R8G8B8A8_UNORM`
- Buffer count: 2 (double-buffered)
- Scaling: `DXGI_SCALING_NONE`
- Swap effect: `DXGI_SWAP_EFFECT_FLIP_DISCARD` (required for `DwmFlush` sync)

**Vertex/Pixel Shader:**
- Vertex shader: fullscreen triangle or quad, transforms normalized device coordinates
- Pixel shader: samples the captured desktop texture with offset and scale based on zoom level
- Texture sampler: `D3D11_FILTER_MIN_MAG_MIP_LINEAR` for bilinear filtering (default), `D3D11_FILTER_MIN_MAG_MIP_POINT` for nearest-neighbor (when `imageSmoothingEnabled` is false in settings)

**Rendering Steps:**
1. Copy the captured desktop texture to a shader resource view (SRV)
2. Set viewport to full overlay window size
3. Update constant buffer with zoom/offset parameters
4. Bind vertex/pixel shaders, SRV, and constant buffer
5. Draw fullscreen quad
6. Render software cursor on top (see below)
7. Call `Present(0, 0)` on the swap chain (do NOT call `DwmFlush` here — RenderLoop handles that)

## Software Cursor Rendering

DDABridge is responsible for drawing the cursor. The captured desktop texture does not include the cursor (Desktop Duplication captures only the desktop content, not the cursor layer).

**Per-Frame Cursor Rendering:**
1. Call `GetCursorInfo()` to get cursor visibility and position
2. If cursor is visible and within the magnified region:
   - Get the cursor icon handle from `CURSORINFO::hCursor`
   - Call `GetIconInfo()` to get hotspot offset
   - Draw the cursor bitmap at the magnified position: `cursorScreenPos * zoom - viewportOffset`
   - Use `DrawIconEx()` with the overlay DC, or render the cursor bitmap as a texture in D3D11
3. Hide the system cursor when the overlay is visible (zoom > 1.0×) using `ShowCursor(FALSE)`

**Cursor icon changes:** Query `GetCursorInfo()` every frame. The icon handle changes when the user hovers over different UI elements (arrow → I-beam → hand → resize, etc.). Render the correct icon each frame.

## The 1.0× Hide-Overlay Behavior

At zoom level 1.0×:
- Call `ShowWindow(overlayHwnd, SW_HIDE)` to hide the overlay
- Pause Desktop Duplication capture (skip `AcquireNextFrame()` calls)
- GPU cost is zero
- The system cursor is restored (`ShowCursor(TRUE)`)

When zoom transitions above 1.0×:
- Call `ShowWindow(overlayHwnd, SW_SHOW)` to show the overlay
- Resume Desktop Duplication capture
- Hide the system cursor (`ShowCursor(FALSE)`)

This transition must be visually seamless — no flash, pop, or frame skip.

**Why:** At 1.0×, DXZoom must be perceptually invisible with no visual artifact, no performance impact, and no input interference. Hiding the overlay and pausing capture ensures this.

## Overlay Input Passthrough

The overlay window uses `WS_EX_TRANSPARENT | WS_EX_LAYERED` to be fully input-transparent. All mouse and keyboard input passes through to the desktop beneath. No coordinate remapping or `SendInput` forwarding is needed.

Scroll interception for zoom is handled by the global low-level mouse hook in InputInterceptor (unchanged from the original design).

## Error Handling

- Every DXGI/D3D11 call that returns `HRESULT`: check for failure, log via `GetLastError()` or `_com_error`, set internal error flag.
- RenderLoop checks `hasError()`. On persistent failure: tray notification (Phase 3), fall back to hiding the overlay.
- Never silently swallow a failure — log enough detail to diagnose the issue (device lost, out of memory, invalid call, etc.).

## Shutdown Sequence — Order Matters

```
1. Hide overlay window (SW_HIDE)
2. Restore system cursor (ShowCursor(TRUE))
3. Release IDXGIOutputDuplication
4. Release D3D11 device context and device
5. Release swap chain
6. Destroy overlay window
```

Follow this order. Releasing resources in the wrong order can cause crashes or resource leaks.

## Common Mistakes

These are specific errors to watch for when writing or reviewing DDABridge code.

1. **Including DXGI/D3D11 headers in another component.** If you need capture or rendering behavior elsewhere, add a method to DDABridge's public interface. Never leak DXGI/D3D11 calls outside this file.

2. **Forgetting to call `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`.** Without this, the overlay appears in the captured desktop framebuffer, creating infinite recursion. This is a Phase 0 risk gate — if it fails, the project must pivot to `Windows.Graphics.Capture`.

3. **Calling DDABridge methods from the wrong thread.** All DDABridge methods must be called from the Render Thread. The D3D11 device context is not thread-safe.

4. **Not handling `DXGI_ERROR_ACCESS_LOST`.** This is a normal, expected error when the display resolution changes or the user switches to secure desktop. Re-create the duplication object. If recovery fails repeatedly, stop capture and set an error flag.

5. **Forgetting to hide the overlay at zoom 1.0×.** The overlay must be hidden when not zoomed to meet the zero-cost idle requirement. Check `zoom <= 1.0` in `setTransform()` and call `ShowWindow(overlayHwnd, SW_HIDE)` + skip capture.

6. **Not rendering the software cursor.** The captured desktop texture does not include the cursor. You must query `GetCursorInfo()` and draw the cursor bitmap manually every frame.

7. **Using the wrong swap effect.** `DXGI_SWAP_EFFECT_FLIP_DISCARD` is required for `DwmFlush` synchronization. `DXGI_SWAP_EFFECT_DISCARD` or `DXGI_SWAP_EFFECT_SEQUENTIAL` will not work correctly with the frame pacing model.

8. **Forgetting to update the texture sampler when `imageSmoothingEnabled` changes.** When the user toggles image smoothing in settings, recreate the sampler state with `D3D11_FILTER_MIN_MAG_MIP_POINT` (nearest-neighbor) or `D3D11_FILTER_MIN_MAG_MIP_LINEAR` (bilinear).

9. **Calling `DwmFlush()` inside DDABridge.** `DwmFlush()` is the responsibility of RenderLoop. DDABridge only calls `Present(0, 0)` on the swap chain. Do not add `DwmFlush()` to `setTransform()`.

10. **Not restoring the system cursor when the overlay is hidden.** When zoom returns to 1.0× and the overlay is hidden, call `ShowCursor(TRUE)` to restore the normal cursor. Otherwise, the cursor remains hidden on the desktop.

11. **Allocating heap memory or acquiring mutexes in `setTransform()`.** This method is called every frame from the Render Thread's hot path. No `new`, `malloc`, `std::vector::push_back`, `std::string` construction, or mutex acquisition. All buffers (constant buffers, vertex buffers, textures) must be pre-allocated during `initialize()`.

12. **Forgetting to handle DPI scaling.** Overlay window size and desktop texture size must match the monitor's actual pixel dimensions, not DPI-virtualized dimensions. Ensure the manifest declares `PerMonitorV2` DPI awareness.
