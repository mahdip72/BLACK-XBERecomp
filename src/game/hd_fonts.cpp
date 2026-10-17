// Both shared game fonts, including their accented letters and button glyphs: replace only their texels.
#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <mutex>
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

constexpr uint32_t kScale = 4;

// WIC reads the game's DXT3 blocks as DDS. Filter premultiplied colour so transparent edges do not turn dark.
bool Enlarge(const gpu::TextureDesc& d, std::vector<uint8_t>& pixels) {
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct ComScope {
        bool initialized;
        ~ComScope() { if (initialized) CoUninitialize(); }
    } scope{SUCCEEDED(com)};
    uint32_t size = gpu::TotalSize(d);
    std::array<uint32_t, 32> header{};
    header[0] = 0x20534444;  // DDS
    header[1] = 124;
    header[2] = 0x81007;
    header[3] = d.height;
    header[4] = d.width;
    header[5] = size;
    header[19] = 32;
    header[20] = 4;
    header[21] = 0x33545844;  // DXT3
    header[27] = 0x1000;
    std::vector<uint8_t> dds(sizeof(header) + size);
    std::memcpy(dds.data(), header.data(), sizeof(header));
    std::memcpy(dds.data() + sizeof(header), HostPtr(d.data), size);
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> premultiplied, straight;
    ComPtr<IWICBitmapScaler> scaler;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (SUCCEEDED(hr)) hr = wic->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromMemory(dds.data(), DWORD(dds.size()));
    if (SUCCEEDED(hr)) hr = wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&premultiplied);
    if (SUCCEEDED(hr)) hr = premultiplied->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) hr = wic->CreateBitmapScaler(&scaler);
    uint32_t w = d.width * kScale, h = d.height * kScale;
    if (SUCCEEDED(hr)) hr = scaler->Initialize(premultiplied.Get(), w, h, WICBitmapInterpolationModeCubic);
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&straight);
    if (SUCCEEDED(hr)) hr = straight->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    pixels.resize(size_t(w) * h * 4);
    if (SUCCEEDED(hr)) hr = straight->CopyPixels(nullptr, w * 4, UINT(pixels.size()), pixels.data());
    if (FAILED(hr)) return false;
    // The game blends straight alpha. Extend edge colours into transparent texels for its bilinear sampler.
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t* p = &pixels[(size_t(y) * w + x) * 4];
            if (p[3]) continue;
            const uint8_t* edge = p;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    uint32_t xx = uint32_t(std::clamp(int(x) + dx, 0, int(w) - 1));
                    uint32_t yy = uint32_t(std::clamp(int(y) + dy, 0, int(h) - 1));
                    const uint8_t* q = &pixels[(size_t(yy) * w + xx) * 4];
                    if (q[3] > edge[3]) edge = q;
                }
            if (edge != p) std::memcpy(p, edge, 3);
        }
    return true;
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
    uint32_t& data = cache[key];
    uint32_t w = d.width * kScale, h = d.height * kScale;
    if (!data) {
        std::vector<uint8_t> pixels;
        if (!Enlarge(d, pixels)) {
            LOGR("fonts: reconstruction failed, keeping the original %ux%u font", d.width, d.height);
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
        LOGR("fonts: reconstructed %ux%u -> %ux%u", d.width, d.height, w, h);
    }
    uint32_t format = R32(texture + 12);
    uint32_t sizes = (((format >> 20) & 15) + 2) | ((((format >> 24) & 15) + 2) << 4);
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
