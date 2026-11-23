/*
 * runtime_window.c - webnative demo window (rects + baked text).
 *
 * Win32 window + wgpu-native swapchain. The scene (rect vertices, glyph
 * vertices, the R8 glyph atlas and both WGSL shaders) is baked into
 * scene_data.h at build time; this runtime contains no JSON parser, no Python,
 * no FreeType/HarfBuzz, no font file and no browser.
 *
 * Header compatibility:
 *   - emit_c.py        (rect-only)   -> no SCENE_GLYPH_QUAD_COUNT; text stage
 *                                      is compiled out, the window is rect-only
 *   - emit_c_text.py   (rect+text)   -> glyph buffer + atlas + glyph pipeline
 *   - exp16 scene_data.h (rect+text) -> also accepted (same macro vocabulary)
 *
 * Rendering: ONE render pass per frame, draw rects first (solid pipeline) and
 * then glyph quads (textured pipeline, alpha blend). Resizing reconfigures the
 * surface in place; the atlas texture/bind group/pipelines are size-independent
 * and are not rebuilt, so glyph rendering survives resize.
 *
 * Build (MSVC):
 *   cl /nologo /O2 /W3 /std:c17 /TC runtime_window.c /I<wgpu-native>\include
 *      /Fe:webnative-window.exe
 *      /link /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup
 *            <wgpu-native>\lib\wgpu_native.dll.lib user32.lib gdi32.lib
 *
 * Usage: webnative-window.exe [--frames N] [--resize-after N]
 *   --frames N       render N frames then exit 0 (automation / smoke test)
 *   --resize-after N request a 720x480 client resize after frame N; the next
 *                    frame reconfigures the surface and keeps drawing glyphs
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wgpu.h>

#include "scene_data.h"

#define WINDOW_W 960
#define WINDOW_H 640
#define WINDOW_CLASS L"WebNativeGpuWindow"
#define WINDOW_TITLE L"webnative - nativo GPU (wgpu-native / D3D12)"

#if defined(SCENE_GLYPH_QUAD_COUNT) && (SCENE_GLYPH_QUAD_COUNT > 0)
#define SCENE_HAS_TEXT 1
#else
#define SCENE_HAS_TEXT 0
#endif

#ifndef SCENE_RECT_COUNT
#define SCENE_RECT_COUNT SCENE_OP_COUNT
#endif

/* emit_c_text.py exposes kRectShaderWGSL; the rect-only emitter exposes
 * kSceneShaderWGSL. exp16 headers also use kRectShaderWGSL. */
#if defined(SCENE_GLYPH_QUAD_COUNT)
#define SCENE_RECT_SHADER kRectShaderWGSL
#else
#define SCENE_RECT_SHADER kSceneShaderWGSL
#endif

#if SCENE_HAS_TEXT
_Static_assert(sizeof(GlyphVertex) == SCENE_GLYPH_VERTEX_STRIDE,
               "GlyphVertex must stay 20 bytes (vec2 + vec2 + unorm8x4)");
_Static_assert(sizeof(kAtlasPixels) == ATLAS_PIXELS,
               "kAtlasPixels must be ATLAS_WIDTH * ATLAS_HEIGHT bytes");
#endif
_Static_assert(sizeof(SceneVertex) == SCENE_VERTEX_STRIDE,
               "SceneVertex must stay 12 bytes (vec2 + unorm8x4)");

static volatile LONG g_running = 1;
static int g_max_frames = 0; /* 0 = run until the user closes the window */
static int g_resize_after = 0; /* 0 = never request a programmatic resize */
static volatile LONG g_resize_pending = 0;
static int g_client_width = WINDOW_W;
static int g_client_height = WINDOW_H;

/* ------------------------------------------------------------------ utils */

static WGPUStringView sv(const char *text)
{
    WGPUStringView view;
    view.data = text;
    view.length = WGPU_STRLEN;
    return view;
}

static void fprint_view(FILE *stream, WGPUStringView view)
{
    if (view.data == NULL || view.length == 0) {
        fputs("(none)", stream);
    } else if (view.length == WGPU_STRLEN) {
        fputs(view.data, stream);
    } else {
        int n = view.length > 4096 ? 4096 : (int)view.length;
        fprintf(stream, "%.*s", n, view.data);
    }
}

static double now_ms(void)
{
    LARGE_INTEGER freq, tick;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&tick);
    return (double)tick.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static void fatal(const char *message)
{
    fprintf(stderr, "fatal: %s\n", message);
    fflush(stderr);
    /* Automated runs (--frames) must never block on a modal dialog. */
    if (g_max_frames <= 0) {
        MessageBoxA(NULL, message, "webnative", MB_ICONERROR | MB_OK);
    }
}

static const char *backend_name(WGPUBackendType type)
{
    switch (type) {
    case WGPUBackendType_D3D11: return "D3D11";
    case WGPUBackendType_D3D12: return "D3D12";
    case WGPUBackendType_Metal: return "Metal";
    case WGPUBackendType_Vulkan: return "Vulkan";
    case WGPUBackendType_OpenGL: return "OpenGL";
    case WGPUBackendType_OpenGLES: return "OpenGLES";
    case WGPUBackendType_WebGPU: return "WebGPU";
    case WGPUBackendType_Null: return "Null";
    default: return "Undefined";
    }
}

static const char *adapter_type_name(WGPUAdapterType type)
{
    switch (type) {
    case WGPUAdapterType_DiscreteGPU: return "discrete-gpu";
    case WGPUAdapterType_IntegratedGPU: return "integrated-gpu";
    case WGPUAdapterType_CPU: return "cpu";
    case WGPUAdapterType_Unknown: return "unknown";
    default: return "undefined";
    }
}

static void on_log(WGPULogLevel level, WGPUStringView message, void *userdata)
{
    (void)userdata;
    if (level < WGPULogLevel_Warn) {
        return;
    }
    fprintf(stderr, "[wgpu %d] ", (int)level);
    fprint_view(stderr, message);
    fputc('\n', stderr);
    fflush(stderr);
}

/* wgpu-native v29 does not implement wgpuInstanceWaitAny; poll instead. */
static int wait_flag(WGPUInstance instance, volatile int *flag, const char *what)
{
    DWORD start = GetTickCount();
    while (!*flag) {
        wgpuInstanceProcessEvents(instance);
        if (GetTickCount() - start > 30000u) {
            fprintf(stderr, "fatal: %s timed out\n", what);
            return 1;
        }
        Sleep(1);
    }
    return 0;
}

/* --------------------------------------------------------------- callbacks */

typedef struct {
    WGPUAdapter adapter;
    WGPURequestAdapterStatus status;
    volatile int done;
} AdapterResult;

typedef struct {
    WGPUDevice device;
    WGPURequestDeviceStatus status;
    volatile int done;
} DeviceResult;

typedef struct {
    WGPUMapAsyncStatus status;
    volatile int done;
} MapResult;

static void on_adapter(WGPURequestAdapterStatus status, WGPUAdapter adapter,
                       WGPUStringView message, void *userdata1, void *userdata2)
{
    AdapterResult *result = (AdapterResult *)userdata1;
    (void)message;
    (void)userdata2;
    result->status = status;
    result->adapter = adapter;
    result->done = 1;
}

static void on_device(WGPURequestDeviceStatus status, WGPUDevice device,
                      WGPUStringView message, void *userdata1, void *userdata2)
{
    DeviceResult *result = (DeviceResult *)userdata1;
    (void)message;
    (void)userdata2;
    result->status = status;
    result->device = device;
    result->done = 1;
}

static void on_map(WGPUMapAsyncStatus status, WGPUStringView message,
                   void *userdata1, void *userdata2)
{
    MapResult *result = (MapResult *)userdata1;
    (void)message;
    (void)userdata2;
    result->status = status;
    result->done = 1;
}

static void on_uncaptured_error(WGPUDevice const *device, WGPUErrorType type,
                                WGPUStringView message, void *userdata1, void *userdata2)
{
    (void)device;
    (void)userdata1;
    (void)userdata2;
    fprintf(stderr, "wgpu uncaptured error (type %d): ", (int)type);
    fprint_view(stderr, message);
    fputc('\n', stderr);
    fflush(stderr);
}

static WGPUShaderModule create_shader_module(WGPUDevice device, const char *code,
                                             const char *label)
{
    WGPUShaderModuleDescriptor descriptor;
    WGPUShaderSourceWGSL wgsl;
    memset(&descriptor, 0, sizeof(descriptor));
    memset(&wgsl, 0, sizeof(wgsl));
    wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgsl.code = sv(code);
    descriptor.label = sv(label);
    descriptor.nextInChain = &wgsl.chain;
    return wgpuDeviceCreateShaderModule(device, &descriptor);
}

#if SCENE_HAS_TEXT
/* ------------------------------------------------------- atlas upload proof */

/* FNV-1a over packed rows: same hash for the tight CPU array (pitch == width)
 * and for a 256-byte-aligned GPU readback (pitch >= width). */
static unsigned int fnv1a_rows(const unsigned char *data, unsigned int row_bytes,
                               unsigned int row_pitch, unsigned int height)
{
    unsigned int hash = 2166136261u;
    unsigned int y, x;
    for (y = 0; y < height; ++y) {
        const unsigned char *row = data + (size_t)y * row_pitch;
        for (x = 0; x < row_bytes; ++x) {
            hash ^= (unsigned int)row[x];
            hash *= 16777619u;
        }
    }
    return hash;
}

/* wgpuQueueWriteTexture wants bytesPerRow % 256 == 0 when height > 1. */
static int upload_atlas_pixels(WGPUQueue queue, WGPUTexture texture)
{
    unsigned int row_pitch = ATLAS_WIDTH;
    unsigned char *staging = NULL;
    const unsigned char *data = kAtlasPixels;
    WGPUTexelCopyTextureInfo destination;
    WGPUTexelCopyBufferLayout layout;
    WGPUExtent3D write_size;
    unsigned int y;

    if (ATLAS_HEIGHT > 1 && (row_pitch % 256u) != 0) {
        row_pitch = (row_pitch + 255u) & ~255u;
        staging = (unsigned char *)calloc(1, (size_t)row_pitch * ATLAS_HEIGHT);
        if (staging == NULL) {
            fprintf(stderr, "fatal: out of memory staging the atlas rows\n");
            return 1;
        }
        for (y = 0; y < ATLAS_HEIGHT; ++y) {
            memcpy(staging + (size_t)y * row_pitch,
                   kAtlasPixels + (size_t)y * ATLAS_WIDTH, ATLAS_WIDTH);
        }
        data = staging;
    }

    memset(&destination, 0, sizeof(destination));
    destination.texture = texture;
    destination.mipLevel = 0;
    destination.origin.x = 0;
    destination.origin.y = 0;
    destination.origin.z = 0;
    destination.aspect = WGPUTextureAspect_All;
    memset(&layout, 0, sizeof(layout));
    layout.offset = 0;
    layout.bytesPerRow = row_pitch;
    layout.rowsPerImage = ATLAS_HEIGHT;
    memset(&write_size, 0, sizeof(write_size));
    write_size.width = ATLAS_WIDTH;
    write_size.height = ATLAS_HEIGHT;
    write_size.depthOrArrayLayers = 1;
    wgpuQueueWriteTexture(queue, &destination, data,
                          (size_t)row_pitch * ATLAS_HEIGHT, &layout, &write_size);
    free(staging);
    return 0;
}

/* Copy the uploaded texture back and compare FNV-1a with the baked array. */
static int verify_atlas_upload(WGPUInstance instance, WGPUDevice device,
                               WGPUQueue queue, WGPUTexture texture)
{
    unsigned int row_pitch = ATLAS_WIDTH;
    uint64_t atlas_size;
    unsigned int cpu_hash;
    unsigned int gpu_hash = 0;
    WGPUBuffer buffer = NULL;
    WGPUCommandEncoder encoder = NULL;
    WGPUCommandBuffer commands = NULL;
    WGPUTexelCopyTextureInfo source;
    WGPUTexelCopyBufferInfo destination;
    WGPUExtent3D extent;
    MapResult map_result;
    int failed = 1;

    if (ATLAS_HEIGHT > 1 && (row_pitch % 256u) != 0)
        row_pitch = (row_pitch + 255u) & ~255u;
    atlas_size = (uint64_t)row_pitch * ATLAS_HEIGHT;
    cpu_hash = fnv1a_rows(kAtlasPixels, ATLAS_WIDTH, ATLAS_WIDTH, ATLAS_HEIGHT);

    {
        WGPUBufferDescriptor descriptor;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("atlas-readback");
        descriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
        descriptor.size = atlas_size;
        buffer = wgpuDeviceCreateBuffer(device, &descriptor);
        if (buffer == NULL) {
            fprintf(stderr, "fatal: atlas readback buffer creation failed\n");
            goto done;
        }
    }

    memset(&source, 0, sizeof(source));
    source.texture = texture;
    source.mipLevel = 0;
    source.aspect = WGPUTextureAspect_All;
    memset(&destination, 0, sizeof(destination));
    destination.buffer = buffer;
    destination.layout.offset = 0;
    destination.layout.bytesPerRow = row_pitch;
    destination.layout.rowsPerImage = ATLAS_HEIGHT;
    memset(&extent, 0, sizeof(extent));
    extent.width = ATLAS_WIDTH;
    extent.height = ATLAS_HEIGHT;
    extent.depthOrArrayLayers = 1;

    encoder = wgpuDeviceCreateCommandEncoder(device, NULL);
    if (encoder == NULL) {
        fprintf(stderr, "fatal: atlas readback encoder creation failed\n");
        goto done;
    }
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &source, &destination, &extent);
    commands = wgpuCommandEncoderFinish(encoder, NULL);
    if (commands == NULL) {
        fprintf(stderr, "fatal: atlas readback command encoding failed\n");
        goto done;
    }
    wgpuQueueSubmit(queue, 1, &commands);

    memset(&map_result, 0, sizeof(map_result));
    {
        WGPUBufferMapCallbackInfo cb;
        memset(&cb, 0, sizeof(cb));
        cb.mode = WGPUCallbackMode_AllowProcessEvents;
        cb.callback = on_map;
        cb.userdata1 = &map_result;
        (void)wgpuBufferMapAsync(buffer, WGPUMapMode_Read, 0, atlas_size, cb);
        if (wait_flag(instance, &map_result.done, "map atlas") != 0)
            goto done;
    }
    if (map_result.status != WGPUMapAsyncStatus_Success) {
        fprintf(stderr, "fatal: atlas map failed (status %d)\n", (int)map_result.status);
        goto done;
    }
    {
        const unsigned char *mapped =
            (const unsigned char *)wgpuBufferGetConstMappedRange(buffer, 0, atlas_size);
        if (mapped == NULL) {
            fprintf(stderr, "fatal: atlas mapped range is NULL\n");
            goto done;
        }
        gpu_hash = fnv1a_rows(mapped, ATLAS_WIDTH, row_pitch, ATLAS_HEIGHT);
        wgpuBufferUnmap(buffer);
    }
    printf("atlas: uploaded %dx%d R8Unorm (%d bytes, row-pitch %u) "
           "gpu-hash=0x%08X cpu-hash=0x%08X %s\n",
           ATLAS_WIDTH, ATLAS_HEIGHT, ATLAS_PIXELS, row_pitch,
           gpu_hash, cpu_hash, gpu_hash == cpu_hash ? "MATCH" : "MISMATCH");
    if (gpu_hash != cpu_hash) {
        fprintf(stderr, "fatal: atlas readback differs from baked pixels\n");
        goto done;
    }
    failed = 0;

done:
    if (commands != NULL) wgpuCommandBufferRelease(commands);
    if (encoder != NULL) wgpuCommandEncoderRelease(encoder);
    if (buffer != NULL) wgpuBufferRelease(buffer);
    return failed;
}
#endif /* SCENE_HAS_TEXT */

/* --------------------------------------------------------------- window */

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_CLOSE:
        g_running = 0;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) {
            int width = LOWORD(lparam);
            int height = HIWORD(lparam);
            if (width > 0 && height > 0) {
                g_client_width = width;
                g_client_height = height;
                InterlockedExchange(&g_resize_pending, 1);
            }
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            g_running = 0;
            return 0;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int main(int argc, char **argv)
{
    WGPUInstance instance = NULL;
    WGPUSurface surface = NULL;
    WGPUAdapter adapter = NULL;
    WGPUDevice device = NULL;
    WGPUQueue queue = NULL;
    WGPUShaderModule rect_shader = NULL;
    WGPUShaderModule glyph_shader = NULL;
    WGPURenderPipeline rect_pipeline = NULL;
    WGPURenderPipeline glyph_pipeline = NULL;
    WGPUPipelineLayout glyph_pipeline_layout = NULL;
    WGPUBindGroupLayout atlas_bind_group_layout = NULL;
    WGPUBindGroup atlas_bind_group = NULL;
    WGPUBuffer rect_buffer = NULL;
    WGPUBuffer glyph_buffer = NULL;
    WGPUTexture atlas_texture = NULL;
    WGPUTextureView atlas_view = NULL;
    WGPUSampler atlas_sampler = NULL;
    WGPUSurfaceConfiguration surface_config;
    HWND hwnd = NULL;
    DWORD window_style;
    AdapterResult adapter_result;
    DeviceResult device_result;
    int exit_code = 1;
    int frames = 0;
#if SCENE_HAS_TEXT
    int glyph_frames = 0;
#endif
    int resize_count = 0;
    int resize_requested = 0;
    double t_start, t_ready;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            g_max_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--resize-after") == 0 && i + 1 < argc) {
            g_resize_after = atoi(argv[++i]);
        }
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    SetProcessDPIAware();
    wgpuSetLogLevel(WGPULogLevel_Warn);
    wgpuSetLogCallback(on_log, NULL);

    /* 1. Native window, resizable, initial client area 960x640. */
    {
        HINSTANCE hinst = GetModuleHandleW(NULL);
        WNDCLASSEXW wc;
        RECT rect = {0, 0, WINDOW_W, WINDOW_H};

        window_style = WS_OVERLAPPEDWINDOW;

        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = hinst;
        wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = WINDOW_CLASS;
        if (!RegisterClassExW(&wc)) {
            fatal("RegisterClassExW failed");
            return 1;
        }

        AdjustWindowRect(&rect, window_style, FALSE);
        hwnd = CreateWindowExW(0, WINDOW_CLASS, WINDOW_TITLE, window_style,
                               CW_USEDEFAULT, CW_USEDEFAULT,
                               rect.right - rect.left, rect.bottom - rect.top,
                               NULL, NULL, hinst, NULL);
        if (hwnd == NULL) {
            fatal("CreateWindowExW failed");
            return 1;
        }
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }

    t_start = now_ms();

    /* 2. wgpu-native: instance -> surface(HWND) -> adapter -> device. */
    instance = wgpuCreateInstance(NULL);
    if (instance == NULL) {
        fatal("wgpuCreateInstance failed");
        goto cleanup;
    }

    {
        WGPUSurfaceSourceWindowsHWND source;
        WGPUSurfaceDescriptor descriptor;

        memset(&source, 0, sizeof(source));
        source.chain.sType = WGPUSType_SurfaceSourceWindowsHWND;
        source.hinstance = GetModuleHandleW(NULL);
        source.hwnd = hwnd;

        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("webnative-window-surface");
        descriptor.nextInChain = &source.chain;
        surface = wgpuInstanceCreateSurface(instance, &descriptor);
        if (surface == NULL) {
            fatal("wgpuInstanceCreateSurface(HWND) failed");
            goto cleanup;
        }
    }

    memset(&adapter_result, 0, sizeof(adapter_result));
    {
        WGPURequestAdapterOptions options;
        WGPURequestAdapterCallbackInfo callback;

        memset(&options, 0, sizeof(options));
        options.powerPreference = WGPUPowerPreference_HighPerformance;
        options.featureLevel = WGPUFeatureLevel_Core;
        options.compatibleSurface = surface;

        memset(&callback, 0, sizeof(callback));
        callback.mode = WGPUCallbackMode_AllowProcessEvents;
        callback.callback = on_adapter;
        callback.userdata1 = &adapter_result;
        (void)wgpuInstanceRequestAdapter(instance, &options, callback);
        if (wait_flag(instance, &adapter_result.done, "request adapter") != 0) {
            fatal("adapter request timed out");
            goto cleanup;
        }
    }
    if (adapter_result.status != WGPURequestAdapterStatus_Success || adapter_result.adapter == NULL) {
        fatal("no compatible GPU adapter");
        goto cleanup;
    }
    adapter = adapter_result.adapter;

    {
        WGPUAdapterInfo info;
        memset(&info, 0, sizeof(info));
        if (wgpuAdapterGetInfo(adapter, &info) == WGPUStatus_Success) {
            printf("adapter: backend=%s (%d) type=%s vendor=0x%04X device=0x%04X\n",
                   backend_name(info.backendType), (int)info.backendType,
                   adapter_type_name(info.adapterType),
                   info.vendorID, info.deviceID);
            if (info.device.data != NULL && info.device.length > 0) {
                fputs("adapter: device=", stdout);
                fprint_view(stdout, info.device);
                fputc('\n', stdout);
            }
            if (info.description.data != NULL && info.description.length > 0) {
                fputs("adapter: description=", stdout);
                fprint_view(stdout, info.description);
                fputc('\n', stdout);
            }
            fflush(stdout);
        }
    }

    memset(&device_result, 0, sizeof(device_result));
    {
        WGPUDeviceDescriptor descriptor;
        WGPURequestDeviceCallbackInfo callback;

        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("webnative-window-device");
        descriptor.requiredFeatureCount = 0;
        descriptor.uncapturedErrorCallbackInfo.callback = on_uncaptured_error;

        memset(&callback, 0, sizeof(callback));
        callback.mode = WGPUCallbackMode_AllowProcessEvents;
        callback.callback = on_device;
        callback.userdata1 = &device_result;
        (void)wgpuAdapterRequestDevice(adapter, &descriptor, callback);
        if (wait_flag(instance, &device_result.done, "request device") != 0) {
            fatal("device request timed out");
            goto cleanup;
        }
    }
    if (device_result.status != WGPURequestDeviceStatus_Success || device_result.device == NULL) {
        fatal("no GPU device");
        goto cleanup;
    }
    device = device_result.device;
    queue = wgpuDeviceGetQueue(device);

    /* 3. Surface config: preferred format, FIFO present, current client size. */
    {
        WGPUSurfaceCapabilities capabilities;
        WGPUStatus status;
        WGPUTextureFormat format;

        memset(&capabilities, 0, sizeof(capabilities));
        status = wgpuSurfaceGetCapabilities(surface, adapter, &capabilities);
        if (status != WGPUStatus_Success || capabilities.formatCount == 0) {
            fprintf(stderr, "fatal: surface capabilities failed (status %d, formats %zu)\n",
                    (int)status, capabilities.formatCount);
            fatal("surface capabilities failed");
            goto cleanup;
        }
        format = capabilities.formats[0];
        printf("surface: formats=%zu presentModes=%zu chosenFormat=%d %dx%d\n",
               capabilities.formatCount, capabilities.presentModeCount, (int)format,
               g_client_width, g_client_height);
        fflush(stdout);

        memset(&surface_config, 0, sizeof(surface_config));
        surface_config.device = device;
        surface_config.format = format;
        surface_config.usage = WGPUTextureUsage_RenderAttachment;
        surface_config.width = (uint32_t)g_client_width;
        surface_config.height = (uint32_t)g_client_height;
        surface_config.presentMode = WGPUPresentMode_Fifo;
        surface_config.alphaMode = WGPUCompositeAlphaMode_Auto;
        wgpuSurfaceConfigure(surface, &surface_config);
    }

    /* 4. Baked geometry: rects + optional glyphs. */
    {
        WGPUBufferDescriptor descriptor;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("scene-vertices");
        descriptor.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        descriptor.size = sizeof(kSceneVertices);
        rect_buffer = wgpuDeviceCreateBuffer(device, &descriptor);
        if (rect_buffer == NULL) {
            fatal("rect vertex buffer creation failed");
            goto cleanup;
        }
        wgpuQueueWriteBuffer(queue, rect_buffer, 0, kSceneVertices, sizeof(kSceneVertices));
    }
#if SCENE_HAS_TEXT
    {
        WGPUBufferDescriptor descriptor;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("glyph-vertices");
        descriptor.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        descriptor.size = sizeof(kGlyphVertices);
        glyph_buffer = wgpuDeviceCreateBuffer(device, &descriptor);
        if (glyph_buffer == NULL) {
            fatal("glyph vertex buffer creation failed");
            goto cleanup;
        }
        wgpuQueueWriteBuffer(queue, glyph_buffer, 0, kGlyphVertices, sizeof(kGlyphVertices));
    }
#endif

    /* 5. Shader modules. */
    {
        rect_shader = create_shader_module(device, SCENE_RECT_SHADER, "rect-shader");
        if (rect_shader == NULL) {
            fatal("rect shader module creation failed");
            goto cleanup;
        }
#if SCENE_HAS_TEXT
        glyph_shader = create_shader_module(device, kGlyphShaderWGSL, "glyph-shader");
        if (glyph_shader == NULL) {
            fatal("glyph shader module creation failed");
            goto cleanup;
        }
#endif
    }

    /* 6. Rect pipeline: vec2 pos + unorm8x4 color (12 B stride). */
    {
        WGPUVertexAttribute attributes[2];
        WGPUVertexBufferLayout buffer_layout;
        WGPUColorTargetState target;
        WGPUBlendState blend;
        WGPUFragmentState fragment;
        WGPUVertexState vertex;
        WGPUPrimitiveState primitive;
        WGPUMultisampleState multisample;
        WGPURenderPipelineDescriptor descriptor;

        memset(attributes, 0, sizeof(attributes));
        attributes[0].format = WGPUVertexFormat_Float32x2;
        attributes[0].offset = 0;
        attributes[0].shaderLocation = 0;
        attributes[1].format = WGPUVertexFormat_Unorm8x4;
        attributes[1].offset = 8;
        attributes[1].shaderLocation = 1;

        memset(&buffer_layout, 0, sizeof(buffer_layout));
        buffer_layout.stepMode = WGPUVertexStepMode_Vertex;
        buffer_layout.arrayStride = SCENE_VERTEX_STRIDE;
        buffer_layout.attributeCount = 2;
        buffer_layout.attributes = attributes;

        memset(&blend, 0, sizeof(blend));
        blend.color.operation = WGPUBlendOperation_Add;
        blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
        blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
        blend.alpha.operation = WGPUBlendOperation_Add;
        blend.alpha.srcFactor = WGPUBlendFactor_One;
        blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;

        memset(&target, 0, sizeof(target));
        target.format = surface_config.format;
        target.blend = &blend;
        target.writeMask = WGPUColorWriteMask_All;

        memset(&fragment, 0, sizeof(fragment));
        fragment.module = rect_shader;
        fragment.entryPoint = sv("fs_main");
        fragment.targetCount = 1;
        fragment.targets = &target;

        memset(&vertex, 0, sizeof(vertex));
        vertex.module = rect_shader;
        vertex.entryPoint = sv("vs_main");
        vertex.bufferCount = 1;
        vertex.buffers = &buffer_layout;

        memset(&primitive, 0, sizeof(primitive));
        primitive.topology = WGPUPrimitiveTopology_TriangleList;
        primitive.frontFace = WGPUFrontFace_CCW;
        primitive.cullMode = WGPUCullMode_None;

        memset(&multisample, 0, sizeof(multisample));
        multisample.count = 1;
        multisample.mask = 0xFFFFFFFFu;

        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("rect-pipeline");
        descriptor.layout = NULL;
        descriptor.vertex = vertex;
        descriptor.primitive = primitive;
        descriptor.depthStencil = NULL;
        descriptor.multisample = multisample;
        descriptor.fragment = &fragment;
        rect_pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
        if (rect_pipeline == NULL) {
            fatal("rect pipeline creation failed");
            goto cleanup;
        }
    }

#if SCENE_HAS_TEXT
    /* 7. Glyph atlas + sampler + bind group + textured pipeline. */
    {
        WGPUTextureDescriptor descriptor;
        WGPUSamplerDescriptor sampler_descriptor;

        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("glyph-atlas");
        descriptor.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc |
                           WGPUTextureUsage_CopyDst;
        descriptor.dimension = WGPUTextureDimension_2D;
        descriptor.size.width = ATLAS_WIDTH;
        descriptor.size.height = ATLAS_HEIGHT;
        descriptor.size.depthOrArrayLayers = 1;
        descriptor.format = WGPUTextureFormat_R8Unorm;
        descriptor.mipLevelCount = 1;
        descriptor.sampleCount = 1;
        atlas_texture = wgpuDeviceCreateTexture(device, &descriptor);
        if (atlas_texture == NULL) {
            fatal("atlas texture creation failed");
            goto cleanup;
        }
        atlas_view = wgpuTextureCreateView(atlas_texture, NULL);
        if (atlas_view == NULL) {
            fatal("atlas texture view creation failed");
            goto cleanup;
        }
        if (upload_atlas_pixels(queue, atlas_texture) != 0) {
            fatal("atlas upload failed");
            goto cleanup;
        }

        memset(&sampler_descriptor, 0, sizeof(sampler_descriptor));
        sampler_descriptor.label = sv("atlas-sampler");
        sampler_descriptor.addressModeU = WGPUAddressMode_ClampToEdge;
        sampler_descriptor.addressModeV = WGPUAddressMode_ClampToEdge;
        sampler_descriptor.addressModeW = WGPUAddressMode_ClampToEdge;
        sampler_descriptor.magFilter = WGPUFilterMode_Linear;
        sampler_descriptor.minFilter = WGPUFilterMode_Linear;
        sampler_descriptor.mipmapFilter = WGPUMipmapFilterMode_Nearest;
        sampler_descriptor.lodMinClamp = 0.0f;
        sampler_descriptor.lodMaxClamp = 32.0f;
        sampler_descriptor.maxAnisotropy = 1;
        atlas_sampler = wgpuDeviceCreateSampler(device, &sampler_descriptor);
        if (atlas_sampler == NULL) {
            fatal("atlas sampler creation failed");
            goto cleanup;
        }

        {
            WGPUBindGroupLayoutEntry entries[2];
            WGPUBindGroupLayoutDescriptor layout_descriptor;
            WGPUBindGroupDescriptor group_descriptor;
            WGPUBindGroupEntry group_entries[2];

            memset(entries, 0, sizeof(entries));
            entries[0].binding = 0;
            entries[0].visibility = WGPUShaderStage_Fragment;
            entries[0].texture.sampleType = WGPUTextureSampleType_Float;
            entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
            entries[0].texture.multisampled = 0;
            entries[1].binding = 1;
            entries[1].visibility = WGPUShaderStage_Fragment;
            entries[1].sampler.type = WGPUSamplerBindingType_Filtering;

            memset(&layout_descriptor, 0, sizeof(layout_descriptor));
            layout_descriptor.label = sv("atlas-bind-group-layout");
            layout_descriptor.entryCount = 2;
            layout_descriptor.entries = entries;
            atlas_bind_group_layout = wgpuDeviceCreateBindGroupLayout(device, &layout_descriptor);
            if (atlas_bind_group_layout == NULL) {
                fatal("atlas bind group layout creation failed");
                goto cleanup;
            }

            memset(&group_entries, 0, sizeof(group_entries));
            group_entries[0].binding = 0;
            group_entries[0].textureView = atlas_view;
            group_entries[1].binding = 1;
            group_entries[1].sampler = atlas_sampler;

            memset(&group_descriptor, 0, sizeof(group_descriptor));
            group_descriptor.label = sv("atlas-bind-group");
            group_descriptor.layout = atlas_bind_group_layout;
            group_descriptor.entryCount = 2;
            group_descriptor.entries = group_entries;
            atlas_bind_group = wgpuDeviceCreateBindGroup(device, &group_descriptor);
            if (atlas_bind_group == NULL) {
                fatal("atlas bind group creation failed");
                goto cleanup;
            }
        }

        {
            WGPUVertexAttribute attributes[3];
            WGPUVertexBufferLayout buffer_layout;
            WGPUColorTargetState target;
            WGPUBlendState blend;
            WGPUFragmentState fragment;
            WGPUVertexState vertex;
            WGPUPrimitiveState primitive;
            WGPUMultisampleState multisample;
            WGPURenderPipelineDescriptor pipeline_descriptor;
            WGPUPipelineLayoutDescriptor layout_descriptor;

            memset(attributes, 0, sizeof(attributes));
            attributes[0].format = WGPUVertexFormat_Float32x2;
            attributes[0].offset = 0;
            attributes[0].shaderLocation = 0;
            attributes[1].format = WGPUVertexFormat_Float32x2;
            attributes[1].offset = 8;
            attributes[1].shaderLocation = 1;
            attributes[2].format = WGPUVertexFormat_Unorm8x4;
            attributes[2].offset = 16;
            attributes[2].shaderLocation = 2;

            memset(&buffer_layout, 0, sizeof(buffer_layout));
            buffer_layout.stepMode = WGPUVertexStepMode_Vertex;
            buffer_layout.arrayStride = SCENE_GLYPH_VERTEX_STRIDE;
            buffer_layout.attributeCount = 3;
            buffer_layout.attributes = attributes;

            memset(&layout_descriptor, 0, sizeof(layout_descriptor));
            layout_descriptor.label = sv("glyph-pipeline-layout");
            layout_descriptor.bindGroupLayoutCount = 1;
            layout_descriptor.bindGroupLayouts = &atlas_bind_group_layout;
            glyph_pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &layout_descriptor);
            if (glyph_pipeline_layout == NULL) {
                fatal("glyph pipeline layout creation failed");
                goto cleanup;
            }

            memset(&blend, 0, sizeof(blend));
            blend.color.operation = WGPUBlendOperation_Add;
            blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
            blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
            blend.alpha.operation = WGPUBlendOperation_Add;
            blend.alpha.srcFactor = WGPUBlendFactor_One;
            blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;

            memset(&target, 0, sizeof(target));
            target.format = surface_config.format;
            target.blend = &blend;
            target.writeMask = WGPUColorWriteMask_All;

            memset(&fragment, 0, sizeof(fragment));
            fragment.module = glyph_shader;
            fragment.entryPoint = sv("fs_main");
            fragment.targetCount = 1;
            fragment.targets = &target;

            memset(&vertex, 0, sizeof(vertex));
            vertex.module = glyph_shader;
            vertex.entryPoint = sv("vs_main");
            vertex.bufferCount = 1;
            vertex.buffers = &buffer_layout;

            memset(&primitive, 0, sizeof(primitive));
            primitive.topology = WGPUPrimitiveTopology_TriangleList;
            primitive.frontFace = WGPUFrontFace_CCW;
            primitive.cullMode = WGPUCullMode_None;

            memset(&multisample, 0, sizeof(multisample));
            multisample.count = 1;
            multisample.mask = 0xFFFFFFFFu;

            memset(&pipeline_descriptor, 0, sizeof(pipeline_descriptor));
            pipeline_descriptor.label = sv("glyph-pipeline");
            pipeline_descriptor.layout = glyph_pipeline_layout;
            pipeline_descriptor.vertex = vertex;
            pipeline_descriptor.primitive = primitive;
            pipeline_descriptor.depthStencil = NULL;
            pipeline_descriptor.multisample = multisample;
            pipeline_descriptor.fragment = &fragment;
            glyph_pipeline = wgpuDeviceCreateRenderPipeline(device, &pipeline_descriptor);
            if (glyph_pipeline == NULL) {
                fatal("glyph pipeline creation failed");
                goto cleanup;
            }
        }

        if (verify_atlas_upload(instance, device, queue, atlas_texture) != 0) {
            fatal("atlas upload verification failed");
            goto cleanup;
        }
    }
#endif /* SCENE_HAS_TEXT */

    t_ready = now_ms();

    /* 8. Message loop + one render pass per frame (rects, then glyphs). */
    while (g_running) {
        MSG msg;
        WGPUSurfaceTexture surface_texture;
        WGPUTextureView view;
        WGPUCommandEncoder encoder;
        WGPURenderPassEncoder pass;
        WGPUCommandBuffer commands;
        WGPURenderPassColorAttachment attachment;
        WGPURenderPassDescriptor pass_descriptor;
        WGPUSurfaceGetCurrentTextureStatus texture_status;

        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_running = 0;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running) {
            break;
        }

        /* Surface resize / reconfigure: only the swapchain changes; the atlas,
         * bind group and both pipelines are kept. */
        if (InterlockedExchange(&g_resize_pending, 0) != 0 &&
            g_client_width > 0 && g_client_height > 0) {
            surface_config.width = (uint32_t)g_client_width;
            surface_config.height = (uint32_t)g_client_height;
            wgpuSurfaceConfigure(surface, &surface_config);
            ++resize_count;
#if SCENE_HAS_TEXT
            printf("window: resized to %dx%d (surface reconfigured; glyph pipeline kept)\n",
                   g_client_width, g_client_height);
#else
            printf("window: resized to %dx%d (surface reconfigured; text=off)\n",
                   g_client_width, g_client_height);
#endif
            fflush(stdout);
        }

        memset(&surface_texture, 0, sizeof(surface_texture));
        wgpuSurfaceGetCurrentTexture(surface, &surface_texture);
        texture_status = surface_texture.status;
        if (texture_status == WGPUSurfaceGetCurrentTextureStatus_Outdated ||
            texture_status == WGPUSurfaceGetCurrentTextureStatus_Lost) {
            surface_config.width = (uint32_t)g_client_width;
            surface_config.height = (uint32_t)g_client_height;
            wgpuSurfaceConfigure(surface, &surface_config);
            continue;
        }
        if ((uint32_t)texture_status ==
            (uint32_t)WGPUSurfaceGetCurrentTextureStatus_Occluded) {
            Sleep(1);
            continue;
        }
        if (texture_status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
            texture_status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
            if (texture_status == WGPUSurfaceGetCurrentTextureStatus_Timeout) {
                Sleep(1);
                continue;
            }
            break;
        }

        view = wgpuTextureCreateView(surface_texture.texture, NULL);
        if (view == NULL) {
            wgpuTextureRelease(surface_texture.texture);
            break;
        }
        encoder = wgpuDeviceCreateCommandEncoder(device, NULL);
        if (encoder == NULL) {
            wgpuTextureViewRelease(view);
            wgpuTextureRelease(surface_texture.texture);
            break;
        }

        memset(&attachment, 0, sizeof(attachment));
        attachment.view = view;
        attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
        attachment.loadOp = WGPULoadOp_Clear;
        attachment.storeOp = WGPUStoreOp_Store;
        attachment.clearValue.r = SCENE_CLEAR_R;
        attachment.clearValue.g = SCENE_CLEAR_G;
        attachment.clearValue.b = SCENE_CLEAR_B;
        attachment.clearValue.a = SCENE_CLEAR_A;

        memset(&pass_descriptor, 0, sizeof(pass_descriptor));
        pass_descriptor.label = sv("scene-pass");
        pass_descriptor.colorAttachmentCount = 1;
        pass_descriptor.colorAttachments = &attachment;

        pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_descriptor);
        if (pass == NULL) {
            wgpuCommandEncoderRelease(encoder);
            wgpuTextureViewRelease(view);
            wgpuTextureRelease(surface_texture.texture);
            break;
        }
        wgpuRenderPassEncoderSetPipeline(pass, rect_pipeline);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 0, rect_buffer, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderDraw(pass, SCENE_VERTEX_COUNT, 1, 0, 0);
#if SCENE_HAS_TEXT
        wgpuRenderPassEncoderSetPipeline(pass, glyph_pipeline);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, atlas_bind_group, 0, NULL);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 0, glyph_buffer, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderDraw(pass, SCENE_GLYPH_VERTEX_COUNT, 1, 0, 0);
#endif
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);

        commands = wgpuCommandEncoderFinish(encoder, NULL);
        if (commands == NULL) {
            wgpuCommandEncoderRelease(encoder);
            wgpuTextureViewRelease(view);
            wgpuTextureRelease(surface_texture.texture);
            break;
        }
        wgpuQueueSubmit(queue, 1, &commands);
        wgpuCommandBufferRelease(commands);
        wgpuCommandEncoderRelease(encoder);

        (void)wgpuSurfacePresent(surface);
        wgpuTextureViewRelease(view);
        wgpuTextureRelease(surface_texture.texture);

#if SCENE_HAS_TEXT
        ++glyph_frames;
#endif
        ++frames;

        if (g_resize_after > 0 && !resize_requested && frames >= g_resize_after) {
            RECT rect = {0, 0, 720, 480};
            AdjustWindowRect(&rect, window_style, FALSE);
            SetWindowPos(hwnd, NULL, 0, 0,
                         rect.right - rect.left, rect.bottom - rect.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            resize_requested = 1;
            printf("window: requested resize to 720x480 client after frame %d\n", frames);
            fflush(stdout);
        }

        if (g_max_frames > 0 && frames >= g_max_frames) {
            break;
        }
    }

#if SCENE_HAS_TEXT
    printf("window: closed after %d frame(s), %d resize(s), %d glyph frame(s), "
           "init %.2f ms, %.2f ms/frame average\n",
           frames, resize_count, glyph_frames, t_ready - t_start,
           frames > 0 ? (now_ms() - t_ready) / (double)frames : 0.0);
#else
    printf("window: closed after %d frame(s), %d resize(s), text=off, "
           "init %.2f ms, %.2f ms/frame average\n",
           frames, resize_count, t_ready - t_start,
           frames > 0 ? (now_ms() - t_ready) / (double)frames : 0.0);
#endif
    fflush(stdout);
    exit_code = 0;

cleanup:
    if (atlas_bind_group != NULL) wgpuBindGroupRelease(atlas_bind_group);
    if (glyph_pipeline_layout != NULL) wgpuPipelineLayoutRelease(glyph_pipeline_layout);
    if (atlas_bind_group_layout != NULL) wgpuBindGroupLayoutRelease(atlas_bind_group_layout);
    if (atlas_sampler != NULL) wgpuSamplerRelease(atlas_sampler);
    if (glyph_pipeline != NULL) wgpuRenderPipelineRelease(glyph_pipeline);
    if (rect_pipeline != NULL) wgpuRenderPipelineRelease(rect_pipeline);
    if (glyph_shader != NULL) wgpuShaderModuleRelease(glyph_shader);
    if (rect_shader != NULL) wgpuShaderModuleRelease(rect_shader);
    if (atlas_view != NULL) wgpuTextureViewRelease(atlas_view);
    if (atlas_texture != NULL) wgpuTextureRelease(atlas_texture);
    if (glyph_buffer != NULL) wgpuBufferRelease(glyph_buffer);
    if (rect_buffer != NULL) wgpuBufferRelease(rect_buffer);
    if (surface != NULL) wgpuSurfaceRelease(surface);
    if (queue != NULL) wgpuQueueRelease(queue);
    if (device != NULL) wgpuDeviceRelease(device);
    if (adapter != NULL) wgpuAdapterRelease(adapter);
    if (instance != NULL) wgpuInstanceRelease(instance);
    if (hwnd != NULL) DestroyWindow(hwnd);
    return exit_code;
}
