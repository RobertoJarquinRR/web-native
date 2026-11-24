/*
 * runtime_gpu_text.c - offscreen native GPU renderer: rects + text in C.
 *
 * Build-time:  HTML/CSS -> display_list.json + atlas.bin
 *              -> emit_c_text.py -> scene_data.h (rect verts, glyph verts,
 *              raw R8 atlas, both WGSL shaders).
 *
 * Runtime:     NO JSON parser, NO Python, NO FreeType/HarfBuzz, NO browser and
 *              NO font file. The glyph quads and the atlas pixels are baked
 *              C arrays in scene_data.h. This file only:
 *
 *                1. uploads the baked rect vertices (12 B/vertex)
 *                2. uploads the baked glyph vertices (20 B/vertex)
 *                3. uploads the R8Unorm atlas + linear clamp sampler and
 *                   creates the bind group (texture + sampler),
 *                4. draws rects (solid pipeline) then glyph quads (textured
 *                   pipeline, alpha blend) in ONE render pass,
 *                5. copies the target to a buffer, writes a PNG through WIC,
 *                6. reads the atlas back from the GPU and compares its FNV-1a
 *                   hash with the baked array (proof the texture was uploaded).
 *
 * A header with SCENE_GLYPH_QUAD_COUNT == 0 (rect-only) still works: the text
 * stage is compiled out and the runtime is a plain rect renderer.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <wincodec.h>
#include <wgpu.h>

#include "scene_data.h"

#define ARRAY_LEN(a) ((size_t)(sizeof(a) / sizeof((a)[0])))

/* Decide the header flavor BEFORE defining the zero fallbacks below:
 *   emit_c_text.py / exp16 -> kRectShaderWGSL + glyph macros
 *   emit_c.py (rect-only)  -> kSceneShaderWGSL, no glyph macros
 * The macro expands at the use site, after the header's array declarations. */
#ifdef SCENE_GLYPH_QUAD_COUNT
#define SCENE_RECT_SHADER kRectShaderWGSL
#else
#define SCENE_RECT_SHADER kSceneShaderWGSL
#endif

#ifndef SCENE_RECT_COUNT
#define SCENE_RECT_COUNT SCENE_OP_COUNT
#endif
#ifndef SCENE_TEXT_RUN_COUNT
#define SCENE_TEXT_RUN_COUNT 0
#endif
#ifndef SCENE_GLYPH_QUAD_COUNT
#define SCENE_GLYPH_QUAD_COUNT 0
#endif
#ifndef SCENE_GLYPH_VERTEX_COUNT
#define SCENE_GLYPH_VERTEX_COUNT 0
#endif
#ifndef ATLAS_WIDTH
#define ATLAS_WIDTH 0
#endif
#ifndef ATLAS_HEIGHT
#define ATLAS_HEIGHT 0
#endif
#ifndef ATLAS_PIXELS
#define ATLAS_PIXELS 0
#endif
#ifndef ATLAS_GLYPH_COUNT
#define ATLAS_GLYPH_COUNT 0
#endif

#if SCENE_GLYPH_QUAD_COUNT > 0
#define SCENE_HAS_TEXT 1
#else
#define SCENE_HAS_TEXT 0
#endif

/* The emitter and the pipeline layouts must agree on the packed strides. */
_Static_assert(sizeof(SceneVertex) == SCENE_VERTEX_STRIDE,
               "SceneVertex must stay 12 bytes (vec2 + unorm8x4)");
#if SCENE_HAS_TEXT
_Static_assert(sizeof(GlyphVertex) == SCENE_GLYPH_VERTEX_STRIDE,
               "GlyphVertex must stay 20 bytes (vec2 + vec2 + unorm8x4)");
_Static_assert(sizeof(kAtlasPixels) == ATLAS_PIXELS,
               "kAtlasPixels must be ATLAS_WIDTH * ATLAS_HEIGHT bytes");
#endif

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

static void on_log(WGPULogLevel level, WGPUStringView message, void *userdata)
{
    (void)userdata;
    fprintf(stderr, "[wgpu %d] ", (int)level);
    fprint_view(stderr, message);
    fputc('\n', stderr);
    fflush(stderr);
}

static void print_view(const char *label, WGPUStringView view)
{
    if (view.data == NULL || view.length == 0) {
        printf("%s(none)", label);
    } else if (view.length == WGPU_STRLEN) {
        printf("%s%s", label, view.data);
    } else {
        int n = view.length > 4096 ? 4096 : (int)view.length;
        printf("%s%.*s", label, n, view.data);
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

static double now_ms(void)
{
    LARGE_INTEGER freq, tick;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&tick);
    return (double)tick.QuadPart * 1000.0 / (double)freq.QuadPart;
}

#if SCENE_HAS_TEXT
/* FNV-1a 32-bit over packed rows: proves the GPU atlas equals the baked array.
 * row_pitch lets the same hash cover a tight CPU array (pitch == row_bytes) and
 * a 256-byte-aligned GPU readback (pitch > row_bytes). */
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

/* wgpuQueueWriteTexture wants bytesPerRow % 256 == 0 when height > 1. The
 * emitter keeps the atlas tight (width bytes per row), so pad into a staging
 * buffer only when the width is not already aligned. */
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
            fprintf(stderr, "error: out of memory staging the atlas rows\n");
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
#endif /* SCENE_HAS_TEXT */

/* Waits for an async result flag set by a callback.
 *
 * wgpu-native v29.0.1.1 does NOT implement wgpuInstanceWaitAny (it panics
 * with `unimplemented`), so the supported path is WGPUCallbackMode_AllowProcessEvents
 * plus wgpuInstanceProcessEvents. */
static int wait_flag(WGPUInstance instance, volatile int *flag, const char *what)
{
    DWORD start = GetTickCount();
    while (!*flag) {
        wgpuInstanceProcessEvents(instance);
        if (GetTickCount() - start > 30000u) {
            fprintf(stderr, "error: %s: timed out after 30s\n", what);
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
    char message[512];
} AdapterResult;

typedef struct {
    WGPUDevice device;
    WGPURequestDeviceStatus status;
    volatile int done;
    char message[512];
} DeviceResult;

typedef struct {
    WGPUMapAsyncStatus status;
    volatile int done;
    char message[512];
} MapResult;

static void copy_message(char *dst, size_t cap, WGPUStringView message)
{
    if (message.data == NULL || message.length == 0) {
        dst[0] = '\0';
        return;
    }
    if (message.length == WGPU_STRLEN) {
        snprintf(dst, cap, "%s", message.data);
    } else {
        size_t n = message.length < cap - 1 ? message.length : cap - 1;
        memcpy(dst, message.data, n);
        dst[n] = '\0';
    }
}

static void on_adapter(WGPURequestAdapterStatus status, WGPUAdapter adapter,
                       WGPUStringView message, void *userdata1, void *userdata2)
{
    AdapterResult *result = (AdapterResult *)userdata1;
    (void)userdata2;
    result->status = status;
    result->adapter = adapter;
    copy_message(result->message, sizeof(result->message), message);
    result->done = 1;
}

static void on_device(WGPURequestDeviceStatus status, WGPUDevice device,
                      WGPUStringView message, void *userdata1, void *userdata2)
{
    DeviceResult *result = (DeviceResult *)userdata1;
    (void)userdata2;
    result->status = status;
    result->device = device;
    copy_message(result->message, sizeof(result->message), message);
    result->done = 1;
}

static void on_map(WGPUMapAsyncStatus status, WGPUStringView message,
                   void *userdata1, void *userdata2)
{
    MapResult *result = (MapResult *)userdata1;
    (void)userdata2;
    result->status = status;
    copy_message(result->message, sizeof(result->message), message);
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
}

/* ------------------------------------------------------------- WIC -> PNG */

static int report_hr(HRESULT hr, const char *what)
{
    fprintf(stderr, "error: %s failed (hr=0x%08lX)\n", what, (unsigned long)hr);
    return 1;
}

static int write_png_rgba(const unsigned char *rgba, unsigned int width,
                          unsigned int height, unsigned int src_stride,
                          const wchar_t *path)
{
    HRESULT hr;
    IWICImagingFactory *factory = NULL;
    IWICBitmap *bitmap = NULL;
    IWICBitmapLock *lock = NULL;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *options = NULL;
    WICRect area;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
    UINT dst_stride = 0, dst_bytes = 0;
    BYTE *dst = NULL;
    unsigned int y;
    int exit_code = 1;

    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWICImagingFactory, (void **)&factory);
    if (FAILED(hr))
        return report_hr(hr, "CoCreateInstance(WICImagingFactory)");

    hr = factory->lpVtbl->CreateBitmap(factory, width, height, &GUID_WICPixelFormat32bppRGBA,
                                       WICBitmapCacheOnLoad, &bitmap);
    if (FAILED(hr)) {
        report_hr(hr, "IWICImagingFactory::CreateBitmap");
        goto cleanup;
    }

    area.X = 0;
    area.Y = 0;
    area.Width = (UINT)width;
    area.Height = (UINT)height;
    hr = bitmap->lpVtbl->Lock(bitmap, &area, WICBitmapLockWrite, &lock);
    if (FAILED(hr)) {
        report_hr(hr, "IWICBitmap::Lock");
        goto cleanup;
    }
    hr = lock->lpVtbl->GetStride(lock, &dst_stride);
    if (FAILED(hr) || dst_stride < width * 4) {
        report_hr(FAILED(hr) ? hr : E_FAIL, "IWICBitmapLock::GetStride");
        goto cleanup;
    }
    hr = lock->lpVtbl->GetDataPointer(lock, &dst_bytes, &dst);
    if (FAILED(hr) || dst == NULL || dst_bytes < dst_stride * height) {
        report_hr(FAILED(hr) ? hr : E_FAIL, "IWICBitmapLock::GetDataPointer");
        goto cleanup;
    }
    for (y = 0; y < height; ++y) {
        memcpy(dst + (size_t)y * dst_stride, rgba + (size_t)y * src_stride, width * 4);
    }
    lock->lpVtbl->Release(lock);
    lock = NULL;

    hr = factory->lpVtbl->CreateStream(factory, &stream);
    if (FAILED(hr)) {
        report_hr(hr, "CreateStream");
        goto cleanup;
    }
    hr = stream->lpVtbl->InitializeFromFilename(stream, path, GENERIC_WRITE);
    if (FAILED(hr)) {
        report_hr(hr, "IWICStream::InitializeFromFilename");
        goto cleanup;
    }
    hr = factory->lpVtbl->CreateEncoder(factory, &GUID_ContainerFormatPng, NULL, &encoder);
    if (FAILED(hr)) {
        report_hr(hr, "CreateEncoder(png)");
        goto cleanup;
    }
    hr = encoder->lpVtbl->Initialize(encoder, (IStream *)stream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        report_hr(hr, "IWICBitmapEncoder::Initialize");
        goto cleanup;
    }
    hr = encoder->lpVtbl->CreateNewFrame(encoder, &frame, &options);
    if (FAILED(hr)) {
        report_hr(hr, "CreateNewFrame");
        goto cleanup;
    }
    if (options != NULL) {
        options->lpVtbl->Release(options);
        options = NULL;
    }
    hr = frame->lpVtbl->Initialize(frame, NULL);
    if (FAILED(hr)) {
        report_hr(hr, "IWICBitmapFrameEncode::Initialize");
        goto cleanup;
    }
    hr = frame->lpVtbl->SetSize(frame, width, height);
    if (FAILED(hr)) {
        report_hr(hr, "IWICBitmapFrameEncode::SetSize");
        goto cleanup;
    }
    hr = frame->lpVtbl->SetPixelFormat(frame, &format);
    if (FAILED(hr)) {
        report_hr(hr, "IWICBitmapFrameEncode::SetPixelFormat");
        goto cleanup;
    }
    hr = frame->lpVtbl->WriteSource(frame, (IWICBitmapSource *)bitmap, NULL);
    if (FAILED(hr)) {
        report_hr(hr, "IWICBitmapFrameEncode::WriteSource");
        goto cleanup;
    }
    hr = frame->lpVtbl->Commit(frame);
    if (FAILED(hr)) {
        report_hr(hr, "IWICBitmapFrameEncode::Commit");
        goto cleanup;
    }
    hr = encoder->lpVtbl->Commit(encoder);
    if (FAILED(hr)) {
        report_hr(hr, "IWICBitmapEncoder::Commit");
        goto cleanup;
    }
    exit_code = 0;

cleanup:
    if (options != NULL) options->lpVtbl->Release(options);
    if (frame != NULL) frame->lpVtbl->Release(frame);
    if (encoder != NULL) encoder->lpVtbl->Release(encoder);
    if (stream != NULL) stream->lpVtbl->Release(stream);
    if (lock != NULL) lock->lpVtbl->Release(lock);
    if (bitmap != NULL) bitmap->lpVtbl->Release(bitmap);
    if (factory != NULL) factory->lpVtbl->Release(factory);
    return exit_code;
}

/* ------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    const wchar_t *out_path = L"scene_text.png";
    wchar_t wide_path[MAX_PATH];
    int exit_code = 1;

    WGPUInstance instance = NULL;
    WGPUAdapter adapter = NULL;
    WGPUDevice device = NULL;
    WGPUQueue queue = NULL;
    WGPUCommandEncoder encoder = NULL;
    WGPUCommandBuffer commands = NULL;
    WGPURenderPassEncoder pass = NULL;
    WGPURenderPipeline rect_pipeline = NULL;
    WGPURenderPipeline glyph_pipeline = NULL;
    WGPUShaderModule rect_shader = NULL;
    WGPUShaderModule glyph_shader = NULL;
    WGPUCommandEncoder atlas_encoder = NULL;
    WGPUCommandBuffer atlas_commands = NULL;
    WGPUBuffer atlas_readback = NULL;
    WGPUTexture target = NULL;
    WGPUTextureView target_view = NULL;
    WGPUBuffer rect_buffer = NULL;
    WGPUBuffer glyph_buffer = NULL;
    WGPUBuffer readback = NULL;
    WGPUTexture atlas_texture = NULL;
    WGPUTextureView atlas_view = NULL;
    WGPUSampler sampler = NULL;
    WGPUBindGroupLayout bind_group_layout = NULL;
    WGPUPipelineLayout glyph_pipeline_layout = NULL;
    WGPUBindGroup bind_group = NULL;

    AdapterResult adapter_result;
    DeviceResult device_result;
    MapResult map_result;
#if SCENE_HAS_TEXT
    MapResult atlas_map_result;
#endif

    WGPUAdapterInfo info;
    WGPUStatus status;
    unsigned int bytes_per_row;
    uint64_t readback_size;
#if SCENE_HAS_TEXT
    unsigned int atlas_hash_cpu;
    unsigned int atlas_hash_gpu = 0;
#endif
    const unsigned char *mapped = NULL;
    double t_start, t_init, t_upload, t_atlas, t_pipeline, t_submit, t_read, t_png;
    int glyphs_drawn = 0;
    int i;

    if (argc > 1) {
        if (MultiByteToWideChar(CP_ACP, 0, argv[1], -1, wide_path, MAX_PATH) == 0) {
            fprintf(stderr, "error: output path too long\n");
            return 1;
        }
        out_path = wide_path;
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    wgpuSetLogLevel(WGPULogLevel_Warn);
    wgpuSetLogCallback(on_log, NULL);

    printf("scene_text: AOT GPU scene %dx%d | %d rect op(s) + %d text run(s) "
           "| atlas %dx%d R8Unorm, %d glyphs\n",
           SCENE_WIDTH, SCENE_HEIGHT, SCENE_RECT_COUNT, SCENE_TEXT_RUN_COUNT,
           ATLAS_WIDTH, ATLAS_HEIGHT, ATLAS_GLYPH_COUNT);
    for (i = 0; i < SCENE_RECT_COUNT; ++i) {
        const RectOp *op = &kSceneOps[i];
        printf("  rect[%d] x=%.1f y=%.1f w=%.1f h=%.1f rgba=0x%08X\n",
               i, op->x, op->y, op->w, op->h, op->rgba);
    }
#if SCENE_HAS_TEXT
    for (i = 0; i < SCENE_GLYPH_QUAD_COUNT; ++i) {
        const GlyphQuad *quad = &kGlyphQuads[i];
        printf("  glyph[%d] run=%d gid=%d x=%.3f y=%.3f %dx%d uv=(%.6f,%.6f)-(%.6f,%.6f)\n",
               i, quad->run_id, quad->gid, quad->x, quad->y, (int)quad->w, (int)quad->h,
               quad->u0, quad->v0, quad->u1, quad->v1);
    }
#endif
    printf("scene_text: clear color rgb(%d,%d,%d)\n",
           (int)(SCENE_CLEAR_R * 255.0f + 0.5f),
           (int)(SCENE_CLEAR_G * 255.0f + 0.5f),
           (int)(SCENE_CLEAR_B * 255.0f + 0.5f));

    if (SCENE_VERTEX_COUNT != SCENE_RECT_COUNT * 6) {
        fprintf(stderr, "error: vertex/op mismatch in generated header\n");
        return 1;
    }
    if (SCENE_GLYPH_VERTEX_COUNT != SCENE_GLYPH_QUAD_COUNT * 6) {
        fprintf(stderr, "error: glyph vertex/quad mismatch in generated header\n");
        return 1;
    }

#if SCENE_HAS_TEXT
    atlas_hash_cpu = fnv1a_rows(kAtlasPixels, ATLAS_WIDTH, ATLAS_WIDTH, ATLAS_HEIGHT);
#endif
    t_start = now_ms();

    instance = wgpuCreateInstance(NULL);
    if (instance == NULL) {
        fprintf(stderr, "error: wgpuCreateInstance returned NULL\n");
        goto cleanup;
    }

    memset(&adapter_result, 0, sizeof(adapter_result));
    {
        WGPURequestAdapterCallbackInfo cb;
        WGPURequestAdapterOptions options;
        memset(&cb, 0, sizeof(cb));
        memset(&options, 0, sizeof(options));
        options.powerPreference = WGPUPowerPreference_HighPerformance;
        options.featureLevel = WGPUFeatureLevel_Core;
        cb.mode = WGPUCallbackMode_AllowProcessEvents;
        cb.callback = on_adapter;
        cb.userdata1 = &adapter_result;
        (void)wgpuInstanceRequestAdapter(instance, &options, cb);
        if (wait_flag(instance, &adapter_result.done, "request adapter") != 0)
            goto cleanup;
    }
    if (adapter_result.status != WGPURequestAdapterStatus_Success || adapter_result.adapter == NULL) {
        fprintf(stderr, "error: no adapter (status %d): %s\n",
                (int)adapter_result.status, adapter_result.message);
        goto cleanup;
    }
    adapter = adapter_result.adapter;

    memset(&info, 0, sizeof(info));
    status = wgpuAdapterGetInfo(adapter, &info);
    if (status != WGPUStatus_Success) {
        fprintf(stderr, "error: wgpuAdapterGetInfo failed\n");
        goto cleanup;
    }
    printf("adapter: backend=%s (%d) type=%s vendorID=0x%04X deviceID=0x%04X\n",
           backend_name(info.backendType), (int)info.backendType,
           adapter_type_name(info.adapterType), info.vendorID, info.deviceID);
    print_view("adapter: device=", info.device);
    printf("\n");

    memset(&device_result, 0, sizeof(device_result));
    {
        WGPUDeviceDescriptor descriptor;
        WGPURequestDeviceCallbackInfo cb;
        memset(&descriptor, 0, sizeof(descriptor));
        memset(&cb, 0, sizeof(cb));
        descriptor.label = sv("webnative-exp16-device");
        descriptor.requiredFeatureCount = 0;
        descriptor.uncapturedErrorCallbackInfo.callback = on_uncaptured_error;
        cb.mode = WGPUCallbackMode_AllowProcessEvents;
        cb.callback = on_device;
        cb.userdata1 = &device_result;
        (void)wgpuAdapterRequestDevice(adapter, &descriptor, cb);
        if (wait_flag(instance, &device_result.done, "request device") != 0)
            goto cleanup;
    }
    if (device_result.status != WGPURequestDeviceStatus_Success || device_result.device == NULL) {
        fprintf(stderr, "error: no device (status %d): %s\n",
                (int)device_result.status, device_result.message);
        goto cleanup;
    }
    device = device_result.device;
    queue = wgpuDeviceGetQueue(device);

    /* Rect vertices: baked clip-space geometry, 12 bytes/vertex. */
    {
        WGPUBufferDescriptor descriptor;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("scene-vertices");
        descriptor.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        descriptor.size = sizeof(kSceneVertices);
        rect_buffer = wgpuDeviceCreateBuffer(device, &descriptor);
        if (rect_buffer == NULL) {
            fprintf(stderr, "error: wgpuDeviceCreateBuffer(rect vertices) failed\n");
            goto cleanup;
        }
        wgpuQueueWriteBuffer(queue, rect_buffer, 0, kSceneVertices, sizeof(kSceneVertices));
    }

#if SCENE_HAS_TEXT
    /* Glyph vertices: pos + uv + color, 20 bytes/vertex. */
    {
        WGPUBufferDescriptor descriptor;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("glyph-vertices");
        descriptor.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        descriptor.size = sizeof(kGlyphVertices);
        glyph_buffer = wgpuDeviceCreateBuffer(device, &descriptor);
        if (glyph_buffer == NULL) {
            fprintf(stderr, "error: wgpuDeviceCreateBuffer(glyph vertices) failed\n");
            goto cleanup;
        }
        wgpuQueueWriteBuffer(queue, glyph_buffer, 0, kGlyphVertices, sizeof(kGlyphVertices));
    }
#endif

    t_init = now_ms();

    /* Offscreen color target. */
    {
        WGPUTextureDescriptor descriptor;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("scene-target");
        descriptor.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
        descriptor.dimension = WGPUTextureDimension_2D;
        descriptor.size.width = SCENE_WIDTH;
        descriptor.size.height = SCENE_HEIGHT;
        descriptor.size.depthOrArrayLayers = 1;
        descriptor.format = WGPUTextureFormat_RGBA8Unorm;
        descriptor.mipLevelCount = 1;
        descriptor.sampleCount = 1;
        target = wgpuDeviceCreateTexture(device, &descriptor);
        if (target == NULL) {
            fprintf(stderr, "error: wgpuDeviceCreateTexture(target) failed\n");
            goto cleanup;
        }
        target_view = wgpuTextureCreateView(target, NULL);
        if (target_view == NULL) {
            fprintf(stderr, "error: wgpuTextureCreateView(target) failed\n");
            goto cleanup;
        }
    }

#if SCENE_HAS_TEXT
    /* Glyph atlas: ATLAS_WIDTH x ATLAS_HEIGHT R8Unorm baked into scene_data.h. */
    {
        WGPUTextureDescriptor descriptor;
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
            fprintf(stderr, "error: wgpuDeviceCreateTexture(atlas) failed\n");
            goto cleanup;
        }
        atlas_view = wgpuTextureCreateView(atlas_texture, NULL);
        if (atlas_view == NULL) {
            fprintf(stderr, "error: wgpuTextureCreateView(atlas) failed\n");
            goto cleanup;
        }
        if (upload_atlas_pixels(queue, atlas_texture) != 0)
            goto cleanup;

        {
            WGPUSamplerDescriptor sampler_descriptor;
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
            sampler = wgpuDeviceCreateSampler(device, &sampler_descriptor);
            if (sampler == NULL) {
                fprintf(stderr, "error: wgpuDeviceCreateSampler failed\n");
                goto cleanup;
            }
        }
    }
#endif

    t_upload = now_ms();

    /* Shader modules: solid rect + textured glyph. */
    {
        WGPUShaderModuleDescriptor descriptor;
        WGPUShaderSourceWGSL wgsl;
        memset(&descriptor, 0, sizeof(descriptor));
        memset(&wgsl, 0, sizeof(wgsl));
        wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
        wgsl.code = sv(SCENE_RECT_SHADER);
        descriptor.label = sv("rect-shader");
        descriptor.nextInChain = &wgsl.chain;
        rect_shader = wgpuDeviceCreateShaderModule(device, &descriptor);
        if (rect_shader == NULL) {
            fprintf(stderr, "error: wgpuDeviceCreateShaderModule(rect) failed\n");
            goto cleanup;
        }
#if SCENE_HAS_TEXT
        memset(&descriptor, 0, sizeof(descriptor));
        memset(&wgsl, 0, sizeof(wgsl));
        wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
        wgsl.code = sv(kGlyphShaderWGSL);
        descriptor.label = sv("glyph-shader");
        descriptor.nextInChain = &wgsl.chain;
        glyph_shader = wgpuDeviceCreateShaderModule(device, &descriptor);
        if (glyph_shader == NULL) {
            fprintf(stderr, "error: wgpuDeviceCreateShaderModule(glyph) failed\n");
            goto cleanup;
        }
#endif
    }

    /* Rect pipeline: interleaved vec2 pos + unorm8x4 color (12 B stride). */
    {
        WGPUVertexAttribute attributes[2];
        WGPUVertexBufferLayout buffer_layout;
        WGPUColorTargetState target_state;
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
        buffer_layout.attributeCount = ARRAY_LEN(attributes);
        buffer_layout.attributes = attributes;

        /* Same blend exp10 used for rects; alpha=255 makes it a no-op. */
        memset(&blend, 0, sizeof(blend));
        blend.color.operation = WGPUBlendOperation_Add;
        blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
        blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
        blend.alpha.operation = WGPUBlendOperation_Add;
        blend.alpha.srcFactor = WGPUBlendFactor_One;
        blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;

        memset(&target_state, 0, sizeof(target_state));
        target_state.format = WGPUTextureFormat_RGBA8Unorm;
        target_state.blend = &blend;
        target_state.writeMask = WGPUColorWriteMask_All;

        memset(&fragment, 0, sizeof(fragment));
        fragment.module = rect_shader;
        fragment.entryPoint = sv("fs_main");
        fragment.targetCount = 1;
        fragment.targets = &target_state;

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
        multisample.alphaToCoverageEnabled = 0;

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
            fprintf(stderr, "error: wgpuDeviceCreateRenderPipeline(rect) failed\n");
            goto cleanup;
        }
    }

#if SCENE_HAS_TEXT
    /* Bind group layout: texture (0) + filtering sampler (1), fragment only. */
    {
        WGPUBindGroupLayoutEntry entries[2];
        WGPUBindGroupLayoutDescriptor descriptor;
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

        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("atlas-bind-group-layout");
        descriptor.entryCount = ARRAY_LEN(entries);
        descriptor.entries = entries;
        bind_group_layout = wgpuDeviceCreateBindGroupLayout(device, &descriptor);
        if (bind_group_layout == NULL) {
            fprintf(stderr, "error: wgpuDeviceCreateBindGroupLayout failed\n");
            goto cleanup;
        }

        memset(&group_entries, 0, sizeof(group_entries));
        group_entries[0].binding = 0;
        group_entries[0].textureView = atlas_view;
        group_entries[1].binding = 1;
        group_entries[1].sampler = sampler;

        memset(&group_descriptor, 0, sizeof(group_descriptor));
        group_descriptor.label = sv("atlas-bind-group");
        group_descriptor.layout = bind_group_layout;
        group_descriptor.entryCount = ARRAY_LEN(group_entries);
        group_descriptor.entries = group_entries;
        bind_group = wgpuDeviceCreateBindGroup(device, &group_descriptor);
        if (bind_group == NULL) {
            fprintf(stderr, "error: wgpuDeviceCreateBindGroup failed\n");
            goto cleanup;
        }
    }

    /* Glyph pipeline: pos + uv + color (20 B stride), alpha blended. */
    {
        WGPUVertexAttribute attributes[3];
        WGPUVertexBufferLayout buffer_layout;
        WGPUColorTargetState target_state;
        WGPUBlendState blend;
        WGPUFragmentState fragment;
        WGPUVertexState vertex;
        WGPUPrimitiveState primitive;
        WGPUMultisampleState multisample;
        WGPURenderPipelineDescriptor descriptor;
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
        buffer_layout.attributeCount = ARRAY_LEN(attributes);
        buffer_layout.attributes = attributes;

        memset(&layout_descriptor, 0, sizeof(layout_descriptor));
        layout_descriptor.label = sv("glyph-pipeline-layout");
        layout_descriptor.bindGroupLayoutCount = 1;
        layout_descriptor.bindGroupLayouts = &bind_group_layout;
        glyph_pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &layout_descriptor);
        if (glyph_pipeline_layout == NULL) {
            fprintf(stderr, "error: wgpuDeviceCreatePipelineLayout failed\n");
            goto cleanup;
        }

        /* Standard alpha blending: src.rgb*a + dst*(1-a). */
        memset(&blend, 0, sizeof(blend));
        blend.color.operation = WGPUBlendOperation_Add;
        blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
        blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
        blend.alpha.operation = WGPUBlendOperation_Add;
        blend.alpha.srcFactor = WGPUBlendFactor_One;
        blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;

        memset(&target_state, 0, sizeof(target_state));
        target_state.format = WGPUTextureFormat_RGBA8Unorm;
        target_state.blend = &blend;
        target_state.writeMask = WGPUColorWriteMask_All;

        memset(&fragment, 0, sizeof(fragment));
        fragment.module = glyph_shader;
        fragment.entryPoint = sv("fs_main");
        fragment.targetCount = 1;
        fragment.targets = &target_state;

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
        multisample.alphaToCoverageEnabled = 0;

        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("glyph-pipeline");
        descriptor.layout = glyph_pipeline_layout;
        descriptor.vertex = vertex;
        descriptor.primitive = primitive;
        descriptor.depthStencil = NULL;
        descriptor.multisample = multisample;
        descriptor.fragment = &fragment;
        glyph_pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
        if (glyph_pipeline == NULL) {
            fprintf(stderr, "error: wgpuDeviceCreateRenderPipeline(glyph) failed\n");
            goto cleanup;
        }
    }
#endif /* SCENE_HAS_TEXT */

    t_atlas = now_ms();

#if SCENE_HAS_TEXT
    /* Prove the atlas really reached the GPU: copy it back and hash it. */
    {
        unsigned int row_pitch = ATLAS_WIDTH;
        uint64_t atlas_size;
        WGPUBufferDescriptor buffer_descriptor;
        WGPUTexelCopyTextureInfo source;
        WGPUTexelCopyBufferInfo destination;
        WGPUExtent3D extent;

        if (ATLAS_HEIGHT > 1 && (row_pitch % 256u) != 0)
            row_pitch = (row_pitch + 255u) & ~255u;
        atlas_size = (uint64_t)row_pitch * ATLAS_HEIGHT;

        memset(&buffer_descriptor, 0, sizeof(buffer_descriptor));
        buffer_descriptor.label = sv("atlas-readback");
        buffer_descriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
        buffer_descriptor.size = atlas_size;
        atlas_readback = wgpuDeviceCreateBuffer(device, &buffer_descriptor);
        if (atlas_readback == NULL) {
            fprintf(stderr, "error: wgpuDeviceCreateBuffer(atlas readback) failed\n");
            goto cleanup;
        }
        memset(&source, 0, sizeof(source));
        source.texture = atlas_texture;
        source.mipLevel = 0;
        source.aspect = WGPUTextureAspect_All;
        memset(&destination, 0, sizeof(destination));
        destination.buffer = atlas_readback;
        destination.layout.offset = 0;
        destination.layout.bytesPerRow = row_pitch;
        destination.layout.rowsPerImage = ATLAS_HEIGHT;
        memset(&extent, 0, sizeof(extent));
        extent.width = ATLAS_WIDTH;
        extent.height = ATLAS_HEIGHT;
        extent.depthOrArrayLayers = 1;

        atlas_encoder = wgpuDeviceCreateCommandEncoder(device, NULL);
        if (atlas_encoder == NULL) {
            fprintf(stderr, "error: wgpuDeviceCreateCommandEncoder(atlas) failed\n");
            goto cleanup;
        }
        wgpuCommandEncoderCopyTextureToBuffer(atlas_encoder, &source, &destination, &extent);
        atlas_commands = wgpuCommandEncoderFinish(atlas_encoder, NULL);
        if (atlas_commands == NULL) {
            fprintf(stderr, "error: wgpuCommandEncoderFinish(atlas) failed\n");
            goto cleanup;
        }
        wgpuQueueSubmit(queue, 1, &atlas_commands);
        wgpuCommandEncoderRelease(atlas_encoder);
        atlas_encoder = NULL;

        memset(&atlas_map_result, 0, sizeof(atlas_map_result));
        {
            WGPUBufferMapCallbackInfo cb;
            memset(&cb, 0, sizeof(cb));
            cb.mode = WGPUCallbackMode_AllowProcessEvents;
            cb.callback = on_map;
            cb.userdata1 = &atlas_map_result;
            (void)wgpuBufferMapAsync(atlas_readback, WGPUMapMode_Read, 0, atlas_size, cb);
            if (wait_flag(instance, &atlas_map_result.done, "map atlas") != 0)
                goto cleanup;
        }
        if (atlas_map_result.status != WGPUMapAsyncStatus_Success) {
            fprintf(stderr, "error: atlas map failed (status %d): %s\n",
                    (int)atlas_map_result.status, atlas_map_result.message);
            goto cleanup;
        }
        {
            const unsigned char *atlas_mapped = (const unsigned char *)wgpuBufferGetConstMappedRange(
                atlas_readback, 0, atlas_size);
            if (atlas_mapped == NULL) {
                fprintf(stderr, "error: wgpuBufferGetConstMappedRange(atlas) failed\n");
                goto cleanup;
            }
            atlas_hash_gpu = fnv1a_rows(atlas_mapped, ATLAS_WIDTH, row_pitch, ATLAS_HEIGHT);
            wgpuBufferUnmap(atlas_readback);
        }
        printf("atlas: uploaded %dx%d R8Unorm (%d bytes, row-pitch %u) "
               "gpu-hash=0x%08X cpu-hash=0x%08X %s\n",
               ATLAS_WIDTH, ATLAS_HEIGHT, ATLAS_PIXELS, row_pitch,
               atlas_hash_gpu, atlas_hash_cpu,
               atlas_hash_gpu == atlas_hash_cpu ? "MATCH" : "MISMATCH");
        if (atlas_hash_gpu != atlas_hash_cpu) {
            fprintf(stderr, "error: atlas readback differs from baked pixels\n");
            goto cleanup;
        }
    }
#endif /* SCENE_HAS_TEXT */

    t_pipeline = now_ms();

    /* One render pass: rects first, then glyph quads on top. */
    encoder = wgpuDeviceCreateCommandEncoder(device, NULL);
    if (encoder == NULL) {
        fprintf(stderr, "error: wgpuDeviceCreateCommandEncoder failed\n");
        goto cleanup;
    }
    {
        WGPURenderPassColorAttachment attachment;
        WGPURenderPassDescriptor descriptor;

        memset(&attachment, 0, sizeof(attachment));
        attachment.view = target_view;
        attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
        attachment.resolveTarget = NULL;
        attachment.loadOp = WGPULoadOp_Clear;
        attachment.storeOp = WGPUStoreOp_Store;
        attachment.clearValue.r = SCENE_CLEAR_R;
        attachment.clearValue.g = SCENE_CLEAR_G;
        attachment.clearValue.b = SCENE_CLEAR_B;
        attachment.clearValue.a = SCENE_CLEAR_A;

        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("scene-pass");
        descriptor.colorAttachmentCount = 1;
        descriptor.colorAttachments = &attachment;
        descriptor.depthStencilAttachment = NULL;
        descriptor.occlusionQuerySet = NULL;
        descriptor.timestampWrites = NULL;

        pass = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
        if (pass == NULL) {
            fprintf(stderr, "error: wgpuCommandEncoderBeginRenderPass failed\n");
            goto cleanup;
        }
        /* draw 1: opaque rects (background, card, chips) */
        wgpuRenderPassEncoderSetPipeline(pass, rect_pipeline);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 0, rect_buffer, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderDraw(pass, SCENE_VERTEX_COUNT, 1, 0, 0);
#if SCENE_HAS_TEXT
        /* draw 2: glyph quads from the R8 atlas */
        wgpuRenderPassEncoderSetPipeline(pass, glyph_pipeline);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, bind_group, 0, NULL);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 0, glyph_buffer, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderDraw(pass, SCENE_GLYPH_VERTEX_COUNT, 1, 0, 0);
        glyphs_drawn = SCENE_GLYPH_QUAD_COUNT;
#endif
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
        pass = NULL;
    }

    /* Texture -> buffer with 256-byte row alignment. */
    bytes_per_row = (SCENE_WIDTH * 4u + 255u) & ~255u;
    readback_size = (uint64_t)bytes_per_row * SCENE_HEIGHT;
    {
        WGPUBufferDescriptor descriptor;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("scene-readback");
        descriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
        descriptor.size = readback_size;
        readback = wgpuDeviceCreateBuffer(device, &descriptor);
        if (readback == NULL) {
            fprintf(stderr, "error: wgpuDeviceCreateBuffer(readback) failed\n");
            goto cleanup;
        }
    }
    {
        WGPUTexelCopyTextureInfo source;
        WGPUTexelCopyBufferInfo destination;
        WGPUExtent3D extent;
        memset(&source, 0, sizeof(source));
        memset(&destination, 0, sizeof(destination));
        memset(&extent, 0, sizeof(extent));
        source.texture = target;
        source.mipLevel = 0;
        source.aspect = WGPUTextureAspect_All;
        destination.layout.offset = 0;
        destination.layout.bytesPerRow = bytes_per_row;
        destination.layout.rowsPerImage = SCENE_HEIGHT;
        destination.buffer = readback;
        extent.width = SCENE_WIDTH;
        extent.height = SCENE_HEIGHT;
        extent.depthOrArrayLayers = 1;
        wgpuCommandEncoderCopyTextureToBuffer(encoder, &source, &destination, &extent);
    }
    commands = wgpuCommandEncoderFinish(encoder, NULL);
    if (commands == NULL) {
        fprintf(stderr, "error: wgpuCommandEncoderFinish failed\n");
        goto cleanup;
    }
    wgpuQueueSubmit(queue, 1, &commands);
    wgpuCommandEncoderRelease(encoder);
    encoder = NULL;

    t_submit = now_ms();

    {
        WGPUBufferMapCallbackInfo cb;
        memset(&map_result, 0, sizeof(map_result));
        memset(&cb, 0, sizeof(cb));
        cb.mode = WGPUCallbackMode_AllowProcessEvents;
        cb.callback = on_map;
        cb.userdata1 = &map_result;
        (void)wgpuBufferMapAsync(readback, WGPUMapMode_Read, 0, readback_size, cb);
        if (wait_flag(instance, &map_result.done, "map readback") != 0)
            goto cleanup;
    }
    if (map_result.status != WGPUMapAsyncStatus_Success) {
        fprintf(stderr, "error: buffer map failed (status %d): %s\n",
                (int)map_result.status, map_result.message);
        goto cleanup;
    }
    mapped = (const unsigned char *)wgpuBufferGetConstMappedRange(readback, 0, readback_size);
    if (mapped == NULL) {
        fprintf(stderr, "error: wgpuBufferGetConstMappedRange failed\n");
        goto cleanup;
    }

    t_read = now_ms();

    {
        HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        if (FAILED(hr)) {
            report_hr(hr, "CoInitializeEx");
            goto cleanup;
        }
        if (write_png_rgba(mapped, SCENE_WIDTH, SCENE_HEIGHT, bytes_per_row, out_path) != 0) {
            CoUninitialize();
            goto cleanup;
        }
        CoUninitialize();
    }
    wgpuBufferUnmap(readback);

    t_png = now_ms();

    wprintf(L"scene_text: wrote %ls (%dx%d)\n", out_path, SCENE_WIDTH, SCENE_HEIGHT);
    printf("scene_text: %d glyph(s) drawn in %d draw call(s) after %d rect op(s)\n",
           glyphs_drawn, glyphs_drawn > 0 ? 2 : 1, SCENE_RECT_COUNT);
    printf("timings: init+adapter+device+upload=%.2fms | atlas+shaders=%.2fms | "
           "pipelines+bindgroup=%.2fms | render+copy+submit=%.2fms | map=%.2fms | "
           "png=%.2fms | total=%.2fms\n",
           t_init - t_start, t_upload - t_init, (t_atlas - t_upload) + (t_pipeline - t_atlas),
           t_submit - t_pipeline, t_read - t_submit, t_png - t_read, t_png - t_start);
    exit_code = 0;

cleanup:
    if (pass != NULL) wgpuRenderPassEncoderRelease(pass);
    if (commands != NULL) wgpuCommandBufferRelease(commands);
    if (encoder != NULL) wgpuCommandEncoderRelease(encoder);
    if (atlas_commands != NULL) wgpuCommandBufferRelease(atlas_commands);
    if (atlas_encoder != NULL) wgpuCommandEncoderRelease(atlas_encoder);
    if (atlas_readback != NULL) wgpuBufferRelease(atlas_readback);
    if (bind_group != NULL) wgpuBindGroupRelease(bind_group);
    if (glyph_pipeline_layout != NULL) wgpuPipelineLayoutRelease(glyph_pipeline_layout);
    if (bind_group_layout != NULL) wgpuBindGroupLayoutRelease(bind_group_layout);
    if (sampler != NULL) wgpuSamplerRelease(sampler);
    if (glyph_pipeline != NULL) wgpuRenderPipelineRelease(glyph_pipeline);
    if (rect_pipeline != NULL) wgpuRenderPipelineRelease(rect_pipeline);
    if (glyph_shader != NULL) wgpuShaderModuleRelease(glyph_shader);
    if (rect_shader != NULL) wgpuShaderModuleRelease(rect_shader);
    if (atlas_view != NULL) wgpuTextureViewRelease(atlas_view);
    if (atlas_texture != NULL) wgpuTextureRelease(atlas_texture);
    if (readback != NULL) wgpuBufferRelease(readback);
    if (glyph_buffer != NULL) wgpuBufferRelease(glyph_buffer);
    if (rect_buffer != NULL) wgpuBufferRelease(rect_buffer);
    if (target_view != NULL) wgpuTextureViewRelease(target_view);
    if (target != NULL) wgpuTextureRelease(target);
    if (queue != NULL) wgpuQueueRelease(queue);
    if (device != NULL) wgpuDeviceRelease(device);
    if (adapter != NULL) wgpuAdapterRelease(adapter);
    if (instance != NULL) wgpuInstanceRelease(instance);
    return exit_code;
}
