#include "render/image.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace render {

using Microsoft::WRL::ComPtr;

DecodedImage DecodeImage(const uint8_t* data, size_t size) {
    DecodedImage img;
    HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct CoUninit {
        HRESULT hr;
        ~CoUninit() {
            if (SUCCEEDED(hr)) CoUninitialize();
        }
    } guard{coInit};

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        img.error = "Windows Imaging Component is not available";
        return img;
    }
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data), DWORD(size)))) {
        img.error = "cannot create an image stream";
        return img;
    }
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) {
        img.error = "unsupported or corrupt image format";
        return img;
    }
    if (FAILED(decoder->GetFrame(0, &frame))) {
        img.error = "image has no frames";
        return img;
    }
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (w == 0 || h == 0 || w > 32768 || h > 32768) {
        img.error = "invalid image size";
        return img;
    }
    WICPixelFormatGUID srcFormat{};
    frame->GetPixelFormat(&srcFormat);
    ComPtr<IWICComponentInfo> info;
    if (SUCCEEDED(factory->CreateComponentInfo(srcFormat, &info))) {
        ComPtr<IWICPixelFormatInfo2> pf;
        if (SUCCEEDED(info.As(&pf))) {
            BOOL alpha = FALSE;
            pf->SupportsTransparency(&alpha);
            img.hasAlpha = alpha != FALSE;
        }
    }
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        img.error = "cannot convert image pixels";
        return img;
    }
    img.rgba.resize(size_t(w) * h * 4);
    if (FAILED(converter->CopyPixels(nullptr, w * 4, UINT(img.rgba.size()), img.rgba.data()))) {
        img.rgba.clear();
        img.error = "cannot decode image pixels";
        return img;
    }
    img.width = int(w);
    img.height = int(h);
    return img;
}

}  // namespace render
