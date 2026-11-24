/*
 * renderer_wgpu.c - wgpu-native renderer backend behind wn_renderer_ops.
 *
 * This is the R2 wgpu code (solid pipeline + R8 text pipeline + surface
 * swapchain) moved behind the renderer vtable. wgpu types stay in this file.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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

    int uploaded_vertices;
    int uploaded_glyphs;

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
    if (surface == NULL || surface->kind != WN_SURFACE_HWND || surface->handle == NULL) {
        return 1;
    }
    gpu = (WnWgpu *)calloc(1, sizeof(*gpu));
    if (gpu == NULL) {
        return 1;
    }
    gpu->width = width > 0 ? width : 1;
    gpu->height = height > 0 ? height : 1;
    gpu->format = WGPUTextureFormat_BGRA8Unorm;
    gpu->info.width = gpu->width;
    gpu->info.height = gpu->height;

    wgpuSetLogLevel(WGPULogLevel_Warn);

    gpu->instance = wgpuCreateInstance(NULL);
    if (gpu->instance == NULL) {
        set_error(gpu, "wgpuCreateInstance returned NULL");
        goto fail;
    }

    {
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

    configure_surface(gpu);
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
    if (gpu->width == width && gpu->height == height && gpu->configured) {
        return;
    }
    gpu->width = width;
    gpu->height = height;
    gpu->info.width = width;
    gpu->info.height = height;
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
    if (gpu->atlas_texture != NULL && gpu->atlas_ptr == atlas->pixels &&
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
    return 0;
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
    return 0;
}

static int renderer_render(void *context, const wn_display_list *list)
{
    WnWgpu *gpu = (WnWgpu *)context;
    WGPUSurfaceTexture surface_texture;
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
    if (!gpu->configured) {
        configure_surface(gpu);
    }

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
    t0 = wn_now_ms();

    view = wgpuTextureCreateView(surface_texture.texture, NULL);
    if (view == NULL) {
        wgpuTextureRelease(surface_texture.texture);
        set_error(gpu, "surface texture view creation failed");
        return 1;
    }
    encoder = wgpuDeviceCreateCommandEncoder(gpu->device, NULL);
    if (encoder == NULL) {
        wgpuTextureViewRelease(view);
        wgpuTextureRelease(surface_texture.texture);
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
            wgpuRenderPassEncoderSetPipeline(pass, gpu->pipeline);
            wgpuRenderPassEncoderSetVertexBuffer(pass, 0, gpu->vertex_buffer, 0,
                                                 WGPU_WHOLE_SIZE);
            wgpuRenderPassEncoderDraw(pass, (uint32_t)gpu->uploaded_vertices, 1, 0, 0);
        }
        if (gpu->uploaded_glyphs > 0 && gpu->text_pipeline != NULL &&
            gpu->text_bg != NULL) {
            wgpuRenderPassEncoderSetPipeline(pass, gpu->text_pipeline);
            wgpuRenderPassEncoderSetVertexBuffer(pass, 0, gpu->glyph_buffer, 0,
                                                 WGPU_WHOLE_SIZE);
            wgpuRenderPassEncoderSetBindGroup(pass, 0, gpu->text_bg, 0, NULL);
            wgpuRenderPassEncoderDraw(pass, (uint32_t)gpu->uploaded_glyphs, 1, 0, 0);
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

    {
        double p0 = wn_now_ms();
        (void)wgpuSurfacePresent(gpu->surface);
        present_ms = wn_now_ms() - p0;
        (void)p0;
    }

    wgpuTextureViewRelease(view);
    wgpuTextureRelease(surface_texture.texture);

    gpu->stats.frames += 1;
    gpu->stats.last_cpu_ms = cpu_ms;
    gpu->stats.last_present_ms = present_ms;
    return 0;
}

static void renderer_destroy(void *context)
{
    WnWgpu *gpu = (WnWgpu *)context;
    if (gpu == NULL) {
        return;
    }
    if (gpu->vertex_buffer != NULL) wgpuBufferRelease(gpu->vertex_buffer);
    if (gpu->glyph_buffer != NULL) wgpuBufferRelease(gpu->glyph_buffer);
    if (gpu->text_bg != NULL) wgpuBindGroupRelease(gpu->text_bg);
    if (gpu->text_pl != NULL) wgpuPipelineLayoutRelease(gpu->text_pl);
    if (gpu->text_bgl != NULL) wgpuBindGroupLayoutRelease(gpu->text_bgl);
    if (gpu->sampler != NULL) wgpuSamplerRelease(gpu->sampler);
    if (gpu->atlas_view != NULL) wgpuTextureViewRelease(gpu->atlas_view);
    if (gpu->atlas_texture != NULL) wgpuTextureRelease(gpu->atlas_texture);
    if (gpu->text_pipeline != NULL) wgpuRenderPipelineRelease(gpu->text_pipeline);
    if (gpu->text_shader != NULL) wgpuShaderModuleRelease(gpu->text_shader);
    if (gpu->pipeline != NULL) wgpuRenderPipelineRelease(gpu->pipeline);
    if (gpu->shader != NULL) wgpuShaderModuleRelease(gpu->shader);
    if (gpu->surface != NULL) wgpuSurfaceRelease(gpu->surface);
    if (gpu->queue != NULL) wgpuQueueRelease(gpu->queue);
    if (gpu->device != NULL) wgpuDeviceRelease(gpu->device);
    if (gpu->adapter != NULL) wgpuAdapterRelease(gpu->adapter);
    if (gpu->instance != NULL) wgpuInstanceRelease(gpu->instance);
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

static const wn_renderer_ops g_wgpu_ops = {
    "wgpu-native",
    renderer_init,
    renderer_resize,
    renderer_upload,
    renderer_render,
    renderer_destroy,
    renderer_last_error,
    renderer_get_info,
    renderer_get_stats
};

const wn_renderer_ops *wn_renderer_wgpu(void)
{
    return &g_wgpu_ops;
}
