// Optional HD atlases retain the Xbox font layout, metrics and button glyphs.
#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "core/log.h"
#include "gpu/texture.h"
#include "xdk/hle.h"

using namespace kernel;
using Microsoft::WRL::ComPtr;

namespace {

bool ReadAtlas(const wchar_t* filename, uint32_t w, uint32_t h, std::vector<uint8_t>& pixels) {
    std::array<wchar_t, 32768> path{};
    DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    if (!length || length >= path.size()) return false;
    std::wstring file(path.data(), length);
    auto slash = file.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    file.resize(slash + 1);
    file += L"fonts\\";
    file += filename;
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct ComScope {
        bool initialized;
        ~ComScope() { if (initialized) CoUninitialize(); }
    } scope{SUCCEEDED(com)};
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> straight;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (SUCCEEDED(hr)) hr = wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    UINT width = 0, height = 0;
    if (SUCCEEDED(hr)) hr = frame->GetSize(&width, &height);
    if (FAILED(hr) || width != w || height != h) return false;
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&straight);
    if (SUCCEEDED(hr)) hr = straight->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    pixels.resize(size_t(w) * h * 4);
    if (SUCCEEDED(hr)) hr = straight->CopyPixels(nullptr, w * 4, UINT(pixels.size()), pixels.data());
    return SUCCEEDED(hr);
}

uint32_t Deposit(uint32_t value, uint32_t mask) {
    uint32_t result = 0;
    for (uint32_t bit = 1; mask; bit <<= 1) {
        uint32_t low = mask & -mask;
        if (value & bit) result |= low;
        mask &= ~low;
    }
    return result;
}

void Upgrade(uint32_t texture) {
    gpu::TextureDesc d;
    if (!gpu::Describe(texture, d) || d.format != 0x0E || d.cube || d.depth != 1 || d.levels != 1 ||
        d.width < 4 || d.height < 4 || d.width > 1024 || d.height > 1024) return;
    static std::mutex mutex;
    static std::map<uint64_t, uint32_t> cache;  // font reloads share the same immutable pixels
    std::lock_guard lock(mutex);
    uint64_t key = 14695981039346656037ull ^ (uint64_t(d.width) << 32 | d.height);
    const uint8_t* original = static_cast<const uint8_t*>(HostPtr(d.data));
    for (uint32_t i = 0; i < gpu::TotalSize(d); i++) key = (key ^ original[i]) * 1099511628211ull;
    // These assets are repacked for the exact original glyph layouts. Unknown fonts keep their own pixels.
    const wchar_t* filename = nullptr;
    uint32_t shift = 0;
    if (key == 0xc64bc320db173a0cull) { filename = L"Big.png"; shift = 2; }
    if (key == 0xd9489cbe3cdb34f1ull) { filename = L"Small.png"; shift = 3; }
    if (!filename) return;
    uint32_t& data = cache[key];
    uint32_t w = d.width << shift, h = d.height << shift;
    if (!data) {
        std::vector<uint8_t> pixels;
        if (!ReadAtlas(filename, w, h, pixels)) {
            LOGR("fonts: HD atlas missing or invalid, keeping the original %ux%u font", d.width, d.height);
            return;
        }
        data = mem::AllocSystem(w * h * 4, 128);
        if (!data) return;
        uint32_t mx = 0, my = 0, bit = 1;
        for (uint32_t i = 1; i < std::max(w, h); i <<= 1) {
            if (i < w) { mx |= bit; bit <<= 1; }
            if (i < h) { my |= bit; bit <<= 1; }
        }
        std::vector<uint32_t> xs(w), ys(h);
        for (uint32_t x = 0; x < w; x++) xs[x] = Deposit(x, mx);
        for (uint32_t y = 0; y < h; y++) ys[y] = Deposit(y, my);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                std::memcpy(HostPtr(data + (xs[x] | ys[y]) * 4), &pixels[(size_t(y) * w + x) * 4], 4);
        LOGR("fonts: loaded HD atlas %ux%u -> %ux%u", d.width, d.height, w, h);
    }
    uint32_t format = R32(texture + 12);
    uint32_t sizes = (((format >> 20) & 15) + shift) | ((((format >> 24) & 15) + shift) << 4);
    W32(texture + 4, mem::PhysicalFromVirtual(data));
    W32(texture + 12, (format & ~0x0FF0FF00u) | (sizes << 20) | 0x600);  // swizzled A8R8G8B8
    W32(texture + 0x38, w);
    W32(texture + 0x3C, h);
}

}  // namespace

// The font resource callback runs for Big and Small before either can be drawn, including fonts reloaded later.
X86_EXTERN_FUNC(__imp__sub_001FF510);
X86_FUNC(sub_001FF510) {
    uint32_t font = R32(R32(ctx.esp + 4) + 0x118);
    __imp__sub_001FF510(ctx, base);
    if (font && R32(font + 4)) Upgrade(R32(font + 4));
}
