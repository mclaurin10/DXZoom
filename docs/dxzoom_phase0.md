# DXZoom — Phase 0: Capture and Render Spike

## Purpose

Phase 0 exists to retire the highest-risk technical assumptions before any real application code is written. It produces a single-file test harness — not a product, not an architecture. The harness validates that the Desktop Duplication capture pipeline and Direct3D 11 rendering pipeline can sustain a real-time magnified overlay at display refresh rate, and that the overlay window can be excluded from its own capture.

If Phase 0 passes, we know the rendering approach works and proceed to Phase 1 (input passthrough and cursor). If the self-capture exclusion risk gate fails (E0.2), the project must evaluate `Windows.Graphics.Capture` as an alternative before investing further.

## Assumptions Under Test

**Assumption 1 — Self-capture exclusion.** `SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)` prevents the overlay window from appearing in the `IDXGIOutputDuplication` captured framebuffer. Without this, the overlay captures itself in an infinite recursion loop.

**Assumption 2 — Real-time capture and render.** The pipeline (acquire desktop frame → sample texture with zoom/offset → present to overlay) can sustain the display's refresh rate (60fps on 60Hz) with acceptable GPU utilization (<5% on mid-range hardware). The magnified view updates within one frame of desktop content changes.

**Assumption 3 — ACCESS_LOST recovery.** When `IDXGIOutputDuplication` returns `DXGI_ERROR_ACCESS_LOST` (resolution change, desktop switch, UAC prompt), the duplication object and associated resources can be recreated and capture resumed within 1 second.

## Deliverable

A single C++ source file (`phase0_harness.cpp`) plus a CMakeLists.txt and application manifest. No installer, no tray icon, no settings, no input handling, no component architecture. This is throwaway validation code.

The harness:
- Creates a D3D11 device and swap chain.
- Creates a full-screen borderless topmost overlay window.
- Calls `SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)` on the overlay.
- Creates an `IDXGIOutputDuplication` on the primary monitor.
- Runs a render loop: acquire frame → render the desktop texture at a hardcoded 2× zoom with a hardcoded center offset → present → `DwmFlush()`.
- Handles `DXGI_ERROR_ACCESS_LOST` by recreating the duplication object (and swap chain if needed).
- Exits on `Escape` key press.

This harness intentionally omits: dynamic zoom, scroll input, modifier keys, viewport tracking, cursor rendering, settings, animation, and all architecture. It is the minimum code needed to validate the three assumptions.

---

## Build Requirements

- C++17, MSVC (Visual Studio 2022+), CMake, x64.
- Link: `d3d11.lib`, `dxgi.lib`, `Dwmapi.lib`, `User32.lib`.
- No UIAccess, no code signing, no secure folder. The Desktop Duplication API does not require them.
- Windows 10 version 2004+ (build 19041) for `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`.

### Application Manifest

```xml
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1">
    <application>
      <supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/>
    </application>
  </compatibility>
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">
        PerMonitorV2
      </dpiAwareness>
    </windowsSettings>
  </application>
</assembly>
```

Note: no `uiAccess="true"` — not needed for Desktop Duplication.

---

## Implementation Steps

### Step 1: Create the Overlay Window

Register a window class and create the overlay `HWND`:

```
Style:          WS_POPUP
Extended style: WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED
Position:       (0, 0) covering the full primary monitor
Size:           GetSystemMetrics(SM_CXSCREEN) × GetSystemMetrics(SM_CYSCREEN)
```

Immediately after `CreateWindowEx`, call:
```cpp
SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
```

If this returns `FALSE`, log `GetLastError()` and abort. The harness cannot proceed without self-capture exclusion.

**Why `WS_EX_LAYERED`:** Required for `WS_EX_TRANSPARENT` in Phase 1 (click-through). Including it now ensures the overlay window's basic style matches what Phase 1 will use. In Phase 0, no input passthrough is needed since we're just displaying a static magnified view.

**Why NOT `WS_EX_TRANSPARENT` in Phase 0:** The overlay doesn't need to be click-through yet. Adding it now is harmless but unnecessary. Phase 1 adds it.

Show the window: `ShowWindow(hwnd, SW_SHOW)`.

### Step 2: Create the D3D11 Device and Swap Chain

Create the D3D11 device and DXGI swap chain together using `D3D11CreateDeviceAndSwapChain`:

```
Driver type:          D3D_DRIVER_TYPE_HARDWARE
Feature level:        D3D_FEATURE_LEVEL_11_0 (minimum)
Device flags:         D3D11_CREATE_DEVICE_BGRA_SUPPORT
                      (add D3D11_CREATE_DEVICE_DEBUG for development builds)
Swap chain desc:
  Width/Height:       SM_CXSCREEN / SM_CYSCREEN (match overlay window)
  Format:             DXGI_FORMAT_B8G8R8A8_UNORM
  SampleDesc:         Count=1, Quality=0 (no MSAA)
  BufferUsage:        DXGI_USAGE_RENDER_TARGET_OUTPUT
  BufferCount:        2
  SwapEffect:         DXGI_SWAP_EFFECT_FLIP_DISCARD
  Scaling:            DXGI_SCALING_NONE
  OutputWindow:       The overlay HWND
  Windowed:           TRUE (even though the window is borderless fullscreen)
  Flags:              0
```

After creation, get the back buffer and create a render target view:
```cpp
ID3D11Texture2D* backBuffer;
swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
device->CreateRenderTargetView(backBuffer, nullptr, &renderTargetView);
backBuffer->Release();
```

**Why `DXGI_FORMAT_B8G8R8A8_UNORM`:** Desktop Duplication typically provides textures in `B8G8R8A8` format. Matching the swap chain format to the captured texture format avoids format conversion overhead.

**Why `DXGI_SWAP_EFFECT_FLIP_DISCARD`:** Required for proper `DwmFlush()` synchronization. Also the recommended swap effect for modern Windows applications.

### Step 3: Create the Desktop Duplication Object

```cpp
// Get the DXGI device from the D3D11 device
IDXGIDevice* dxgiDevice;
device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);

// Get the adapter
IDXGIAdapter* adapter;
dxgiDevice->GetAdapter(&adapter);

// Get the primary output (monitor 0)
IDXGIOutput* output;
adapter->EnumOutputs(0, &output);

// Query for IDXGIOutput1 (required for DuplicateOutput)
IDXGIOutput1* output1;
output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);

// Create the duplication
IDXGIOutputDuplication* duplication;
HRESULT hr = output1->DuplicateOutput(device, &duplication);
```

If `DuplicateOutput` fails:
- `E_ACCESSDENIED`: Another application already has an active duplication on this output, or the desktop is on the secure desktop. Log and retry after a delay.
- `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE`: Too many active duplications system-wide (limit is typically 4). Log and abort.
- `E_INVALIDARG`: Wrong device or output. Programming error — fix the code.

**Release order matters.** After getting the duplication object, release `output1`, `output`, `adapter`, and `dxgiDevice` — they're not needed anymore. The duplication object holds its own references.

### Step 4: Compile the Shaders

The Phase 0 harness needs a vertex shader and a pixel shader.

**Vertex shader:** Generates a fullscreen triangle from the vertex ID (no vertex buffer needed):

```hlsl
struct VS_OUTPUT {
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD0;
};

VS_OUTPUT main(uint vertexID : SV_VertexID) {
    VS_OUTPUT output;
    // Fullscreen triangle: vertices at (-1,-1), (3,-1), (-1,3)
    output.tex = float2((vertexID << 1) & 2, vertexID & 2);
    output.pos = float4(output.tex * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}
```

**Pixel shader:** Samples the captured desktop texture with zoom and offset:

```hlsl
Texture2D desktopTexture : register(t0);
SamplerState texSampler : register(s0);

cbuffer ZoomParams : register(b0) {
    float2 uvOffset;    // Top-left corner of the viewport in UV space (0-1)
    float2 uvScale;     // Size of the viewport in UV space (1/zoom in each dimension)
};

float4 main(float2 tex : TEXCOORD0) : SV_TARGET {
    float2 sampleUV = uvOffset + tex * uvScale;
    return desktopTexture.Sample(texSampler, sampleUV);
}
```

For Phase 0, the zoom parameters are hardcoded:
```
zoom = 2.0
uvScale = (1.0 / zoom, 1.0 / zoom) = (0.5, 0.5)
uvOffset = (0.25, 0.25)  // Center of the desktop
```

This means the shader samples the center 50% of the desktop and stretches it to fill the overlay — a 2× magnification of the center region.

**Shader compilation:** Use `D3DCompileFromFile` or embed the HLSL as string literals and use `D3DCompile`. For a Phase 0 harness, string literals are simpler (no external .hlsl files to manage).

**Sampler state:** Create a `D3D11_SAMPLER_DESC` with `D3D11_FILTER_MIN_MAG_MIP_LINEAR` (bilinear filtering). This produces smooth magnification. Phase 0 doesn't need nearest-neighbor toggle — that's a settings feature.

**Constant buffer:** Create a `D3D11_BUFFER_DESC` with `ByteWidth = sizeof(ZoomParams)` (16 bytes — two float2s, already 16-byte aligned), `Usage = D3D11_USAGE_DEFAULT`, `BindFlags = D3D11_BIND_CONSTANT_BUFFER`. Update with `UpdateSubresource` before each draw.

### Step 5: The Render Loop

```
while (running) {
    // 1. Check for Escape key to exit
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) running = false;
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) running = false;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    if (!running) break;

    // 2. Acquire the next desktop frame
    IDXGIResource* desktopResource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    HRESULT hr = duplication->AcquireNextFrame(0, &frameInfo, &desktopResource);

    if (hr == DXGI_ERROR_ACCESS_LOST) {
        // Recovery sequence (see Step 6)
        recreateDuplication();
        continue;
    }

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // No new frame available — desktop hasn't changed.
        // Still present the last frame (or skip and just DwmFlush).
        DwmFlush();
        continue;
    }

    if (FAILED(hr)) {
        // Unexpected error — log and continue
        DwmFlush();
        continue;
    }

    // 3. Get the desktop texture from the resource
    ID3D11Texture2D* desktopTexture;
    desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&desktopTexture);

    // 4. Create a shader resource view for the desktop texture
    //    (Recreate each frame — the texture handle may change)
    ID3D11ShaderResourceView* srv;
    device->CreateShaderResourceView(desktopTexture, nullptr, &srv);

    // 5. Render the magnified view to the overlay
    context->OMSetRenderTargets(1, &renderTargetView, nullptr);
    D3D11_VIEWPORT viewport = { 0, 0, (float)screenW, (float)screenH, 0, 1 };
    context->RSSetViewports(1, &viewport);

    // Update constant buffer with hardcoded 2× center zoom
    ZoomParams params = { {0.25f, 0.25f}, {0.5f, 0.5f} };
    context->UpdateSubresource(constantBuffer, 0, nullptr, &params, 0, 0);

    // Bind shaders, SRV, sampler, constant buffer
    context->VSSetShader(vertexShader, nullptr, 0);
    context->PSSetShader(pixelShader, nullptr, 0);
    context->PSSetShaderResources(0, 1, &srv);
    context->PSSetSamplers(0, 1, &samplerState);
    context->PSSetConstantBuffers(0, 1, &constantBuffer);

    // Draw fullscreen triangle (3 vertices, no vertex buffer)
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->Draw(3, 0);

    // 6. Present
    swapChain->Present(0, 0);

    // 7. Release frame resources
    srv->Release();
    desktopTexture->Release();
    duplication->ReleaseFrame();
    desktopResource->Release();

    // 8. Wait for next VSync
    DwmFlush();
}
```

**Why timeout=0 on AcquireNextFrame:** Non-blocking. If no new frame is available (desktop is static), we skip to `DwmFlush()` without wasting time waiting. The last presented frame remains on the overlay (swap effect preserves it until the next `Present`).

**Why DwmFlush after Present:** `Present(0, 0)` queues the frame but doesn't block. `DwmFlush()` blocks until the next VSync, giving us natural frame pacing tied to the display refresh rate. This is the same timing model used in the original SmoothZoom RenderLoop.

**Why recreate the SRV each frame:** `AcquireNextFrame` may return a different `ID3D11Texture2D` handle each frame (the duplication uses a rotating pool of textures). Creating an SRV is cheap (~microseconds on modern drivers). An optimization for later phases would be to cache the SRV and only recreate it when the texture handle changes.

### Step 6: ACCESS_LOST Recovery

```cpp
void recreateDuplication() {
    // Release old duplication
    if (duplication) { duplication->Release(); duplication = nullptr; }

    // Retry with backoff
    int retries = 5;
    DWORD delays[] = { 0, 50, 100, 200, 500 };

    for (int i = 0; i < retries; i++) {
        if (delays[i] > 0) Sleep(delays[i]);

        // Re-get the output (resolution may have changed)
        IDXGIOutput* output = nullptr;
        IDXGIOutput1* output1 = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIDevice* dxgiDevice = nullptr;

        device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
        dxgiDevice->GetAdapter(&adapter);
        adapter->EnumOutputs(0, &output);
        output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);

        HRESULT hr = output1->DuplicateOutput(device, &duplication);

        // Release intermediates
        output1->Release(); output->Release();
        adapter->Release(); dxgiDevice->Release();

        if (SUCCEEDED(hr)) {
            // Check if resolution changed — if so, recreate swap chain
            DXGI_OUTDUPL_DESC duplDesc;
            duplication->GetDesc(&duplDesc);
            if (duplDesc.ModeDesc.Width != screenW ||
                duplDesc.ModeDesc.Height != screenH) {
                recreateSwapChain(duplDesc.ModeDesc.Width,
                                  duplDesc.ModeDesc.Height);
            }
            return; // Success
        }

        if (hr == E_ACCESSDENIED) {
            // Secure desktop or another duplication active — keep retrying
            continue;
        }

        // Other failure — log and keep trying
    }

    // All retries exhausted — set error flag
    // In Phase 0 harness: just log and keep the loop running.
    // The next frame tick will try again.
}
```

**Swap chain recreation when resolution changes:** Release the old render target view, call `swapChain->ResizeBuffers(0, newWidth, newHeight, DXGI_FORMAT_UNKNOWN, 0)`, then recreate the render target view from the new back buffer. Update `screenW` and `screenH`. Also resize the overlay window to match: `SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, newWidth, newHeight, SWP_NOACTIVATE)`.

### Step 7: Shutdown

```cpp
// Release all resources in order
if (duplication) duplication->Release();
if (renderTargetView) renderTargetView->Release();
if (swapChain) swapChain->Release();
if (context) context->Release();
if (device) device->Release();
DestroyWindow(hwnd);
```

---

## What This Harness Does NOT Do

These are intentionally omitted. Do not add them in Phase 0.

- **No dynamic zoom.** The zoom level is hardcoded to 2× and the offset is hardcoded to center. There is no scroll input, no modifier keys, no zoom controller.
- **No cursor rendering.** The captured desktop texture does not include the cursor. The user will see their real cursor moving over the magnified content but it won't be rendered into the magnified view. This is expected and acceptable for Phase 0.
- **No input passthrough.** The overlay window is not click-through. Clicking on the overlay hits the overlay, not the desktop beneath. This is fine — Phase 0 is for visual validation only, not interactive use.
- **No 1.0× hide behavior.** The overlay is always visible. There is no zoom-level-dependent show/hide logic.
- **No component architecture.** No DDABridge class, no RenderLoop class, no separation of concerns. This is a single `main()` function with helper functions. The architecture arrives in Phase 2 when the actual components are assembled.
- **No error recovery beyond ACCESS_LOST.** Device lost (`DXGI_ERROR_DEVICE_REMOVED`) is not handled in Phase 0. If the GPU resets, the harness crashes. That's fine for a test harness.

---

## Exit Criteria

| # | Criterion | Validates |
|---|-----------|-----------|
| E0.1 | The overlay displays a 2× magnified view of the center of the desktop, updating in real time. Moving a window on the desktop is visible (magnified) in the overlay within one frame. | Assumption 2 |
| E0.2 | The overlay window itself is NOT visible in the magnified content. There is no hall-of-mirrors effect, no recursive rendering, no visual feedback loop. | Assumption 1 (risk gate — DR-01) |
| E0.3 | The magnified view updates with no perceptible lag. Dragging a window on the desktop, the magnified view tracks it in real time. | Assumption 2 |
| E0.4 | GPU utilization stays below 5% on mid-range hardware (e.g., Intel UHD 620 or NVIDIA GTX 1650) while the harness runs at display refresh rate. Verify with Task Manager's GPU tab or GPU-Z. | Assumption 2 |
| E0.5 | While the harness is running, change the display resolution (Settings → Display → Resolution). The harness recovers and resumes showing the magnified desktop at the new resolution within 1 second. No crash. | Assumption 3 |

### Risk Gate

**If E0.2 fails** (the overlay is visible in the captured frame):
1. Verify `SetWindowDisplayAffinity` returned `TRUE`. Check `GetLastError()` if it returned `FALSE`.
2. Test on a different machine / GPU to determine if the failure is driver-specific.
3. Try `WDA_MONITOR` instead of `WDA_EXCLUDEFROMCAPTURE` (more aggressive — excludes from all capture including screen sharing).
4. If no `WDA` flag works, evaluate `Windows.Graphics.Capture` with `CreateForMonitor()` as an alternative capture backend. This is a significant pivot — do not proceed to Phase 1 until the capture approach is validated.

**If E0.4 fails** (GPU utilization too high):
1. Check whether the harness is creating a new SRV every frame unnecessarily when the desktop hasn't changed (`AcquireNextFrame` returned `DXGI_ERROR_WAIT_TIMEOUT`).
2. Verify the swap chain is using `DXGI_SWAP_EFFECT_FLIP_DISCARD` — other swap effects may cause GPU-side copies.
3. Profile with PIX or RenderDoc to identify the hotspot.
4. If GPU utilization is high only on integrated graphics: this may be acceptable for the validation phase. Document the numbers and evaluate whether optimization is needed before Phase 2.

**If E0.5 fails** (ACCESS_LOST recovery doesn't work):
1. Check whether the recovery is releasing all old resources before recreating. Stale references to the old output or duplication object will cause `DuplicateOutput` to fail.
2. Increase the retry delay — the new output may not be available immediately after a resolution change.
3. Check whether the swap chain also needs to be recreated (buffer dimensions must match the new resolution).

---

## Testing Notes

- **Test on at least two machines with different GPUs** (one Intel integrated, one discrete NVIDIA or AMD) to validate that the pipeline isn't driver-dependent.
- **Test the Escape-to-exit path.** The harness must release all DXGI/D3D11 resources cleanly. Leaking the duplication object prevents other applications from using Desktop Duplication until the process exits.
- **Test ACCESS_LOST by triggering a UAC prompt** (e.g., run a program as administrator while the harness is active). The secure desktop triggers `ACCESS_LOST`. Verify the harness recovers when the UAC dialog is dismissed.
- **Observe the overlay for visual artifacts:** tearing (horizontal line where the top and bottom halves are from different frames), color banding, misalignment between the overlay and desktop coordinates, or a one-pixel border where the overlay doesn't quite cover the screen.
- **Check that the magnified view is actually 2× and centered.** A window in the center of the desktop should appear twice as large in the overlay. Corners of the desktop should not be visible (the 2× viewport covers only the center 50% of each dimension).
