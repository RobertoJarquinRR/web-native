/*
 * renderer_wgpu.c - wgpu-native renderer backend behind wn_renderer_ops.
 *
 * Solid pipeline, R8 glyph-atlas text pipeline and the surface swapchain.
 * wgpu types stay in this file.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wgpu.h>

#include "wn_renderer.h"
#include "wn_internal.h"

_Static_assert(sizeof(wn_vertex) == 12, "wn_vertex must stay 12 bytes");
_Static_assert(sizeof(wn_glyph_vertex) == 20, "wn_glyph_vertex must stay 20 bytes");
_Static_assert(sizeof(wn_image_vertex) == 16, "wn_image_vertex must stay 16 bytes");

/* Decoded local image cached by path (PNG/JPEG via WIC -> RGBA8). */
typedef struct {
    char *path;
    WGPUTexture texture;
    WGPUTextureView view;
    WGPUBindGroup bind;
    int width;
    int height;
} WnImageTexture;

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
    WGPUInstance instance;
    WGPUAdapter adapter;
    WGPUDevice device;
    WGPUQueue queue;
    WGPUSurface surface;
    WGPUTextureFormat format;
    int width;
    int height;
    int configured;
    int offscreen;
    WGPUTexture target;
    WGPUTextureView target_view;

    WGPUShaderModule shader;
    WGPURenderPipeline pipeline;
    WGPUBuffer vertex_buffer;
    size_t vertex_capacity;

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
    int atlas_width;
    int atlas_height;
    const unsigned char *atlas_ptr;
    unsigned int atlas_serial;

    /* ui-tree/2 images (WIC -> RGBA8, cache by path). */
    WGPUShaderModule image_shader;
    WGPURenderPipeline image_pipeline;
    WGPUBuffer image_buffer;
    size_t image_capacity;
    WnImageTexture *images;
    int image_count;
    int image_cache_capacity;
    IWICImagingFactory *wic;
    int com_initialized;

    int uploaded_vertices;
    int uploaded_glyphs;
    int uploaded_images;

    wn_renderer_info info;
    wn_renderer_stats stats;
    char error[512];
} WnWgpu;

static WGPUStringView wsv(const char *text)
{
    WGPUStringView view;
    view.data = text;
    view.length = WGPU_STRLEN;
    return view;
}

static void copy_view(char *dst, size_t cap, WGPUStringView view)
{
    size_t n;
    if (dst == NULL || cap == 0) {
        return;
    }
    if (view.data == NULL || view.length == 0) {
        dst[0] = '\0';
        return;
    }
    if (view.length == WGPU_STRLEN) {
        snprintf(dst, cap, "%s", view.data);
        return;
    }
    n = view.length < cap - 1 ? (size_t)view.length : cap - 1;
    memcpy(dst, view.data, n);
    dst[n] = '\0';
}

static void set_error(WnWgpu *gpu, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(gpu->error, sizeof(gpu->error), fmt, args);
    va_end(args);
    gpu->error[sizeof(gpu->error) - 1] = '\0';
}

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

static void on_uncaptured_error(WGPUDevice const *device, WGPUErrorType type,
                                WGPUStringView message, void *userdata1, void *userdata2)
{
    (void)device;
    (void)userdata1;
    (void)userdata2;
    fprintf(stderr, "renderer: wgpu error type %d: %.*s\n", (int)type,
            (int)(message.data != NULL ? (message.length == WGPU_STRLEN ? 512u : message.length) : 0u),
            message.data != NULL ? message.data : "");
    fflush(stderr);
}

/* wgpu-native v29 does not implement wgpuInstanceWaitAny; poll instead. */
static int wait_flag(WGPUInstance instance, volatile int *flag, const char *what,
                     char *error, size_t error_cap)
{
    DWORD start = GetTickCount();
    while (!*flag) {
        wgpuInstanceProcessEvents(instance);
        if (GetTickCount() - start > 30000u) {
            snprintf(error, error_cap, "%s timed out after 30s", what);
            return 1;
        }
        Sleep(1);
    }
    return 0;
}

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

static int create_offscreen_target(WnWgpu *gpu);

static void configure_surface(WnWgpu *gpu)
{
    WGPUSurfaceConfiguration config;
    if (gpu->surface == NULL || gpu->device == NULL || gpu->width <= 0 || gpu->height <= 0) {
        return;
    }
    memset(&config, 0, sizeof(config));
    config.device = gpu->device;
    config.format = gpu->format;
    config.usage = WGPUTextureUsage_RenderAttachment;
    config.width = (uint32_t)gpu->width;
    config.height = (uint32_t)gpu->height;
    config.presentMode = WGPUPresentMode_Fifo;
    config.alphaMode = WGPUCompositeAlphaMode_Auto;
    wgpuSurfaceConfigure(gpu->surface, &config);
    gpu->configured = 1;
}

static int ensure_buffer(WnWgpu *gpu, WGPUBuffer *buffer, size_t *capacity,
                         size_t needed, WGPUBufferUsage usage, const char *label)
{
    WGPUBufferDescriptor descriptor;
    size_t size;
    if (*buffer != NULL && *capacity >= needed) {
        return 0;
    }
    size = needed > 256 ? needed : 256;
    size = (size + 3u) & ~3u;
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.label = wsv(label);
    descriptor.usage = usage;
    descriptor.size = size;
    if (*buffer != NULL) {
        wgpuBufferRelease(*buffer);
    }
    *buffer = wgpuDeviceCreateBuffer(gpu->device, &descriptor);
    if (*buffer == NULL) {
        *capacity = 0;
        set_error(gpu, "wgpuDeviceCreateBuffer(%s) failed", label);
        return 1;
    }
    *capacity = size;
    return 0;
}

static int create_text_resources(WnWgpu *gpu)
{
    WGPUBindGroupLayoutEntry entries[2];
    WGPUBindGroupLayoutDescriptor bgl_desc;
    WGPUPipelineLayoutDescriptor pl_desc;
    WGPUSamplerDescriptor sampler_desc;

    memset(&sampler_desc, 0, sizeof(sampler_desc));
    sampler_desc.label = wsv("wn-atlas-sampler");
    sampler_desc.addressModeU = WGPUAddressMode_ClampToEdge;
    sampler_desc.addressModeV = WGPUAddressMode_ClampToEdge;
    sampler_desc.addressModeW = WGPUAddressMode_ClampToEdge;
    sampler_desc.magFilter = WGPUFilterMode_Linear;
    sampler_desc.minFilter = WGPUFilterMode_Linear;
    sampler_desc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    sampler_desc.lodMinClamp = 0.0f;
    sampler_desc.lodMaxClamp = 32.0f;
    sampler_desc.maxAnisotropy = 1;
    gpu->sampler = wgpuDeviceCreateSampler(gpu->device, &sampler_desc);
    if (gpu->sampler == NULL) {
        set_error(gpu, "sampler creation failed");
        return 1;
    }

    memset(entries, 0, sizeof(entries));
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Fragment;
    entries[0].texture.sampleType = WGPUTextureSampleType_Float;
    entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Fragment;
    entries[1].sampler.type = WGPUSamplerBindingType_Filtering;
    memset(&bgl_desc, 0, sizeof(bgl_desc));
    bgl_desc.label = wsv("wn-text-bgl");
    bgl_desc.entryCount = 2;
    bgl_desc.entries = entries;
    gpu->text_bgl = wgpuDeviceCreateBindGroupLayout(gpu->device, &bgl_desc);
    if (gpu->text_bgl == NULL) {
        set_error(gpu, "text bind group layout creation failed");
        return 1;
    }

    memset(&pl_desc, 0, sizeof(pl_desc));
    pl_desc.label = wsv("wn-text-pipeline-layout");
    pl_desc.bindGroupLayoutCount = 1;
    pl_desc.bindGroupLayouts = &gpu->text_bgl;
    gpu->text_pl = wgpuDeviceCreatePipelineLayout(gpu->device, &pl_desc);
    if (gpu->text_pl == NULL) {
        set_error(gpu, "text pipeline layout creation failed");
        return 1;
    }
    return 0;
}

static int create_pipelines(WnWgpu *gpu)
{
    {
        WGPUShaderModuleDescriptor descriptor;
        WGPUShaderSourceWGSL wgsl;
        memset(&descriptor, 0, sizeof(descriptor));
        memset(&wgsl, 0, sizeof(wgsl));
        wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
        wgsl.code = wsv(kShaderWGSL);
        descriptor.label = wsv("wn-solid-shader");
        descriptor.nextInChain = &wgsl.chain;
        gpu->shader = wgpuDeviceCreateShaderModule(gpu->device, &descriptor);
        if (gpu->shader == NULL) {
            set_error(gpu, "solid shader creation failed");
            return 1;
        }
    }
    {
        WGPUShaderModuleDescriptor descriptor;
        WGPUShaderSourceWGSL wgsl;
        memset(&descriptor, 0, sizeof(descriptor));
        memset(&wgsl, 0, sizeof(wgsl));
        wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
        wgsl.code = wsv(kTextShaderWGSL);
        descriptor.label = wsv("wn-text-shader");
        descriptor.nextInChain = &wgsl.chain;
        gpu->text_shader = wgpuDeviceCreateShaderModule(gpu->device, &descriptor);
        if (gpu->text_shader == NULL) {
            set_error(gpu, "text shader creation failed");
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
        buffer_layout.arrayStride = sizeof(wn_vertex);
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
        fragment.module = gpu->shader;
        fragment.entryPoint = wsv("fs_main");
        fragment.targetCount = 1;
        fragment.targets = &target;
        memset(&vertex, 0, sizeof(vertex));
        vertex.module = gpu->shader;
        vertex.entryPoint = wsv("vs_main");
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
        descriptor.label = wsv("wn-solid-pipeline");
        descriptor.vertex = vertex;
        descriptor.primitive = primitive;
        descriptor.multisample = multisample;
        descriptor.fragment = &fragment;
        gpu->pipeline = wgpuDeviceCreateRenderPipeline(gpu->device, &descriptor);
        if (gpu->pipeline == NULL) {
            set_error(gpu, "solid pipeline creation failed");
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
        buffer_layout.arrayStride = sizeof(wn_glyph_vertex);
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
        fragment.entryPoint = wsv("fs_main");
        fragment.targetCount = 1;
        fragment.targets = &target;
        memset(&vertex, 0, sizeof(vertex));
        vertex.module = gpu->text_shader;
        vertex.entryPoint = wsv("vs_main");
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
        descriptor.label = wsv("wn-text-pipeline");
        descriptor.layout = gpu->text_pl;
        descriptor.vertex = vertex;
        descriptor.primitive = primitive;
        descriptor.multisample = multisample;
        descriptor.fragment = &fragment;
        gpu->text_pipeline = wgpuDeviceCreateRenderPipeline(gpu->device, &descriptor);
        if (gpu->text_pipeline == NULL) {
            set_error(gpu, "text pipeline creation failed");
            return 1;
        }
    }

    {
        WGPUShaderModuleDescriptor descriptor;
        WGPUShaderSourceWGSL wgsl;
        memset(&descriptor, 0, sizeof(descriptor));
        memset(&wgsl, 0, sizeof(wgsl));
        wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
        wgsl.code = wsv(kImageShaderWGSL);
        descriptor.label = wsv("wn-image-shader");
        descriptor.nextInChain = &wgsl.chain;
        gpu->image_shader = wgpuDeviceCreateShaderModule(gpu->device, &descriptor);
        if (gpu->image_shader == NULL) {
            set_error(gpu, "image shader creation failed");
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
        fragment.entryPoint = wsv("fs_main");
        fragment.targetCount = 1;
        fragment.targets = &target;
        memset(&vertex, 0, sizeof(vertex));
        vertex.module = gpu->image_shader;
        vertex.entryPoint = wsv("vs_main");
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
        descriptor.label = wsv("wn-image-pipeline");
        descriptor.layout = gpu->text_pl; /* same texture+sampler layout */
        descriptor.vertex = vertex;
        descriptor.primitive = primitive;
        descriptor.multisample = multisample;
        descriptor.fragment = &fragment;
        gpu->image_pipeline = wgpuDeviceCreateRenderPipeline(gpu->device, &descriptor);
        if (gpu->image_pipeline == NULL) {
            set_error(gpu, "image pipeline creation failed");
            return 1;
        }
    }
    return 0;
}

static int renderer_init(void **out, const wn_surface_desc *surface, int width, int height)
{
    WnWgpu *gpu;
    AdapterResult adapter_result;
    DeviceResult device_result;
    WGPUAdapterInfo adapter_info;

    if (out == NULL) {
        return 1;
    }
    *out = NULL;
    if (surface == NULL ||
        (surface->kind != WN_SURFACE_HWND && surface->kind != WN_SURFACE_OFFSCREEN) ||
        (surface->kind == WN_SURFACE_HWND && surface->handle == NULL)) {
        return 1;
    }
    gpu = (WnWgpu *)calloc(1, sizeof(*gpu));
    if (gpu == NULL) {
        return 1;
    }
    gpu->width = width > 0 ? width : 1;
    gpu->height = height > 0 ? height : 1;
    gpu->offscreen = surface->kind == WN_SURFACE_OFFSCREEN ? 1 : 0;
    gpu->format = WGPUTextureFormat_BGRA8Unorm;
    gpu->info.width = gpu->width;
    gpu->info.height = gpu->height;

    {
        HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        gpu->com_initialized = SUCCEEDED(hr) ? 1 : 0;
    }

    wgpuSetLogLevel(WGPULogLevel_Warn);

    gpu->instance = wgpuCreateInstance(NULL);
    if (gpu->instance == NULL) {
        set_error(gpu, "wgpuCreateInstance returned NULL");
        goto fail;
    }

    if (!gpu->offscreen) {
        WGPUSurfaceSourceWindowsHWND source;
        WGPUSurfaceDescriptor descriptor;
        memset(&source, 0, sizeof(source));
        source.chain.sType = WGPUSType_SurfaceSourceWindowsHWND;
        source.hinstance = surface->instance_handle != NULL
                               ? (HINSTANCE)surface->instance_handle
                               : GetModuleHandleW(NULL);
        source.hwnd = (HWND)surface->handle;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = wsv("wn-surface");
        descriptor.nextInChain = &source.chain;
        gpu->surface = wgpuInstanceCreateSurface(gpu->instance, &descriptor);
        if (gpu->surface == NULL) {
            set_error(gpu, "wgpuInstanceCreateSurface failed");
            goto fail;
        }
    }

    memset(&adapter_result, 0, sizeof(adapter_result));
    {
        WGPURequestAdapterOptions options;
        WGPURequestAdapterCallbackInfo callback;
        memset(&options, 0, sizeof(options));
        memset(&callback, 0, sizeof(callback));
        options.powerPreference = WGPUPowerPreference_HighPerformance;
        options.featureLevel = WGPUFeatureLevel_Core;
        options.compatibleSurface = gpu->surface;
        callback.mode = WGPUCallbackMode_AllowProcessEvents;
        callback.callback = on_adapter;
        callback.userdata1 = &adapter_result;
        (void)wgpuInstanceRequestAdapter(gpu->instance, &options, callback);
        if (wait_flag(gpu->instance, &adapter_result.done, "request adapter",
                      gpu->error, sizeof(gpu->error)) != 0) {
            goto fail;
        }
    }
    if (adapter_result.status != WGPURequestAdapterStatus_Success ||
        adapter_result.adapter == NULL) {
        set_error(gpu, "no adapter (status %d): %s", (int)adapter_result.status,
                  adapter_result.message);
        goto fail;
    }
    gpu->adapter = adapter_result.adapter;

    memset(&adapter_info, 0, sizeof(adapter_info));
    if (wgpuAdapterGetInfo(gpu->adapter, &adapter_info) == WGPUStatus_Success) {
        snprintf(gpu->info.backend, sizeof(gpu->info.backend), "%s",
                 backend_name(adapter_info.backendType));
        copy_view(gpu->info.device, sizeof(gpu->info.device), adapter_info.device);
        printf("renderer: backend=%s (%d) type=%s vendorID=0x%04X deviceID=0x%04X\n",
               backend_name(adapter_info.backendType), (int)adapter_info.backendType,
               adapter_type_name(adapter_info.adapterType),
               adapter_info.vendorID, adapter_info.deviceID);
        printf("renderer: device=%s\n", gpu->info.device[0] != '\0' ? gpu->info.device : "(unknown)");
        fflush(stdout);
    } else {
        snprintf(gpu->info.backend, sizeof(gpu->info.backend), "%s", "unknown");
    }

    memset(&device_result, 0, sizeof(device_result));
    {
        WGPUDeviceDescriptor descriptor;
        WGPURequestDeviceCallbackInfo callback;
        memset(&descriptor, 0, sizeof(descriptor));
        memset(&callback, 0, sizeof(callback));
        descriptor.label = wsv("wn-device");
        descriptor.uncapturedErrorCallbackInfo.callback = on_uncaptured_error;
        callback.mode = WGPUCallbackMode_AllowProcessEvents;
        callback.callback = on_device;
        callback.userdata1 = &device_result;
        (void)wgpuAdapterRequestDevice(gpu->adapter, &descriptor, callback);
        if (wait_flag(gpu->instance, &device_result.done, "request device",
                      gpu->error, sizeof(gpu->error)) != 0) {
            goto fail;
        }
    }
    if (device_result.status != WGPURequestDeviceStatus_Success ||
        device_result.device == NULL) {
        set_error(gpu, "no device (status %d): %s", (int)device_result.status,
                  device_result.message);
        goto fail;
    }
    gpu->device = device_result.device;
    gpu->queue = wgpuDeviceGetQueue(gpu->device);

    {
        WGPUSurfaceCapabilities caps;
        if (gpu->offscreen) {
            gpu->format = WGPUTextureFormat_BGRA8Unorm;
            gpu->info.format = (int)gpu->format;
        } else {
            memset(&caps, 0, sizeof(caps));
            if (wgpuSurfaceGetCapabilities(gpu->surface, gpu->adapter, &caps) != WGPUStatus_Success ||
                caps.formatCount == 0) {
                set_error(gpu, "surface capabilities failed");
                goto fail;
            }
            gpu->format = caps.formats[0];
            for (size_t i = 0; i < caps.formatCount; ++i) {
                if (caps.formats[i] == WGPUTextureFormat_BGRA8Unorm ||
                    caps.formats[i] == WGPUTextureFormat_RGBA8Unorm) {
                    gpu->format = caps.formats[i];
                    break;
                }
            }
            gpu->info.format = (int)gpu->format;
        }
    }

    if (!gpu->offscreen) {
        configure_surface(gpu);
    }
    if (create_text_resources(gpu) != 0 || create_pipelines(gpu) != 0) {
        goto fail;
    }
    if (ensure_buffer(gpu, &gpu->vertex_buffer, &gpu->vertex_capacity, 1024,
                      WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst,
                      "wn-vertices") != 0) {
        goto fail;
    }
    if (ensure_buffer(gpu, &gpu->glyph_buffer, &gpu->glyph_capacity, 1024,
                      WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst,
                      "wn-glyph-vertices") != 0) {
        goto fail;
    }
    if (ensure_buffer(gpu, &gpu->image_buffer, &gpu->image_capacity, 1024,
                      WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst,
                      "wn-image-vertices") != 0) {
        goto fail;
    }

    *out = gpu;
    return 0;

fail:
    if (gpu->surface != NULL) wgpuSurfaceRelease(gpu->surface);
    if (gpu->queue != NULL) wgpuQueueRelease(gpu->queue);
    if (gpu->device != NULL) wgpuDeviceRelease(gpu->device);
    if (gpu->adapter != NULL) wgpuAdapterRelease(gpu->adapter);
    if (gpu->instance != NULL) wgpuInstanceRelease(gpu->instance);
    free(gpu);
    return 1;
}

static void renderer_resize(void *context, int width, int height)
{
    WnWgpu *gpu = (WnWgpu *)context;
    if (gpu == NULL) {
        return;
    }
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    if (gpu->width == width && gpu->height == height &&
        (gpu->offscreen ? gpu->target_view != NULL : gpu->configured)) {
        return;
    }
    gpu->width = width;
    gpu->height = height;
    gpu->info.width = width;
    gpu->info.height = height;
    if (gpu->offscreen) {
        (void)create_offscreen_target(gpu);
        return;
    }
    configure_surface(gpu);
}

static int upload_atlas(WnWgpu *gpu, const wn_atlas_view *atlas)
{
    WGPUTextureDescriptor texture_desc;
    WGPUTexelCopyTextureInfo destination;
    WGPUTexelCopyBufferLayout layout;
    WGPUExtent3D extent;
    WGPUBindGroupEntry bind_entries[2];
    WGPUBindGroupDescriptor bind_desc;

    if (atlas == NULL || atlas->pixels == NULL || atlas->width <= 0 || atlas->height <= 0) {
        return 0;
    }
    if (gpu->atlas_texture != NULL && gpu->atlas_ptr == atlas->pixels && gpu->atlas_serial == atlas->serial &&
        gpu->atlas_width == atlas->width && gpu->atlas_height == atlas->height) {
        return 0;
    }

    if (gpu->text_bg != NULL) {
        wgpuBindGroupRelease(gpu->text_bg);
        gpu->text_bg = NULL;
    }
    if (gpu->atlas_view != NULL) {
        wgpuTextureViewRelease(gpu->atlas_view);
        gpu->atlas_view = NULL;
    }
    if (gpu->atlas_texture != NULL) {
        wgpuTextureRelease(gpu->atlas_texture);
        gpu->atlas_texture = NULL;
    }

    memset(&texture_desc, 0, sizeof(texture_desc));
    texture_desc.label = wsv("wn-glyph-atlas");
    texture_desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    texture_desc.dimension = WGPUTextureDimension_2D;
    texture_desc.size.width = (uint32_t)atlas->width;
    texture_desc.size.height = (uint32_t)atlas->height;
    texture_desc.size.depthOrArrayLayers = 1;
    texture_desc.format = WGPUTextureFormat_R8Unorm;
    texture_desc.mipLevelCount = 1;
    texture_desc.sampleCount = 1;
    gpu->atlas_texture = wgpuDeviceCreateTexture(gpu->device, &texture_desc);
    if (gpu->atlas_texture == NULL) {
        set_error(gpu, "glyph atlas texture creation failed");
        return 1;
    }
    gpu->atlas_view = wgpuTextureCreateView(gpu->atlas_texture, NULL);
    if (gpu->atlas_view == NULL) {
        set_error(gpu, "glyph atlas view creation failed");
        return 1;
    }

    memset(&destination, 0, sizeof(destination));
    destination.texture = gpu->atlas_texture;
    destination.mipLevel = 0;
    destination.aspect = WGPUTextureAspect_All;
    memset(&layout, 0, sizeof(layout));
    layout.offset = 0;
    layout.bytesPerRow = (uint32_t)atlas->width;
    layout.rowsPerImage = (uint32_t)atlas->height;
    memset(&extent, 0, sizeof(extent));
    extent.width = (uint32_t)atlas->width;
    extent.height = (uint32_t)atlas->height;
    extent.depthOrArrayLayers = 1;
    wgpuQueueWriteTexture(gpu->queue, &destination, atlas->pixels,
                          (size_t)atlas->size, &layout, &extent);

    memset(bind_entries, 0, sizeof(bind_entries));
    bind_entries[0].binding = 0;
    bind_entries[0].textureView = gpu->atlas_view;
    bind_entries[1].binding = 1;
    bind_entries[1].sampler = gpu->sampler;
    memset(&bind_desc, 0, sizeof(bind_desc));
    bind_desc.label = wsv("wn-text-bind-group");
    bind_desc.layout = gpu->text_bgl;
    bind_desc.entryCount = 2;
    bind_desc.entries = bind_entries;
    gpu->text_bg = wgpuDeviceCreateBindGroup(gpu->device, &bind_desc);
    if (gpu->text_bg == NULL) {
        set_error(gpu, "text bind group creation failed");
        return 1;
    }
    gpu->atlas_width = atlas->width;
    gpu->atlas_height = atlas->height;
    gpu->atlas_ptr = atlas->pixels;
    gpu->atlas_serial = atlas->serial;
    return 0;
}

static int decode_image_rgba(WnWgpu *gpu, const char *path, unsigned char **pixels_out,
                             int *width_out, int *height_out)
{
    wchar_t wide[1024];
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
            set_error(gpu, "WIC factory creation failed (hr=0x%08lX)", (unsigned long)hr);
            return 1;
        }
    }
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide,
                            (int)(sizeof(wide) / sizeof(wide[0]))) <= 0) {
        set_error(gpu, "image path is not valid UTF-8: %s", path);
        return 1;
    }
    hr = gpu->wic->lpVtbl->CreateDecoderFromFilename(
        gpu->wic, wide, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr) || decoder == NULL) {
        set_error(gpu, "cannot decode image '%s' (hr=0x%08lX)", path, (unsigned long)hr);
        return 1;
    }
    hr = decoder->lpVtbl->GetFrame(decoder, 0, &frame);
    if (FAILED(hr) || frame == NULL) {
        set_error(gpu, "image '%s' has no frame 0 (hr=0x%08lX)", path, (unsigned long)hr);
        goto fail;
    }
    hr = frame->lpVtbl->GetSize(frame, &width, &height);
    if (FAILED(hr) || width == 0 || height == 0 || width > 16384 || height > 16384) {
        set_error(gpu, "image '%s' has invalid size %ux%u", path, width, height);
        goto fail;
    }
    hr = gpu->wic->lpVtbl->CreateFormatConverter(gpu->wic, &converter);
    if (FAILED(hr) || converter == NULL) {
        set_error(gpu, "WIC converter creation failed for '%s'", path);
        goto fail;
    }
    hr = converter->lpVtbl->Initialize(converter, (IWICBitmapSource *)frame,
                                       &GUID_WICPixelFormat32bppRGBA,
                                       WICBitmapDitherTypeNone, NULL, 0.0,
                                       WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        set_error(gpu, "WIC RGBA conversion failed for '%s' (hr=0x%08lX)",
                  path, (unsigned long)hr);
        goto fail;
    }
    {
        const UINT stride = width * 4u;
        const UINT bytes = stride * height;
        pixels = (unsigned char *)malloc(bytes);
        if (pixels == NULL) {
            set_error(gpu, "out of memory decoding image '%s'", path);
            goto fail;
        }
        hr = converter->lpVtbl->CopyPixels(converter, NULL, stride, bytes, pixels);
        if (FAILED(hr)) {
            set_error(gpu, "WIC CopyPixels failed for '%s' (hr=0x%08lX)",
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

static WnImageTexture *find_cached_image(WnWgpu *gpu, const char *path)
{
    int i;
    for (i = 0; i < gpu->image_count; ++i) {
        if (strcmp(gpu->images[i].path, path) == 0) {
            return &gpu->images[i];
        }
    }
    return NULL;
}

static WnImageTexture *ensure_image_texture(WnWgpu *gpu, const char *path)
{
    WnImageTexture *entry;
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
        WnImageTexture *grown = (WnImageTexture *)realloc(
            gpu->images, (size_t)capacity * sizeof(WnImageTexture));
        if (grown == NULL) {
            free(pixels);
            set_error(gpu, "out of memory growing the image cache");
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
                set_error(gpu, "out of memory padding image '%s'", path);
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
    texture_desc.label = wsv("wn-image");
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
        free(padded);
        if (padded != pixels) free(pixels);
        set_error(gpu, "image texture creation failed for '%s'", path);
        return NULL;
    }
    entry->view = wgpuTextureCreateView(entry->texture, NULL);
    if (entry->view == NULL) {
        wgpuTextureRelease(entry->texture);
        free(padded);
        if (padded != pixels) free(pixels);
        set_error(gpu, "image texture view creation failed for '%s'", path);
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
    bind_desc.label = wsv("wn-image-bind-group");
    bind_desc.layout = gpu->text_bgl;
    bind_desc.entryCount = 2;
    bind_desc.entries = bind_entries;
    entry->bind = wgpuDeviceCreateBindGroup(gpu->device, &bind_desc);
    if (entry->bind == NULL) {
        wgpuTextureViewRelease(entry->view);
        wgpuTextureRelease(entry->texture);
        set_error(gpu, "image bind group creation failed for '%s'", path);
        return NULL;
    }
    entry->path = wn_strdup_(path);
    if (entry->path == NULL) {
        wgpuBindGroupRelease(entry->bind);
        wgpuTextureViewRelease(entry->view);
        wgpuTextureRelease(entry->texture);
        set_error(gpu, "out of memory caching image '%s'", path);
        return NULL;
    }
    entry->width = width;
    entry->height = height;
    ++gpu->image_count;
    return entry;
}

static int renderer_upload(void *context, const wn_display_list *list)
{
    WnWgpu *gpu = (WnWgpu *)context;
    size_t bytes;
    size_t glyph_bytes;
    if (gpu == NULL || list == NULL) {
        return 1;
    }
    gpu->uploaded_vertices = 0;
    gpu->uploaded_glyphs = 0;
    gpu->uploaded_images = 0;

    if (list->atlas.pixels != NULL) {
        if (upload_atlas(gpu, &list->atlas) != 0) {
            return 1;
        }
    }

    if (list->vertex_count > 0) {
        bytes = (size_t)list->vertex_count * sizeof(wn_vertex);
        if (ensure_buffer(gpu, &gpu->vertex_buffer, &gpu->vertex_capacity, bytes,
                          WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst,
                          "wn-vertices") != 0) {
            return 1;
        }
        wgpuQueueWriteBuffer(gpu->queue, gpu->vertex_buffer, 0, list->vertices, bytes);
        gpu->uploaded_vertices = list->vertex_count;
    }

    if (list->glyph_vertex_count > 0) {
        glyph_bytes = (size_t)list->glyph_vertex_count * sizeof(wn_glyph_vertex);
        if (ensure_buffer(gpu, &gpu->glyph_buffer, &gpu->glyph_capacity, glyph_bytes,
                          WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst,
                          "wn-glyph-vertices") != 0) {
            return 1;
        }
        wgpuQueueWriteBuffer(gpu->queue, gpu->glyph_buffer, 0,
                             list->glyph_vertices, glyph_bytes);
        gpu->uploaded_glyphs = list->glyph_vertex_count;
    }

    if (list->image_vertex_count > 0) {
        size_t image_bytes = (size_t)list->image_vertex_count * sizeof(wn_image_vertex);
        int b;
        if (ensure_buffer(gpu, &gpu->image_buffer, &gpu->image_capacity, image_bytes,
                          WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst,
                          "wn-image-vertices") != 0) {
            return 1;
        }
        wgpuQueueWriteBuffer(gpu->queue, gpu->image_buffer, 0,
                             list->image_vertices, image_bytes);
        gpu->uploaded_images = list->image_vertex_count;
        for (b = 0; b < list->image_batch_count; ++b) {
            const wn_image_batch *batch = &list->image_batches[b];
            if (batch->src == NULL) {
                continue;
            }
            if (ensure_image_texture(gpu, batch->src) == NULL) {
                return 1;
            }
        }
    } else {
        gpu->uploaded_images = 0;
    }
    return 0;
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

static int create_offscreen_target(WnWgpu *gpu)
{
    WGPUTextureDescriptor desc;
    if (gpu == NULL || gpu->device == NULL) {
        return 1;
    }
    if (gpu->target_view != NULL) {
        wgpuTextureViewRelease(gpu->target_view);
        gpu->target_view = NULL;
    }
    if (gpu->target != NULL) {
        wgpuTextureRelease(gpu->target);
        gpu->target = NULL;
    }
    memset(&desc, 0, sizeof(desc));
    desc.label = wsv("wn-offscreen");
    desc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size.width = (uint32_t)(gpu->width > 0 ? gpu->width : 1);
    desc.size.height = (uint32_t)(gpu->height > 0 ? gpu->height : 1);
    desc.size.depthOrArrayLayers = 1;
    desc.format = gpu->format;
    desc.mipLevelCount = 1;
    desc.sampleCount = 1;
    gpu->target = wgpuDeviceCreateTexture(gpu->device, &desc);
    if (gpu->target == NULL) {
        set_error(gpu, "offscreen texture creation failed");
        return 1;
    }
    gpu->target_view = wgpuTextureCreateView(gpu->target, NULL);
    if (gpu->target_view == NULL) {
        set_error(gpu, "offscreen texture view creation failed");
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------- offscreen readback */

typedef struct {
    volatile int done;
    WGPUMapAsyncStatus status;
} WnMapResult;

static void on_map(WGPUMapAsyncStatus status, WGPUStringView message, void *userdata1,
                   void *userdata2)
{
    WnMapResult *result = (WnMapResult *)userdata1;
    (void)message;
    (void)userdata2;
    if (result != NULL) {
        result->status = status;
        result->done = 1;
    }
}

static int write_png_bgra_file(WnWgpu *gpu, const char *path, const unsigned char *pixels,
                               int width, int height, unsigned int stride)
{
    wchar_t wide[1024];
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IWICBitmap *bitmap = NULL;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    HRESULT hr;
    int result = 1;

    if (gpu->wic == NULL) {
        hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                             &IID_IWICImagingFactory, (void **)&gpu->wic);
        if (FAILED(hr) || gpu->wic == NULL) {
            return 1;
        }
    }
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide,
                           (int)(sizeof(wide) / sizeof(wide[0]))) <= 0) {
        return 1;
    }
    hr = gpu->wic->lpVtbl->CreateStream(gpu->wic, &stream);
    if (FAILED(hr)) goto fail;
    hr = stream->lpVtbl->InitializeFromFilename(stream, wide, GENERIC_WRITE);
    if (FAILED(hr)) goto fail;
    hr = gpu->wic->lpVtbl->CreateEncoder(gpu->wic, &GUID_ContainerFormatPng, NULL,
                                        &encoder);
    if (FAILED(hr)) goto fail;
    hr = encoder->lpVtbl->Initialize(encoder, (IStream *)stream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) goto fail;
    hr = encoder->lpVtbl->CreateNewFrame(encoder, &frame, NULL);
    if (FAILED(hr)) goto fail;
    hr = frame->lpVtbl->Initialize(frame, NULL);
    if (FAILED(hr)) goto fail;
    hr = frame->lpVtbl->SetSize(frame, (UINT)width, (UINT)height);
    if (FAILED(hr)) goto fail;
    hr = frame->lpVtbl->SetPixelFormat(frame, &format);
    if (FAILED(hr)) goto fail;
    hr = gpu->wic->lpVtbl->CreateBitmapFromMemory(
        gpu->wic, (UINT)width, (UINT)height, &GUID_WICPixelFormat32bppBGRA, stride,
        stride * (UINT)height, (BYTE *)pixels, &bitmap);
    if (FAILED(hr)) goto fail;
    hr = frame->lpVtbl->WriteSource(frame, (IWICBitmapSource *)bitmap, NULL);
    if (FAILED(hr)) goto fail;
    hr = frame->lpVtbl->Commit(frame);
    if (FAILED(hr)) goto fail;
    hr = encoder->lpVtbl->Commit(encoder);
    if (FAILED(hr)) goto fail;
    result = 0;

fail:
    if (bitmap != NULL) bitmap->lpVtbl->Release(bitmap);
    if (frame != NULL) frame->lpVtbl->Release(frame);
    if (encoder != NULL) encoder->lpVtbl->Release(encoder);
    if (stream != NULL) stream->lpVtbl->Release(stream);
    return result;
}

static int renderer_readback(void *context, const char *path)
{
    WnWgpu *gpu = (WnWgpu *)context;
    WGPUBufferDescriptor buffer_desc;
    WGPUTexelCopyTextureInfo source;
    WGPUTexelCopyBufferInfo destination;
    WGPUExtent3D extent;
    WGPUCommandEncoder encoder;
    WGPUCommandBuffer command;
    WGPUBufferMapCallbackInfo callback_info;
    WnMapResult map_result;
    WGPUBuffer buffer = NULL;
    void *mapped;
    unsigned int stride;
    size_t bytes;
    int rc = 1;

    if (gpu == NULL || path == NULL || path[0] == '\0' || !gpu->offscreen) {
        return 1;
    }
    if (gpu->target == NULL) {
        set_error(gpu, "readback before render");
        return 1;
    }
    stride = ((unsigned int)gpu->width * 4u + 255u) & ~255u;
    bytes = (size_t)stride * (size_t)gpu->height;

    memset(&buffer_desc, 0, sizeof(buffer_desc));
    buffer_desc.label = wsv("wn-readback");
    buffer_desc.size = (uint64_t)bytes;
    buffer_desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    buffer = wgpuDeviceCreateBuffer(gpu->device, &buffer_desc);
    if (buffer == NULL) {
        set_error(gpu, "readback buffer creation failed");
        return 1;
    }

    memset(&source, 0, sizeof(source));
    source.texture = gpu->target;
    source.mipLevel = 0;
    source.origin.x = 0;
    source.origin.y = 0;
    source.origin.z = 0;
    source.aspect = WGPUTextureAspect_All;

    memset(&destination, 0, sizeof(destination));
    destination.buffer = buffer;
    destination.layout.offset = 0;
    destination.layout.bytesPerRow = stride;
    destination.layout.rowsPerImage = (uint32_t)gpu->height;

    memset(&extent, 0, sizeof(extent));
    extent.width = (uint32_t)gpu->width;
    extent.height = (uint32_t)gpu->height;
    extent.depthOrArrayLayers = 1;

    encoder = wgpuDeviceCreateCommandEncoder(gpu->device, NULL);
    if (encoder == NULL) {
        set_error(gpu, "readback encoder creation failed");
        goto done;
    }
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &source, &destination, &extent);
    command = wgpuCommandEncoderFinish(encoder, NULL);
    wgpuCommandEncoderRelease(encoder);
    if (command == NULL) {
        set_error(gpu, "readback command buffer failed");
        goto done;
    }
    wgpuQueueSubmit(gpu->queue, 1, &command);
    wgpuCommandBufferRelease(command);

    memset(&map_result, 0, sizeof(map_result));
    memset(&callback_info, 0, sizeof(callback_info));
    callback_info.mode = WGPUCallbackMode_AllowProcessEvents;
    callback_info.callback = on_map;
    callback_info.userdata1 = &map_result;
    (void)wgpuBufferMapAsync(buffer, WGPUMapMode_Read, 0, bytes, callback_info);
    if (map_result.done == 0) {
        DWORD start = GetTickCount();
        while (!map_result.done) {
            wgpuInstanceProcessEvents(gpu->instance);
            if (GetTickCount() - start > 30000u) {
                set_error(gpu, "readback map timed out");
                goto done;
            }
            Sleep(1);
        }
    }
    if (map_result.status != WGPUMapAsyncStatus_Success) {
        set_error(gpu, "readback map failed (status %d)", (int)map_result.status);
        goto done;
    }
    mapped = wgpuBufferGetMappedRange(buffer, 0, bytes);
    if (mapped == NULL) {
        set_error(gpu, "readback mapped range is NULL");
        goto done;
    }
    rc = write_png_bgra_file(gpu, path, (const unsigned char *)mapped, gpu->width,
                             gpu->height, stride);
    wgpuBufferUnmap(buffer);

done:
    if (buffer != NULL) {
        wgpuBufferRelease(buffer);
    }
    return rc;
}

static int renderer_render(void *context, const wn_display_list *list)
{
    WnWgpu *gpu = (WnWgpu *)context;
    WGPUSurfaceTexture surface_texture;
    memset(&surface_texture, 0, sizeof(surface_texture));
    WGPUTextureView view = NULL;
    WGPUCommandEncoder encoder = NULL;
    WGPUCommandBuffer command = NULL;
    WGPURenderPassColorAttachment attachment;
    WGPURenderPassDescriptor pass_desc;
    WGPURenderPassEncoder pass = NULL;
    float clear[4] = {0.043137f, 0.054902f, 0.078431f, 1.0f};
    double t0;
    double cpu_ms;
    double present_ms = 0.0;
    int attempt;

    if (gpu == NULL) {
        return 1;
    }
    if (gpu->width <= 0 || gpu->height <= 0) {
        return 0;
    }
    if (list != NULL) {
        clear[0] = list->clear_color[0];
        clear[1] = list->clear_color[1];
        clear[2] = list->clear_color[2];
        clear[3] = list->clear_color[3];
    }
    if (!gpu->configured && !gpu->offscreen) {
        configure_surface(gpu);
    }

    if (gpu->offscreen) {
        if (gpu->target_view == NULL && create_offscreen_target(gpu) != 0) {
            return 1;
        }
    } else {
        for (attempt = 0; attempt < 2; ++attempt) {
            memset(&surface_texture, 0, sizeof(surface_texture));
            wgpuSurfaceGetCurrentTexture(gpu->surface, &surface_texture);
            if (surface_texture.status == WGPUSurfaceGetCurrentTextureStatus_Outdated ||
                surface_texture.status == WGPUSurfaceGetCurrentTextureStatus_Lost) {
                configure_surface(gpu);
                continue;
            }
            if (surface_texture.status == WGPUSurfaceGetCurrentTextureStatus_Timeout) {
                return 0;
            }
            if (surface_texture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
                surface_texture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
                set_error(gpu, "getCurrentTexture status %d", (int)surface_texture.status);
                return 1;
            }
            break;
        }
        if (attempt == 2) {
            set_error(gpu, "surface texture unavailable after reconfigure");
            return 1;
        }
    }
    t0 = wn_now_ms();

    view = gpu->offscreen ? gpu->target_view
                          : wgpuTextureCreateView(surface_texture.texture, NULL);
    if (view == NULL) {
        if (!gpu->offscreen) {
            wgpuTextureRelease(surface_texture.texture);
        }
        set_error(gpu, "surface texture view creation failed");
        return 1;
    }
    encoder = wgpuDeviceCreateCommandEncoder(gpu->device, NULL);
    if (encoder == NULL) {
        if (!gpu->offscreen) {
            wgpuTextureViewRelease(view);
            wgpuTextureRelease(surface_texture.texture);
        }
        set_error(gpu, "command encoder creation failed");
        return 1;
    }

    memset(&attachment, 0, sizeof(attachment));
    attachment.view = view;
    attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    attachment.loadOp = WGPULoadOp_Clear;
    attachment.storeOp = WGPUStoreOp_Store;
    attachment.clearValue.r = clear[0];
    attachment.clearValue.g = clear[1];
    attachment.clearValue.b = clear[2];
    attachment.clearValue.a = clear[3];
    memset(&pass_desc, 0, sizeof(pass_desc));
    pass_desc.label = wsv("wn-pass");
    pass_desc.colorAttachmentCount = 1;
    pass_desc.colorAttachments = &attachment;

    pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
    if (pass != NULL) {
        if (gpu->uploaded_vertices > 0) {
            int b;
            wgpuRenderPassEncoderSetPipeline(pass, gpu->pipeline);
            wgpuRenderPassEncoderSetVertexBuffer(pass, 0, gpu->vertex_buffer, 0,
                                                 WGPU_WHOLE_SIZE);
            if (list != NULL && list->solid_batch_count > 0) {
                for (b = 0; b < list->solid_batch_count; ++b) {
                    const wn_draw_batch *batch = &list->solid_batches[b];
                    if (set_scissor(pass, batch->clip_x, batch->clip_y,
                                    batch->clip_w, batch->clip_h,
                                    gpu->width, gpu->height)) {
                        wgpuRenderPassEncoderDraw(pass, (uint32_t)batch->count, 1,
                                                  (uint32_t)batch->first, 0);
                    }
                }
            } else {
                wgpuRenderPassEncoderDraw(pass, (uint32_t)gpu->uploaded_vertices, 1, 0, 0);
            }
        }
        if (gpu->uploaded_images > 0 && gpu->image_pipeline != NULL && list != NULL &&
            list->image_batch_count > 0) {
            int b;
            wgpuRenderPassEncoderSetPipeline(pass, gpu->image_pipeline);
            wgpuRenderPassEncoderSetVertexBuffer(pass, 0, gpu->image_buffer, 0,
                                                 WGPU_WHOLE_SIZE);
            for (b = 0; b < list->image_batch_count; ++b) {
                const wn_image_batch *batch = &list->image_batches[b];
                WnImageTexture *texture = batch->src != NULL
                                              ? find_cached_image(gpu, batch->src)
                                              : NULL;
                if (texture == NULL || texture->bind == NULL) {
                    continue;
                }
                if (!set_scissor(pass, batch->clip_x, batch->clip_y,
                                 batch->clip_w, batch->clip_h,
                                 gpu->width, gpu->height)) {
                    continue;
                }
                wgpuRenderPassEncoderSetBindGroup(pass, 0, texture->bind, 0, NULL);
                wgpuRenderPassEncoderDraw(pass, (uint32_t)batch->count, 1,
                                          (uint32_t)batch->first, 0);
            }
        }
        if (gpu->uploaded_glyphs > 0 && gpu->text_pipeline != NULL &&
            gpu->text_bg != NULL) {
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
                                    gpu->width, gpu->height)) {
                        wgpuRenderPassEncoderDraw(pass, (uint32_t)batch->count, 1,
                                                  (uint32_t)batch->first, 0);
                    }
                }
            } else {
                wgpuRenderPassEncoderDraw(pass, (uint32_t)gpu->uploaded_glyphs, 1, 0, 0);
            }
        }
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
    }

    command = wgpuCommandEncoderFinish(encoder, NULL);
    if (command == NULL) {
        wgpuCommandEncoderRelease(encoder);
        wgpuTextureViewRelease(view);
        wgpuTextureRelease(surface_texture.texture);
        set_error(gpu, "command encoder finish failed");
        return 1;
    }
    {
        wgpuQueueSubmit(gpu->queue, 1, &command);
        wgpuCommandBufferRelease(command);
    }
    wgpuCommandEncoderRelease(encoder);
    cpu_ms = wn_now_ms() - t0;

    if (!gpu->offscreen) {
        double p0 = wn_now_ms();
        (void)wgpuSurfacePresent(gpu->surface);
        present_ms = wn_now_ms() - p0;
        (void)p0;
        wgpuTextureViewRelease(view);
        wgpuTextureRelease(surface_texture.texture);
    }

    gpu->stats.frames += 1;
    gpu->stats.last_cpu_ms = cpu_ms;
    gpu->stats.last_present_ms = present_ms;
    return 0;
}

static void renderer_destroy(void *context)
{
    WnWgpu *gpu = (WnWgpu *)context;
    int i;
    if (gpu == NULL) {
        return;
    }
    if (gpu->vertex_buffer != NULL) wgpuBufferRelease(gpu->vertex_buffer);
    if (gpu->glyph_buffer != NULL) wgpuBufferRelease(gpu->glyph_buffer);
    if (gpu->image_buffer != NULL) wgpuBufferRelease(gpu->image_buffer);
    if (gpu->target_view != NULL) wgpuTextureViewRelease(gpu->target_view);
    if (gpu->target != NULL) wgpuTextureRelease(gpu->target);
    for (i = 0; i < gpu->image_count; ++i) {
        if (gpu->images[i].bind != NULL) wgpuBindGroupRelease(gpu->images[i].bind);
        if (gpu->images[i].view != NULL) wgpuTextureViewRelease(gpu->images[i].view);
        if (gpu->images[i].texture != NULL) wgpuTextureRelease(gpu->images[i].texture);
        wn_free_(gpu->images[i].path);
    }
    wn_free_(gpu->images);
    if (gpu->wic != NULL) gpu->wic->lpVtbl->Release(gpu->wic);
    if (gpu->text_bg != NULL) wgpuBindGroupRelease(gpu->text_bg);
    if (gpu->text_pl != NULL) wgpuPipelineLayoutRelease(gpu->text_pl);
    if (gpu->text_bgl != NULL) wgpuBindGroupLayoutRelease(gpu->text_bgl);
    if (gpu->sampler != NULL) wgpuSamplerRelease(gpu->sampler);
    if (gpu->atlas_view != NULL) wgpuTextureViewRelease(gpu->atlas_view);
    if (gpu->atlas_texture != NULL) wgpuTextureRelease(gpu->atlas_texture);
    if (gpu->image_pipeline != NULL) wgpuRenderPipelineRelease(gpu->image_pipeline);
    if (gpu->image_shader != NULL) wgpuShaderModuleRelease(gpu->image_shader);
    if (gpu->text_pipeline != NULL) wgpuRenderPipelineRelease(gpu->text_pipeline);
    if (gpu->text_shader != NULL) wgpuShaderModuleRelease(gpu->text_shader);
    if (gpu->pipeline != NULL) wgpuRenderPipelineRelease(gpu->pipeline);
    if (gpu->shader != NULL) wgpuShaderModuleRelease(gpu->shader);
    if (gpu->surface != NULL) wgpuSurfaceRelease(gpu->surface);
    if (gpu->queue != NULL) wgpuQueueRelease(gpu->queue);
    if (gpu->device != NULL) wgpuDeviceRelease(gpu->device);
    if (gpu->adapter != NULL) wgpuAdapterRelease(gpu->adapter);
    if (gpu->instance != NULL) wgpuInstanceRelease(gpu->instance);
    if (gpu->com_initialized) CoUninitialize();
    free(gpu);
}

static const char *renderer_last_error(void *context)
{
    WnWgpu *gpu = (WnWgpu *)context;
    if (gpu == NULL || gpu->error[0] == '\0') {
        return NULL;
    }
    return gpu->error;
}

static void renderer_get_info(void *context, wn_renderer_info *info)
{
    WnWgpu *gpu = (WnWgpu *)context;
    if (gpu != NULL && info != NULL) {
        *info = gpu->info;
    }
}

static void renderer_get_stats(void *context, wn_renderer_stats *stats)
{
    WnWgpu *gpu = (WnWgpu *)context;
    if (gpu != NULL && stats != NULL) {
        *stats = gpu->stats;
    }
}

static int renderer_image_cache_count(void *context)
{
    WnWgpu *gpu = (WnWgpu *)context;
    return gpu != NULL ? gpu->image_count : -1;
}

static const wn_renderer_ops g_wgpu_ops = {
    "wgpu-native",
    renderer_init,
    renderer_resize,
    renderer_upload,
    renderer_render,
    renderer_destroy,
    renderer_last_error,
    renderer_get_info,
    renderer_get_stats,
    renderer_readback,
    renderer_image_cache_count
};

const wn_renderer_ops *wn_renderer_wgpu(void)
{
    return &g_wgpu_ops;
}
