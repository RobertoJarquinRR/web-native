/*
 * runtime_gpu.c - native GPU renderer for the AOT-baked scene (exp10).
 *
 * Pipeline:  display_list.json -> emit_c.py -> scene_data.h
 *            (clip-space vertices + generated WGSL)
 *            -> wgpu-native (webgpu.h C API) -> D3D12 / Vulkan / Metal
 *
 * There is NO JSON parser, NO Python and NO browser here. The scene is an
 * array of SceneVertex baked at build time; this file is a generic consumer:
 * it allocates a vertex buffer, uploads SCENE_VERTEX_COUNT vertices and draws
 * them with ONE draw call inside a single render pass.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include <wincodec.h>
#include <wgpu.h>

#include "scene_data.h"

#define ARRAY_LEN(a) ((size_t)(sizeof(a) / sizeof((a)[0])))

/* The emitter and the pipeline layout must agree on the packed vertex stride. */
_Static_assert(sizeof(SceneVertex) == SCENE_VERTEX_STRIDE,
               "SceneVertex must stay 12 bytes (vec2 + unorm8x4)");

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
    const wchar_t *out_path = L"scene_gpu.png";
    wchar_t wide_path[MAX_PATH];
    int exit_code = 1;

    WGPUInstance instance = NULL;
    WGPUAdapter adapter = NULL;
    WGPUDevice device = NULL;
    WGPUQueue queue = NULL;
    WGPUShaderModule shader = NULL;
    WGPUTexture texture = NULL;
    WGPUTextureView view = NULL;
    WGPUBuffer vertex_buffer = NULL;
    WGPUBuffer readback = NULL;
    WGPUCommandEncoder encoder = NULL;
    WGPUCommandBuffer commands = NULL;
    WGPURenderPassEncoder pass = NULL;
    WGPURenderPipeline pipeline = NULL;

    AdapterResult adapter_result;
    DeviceResult device_result;
    MapResult map_result;

    WGPUAdapterInfo info;
    WGPUStatus status;
    unsigned int bytes_per_row;
    uint64_t readback_size;
    const unsigned char *mapped = NULL;
    double t_start, t_init, t_upload, t_pass, t_read, t_png;

    if (argc > 1) {
        if (MultiByteToWideChar(CP_ACP, 0, argv[1], -1, wide_path, MAX_PATH) == 0) {
            fprintf(stderr, "error: output path too long\n");
            return 1;
        }
        out_path = wide_path;
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    wgpuSetLogLevel(WGPULogLevel_Info);
    wgpuSetLogCallback(on_log, NULL);

    printf("scene_gpu: AOT GPU scene %dx%d, %d rect op(s), %d vertices\n",
           SCENE_WIDTH, SCENE_HEIGHT, SCENE_OP_COUNT, SCENE_VERTEX_COUNT);
    for (int i = 0; i < SCENE_OP_COUNT; ++i) {
        const RectOp *op = &kSceneOps[i];
        printf("  op[%d] rect x=%.1f y=%.1f w=%.1f h=%.1f rgba=0x%08X\n",
               i, op->x, op->y, op->w, op->h, op->rgba);
    }
    printf("scene_gpu: clear color rgb(%d,%d,%d)\n",
           (int)(SCENE_CLEAR_R * 255.0f + 0.5f),
           (int)(SCENE_CLEAR_G * 255.0f + 0.5f),
           (int)(SCENE_CLEAR_B * 255.0f + 0.5f));

    if (SCENE_VERTEX_COUNT != SCENE_OP_COUNT * 6) {
        fprintf(stderr, "error: vertex/op mismatch in generated header\n");
        return 1;
    }

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
    print_view("adapter: vendor=", info.vendor);
    printf("\n");
    print_view("adapter: architecture=", info.architecture);
    printf("\n");
    print_view("adapter: device=", info.device);
    printf("\n");
    print_view("adapter: description=", info.description);
    printf("\n");

    memset(&device_result, 0, sizeof(device_result));
    {
        WGPUDeviceDescriptor descriptor;
        WGPURequestDeviceCallbackInfo cb;
        memset(&descriptor, 0, sizeof(descriptor));
        memset(&cb, 0, sizeof(cb));
        descriptor.label = sv("webnative-exp10-device");
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

    /* Vertex buffer: baked clip-space geometry, 12 bytes/vertex. */
    {
        WGPUBufferDescriptor descriptor;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("scene-vertices");
        descriptor.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        descriptor.size = sizeof(kSceneVertices);
        vertex_buffer = wgpuDeviceCreateBuffer(device, &descriptor);
        if (vertex_buffer == NULL) {
            fprintf(stderr, "error: wgpuDeviceCreateBuffer(vertices) failed\n");
            goto cleanup;
        }
        wgpuQueueWriteBuffer(queue, vertex_buffer, 0, kSceneVertices, sizeof(kSceneVertices));
    }

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
        texture = wgpuDeviceCreateTexture(device, &descriptor);
        if (texture == NULL) {
            fprintf(stderr, "error: wgpuDeviceCreateTexture failed\n");
            goto cleanup;
        }
        view = wgpuTextureCreateView(texture, NULL);
        if (view == NULL) {
            fprintf(stderr, "error: wgpuTextureCreateView failed\n");
            goto cleanup;
        }
    }

    /* Shader module from the generated WGSL baked into scene_data.h. */
    {
        WGPUShaderModuleDescriptor descriptor;
        WGPUShaderSourceWGSL wgsl;
        memset(&descriptor, 0, sizeof(descriptor));
        memset(&wgsl, 0, sizeof(wgsl));
        wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
        wgsl.code = sv(kSceneShaderWGSL);
        descriptor.label = sv("scene-shader");
        descriptor.nextInChain = &wgsl.chain;
        shader = wgpuDeviceCreateShaderModule(device, &descriptor);
        if (shader == NULL) {
            fprintf(stderr, "error: wgpuDeviceCreateShaderModule failed\n");
            goto cleanup;
        }
    }

    /* Pipeline: one interleaved vertex buffer, vec2 pos + unorm8x4 color. */
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
        buffer_layout.attributeCount = ARRAY_LEN(attributes);
        buffer_layout.attributes = attributes;

        memset(&blend, 0, sizeof(blend));
        blend.color.operation = WGPUBlendOperation_Add;
        blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
        blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
        blend.alpha.operation = WGPUBlendOperation_Add;
        blend.alpha.srcFactor = WGPUBlendFactor_One;
        blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;

        memset(&target, 0, sizeof(target));
        target.format = WGPUTextureFormat_RGBA8Unorm;
        target.blend = &blend;
        target.writeMask = WGPUColorWriteMask_All;

        memset(&fragment, 0, sizeof(fragment));
        fragment.module = shader;
        fragment.entryPoint = sv("fs_main");
        fragment.targetCount = 1;
        fragment.targets = &target;

        memset(&vertex, 0, sizeof(vertex));
        vertex.module = shader;
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
        descriptor.label = sv("scene-pipeline");
        descriptor.layout = NULL;
        descriptor.vertex = vertex;
        descriptor.primitive = primitive;
        descriptor.depthStencil = NULL;
        descriptor.multisample = multisample;
        descriptor.fragment = &fragment;
        pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
        if (pipeline == NULL) {
            fprintf(stderr, "error: wgpuDeviceCreateRenderPipeline failed\n");
            goto cleanup;
        }
    }

    t_upload = now_ms();

    /* Single render pass, single draw call. */
    encoder = wgpuDeviceCreateCommandEncoder(device, NULL);
    if (encoder == NULL) {
        fprintf(stderr, "error: wgpuDeviceCreateCommandEncoder failed\n");
        goto cleanup;
    }
    {
        WGPURenderPassColorAttachment attachment;
        WGPURenderPassDescriptor descriptor;

        memset(&attachment, 0, sizeof(attachment));
        attachment.view = view;
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
        wgpuRenderPassEncoderSetPipeline(pass, pipeline);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 0, vertex_buffer, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderDraw(pass, SCENE_VERTEX_COUNT, 1, 0, 0);
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
        source.texture = texture;
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

    t_pass = now_ms();

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

    wprintf(L"scene_gpu: wrote %ls (%dx%d)\n", out_path, SCENE_WIDTH, SCENE_HEIGHT);
    printf("timings: init+adapter+device+upload=%.2fms | pipeline=%.2fms | "
           "render+copy+submit=%.2fms | map=%.2fms | png=%.2fms | total=%.2fms\n",
           t_init - t_start, t_upload - t_init, t_pass - t_upload,
           t_read - t_pass, t_png - t_read, t_png - t_start);
    exit_code = 0;

cleanup:
    if (pass != NULL) wgpuRenderPassEncoderRelease(pass);
    if (commands != NULL) wgpuCommandBufferRelease(commands);
    if (encoder != NULL) wgpuCommandEncoderRelease(encoder);
    if (pipeline != NULL) wgpuRenderPipelineRelease(pipeline);
    if (shader != NULL) wgpuShaderModuleRelease(shader);
    if (readback != NULL) wgpuBufferRelease(readback);
    if (vertex_buffer != NULL) wgpuBufferRelease(vertex_buffer);
    if (view != NULL) wgpuTextureViewRelease(view);
    if (texture != NULL) wgpuTextureRelease(texture);
    if (queue != NULL) wgpuQueueRelease(queue);
    if (device != NULL) wgpuDeviceRelease(device);
    if (adapter != NULL) wgpuAdapterRelease(adapter);
    if (instance != NULL) wgpuInstanceRelease(instance);
    return exit_code;
}
