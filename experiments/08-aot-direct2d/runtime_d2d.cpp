/*
 * runtime_d2d.cpp - primary native renderer for the baked scene.
 *
 * Contains NO scene-specific data and NO JSON parser: it only knows how to
 * paint an array of RectOp values emitted by emit_c.py into scene_data.h.
 * Uses Direct2D for drawing and WIC for PNG encoding (Windows only).
 *
 * Compiled as C++ because the Windows SDK's d2d1.h only exposes C++ vtables;
 * the pure C path lives in runtime.c (software rasterizer).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

#include <d2d1.h>
#include <wincodec.h>

#include "scene_data.h"

static int report(HRESULT hr, const char *what)
{
    fprintf(stderr, "error: %s failed (hr=0x%08lX)\n", what, (unsigned long)hr);
    return 1;
}

static D2D1_COLOR_F color_from_rgba(unsigned int rgba)
{
    D2D1_COLOR_F color;
    color.r = ((rgba >> 24) & 0xFFu) / 255.0f;
    color.g = ((rgba >> 16) & 0xFFu) / 255.0f;
    color.b = ((rgba >> 8) & 0xFFu) / 255.0f;
    color.a = (rgba & 0xFFu) / 255.0f;
    return color;
}

static int save_png(IWICImagingFactory *wic, IWICBitmap *bitmap, const wchar_t *path)
{
    HRESULT hr;
    IWICFormatConverter *converter = NULL;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *options = NULL;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    UINT width = 0, height = 0;

    hr = wic->CreateFormatConverter(&converter);
    if (FAILED(hr))
        return report(hr, "CreateFormatConverter");
    hr = converter->Initialize((IWICBitmapSource *)bitmap,
                               GUID_WICPixelFormat32bppBGRA,
                               WICBitmapDitherTypeNone, NULL, 0.0,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr))
        return report(hr, "IWICFormatConverter::Initialize");
    hr = converter->GetSize(&width, &height);
    if (FAILED(hr))
        return report(hr, "IWICFormatConverter::GetSize");

    hr = wic->CreateStream(&stream);
    if (FAILED(hr))
        return report(hr, "CreateStream");
    hr = stream->InitializeFromFilename(path, GENERIC_WRITE);
    if (FAILED(hr))
        return report(hr, "IWICStream::InitializeFromFilename");

    hr = wic->CreateEncoder(GUID_ContainerFormatPng, NULL, &encoder);
    if (FAILED(hr))
        return report(hr, "CreateEncoder(png)");
    hr = encoder->Initialize((IStream *)stream, WICBitmapEncoderNoCache);
    if (FAILED(hr))
        return report(hr, "IWICBitmapEncoder::Initialize");

    hr = encoder->CreateNewFrame(&frame, &options);
    if (FAILED(hr))
        return report(hr, "CreateNewFrame");
    if (options != NULL)
        options->Release();
    hr = frame->Initialize(NULL);
    if (FAILED(hr))
        return report(hr, "IWICBitmapFrameEncode::Initialize");
    hr = frame->SetSize(width, height);
    if (FAILED(hr))
        return report(hr, "IWICBitmapFrameEncode::SetSize");
    hr = frame->SetPixelFormat(&format);
    if (FAILED(hr))
        return report(hr, "IWICBitmapFrameEncode::SetPixelFormat");
    hr = frame->WriteSource((IWICBitmapSource *)converter, NULL);
    if (FAILED(hr))
        return report(hr, "IWICBitmapFrameEncode::WriteSource");
    hr = frame->Commit();
    if (FAILED(hr))
        return report(hr, "IWICBitmapFrameEncode::Commit");
    hr = encoder->Commit();
    if (FAILED(hr))
        return report(hr, "IWICBitmapEncoder::Commit");

    frame->Release();
    encoder->Release();
    stream->Release();
    converter->Release();
    return 0;
}

int main(int argc, char **argv)
{
    HRESULT hr;
    IWICImagingFactory *wic = NULL;
    IWICBitmap *bitmap = NULL;
    ID2D1Factory *factory = NULL;
    ID2D1RenderTarget *target = NULL;
    D2D1_RENDER_TARGET_PROPERTIES props;
    D2D1_COLOR_F clear_color;
    wchar_t wide_path[MAX_PATH];
    const wchar_t *out_path = L"scene.png";
    int exit_code = 1;

    if (argc > 1) {
        if (MultiByteToWideChar(CP_ACP, 0, argv[1], -1, wide_path, MAX_PATH) == 0) {
            fprintf(stderr, "error: output path too long\n");
            return 1;
        }
        out_path = wide_path;
    }

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
        return report(hr, "CoInitializeEx");

    hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&wic));
    if (FAILED(hr))
        return report(hr, "CoCreateInstance(WICImagingFactory)");

    hr = wic->CreateBitmap(SCENE_WIDTH, SCENE_HEIGHT,
                           GUID_WICPixelFormat32bppPBGRA,
                           WICBitmapCacheOnLoad, &bitmap);
    if (FAILED(hr))
        return report(hr, "IWICImagingFactory::CreateBitmap");

    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory),
                           NULL, reinterpret_cast<void **>(&factory));
    if (FAILED(hr))
        return report(hr, "D2D1CreateFactory");

    props.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
    props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    props.dpiX = 96.0f;
    props.dpiY = 96.0f;
    props.usage = D2D1_RENDER_TARGET_USAGE_NONE;
    props.minLevel = D2D1_FEATURE_LEVEL_DEFAULT;

    hr = factory->CreateWicBitmapRenderTarget(bitmap, &props, &target);
    if (FAILED(hr))
        return report(hr, "CreateWicBitmapRenderTarget");

    target->BeginDraw();

    clear_color = color_from_rgba(kSceneOps[0].rgba);
    target->Clear(&clear_color);

    for (int i = 0; i < SCENE_OP_COUNT; ++i) {
        const RectOp *op = &kSceneOps[i];
        ID2D1SolidColorBrush *brush = NULL;
        D2D1_COLOR_F color = color_from_rgba(op->rgba);
        D2D1_RECT_F rect;

        hr = target->CreateSolidColorBrush(&color, NULL, &brush);
        if (FAILED(hr))
            return report(hr, "CreateSolidColorBrush");
        rect.left = op->x;
        rect.top = op->y;
        rect.right = op->x + op->w;
        rect.bottom = op->y + op->h;
        target->FillRectangle(&rect, brush);
        brush->Release();
    }

    hr = target->EndDraw();
    if (FAILED(hr))
        return report(hr, "ID2D1RenderTarget::EndDraw");

    if (save_png(wic, bitmap, out_path) != 0)
        goto cleanup;

    wprintf(L"scene.exe [d2d]: %dx%d, %d rect op(s) -> %ls\n",
            SCENE_WIDTH, SCENE_HEIGHT, SCENE_OP_COUNT, out_path);
    exit_code = 0;

cleanup:
    if (target != NULL)
        target->Release();
    if (factory != NULL)
        factory->Release();
    if (bitmap != NULL)
        bitmap->Release();
    if (wic != NULL)
        wic->Release();
    CoUninitialize();
    return exit_code;
}
