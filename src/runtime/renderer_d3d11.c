/*
 * renderer_d3d11.c - Direct3D 11 renderer backend behind wn_renderer_ops.
 *
 * Renders the wn_display_list (solid vertices, R8 glyph atlas + glyph
 * vertices and RGBA image batches) into a DXGI swapchain bound to an HWND,
 * using D3D11 hardware by default and WARP only as an explicit fallback. No
 * D3D/DXGI type crosses the vtable.
 *
 * Shaders are HLSL compiled at runtime with D3DCompile(); images are decoded
 * through WIC and cached by path. Vertex data uses the exact engine structs:
 *   wn_vertex        12 bytes (R32G32_FLOAT + R8G8B8A8_UNORM)
 *   wn_glyph_vertex  20 bytes (+ R32G32_FLOAT uv)
 *   wn_image_vertex  20 bytes (R32G32_FLOAT + R32G32_FLOAT + R32_FLOAT alpha)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
/* Define the IIDs/CLSIDs used below in this TU (IID_ID3D11Texture2D and
 * IID_IDXGIFactory2 are not exported by the import libraries on every SDK). */
#include <initguid.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wincodec.h>

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wn_renderer.h"
#include "wn_internal.h"

_Static_assert(sizeof(wn_vertex) == 12, "wn_vertex must stay 12 bytes");
_Static_assert(sizeof(wn_glyph_vertex) == 20, "wn_glyph_vertex must stay 20 bytes");
_Static_assert(sizeof(wn_image_vertex) == 20, "wn_image_vertex must stay 20 bytes");

/* Decoded local image cached by path (PNG/JPEG via WIC -> RGBA8). */
typedef struct {
    char *path;
    ID3D11Texture2D *texture;
    ID3D11ShaderResourceView *view;
    int width;
    int height;
    size_t bytes;                 /* decoded RGBA bytes (LRU accounting) */
    unsigned long long last_used; /* monotonic touch counter */
    unsigned long long frame;     /* last frame that referenced the entry */
} WnD3D11Image;

/* One viewport-sized offscreen target for a nested group layer. */
typedef struct {
    ID3D11Texture2D *texture;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;
} WnD3D11LayerTarget;

typedef struct {
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    IDXGISwapChain1 *swapchain;
    DXGI_FORMAT format;
    D3D_FEATURE_LEVEL feature_level;
    int driver_hardware;
    int flip_model;
    UINT swapchain_flags;
    int offscreen;   /* 1 when rendering into a texture (no swapchain) */
    int width;
    int height;

    ID3D11Texture2D *backbuffer;
    ID3D11RenderTargetView *rtv;

    /* Group layers (blob v3 opacity): one pooled target per nesting depth. */
    WnD3D11LayerTarget *layer_targets;
    int layer_target_count;
    ID3D11VertexShader *composite_vs;
    ID3D11PixelShader *composite_ps;
    ID3D11BlendState *composite_blend;
    ID3D11Buffer *composite_params;

    ID3D11VertexShader *solid_vs;
    ID3D11PixelShader *solid_ps;
    ID3D11InputLayout *solid_layout;
    ID3D11Buffer *solid_buffer;
    UINT solid_capacity;

    ID3D11VertexShader *text_vs;
    ID3D11PixelShader *text_ps;
    ID3D11InputLayout *text_layout;
    ID3D11Buffer *text_buffer;
    UINT text_capacity;

    ID3D11VertexShader *image_vs;
    ID3D11PixelShader *image_ps;
    ID3D11InputLayout *image_layout;
    ID3D11Buffer *image_buffer;
    UINT image_capacity;

    ID3D11Texture2D *atlas_texture;
    ID3D11ShaderResourceView *atlas_view;
    int atlas_width;
    int atlas_height;
    const unsigned char *atlas_ptr;
    unsigned int atlas_serial;

    ID3D11SamplerState *sampler;
    ID3D11BlendState *blend;
    ID3D11RasterizerState *raster;

    WnD3D11Image *images;
    int image_count;
    int image_cache_capacity;
    size_t image_cache_bytes;
    size_t image_cache_budget;
    unsigned long long image_cache_tick;
    unsigned long long image_frame;
    IWICImagingFactory *wic;
    int com_initialized;

    int uploaded_vertices;
    int uploaded_glyphs;
    int uploaded_images;

    /* Optional visual-check hook: WN_CAPTURE=<path> [WN_CAPTURE_FRAME=N]. */
    char capture_path[1024];
    int capture_frame;

    wn_renderer_info info;
    wn_renderer_stats stats;
    char error[512];
} WnD3D11;

static void set_error(WnD3D11 *gpu, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(gpu->error, sizeof(gpu->error), fmt, args);
    va_end(args);
    gpu->error[sizeof(gpu->error) - 1] = '\0';
}

static void com_release(IUnknown *object)
{
    if (object != NULL) {
        object->lpVtbl->Release(object);
    }
}

static void release_layer_targets(WnD3D11 *gpu);

/* ----------------------------------------------------------------- shaders */

static const char *kSolidShader =
    "struct VSIn {\n"
    "  float2 pos : POSITION;\n"
    "  float4 color : COLOR0;\n"
    "};\n"
    "struct VSOut {\n"
    "  float4 pos : SV_Position;\n"
    "  float4 color : COLOR0;\n"
    "};\n"
    "VSOut vs_main(VSIn input) {\n"
    "  VSOut output;\n"
    "  output.pos = float4(input.pos, 0.0, 1.0);\n"
    "  output.color = input.color;\n"
    "  return output;\n"
    "}\n"
    "float4 ps_main(VSOut input) : SV_Target {\n"
    "  return input.color;\n"
    "}\n";

static const char *kTextShader =
    "Texture2D atlas_texture : register(t0);\n"
    "SamplerState atlas_sampler : register(s0);\n"
    "struct VSIn {\n"
    "  float2 pos : POSITION;\n"
    "  float2 uv : TEXCOORD0;\n"
    "  float4 color : COLOR0;\n"
    "};\n"
    "struct VSOut {\n"
    "  float4 pos : SV_Position;\n"
    "  float2 uv : TEXCOORD0;\n"
    "  float4 color : COLOR0;\n"
    "};\n"
    "VSOut vs_main(VSIn input) {\n"
    "  VSOut output;\n"
    "  output.pos = float4(input.pos, 0.0, 1.0);\n"
    "  output.uv = input.uv;\n"
    "  output.color = input.color;\n"
    "  return output;\n"
    "}\n"
    "float4 ps_main(VSOut input) : SV_Target {\n"
    "  float coverage = atlas_texture.Sample(atlas_sampler, input.uv).r;\n"
    "  return float4(input.color.rgb, input.color.a * coverage);\n"
    "}\n";

static const char *kImageShader =
    "Texture2D image_texture : register(t0);\n"
    "SamplerState image_sampler : register(s0);\n"
    "struct VSIn {\n"
    "  float2 pos : POSITION;\n"
    "  float2 uv : TEXCOORD0;\n"
    "  float alpha : TEXCOORD1;\n"
    "};\n"
    "struct VSOut {\n"
    "  float4 pos : SV_Position;\n"
    "  float2 uv : TEXCOORD0;\n"
    "  float alpha : TEXCOORD1;\n"
    "};\n"
    "VSOut vs_main(VSIn input) {\n"
    "  VSOut output;\n"
    "  output.pos = float4(input.pos, 0.0, 1.0);\n"
    "  output.uv = input.uv;\n"
    "  output.alpha = input.alpha;\n"
    "  return output;\n"
    "}\n"
    "float4 ps_main(VSOut input) : SV_Target {\n"
    "  float4 c = image_texture.Sample(image_sampler, input.uv);\n"
    "  return float4(c.rgb, c.a * input.alpha);\n"
    "}\n";

/* Group-layer composite: the layer texture already holds premultiplied RGB
 * (normal SrcAlpha blending accumulated it over transparent black), so the
 * shader scales rgb and alpha by the group opacity and the composite blend is
 * ONE / INV_SRC_ALPHA. That applies the group alpha exactly once. */
static const char *kCompositeShader =
    "cbuffer LayerParams : register(b0) { float layer_opacity; };\n"
    "Texture2D layer_texture : register(t0);\n"
    "SamplerState layer_sampler : register(s0);\n"
    "struct VSOut {\n"
    "  float4 pos : SV_Position;\n"
    "  float2 uv : TEXCOORD0;\n"
    "};\n"
    "VSOut vs_main(uint id : SV_VertexID) {\n"
    "  VSOut output;\n"
    "  float2 p = float2((float)((id << 1) & 2), (float)(id & 2));\n"
    "  output.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);\n"
    "  output.uv = p;\n"
    "  return output;\n"
    "}\n"
    "float4 ps_main(VSOut input) : SV_Target {\n"
    "  float4 c = layer_texture.Sample(layer_sampler, input.uv);\n"
    "  return float4(c.rgb * layer_opacity, c.a * layer_opacity);\n"
    "}\n";

static ID3DBlob *compile_shader(WnD3D11 *gpu, const char *source, const char *entry,
                               const char *target)
{
    ID3DBlob *code = NULL;
    ID3DBlob *errors = NULL;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    HRESULT hr = D3DCompile(source, (SIZE_T)strlen(source), "wn_renderer_d3d11.hlsl",
                            NULL, NULL, entry, target, flags, 0, &code, &errors);
    if (FAILED(hr)) {
        set_error(gpu, "D3DCompile(%s/%s) failed (hr=0x%08lX): %s", entry, target,
                  (unsigned long)hr,
                  errors != NULL ? (const char *)errors->lpVtbl->GetBufferPointer(errors)
                                 : "(no compiler message)");
        if (errors != NULL) {
            errors->lpVtbl->Release(errors);
        }
        return NULL;
    }
    if (errors != NULL) {
        errors->lpVtbl->Release(errors);
    }
    return code;
}

static int create_pipeline(WnD3D11 *gpu, const char *source,
                           const D3D11_INPUT_ELEMENT_DESC *elements, UINT element_count,
                           ID3D11VertexShader **vs, ID3D11PixelShader **ps,
                           ID3D11InputLayout **layout)
{
    ID3DBlob *vs_code;
    ID3DBlob *ps_code;
    const char *vs_target = gpu->feature_level >= D3D_FEATURE_LEVEL_11_0 ? "vs_5_0"
                                                                        : "vs_4_0";
    const char *ps_target = gpu->feature_level >= D3D_FEATURE_LEVEL_11_0 ? "ps_5_0"
                                                                        : "ps_4_0";
    HRESULT hr;

    vs_code = compile_shader(gpu, source, "vs_main", vs_target);
    if (vs_code == NULL) {
        return 1;
    }
    ps_code = compile_shader(gpu, source, "ps_main", ps_target);
    if (ps_code == NULL) {
        vs_code->lpVtbl->Release(vs_code);
        return 1;
    }
    hr = gpu->device->lpVtbl->CreateVertexShader(gpu->device,
                                                 vs_code->lpVtbl->GetBufferPointer(vs_code),
                                                 vs_code->lpVtbl->GetBufferSize(vs_code),
                                                 NULL, vs);
    if (SUCCEEDED(hr)) {
        hr = gpu->device->lpVtbl->CreateInputLayout(gpu->device, elements, element_count,
                                                    vs_code->lpVtbl->GetBufferPointer(vs_code),
                                                    vs_code->lpVtbl->GetBufferSize(vs_code),
                                                    layout);
    }
    if (SUCCEEDED(hr)) {
        hr = gpu->device->lpVtbl->CreatePixelShader(gpu->device,
                                                    ps_code->lpVtbl->GetBufferPointer(ps_code),
                                                    ps_code->lpVtbl->GetBufferSize(ps_code),
                                                    NULL, ps);
    }
    vs_code->lpVtbl->Release(vs_code);
    ps_code->lpVtbl->Release(ps_code);
    if (FAILED(hr)) {
        set_error(gpu, "pipeline creation failed (hr=0x%08lX)", (unsigned long)hr);
        return 1;
    }
    return 0;
}

static int create_composite_pipeline(WnD3D11 *gpu)
{
    ID3DBlob *vs_code;
    ID3DBlob *ps_code;
    const char *vs_target = gpu->feature_level >= D3D_FEATURE_LEVEL_11_0 ? "vs_5_0"
                                                                        : "vs_4_0";
    const char *ps_target = gpu->feature_level >= D3D_FEATURE_LEVEL_11_0 ? "ps_5_0"
                                                                        : "ps_4_0";
    HRESULT hr;
    vs_code = compile_shader(gpu, kCompositeShader, "vs_main", vs_target);
    if (vs_code == NULL) {
        return 1;
    }
    ps_code = compile_shader(gpu, kCompositeShader, "ps_main", ps_target);
    if (ps_code == NULL) {
        vs_code->lpVtbl->Release(vs_code);
        return 1;
    }
    hr = gpu->device->lpVtbl->CreateVertexShader(
        gpu->device, vs_code->lpVtbl->GetBufferPointer(vs_code),
        vs_code->lpVtbl->GetBufferSize(vs_code), NULL, &gpu->composite_vs);
    if (SUCCEEDED(hr)) {
        hr = gpu->device->lpVtbl->CreatePixelShader(
            gpu->device, ps_code->lpVtbl->GetBufferPointer(ps_code),
            ps_code->lpVtbl->GetBufferSize(ps_code), NULL, &gpu->composite_ps);
    }
    vs_code->lpVtbl->Release(vs_code);
    ps_code->lpVtbl->Release(ps_code);
    if (FAILED(hr)) {
        set_error(gpu, "composite pipeline creation failed (hr=0x%08lX)",
                  (unsigned long)hr);
        return 1;
    }
    return 0;
}

static int create_pipelines(WnD3D11 *gpu)
{
    static const D3D11_INPUT_ELEMENT_DESC solid_elements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    static const D3D11_INPUT_ELEMENT_DESC text_elements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    static const D3D11_INPUT_ELEMENT_DESC image_elements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };

    if (create_pipeline(gpu, kSolidShader, solid_elements,
                        (UINT)(sizeof(solid_elements) / sizeof(solid_elements[0])),
                        &gpu->solid_vs, &gpu->solid_ps, &gpu->solid_layout) != 0) {
        return 1;
    }
    if (create_pipeline(gpu, kTextShader, text_elements,
                        (UINT)(sizeof(text_elements) / sizeof(text_elements[0])),
                        &gpu->text_vs, &gpu->text_ps, &gpu->text_layout) != 0) {
        return 1;
    }
    if (create_pipeline(gpu, kImageShader, image_elements,
                        (UINT)(sizeof(image_elements) / sizeof(image_elements[0])),
                        &gpu->image_vs, &gpu->image_ps, &gpu->image_layout) != 0) {
        return 1;
    }
    return create_composite_pipeline(gpu);
}

/* ------------------------------------------------------------- swapchain */

static int create_render_target(WnD3D11 *gpu)
{
    HRESULT hr;
    ID3D11Texture2D *backbuffer = NULL;
    ID3D11RenderTargetView *rtv = NULL;

    if (gpu->offscreen) {
        D3D11_TEXTURE2D_DESC desc;
        memset(&desc, 0, sizeof(desc));
        desc.Width = (UINT)(gpu->width > 0 ? gpu->width : 1);
        desc.Height = (UINT)(gpu->height > 0 ? gpu->height : 1);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        hr = gpu->device->lpVtbl->CreateTexture2D(gpu->device, &desc, NULL, &backbuffer);
        if (FAILED(hr) || backbuffer == NULL) {
            set_error(gpu, "offscreen CreateTexture2D failed (hr=0x%08lX)",
                      (unsigned long)hr);
            return 1;
        }
        hr = gpu->device->lpVtbl->CreateRenderTargetView(
            gpu->device, (ID3D11Resource *)backbuffer, NULL, &rtv);
        if (FAILED(hr) || rtv == NULL) {
            set_error(gpu, "offscreen CreateRenderTargetView failed (hr=0x%08lX)",
                      (unsigned long)hr);
            backbuffer->lpVtbl->Release(backbuffer);
            return 1;
        }
        com_release((IUnknown *)gpu->rtv);
        com_release((IUnknown *)gpu->backbuffer);
        gpu->backbuffer = backbuffer;
        gpu->rtv = rtv;
        return 0;
    }

    if (gpu->swapchain == NULL) {
        return 1;
    }
    hr = gpu->swapchain->lpVtbl->GetBuffer(gpu->swapchain, 0, &IID_ID3D11Texture2D,
                                          (void **)&backbuffer);
    if (FAILED(hr) || backbuffer == NULL) {
        set_error(gpu, "IDXGISwapChain::GetBuffer(0) failed (hr=0x%08lX)",
                  (unsigned long)hr);
        return 1;
    }
    hr = gpu->device->lpVtbl->CreateRenderTargetView(gpu->device, (ID3D11Resource *)backbuffer,
                                                     NULL, &rtv);
    if (FAILED(hr) || rtv == NULL) {
        set_error(gpu, "CreateRenderTargetView failed (hr=0x%08lX)", (unsigned long)hr);
        backbuffer->lpVtbl->Release(backbuffer);
        return 1;
    }
    com_release((IUnknown *)gpu->rtv);
    com_release((IUnknown *)gpu->backbuffer);
    gpu->backbuffer = backbuffer;
    gpu->rtv = rtv;
    return 0;
}

static void fill_adapter_name(WnD3D11 *gpu)
{
    IDXGIDevice *dxgi_device = NULL;
    IDXGIAdapter1 *adapter = NULL;
    DXGI_ADAPTER_DESC1 desc;
    HRESULT hr;

    if (gpu == NULL || gpu->device == NULL || gpu->info.device[0] != '\0') {
        return;
    }
    hr = gpu->device->lpVtbl->QueryInterface(gpu->device, &IID_IDXGIDevice,
                                             (void **)&dxgi_device);
    if (FAILED(hr) || dxgi_device == NULL) {
        return;
    }
    hr = dxgi_device->lpVtbl->GetAdapter(dxgi_device, (IDXGIAdapter **)&adapter);
    dxgi_device->lpVtbl->Release(dxgi_device);
    if (FAILED(hr) || adapter == NULL) {
        return;
    }
    memset(&desc, 0, sizeof(desc));
    if (SUCCEEDED(adapter->lpVtbl->GetDesc1(adapter, &desc))) {
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, gpu->info.device,
                            (int)sizeof(gpu->info.device), NULL, NULL);
        gpu->info.device[sizeof(gpu->info.device) - 1] = '\0';
    }
    adapter->lpVtbl->Release(adapter);
}

static int create_swapchain(WnD3D11 *gpu, void *handle, int width, int height)
{
    IDXGIDevice *dxgi_device = NULL;
    IDXGIAdapter1 *adapter = NULL;
    IDXGIFactory1 *factory1 = NULL;
    IDXGIFactory2 *factory2 = NULL;
    DXGI_ADAPTER_DESC1 desc;
    DXGI_SWAP_CHAIN_DESC1 desc1;
    DXGI_SWAP_CHAIN_DESC desc0;
    HRESULT hr;

    memset(&desc, 0, sizeof(desc));
    hr = gpu->device->lpVtbl->QueryInterface(gpu->device, &IID_IDXGIDevice,
                                             (void **)&dxgi_device);
    if (FAILED(hr) || dxgi_device == NULL) {
        set_error(gpu, "ID3D11Device -> IDXGIDevice failed (hr=0x%08lX)",
                  (unsigned long)hr);
        return 1;
    }
    hr = dxgi_device->lpVtbl->GetAdapter(dxgi_device, (IDXGIAdapter **)&adapter);
    dxgi_device->lpVtbl->Release(dxgi_device);
    dxgi_device = NULL;
    if (FAILED(hr) || adapter == NULL) {
        set_error(gpu, "IDXGIDevice::GetAdapter failed (hr=0x%08lX)", (unsigned long)hr);
        return 1;
    }
    if (SUCCEEDED(adapter->lpVtbl->GetDesc1(adapter, &desc))) {
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, gpu->info.device,
                            (int)sizeof(gpu->info.device), NULL, NULL);
        gpu->info.device[sizeof(gpu->info.device) - 1] = '\0';
    }
    /* Keep both factories: factory2 for the flip model and factory1 for the
     * classic fallback if flip creation is rejected at runtime. */
    hr = adapter->lpVtbl->GetParent(adapter, &IID_IDXGIFactory2, (void **)&factory2);
    if (FAILED(hr)) {
        factory2 = NULL;
    }
    hr = adapter->lpVtbl->GetParent(adapter, &IID_IDXGIFactory1, (void **)&factory1);
    if (FAILED(hr)) {
        factory1 = NULL;
    }
    adapter->lpVtbl->Release(adapter);
    if (factory1 == NULL && factory2 == NULL) {
        set_error(gpu, "IDXGIAdapter::GetParent(IDXGIFactory) failed (hr=0x%08lX)",
                  (unsigned long)hr);
        return 1;
    }

    gpu->format = DXGI_FORMAT_B8G8R8A8_UNORM;
    memset(&desc1, 0, sizeof(desc1));
    desc1.Width = (UINT)(width > 0 ? width : 1);
    desc1.Height = (UINT)(height > 0 ? height : 1);
    desc1.Format = gpu->format;
    desc1.Stereo = FALSE;
    desc1.SampleDesc.Count = 1;
    desc1.SampleDesc.Quality = 0;
    desc1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc1.BufferCount = 2;
    desc1.Scaling = DXGI_SCALING_STRETCH;
    desc1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc1.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc1.Flags = 0;
    gpu->swapchain_flags = desc1.Flags;

    if (factory2 != NULL) {
        hr = factory2->lpVtbl->CreateSwapChainForHwnd(factory2, (IUnknown *)gpu->device,
                                                     (HWND)handle, &desc1, NULL, NULL,
                                                     &gpu->swapchain);
        if (SUCCEEDED(hr) && gpu->swapchain != NULL) {
            gpu->flip_model = 1;
        } else {
            gpu->swapchain = NULL;
        }
        factory2->lpVtbl->MakeWindowAssociation(factory2, (HWND)handle,
                                                DXGI_MWA_NO_ALT_ENTER);
        factory2->lpVtbl->Release(factory2);
    }
    if (gpu->swapchain != NULL && factory1 != NULL) {
        factory1->lpVtbl->Release(factory1);
        factory1 = NULL;
    }

    if (gpu->swapchain == NULL) {
        if (factory1 == NULL) {
            set_error(gpu, "flip-model swapchain creation failed (hr=0x%08lX)",
                      (unsigned long)hr);
            return 1;
        }
        memset(&desc0, 0, sizeof(desc0));
        desc0.BufferDesc.Width = (UINT)(width > 0 ? width : 1);
        desc0.BufferDesc.Height = (UINT)(height > 0 ? height : 1);
        desc0.BufferDesc.Format = gpu->format;
        desc0.SampleDesc.Count = 1;
        desc0.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc0.BufferCount = 1;
        desc0.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        desc0.Windowed = TRUE;
        hr = factory1->lpVtbl->CreateSwapChain(factory1, (IUnknown *)gpu->device, &desc0,
                                               (IDXGISwapChain **)&gpu->swapchain);
        factory1->lpVtbl->MakeWindowAssociation(factory1, (HWND)handle,
                                                DXGI_MWA_NO_ALT_ENTER);
        factory1->lpVtbl->Release(factory1);
        if (FAILED(hr) || gpu->swapchain == NULL) {
            set_error(gpu, "CreateSwapChain failed (hr=0x%08lX)", (unsigned long)hr);
            return 1;
        }
        gpu->flip_model = 0;
    }

    gpu->width = width > 0 ? width : 1;
    gpu->height = height > 0 ? height : 1;
    gpu->info.width = gpu->width;
    gpu->info.height = gpu->height;
    gpu->info.format = (int)gpu->format;
    return create_render_target(gpu);
}

/* --------------------------------------------------------------- textures */

static int upload_atlas(WnD3D11 *gpu, const wn_atlas_view *atlas)
{
    D3D11_TEXTURE2D_DESC desc;
    D3D11_SUBRESOURCE_DATA initial;
    ID3D11Texture2D *texture = NULL;
    ID3D11ShaderResourceView *view = NULL;
    HRESULT hr;

    if (atlas == NULL || atlas->pixels == NULL || atlas->width <= 0 || atlas->height <= 0) {
        return 0;
    }
    if (gpu->atlas_texture != NULL && gpu->atlas_ptr == atlas->pixels && gpu->atlas_serial == atlas->serial &&
        gpu->atlas_width == atlas->width && gpu->atlas_height == atlas->height) {
        return 0;
    }

    memset(&desc, 0, sizeof(desc));
    desc.Width = (UINT)atlas->width;
    desc.Height = (UINT)atlas->height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    memset(&initial, 0, sizeof(initial));
    initial.pSysMem = atlas->pixels;
    initial.SysMemPitch = (UINT)atlas->width;
    initial.SysMemSlicePitch = (UINT)(atlas->width * atlas->height);

    hr = gpu->device->lpVtbl->CreateTexture2D(gpu->device, &desc, &initial, &texture);
    if (FAILED(hr) || texture == NULL) {
        set_error(gpu, "glyph atlas texture creation failed (hr=0x%08lX)",
                  (unsigned long)hr);
        return 1;
    }
    hr = gpu->device->lpVtbl->CreateShaderResourceView(gpu->device,
                                                       (ID3D11Resource *)texture, NULL, &view);
    if (FAILED(hr) || view == NULL) {
        set_error(gpu, "glyph atlas SRV creation failed (hr=0x%08lX)", (unsigned long)hr);
        texture->lpVtbl->Release(texture);
        return 1;
    }
    com_release((IUnknown *)gpu->atlas_view);
    com_release((IUnknown *)gpu->atlas_texture);
    gpu->atlas_texture = texture;
    gpu->atlas_view = view;
    gpu->atlas_width = atlas->width;
    gpu->atlas_height = atlas->height;
    gpu->atlas_ptr = atlas->pixels;
    gpu->atlas_serial = atlas->serial;
    return 0;
}

static int decode_image_rgba(WnD3D11 *gpu, const char *path, unsigned char **pixels_out,
                             int *width_out, int *height_out)
{
    wchar_t wide[1024];
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;
    UINT width = 0;
    UINT height = 0;
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
        set_error(gpu, "image '%s' has no frame 0 (hr=0x%08lX)", path,
                  (unsigned long)hr);
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
        set_error(gpu, "WIC RGBA conversion failed for '%s' (hr=0x%08lX)", path,
                  (unsigned long)hr);
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
            set_error(gpu, "WIC CopyPixels failed for '%s' (hr=0x%08lX)", path,
                      (unsigned long)hr);
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

/*
 * Image cache budget. Decoded textures are cached by path; the cache is an LRU
 * bounded by decoded bytes so a media library cannot exhaust VRAM. Entries
 * referenced by the frame being uploaded are pinned and never evicted for that
 * frame; if one frame alone exceeds the budget the cache overflows by design
 * (correct pixels beat the accounting limit) and shrinks at the next upload.
 */
#define WN_IMAGE_CACHE_DEFAULT_MB 256

static size_t image_cache_budget_bytes(void)
{
    const char *env = getenv("WN_IMAGE_CACHE_MB");
    long mb = WN_IMAGE_CACHE_DEFAULT_MB;

    if (env != NULL && env[0] != '\0') {
        char *end = NULL;
        long parsed = strtol(env, &end, 10);
        if (end != env && end != NULL && *end == '\0' && parsed >= 0 && parsed <= 16384) {
            mb = parsed;
        }
    }
    return (size_t)mb * 1024u * 1024u;
}

static void release_image_entry(WnD3D11Image *entry)
{
    if (entry->view != NULL) {
        entry->view->lpVtbl->Release(entry->view);
    }
    if (entry->texture != NULL) {
        entry->texture->lpVtbl->Release(entry->texture);
    }
    wn_free_(entry->path);
    memset(entry, 0, sizeof(*entry));
}

static void remove_image_entry(WnD3D11 *gpu, int index)
{
    WnD3D11Image *entry = &gpu->images[index];

    gpu->image_cache_bytes -= entry->bytes;
    release_image_entry(entry);
    if (index + 1 < gpu->image_count) {
        memmove(&gpu->images[index], &gpu->images[index + 1],
                (size_t)(gpu->image_count - index - 1) * sizeof(WnD3D11Image));
    }
    --gpu->image_count;
}

/* Evicts least-recently-used entries not pinned by the current frame. */
static void evict_images_for(WnD3D11 *gpu, size_t incoming_bytes)
{
    while (gpu->image_cache_bytes + incoming_bytes > gpu->image_cache_budget) {
        int victim = -1;
        unsigned long long oldest = 0;
        int i;

        for (i = 0; i < gpu->image_count; ++i) {
            if (gpu->images[i].frame == gpu->image_frame) {
                continue;
            }
            if (victim < 0 || gpu->images[i].last_used < oldest) {
                victim = i;
                oldest = gpu->images[i].last_used;
            }
        }
        if (victim < 0) {
            break;
        }
        remove_image_entry(gpu, victim);
    }
}

/* Marks every cached image of the incoming frame as pinned and recently used. */
static void touch_frame_images(WnD3D11 *gpu, const wn_display_list *list)
{
    int b;

    ++gpu->image_frame;
    if (list == NULL) {
        return;
    }
    for (b = 0; b < list->image_batch_count; ++b) {
        const wn_image_batch *batch = &list->image_batches[b];
        int i;
        if (batch->src == NULL) {
            continue;
        }
        for (i = 0; i < gpu->image_count; ++i) {
            if (strcmp(gpu->images[i].path, batch->src) == 0) {
                gpu->images[i].frame = gpu->image_frame;
                gpu->images[i].last_used = ++gpu->image_cache_tick;
                break;
            }
        }
    }
}

static WnD3D11Image *find_cached_image(WnD3D11 *gpu, const char *path)
{
    int i;
    for (i = 0; i < gpu->image_count; ++i) {
        if (strcmp(gpu->images[i].path, path) == 0) {
            gpu->images[i].frame = gpu->image_frame;
            gpu->images[i].last_used = ++gpu->image_cache_tick;
            return &gpu->images[i];
        }
    }
    return NULL;
}

static WnD3D11Image *ensure_image_texture(WnD3D11 *gpu, const char *path)
{
    WnD3D11Image *entry;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_SUBRESOURCE_DATA initial;
    unsigned char *pixels = NULL;
    int width = 0;
    int height = 0;
    HRESULT hr;

    entry = find_cached_image(gpu, path);
    if (entry != NULL) {
        return entry;
    }
    if (decode_image_rgba(gpu, path, &pixels, &width, &height) != 0) {
        return NULL;
    }
    evict_images_for(gpu, (size_t)width * (size_t)height * 4u);
    if (gpu->image_count >= gpu->image_cache_capacity) {
        int capacity = gpu->image_cache_capacity > 0 ? gpu->image_cache_capacity * 2 : 8;
        WnD3D11Image *grown = (WnD3D11Image *)realloc(
            gpu->images, (size_t)capacity * sizeof(WnD3D11Image));
        if (grown == NULL) {
            free(pixels);
            set_error(gpu, "out of memory growing the image cache");
            return NULL;
        }
        gpu->images = grown;
        gpu->image_cache_capacity = capacity;
    }

    memset(&desc, 0, sizeof(desc));
    desc.Width = (UINT)width;
    desc.Height = (UINT)height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    memset(&initial, 0, sizeof(initial));
    initial.pSysMem = pixels;
    initial.SysMemPitch = (UINT)width * 4u;
    initial.SysMemSlicePitch = (UINT)width * 4u * (UINT)height;

    entry = &gpu->images[gpu->image_count];
    memset(entry, 0, sizeof(*entry));
    hr = gpu->device->lpVtbl->CreateTexture2D(gpu->device, &desc, &initial,
                                              &entry->texture);
    if (FAILED(hr) || entry->texture == NULL) {
        free(pixels);
        set_error(gpu, "image texture creation failed for '%s' (hr=0x%08lX)", path,
                  (unsigned long)hr);
        return NULL;
    }
    hr = gpu->device->lpVtbl->CreateShaderResourceView(
        gpu->device, (ID3D11Resource *)entry->texture, NULL, &entry->view);
    free(pixels);
    if (FAILED(hr) || entry->view == NULL) {
        entry->texture->lpVtbl->Release(entry->texture);
        set_error(gpu, "image SRV creation failed for '%s' (hr=0x%08lX)", path,
                  (unsigned long)hr);
        return NULL;
    }
    entry->path = wn_strdup_(path);
    if (entry->path == NULL) {
        entry->view->lpVtbl->Release(entry->view);
        entry->texture->lpVtbl->Release(entry->texture);
        set_error(gpu, "out of memory caching image '%s'", path);
        return NULL;
    }
    entry->width = width;
    entry->height = height;
    entry->bytes = (size_t)width * (size_t)height * 4u;
    entry->last_used = ++gpu->image_cache_tick;
    entry->frame = gpu->image_frame;
    gpu->image_cache_bytes += entry->bytes;
    ++gpu->image_count;
    return entry;
}

/* ---------------------------------------------------------------- buffers */

static int ensure_buffer(WnD3D11 *gpu, ID3D11Buffer **buffer, UINT *capacity, size_t needed)
{
    D3D11_BUFFER_DESC desc;
    ID3D11Buffer *created = NULL;
    UINT size;
    HRESULT hr;

    if (*buffer != NULL && (size_t)*capacity >= needed) {
        return 0;
    }
    size = (UINT)((needed + 255u) & ~(size_t)255u);
    if (size < 1024u) {
        size = 1024u;
    }
    memset(&desc, 0, sizeof(desc));
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.ByteWidth = size;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = gpu->device->lpVtbl->CreateBuffer(gpu->device, &desc, NULL, &created);
    if (FAILED(hr) || created == NULL) {
        set_error(gpu, "vertex buffer creation failed (hr=0x%08lX)", (unsigned long)hr);
        return 1;
    }
    com_release((IUnknown *)*buffer);
    *buffer = created;
    *capacity = size;
    return 0;
}

static int upload_vertices(WnD3D11 *gpu, ID3D11Buffer *buffer, const void *data,
                           size_t bytes)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;
    if (bytes == 0) {
        return 0;
    }
    hr = gpu->context->lpVtbl->Map(gpu->context, (ID3D11Resource *)buffer, 0,
                                   D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        set_error(gpu, "Map(vertex buffer) failed (hr=0x%08lX)", (unsigned long)hr);
        return 1;
    }
    memcpy(mapped.pData, data, bytes);
    gpu->context->lpVtbl->Unmap(gpu->context, (ID3D11Resource *)buffer, 0);
    return 0;
}

/* ------------------------------------------------------------- screenshot */

static int write_png_bgra(WnD3D11 *gpu, const char *path, const unsigned char *pixels,
                          int width, int height, UINT stride)
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

static int capture_backbuffer(WnD3D11 *gpu, const char *path)
{
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    ID3D11Texture2D *staging = NULL;
    HRESULT hr;
    int result = 1;

    if (gpu->backbuffer == NULL || gpu->context == NULL) {
        return 1;
    }
    gpu->backbuffer->lpVtbl->GetDesc(gpu->backbuffer, &desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    hr = gpu->device->lpVtbl->CreateTexture2D(gpu->device, &desc, NULL, &staging);
    if (FAILED(hr) || staging == NULL) {
        return 1;
    }
    /* The backbuffer is still bound as the render target; unbind it before
     * reading it back so the debug layer stays clean. */
    gpu->context->lpVtbl->OMSetRenderTargets(gpu->context, 0, NULL, NULL);
    gpu->context->lpVtbl->CopyResource(gpu->context, (ID3D11Resource *)staging,
                                       (ID3D11Resource *)gpu->backbuffer);
    memset(&mapped, 0, sizeof(mapped));
    hr = gpu->context->lpVtbl->Map(gpu->context, (ID3D11Resource *)staging, 0,
                                   D3D11_MAP_READ, 0, &mapped);
    if (SUCCEEDED(hr)) {
        result = write_png_bgra(gpu, path, (const unsigned char *)mapped.pData,
                                (int)desc.Width, (int)desc.Height, mapped.RowPitch);
        gpu->context->lpVtbl->Unmap(gpu->context, (ID3D11Resource *)staging, 0);
    }
    staging->lpVtbl->Release(staging);
    return result;
}

/* ------------------------------------------------------------- vtable ops */

static int renderer_init(void **out, const wn_surface_desc *surface, int width, int height)
{
    WnD3D11 *gpu;
    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL got_level = D3D_FEATURE_LEVEL_10_0;
    D3D11_SAMPLER_DESC sampler_desc;
    D3D11_BLEND_DESC blend_desc;
    D3D11_BUFFER_DESC buffer_desc;
    D3D11_RASTERIZER_DESC raster_desc;
    UINT create_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const char *capture;
    const char *capture_frame;
    HRESULT hr;

    if (out == NULL) {
        return 1;
    }
    *out = NULL;
    if (surface == NULL ||
        (surface->kind != WN_SURFACE_HWND && surface->kind != WN_SURFACE_OFFSCREEN) ||
        (surface->kind == WN_SURFACE_HWND && surface->handle == NULL)) {
        fprintf(stderr, "renderer: d3d11 init needs an HWND or offscreen surface\n");
        return 1;
    }
    gpu = (WnD3D11 *)calloc(1, sizeof(*gpu));
    if (gpu == NULL) {
        return 1;
    }
    gpu->width = width > 0 ? width : 1;
    gpu->height = height > 0 ? height : 1;
    gpu->offscreen = surface->kind == WN_SURFACE_OFFSCREEN ? 1 : 0;
    gpu->info.width = gpu->width;
    gpu->info.height = gpu->height;
    gpu->capture_frame = 1;
    gpu->image_cache_budget = image_cache_budget_bytes();

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    gpu->com_initialized = SUCCEEDED(hr) ? 1 : 0;

    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, create_flags, levels,
                           (UINT)(sizeof(levels) / sizeof(levels[0])), D3D11_SDK_VERSION,
                           &gpu->device, &got_level, &gpu->context);
    if (hr == E_INVALIDARG) {
        /* Windows 7 without the 11.1 runtime rejects the 11_1 entry. */
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, create_flags,
                               levels + 1,
                               (UINT)(sizeof(levels) / sizeof(levels[0])) - 1u,
                               D3D11_SDK_VERSION, &gpu->device, &got_level, &gpu->context);
    }
    if (SUCCEEDED(hr)) {
        gpu->driver_hardware = 1;
    } else {
        fprintf(stderr, "renderer: D3D11 hardware device failed (hr=0x%08lX); "
                        "falling back to WARP\n", (unsigned long)hr);
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, create_flags, levels,
                               (UINT)(sizeof(levels) / sizeof(levels[0])),
                               D3D11_SDK_VERSION, &gpu->device, &got_level,
                               &gpu->context);
        if (hr == E_INVALIDARG) {
            hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, create_flags,
                                   levels + 1,
                                   (UINT)(sizeof(levels) / sizeof(levels[0])) - 1u,
                                   D3D11_SDK_VERSION, &gpu->device, &got_level,
                                   &gpu->context);
        }
        if (FAILED(hr) || gpu->device == NULL || gpu->context == NULL) {
            fprintf(stderr, "renderer: D3D11 WARP device failed (hr=0x%08lX)\n",
                    (unsigned long)hr);
            goto fail;
        }
    }
    gpu->feature_level = got_level;
    snprintf(gpu->info.backend, sizeof(gpu->info.backend), "%s", "D3D11");
    fill_adapter_name(gpu);

    if (gpu->offscreen) {
        if (create_render_target(gpu) != 0) {
            goto fail;
        }
    } else if (create_swapchain(gpu, surface->handle, gpu->width, gpu->height) != 0) {
        goto fail;
    }

    memset(&sampler_desc, 0, sizeof(sampler_desc));
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = gpu->device->lpVtbl->CreateSamplerState(gpu->device, &sampler_desc,
                                                 &gpu->sampler);
    if (FAILED(hr)) {
        set_error(gpu, "CreateSamplerState failed (hr=0x%08lX)", (unsigned long)hr);
        goto fail;
    }

    memset(&blend_desc, 0, sizeof(blend_desc));
    blend_desc.AlphaToCoverageEnable = FALSE;
    blend_desc.IndependentBlendEnable = FALSE;
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = gpu->device->lpVtbl->CreateBlendState(gpu->device, &blend_desc, &gpu->blend);
    if (FAILED(hr)) {
        set_error(gpu, "CreateBlendState failed (hr=0x%08lX)", (unsigned long)hr);
        goto fail;
    }

    /* Layer composite: the shader outputs premultiplied rgb*opacity, so the
     * blend is ONE / INV_SRC_ALPHA (normal straight-alpha would multiply the
     * alpha twice). */
    memset(&blend_desc, 0, sizeof(blend_desc));
    blend_desc.AlphaToCoverageEnable = FALSE;
    blend_desc.IndependentBlendEnable = FALSE;
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = gpu->device->lpVtbl->CreateBlendState(gpu->device, &blend_desc,
                                               &gpu->composite_blend);
    if (FAILED(hr)) {
        set_error(gpu, "composite CreateBlendState failed (hr=0x%08lX)",
                  (unsigned long)hr);
        goto fail;
    }

    memset(&buffer_desc, 0, sizeof(buffer_desc));
    buffer_desc.ByteWidth = 16;
    buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
    buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = gpu->device->lpVtbl->CreateBuffer(gpu->device, &buffer_desc, NULL,
                                           &gpu->composite_params);
    if (FAILED(hr)) {
        set_error(gpu, "composite CreateBuffer failed (hr=0x%08lX)",
                  (unsigned long)hr);
        goto fail;
    }

    memset(&raster_desc, 0, sizeof(raster_desc));
    raster_desc.FillMode = D3D11_FILL_SOLID;
    raster_desc.CullMode = D3D11_CULL_NONE;
    raster_desc.FrontCounterClockwise = FALSE;
    raster_desc.DepthClipEnable = TRUE;
    raster_desc.ScissorEnable = TRUE;
    raster_desc.MultisampleEnable = FALSE;
    raster_desc.AntialiasedLineEnable = FALSE;
    hr = gpu->device->lpVtbl->CreateRasterizerState(gpu->device, &raster_desc,
                                                    &gpu->raster);
    if (FAILED(hr)) {
        set_error(gpu, "CreateRasterizerState failed (hr=0x%08lX)", (unsigned long)hr);
        goto fail;
    }

    if (create_pipelines(gpu) != 0) {
        goto fail;
    }
    if (ensure_buffer(gpu, &gpu->solid_buffer, &gpu->solid_capacity, 1024) != 0 ||
        ensure_buffer(gpu, &gpu->text_buffer, &gpu->text_capacity, 1024) != 0 ||
        ensure_buffer(gpu, &gpu->image_buffer, &gpu->image_capacity, 1024) != 0) {
        goto fail;
    }

    capture = getenv("WN_CAPTURE");
    if (capture != NULL && capture[0] != '\0') {
        snprintf(gpu->capture_path, sizeof(gpu->capture_path), "%s", capture);
        capture_frame = getenv("WN_CAPTURE_FRAME");
        if (capture_frame != NULL && capture_frame[0] != '\0') {
            int parsed = atoi(capture_frame);
            if (parsed > 0) {
                gpu->capture_frame = parsed;
            }
        }
    }

    printf("renderer: backend=%s device=%s driver=%s feature_level=0x%04X "
           "swapchain=%s\n",
           gpu->info.backend,
           gpu->info.device[0] != '\0' ? gpu->info.device : "(unknown)",
           gpu->driver_hardware ? "hardware" : "warp",
           (unsigned)got_level,
           gpu->offscreen ? "offscreen" : (gpu->flip_model ? "flip-discard" : "discard"));
    fflush(stdout);

    *out = gpu;
    return 0;

fail:
    if (gpu->error[0] != '\0') {
        fprintf(stderr, "renderer: d3d11 init failed: %s\n", gpu->error);
    }
    /* Release everything allocated so far; *out stays NULL. */
    {
        int i;
        if (gpu->composite_params != NULL) gpu->composite_params->lpVtbl->Release(gpu->composite_params);
        if (gpu->composite_blend != NULL) gpu->composite_blend->lpVtbl->Release(gpu->composite_blend);
        if (gpu->composite_ps != NULL) gpu->composite_ps->lpVtbl->Release(gpu->composite_ps);
        if (gpu->composite_vs != NULL) gpu->composite_vs->lpVtbl->Release(gpu->composite_vs);
        if (gpu->solid_buffer != NULL) gpu->solid_buffer->lpVtbl->Release(gpu->solid_buffer);
        if (gpu->text_buffer != NULL) gpu->text_buffer->lpVtbl->Release(gpu->text_buffer);
        if (gpu->image_buffer != NULL) gpu->image_buffer->lpVtbl->Release(gpu->image_buffer);
        for (i = 0; i < gpu->image_count; ++i) {
            release_image_entry(&gpu->images[i]);
        }
        wn_free_(gpu->images);
        if (gpu->wic != NULL) gpu->wic->lpVtbl->Release(gpu->wic);
        if (gpu->raster != NULL) gpu->raster->lpVtbl->Release(gpu->raster);
        if (gpu->blend != NULL) gpu->blend->lpVtbl->Release(gpu->blend);
        if (gpu->sampler != NULL) gpu->sampler->lpVtbl->Release(gpu->sampler);
        if (gpu->atlas_view != NULL) gpu->atlas_view->lpVtbl->Release(gpu->atlas_view);
        if (gpu->atlas_texture != NULL) gpu->atlas_texture->lpVtbl->Release(gpu->atlas_texture);
        if (gpu->image_layout != NULL) gpu->image_layout->lpVtbl->Release(gpu->image_layout);
        if (gpu->image_ps != NULL) gpu->image_ps->lpVtbl->Release(gpu->image_ps);
        if (gpu->image_vs != NULL) gpu->image_vs->lpVtbl->Release(gpu->image_vs);
        if (gpu->text_layout != NULL) gpu->text_layout->lpVtbl->Release(gpu->text_layout);
        if (gpu->text_ps != NULL) gpu->text_ps->lpVtbl->Release(gpu->text_ps);
        if (gpu->text_vs != NULL) gpu->text_vs->lpVtbl->Release(gpu->text_vs);
        if (gpu->solid_layout != NULL) gpu->solid_layout->lpVtbl->Release(gpu->solid_layout);
        if (gpu->solid_ps != NULL) gpu->solid_ps->lpVtbl->Release(gpu->solid_ps);
        if (gpu->solid_vs != NULL) gpu->solid_vs->lpVtbl->Release(gpu->solid_vs);
        if (gpu->rtv != NULL) gpu->rtv->lpVtbl->Release(gpu->rtv);
        if (gpu->backbuffer != NULL) gpu->backbuffer->lpVtbl->Release(gpu->backbuffer);
        if (gpu->swapchain != NULL) gpu->swapchain->lpVtbl->Release(gpu->swapchain);
        if (gpu->context != NULL) gpu->context->lpVtbl->Release(gpu->context);
        if (gpu->device != NULL) gpu->device->lpVtbl->Release(gpu->device);
        if (gpu->com_initialized) CoUninitialize();
        free(gpu);
    }
    return 1;
}

static void renderer_resize(void *context, int width, int height)
{
    WnD3D11 *gpu = (WnD3D11 *)context;
    int old_width;
    int old_height;
    UINT old_flags;
    HRESULT hr;

    if (gpu == NULL) {
        return;
    }
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    if (gpu->width == width && gpu->height == height && gpu->rtv != NULL) {
        return;
    }
    /* Capture before overwriting: the ResizeBuffers failure path restores these. */
    old_width = gpu->width;
    old_height = gpu->height;
    old_flags = gpu->swapchain_flags;
    gpu->width = width;
    gpu->height = height;
    gpu->info.width = width;
    gpu->info.height = height;
    /* Layer targets are viewport-sized; drop them and recreate lazily. */
    release_layer_targets(gpu);
    if (gpu->offscreen) {
        com_release((IUnknown *)gpu->rtv);
        gpu->rtv = NULL;
        com_release((IUnknown *)gpu->backbuffer);
        gpu->backbuffer = NULL;
        (void)create_render_target(gpu);
        return;
    }
    if (gpu->swapchain == NULL) {
        return;
    }

    {
        com_release((IUnknown *)gpu->rtv);
        gpu->rtv = NULL;
        com_release((IUnknown *)gpu->backbuffer);
        gpu->backbuffer = NULL;

        hr = gpu->swapchain->lpVtbl->ResizeBuffers(gpu->swapchain, 0, (UINT)width,
                                                   (UINT)height, DXGI_FORMAT_UNKNOWN,
                                                   old_flags);
        if (FAILED(hr)) {
            set_error(gpu, "ResizeBuffers(%dx%d) failed (hr=0x%08lX)", width, height,
                      (unsigned long)hr);
            /* The swapchain kept its old buffers: restore the last valid state
             * and reacquire its backbuffer/RTV so rendering can continue. */
            gpu->width = old_width;
            gpu->height = old_height;
            gpu->info.width = old_width;
            gpu->info.height = old_height;
            gpu->swapchain_flags = old_flags;
            if (create_render_target(gpu) != 0) {
                gpu->info.width = 0;
                gpu->info.height = 0;
            }
            return;
        }
    }
    (void)create_render_target(gpu);
}

static int renderer_upload(void *context, const wn_display_list *list)
{
    WnD3D11 *gpu = (WnD3D11 *)context;
    size_t bytes;

    if (gpu == NULL || list == NULL) {
        return 1;
    }
    gpu->uploaded_vertices = 0;
    gpu->uploaded_glyphs = 0;
    gpu->uploaded_images = 0;
    touch_frame_images(gpu, list);

    if (list->atlas.pixels != NULL) {
        if (upload_atlas(gpu, &list->atlas) != 0) {
            return 1;
        }
    }

    if (list->vertex_count > 0) {
        bytes = (size_t)list->vertex_count * sizeof(wn_vertex);
        if (ensure_buffer(gpu, &gpu->solid_buffer, &gpu->solid_capacity, bytes) != 0 ||
            upload_vertices(gpu, gpu->solid_buffer, list->vertices, bytes) != 0) {
            return 1;
        }
        gpu->uploaded_vertices = list->vertex_count;
    }

    if (list->glyph_vertex_count > 0) {
        bytes = (size_t)list->glyph_vertex_count * sizeof(wn_glyph_vertex);
        if (ensure_buffer(gpu, &gpu->text_buffer, &gpu->text_capacity, bytes) != 0 ||
            upload_vertices(gpu, gpu->text_buffer, list->glyph_vertices, bytes) != 0) {
            return 1;
        }
        gpu->uploaded_glyphs = list->glyph_vertex_count;
    }

    if (list->image_vertex_count > 0) {
        int b;
        bytes = (size_t)list->image_vertex_count * sizeof(wn_image_vertex);
        if (ensure_buffer(gpu, &gpu->image_buffer, &gpu->image_capacity, bytes) != 0 ||
            upload_vertices(gpu, gpu->image_buffer, list->image_vertices, bytes) != 0) {
            return 1;
        }
        gpu->uploaded_images = list->image_vertex_count;
        for (b = 0; b < list->image_batch_count; ++b) {
            const wn_image_batch *batch = &list->image_batches[b];
            if (batch->src == NULL) {
                continue;
            }
            if (ensure_image_texture(gpu, batch->src) == NULL) {
                /* A broken poster only drops that quad; the frame stays alive. */
                gpu->stats.image_failures += 1;
                set_error(gpu, "image '%s' skipped (decode/cache failure #%d)",
                          batch->src, gpu->stats.image_failures);
                continue;
            }
        }
    }
    return 0;
}

static int set_scissor(WnD3D11 *gpu, float x, float y, float w, float h)
{
    int left = (int)floorf(x);
    int top = (int)floorf(y);
    int right = (int)ceilf(x + w);
    int bottom = (int)ceilf(y + h);
    D3D11_RECT rect;

    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > gpu->width) right = gpu->width;
    if (bottom > gpu->height) bottom = gpu->height;
    if (right <= left || bottom <= top) {
        return 0;
    }
    rect.left = (LONG)left;
    rect.top = (LONG)top;
    rect.right = (LONG)right;
    rect.bottom = (LONG)bottom;
    gpu->context->lpVtbl->RSSetScissorRects(gpu->context, 1, &rect);
    return 1;
}

static void release_layer_targets(WnD3D11 *gpu)
{
    int i;
    for (i = 0; i < gpu->layer_target_count; ++i) {
        com_release((IUnknown *)gpu->layer_targets[i].srv);
        com_release((IUnknown *)gpu->layer_targets[i].rtv);
        com_release((IUnknown *)gpu->layer_targets[i].texture);
    }
    wn_free_(gpu->layer_targets);
    gpu->layer_targets = NULL;
    gpu->layer_target_count = 0;
}

/* Lazily creates the viewport-sized RGBA8 target for nesting index `depth`.
 * A resize releases the pool; the next render recreates only the depths the
 * page actually nests. */
static int ensure_layer_target(WnD3D11 *gpu, int depth)
{
    WnD3D11LayerTarget *target;
    D3D11_TEXTURE2D_DESC desc;
    HRESULT hr;

    if (depth < 0 || depth >= WN_MAX_PAINT_LAYER_DEPTH) {
        set_error(gpu, "layer depth %d exceeds %d", depth, WN_MAX_PAINT_LAYER_DEPTH);
        return 1;
    }
    if (depth >= gpu->layer_target_count) {
        int capacity = gpu->layer_target_count > 0 ? gpu->layer_target_count : 4;
        WnD3D11LayerTarget *grown;
        while (capacity <= depth) {
            capacity *= 2;
        }
        grown = (WnD3D11LayerTarget *)realloc(
            gpu->layer_targets, (size_t)capacity * sizeof(WnD3D11LayerTarget));
        if (grown == NULL) {
            set_error(gpu, "out of memory for layer targets");
            return 1;
        }
        memset(grown + gpu->layer_target_count, 0,
               (size_t)(capacity - gpu->layer_target_count) * sizeof(*grown));
        gpu->layer_targets = grown;
        gpu->layer_target_count = capacity;
    }
    target = &gpu->layer_targets[depth];
    if (target->texture != NULL) {
        return 0;
    }
    memset(&desc, 0, sizeof(desc));
    desc.Width = (UINT)(gpu->width > 0 ? gpu->width : 1);
    desc.Height = (UINT)(gpu->height > 0 ? gpu->height : 1);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = gpu->device->lpVtbl->CreateTexture2D(gpu->device, &desc, NULL,
                                              &target->texture);
    if (FAILED(hr) || target->texture == NULL) {
        set_error(gpu, "layer CreateTexture2D(%dx%d) failed (hr=0x%08lX)",
                  gpu->width, gpu->height, (unsigned long)hr);
        return 1;
    }
    hr = gpu->device->lpVtbl->CreateRenderTargetView(
        gpu->device, (ID3D11Resource *)target->texture, NULL, &target->rtv);
    if (FAILED(hr) || target->rtv == NULL) {
        set_error(gpu, "layer CreateRenderTargetView failed (hr=0x%08lX)",
                  (unsigned long)hr);
        return 1;
    }
    hr = gpu->device->lpVtbl->CreateShaderResourceView(
        gpu->device, (ID3D11Resource *)target->texture, NULL, &target->srv);
    if (FAILED(hr) || target->srv == NULL) {
        set_error(gpu, "layer CreateShaderResourceView failed (hr=0x%08lX)",
                  (unsigned long)hr);
        return 1;
    }
    return 0;
}

static void draw_solid_range(WnD3D11 *gpu, const wn_paint_range *range)
{
    UINT stride;
    UINT offset = 0;
    if (range->count <= 0 || gpu->uploaded_vertices <= 0) {
        return;
    }
    if (!set_scissor(gpu, range->clip.x, range->clip.y, range->clip.w,
                     range->clip.h)) {
        return;
    }
    stride = (UINT)sizeof(wn_vertex);
    gpu->context->lpVtbl->IASetInputLayout(gpu->context, gpu->solid_layout);
    gpu->context->lpVtbl->IASetVertexBuffers(gpu->context, 0, 1,
                                             &gpu->solid_buffer, &stride, &offset);
    gpu->context->lpVtbl->VSSetShader(gpu->context, gpu->solid_vs, NULL, 0);
    gpu->context->lpVtbl->PSSetShader(gpu->context, gpu->solid_ps, NULL, 0);
    gpu->context->lpVtbl->Draw(gpu->context, (UINT)range->count,
                               (UINT)range->first);
}

static void draw_image_range(WnD3D11 *gpu, const wn_paint_draw *draw)
{
    const wn_paint_range *range = &draw->image;
    WnD3D11Image *texture;
    UINT stride;
    UINT offset = 0;
    if (range->count <= 0 || draw->image_src == NULL ||
        gpu->uploaded_images <= 0) {
        return;
    }
    texture = find_cached_image(gpu, draw->image_src);
    if (texture == NULL || texture->view == NULL) {
        return;
    }
    if (!set_scissor(gpu, range->clip.x, range->clip.y, range->clip.w,
                     range->clip.h)) {
        return;
    }
    stride = (UINT)sizeof(wn_image_vertex);
    gpu->context->lpVtbl->IASetInputLayout(gpu->context, gpu->image_layout);
    gpu->context->lpVtbl->IASetVertexBuffers(gpu->context, 0, 1,
                                             &gpu->image_buffer, &stride, &offset);
    gpu->context->lpVtbl->VSSetShader(gpu->context, gpu->image_vs, NULL, 0);
    gpu->context->lpVtbl->PSSetShader(gpu->context, gpu->image_ps, NULL, 0);
    gpu->context->lpVtbl->PSSetSamplers(gpu->context, 0, 1, &gpu->sampler);
    gpu->context->lpVtbl->PSSetShaderResources(gpu->context, 0, 1, &texture->view);
    gpu->context->lpVtbl->Draw(gpu->context, (UINT)range->count,
                               (UINT)range->first);
}

static void draw_glyph_range(WnD3D11 *gpu, const wn_paint_range *range)
{
    UINT stride;
    UINT offset = 0;
    if (range->count <= 0 || gpu->uploaded_glyphs <= 0 ||
        gpu->atlas_view == NULL) {
        return;
    }
    if (!set_scissor(gpu, range->clip.x, range->clip.y, range->clip.w,
                     range->clip.h)) {
        return;
    }
    stride = (UINT)sizeof(wn_glyph_vertex);
    gpu->context->lpVtbl->IASetInputLayout(gpu->context, gpu->text_layout);
    gpu->context->lpVtbl->IASetVertexBuffers(gpu->context, 0, 1,
                                             &gpu->text_buffer, &stride, &offset);
    gpu->context->lpVtbl->VSSetShader(gpu->context, gpu->text_vs, NULL, 0);
    gpu->context->lpVtbl->PSSetShader(gpu->context, gpu->text_ps, NULL, 0);
    gpu->context->lpVtbl->PSSetSamplers(gpu->context, 0, 1, &gpu->sampler);
    gpu->context->lpVtbl->PSSetShaderResources(gpu->context, 0, 1,
                                               &gpu->atlas_view);
    gpu->context->lpVtbl->Draw(gpu->context, (UINT)range->count,
                               (UINT)range->first);
}

/* One paint item in the layered replay, in the same pass order the flat
 * renderer uses: solids (background, shadow, input decorations, scrollbars),
 * then the image, then text. */
static void draw_paint_item(WnD3D11 *gpu, const wn_paint_draw *draw)
{
    draw_solid_range(gpu, &draw->solid);
    draw_solid_range(gpu, &draw->decoration);
    draw_solid_range(gpu, &draw->input_body);
    draw_solid_range(gpu, &draw->scrollbar);
    draw_image_range(gpu, draw);
    draw_glyph_range(gpu, &draw->glyph);
    draw_glyph_range(gpu, &draw->input_glyph);
}

static void composite_layer(WnD3D11 *gpu, ID3D11RenderTargetView *parent,
                            const WnD3D11LayerTarget *layer_target,
                            const wn_paint_layer *layer)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    ID3D11ShaderResourceView *views[1];
    ID3D11ShaderResourceView *null_views[1] = { NULL };

    gpu->context->lpVtbl->OMSetRenderTargets(gpu->context, 1, &parent, NULL);
    gpu->context->lpVtbl->IASetInputLayout(gpu->context, NULL);
    gpu->context->lpVtbl->VSSetShader(gpu->context, gpu->composite_vs, NULL, 0);
    gpu->context->lpVtbl->PSSetShader(gpu->context, gpu->composite_ps, NULL, 0);
    memset(&mapped, 0, sizeof(mapped));
    if (SUCCEEDED(gpu->context->lpVtbl->Map(gpu->context,
                                            (ID3D11Resource *)gpu->composite_params,
                                            0, D3D11_MAP_WRITE_DISCARD, 0,
                                            &mapped))) {
        ((float *)mapped.pData)[0] = layer->opacity;
        gpu->context->lpVtbl->Unmap(gpu->context,
                                    (ID3D11Resource *)gpu->composite_params, 0);
    }
    gpu->context->lpVtbl->PSSetConstantBuffers(gpu->context, 0, 1,
                                               &gpu->composite_params);
    gpu->context->lpVtbl->PSSetSamplers(gpu->context, 0, 1, &gpu->sampler);
    views[0] = layer_target->srv;
    gpu->context->lpVtbl->PSSetShaderResources(gpu->context, 0, 1, views);
    gpu->context->lpVtbl->OMSetBlendState(gpu->context, gpu->composite_blend,
                                          NULL, 0xFFFFFFFFu);
    if (set_scissor(gpu, layer->clip.x, layer->clip.y, layer->clip.w,
                    layer->clip.h)) {
        gpu->context->lpVtbl->Draw(gpu->context, 3, 0);
    }
    gpu->context->lpVtbl->PSSetShaderResources(gpu->context, 0, 1, null_views);
    gpu->context->lpVtbl->OMSetBlendState(gpu->context, gpu->blend, NULL,
                                          0xFFFFFFFFu);
    gpu->context->lpVtbl->IASetPrimitiveTopology(gpu->context,
                                                 D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

static int render_layered(WnD3D11 *gpu, const wn_display_list *list)
{
    ID3D11RenderTargetView *targets[WN_MAX_PAINT_LAYER_DEPTH];
    float transparent[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    int depth = 0;
    int i;

    if (list->layer_count > WN_MAX_PAINT_LAYERS) {
        set_error(gpu, "list has %d layers (max %d)", list->layer_count,
                  WN_MAX_PAINT_LAYERS);
        return 1;
    }
    targets[0] = gpu->rtv;
    for (i = 0; i < list->command_count; ++i) {
        const wn_paint_command *command = &list->commands[i];
        if (command->kind == WN_PAINT_CMD_DRAW) {
            if (command->item >= 0 && command->item < list->draw_count) {
                draw_paint_item(gpu, &list->draws[command->item]);
            }
            continue;
        }
        if (command->kind == WN_PAINT_CMD_BEGIN_LAYER) {
            WnD3D11LayerTarget *target;
            if (depth + 1 >= WN_MAX_PAINT_LAYER_DEPTH) {
                set_error(gpu, "layer nesting exceeds %d", WN_MAX_PAINT_LAYER_DEPTH);
                return 1;
            }
            if (ensure_layer_target(gpu, depth) != 0) {
                return 1;
            }
            target = &gpu->layer_targets[depth];
            gpu->context->lpVtbl->ClearRenderTargetView(gpu->context, target->rtv,
                                                        transparent);
            gpu->context->lpVtbl->OMSetRenderTargets(gpu->context, 1, &target->rtv,
                                                     NULL);
            targets[++depth] = target->rtv;
            continue;
        }
        if (command->kind == WN_PAINT_CMD_END_LAYER) {
            const wn_paint_layer *layer;
            if (depth <= 0 || command->layer < 0 ||
                command->layer >= list->layer_count) {
                continue;
            }
            layer = &list->layers[command->layer];
            composite_layer(gpu, targets[depth - 1], &gpu->layer_targets[depth - 1],
                            layer);
            --depth;
        }
    }
    return 0;
}

static int renderer_render(void *context, const wn_display_list *list)
{
    WnD3D11 *gpu = (WnD3D11 *)context;
    float clear[4] = { 0.043137f, 0.054902f, 0.078431f, 1.0f };
    D3D11_VIEWPORT viewport;
    D3D11_RECT full_scissor;
    UINT stride;
    UINT offset = 0;
    double t0;
    double cpu_ms;
    double present_ms = 0.0;
    HRESULT hr;

    if (gpu == NULL) {
        return 1;
    }
    if (gpu->width <= 0 || gpu->height <= 0) {
        return 0;
    }
    if (gpu->device == NULL || gpu->context == NULL ||
        (gpu->swapchain == NULL && !gpu->offscreen)) {
        set_error(gpu, "renderer is not initialized");
        return 1;
    }
    if (gpu->rtv == NULL && create_render_target(gpu) != 0) {
        return 1;
    }
    if (list != NULL) {
        clear[0] = list->clear_color[0];
        clear[1] = list->clear_color[1];
        clear[2] = list->clear_color[2];
        clear[3] = list->clear_color[3];
    }

    t0 = wn_now_ms();
    gpu->context->lpVtbl->OMSetRenderTargets(gpu->context, 1, &gpu->rtv, NULL);
    gpu->context->lpVtbl->ClearRenderTargetView(gpu->context, gpu->rtv, clear);

    memset(&viewport, 0, sizeof(viewport));
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = (float)gpu->width;
    viewport.Height = (float)gpu->height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    gpu->context->lpVtbl->RSSetViewports(gpu->context, 1, &viewport);
    full_scissor.left = 0;
    full_scissor.top = 0;
    full_scissor.right = (LONG)gpu->width;
    full_scissor.bottom = (LONG)gpu->height;
    gpu->context->lpVtbl->RSSetScissorRects(gpu->context, 1, &full_scissor);
    gpu->context->lpVtbl->RSSetState(gpu->context, gpu->raster);
    gpu->context->lpVtbl->OMSetBlendState(gpu->context, gpu->blend, NULL, 0xFFFFFFFFu);
    gpu->context->lpVtbl->IASetPrimitiveTopology(gpu->context,
                                                 D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    if (list != NULL && list->layer_count > 1 && list->command_count > 0) {
        if (render_layered(gpu, list) != 0) {
            return 1;
        }
    } else {
    if (gpu->uploaded_vertices > 0) {
        int b;
        stride = (UINT)sizeof(wn_vertex);
        gpu->context->lpVtbl->IASetInputLayout(gpu->context, gpu->solid_layout);
        gpu->context->lpVtbl->IASetVertexBuffers(gpu->context, 0, 1, &gpu->solid_buffer,
                                                 &stride, &offset);
        gpu->context->lpVtbl->VSSetShader(gpu->context, gpu->solid_vs, NULL, 0);
        gpu->context->lpVtbl->PSSetShader(gpu->context, gpu->solid_ps, NULL, 0);
        if (list != NULL && list->solid_batch_count > 0) {
            for (b = 0; b < list->solid_batch_count; ++b) {
                const wn_draw_batch *batch = &list->solid_batches[b];
                if (set_scissor(gpu, batch->clip_x, batch->clip_y, batch->clip_w,
                                batch->clip_h)) {
                    gpu->context->lpVtbl->Draw(gpu->context, (UINT)batch->count,
                                               (UINT)batch->first);
                }
            }
        } else {
            gpu->context->lpVtbl->Draw(gpu->context, (UINT)gpu->uploaded_vertices, 0);
        }
    }

    if (gpu->uploaded_images > 0 && list != NULL && list->image_batch_count > 0) {
        int b;
        stride = (UINT)sizeof(wn_image_vertex);
        gpu->context->lpVtbl->IASetInputLayout(gpu->context, gpu->image_layout);
        gpu->context->lpVtbl->IASetVertexBuffers(gpu->context, 0, 1, &gpu->image_buffer,
                                                 &stride, &offset);
        gpu->context->lpVtbl->VSSetShader(gpu->context, gpu->image_vs, NULL, 0);
        gpu->context->lpVtbl->PSSetShader(gpu->context, gpu->image_ps, NULL, 0);
        gpu->context->lpVtbl->PSSetSamplers(gpu->context, 0, 1, &gpu->sampler);
        for (b = 0; b < list->image_batch_count; ++b) {
            const wn_image_batch *batch = &list->image_batches[b];
            WnD3D11Image *texture = batch->src != NULL
                                        ? find_cached_image(gpu, batch->src)
                                        : NULL;
            if (texture == NULL || texture->view == NULL) {
                continue;
            }
            if (!set_scissor(gpu, batch->clip_x, batch->clip_y, batch->clip_w,
                             batch->clip_h)) {
                continue;
            }
            gpu->context->lpVtbl->PSSetShaderResources(gpu->context, 0, 1,
                                                       &texture->view);
            gpu->context->lpVtbl->Draw(gpu->context, (UINT)batch->count,
                                       (UINT)batch->first);
        }
    }

    if (gpu->uploaded_glyphs > 0 && gpu->atlas_view != NULL) {
        int b;
        stride = (UINT)sizeof(wn_glyph_vertex);
        gpu->context->lpVtbl->IASetInputLayout(gpu->context, gpu->text_layout);
        gpu->context->lpVtbl->IASetVertexBuffers(gpu->context, 0, 1, &gpu->text_buffer,
                                                 &stride, &offset);
        gpu->context->lpVtbl->VSSetShader(gpu->context, gpu->text_vs, NULL, 0);
        gpu->context->lpVtbl->PSSetShader(gpu->context, gpu->text_ps, NULL, 0);
        gpu->context->lpVtbl->PSSetSamplers(gpu->context, 0, 1, &gpu->sampler);
        gpu->context->lpVtbl->PSSetShaderResources(gpu->context, 0, 1, &gpu->atlas_view);
        if (list != NULL && list->glyph_batch_count > 0) {
            for (b = 0; b < list->glyph_batch_count; ++b) {
                const wn_draw_batch *batch = &list->glyph_batches[b];
                if (set_scissor(gpu, batch->clip_x, batch->clip_y, batch->clip_w,
                                batch->clip_h)) {
                    gpu->context->lpVtbl->Draw(gpu->context, (UINT)batch->count,
                                               (UINT)batch->first);
                }
            }
        } else {
            gpu->context->lpVtbl->Draw(gpu->context, (UINT)gpu->uploaded_glyphs, 0);
        }
    }
    }
    cpu_ms = wn_now_ms() - t0;

    if (gpu->capture_path[0] != '\0' &&
        gpu->stats.frames + 1 == gpu->capture_frame) {
        if (capture_backbuffer(gpu, gpu->capture_path) != 0) {
            fprintf(stderr, "renderer: capture to '%s' failed\n", gpu->capture_path);
            fflush(stderr);
        } else {
            printf("renderer: captured frame %d -> %s\n", gpu->stats.frames + 1,
                   gpu->capture_path);
            fflush(stdout);
        }
    }

    if (gpu->offscreen) {
        /* No present: the frame stays in the render target until readback. */
        gpu->stats.frames += 1;
        gpu->stats.last_cpu_ms = cpu_ms;
        gpu->stats.last_present_ms = 0.0;
        return 0;
    }

    {
        double p0 = wn_now_ms();
        /* Present(1, 0) caps the app to the display refresh with vsync. */
        hr = gpu->swapchain->lpVtbl->Present(gpu->swapchain, 1, 0);
        present_ms = wn_now_ms() - p0;
    }
    if (FAILED(hr)) {
        set_error(gpu, "Present failed (hr=0x%08lX)", (unsigned long)hr);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            set_error(gpu, "D3D11 device removed/reset (hr=0x%08lX)",
                      (unsigned long)hr);
        }
        return 1;
    }

    gpu->stats.frames += 1;
    gpu->stats.last_cpu_ms = cpu_ms;
    gpu->stats.last_present_ms = present_ms;
    return 0;
}

static void renderer_destroy(void *context)
{
    WnD3D11 *gpu = (WnD3D11 *)context;
    int i;
    if (gpu == NULL) {
        return;
    }
    if (gpu->context != NULL) {
        gpu->context->lpVtbl->ClearState(gpu->context);
        gpu->context->lpVtbl->Flush(gpu->context);
    }
    com_release((IUnknown *)gpu->solid_buffer);
    com_release((IUnknown *)gpu->text_buffer);
    com_release((IUnknown *)gpu->image_buffer);
    release_layer_targets(gpu);
    com_release((IUnknown *)gpu->composite_params);
    com_release((IUnknown *)gpu->composite_blend);
    com_release((IUnknown *)gpu->composite_ps);
    com_release((IUnknown *)gpu->composite_vs);
    for (i = 0; i < gpu->image_count; ++i) {
        release_image_entry(&gpu->images[i]);
    }
    wn_free_(gpu->images);
    com_release((IUnknown *)gpu->wic);
    com_release((IUnknown *)gpu->raster);
    com_release((IUnknown *)gpu->blend);
    com_release((IUnknown *)gpu->sampler);
    com_release((IUnknown *)gpu->atlas_view);
    com_release((IUnknown *)gpu->atlas_texture);
    com_release((IUnknown *)gpu->image_layout);
    com_release((IUnknown *)gpu->image_ps);
    com_release((IUnknown *)gpu->image_vs);
    com_release((IUnknown *)gpu->text_layout);
    com_release((IUnknown *)gpu->text_ps);
    com_release((IUnknown *)gpu->text_vs);
    com_release((IUnknown *)gpu->solid_layout);
    com_release((IUnknown *)gpu->solid_ps);
    com_release((IUnknown *)gpu->solid_vs);
    com_release((IUnknown *)gpu->rtv);
    com_release((IUnknown *)gpu->backbuffer);
    com_release((IUnknown *)gpu->swapchain);
    com_release((IUnknown *)gpu->context);
    com_release((IUnknown *)gpu->device);
    if (gpu->com_initialized) {
        CoUninitialize();
    }
    free(gpu);
}

static const char *renderer_last_error(void *context)
{
    WnD3D11 *gpu = (WnD3D11 *)context;
    if (gpu == NULL || gpu->error[0] == '\0') {
        return NULL;
    }
    return gpu->error;
}

static void renderer_get_info(void *context, wn_renderer_info *info)
{
    WnD3D11 *gpu = (WnD3D11 *)context;
    if (gpu != NULL && info != NULL) {
        *info = gpu->info;
    }
}

static void renderer_get_stats(void *context, wn_renderer_stats *stats)
{
    WnD3D11 *gpu = (WnD3D11 *)context;
    if (gpu != NULL && stats != NULL) {
        *stats = gpu->stats;
    }
}

static int renderer_readback(void *context, const char *path)
{
    WnD3D11 *gpu = (WnD3D11 *)context;
    if (gpu == NULL || path == NULL || path[0] == '\0') {
        return 1;
    }
    if (!gpu->offscreen) {
        set_error(gpu, "readback requires an offscreen surface");
        return 1;
    }
    if (capture_backbuffer(gpu, path) != 0) {
        set_error(gpu, "readback failed");
        return 1;
    }
    return 0;
}

static int renderer_image_cache_count(void *context)
{
    WnD3D11 *gpu = (WnD3D11 *)context;
    return gpu != NULL ? gpu->image_count : -1;
}

static long long renderer_image_cache_bytes(void *context)
{
    WnD3D11 *gpu = (WnD3D11 *)context;
    return gpu != NULL ? (long long)gpu->image_cache_bytes : -1;
}

static const wn_renderer_ops g_d3d11_ops = {
    "d3d11",
    renderer_init,
    renderer_resize,
    renderer_upload,
    renderer_render,
    renderer_destroy,
    renderer_last_error,
    renderer_get_info,
    renderer_get_stats,
    renderer_readback,
    renderer_image_cache_count,
    renderer_image_cache_bytes
};

const wn_renderer_ops *wn_renderer_d3d11(void)
{
    return &g_d3d11_ops;
}
