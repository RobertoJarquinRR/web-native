/*
 * runtime_r2.c - webnative R2 core runtime (src/runtime/c-r2).
 *
 * A real runtime, not an AOT viewer:
 *   - layout is computed in C at runtime from the generated UiNode table,
 *   - the Win32 window is resizable and WM_SIZE triggers a full re-layout,
 *   - hit testing drives hover / pressed / selected state,
 *   - all rectangles are rendered by wgpu-native (D3D12 on this machine),
 *   - the same layout engine can render deterministic offscreen PNGs.
 *
 * There is no browser, no WebView, no Skia, no Electron and no Python at
 * runtime. Build-time Python only produces ui_data.h (node table + strings).
 *
 * Usage:
 *   runtime_r2.exe --render 960 640 out.png
 *   runtime_r2.exe --render 960x640 out.png [--hover ID] [--press ID] [--selected ID]
 *   runtime_r2.exe --states out_prefix [--node ID] [--size 960x640]
 *   runtime_r2.exe --report layout_report.json [--sizes 960x640,720x640,560x640]
 *   runtime_r2.exe --dynamic out_prefix        (11-stage Dynamic UI v0 demo)
 *   runtime_r2.exe --input out_prefix          (10-stage text input v1 demo)
 *   runtime_r2.exe --frames 30 [--resize-demo] [--input-demo]
 *   runtime_r2.exe                      (window; Esc blurs an input, else closes)
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <wchar.h>

#include <wincodec.h>
#include <wgpu.h>

#include "ui_data.h"
#include "wn_core.h"
#include "wn_internal.h"

/* ui-tree/3 only emits R2_NODE_INPUT in headers with inputs; the demo helper
 * must compile against v1/v2 headers too. */
#ifndef R2_NODE_INPUT
#define R2_NODE_INPUT 5
#endif

#define ARRAY_LEN(a) ((size_t)(sizeof(a) / sizeof((a)[0])))
#define MAX_FLEX_ITEMS UI_NODE_COUNT

/* --------------------------------------------------------------- utilities */

static WGPUStringView sv(const char *text)
{
    WGPUStringView view;
    view.data = text;
    view.length = WGPU_STRLEN;
    return view;
}

static double now_ms(void)
{
    LARGE_INTEGER freq, tick;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&tick);
    return (double)tick.QuadPart * 1000.0 / (double)freq.QuadPart;
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

static void print_view(const char *label, WGPUStringView view)
{
    fputs(label, stdout);
    fprint_view(stdout, view);
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

/* wgpu-native v29.0.1.1 does not implement wgpuInstanceWaitAny; poll. */
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
    fflush(stderr);
}

static void on_log(WGPULogLevel level, WGPUStringView message, void *userdata)
{
    (void)userdata;
    if (level <= WGPULogLevel_Warn) {
        fprintf(stderr, "[wgpu %d] ", (int)level);
        fprint_view(stderr, message);
        fputc('\n', stderr);
        fflush(stderr);
    }
}

/* ------------------------------------------------------- core bridge (R2) */

/*
 * The layout, state machine, hit testing and display-list generation live in
 * wn_core.c. runtime_r2.c keeps only the CLI, reporting, Win32 and wgpu code,
 * so the reference executable and the ABI layer share one engine.
 */

typedef wn_rect Rect;
typedef wn_vertex Vertex;
typedef wn_glyph_vertex GlyphVertex;

static wn_core *g_core;

#define g_rects (wn_core_rects(g_core, NULL))
#define g_hover (wn_core_hover(g_core))
#define g_press (wn_core_press(g_core))
#define g_selected (wn_core_selected(g_core))
#define g_vertices (wn_core_display_list(g_core)->vertices)
#define g_glyph_vertices (wn_core_display_list(g_core)->glyph_vertices)
#define g_image_vertices (wn_core_display_list(g_core)->image_vertices)

static int g_vertex_count = 0;
static int g_glyph_vertex_count = 0;
static int g_image_vertex_count = 0;

static void layout_page(float viewport_w, float viewport_h)
{
    wn_core_resize(g_core, (int)viewport_w, (int)viewport_h);
    wn_core_relayout(g_core);
}

static void build_geometry(float viewport_w, float viewport_h)
{
    const wn_display_list *list;
    (void)viewport_w;
    (void)viewport_h;
    wn_core_build_display_list(g_core);
    list = wn_core_display_list(g_core);
    g_vertex_count = list != NULL ? list->vertex_count : 0;
    g_glyph_vertex_count = list != NULL ? list->glyph_vertex_count : 0;
    g_image_vertex_count = list != NULL ? list->image_vertex_count : 0;
}

static void set_state_from_arg(int state, int node)
{
    if (node < 0) {
        wn_core_set_state(g_core, 0, state, 0);
    } else {
        wn_core_set_state(g_core, node, state, 1);
    }
}

/* ------------------------------------------------------------------- wgpu */

typedef struct {
    char *path;
    WGPUTexture texture;
    WGPUTextureView view;
    WGPUBindGroup bind;
    int width;
    int height;
} R2ImageTexture;

typedef struct {
    WGPUInstance instance;
    WGPUAdapter adapter;
    WGPUDevice device;
    WGPUQueue queue;
    WGPUShaderModule shader;
    WGPURenderPipeline pipeline;
    WGPUBuffer vertex_buffer;
    WGPUTextureFormat format;
    size_t vertex_capacity;
    /* R8 glyph atlas + textured pipeline (stretch goal) */
    WGPUShaderModule text_shader;
    WGPURenderPipeline text_pipeline;
    WGPUBuffer glyph_buffer;
    size_t glyph_capacity;
    WGPUTexture atlas_texture;
    WGPUTextureView atlas_view;
    WGPUSampler sampler;
    WGPUBindGroupLayout text_bgl;
    WGPUPipelineLayout text_pl;
    WGPUBindGroup text_bg;
    /* RGBA8 local images (ui-tree/2) */
    WGPUShaderModule image_shader;
    WGPURenderPipeline image_pipeline;
    WGPUBuffer image_buffer;
    size_t image_capacity;
    R2ImageTexture *images;
    int image_count;
    int image_cache_capacity;
    IWICImagingFactory *wic;
    int image_width_hint;
    int image_height_hint;
} Gpu;

static const char *kShaderWGSL =
    "struct VSOut {\n"
    "  @builtin(position) position: vec4<f32>,\n"
    "  @location(0) color: vec4<f32>,\n"
    "};\n"
    "\n"
    "@vertex\n"
    "fn vs_main(@location(0) position: vec2<f32>, @location(1) color: vec4<f32>) -> VSOut {\n"
    "  var out: VSOut;\n"
    "  out.position = vec4<f32>(position, 0.0, 1.0);\n"
    "  out.color = color;\n"
    "  return out;\n"
    "}\n"
    "\n"
    "@fragment\n"
    "fn fs_main(in: VSOut) -> @location(0) vec4<f32> {\n"
    "  return in.color;\n"
    "}\n";

/* Text uses the alpha channel of an R8 atlas. Positions are pre-shaped at
 * build time, so the vertex shader only needs the current viewport transform
 * (already baked into clip space by the CPU). */
static const char *kTextShaderWGSL =
    "@group(0) @binding(0) var atlas_texture: texture_2d<f32>;\n"
    "@group(0) @binding(1) var atlas_sampler: sampler;\n"
    "\n"
    "struct VSOut {\n"
    "  @builtin(position) position: vec4<f32>,\n"
    "  @location(0) uv: vec2<f32>,\n"
    "  @location(1) color: vec4<f32>,\n"
    "};\n"
    "\n"
    "@vertex\n"
    "fn vs_main(@location(0) position: vec2<f32>, @location(1) uv: vec2<f32>,\n"
    "           @location(2) color: vec4<f32>) -> VSOut {\n"
    "  var out: VSOut;\n"
    "  out.position = vec4<f32>(position, 0.0, 1.0);\n"
    "  out.uv = uv;\n"
    "  out.color = color;\n"
    "  return out;\n"
    "}\n"
    "\n"
    "@fragment\n"
    "fn fs_main(in: VSOut) -> @location(0) vec4<f32> {\n"
    "  let coverage = textureSample(atlas_texture, atlas_sampler, in.uv).r;\n"
    "  return vec4<f32>(in.color.rgb, in.color.a * coverage);\n"
    "}\n";

static const char *kImageShaderWGSL =
    "@group(0) @binding(0) var image_texture: texture_2d<f32>;\n"
    "@group(0) @binding(1) var image_sampler: sampler;\n"
    "\n"
    "struct VSOut {\n"
    "  @builtin(position) position: vec4<f32>,\n"
    "  @location(0) uv: vec2<f32>,\n"
    "};\n"
    "\n"
    "@vertex\n"
    "fn vs_main(@location(0) position: vec2<f32>, @location(1) uv: vec2<f32>) -> VSOut {\n"
    "  var out: VSOut;\n"
    "  out.position = vec4<f32>(position, 0.0, 1.0);\n"
    "  out.uv = uv;\n"
    "  return out;\n"
    "}\n"
    "\n"
    "@fragment\n"
    "fn fs_main(in: VSOut) -> @location(0) vec4<f32> {\n"
    "  return textureSample(image_texture, image_sampler, in.uv);\n"
    "}\n";

static void gpu_release(Gpu *gpu)
{
    int i;
    if (gpu->vertex_buffer) wgpuBufferRelease(gpu->vertex_buffer);
    if (gpu->glyph_buffer) wgpuBufferRelease(gpu->glyph_buffer);
    if (gpu->image_buffer) wgpuBufferRelease(gpu->image_buffer);
    for (i = 0; i < gpu->image_count; ++i) {
        if (gpu->images[i].bind) wgpuBindGroupRelease(gpu->images[i].bind);
        if (gpu->images[i].view) wgpuTextureViewRelease(gpu->images[i].view);
        if (gpu->images[i].texture) wgpuTextureRelease(gpu->images[i].texture);
        free(gpu->images[i].path);
    }
    free(gpu->images);
    if (gpu->wic) gpu->wic->lpVtbl->Release(gpu->wic);
    if (gpu->text_bg) wgpuBindGroupRelease(gpu->text_bg);
    if (gpu->text_pl) wgpuPipelineLayoutRelease(gpu->text_pl);
    if (gpu->text_bgl) wgpuBindGroupLayoutRelease(gpu->text_bgl);
    if (gpu->sampler) wgpuSamplerRelease(gpu->sampler);
    if (gpu->atlas_view) wgpuTextureViewRelease(gpu->atlas_view);
    if (gpu->atlas_texture) wgpuTextureRelease(gpu->atlas_texture);
    if (gpu->image_pipeline) wgpuRenderPipelineRelease(gpu->image_pipeline);
    if (gpu->image_shader) wgpuShaderModuleRelease(gpu->image_shader);
    if (gpu->text_pipeline) wgpuRenderPipelineRelease(gpu->text_pipeline);
    if (gpu->text_shader) wgpuShaderModuleRelease(gpu->text_shader);
    if (gpu->pipeline) wgpuRenderPipelineRelease(gpu->pipeline);
    if (gpu->shader) wgpuShaderModuleRelease(gpu->shader);
    if (gpu->queue) wgpuQueueRelease(gpu->queue);
    if (gpu->device) wgpuDeviceRelease(gpu->device);
    if (gpu->adapter) wgpuAdapterRelease(gpu->adapter);
    if (gpu->instance) wgpuInstanceRelease(gpu->instance);
    memset(gpu, 0, sizeof(*gpu));
}

static int gpu_create_text(Gpu *gpu);
static int gpu_create_images(Gpu *gpu);

static int gpu_init(Gpu *gpu, WGPUSurface compatible_surface,
                    int request_surface_format, WGPUTextureFormat forced_format,
                    const char *label)
{
    AdapterResult adapter_result;
    DeviceResult device_result;
    WGPUAdapterInfo info;

    memset(gpu, 0, sizeof(*gpu));
    gpu->instance = wgpuCreateInstance(NULL);
    if (gpu->instance == NULL) {
        fprintf(stderr, "error: wgpuCreateInstance returned NULL\n");
        return 1;
    }

    memset(&adapter_result, 0, sizeof(adapter_result));
    {
        WGPURequestAdapterOptions options;
        WGPURequestAdapterCallbackInfo cb;
        memset(&options, 0, sizeof(options));
        memset(&cb, 0, sizeof(cb));
        options.powerPreference = WGPUPowerPreference_HighPerformance;
        options.featureLevel = WGPUFeatureLevel_Core;
        options.compatibleSurface = compatible_surface;
        cb.mode = WGPUCallbackMode_AllowProcessEvents;
        cb.callback = on_adapter;
        cb.userdata1 = &adapter_result;
        (void)wgpuInstanceRequestAdapter(gpu->instance, &options, cb);
        if (wait_flag(gpu->instance, &adapter_result.done, "request adapter") != 0) {
            return 1;
        }
    }
    if (adapter_result.status != WGPURequestAdapterStatus_Success || adapter_result.adapter == NULL) {
        fprintf(stderr, "error: no adapter (status %d): %s\n",
                (int)adapter_result.status, adapter_result.message);
        return 1;
    }
    gpu->adapter = adapter_result.adapter;

    memset(&info, 0, sizeof(info));
    if (wgpuAdapterGetInfo(gpu->adapter, &info) == WGPUStatus_Success) {
        printf("adapter: backend=%s (%d) type=%s vendorID=0x%04X deviceID=0x%04X\n",
               backend_name(info.backendType), (int)info.backendType,
               adapter_type_name(info.adapterType), info.vendorID, info.deviceID);
        print_view("adapter: device=", info.device);
        printf("\n");
        print_view("adapter: description=", info.description);
        printf("\n");
        fflush(stdout);
    }

    memset(&device_result, 0, sizeof(device_result));
    {
        WGPUDeviceDescriptor descriptor;
        WGPURequestDeviceCallbackInfo cb;
        memset(&descriptor, 0, sizeof(descriptor));
        memset(&cb, 0, sizeof(cb));
        descriptor.label = sv(label);
        descriptor.requiredFeatureCount = 0;
        descriptor.uncapturedErrorCallbackInfo.callback = on_uncaptured_error;
        cb.mode = WGPUCallbackMode_AllowProcessEvents;
        cb.callback = on_device;
        cb.userdata1 = &device_result;
        (void)wgpuAdapterRequestDevice(gpu->adapter, &descriptor, cb);
        if (wait_flag(gpu->instance, &device_result.done, "request device") != 0) {
            return 1;
        }
    }
    if (device_result.status != WGPURequestDeviceStatus_Success || device_result.device == NULL) {
        fprintf(stderr, "error: no device (status %d): %s\n",
                (int)device_result.status, device_result.message);
        return 1;
    }
    gpu->device = device_result.device;
    gpu->queue = wgpuDeviceGetQueue(gpu->device);

    /* Pick a format: prefer a plain (non-sRGB) 8-bit format so offscreen and
     * surface colors match byte for byte. */
    gpu->format = forced_format;
    if (request_surface_format) {
        WGPUSurfaceCapabilities caps;
        memset(&caps, 0, sizeof(caps));
        if (wgpuSurfaceGetCapabilities(compatible_surface, gpu->adapter, &caps) != WGPUStatus_Success ||
            caps.formatCount == 0) {
            fprintf(stderr, "error: surface capabilities failed\n");
            return 1;
        }
        gpu->format = caps.formats[0];
        for (size_t i = 0; i < caps.formatCount; ++i) {
            if (caps.formats[i] == WGPUTextureFormat_BGRA8Unorm ||
                caps.formats[i] == WGPUTextureFormat_RGBA8Unorm) {
                gpu->format = caps.formats[i];
                break;
            }
        }
        printf("surface: formats=%zu chosenFormat=%d\n", caps.formatCount, (int)gpu->format);
    }

    {
        WGPUShaderModuleDescriptor descriptor;
        WGPUShaderSourceWGSL wgsl;
        memset(&descriptor, 0, sizeof(descriptor));
        memset(&wgsl, 0, sizeof(wgsl));
        wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
        wgsl.code = sv(kShaderWGSL);
        descriptor.label = sv("r2-shader");
        descriptor.nextInChain = &wgsl.chain;
        gpu->shader = wgpuDeviceCreateShaderModule(gpu->device, &descriptor);
        if (gpu->shader == NULL) {
            fprintf(stderr, "error: shader module creation failed\n");
            return 1;
        }
    }

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
        buffer_layout.arrayStride = sizeof(Vertex);
        buffer_layout.attributeCount = (uint32_t)ARRAY_LEN(attributes);
        buffer_layout.attributes = attributes;

        memset(&blend, 0, sizeof(blend));
        blend.color.operation = WGPUBlendOperation_Add;
        blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
        blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
        blend.alpha.operation = WGPUBlendOperation_Add;
        blend.alpha.srcFactor = WGPUBlendFactor_One;
        blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;

        memset(&target, 0, sizeof(target));
        target.format = gpu->format;
        target.blend = &blend;
        target.writeMask = WGPUColorWriteMask_All;

        memset(&fragment, 0, sizeof(fragment));
        fragment.module = gpu->shader;
        fragment.entryPoint = sv("fs_main");
        fragment.targetCount = 1;
        fragment.targets = &target;

        memset(&vertex, 0, sizeof(vertex));
        vertex.module = gpu->shader;
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
        descriptor.label = sv("r2-rect-pipeline");
        descriptor.layout = NULL;
        descriptor.vertex = vertex;
        descriptor.primitive = primitive;
        descriptor.depthStencil = NULL;
        descriptor.multisample = multisample;
        descriptor.fragment = &fragment;
        gpu->pipeline = wgpuDeviceCreateRenderPipeline(gpu->device, &descriptor);
        if (gpu->pipeline == NULL) {
            fprintf(stderr, "error: render pipeline creation failed\n");
            return 1;
        }
    }

    gpu->vertex_capacity = (size_t)UI_NODE_COUNT * 6u * sizeof(Vertex);
    {
        WGPUBufferDescriptor descriptor;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("r2-vertices");
        descriptor.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        descriptor.size = gpu->vertex_capacity;
        gpu->vertex_buffer = wgpuDeviceCreateBuffer(gpu->device, &descriptor);
        if (gpu->vertex_buffer == NULL) {
            fprintf(stderr, "error: vertex buffer creation failed\n");
            return 1;
        }
    }
    return gpu_create_text(gpu) == 0 && gpu_create_images(gpu) == 0 ? 0 : 1;
}

/* R8 glyph atlas + textured pipeline. Created once; the atlas is immutable. */
static int gpu_create_text(Gpu *gpu)
{
    {
        WGPUShaderModuleDescriptor descriptor;
        WGPUShaderSourceWGSL wgsl;
        memset(&descriptor, 0, sizeof(descriptor));
        memset(&wgsl, 0, sizeof(wgsl));
        wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
        wgsl.code = sv(kTextShaderWGSL);
        descriptor.label = sv("r2-text-shader");
        descriptor.nextInChain = &wgsl.chain;
        gpu->text_shader = wgpuDeviceCreateShaderModule(gpu->device, &descriptor);
        if (gpu->text_shader == NULL) {
            fprintf(stderr, "error: text shader creation failed\n");
            return 1;
        }
    }

    {
        WGPUTextureDescriptor descriptor;
        WGPUTexelCopyTextureInfo destination;
        WGPUTexelCopyBufferLayout layout;
        WGPUExtent3D extent;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("r2-glyph-atlas");
        descriptor.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
        descriptor.dimension = WGPUTextureDimension_2D;
        descriptor.size.width = UI_ATLAS_SIZE;
        descriptor.size.height = UI_ATLAS_SIZE;
        descriptor.size.depthOrArrayLayers = 1;
        descriptor.format = WGPUTextureFormat_R8Unorm;
        descriptor.mipLevelCount = 1;
        descriptor.sampleCount = 1;
        gpu->atlas_texture = wgpuDeviceCreateTexture(gpu->device, &descriptor);
        if (gpu->atlas_texture == NULL) {
            fprintf(stderr, "error: glyph atlas creation failed\n");
            return 1;
        }
        gpu->atlas_view = wgpuTextureCreateView(gpu->atlas_texture, NULL);
        if (gpu->atlas_view == NULL) {
            fprintf(stderr, "error: glyph atlas view failed\n");
            return 1;
        }
        memset(&destination, 0, sizeof(destination));
        destination.texture = gpu->atlas_texture;
        destination.mipLevel = 0;
        destination.aspect = WGPUTextureAspect_All;
        memset(&layout, 0, sizeof(layout));
        layout.offset = 0;
        layout.bytesPerRow = UI_ATLAS_SIZE;
        layout.rowsPerImage = UI_ATLAS_SIZE;
        memset(&extent, 0, sizeof(extent));
        extent.width = UI_ATLAS_SIZE;
        extent.height = UI_ATLAS_SIZE;
        extent.depthOrArrayLayers = 1;
        wgpuQueueWriteTexture(gpu->queue, &destination, kUiAtlas, sizeof(kUiAtlas),
                              &layout, &extent);
    }

    {
        WGPUSamplerDescriptor descriptor;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("r2-atlas-sampler");
        descriptor.addressModeU = WGPUAddressMode_ClampToEdge;
        descriptor.addressModeV = WGPUAddressMode_ClampToEdge;
        descriptor.addressModeW = WGPUAddressMode_ClampToEdge;
        descriptor.magFilter = WGPUFilterMode_Linear;
        descriptor.minFilter = WGPUFilterMode_Linear;
        descriptor.mipmapFilter = WGPUMipmapFilterMode_Nearest;
        descriptor.lodMinClamp = 0.0f;
        descriptor.lodMaxClamp = 32.0f;
        descriptor.maxAnisotropy = 1;
        gpu->sampler = wgpuDeviceCreateSampler(gpu->device, &descriptor);
        if (gpu->sampler == NULL) {
            fprintf(stderr, "error: sampler creation failed\n");
            return 1;
        }
    }

    {
        WGPUBindGroupLayoutEntry entries[2];
        WGPUBindGroupLayoutDescriptor descriptor;
        memset(entries, 0, sizeof(entries));
        entries[0].binding = 0;
        entries[0].visibility = WGPUShaderStage_Fragment;
        entries[0].texture.sampleType = WGPUTextureSampleType_Float;
        entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
        entries[1].binding = 1;
        entries[1].visibility = WGPUShaderStage_Fragment;
        entries[1].sampler.type = WGPUSamplerBindingType_Filtering;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("r2-text-bgl");
        descriptor.entryCount = 2;
        descriptor.entries = entries;
        gpu->text_bgl = wgpuDeviceCreateBindGroupLayout(gpu->device, &descriptor);
        if (gpu->text_bgl == NULL) {
            fprintf(stderr, "error: bind group layout creation failed\n");
            return 1;
        }
    }

    {
        WGPUPipelineLayoutDescriptor descriptor;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("r2-text-pipeline-layout");
        descriptor.bindGroupLayoutCount = 1;
        descriptor.bindGroupLayouts = &gpu->text_bgl;
        gpu->text_pl = wgpuDeviceCreatePipelineLayout(gpu->device, &descriptor);
        if (gpu->text_pl == NULL) {
            fprintf(stderr, "error: pipeline layout creation failed\n");
            return 1;
        }
    }

    {
        WGPUBindGroupEntry entries[2];
        WGPUBindGroupDescriptor descriptor;
        memset(entries, 0, sizeof(entries));
        entries[0].binding = 0;
        entries[0].textureView = gpu->atlas_view;
        entries[1].binding = 1;
        entries[1].sampler = gpu->sampler;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("r2-text-bind-group");
        descriptor.layout = gpu->text_bgl;
        descriptor.entryCount = 2;
        descriptor.entries = entries;
        gpu->text_bg = wgpuDeviceCreateBindGroup(gpu->device, &descriptor);
        if (gpu->text_bg == NULL) {
            fprintf(stderr, "error: bind group creation failed\n");
            return 1;
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
        WGPURenderPipelineDescriptor descriptor;

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
        buffer_layout.arrayStride = 20; /* GlyphVertex */
        buffer_layout.attributeCount = 3;
        buffer_layout.attributes = attributes;

        memset(&blend, 0, sizeof(blend));
        blend.color.operation = WGPUBlendOperation_Add;
        blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
        blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
        blend.alpha.operation = WGPUBlendOperation_Add;
        blend.alpha.srcFactor = WGPUBlendFactor_One;
        blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;

        memset(&target, 0, sizeof(target));
        target.format = gpu->format;
        target.blend = &blend;
        target.writeMask = WGPUColorWriteMask_All;

        memset(&fragment, 0, sizeof(fragment));
        fragment.module = gpu->text_shader;
        fragment.entryPoint = sv("fs_main");
        fragment.targetCount = 1;
        fragment.targets = &target;

        memset(&vertex, 0, sizeof(vertex));
        vertex.module = gpu->text_shader;
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
        descriptor.label = sv("r2-text-pipeline");
        descriptor.layout = gpu->text_pl;
        descriptor.vertex = vertex;
        descriptor.primitive = primitive;
        descriptor.depthStencil = NULL;
        descriptor.multisample = multisample;
        descriptor.fragment = &fragment;
        gpu->text_pipeline = wgpuDeviceCreateRenderPipeline(gpu->device, &descriptor);
        if (gpu->text_pipeline == NULL) {
            fprintf(stderr, "error: text pipeline creation failed\n");
            return 1;
        }
    }

    {
        WGPUBufferDescriptor descriptor;
        size_t glyphs = UI_GLYPH_COUNT > 0 ? (size_t)UI_GLYPH_COUNT : 1u;
        gpu->glyph_capacity = glyphs * 6u * 20u;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("r2-glyph-vertices");
        descriptor.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        descriptor.size = gpu->glyph_capacity;
        gpu->glyph_buffer = wgpuDeviceCreateBuffer(gpu->device, &descriptor);
        if (gpu->glyph_buffer == NULL) {
            fprintf(stderr, "error: glyph vertex buffer creation failed\n");
            return 1;
        }
    }
    return 0;
}

/* RGBA8 image pipeline; shares the texture+sampler bind group layout with the
 * glyph atlas. Local PNG/JPEG only (WIC, no network). */
static int gpu_create_images(Gpu *gpu)
{
    {
        WGPUShaderModuleDescriptor descriptor;
        WGPUShaderSourceWGSL wgsl;
        memset(&descriptor, 0, sizeof(descriptor));
        memset(&wgsl, 0, sizeof(wgsl));
        wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
        wgsl.code = sv(kImageShaderWGSL);
        descriptor.label = sv("r2-image-shader");
        descriptor.nextInChain = &wgsl.chain;
        gpu->image_shader = wgpuDeviceCreateShaderModule(gpu->device, &descriptor);
        if (gpu->image_shader == NULL) {
            fprintf(stderr, "error: image shader creation failed\n");
            return 1;
        }
    }
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
        attributes[1].format = WGPUVertexFormat_Float32x2;
        attributes[1].offset = 8;
        attributes[1].shaderLocation = 1;
        memset(&buffer_layout, 0, sizeof(buffer_layout));
        buffer_layout.stepMode = WGPUVertexStepMode_Vertex;
        buffer_layout.arrayStride = sizeof(wn_image_vertex);
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
        target.format = gpu->format;
        target.blend = &blend;
        target.writeMask = WGPUColorWriteMask_All;
        memset(&fragment, 0, sizeof(fragment));
        fragment.module = gpu->image_shader;
        fragment.entryPoint = sv("fs_main");
        fragment.targetCount = 1;
        fragment.targets = &target;
        memset(&vertex, 0, sizeof(vertex));
        vertex.module = gpu->image_shader;
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
        descriptor.label = sv("r2-image-pipeline");
        descriptor.layout = gpu->text_pl;
        descriptor.vertex = vertex;
        descriptor.primitive = primitive;
        descriptor.multisample = multisample;
        descriptor.fragment = &fragment;
        gpu->image_pipeline = wgpuDeviceCreateRenderPipeline(gpu->device, &descriptor);
        if (gpu->image_pipeline == NULL) {
            fprintf(stderr, "error: image pipeline creation failed\n");
            return 1;
        }
    }
    {
        WGPUBufferDescriptor descriptor;
        size_t images = UI_NODE_COUNT > 0 ? (size_t)UI_NODE_COUNT : 1u;
        gpu->image_capacity = images * 6u * sizeof(wn_image_vertex);
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("r2-image-vertices");
        descriptor.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        descriptor.size = gpu->image_capacity;
        gpu->image_buffer = wgpuDeviceCreateBuffer(gpu->device, &descriptor);
        if (gpu->image_buffer == NULL) {
            fprintf(stderr, "error: image vertex buffer creation failed\n");
            return 1;
        }
    }
    return 0;
}

static int gpu_ensure_buffer(Gpu *gpu, WGPUBuffer *buffer, size_t *capacity,
                             size_t bytes, WGPUBufferUsage usage, const char *label)
{
    size_t grown_capacity;
    WGPUBufferDescriptor descriptor;
    WGPUBuffer grown;
    if (gpu == NULL || buffer == NULL || capacity == NULL || bytes == 0) {
        return bytes == 0 ? 0 : 1;
    }
    if (*buffer != NULL && bytes <= *capacity) {
        return 0;
    }
    grown_capacity = *capacity > 0 ? *capacity : 256;
    while (grown_capacity < bytes) {
        grown_capacity *= 2;
    }
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.label = sv(label);
    descriptor.usage = usage;
    descriptor.size = grown_capacity;
    grown = wgpuDeviceCreateBuffer(gpu->device, &descriptor);
    if (grown == NULL) {
        fprintf(stderr, "error: %s buffer creation failed (%zu bytes)\n",
                label, grown_capacity);
        return 1;
    }
    if (*buffer != NULL) {
        wgpuBufferRelease(*buffer);
    }
    *buffer = grown;
    *capacity = grown_capacity;
    return 0;
}

static int gpu_upload_geometry(Gpu *gpu)
{
    size_t bytes = (size_t)g_vertex_count * sizeof(Vertex);
    if (bytes > 0) {
        if (gpu_ensure_buffer(gpu, &gpu->vertex_buffer, &gpu->vertex_capacity, bytes,
                              WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst,
                              "r2-vertices") != 0) {
            return 1;
        }
        wgpuQueueWriteBuffer(gpu->queue, gpu->vertex_buffer, 0, g_vertices, bytes);
    }
    {
        size_t glyph_bytes = (size_t)g_glyph_vertex_count * sizeof(GlyphVertex);
        if (glyph_bytes > 0) {
            if (gpu_ensure_buffer(gpu, &gpu->glyph_buffer, &gpu->glyph_capacity, glyph_bytes,
                                  WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst,
                                  "r2-glyph-vertices") != 0) {
                return 1;
            }
            wgpuQueueWriteBuffer(gpu->queue, gpu->glyph_buffer, 0,
                                 g_glyph_vertices, glyph_bytes);
        }
    }
    {
        size_t image_bytes = (size_t)g_image_vertex_count * sizeof(wn_image_vertex);
        if (image_bytes > 0) {
            if (gpu_ensure_buffer(gpu, &gpu->image_buffer, &gpu->image_capacity, image_bytes,
                                  WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst,
                                  "r2-image-vertices") != 0) {
                return 1;
            }
            wgpuQueueWriteBuffer(gpu->queue, gpu->image_buffer, 0,
                                 g_image_vertices, image_bytes);
        }
    }
    return 0;
}

/* -------------------------------------------------------- WIC image cache */

static int decode_image_rgba(Gpu *gpu, const char *path, unsigned char **pixels_out,
                             int *width_out, int *height_out)
{
    wchar_t wide[MAX_PATH * 4];
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;
    UINT width = 0, height = 0;
    unsigned char *pixels = NULL;
    HRESULT hr;

    *pixels_out = NULL;
    *width_out = 0;
    *height_out = 0;
    if (gpu->wic == NULL) {
        hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                              &IID_IWICImagingFactory, (void **)&gpu->wic);
        if (FAILED(hr) || gpu->wic == NULL) {
            fprintf(stderr, "error: WIC factory creation failed (hr=0x%08lX)\n",
                    (unsigned long)hr);
            return 1;
        }
    }
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide,
                            (int)(sizeof(wide) / sizeof(wide[0]))) <= 0) {
        fprintf(stderr, "error: image path is not valid UTF-8: %s\n", path);
        return 1;
    }
    hr = gpu->wic->lpVtbl->CreateDecoderFromFilename(
        gpu->wic, wide, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr) || decoder == NULL) {
        fprintf(stderr, "error: cannot decode image '%s' (hr=0x%08lX)\n",
                path, (unsigned long)hr);
        return 1;
    }
    hr = decoder->lpVtbl->GetFrame(decoder, 0, &frame);
    if (FAILED(hr) || frame == NULL) {
        fprintf(stderr, "error: image '%s' has no frame 0 (hr=0x%08lX)\n",
                path, (unsigned long)hr);
        goto fail;
    }
    hr = frame->lpVtbl->GetSize(frame, &width, &height);
    if (FAILED(hr) || width == 0 || height == 0 || width > 16384 || height > 16384) {
        fprintf(stderr, "error: image '%s' has invalid size %ux%u\n", path, width, height);
        goto fail;
    }
    hr = gpu->wic->lpVtbl->CreateFormatConverter(gpu->wic, &converter);
    if (FAILED(hr) || converter == NULL) {
        fprintf(stderr, "error: WIC converter creation failed for '%s'\n", path);
        goto fail;
    }
    hr = converter->lpVtbl->Initialize(converter, (IWICBitmapSource *)frame,
                                       &GUID_WICPixelFormat32bppRGBA,
                                       WICBitmapDitherTypeNone, NULL, 0.0,
                                       WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        fprintf(stderr, "error: WIC RGBA conversion failed for '%s' (hr=0x%08lX)\n",
                path, (unsigned long)hr);
        goto fail;
    }
    {
        const UINT stride = width * 4u;
        const UINT bytes = stride * height;
        pixels = (unsigned char *)malloc(bytes);
        if (pixels == NULL) {
            fprintf(stderr, "error: out of memory decoding image '%s'\n", path);
            goto fail;
        }
        hr = converter->lpVtbl->CopyPixels(converter, NULL, stride, bytes, pixels);
        if (FAILED(hr)) {
            fprintf(stderr, "error: WIC CopyPixels failed for '%s' (hr=0x%08lX)\n",
                    path, (unsigned long)hr);
            goto fail;
        }
    }
    converter->lpVtbl->Release(converter);
    frame->lpVtbl->Release(frame);
    decoder->lpVtbl->Release(decoder);
    *pixels_out = pixels;
    *width_out = (int)width;
    *height_out = (int)height;
    return 0;

fail:
    free(pixels);
    if (converter != NULL) converter->lpVtbl->Release(converter);
    if (frame != NULL) frame->lpVtbl->Release(frame);
    if (decoder != NULL) decoder->lpVtbl->Release(decoder);
    return 1;
}

static R2ImageTexture *find_cached_image(Gpu *gpu, const char *path)
{
    int i;
    for (i = 0; i < gpu->image_count; ++i) {
        if (strcmp(gpu->images[i].path, path) == 0) {
            return &gpu->images[i];
        }
    }
    return NULL;
}

static R2ImageTexture *ensure_image_texture(Gpu *gpu, const char *path)
{
    R2ImageTexture *entry;
    WGPUTextureDescriptor texture_desc;
    WGPUTexelCopyTextureInfo destination;
    WGPUTexelCopyBufferLayout layout;
    WGPUExtent3D extent;
    WGPUBindGroupEntry bind_entries[2];
    WGPUBindGroupDescriptor bind_desc;
    unsigned char *pixels = NULL;
    unsigned char *padded = NULL;
    int width = 0, height = 0;
    uint32_t stride;

    entry = find_cached_image(gpu, path);
    if (entry != NULL) {
        return entry;
    }
    if (decode_image_rgba(gpu, path, &pixels, &width, &height) != 0) {
        return NULL;
    }
    if (gpu->image_count >= gpu->image_cache_capacity) {
        int capacity = gpu->image_cache_capacity > 0 ? gpu->image_cache_capacity * 2 : 8;
        R2ImageTexture *grown = (R2ImageTexture *)realloc(
            gpu->images, (size_t)capacity * sizeof(R2ImageTexture));
        if (grown == NULL) {
            free(pixels);
            fprintf(stderr, "error: out of memory growing the image cache\n");
            return NULL;
        }
        gpu->images = grown;
        gpu->image_cache_capacity = capacity;
    }
    stride = (uint32_t)width * 4u;
    {
        uint32_t aligned = (stride + 255u) & ~255u;
        if (aligned != stride && height > 1) {
            uint32_t y;
            padded = (unsigned char *)malloc((size_t)aligned * (size_t)height);
            if (padded == NULL) {
                free(pixels);
                fprintf(stderr, "error: out of memory padding image '%s'\n", path);
                return NULL;
            }
            for (y = 0; y < (uint32_t)height; ++y) {
                memcpy(padded + (size_t)y * aligned,
                       pixels + (size_t)y * stride, stride);
            }
        } else {
            padded = pixels;
            aligned = stride;
        }
        stride = aligned;
    }

    memset(&texture_desc, 0, sizeof(texture_desc));
    texture_desc.label = sv("r2-image");
    texture_desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    texture_desc.dimension = WGPUTextureDimension_2D;
    texture_desc.size.width = (uint32_t)width;
    texture_desc.size.height = (uint32_t)height;
    texture_desc.size.depthOrArrayLayers = 1;
    texture_desc.format = WGPUTextureFormat_RGBA8Unorm;
    texture_desc.mipLevelCount = 1;
    texture_desc.sampleCount = 1;
    entry = &gpu->images[gpu->image_count];
    memset(entry, 0, sizeof(*entry));
    entry->texture = wgpuDeviceCreateTexture(gpu->device, &texture_desc);
    if (entry->texture == NULL) {
        if (padded != pixels) free(padded);
        free(pixels);
        fprintf(stderr, "error: image texture creation failed for '%s'\n", path);
        return NULL;
    }
    entry->view = wgpuTextureCreateView(entry->texture, NULL);
    if (entry->view == NULL) {
        wgpuTextureRelease(entry->texture);
        if (padded != pixels) free(padded);
        free(pixels);
        fprintf(stderr, "error: image texture view creation failed for '%s'\n", path);
        return NULL;
    }

    memset(&destination, 0, sizeof(destination));
    destination.texture = entry->texture;
    destination.mipLevel = 0;
    destination.aspect = WGPUTextureAspect_All;
    memset(&layout, 0, sizeof(layout));
    layout.offset = 0;
    layout.bytesPerRow = stride;
    layout.rowsPerImage = (uint32_t)height;
    memset(&extent, 0, sizeof(extent));
    extent.width = (uint32_t)width;
    extent.height = (uint32_t)height;
    extent.depthOrArrayLayers = 1;
    wgpuQueueWriteTexture(gpu->queue, &destination, padded,
                          (size_t)stride * (size_t)height, &layout, &extent);
    if (padded != pixels) {
        free(padded);
    }
    free(pixels);

    memset(bind_entries, 0, sizeof(bind_entries));
    bind_entries[0].binding = 0;
    bind_entries[0].textureView = entry->view;
    bind_entries[1].binding = 1;
    bind_entries[1].sampler = gpu->sampler;
    memset(&bind_desc, 0, sizeof(bind_desc));
    bind_desc.label = sv("r2-image-bind-group");
    bind_desc.layout = gpu->text_bgl;
    bind_desc.entryCount = 2;
    bind_desc.entries = bind_entries;
    entry->bind = wgpuDeviceCreateBindGroup(gpu->device, &bind_desc);
    if (entry->bind == NULL) {
        wgpuTextureViewRelease(entry->view);
        wgpuTextureRelease(entry->texture);
        fprintf(stderr, "error: image bind group creation failed for '%s'\n", path);
        return NULL;
    }
    entry->path = wn_strdup_(path);
    if (entry->path == NULL) {
        wgpuBindGroupRelease(entry->bind);
        wgpuTextureViewRelease(entry->view);
        wgpuTextureRelease(entry->texture);
        return NULL;
    }
    entry->width = width;
    entry->height = height;
    ++gpu->image_count;
    return entry;
}

static int set_scissor(WGPURenderPassEncoder pass, float x, float y, float w, float h,
                       int view_w, int view_h)
{
    int left = (int)floorf(x);
    int top = (int)floorf(y);
    int right = (int)ceilf(x + w);
    int bottom = (int)ceilf(y + h);
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > view_w) right = view_w;
    if (bottom > view_h) bottom = view_h;
    if (right <= left || bottom <= top) {
        return 0;
    }
    wgpuRenderPassEncoderSetScissorRect(pass, (uint32_t)left, (uint32_t)top,
                                        (uint32_t)(right - left),
                                        (uint32_t)(bottom - top));
    return 1;
}

static void gpu_draw(Gpu *gpu, WGPUCommandEncoder encoder, WGPUTextureView view,
                     const float clear[4])
{
    WGPURenderPassColorAttachment attachment;
    WGPURenderPassDescriptor descriptor;
    WGPURenderPassEncoder pass;
    memset(&attachment, 0, sizeof(attachment));
    attachment.view = view;
    attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    attachment.loadOp = WGPULoadOp_Clear;
    attachment.storeOp = WGPUStoreOp_Store;
    attachment.clearValue.r = clear[0];
    attachment.clearValue.g = clear[1];
    attachment.clearValue.b = clear[2];
    attachment.clearValue.a = clear[3];

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.label = sv("r2-pass");
    descriptor.colorAttachmentCount = 1;
    descriptor.colorAttachments = &attachment;

    pass = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
    if (pass == NULL) {
        return;
    }
    {
        const wn_display_list *list = wn_core_display_list(g_core);
        if (g_vertex_count > 0) {
            int b;
            wgpuRenderPassEncoderSetPipeline(pass, gpu->pipeline);
            wgpuRenderPassEncoderSetVertexBuffer(pass, 0, gpu->vertex_buffer, 0,
                                                 WGPU_WHOLE_SIZE);
            if (list != NULL && list->solid_batch_count > 0) {
                for (b = 0; b < list->solid_batch_count; ++b) {
                    const wn_draw_batch *batch = &list->solid_batches[b];
                    if (set_scissor(pass, batch->clip_x, batch->clip_y,
                                    batch->clip_w, batch->clip_h,
                                    gpu->image_width_hint, gpu->image_height_hint)) {
                        wgpuRenderPassEncoderDraw(pass, (uint32_t)batch->count, 1,
                                                  (uint32_t)batch->first, 0);
                    }
                }
            } else {
                wgpuRenderPassEncoderDraw(pass, (uint32_t)g_vertex_count, 1, 0, 0);
            }
        }
        if (g_image_vertex_count > 0 && gpu->image_pipeline != NULL && list != NULL) {
            int b;
            wgpuRenderPassEncoderSetPipeline(pass, gpu->image_pipeline);
            wgpuRenderPassEncoderSetVertexBuffer(pass, 0, gpu->image_buffer, 0,
                                                 WGPU_WHOLE_SIZE);
            for (b = 0; b < list->image_batch_count; ++b) {
                const wn_image_batch *batch = &list->image_batches[b];
                R2ImageTexture *texture = batch->src != NULL
                                              ? ensure_image_texture(gpu, batch->src)
                                              : NULL;
                if (texture == NULL || texture->bind == NULL) {
                    continue;
                }
                if (!set_scissor(pass, batch->clip_x, batch->clip_y,
                                 batch->clip_w, batch->clip_h,
                                 gpu->image_width_hint, gpu->image_height_hint)) {
                    continue;
                }
                wgpuRenderPassEncoderSetBindGroup(pass, 0, texture->bind, 0, NULL);
                wgpuRenderPassEncoderDraw(pass, (uint32_t)batch->count, 1,
                                          (uint32_t)batch->first, 0);
            }
        }
        if (g_glyph_vertex_count > 0 && gpu->text_pipeline != NULL && gpu->text_bg != NULL) {
            int b;
            wgpuRenderPassEncoderSetPipeline(pass, gpu->text_pipeline);
            wgpuRenderPassEncoderSetVertexBuffer(pass, 0, gpu->glyph_buffer, 0,
                                                 WGPU_WHOLE_SIZE);
            wgpuRenderPassEncoderSetBindGroup(pass, 0, gpu->text_bg, 0, NULL);
            if (list != NULL && list->glyph_batch_count > 0) {
                for (b = 0; b < list->glyph_batch_count; ++b) {
                    const wn_draw_batch *batch = &list->glyph_batches[b];
                    if (set_scissor(pass, batch->clip_x, batch->clip_y,
                                    batch->clip_w, batch->clip_h,
                                    gpu->image_width_hint, gpu->image_height_hint)) {
                        wgpuRenderPassEncoderDraw(pass, (uint32_t)batch->count, 1,
                                                  (uint32_t)batch->first, 0);
                    }
                }
            } else {
                wgpuRenderPassEncoderDraw(pass, (uint32_t)g_glyph_vertex_count, 1, 0, 0);
            }
        }
    }
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
}

/* -------------------------------------------------------------- WIC -> PNG */

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
    if (FAILED(hr)) {
        return report_hr(hr, "CoCreateInstance(WICImagingFactory)");
    }

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

/* ----------------------------------------------------------- offscreen PNG */

static double g_last_pass_ms = 0.0;
static double g_last_readback_ms = 0.0;
static double g_last_png_ms = 0.0;

static int g_wheel_active = 0;
static float g_wheel_x = 0.0f;
static float g_wheel_y = 0.0f;
static float g_wheel_dy = 0.0f;

static void apply_pending_wheel(void)
{
    int container;
    if (!g_wheel_active) {
        return;
    }
    g_wheel_active = 0;
    container = wn_core_wheel(g_core, g_wheel_x, g_wheel_y, 0.0f, g_wheel_dy);
    if (container >= 0) {
        printf("wheel: (%g,%g) delta_y=%g -> node=%d %s scroll=%.1f/%.1f\n",
               g_wheel_x, g_wheel_y, g_wheel_dy, container, kUiNodeNames[container],
               wn_core_scroll_offset(g_core, container),
               wn_core_scroll_max(g_core, container));
    } else {
        printf("wheel: (%g,%g) delta_y=%g -> no scrollable container\n",
               g_wheel_x, g_wheel_y, g_wheel_dy);
    }
    fflush(stdout);
}

static int render_offscreen(Gpu *gpu, int width, int height, const wchar_t *path,
                            double *layout_ms_out, double *frame_ms_out)
{
    WGPUTexture texture = NULL;
    WGPUTextureView view = NULL;
    WGPUBuffer readback = NULL;
    WGPUCommandEncoder encoder = NULL;
    WGPUCommandBuffer commands = NULL;
    MapResult map_result;
    const unsigned char *mapped = NULL;
    unsigned int bytes_per_row;
    uint64_t readback_size;
    float clear[4] = {0.043137f, 0.054902f, 0.078431f, 1.0f};
    int exit_code = 1;
    double t0, t1;

    t0 = now_ms();
    layout_page((float)width, (float)height);
    t1 = now_ms();
    if (layout_ms_out) *layout_ms_out = t1 - t0;
    apply_pending_wheel();
    build_geometry((float)width, (float)height);
    if (gpu_upload_geometry(gpu) != 0) {
        goto cleanup;
    }
    gpu->image_width_hint = width;
    gpu->image_height_hint = height;
    {
        const wn_display_list *list = wn_core_display_list(g_core);
        if (list != NULL) {
            clear[0] = list->clear_color[0];
            clear[1] = list->clear_color[1];
            clear[2] = list->clear_color[2];
            clear[3] = list->clear_color[3];
        }
    }

    t0 = now_ms();
    {
        WGPUTextureDescriptor descriptor;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("r2-offscreen-target");
        descriptor.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
        descriptor.dimension = WGPUTextureDimension_2D;
        descriptor.size.width = (uint32_t)width;
        descriptor.size.height = (uint32_t)height;
        descriptor.size.depthOrArrayLayers = 1;
        descriptor.format = gpu->format;
        descriptor.mipLevelCount = 1;
        descriptor.sampleCount = 1;
        texture = wgpuDeviceCreateTexture(gpu->device, &descriptor);
        if (texture == NULL) {
            fprintf(stderr, "error: offscreen texture creation failed\n");
            goto cleanup;
        }
        view = wgpuTextureCreateView(texture, NULL);
        if (view == NULL) {
            fprintf(stderr, "error: offscreen texture view failed\n");
            goto cleanup;
        }
    }

    bytes_per_row = ((unsigned int)width * 4u + 255u) & ~255u;
    readback_size = (uint64_t)bytes_per_row * (unsigned int)height;
    {
        WGPUBufferDescriptor descriptor;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("r2-readback");
        descriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
        descriptor.size = readback_size;
        readback = wgpuDeviceCreateBuffer(gpu->device, &descriptor);
        if (readback == NULL) {
            fprintf(stderr, "error: readback buffer creation failed\n");
            goto cleanup;
        }
    }

    encoder = wgpuDeviceCreateCommandEncoder(gpu->device, NULL);
    if (encoder == NULL) {
        fprintf(stderr, "error: command encoder creation failed\n");
        goto cleanup;
    }
    gpu_draw(gpu, encoder, view, clear);
    {
        WGPUTexelCopyTextureInfo source;
        WGPUTexelCopyBufferInfo destination;
        WGPUExtent3D extent;
        memset(&source, 0, sizeof(source));
        memset(&destination, 0, sizeof(destination));
        memset(&extent, 0, sizeof(extent));
        source.texture = texture;
        source.mipLevel = 0;
        source.aspect = WGPUTextureAspect_All;
        destination.layout.offset = 0;
        destination.layout.bytesPerRow = bytes_per_row;
        destination.layout.rowsPerImage = (uint32_t)height;
        destination.buffer = readback;
        extent.width = (uint32_t)width;
        extent.height = (uint32_t)height;
        extent.depthOrArrayLayers = 1;
        wgpuCommandEncoderCopyTextureToBuffer(encoder, &source, &destination, &extent);
    }
    commands = wgpuCommandEncoderFinish(encoder, NULL);
    if (commands == NULL) {
        fprintf(stderr, "error: command encoder finish failed\n");
        goto cleanup;
    }
    wgpuQueueSubmit(gpu->queue, 1, &commands);
    wgpuCommandEncoderRelease(encoder);
    encoder = NULL;
    g_last_pass_ms = now_ms() - t0;

    memset(&map_result, 0, sizeof(map_result));
    {
        WGPUBufferMapCallbackInfo cb;
        memset(&cb, 0, sizeof(cb));
        cb.mode = WGPUCallbackMode_AllowProcessEvents;
        cb.callback = on_map;
        cb.userdata1 = &map_result;
        (void)wgpuBufferMapAsync(readback, WGPUMapMode_Read, 0, readback_size, cb);
        if (wait_flag(gpu->instance, &map_result.done, "map readback") != 0) {
            goto cleanup;
        }
    }
    if (map_result.status != WGPUMapAsyncStatus_Success) {
        fprintf(stderr, "error: buffer map failed (status %d): %s\n",
                (int)map_result.status, map_result.message);
        goto cleanup;
    }
    mapped = (const unsigned char *)wgpuBufferGetConstMappedRange(readback, 0, readback_size);
    if (mapped == NULL) {
        fprintf(stderr, "error: mapped range is NULL\n");
        goto cleanup;
    }

    double t_after_map = now_ms();
    g_last_readback_ms = t_after_map - (t0 + g_last_pass_ms);

    if (write_png_rgba(mapped, (unsigned)width, (unsigned)height, bytes_per_row, path) != 0) {
        goto cleanup;
    }
    wgpuBufferUnmap(readback);
    t1 = now_ms();
    g_last_png_ms = t1 - t_after_map;
    if (frame_ms_out) *frame_ms_out = t1 - t0;
    exit_code = 0;

cleanup:
    if (commands != NULL) wgpuCommandBufferRelease(commands);
    if (encoder != NULL) wgpuCommandEncoderRelease(encoder);
    if (readback != NULL) wgpuBufferRelease(readback);
    if (view != NULL) wgpuTextureViewRelease(view);
    if (texture != NULL) wgpuTextureRelease(texture);
    return exit_code;
}

/* --------------------------------------------------------------- reporting */

static void json_escape(FILE *f, const char *text)
{
    fputc('"', f);
    for (const char *p = text; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            fputc('\\', f);
            fputc(c, f);
        } else if (c < 0x20) {
            fprintf(f, "\\u%04x", c);
        } else {
            fputc(c, f);
        }
    }
    fputc('"', f);
}

static const char *kind_name(int kind)
{
    switch (kind) {
    case R2_NODE_FLEX: return "flex";
    case R2_NODE_TEXT: return "text";
    case R2_NODE_GRID: return "grid";
    case R2_NODE_IMAGE: return "image";
    default: return "block";
    }
}

static int parse_size(const char *text, int *w, int *h)
{
    int a = 0, b = 0;
    if (sscanf(text, "%dx%d", &a, &b) != 2) {
        return 1;
    }
    if (a <= 0 || b <= 0 || a > 16384 || b > 16384) {
        return 1;
    }
    *w = a;
    *h = b;
    return 0;
}

static int write_layout_report(const char *path, const int *widths, const int *heights,
                               int count)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "error: cannot open report %s\n", path);
        return 1;
    }
    fprintf(f, "{\n  \"generated_by\": \"runtime_r2.c\",\n  \"node_count\": %d,\n", UI_NODE_COUNT);
    fprintf(f, "  \"sizes\": [\n");
    for (int s = 0; s < count; ++s) {
        const int w = widths[s], h = heights[s];
        const int iterations = 100;
        double best = 1e9, total = 0.0;
        for (int it = 0; it < iterations; ++it) {
            double t0 = now_ms();
            layout_page((float)w, (float)h);
            double dt = now_ms() - t0;
            total += dt;
            if (dt < best) best = dt;
        }
        layout_page((float)w, (float)h);
        fprintf(f, "    {\n");
        fprintf(f, "      \"width\": %d,\n      \"height\": %d,\n", w, h);
        fprintf(f, "      \"layout_ms_best\": %.4f,\n      \"layout_ms_avg\": %.4f,\n",
                best, total / (double)iterations);
        fprintf(f, "      \"nodes\": [\n");
        for (int i = 0; i < UI_NODE_COUNT; ++i) {
            const Rect *r = &g_rects[i];
            fprintf(f, "        {\"id\": %d, \"name\": ", i);
            json_escape(f, kUiNodeNames[i]);
            fprintf(f, ", \"kind\": \"%s\", \"x\": %.2f, \"y\": %.2f, \"w\": %.2f, \"h\": %.2f}%s\n",
                    kind_name(kUiNodes[i].kind), r->x, r->y, r->w, r->h,
                    i + 1 < UI_NODE_COUNT ? "," : "");
        }
        fprintf(f, "      ]\n    }%s\n", s + 1 < count ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    printf("report: wrote %s (%d size(s), %d nodes each)\n", path, count, UI_NODE_COUNT);
    return 0;
}

static int name_is(const char *name, const char *exact)
{
    return strcmp(name, exact) == 0;
}

static int is_card_node(const char *name)
{
    const char *tag = "/div.cards/div.card";
    const char *at = strstr(name, tag);
    if (at == NULL) {
        return 0;
    }
    const char *tail = at + strlen(tag);
    if (*tail == '[') {
        ++tail;
        while (*tail >= '0' && *tail <= '9') {
            ++tail;
        }
        if (*tail == ']') {
            ++tail;
        }
    }
    return *tail == '\0';
}

static void print_layout_summary(float w, float h)
{
    int sidebar = -1, content = -1, header = -1, cards[8], card_count = 0;
    for (int i = 0; i < UI_NODE_COUNT; ++i) {
        const char *name = kUiNodeNames[i];
        if (name_is(name, "body.root/div.sidebar")) sidebar = i;
        else if (name_is(name, "body.root/div.content")) content = i;
        else if (name_is(name, "body.root/div.content/div.header")) header = i;
        else if (is_card_node(name) && card_count < 8) {
            cards[card_count++] = i;
        }
    }
    printf("layout %gx%g:", w, h);
    if (sidebar >= 0) printf(" sidebar=[%.1f,%.1f,%.1f,%.1f]",
                             g_rects[sidebar].x, g_rects[sidebar].y,
                             g_rects[sidebar].w, g_rects[sidebar].h);
    if (content >= 0) printf(" content=[%.1f,%.1f,%.1f,%.1f]",
                             g_rects[content].x, g_rects[content].y,
                             g_rects[content].w, g_rects[content].h);
    if (header >= 0) printf(" header_w=%.1f", g_rects[header].w);
    printf(" cards_y=");
    for (int i = 0; i < card_count; ++i) {
        printf("%s%.1f", i ? "," : "", g_rects[cards[i]].y);
    }
    printf(" cards_w=");
    for (int i = 0; i < card_count; ++i) {
        printf("%s%.1f", i ? "," : "", g_rects[cards[i]].w);
    }
    printf("\n");
}

/* One line per scroll container, machine-readable for the pilot verifier. */
static void print_scroll_summary(void)
{
    for (int i = 0; i < UI_NODE_COUNT; ++i) {
        if (kUiNodes[i].overflow != R2_OVERFLOW_SCROLL) {
            continue;
        }
        printf("scroll: node=%d name=%s rect=[%.1f,%.1f,%.1f,%.1f] "
               "offset=%.2f max=%.2f\n",
               i, kUiNodeNames[i], g_rects[i].x, g_rects[i].y,
               g_rects[i].w, g_rects[i].h,
               wn_core_scroll_offset(g_core, i),
               wn_core_scroll_max(g_core, i));
    }
}

/* ------------------------------------------------------------------ states */

static int first_interactive_node(void)
{
    for (int i = 0; i < UI_NODE_COUNT; ++i) {
        if (kUiNodes[i].action != ACTION_NONE) {
            return i;
        }
    }
    return -1;
}

static int first_input_node(void)
{
    for (int i = 0; i < UI_NODE_COUNT; ++i) {
        if (kUiNodes[i].kind == R2_NODE_INPUT) {
            return i;
        }
    }
    return -1;
}

static int run_states(const char *prefix, int node, int width, int height)
{
    Gpu gpu;
    char path[MAX_PATH];
    double layout_ms = 0.0, frame_ms = 0.0;
    const char *suffixes[4] = {"normal", "hover", "press", "selected"};

    if (gpu_init(&gpu, NULL, 0, WGPUTextureFormat_RGBA8Unorm, "r2-states-device") != 0) {
        gpu_release(&gpu);
        return 1;
    }
    if (node < 0 || node >= UI_NODE_COUNT || kUiNodes[node].action == ACTION_NONE) {
        fprintf(stderr, "error: node %d is not interactive\n", node);
        gpu_release(&gpu);
        return 1;
    }

    wn_core_set_state(g_core, 0, WN_STATE_HOVER, 0);
    wn_core_set_state(g_core, 0, WN_STATE_PRESS, 0);
    wn_core_set_state(g_core, 0, WN_STATE_SELECTED, 0);
    for (int state = 0; state < 4; ++state) {
        wn_core_set_state(g_core, node, WN_STATE_HOVER, state == 1);
        wn_core_set_state(g_core, node, WN_STATE_PRESS, state == 2);
        wn_core_set_state(g_core, node, WN_STATE_SELECTED, state == 3);
        snprintf(path, sizeof(path), "%s_%s.png", prefix, suffixes[state]);
        wchar_t wide[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
        if (render_offscreen(&gpu, width, height, wide, &layout_ms, &frame_ms) != 0) {
            gpu_release(&gpu);
            return 1;
        }
        printf("state %-8s node=%d %-40s layout=%.3fms frame=%.3fms -> %s\n",
               suffixes[state], node, kUiNodeNames[node], layout_ms, frame_ms, path);
    }

    /* Small machine-readable state report for verify.py. */
    snprintf(path, sizeof(path), "%s_states.json", prefix);
    FILE *f = fopen(path, "wb");
    if (f != NULL) {
        const Rect *r = &g_rects[node];
        fprintf(f, "{\n  \"node\": %d,\n  \"name\": ", node);
        json_escape(f, kUiNodeNames[node]);
        fprintf(f, ",\n  \"rect\": {\"x\": %.2f, \"y\": %.2f, \"w\": %.2f, \"h\": %.2f},\n",
                r->x, r->y, r->w, r->h);
        fprintf(f, "  \"colors\": {\n");
        fprintf(f, "    \"normal\": \"#%06X\",\n", kUiNodes[node].background & 0xFFFFFFu);
        fprintf(f, "    \"hover\": \"#%06X\",\n", kUiNodes[node].hover_bg & 0xFFFFFFu);
        fprintf(f, "    \"press\": \"#%06X\",\n", kUiNodes[node].press_bg & 0xFFFFFFu);
        fprintf(f, "    \"selected\": \"#%06X\"\n", kUiNodes[node].selected_bg & 0xFFFFFFu);
        fprintf(f, "  },\n  \"pngs\": [");
        for (int state = 0; state < 4; ++state) {
            snprintf(path, sizeof(path), "%s_%s.png", prefix, suffixes[state]);
            fprintf(f, "%s\n    ", state ? "," : "");
            json_escape(f, path);
        }
        fprintf(f, "\n  ]\n}\n");
        fclose(f);
        printf("states: wrote %s_states.json\n", prefix);
    }

    gpu_release(&gpu);
    return 0;
}

/* ------------------------------------------------------------------ window */

#define WINDOW_CLASS L"WebNativeR2Window"
#define WINDOW_TITLE L"webnative R2 core - runtime layout + wgpu-native"

static volatile LONG g_running = 1;
static int g_window_width = 960;
static int g_window_height = 640;
static int g_need_layout = 1;
static int g_need_geometry = 1;
static int g_need_configure = 1;
static int g_max_frames = 0;
static int g_resize_demo = 0;
static int g_input_demo = 0;
static int g_frame_index = 0;
static double g_last_layout_ms = 0.0;
static double g_cpu_frame_total = 0.0;
static int g_demo_stage = 0;

static int find_node_contains(const char *fragment)
{
    for (int i = 0; i < UI_NODE_COUNT; ++i) {
        if (strstr(kUiNodeNames[i], fragment)) {
            return i;
        }
    }
    return -1;
}

static void log_reflow(const char *why)
{
    double t0 = now_ms();
    layout_page((float)g_window_width, (float)g_window_height);
    g_last_layout_ms = now_ms() - t0;
    build_geometry((float)g_window_width, (float)g_window_height);
    g_need_layout = 0;
    g_need_geometry = 0;
    int c0 = find_node_contains("/div.card");
    int c2 = find_node_contains("/div.card[2]");
    printf("%s: %dx%d layout %.3fms", why, g_window_width, g_window_height, g_last_layout_ms);
    if (c0 >= 0 && c2 >= 0) {
        printf(" card0_y=%.1f card2_y=%.1f", g_rects[c0].y, g_rects[c2].y);
    }
    printf(" vertices=%d glyph_vertices=%d image_vertices=%d\n",
           g_vertex_count, g_glyph_vertex_count, g_image_vertex_count);
    fflush(stdout);
}

static void set_client_size(HWND hwnd, int client_w, int client_h)
{
    RECT rect = {0, 0, client_w, client_h};
    DWORD style = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
    DWORD exstyle = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    AdjustWindowRectEx(&rect, style, FALSE, exstyle);
    SetWindowPos(hwnd, NULL, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* ------------------------------------------------ window text input (v4) */

static uint32_t input_modifiers(void)
{
    uint32_t mods = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) mods |= WN_KEY_MOD_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= 0x02u;
    if (GetKeyState(VK_MENU) & 0x8000) mods |= 0x04u;
    return mods;
}

static void r2_clipboard_set(const char *text)
{
    int length;
    HGLOBAL memory;
    wchar_t *wide;
    if (text == NULL || !OpenClipboard(NULL)) {
        return;
    }
    EmptyClipboard();
    length = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (length > 0) {
        memory = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)length * sizeof(wchar_t));
        if (memory != NULL) {
            wide = (wchar_t *)GlobalLock(memory);
            if (wide != NULL) {
                MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, length);
                GlobalUnlock(memory);
                if (SetClipboardData(CF_UNICODETEXT, memory) == NULL) {
                    GlobalFree(memory);
                }
            } else {
                GlobalFree(memory);
            }
        }
    }
    CloseClipboard();
}

static int r2_clipboard_get(char *out, int capacity)
{
    int copied = 0;
    if (out == NULL || capacity <= 0) {
        return 0;
    }
    out[0] = '\0';
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(NULL)) {
        return 0;
    }
    {
        HANDLE handle = GetClipboardData(CF_UNICODETEXT);
        if (handle != NULL) {
            const wchar_t *wide = (const wchar_t *)GlobalLock(handle);
            if (wide != NULL) {
                int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
                if (bytes > 1) {
                    char *utf8 = (char *)malloc((size_t)bytes);
                    if (utf8 != NULL) {
                        if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, bytes,
                                                NULL, NULL) > 0) {
                            copied = (int)strlen(utf8);
                            if (copied >= capacity) {
                                copied = capacity - 1;
                            }
                            memcpy(out, utf8, (size_t)copied);
                            out[copied] = '\0';
                        }
                        free(utf8);
                    }
                }
                GlobalUnlock(handle);
            }
        }
    }
    CloseClipboard();
    return copied;
}

static void log_input_value(const char *why, int node)
{
    const char *value = wn_core_input_value(g_core, node);
    printf("input: %s node=%d value=\"%s\"\n", why, node, value != NULL ? value : "");
    fflush(stdout);
}

/* Returns 1 when the key was consumed. */
static int window_input_key(int key)
{
    char buffer[256];
    uint32_t mods = input_modifiers();
    int node = wn_core_input_focused(g_core);
    int value_changed = 0;
    int visual_changed = 0;
    if (node < 0) {
        return 0;
    }
    if (mods & 0x02u) {
        if (key == WN_KEY_A) {
            wn_core_input_select_all(g_core, &visual_changed);
        } else if (key == WN_KEY_C) {
            int copied = wn_core_input_copy(g_core, buffer, (int)sizeof(buffer));
            if (copied > 0) r2_clipboard_set(buffer);
            log_input_value("copy", node);
        } else if (key == WN_KEY_X) {
            int copied = wn_core_input_cut(g_core, buffer, (int)sizeof(buffer),
                                           &value_changed, &visual_changed);
            if (copied > 0) r2_clipboard_set(buffer);
            if (value_changed) log_input_value("cut", node);
        } else if (key == WN_KEY_V) {
            if (r2_clipboard_get(buffer, (int)sizeof(buffer)) > 0) {
                wn_core_input_paste(g_core, buffer, &value_changed, &visual_changed);
            }
            if (value_changed) log_input_value("paste", node);
        } else {
            return 0;
        }
    } else {
        if (wn_core_input_key(g_core, key, mods, &value_changed, &visual_changed) < 0) {
            return 0;
        }
        if (value_changed) log_input_value("key", node);
    }
    if (visual_changed || value_changed) {
        g_need_geometry = 1;
    }
    return 1;
}

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
        g_window_width = LOWORD(lparam);
        g_window_height = HIWORD(lparam);
        g_need_layout = 1;
        g_need_configure = 1;
        return 0;
    case WM_KEYDOWN:
        if (window_input_key((int)wparam)) {
            return 0;
        }
        if (wparam == VK_ESCAPE && wn_core_input_focused(g_core) < 0) {
            g_running = 0;
            return 0;
        }
        return 0;
    case WM_CHAR: {
        int value_changed = 0;
        int visual_changed = 0;
        if (wn_core_input_char(g_core, (uint32_t)wparam,
                               &value_changed, &visual_changed) >= 0) {
            if (value_changed) {
                log_input_value("char", wn_core_input_focused(g_core));
            }
            if (value_changed || visual_changed) {
                g_need_geometry = 1;
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = (int)(short)LOWORD(lparam);
        int y = (int)(short)HIWORD(lparam);
        int before = g_hover;
        int hit = wn_core_pointer_move(g_core, (float)x, (float)y);
        if (hit != before) {
            g_need_geometry = 1;
        }
        TRACKMOUSEEVENT track;
        memset(&track, 0, sizeof(track));
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = hwnd;
        TrackMouseEvent(&track);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_hover != -1) {
            wn_core_pointer_leave(g_core);
            g_need_geometry = 1;
        }
        return 0;
    case WM_MOUSEWHEEL: {
        POINT point;
        int notches;
        int container;
        point.x = (int)(short)LOWORD(lparam);
        point.y = (int)(short)HIWORD(lparam);
        ScreenToClient(hwnd, &point);
        notches = (int)GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
        if (notches == 0) {
            notches = GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? 1 : -1;
        }
        container = wn_core_wheel(g_core, (float)point.x, (float)point.y,
                                  0.0f, (float)(-notches * 120));
        if (container >= 0) {
            g_need_geometry = 1;
            printf("wheel: (%ld,%ld) notches=%d -> node=%d %s scroll=%.1f/%.1f\n",
                   point.x, point.y, notches, container, kUiNodeNames[container],
                   wn_core_scroll_offset(g_core, container),
                   wn_core_scroll_max(g_core, container));
            fflush(stdout);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int x = (int)(short)LOWORD(lparam);
        int y = (int)(short)HIWORD(lparam);
        wn_core_pointer_down(g_core, (float)x, (float)y);
        SetCapture(hwnd);
        g_need_geometry = 1;
        return 0;
    }
    case WM_LBUTTONUP: {
        int x = (int)(short)LOWORD(lparam);
        int y = (int)(short)HIWORD(lparam);
        int press = g_press;
        int hit = wn_core_pointer_up(g_core, (float)x, (float)y);
        if (press >= 0 && hit == press && kUiNodes[press].action == ACTION_SELECT) {
            printf("click: node=%d %s -> selected\n", press, kUiNodeNames[press]);
            fflush(stdout);
        }
        ReleaseCapture();
        g_need_geometry = 1;
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static int run_window(void)
{
    Gpu gpu;
    HWND hwnd = NULL;
    WGPUSurface surface = NULL;
    WGPUSurfaceConfiguration surface_config;
    int exit_code = 1;
    int frames = 0;
    double t_start, t_ready;
    WGPUAdapterInfo info;

    memset(&gpu, 0, sizeof(gpu));

    {
        HINSTANCE hinst = GetModuleHandleW(NULL);
        WNDCLASSEXW wc;
        DWORD style = WS_OVERLAPPEDWINDOW;
        RECT rect = {0, 0, g_window_width, g_window_height};

        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = hinst;
        wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = WINDOW_CLASS;
        if (!RegisterClassExW(&wc)) {
            fprintf(stderr, "error: RegisterClassExW failed\n");
            return 1;
        }
        AdjustWindowRect(&rect, style, FALSE);
        hwnd = CreateWindowExW(0, WINDOW_CLASS, WINDOW_TITLE, style,
                               CW_USEDEFAULT, CW_USEDEFAULT,
                               rect.right - rect.left, rect.bottom - rect.top,
                               NULL, NULL, hinst, NULL);
        if (hwnd == NULL) {
            fprintf(stderr, "error: CreateWindowExW failed\n");
            return 1;
        }
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }

    t_start = now_ms();

    {
        /* Window path: instance -> surface -> adapter -> device, because the
         * surface must be created from the instance that stays alive. */
        WGPUInstance instance = wgpuCreateInstance(NULL);
        WGPUSurfaceSourceWindowsHWND source;
        WGPUSurfaceDescriptor descriptor;
        AdapterResult adapter_result;
        DeviceResult device_result;
        WGPUShaderModule shader;
        WGPUBufferDescriptor buffer_desc;

        if (instance == NULL) {
            fprintf(stderr, "error: wgpuCreateInstance failed\n");
            goto cleanup;
        }
        gpu.instance = instance;

        memset(&source, 0, sizeof(source));
        source.chain.sType = WGPUSType_SurfaceSourceWindowsHWND;
        source.hinstance = GetModuleHandleW(NULL);
        source.hwnd = hwnd;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("r2-surface");
        descriptor.nextInChain = &source.chain;
        surface = wgpuInstanceCreateSurface(instance, &descriptor);
        if (surface == NULL) {
            fprintf(stderr, "error: wgpuInstanceCreateSurface failed\n");
            goto cleanup;
        }

        memset(&adapter_result, 0, sizeof(adapter_result));
        {
            WGPURequestAdapterOptions options;
            WGPURequestAdapterCallbackInfo cb;
            memset(&options, 0, sizeof(options));
            memset(&cb, 0, sizeof(cb));
            options.powerPreference = WGPUPowerPreference_HighPerformance;
            options.featureLevel = WGPUFeatureLevel_Core;
            options.compatibleSurface = surface;
            cb.mode = WGPUCallbackMode_AllowProcessEvents;
            cb.callback = on_adapter;
            cb.userdata1 = &adapter_result;
            (void)wgpuInstanceRequestAdapter(instance, &options, cb);
            if (wait_flag(instance, &adapter_result.done, "request adapter") != 0) {
                goto cleanup;
            }
        }
        if (adapter_result.status != WGPURequestAdapterStatus_Success || adapter_result.adapter == NULL) {
            fprintf(stderr, "error: no adapter: %s\n", adapter_result.message);
            goto cleanup;
        }
        gpu.adapter = adapter_result.adapter;

        memset(&info, 0, sizeof(info));
        if (wgpuAdapterGetInfo(gpu.adapter, &info) == WGPUStatus_Success) {
            printf("adapter: backend=%s (%d) type=%s vendorID=0x%04X deviceID=0x%04X\n",
                   backend_name(info.backendType), (int)info.backendType,
                   adapter_type_name(info.adapterType), info.vendorID, info.deviceID);
            print_view("adapter: device=", info.device);
            printf("\n");
            print_view("adapter: description=", info.description);
            printf("\n");
            fflush(stdout);
        }

        memset(&device_result, 0, sizeof(device_result));
        {
            WGPUDeviceDescriptor device_desc;
            WGPURequestDeviceCallbackInfo cb;
            memset(&device_desc, 0, sizeof(device_desc));
            memset(&cb, 0, sizeof(cb));
            device_desc.label = sv("r2-window-device");
            device_desc.uncapturedErrorCallbackInfo.callback = on_uncaptured_error;
            cb.mode = WGPUCallbackMode_AllowProcessEvents;
            cb.callback = on_device;
            cb.userdata1 = &device_result;
            (void)wgpuAdapterRequestDevice(gpu.adapter, &device_desc, cb);
            if (wait_flag(instance, &device_result.done, "request device") != 0) {
                goto cleanup;
            }
        }
        if (device_result.status != WGPURequestDeviceStatus_Success || device_result.device == NULL) {
            fprintf(stderr, "error: no device: %s\n", device_result.message);
            goto cleanup;
        }
        gpu.device = device_result.device;
        gpu.queue = wgpuDeviceGetQueue(gpu.device);

        {
            WGPUSurfaceCapabilities caps;
            memset(&caps, 0, sizeof(caps));
            if (wgpuSurfaceGetCapabilities(surface, gpu.adapter, &caps) != WGPUStatus_Success ||
                caps.formatCount == 0) {
                fprintf(stderr, "error: surface capabilities failed\n");
                goto cleanup;
            }
            gpu.format = caps.formats[0];
            for (size_t i = 0; i < caps.formatCount; ++i) {
                if (caps.formats[i] == WGPUTextureFormat_BGRA8Unorm ||
                    caps.formats[i] == WGPUTextureFormat_RGBA8Unorm) {
                    gpu.format = caps.formats[i];
                    break;
                }
            }
            printf("surface: formats=%zu chosenFormat=%d\n", caps.formatCount, (int)gpu.format);
        }

        memset(&surface_config, 0, sizeof(surface_config));
        surface_config.device = gpu.device;
        surface_config.format = gpu.format;
        surface_config.usage = WGPUTextureUsage_RenderAttachment;
        surface_config.width = (uint32_t)g_window_width;
        surface_config.height = (uint32_t)g_window_height;
        surface_config.presentMode = WGPUPresentMode_Fifo;
        surface_config.alphaMode = WGPUCompositeAlphaMode_Auto;
        wgpuSurfaceConfigure(surface, &surface_config);
        g_need_configure = 0;

        {
            WGPUShaderModuleDescriptor shader_desc;
            WGPUShaderSourceWGSL wgsl;
            memset(&shader_desc, 0, sizeof(shader_desc));
            memset(&wgsl, 0, sizeof(wgsl));
            wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
            wgsl.code = sv(kShaderWGSL);
            shader_desc.label = sv("r2-window-shader");
            shader_desc.nextInChain = &wgsl.chain;
            shader = wgpuDeviceCreateShaderModule(gpu.device, &shader_desc);
            if (shader == NULL) {
                fprintf(stderr, "error: shader creation failed\n");
                goto cleanup;
            }
            gpu.shader = shader;
        }

        {
            WGPUVertexAttribute attributes[2];
            WGPUVertexBufferLayout buffer_layout;
            WGPUColorTargetState target;
            WGPUBlendState blend;
            WGPUFragmentState fragment;
            WGPUVertexState vertex;
            WGPUPrimitiveState primitive;
            WGPUMultisampleState multisample;
            WGPURenderPipelineDescriptor pipeline_desc;

            memset(attributes, 0, sizeof(attributes));
            attributes[0].format = WGPUVertexFormat_Float32x2;
            attributes[0].offset = 0;
            attributes[0].shaderLocation = 0;
            attributes[1].format = WGPUVertexFormat_Unorm8x4;
            attributes[1].offset = 8;
            attributes[1].shaderLocation = 1;

            memset(&buffer_layout, 0, sizeof(buffer_layout));
            buffer_layout.stepMode = WGPUVertexStepMode_Vertex;
            buffer_layout.arrayStride = sizeof(Vertex);
            buffer_layout.attributeCount = (uint32_t)ARRAY_LEN(attributes);
            buffer_layout.attributes = attributes;

            memset(&blend, 0, sizeof(blend));
            blend.color.operation = WGPUBlendOperation_Add;
            blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
            blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
            blend.alpha.operation = WGPUBlendOperation_Add;
            blend.alpha.srcFactor = WGPUBlendFactor_One;
            blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;

            memset(&target, 0, sizeof(target));
            target.format = gpu.format;
            target.blend = &blend;
            target.writeMask = WGPUColorWriteMask_All;

            memset(&fragment, 0, sizeof(fragment));
            fragment.module = gpu.shader;
            fragment.entryPoint = sv("fs_main");
            fragment.targetCount = 1;
            fragment.targets = &target;

            memset(&vertex, 0, sizeof(vertex));
            vertex.module = gpu.shader;
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

            memset(&pipeline_desc, 0, sizeof(pipeline_desc));
            pipeline_desc.label = sv("r2-window-pipeline");
            pipeline_desc.layout = NULL;
            pipeline_desc.vertex = vertex;
            pipeline_desc.primitive = primitive;
            pipeline_desc.depthStencil = NULL;
            pipeline_desc.multisample = multisample;
            pipeline_desc.fragment = &fragment;
            gpu.pipeline = wgpuDeviceCreateRenderPipeline(gpu.device, &pipeline_desc);
            if (gpu.pipeline == NULL) {
                fprintf(stderr, "error: pipeline creation failed\n");
                goto cleanup;
            }
        }

        gpu.vertex_capacity = (size_t)UI_NODE_COUNT * 6u * sizeof(Vertex);
        memset(&buffer_desc, 0, sizeof(buffer_desc));
        buffer_desc.label = sv("r2-window-vertices");
        buffer_desc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        buffer_desc.size = gpu.vertex_capacity;
        gpu.vertex_buffer = wgpuDeviceCreateBuffer(gpu.device, &buffer_desc);
        if (gpu.vertex_buffer == NULL) {
            fprintf(stderr, "error: vertex buffer creation failed\n");
            goto cleanup;
        }
        if (gpu_create_text(&gpu) != 0) {
            fprintf(stderr, "error: text pipeline creation failed\n");
            goto cleanup;
        }
        if (gpu_create_images(&gpu) != 0) {
            fprintf(stderr, "error: image pipeline creation failed\n");
            goto cleanup;
        }
    }

    t_ready = now_ms();

    while (g_running) {
        MSG msg;
        WGPUSurfaceTexture surface_texture;
        WGPUTextureView view;
        WGPUCommandEncoder encoder;
        WGPUCommandBuffer commands;
        const float clear[4] = {0.043137f, 0.054902f, 0.078431f, 1.0f};

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
        if (g_window_width <= 0 || g_window_height <= 0 || IsIconic(hwnd)) {
            Sleep(10);
            continue;
        }

        if (g_need_layout) {
            log_reflow("WM_SIZE/layout");
        } else if (g_need_geometry) {
            build_geometry((float)g_window_width, (float)g_window_height);
            g_need_geometry = 0;
        }
        if (g_need_configure) {
            surface_config.width = (uint32_t)g_window_width;
            surface_config.height = (uint32_t)g_window_height;
            wgpuSurfaceConfigure(surface, &surface_config);
            g_need_configure = 0;
            printf("surface: reconfigured %dx%d\n", g_window_width, g_window_height);
            fflush(stdout);
        }
        if (gpu_upload_geometry(&gpu) != 0) {
            fprintf(stderr, "error: geometry upload failed\n");
            break;
        }
        gpu.image_width_hint = g_window_width;
        gpu.image_height_hint = g_window_height;

        memset(&surface_texture, 0, sizeof(surface_texture));
        wgpuSurfaceGetCurrentTexture(surface, &surface_texture);
        if (surface_texture.status == WGPUSurfaceGetCurrentTextureStatus_Outdated ||
            surface_texture.status == WGPUSurfaceGetCurrentTextureStatus_Lost) {
            wgpuSurfaceConfigure(surface, &surface_config);
            continue;
        }
        if (surface_texture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
            surface_texture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
            if (surface_texture.status == WGPUSurfaceGetCurrentTextureStatus_Timeout) {
                Sleep(1);
                continue;
            }
            fprintf(stderr, "error: getCurrentTexture status %d\n", (int)surface_texture.status);
            break;
        }
        double t_cpu_frame = now_ms();
        view = wgpuTextureCreateView(surface_texture.texture, NULL);
        if (view == NULL) {
            wgpuTextureRelease(surface_texture.texture);
            break;
        }
        encoder = wgpuDeviceCreateCommandEncoder(gpu.device, NULL);
        gpu_draw(&gpu, encoder, view, clear);
        commands = wgpuCommandEncoderFinish(encoder, NULL);
        if (commands == NULL) {
            wgpuCommandEncoderRelease(encoder);
            wgpuTextureViewRelease(view);
            wgpuTextureRelease(surface_texture.texture);
            break;
        }
        wgpuQueueSubmit(gpu.queue, 1, &commands);
        wgpuCommandBufferRelease(commands);
        wgpuCommandEncoderRelease(encoder);
        g_cpu_frame_total += now_ms() - t_cpu_frame;
        (void)wgpuSurfacePresent(surface);
        wgpuTextureViewRelease(view);
        wgpuTextureRelease(surface_texture.texture);

        ++frames;
        ++g_frame_index;

        if (g_resize_demo) {
            if (g_demo_stage == 0 && g_frame_index >= 5) {
                g_demo_stage = 1;
                printf("resize-demo: frame %d -> client 560x640\n", g_frame_index);
                fflush(stdout);
                set_client_size(hwnd, 560, 640);
            } else if (g_demo_stage == 1 && g_frame_index >= 15) {
                g_demo_stage = 2;
                printf("resize-demo: frame %d -> client 960x640\n", g_frame_index);
                fflush(stdout);
                set_client_size(hwnd, 960, 640);
            }
        }

        /* Real Win32 input path: synthetic WM_MOUSEMOVE / LBUTTONDOWN / UP
         * are dispatched through the window procedure and hit testing. When
         * the fixture has no action node, the demo targets the first text
         * input and adds WM_CHAR / VK_BACK. */
        if (g_input_demo) {
            int node = first_interactive_node();
            int input_mode = 0;
            if (node < 0) {
                node = first_input_node();
                input_mode = node >= 0;
            }
            if (node >= 0) {
                int cx = (int)(g_rects[node].x + g_rects[node].w * 0.5f);
                int cy = (int)(g_rects[node].y + g_rects[node].h * 0.5f);
                LPARAM point = MAKELPARAM(cx, cy);
                if (g_frame_index == 8) {
                    SendMessageW(hwnd, WM_MOUSEMOVE, 0, point);
                    printf("input-demo: WM_MOUSEMOVE (%d,%d) -> hover=%d %s\n",
                           cx, cy, g_hover, g_hover >= 0 ? kUiNodeNames[g_hover] : "(none)");
                    fflush(stdout);
                } else if (g_frame_index == 9) {
                    SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point);
                    printf("input-demo: WM_LBUTTONDOWN -> press=%d %s\n",
                           g_press, g_press >= 0 ? kUiNodeNames[g_press] : "(none)");
                    fflush(stdout);
                } else if (g_frame_index == 10) {
                    SendMessageW(hwnd, WM_LBUTTONUP, 0, point);
                    printf("input-demo: WM_LBUTTONUP -> selected=%d %s\n",
                           g_selected, g_selected >= 0 ? kUiNodeNames[g_selected] : "(none)");
                    fflush(stdout);
                } else if (input_mode && g_frame_index == 12) {
                    SendMessageW(hwnd, WM_CHAR, (WPARAM)'a', 0);
                    printf("input-demo: WM_CHAR 'a'\n");
                    fflush(stdout);
                } else if (input_mode && g_frame_index == 13) {
                    SendMessageW(hwnd, WM_CHAR, (WPARAM)'b', 0);
                    printf("input-demo: WM_CHAR 'b'\n");
                    fflush(stdout);
                } else if (input_mode && g_frame_index == 14) {
                    SendMessageW(hwnd, WM_KEYDOWN, VK_BACK, 0);
                    printf("input-demo: WM_KEYDOWN VK_BACK\n");
                    fflush(stdout);
                } else if (input_mode && g_frame_index == 15) {
                    const char *value = wn_core_input_value(g_core, node);
                    printf("input-demo: value=\"%s\" focused=%d\n",
                           value != NULL ? value : "(null)",
                           wn_core_input_focused(g_core) == node ? 1 : 0);
                    fflush(stdout);
                } else if (!input_mode && g_frame_index == 12) {
                    SendMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(900, 600));
                    printf("input-demo: WM_MOUSEMOVE away -> hover=%d\n", g_hover);
                    fflush(stdout);
                }
            }
        }

        if (g_max_frames > 0 && frames >= g_max_frames) {
            break;
        }
    }

    printf("window: closed after %d frame(s), init %.2f ms, %.2f ms/frame average\n",
           frames, t_ready - t_start,
           frames > 0 ? (now_ms() - t_ready) / (double)frames : 0.0);
    printf("window: last layout %.3f ms at %dx%d, vertices=%d glyph_vertices=%d\n",
           g_last_layout_ms, g_window_width, g_window_height, g_vertex_count,
           g_glyph_vertex_count);
    printf("window: cpu frame %.3f ms average (layout excluded, present/vsync excluded)\n",
           frames > 0 ? g_cpu_frame_total / (double)frames : 0.0);
    fflush(stdout);
    exit_code = 0;

cleanup:
    if (surface != NULL) wgpuSurfaceRelease(surface);
    gpu_release(&gpu);
    if (hwnd != NULL) DestroyWindow(hwnd);
    return exit_code;
}

/* ------------------------------------------------------------ dynamic demo */

static int dynamic_rect_json(FILE *f, int node)
{
    const wn_rect *rects;
    wn_rect r;
    int hit = -1;
    int alive;
    if (g_core == NULL || node < 0) {
        fputs("null", f);
        return -1;
    }
    alive = wn_core_node_is_alive(g_core, node);
    rects = wn_core_rects(g_core, NULL);
    r = rects[node];
    if (alive && wn_core_node_is_visible(g_core, node) && r.w > 0.0f && r.h > 0.0f) {
        hit = wn_core_hit_test(g_core, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
    }
    fprintf(f, "{\"id\":%d,\"alive\":%d,\"visible\":%d,"
               "\"x\":%.2f,\"y\":%.2f,\"w\":%.2f,\"h\":%.2f,\"hit\":%d}",
            node, alive, wn_core_node_is_visible(g_core, node),
            r.x, r.y, r.w, r.h, hit);
    return alive;
}

static int dynamic_stage(Gpu *gpu, FILE *report, const char *prefix, int stage,
                         const char *action, int card0, int card1, int clone)
{
    char path[MAX_PATH];
    wchar_t wide[MAX_PATH];
    double layout_ms = 0.0;
    double frame_ms = 0.0;
    int node_count;

    snprintf(path, sizeof(path), "%s_%02d.png", prefix, stage);
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    if (render_offscreen(gpu, 400, 300, wide, &layout_ms, &frame_ms) != 0) {
        fprintf(stderr, "error: dynamic stage %d render failed\n", stage);
        return 1;
    }
    node_count = wn_core_live_node_count(g_core);
    printf("dynamic: stage=%d action=%s live=%d vertices=%d glyphs=%d images=%d "
           "cache=%d layout=%.3fms frame=%.3fms\n",
           stage, action, node_count, g_vertex_count, g_glyph_vertex_count,
           g_image_vertex_count, gpu->image_count, layout_ms, frame_ms);
    fflush(stdout);

    fprintf(report, "%s{\"stage\":%d,\"action\":\"%s\",\"live\":%d,"
                    "\"vertices\":%d,\"glyphs\":%d,\"image_vertices\":%d,"
                    "\"image_cache\":%d,\"layout_ms\":%.3f,\"frame_ms\":%.3f,",
            stage == 0 ? "" : ",\n    ", stage, action, node_count,
            g_vertex_count, g_glyph_vertex_count, g_image_vertex_count,
            gpu->image_count, layout_ms, frame_ms);
    fputs("\"card0\":", report);
    dynamic_rect_json(report, card0);
    fputs(",\"card1\":", report);
    dynamic_rect_json(report, card1);
    fputs(",\"clone\":", report);
    if (clone >= 0 && wn_core_node_is_alive(g_core, clone)) {
        dynamic_rect_json(report, clone);
    } else {
        fputs("null", report);
    }
    fputs(",\"card0_image\":", report);
    {
        const char *src = NULL;
        if (card0 >= 0 && wn_core_node_is_alive(g_core, card0)) {
            int image = wn_core_node_first_child(g_core, card0);
            src = wn_core_node_image_src(g_core, image);
        }
        if (src != NULL) {
            json_escape(report, src);
        } else {
            fputs("null", report);
        }
    }
    fputs(",\"clone_image\":", report);
    if (clone >= 0 && wn_core_node_is_alive(g_core, clone)) {
        int image = wn_core_node_first_child(g_core, clone);
        const char *src = wn_core_node_image_src(g_core, image);
        if (src != NULL) {
            json_escape(report, src);
        } else {
            fputs("null", report);
        }
    } else {
        fputs("null", report);
    }
    fputc('}', report);
    return 0;
}

static int run_dynamic(const char *prefix)
{
    Gpu gpu;
    FILE *report = NULL;
    char report_path[MAX_PATH];
    int cards;
    int card0;
    int card1;
    int image0;
    int label0;
    int text0;
    int clone = -1;
    int clone_image;
    int clone_label;
    int clone_text;
    int rc = 1;

    if (g_core == NULL || prefix == NULL || prefix[0] == '\0') {
        fprintf(stderr, "error: --dynamic needs an output prefix\n");
        return 1;
    }
    cards = wn_core_find_node(g_core, "body.root/div.cards");
    if (cards < 0) {
        cards = wn_core_find_node(g_core, "div.cards");
    }
    if (cards < 0) {
        fprintf(stderr, "error: --dynamic fixture has no div.cards\n");
        return 1;
    }
    card0 = wn_core_node_first_child(g_core, cards);
    card1 = card0 >= 0 ? wn_core_node_next_sibling(g_core, card0) : -1;
    image0 = card0 >= 0 ? wn_core_node_first_child(g_core, card0) : -1;
    label0 = image0 >= 0 ? wn_core_node_next_sibling(g_core, image0) : -1;
    text0 = label0 >= 0 ? wn_core_node_first_child(g_core, label0) : -1;
    if (card1 < 0 || text0 < 0) {
        fprintf(stderr, "error: --dynamic fixture is not the expected two-card page\n");
        return 1;
    }

    snprintf(report_path, sizeof(report_path), "%s_report.json", prefix);
    report = fopen(report_path, "wb");
    if (report == NULL) {
        fprintf(stderr, "error: cannot create %s\n", report_path);
        return 1;
    }
    memset(&gpu, 0, sizeof(gpu));
    if (gpu_init(&gpu, NULL, 0, WGPUTextureFormat_RGBA8Unorm,
                 "r2-dynamic-device") != 0) {
        gpu_release(&gpu);
        fclose(report);
        return 1;
    }
    fprintf(report, "{\n  \"fixture\": \"tests/dynamic/page\",\n  "
                    "\"viewport\": {\"width\": 400, \"height\": 300},\n  "
                    "\"cards\": %d, \"card0\": %d, \"card1\": %d,\n  "
                    "\"stages\": [\n    ", cards, card0, card1);

    if (dynamic_stage(&gpu, report, prefix, 0, "initial", card0, card1, clone) != 0) goto done;
    if (wn_core_set_visible(g_core, card0, 0) != 0) goto done;
    if (dynamic_stage(&gpu, report, prefix, 1, "hide card0", card0, card1, clone) != 0) goto done;
    if (wn_core_set_visible(g_core, card0, 1) != 0) goto done;
    if (dynamic_stage(&gpu, report, prefix, 2, "show card0", card0, card1, clone) != 0) goto done;

    clone = wn_core_clone(g_core, card0, cards);
    if (clone < 0) {
        fprintf(stderr, "error: clone failed\n");
        goto done;
    }
    clone_image = wn_core_node_first_child(g_core, clone);
    clone_label = wn_core_node_next_sibling(g_core, clone_image);
    clone_text = wn_core_node_first_child(g_core, clone_label);
    if (dynamic_stage(&gpu, report, prefix, 3, "clone card0", card0, card1, clone) != 0) goto done;

    if (wn_core_set_image(g_core, clone_image, "images/b.png") != 0) {
        fprintf(stderr, "error: set_image(clone) failed\n");
        goto done;
    }
    if (dynamic_stage(&gpu, report, prefix, 4, "set_image clone b", card0, card1, clone) != 0) goto done;

    if (wn_core_set_text(g_core, clone_text, "Beta") != 0) {
        fprintf(stderr, "error: set_text(clone) failed\n");
        goto done;
    }
    if (dynamic_stage(&gpu, report, prefix, 5, "set_text clone Beta", card0, card1, clone) != 0) goto done;

    if (wn_core_remove(g_core, clone) != 0) {
        fprintf(stderr, "error: remove(clone) failed\n");
        goto done;
    }
    if (dynamic_stage(&gpu, report, prefix, 6, "remove clone", card0, card1, -1) != 0) goto done;
    clone = -1;

    if (wn_core_append(g_core, cards, card1) != 0) {
        fprintf(stderr, "error: append(card1) failed\n");
        goto done;
    }
    if (dynamic_stage(&gpu, report, prefix, 7, "append card1 last", card0, card1, clone) != 0) goto done;

    if (wn_core_set_image(g_core, image0, "images/b.png") != 0) {
        fprintf(stderr, "error: set_image(card0) failed\n");
        goto done;
    }
    if (dynamic_stage(&gpu, report, prefix, 8, "set_image card0 b", card0, card1, clone) != 0) goto done;
    if (wn_core_set_image(g_core, image0, "images/a.png") != 0) {
        fprintf(stderr, "error: set_image(card0 a) failed\n");
        goto done;
    }
    if (dynamic_stage(&gpu, report, prefix, 9, "set_image card0 a", card0, card1, clone) != 0) goto done;

    if (wn_core_set_text(g_core, text0, "\xce\xa9") == 1) {
        printf("dynamic: set_text(Omega) -> unsupported (expected)\n");
    } else {
        fprintf(stderr, "error: set_text(Omega) was accepted\n");
        goto done;
    }
    if (wn_core_clear_children(g_core, cards) != 0) {
        fprintf(stderr, "error: clear_children(cards) failed\n");
        goto done;
    }
    if (dynamic_stage(&gpu, report, prefix, 10, "clear cards", card0, card1, clone) != 0) goto done;

    fprintf(report, "\n  ]\n}\n");
    rc = 0;

done:
    if (!rc) {
        fclose(report);
        report = NULL;
    } else if (report != NULL) {
        fprintf(report, "\n  ]\n}\n");
        fclose(report);
    }
    gpu_release(&gpu);
    return rc;
}

/* ---------------------------------------------------------------- input v1 */

static int input_stage(Gpu *gpu, FILE *report, const char *prefix, int stage,
                       const char *action, int node, int value_changes)
{
    char path[MAX_PATH];
    wchar_t wide[MAX_PATH];
    double layout_ms = 0.0;
    double frame_ms = 0.0;
    wn_input_state state;
    const wn_display_list *list;

    snprintf(path, sizeof(path), "%s_%02d.png", prefix, stage);
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    if (render_offscreen(gpu, 480, 240, wide, &layout_ms, &frame_ms) != 0) {
        fprintf(stderr, "error: input stage %d render failed\n", stage);
        return 1;
    }
    if (wn_core_input_state(g_core, node, &state) != 0) {
        fprintf(stderr, "error: input state failed\n");
        return 1;
    }
    list = wn_core_display_list(g_core);
    printf("input: stage=%d action=%s value=\"%s\" cursor=%d anchor=%d focus=%d "
           "caret=%.2f sel=(%.2f,%.2f) scroll=%.2f glyphs=%d layout=%.3fms\n",
           stage, action, state.value, state.cursor, state.anchor, state.focused,
           state.caret_x, state.selection_x0, state.selection_x1, state.scroll,
           list != NULL ? list->glyph_vertex_count : 0, layout_ms);
    fflush(stdout);
    fprintf(report,
            "%s{\"stage\":%d,\"action\":\"%s\",\"value\":",
            stage == 0 ? "" : ",\n    ", stage, action);
    json_escape(report, state.value);
    fprintf(report,
            ",\"cursor\":%d,\"anchor\":%d,\"focused\":%d,"
            "\"caret_x\":%.3f,\"selection_x0\":%.3f,\"selection_x1\":%.3f,"
            "\"scroll\":%.3f,\"baseline\":%.3f,\"line_height\":%.3f,"
            "\"rect\":{\"x\":%.2f,\"y\":%.2f,\"w\":%.2f,\"h\":%.2f},"
            "\"content\":{\"x\":%.2f,\"y\":%.2f,\"w\":%.2f,\"h\":%.2f},"
            "\"value_changes\":%d,\"glyph_vertices\":%d,\"png\":",
            state.cursor, state.anchor, state.focused, state.caret_x,
            state.selection_x0, state.selection_x1, state.scroll, state.baseline,
            state.line_height, state.rect.x, state.rect.y, state.rect.w, state.rect.h,
            state.content.x, state.content.y, state.content.w, state.content.h,
            value_changes, list != NULL ? list->glyph_vertex_count : 0);
    json_escape(report, path);
    fputc('}', report);
    return 0;
}

static int input_type(int node, const char *text, int *value_changes)
{
    int i;
    for (i = 0; text[i] != '\0'; ++i) {
        int changed = 0;
        int visual = 0;
        if (wn_core_input_char(g_core, (uint32_t)(unsigned char)text[i],
                               &changed, &visual) < 0) {
            return -1;
        }
        if (changed) {
            ++*value_changes;
        }
    }
    return node;
}

static int input_key(int node, int key, uint32_t mods, int *value_changes)
{
    int changed = 0;
    int visual = 0;
    if (wn_core_input_key(g_core, key, mods, &changed, &visual) < 0) {
        return -1;
    }
    if (changed) {
        ++*value_changes;
    }
    return node;
}

static int run_input(const char *prefix)
{
    Gpu gpu;
    FILE *report = NULL;
    char report_path[MAX_PATH];
    int node;
    int value_changes = 0;
    wn_rect rect;
    wn_input_state state;
    char clipboard[256];
    int copied;
    int paste_changed = 0;
    int paste_visual = 0;
    int rc = 1;

    if (g_core == NULL || prefix == NULL || prefix[0] == '\0') {
        fprintf(stderr, "error: --input needs an output prefix\n");
        return 1;
    }
    node = wn_core_find_node(g_core, "body.root/div.bar/input#search.search");
    if (node < 0) {
        node = wn_core_find_node(g_core, "input#search.search");
    }
    if (node < 0 || !wn_core_node_is_input(g_core, node)) {
        fprintf(stderr, "error: --input fixture has no input#search\n");
        return 1;
    }
    snprintf(report_path, sizeof(report_path), "%s_report.json", prefix);
    report = fopen(report_path, "wb");
    if (report == NULL) {
        fprintf(stderr, "error: cannot create %s\n", report_path);
        return 1;
    }
    memset(&gpu, 0, sizeof(gpu));
    if (gpu_init(&gpu, NULL, 0, WGPUTextureFormat_RGBA8Unorm,
                 "r2-input-device") != 0) {
        gpu_release(&gpu);
        fclose(report);
        return 1;
    }
    {
        wn_rect content;
        if (wn_core_input_state(g_core, node, &state) != 0) {
            goto done;
        }
        content = state.content;
        fprintf(report,
                "{\n  \"fixture\": \"tests/input/page\",\n"
                "  \"viewport\": {\"width\": 480, \"height\": 240},\n"
                "  \"node\": %d,\n  \"field\": \"input#search\",\n"
                "  \"max_length\": %d,\n  \"stages\": [\n    ",
                node, state.max_length);
        (void)content;
    }

    if (input_stage(&gpu, report, prefix, 0, "initial", node, value_changes) != 0) goto done;

    rect = wn_core_rects(g_core, NULL)[node];
    wn_core_pointer_down(g_core, rect.x + rect.w - 12.0f, rect.y + rect.h * 0.5f);
    wn_core_pointer_up(g_core, rect.x + rect.w - 12.0f, rect.y + rect.h * 0.5f);
    if (input_stage(&gpu, report, prefix, 1, "click focuses", node, value_changes) != 0) goto done;

    if (input_type(node, "abc", &value_changes) < 0) goto done;
    if (input_stage(&gpu, report, prefix, 2, "type abc", node, value_changes) != 0) goto done;

    /* Caret by click: Home+Right leaves the caret after 'a'; click 6 px past
     * that boundary to land after 'b', then insert X -> "abXc". */
    if (input_key(node, WN_KEY_HOME, 0, &value_changes) < 0) goto done;
    if (input_key(node, WN_KEY_RIGHT, 0, &value_changes) < 0) goto done;
    if (wn_core_input_state(g_core, node, &state) != 0) goto done;
    wn_core_pointer_down(g_core, state.caret_x + 6.0f, rect.y + rect.h * 0.5f);
    wn_core_pointer_up(g_core, state.caret_x + 6.0f, rect.y + rect.h * 0.5f);
    if (input_type(node, "X", &value_changes) < 0) goto done;
    if (input_stage(&gpu, report, prefix, 3, "click caret + insert X", node,
                    value_changes) != 0) goto done;

    if (input_key(node, WN_KEY_BACKSPACE, 0, &value_changes) < 0) goto done;
    if (input_stage(&gpu, report, prefix, 4, "backspace", node, value_changes) != 0) goto done;

    if (input_key(node, WN_KEY_HOME, 0, &value_changes) < 0) goto done;
    if (input_key(node, WN_KEY_RIGHT, WN_KEY_MOD_SHIFT, &value_changes) < 0) goto done;
    if (input_key(node, WN_KEY_RIGHT, WN_KEY_MOD_SHIFT, &value_changes) < 0) goto done;
    if (input_stage(&gpu, report, prefix, 5, "shift selection", node,
                    value_changes) != 0) goto done;

    copied = wn_core_input_copy(g_core, clipboard, (int)sizeof(clipboard));
    printf("input: copy selection = \"%s\" (%d bytes)\n", clipboard, copied);
    fflush(stdout);
    if (input_key(node, WN_KEY_END, 0, &value_changes) < 0) goto done;
    if (wn_core_input_paste(g_core, clipboard, &paste_changed, &paste_visual) < 0) goto done;
    if (paste_changed) {
        ++value_changes;
    }
    if (input_stage(&gpu, report, prefix, 6, "copy + paste", node,
                    value_changes) != 0) goto done;

    if (wn_core_input_select_all(g_core, NULL) < 0) goto done;
    if (input_key(node, WN_KEY_DELETE, 0, &value_changes) < 0) goto done;
    if (input_stage(&gpu, report, prefix, 7, "select all + delete", node,
                    value_changes) != 0) goto done;

    if (input_type(node, "0123456789abcd", &value_changes) < 0) goto done;
    if (input_stage(&gpu, report, prefix, 8, "max_length clamps at 12", node,
                    value_changes) != 0) goto done;

    if (input_key(node, WN_KEY_ESCAPE, 0, &value_changes) < 0) goto done;
    if (input_stage(&gpu, report, prefix, 9, "escape blurs", node,
                    value_changes) != 0) goto done;

    fprintf(report, "\n  ]\n}\n");
    rc = 0;

done:
    if (rc != 0 && report != NULL) {
        fprintf(report, "\n  ]\n}\n");
    }
    if (report != NULL) {
        fclose(report);
    }
    gpu_release(&gpu);
    return rc;
}

/* -------------------------------------------------------------------- main */

static int parse_size_list(const char *text, int *widths, int *heights, int max_count)
{
    int count = 0;
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "%s", text);
    char *save = NULL;
    for (char *token = strtok_s(buffer, ",", &save); token != NULL;
         token = strtok_s(NULL, ",", &save)) {
        int w = 0, h = 0;
        if (parse_size(token, &w, &h) != 0) {
            fprintf(stderr, "error: bad size '%s' (expected WxH)\n", token);
            return -1;
        }
        if (count < max_count) {
            widths[count] = w;
            heights[count] = h;
            ++count;
        }
    }
    return count;
}

int main(int argc, char **argv)
{
    int exit_code = 1;
    int mode_render = 0, mode_states = 0, mode_report = 0, mode_dump = 0, mode_dynamic = 0;
    int mode_input = 0;
    const char *dynamic_prefix = "dynamic";
    const char *input_prefix = "input";
    int render_w = UI_VIEWPORT_WIDTH, render_h = UI_VIEWPORT_HEIGHT;
    const char *render_path = "out.png";
    const char *states_prefix = "state";
    const char *report_path = "layout_report.json";
    int state_node = -1;
    int width_arg = 0, height_arg = 0;
    int default_sizes = 1;
    int widths[8] = {960, 720, 560, 0, 0, 0, 0, 0};
    int heights[8] = {640, 640, 640, 0, 0, 0, 0, 0};
    int size_count = 3;

    setvbuf(stdout, NULL, _IONBF, 0);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    SetProcessDPIAware();
    wgpuSetLogLevel(WGPULogLevel_Warn);
    wgpuSetLogCallback(on_log, NULL);

    g_core = wn_core_create(wn_document_builtin());
    if (g_core == NULL) {
        fprintf(stderr, "error: wn_core_create failed\n");
        exit_code = 1;
        goto done;
    }

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--render") == 0) {
            mode_render = 1;
            if (i + 1 < argc && strchr(argv[i + 1], 'x') != NULL) {
                if (parse_size(argv[i + 1], &render_w, &render_h) != 0) {
                    fprintf(stderr, "error: bad --render size\n");
                    return 1;
                }
                if (i + 2 >= argc) {
                    fprintf(stderr, "error: --render needs an output path\n");
                    return 1;
                }
                render_path = argv[i + 2];
                i += 2;
            } else {
                if (i + 3 >= argc) {
                    fprintf(stderr, "error: --render needs W H out.png\n");
                    return 1;
                }
                render_w = atoi(argv[i + 1]);
                render_h = atoi(argv[i + 2]);
                render_path = argv[i + 3];
                i += 3;
            }
        } else if (strcmp(argv[i], "--states") == 0) {
            mode_states = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                states_prefix = argv[++i];
            }
        } else if (strcmp(argv[i], "--node") == 0 && i + 1 < argc) {
            state_node = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--hover") == 0 && i + 1 < argc) {
            set_state_from_arg(WN_STATE_HOVER, atoi(argv[++i]));
        } else if (strcmp(argv[i], "--press") == 0 && i + 1 < argc) {
            set_state_from_arg(WN_STATE_PRESS, atoi(argv[++i]));
        } else if (strcmp(argv[i], "--selected") == 0 && i + 1 < argc) {
            set_state_from_arg(WN_STATE_SELECTED, atoi(argv[++i]));
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            if (parse_size(argv[++i], &render_w, &render_h) != 0) {
                fprintf(stderr, "error: bad --size\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--dynamic") == 0) {
            mode_dynamic = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                dynamic_prefix = argv[++i];
            }
        } else if (strcmp(argv[i], "--input") == 0) {
            mode_input = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                input_prefix = argv[++i];
            }
        } else if (strcmp(argv[i], "--report") == 0) {
            mode_report = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                report_path = argv[++i];
            }
        } else if (strcmp(argv[i], "--sizes") == 0 && i + 1 < argc) {
            int n = parse_size_list(argv[++i], widths, heights, 8);
            if (n <= 0) {
                return 1;
            }
            size_count = n;
            default_sizes = 0;
        } else if (strcmp(argv[i], "--dump") == 0) {
            mode_dump = 1;
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            g_max_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--resize-demo") == 0) {
            g_resize_demo = 1;
        } else if (strcmp(argv[i], "--input-demo") == 0) {
            g_input_demo = 1;
        } else if (strcmp(argv[i], "--wheel") == 0 && i + 3 < argc) {
            g_wheel_active = 1;
            g_wheel_x = (float)atof(argv[i + 1]);
            g_wheel_y = (float)atof(argv[i + 2]);
            g_wheel_dy = (float)atof(argv[i + 3]);
            i += 3;
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            width_arg = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            height_arg = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: runtime_r2.exe [--render W H out.png] [--states PREFIX] "
                   "[--report FILE] [--dynamic PREFIX] [--input PREFIX] [--frames N] "
                   "[--resize-demo]\n");
            CoUninitialize();
            return 0;
        } else {
            fprintf(stderr, "warning: ignoring argument '%s'\n", argv[i]);
        }
    }
    if (width_arg > 0 && height_arg > 0) {
        g_window_width = width_arg;
        g_window_height = height_arg;
        render_w = width_arg;
        render_h = height_arg;
    }
    (void)default_sizes;
    (void)height_arg;

    if (mode_dump) {
        printf("nodes: %d\n", UI_NODE_COUNT);
        for (int i = 0; i < UI_NODE_COUNT; ++i) {
            const UiNode *n = &kUiNodes[i];
            printf("[%2d] parent=%2d %-42s kind=%s dir=%s justify=%d align=%d wrap=%d "
                   "w=%.1f h=%.1f wpct=%.1f hpct=%.1f gap=%.1f bg=0x%08X action=%d\n",
                   i, n->parent, kUiNodeNames[i], kind_name(n->kind),
                   n->direction ? "col" : "row", n->justify, n->align, n->wrap,
                   n->width, n->height, n->width_pct, n->height_pct, n->gap,
                   n->background, n->action);
        }
        exit_code = 0;
        goto done;
    }

    if (mode_report) {
        exit_code = write_layout_report(report_path, widths, heights, size_count);
        goto done;
    }

    if (mode_dynamic) {
        exit_code = run_dynamic(dynamic_prefix);
        goto done;
    }

    if (mode_input) {
        exit_code = run_input(input_prefix);
        goto done;
    }

    if (mode_states) {
        if (state_node < 0) {
            state_node = first_interactive_node();
        }
        exit_code = run_states(states_prefix, state_node, render_w, render_h);
        goto done;
    }

    if (mode_render) {
        Gpu gpu;
        double layout_ms = 0.0, frame_ms = 0.0;
        wchar_t wide[MAX_PATH];
        if (render_w <= 0 || render_h <= 0) {
            fprintf(stderr, "error: bad render size %dx%d\n", render_w, render_h);
            return 1;
        }
        if (gpu_init(&gpu, NULL, 0, WGPUTextureFormat_RGBA8Unorm, "r2-offscreen-device") != 0) {
            gpu_release(&gpu);
            return 1;
        }
        MultiByteToWideChar(CP_ACP, 0, render_path, -1, wide, MAX_PATH);
        if (render_offscreen(&gpu, render_w, render_h, wide, &layout_ms, &frame_ms) != 0) {
            gpu_release(&gpu);
            return 1;
        }
        gpu_release(&gpu);
        print_layout_summary((float)render_w, (float)render_h);
        print_scroll_summary();
        printf("render: %s %dx%d nodes=%d vertices=%d glyph_vertices=%d "
               "image_vertices=%d layout=%.3fms frame=%.3fms\n",
               render_path, render_w, render_h, UI_NODE_COUNT, g_vertex_count,
               g_glyph_vertex_count, g_image_vertex_count, layout_ms, frame_ms);
        printf("timings: layout=%.3fms | frame(build+upload+pass+readback+png)=%.3fms\n",
               layout_ms, frame_ms);
        printf("timings: gpu(encode+submit)=%.3fms | readback(map)=%.3fms | png=%.3fms\n",
               g_last_pass_ms, g_last_readback_ms, g_last_png_ms);
        exit_code = 0;
        goto done;
    }

    exit_code = run_window();

done:
    wn_core_destroy(g_core);
    g_core = NULL;
    CoUninitialize();
    return exit_code;
}
