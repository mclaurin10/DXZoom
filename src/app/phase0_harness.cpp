// =============================================================================
// DXZoom — Phase 0: Desktop Duplication API Capture and Render Spike
//
// Purpose: Validate three critical assumptions before full development:
//   1. SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) prevents self-capture
//   2. Real-time 60fps capture + render with <5% GPU utilization
//   3. DXGI_ERROR_ACCESS_LOST recovery when resolution changes
//
// This is a single-file test harness with no architecture. It displays a
// hardcoded 2× magnified view of the center of the desktop in a full-screen
// overlay. Exit with Escape key.
//
// Exit Criteria:
//   E0.1 - Overlay shows 2× magnified center of desktop, real-time updates
//   E0.2 - Overlay NOT visible in captured frame (no infinite recursion) [RISK GATE]
//   E0.3 - No perceptible lag
//   E0.4 - <5% GPU utilization at display refresh rate
//   E0.5 - Recovery within 1 second after resolution change
// =============================================================================

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwmapi.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdint>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "d3dcompiler.lib")

// ─── Shader Source Code ─────────────────────────────────────────────────────
// Vertex shader: generates a fullscreen triangle from vertex ID
static const char* g_vertexShaderSource = R"(
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
)";

// Pixel shader: samples the captured desktop texture with zoom and offset
static const char* g_pixelShaderSource = R"(
Texture2D desktopTexture : register(t0);
SamplerState texSampler : register(s0);

cbuffer ZoomParams : register(b0) {
    float2 uvOffset;    // Top-left corner of viewport in UV space (0-1)
    float2 uvScale;     // Size of viewport in UV space (1/zoom)
};

float4 main(float2 tex : TEXCOORD0) : SV_TARGET {
    // Flip both axes to correct 180° rotation (UV-to-screen mapping)
    float2 sampleUV = uvOffset + float2(1.0 - tex.x, 1.0 - tex.y) * uvScale;
    return desktopTexture.Sample(texSampler, sampleUV);
}
)";

// Constant buffer structure (must match shader cbuffer)
struct ZoomParams {
    float uvOffsetX;
    float uvOffsetY;
    float uvScaleX;
    float uvScaleY;
};

// ─── Global State ───────────────────────────────────────────────────────────
static HWND g_overlayWindow = nullptr;
static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static IDXGISwapChain* g_swapChain = nullptr;
static ID3D11RenderTargetView* g_renderTargetView = nullptr;
static IDXGIOutputDuplication* g_duplication = nullptr;
static ID3D11VertexShader* g_vertexShader = nullptr;
static ID3D11PixelShader* g_pixelShader = nullptr;
static ID3D11SamplerState* g_samplerState = nullptr;
static ID3D11Buffer* g_constantBuffer = nullptr;

static int g_screenW = 0;
static int g_screenH = 0;
static bool g_running = true;

// ─── Forward Declarations ───────────────────────────────────────────────────
bool recreateDuplication();
bool recreateSwapChain(int newWidth, int newHeight);

// ─── Helper: Compile Shader ─────────────────────────────────────────────────
static bool compileShader(const char* source, const char* entryPoint, const char* target, ID3DBlob** outBlob)
{
    ID3DBlob* errorBlob = nullptr;
    HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr,
                            entryPoint, target, 0, 0, outBlob, &errorBlob);

    if (FAILED(hr)) {
        if (errorBlob) {
            printf("Shader compilation failed: %s\n", (char*)errorBlob->GetBufferPointer());
            errorBlob->Release();
        }
        return false;
    }

    if (errorBlob) errorBlob->Release();
    return true;
}

// ─── Recreate Duplication (ACCESS_LOST Recovery) ────────────────────────────
bool recreateDuplication()
{
    // Release old duplication
    if (g_duplication) {
        g_duplication->Release();
        g_duplication = nullptr;
    }

    // Retry with backoff
    const int retries = 5;
    const DWORD delays[] = { 0, 50, 100, 200, 500 };

    for (int i = 0; i < retries; i++) {
        if (delays[i] > 0) Sleep(delays[i]);

        // Get DXGI device
        IDXGIDevice* dxgiDevice = nullptr;
        HRESULT hr = g_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
        if (FAILED(hr)) continue;

        // Get adapter
        IDXGIAdapter* adapter = nullptr;
        hr = dxgiDevice->GetAdapter(&adapter);
        dxgiDevice->Release();
        if (FAILED(hr)) continue;

        // Get primary output
        IDXGIOutput* output = nullptr;
        hr = adapter->EnumOutputs(0, &output);
        adapter->Release();
        if (FAILED(hr)) continue;

        // Query for IDXGIOutput1
        IDXGIOutput1* output1 = nullptr;
        hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
        output->Release();
        if (FAILED(hr)) continue;

        // Create duplication
        hr = output1->DuplicateOutput(g_device, &g_duplication);

        if (SUCCEEDED(hr)) {
            // Check if resolution changed
            DXGI_OUTDUPL_DESC duplDesc;
            g_duplication->GetDesc(&duplDesc);

            if (duplDesc.ModeDesc.Width != (UINT)g_screenW ||
                duplDesc.ModeDesc.Height != (UINT)g_screenH) {
                printf("[INFO] Resolution changed: %d×%d → %u×%u\n",
                       g_screenW, g_screenH,
                       duplDesc.ModeDesc.Width, duplDesc.ModeDesc.Height);

                output1->Release();
                if (!recreateSwapChain(duplDesc.ModeDesc.Width, duplDesc.ModeDesc.Height)) {
                    return false;
                }
                return true;
            }

            output1->Release();
            printf("[OK] Desktop Duplication recreated\n");
            return true;
        }

        output1->Release();

        if (hr == E_ACCESSDENIED) {
            // Secure desktop or another duplication active — keep retrying
            continue;
        }
    }

    printf("[ERROR] Failed to recreate Desktop Duplication after %d retries\n", retries);
    return false;
}

// ─── Recreate Swap Chain (Resolution Change) ────────────────────────────────
bool recreateSwapChain(int newWidth, int newHeight)
{
    // Release old render target view
    if (g_renderTargetView) {
        g_renderTargetView->Release();
        g_renderTargetView = nullptr;
    }

    // Resize buffers
    HRESULT hr = g_swapChain->ResizeBuffers(0, newWidth, newHeight, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        printf("[ERROR] ResizeBuffers failed: 0x%08X\n", hr);
        return false;
    }

    // Get new back buffer
    ID3D11Texture2D* backBuffer = nullptr;
    hr = g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    if (FAILED(hr)) {
        printf("[ERROR] GetBuffer failed: 0x%08X\n", hr);
        return false;
    }

    // Create new render target view
    hr = g_device->CreateRenderTargetView(backBuffer, nullptr, &g_renderTargetView);
    backBuffer->Release();
    if (FAILED(hr)) {
        printf("[ERROR] CreateRenderTargetView failed: 0x%08X\n", hr);
        return false;
    }

    // Update screen dimensions and resize window
    g_screenW = newWidth;
    g_screenH = newHeight;
    SetWindowPos(g_overlayWindow, HWND_TOPMOST, 0, 0, newWidth, newHeight, SWP_NOACTIVATE);

    printf("[OK] Swap chain recreated: %d×%d\n", newWidth, newHeight);
    return true;
}

// ─── Window Procedure ───────────────────────────────────────────────────────
static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                printf("[INFO] Escape pressed — exiting\n");
                g_running = false;
                PostQuitMessage(0);
                return 0;
            }
            break;

        case WM_CLOSE:
            g_running = false;
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ─── Entry Point ────────────────────────────────────────────────────────────
int WINAPI wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE /*hPrevInstance*/,
    _In_ LPWSTR /*lpCmdLine*/,
    _In_ int /*nCmdShow*/)
{
    // Allocate console for diagnostics
    AllocConsole();
    FILE* fp = nullptr;
    freopen_s(&fp, "CONOUT$", "w", stdout);

    printf("=== DXZoom Phase 0: Desktop Duplication Spike ===\n");
    printf("Validating:\n");
    printf("  E0.1 - Real-time 2× magnified center view\n");
    printf("  E0.2 - Self-capture exclusion [RISK GATE]\n");
    printf("  E0.3 - No perceptible lag\n");
    printf("  E0.4 - <5%% GPU utilization\n");
    printf("  E0.5 - ACCESS_LOST recovery\n\n");
    printf("Press Escape to exit.\n\n");

    // Get physical pixel dimensions (not DPI-virtualized)
    // This ensures the overlay matches the actual monitor resolution on high-DPI displays
    HMONITOR monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfo(monitor, &mi);
    g_screenW = mi.rcMonitor.right - mi.rcMonitor.left;
    g_screenH = mi.rcMonitor.bottom - mi.rcMonitor.top;
    printf("[INFO] Primary monitor physical resolution: %d×%d\n", g_screenW, g_screenH);

    // ─── Step 1: Create Overlay Window ─────────────────────────────────────
    printf("[STEP 1] Creating overlay window...\n");

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = windowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"DXZoomPhase0";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

    if (!RegisterClassExW(&wc)) {
        printf("[ERROR] RegisterClassExW failed: %lu\n", GetLastError());
        return 1;
    }

    g_overlayWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        L"DXZoomPhase0",
        L"DXZoom Phase 0",
        WS_POPUP,
        0, 0, g_screenW, g_screenH,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!g_overlayWindow) {
        printf("[ERROR] CreateWindowExW failed: %lu\n", GetLastError());
        return 1;
    }

    // Apply self-capture exclusion — CRITICAL RISK GATE
    if (!SetWindowDisplayAffinity(g_overlayWindow, WDA_EXCLUDEFROMCAPTURE)) {
        DWORD err = GetLastError();
        printf("[ERROR] SetWindowDisplayAffinity failed: %lu\n", err);
        printf("[RISK GATE FAILED] E0.2 cannot be validated!\n");
        printf("Possible causes:\n");
        printf("  - Windows version < 2004 (build 19041)\n");
        printf("  - Driver does not support WDA_EXCLUDEFROMCAPTURE\n");
        DestroyWindow(g_overlayWindow);
        return 1;
    }
    printf("[OK] SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) succeeded\n");

    ShowWindow(g_overlayWindow, SW_SHOW);

    // ─── Declare variables (before any goto cleanup) ───────────────────────
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    D3D11_SAMPLER_DESC sampDesc = {};
    D3D11_BUFFER_DESC cbDesc = {};
    ZoomParams zoomParams = {};
    DWORD frameCount = 0;
    DWORD lastReportTime = 0;

    // ─── Step 2: Create D3D11 Device and Swap Chain ────────────────────────
    printf("[STEP 2] Creating D3D11 device and swap chain...\n");

    DXGI_SWAP_CHAIN_DESC scDesc = {};
    scDesc.BufferCount = 2;
    scDesc.BufferDesc.Width = g_screenW;
    scDesc.BufferDesc.Height = g_screenH;
    scDesc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scDesc.BufferDesc.RefreshRate.Numerator = 60;
    scDesc.BufferDesc.RefreshRate.Denominator = 1;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.OutputWindow = g_overlayWindow;
    scDesc.SampleDesc.Count = 1;
    scDesc.SampleDesc.Quality = 0;
    scDesc.Windowed = TRUE;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.Flags = 0;

    D3D_FEATURE_LEVEL featureLevel;
    UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createFlags,
        nullptr, 0,
        D3D11_SDK_VERSION,
        &scDesc,
        &g_swapChain,
        &g_device,
        &featureLevel,
        &g_context
    );

    if (FAILED(hr)) {
        printf("[ERROR] D3D11CreateDeviceAndSwapChain failed: 0x%08X\n", hr);
        DestroyWindow(g_overlayWindow);
        return 1;
    }
    printf("[OK] D3D11 device and swap chain created (feature level: 0x%X)\n", featureLevel);

    // Create render target view
    ID3D11Texture2D* backBuffer = nullptr;
    hr = g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    if (FAILED(hr)) {
        printf("[ERROR] GetBuffer failed: 0x%08X\n", hr);
        goto cleanup;
    }

    hr = g_device->CreateRenderTargetView(backBuffer, nullptr, &g_renderTargetView);
    backBuffer->Release();
    if (FAILED(hr)) {
        printf("[ERROR] CreateRenderTargetView failed: 0x%08X\n", hr);
        goto cleanup;
    }

    // ─── Step 3: Create Desktop Duplication ────────────────────────────────
    printf("[STEP 3] Creating Desktop Duplication object...\n");

    if (!recreateDuplication()) {
        printf("[ERROR] Initial Desktop Duplication creation failed\n");
        goto cleanup;
    }

    // ─── Step 4: Compile Shaders ────────────────────────────────────────────
    printf("[STEP 4] Compiling shaders...\n");

    if (!compileShader(g_vertexShaderSource, "main", "vs_5_0", &vsBlob)) {
        printf("[ERROR] Vertex shader compilation failed\n");
        goto cleanup;
    }

    hr = g_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_vertexShader);
    vsBlob->Release();
    if (FAILED(hr)) {
        printf("[ERROR] CreateVertexShader failed: 0x%08X\n", hr);
        goto cleanup;
    }

    if (!compileShader(g_pixelShaderSource, "main", "ps_5_0", &psBlob)) {
        printf("[ERROR] Pixel shader compilation failed\n");
        goto cleanup;
    }

    hr = g_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_pixelShader);
    psBlob->Release();
    if (FAILED(hr)) {
        printf("[ERROR] CreatePixelShader failed: 0x%08X\n", hr);
        goto cleanup;
    }

    printf("[OK] Shaders compiled\n");

    // Create sampler state (bilinear filtering)
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;

    hr = g_device->CreateSamplerState(&sampDesc, &g_samplerState);
    if (FAILED(hr)) {
        printf("[ERROR] CreateSamplerState failed: 0x%08X\n", hr);
        goto cleanup;
    }

    // Create constant buffer
    cbDesc.ByteWidth = sizeof(ZoomParams);
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    hr = g_device->CreateBuffer(&cbDesc, nullptr, &g_constantBuffer);
    if (FAILED(hr)) {
        printf("[ERROR] CreateBuffer (constant buffer) failed: 0x%08X\n", hr);
        goto cleanup;
    }

    printf("[OK] Pipeline state created\n");

    // ─── Step 5: Render Loop ────────────────────────────────────────────────
    printf("[STEP 5] Entering render loop...\n");
    printf("────────────────────────────────────────────────────────────────────\n\n");
    printf("Phase 0 harness running. Press Escape to exit.\n");
    printf("Monitor GPU usage in Task Manager while running.\n\n");

    // Hardcoded 2× zoom parameters (center of screen)
    // zoom = 2.0, so we sample the center 50% of the desktop (uvScale = 0.5, 0.5)
    // and stretch it to fill the overlay
    zoomParams.uvOffsetX = 0.25f;  // Start at 25% from left
    zoomParams.uvOffsetY = 0.25f;  // Start at 25% from top
    zoomParams.uvScaleX = 0.5f;    // Sample 50% width
    zoomParams.uvScaleY = 0.5f;    // Sample 50% height

    frameCount = 0;
    lastReportTime = GetTickCount();

    while (g_running) {
        // Process messages
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running) break;

        // ─── Acquire Desktop Frame ──────────────────────────────────────────
        IDXGIResource* desktopResource = nullptr;
        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        hr = g_duplication->AcquireNextFrame(0, &frameInfo, &desktopResource);

        if (hr == DXGI_ERROR_ACCESS_LOST) {
            printf("[WARN] DXGI_ERROR_ACCESS_LOST — recreating duplication\n");
            if (!recreateDuplication()) {
                printf("[ERROR] Failed to recover from ACCESS_LOST\n");
                break;
            }
            continue;
        }

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            // No new frame — desktop is static
            DwmFlush();
            continue;
        }

        if (FAILED(hr)) {
            // Unexpected error
            printf("[WARN] AcquireNextFrame failed: 0x%08X\n", hr);
            DwmFlush();
            continue;
        }

        // ─── Get Desktop Texture ────────────────────────────────────────────
        ID3D11Texture2D* desktopTexture = nullptr;
        hr = desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&desktopTexture);
        if (FAILED(hr)) {
            desktopResource->Release();
            g_duplication->ReleaseFrame();
            DwmFlush();
            continue;
        }

        // Create shader resource view
        ID3D11ShaderResourceView* srv = nullptr;
        hr = g_device->CreateShaderResourceView(desktopTexture, nullptr, &srv);
        if (FAILED(hr)) {
            desktopTexture->Release();
            desktopResource->Release();
            g_duplication->ReleaseFrame();
            DwmFlush();
            continue;
        }

        // ─── Render Magnified View ──────────────────────────────────────────
        g_context->OMSetRenderTargets(1, &g_renderTargetView, nullptr);

        D3D11_VIEWPORT viewport = {};
        viewport.Width = (float)g_screenW;
        viewport.Height = (float)g_screenH;
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        g_context->RSSetViewports(1, &viewport);

        // Update constant buffer
        g_context->UpdateSubresource(g_constantBuffer, 0, nullptr, &zoomParams, 0, 0);

        // Bind pipeline state
        g_context->VSSetShader(g_vertexShader, nullptr, 0);
        g_context->PSSetShader(g_pixelShader, nullptr, 0);
        g_context->PSSetShaderResources(0, 1, &srv);
        g_context->PSSetSamplers(0, 1, &g_samplerState);
        g_context->PSSetConstantBuffers(0, 1, &g_constantBuffer);

        // Draw fullscreen triangle
        g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_context->Draw(3, 0);

        // Present
        g_swapChain->Present(0, 0);

        // Release frame resources
        srv->Release();
        desktopTexture->Release();
        g_duplication->ReleaseFrame();
        desktopResource->Release();

        // Frame pacing
        DwmFlush();

        // Frame counter
        frameCount++;
        DWORD now = GetTickCount();
        if (now - lastReportTime >= 5000) {
            float fps = frameCount / ((now - lastReportTime) / 1000.0f);
            printf("[INFO] %u frames in 5s (%.1f fps)\n", frameCount, fps);
            frameCount = 0;
            lastReportTime = now;
        }
    }

    printf("\n────────────────────────────────────────────────────────────────────\n");
    printf("Harness exited. Cleaning up...\n");

cleanup:
    // ─── Step 6: Cleanup ────────────────────────────────────────────────────
    if (g_constantBuffer) g_constantBuffer->Release();
    if (g_samplerState) g_samplerState->Release();
    if (g_pixelShader) g_pixelShader->Release();
    if (g_vertexShader) g_vertexShader->Release();
    if (g_duplication) g_duplication->Release();
    if (g_renderTargetView) g_renderTargetView->Release();
    if (g_swapChain) g_swapChain->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
    if (g_overlayWindow) DestroyWindow(g_overlayWindow);

    printf("\n=== Phase 0 Exit Criteria Checklist ===\n");
    printf("E0.1 - Did the overlay show a 2× magnified view of the center?\n");
    printf("E0.2 - [RISK GATE] Was the overlay window NOT visible in the magnified view?\n");
    printf("E0.3 - Did desktop changes appear instantly in the overlay?\n");
    printf("E0.4 - Was GPU utilization below 5%% (check Task Manager)?\n");
    printf("E0.5 - (Test manually) Change resolution — did it recover within 1 second?\n");
    printf("\n");

    if (fp) fclose(fp);
    FreeConsole();

    return 0;
}
