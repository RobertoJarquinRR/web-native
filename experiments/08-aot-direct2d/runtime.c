/*
 * runtime.c - pure C fallback renderer for the baked scene.
 *
 * Contains NO scene-specific data and NO JSON parser: it only knows how to
 * paint an array of RectOp values emitted by emit_c.py into scene_data.h.
 * Software rasterizer (source-over on an opaque canvas) + WIC PNG encoder.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include <math.h>

#include <wincodec.h>

#include "scene_data.h"

static int report(HRESULT hr, const char *what)
{
    fprintf(stderr, "error: %s failed (hr=0x%08lX)\n", what, (unsigned long)hr);
    return 1;
}

static void blend_pixel(unsigned char *pixel, unsigned int rgba)
{
    unsigned int r = (rgba >> 24) & 0xFFu;
    unsigned int g = (rgba >> 16) & 0xFFu;
    unsigned int b = (rgba >> 8) & 0xFFu;
    unsigned int a = rgba & 0xFFu;

    if (a == 0xFFu) {
        pixel[0] = (unsigned char)b;
        pixel[1] = (unsigned char)g;
        pixel[2] = (unsigned char)r;
        pixel[3] = 255;
        return;
    }

    {
        unsigned int inv = 255u - a;
        pixel[0] = (unsigned char)((b * a + pixel[0] * inv + 127u) / 255u);
        pixel[1] = (unsigned char)((g * a + pixel[1] * inv + 127u) / 255u);
        pixel[2] = (unsigned char)((r * a + pixel[2] * inv + 127u) / 255u);
        pixel[3] = (unsigned char)(a + (pixel[3] * inv + 127u) / 255u);
    }
}

static int save_png(IWICImagingFactory *wic, IWICBitmap *bitmap, const wchar_t *path)
{
    HRESULT hr;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *options = NULL;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;

    hr = wic->lpVtbl->CreateStream(wic, &stream);
    if (FAILED(hr))
        return report(hr, "CreateStream");
    hr = stream->lpVtbl->InitializeFromFilename(stream, path, GENERIC_WRITE);
    if (FAILED(hr))
        return report(hr, "IWICStream::InitializeFromFilename");

    hr = wic->lpVtbl->CreateEncoder(wic, &GUID_ContainerFormatPng, NULL, &encoder);
    if (FAILED(hr))
        return report(hr, "CreateEncoder(png)");
    hr = encoder->lpVtbl->Initialize(encoder, (IStream *)stream, WICBitmapEncoderNoCache);
    if (FAILED(hr))
        return report(hr, "IWICBitmapEncoder::Initialize");

    hr = encoder->lpVtbl->CreateNewFrame(encoder, &frame, &options);
    if (FAILED(hr))
        return report(hr, "CreateNewFrame");
    if (options != NULL)
        options->lpVtbl->Release(options);
    hr = frame->lpVtbl->Initialize(frame, NULL);
    if (FAILED(hr))
        return report(hr, "IWICBitmapFrameEncode::Initialize");
    hr = frame->lpVtbl->SetSize(frame, SCENE_WIDTH, SCENE_HEIGHT);
    if (FAILED(hr))
        return report(hr, "IWICBitmapFrameEncode::SetSize");
    hr = frame->lpVtbl->SetPixelFormat(frame, &format);
    if (FAILED(hr))
        return report(hr, "IWICBitmapFrameEncode::SetPixelFormat");
    hr = frame->lpVtbl->WriteSource(frame, (IWICBitmapSource *)bitmap, NULL);
    if (FAILED(hr))
        return report(hr, "IWICBitmapFrameEncode::WriteSource");
    hr = frame->lpVtbl->Commit(frame);
    if (FAILED(hr))
        return report(hr, "IWICBitmapFrameEncode::Commit");
    hr = encoder->lpVtbl->Commit(encoder);
    if (FAILED(hr))
        return report(hr, "IWICBitmapEncoder::Commit");

    frame->lpVtbl->Release(frame);
    encoder->lpVtbl->Release(encoder);
    stream->lpVtbl->Release(stream);
    return 0;
}

int main(int argc, char **argv)
{
    HRESULT hr;
    IWICImagingFactory *wic = NULL;
    IWICBitmap *bitmap = NULL;
    IWICBitmapLock *lock = NULL;
    WICRect area;
    UINT stride = 0, bytes = 0;
    BYTE *pixels = NULL;
    wchar_t wide_path[MAX_PATH];
    const wchar_t *out_path = L"scene.png";
    int exit_code = 1;
    int y, x, i;

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

    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWICImagingFactory, (void **)&wic);
    if (FAILED(hr))
        return report(hr, "CoCreateInstance(WICImagingFactory)");

    hr = wic->lpVtbl->CreateBitmap(wic, SCENE_WIDTH, SCENE_HEIGHT,
                                   &GUID_WICPixelFormat32bppBGRA,
                                   WICBitmapCacheOnLoad, &bitmap);
    if (FAILED(hr))
        return report(hr, "IWICImagingFactory::CreateBitmap");

    area.X = 0;
    area.Y = 0;
    area.Width = SCENE_WIDTH;
    area.Height = SCENE_HEIGHT;
    hr = bitmap->lpVtbl->Lock(bitmap, &area, WICBitmapLockWrite, &lock);
    if (FAILED(hr))
        return report(hr, "IWICBitmap::Lock");
    hr = lock->lpVtbl->GetStride(lock, &stride);
    if (FAILED(hr))
        return report(hr, "IWICBitmapLock::GetStride");
    hr = lock->lpVtbl->GetDataPointer(lock, &bytes, &pixels);
    if (FAILED(hr) || pixels == NULL || bytes < stride * SCENE_HEIGHT)
        return report(FAILED(hr) ? hr : E_FAIL, "IWICBitmapLock::GetDataPointer");

    /* Clear to the first op's color, then paint every op in painter's order. */
    for (y = 0; y < SCENE_HEIGHT; ++y) {
        BYTE *row = pixels + (size_t)y * stride;
        for (x = 0; x < SCENE_WIDTH; ++x)
            blend_pixel(row + (size_t)x * 4, kSceneOps[0].rgba);
    }

    for (i = 0; i < SCENE_OP_COUNT; ++i) {
        const RectOp *op = &kSceneOps[i];
        int x0 = (int)floorf(op->x + 0.5f);
        int y0 = (int)floorf(op->y + 0.5f);
        int x1 = (int)floorf(op->x + op->w + 0.5f);
        int y1 = (int)floorf(op->y + op->h + 0.5f);

        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > SCENE_WIDTH) x1 = SCENE_WIDTH;
        if (y1 > SCENE_HEIGHT) y1 = SCENE_HEIGHT;

        for (y = y0; y < y1; ++y) {
            BYTE *row = pixels + (size_t)y * stride;
            for (x = x0; x < x1; ++x)
                blend_pixel(row + (size_t)x * 4, op->rgba);
        }
    }

    lock->lpVtbl->Release(lock);
    lock = NULL;

    if (save_png(wic, bitmap, out_path) != 0)
        goto cleanup;

    wprintf(L"scene.exe [cpu]: %dx%d, %d rect op(s) -> %ls\n",
            SCENE_WIDTH, SCENE_HEIGHT, SCENE_OP_COUNT, out_path);
    exit_code = 0;

cleanup:
    if (lock != NULL)
        lock->lpVtbl->Release(lock);
    if (bitmap != NULL)
        bitmap->lpVtbl->Release(bitmap);
    if (wic != NULL)
        wic->lpVtbl->Release(wic);
    CoUninitialize();
    return exit_code;
}
