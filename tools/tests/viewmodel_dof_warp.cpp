// Production shader checks on offscreen D3D11 WARP only. No game, window, or hardware adapter.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include "d3d/viewmodel_dof_shader.h"

using Microsoft::WRL::ComPtr;
namespace {
constexpr unsigned W = 32, H = 32;
constexpr DXGI_FORMAT RGBA = DXGI_FORMAT_R32G32B32A32_FLOAT;
constexpr DXGI_FORMAT R = DXGI_FORMAT_R32_FLOAT;
struct Pixel { float r, g, b, a; };
void Check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
void Hr(HRESULT hr, const char* why) { Check(SUCCEEDED(hr), why); }
size_t At(unsigned x, unsigned y) { return size_t(y) * W + x; }
float Raw(float z) { return 1.001f - .01f / z; }
float Half(uint16_t bits) {
    unsigned e = (bits >> 10) & 31, f = bits & 1023;
    float v = e == 0 ? std::ldexp(float(f), -24)
            : e == 31 ? INFINITY : std::ldexp(float(1024 + f), int(e) - 25);
    return bits & 0x8000 ? -v : v;
}
ComPtr<ID3DBlob> Compile(const char* source, const char* entry, const char* profile, unsigned samples = 1) {
    std::string count = std::to_string(samples);
    D3D_SHADER_MACRO macros[] = {{"DOF_SAMPLES", count.c_str()}, {"MS", "1"}, {nullptr, nullptr}};
    if (samples == 1) macros[1] = {nullptr, nullptr};
    ComPtr<ID3DBlob> code, error;
    HRESULT hr = D3DCompile(source, std::strlen(source), "viewmodel_dof", macros, nullptr,
                            entry, profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &error);
    if (FAILED(hr)) throw std::runtime_error(error ? static_cast<const char*>(error->GetBufferPointer())
                                                 : "D3DCompile failed");
    return code;
}
struct Texture {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
};
struct Result { std::vector<Pixel> packed, original, output; std::vector<float> depth; };

class Warp {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> fixture, blur;
    std::map<unsigned, ComPtr<ID3D11PixelShader>> packs;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11BlendState> rgb;

    ComPtr<ID3D11PixelShader> Shader(const char* source, const char* entry, unsigned samples = 1) {
        auto code = Compile(source, entry, "ps_5_0", samples);
        ComPtr<ID3D11PixelShader> shader;
        Hr(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader),
           "CreatePixelShader");
        return shader;
    }
    Texture Make(DXGI_FORMAT format, unsigned samples = 1, const void* data = nullptr, unsigned stride = 0) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = W; d.Height = H; d.MipLevels = d.ArraySize = 1;
        d.Format = format; d.SampleDesc.Count = samples; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        D3D11_SUBRESOURCE_DATA init{data, W * stride, 0};
        Texture t;
        Hr(device->CreateTexture2D(&d, data ? &init : nullptr, &t.tex), "CreateTexture2D");
        Hr(device->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv), "CreateShaderResourceView");
        Hr(device->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv), "CreateRenderTargetView");
        return t;
    }
    void Draw(ID3D11PixelShader* shader, std::initializer_list<ID3D11RenderTargetView*> targets,
              std::initializer_list<ID3D11ShaderResourceView*> sources, unsigned mask = ~0u,
              bool rgb_only = false) {
        context->OMSetRenderTargets(UINT(targets.size()), targets.begin(), nullptr);
        context->OMSetBlendState(rgb_only ? rgb.Get() : nullptr, nullptr, mask);
        context->PSSetShaderResources(0, UINT(sources.size()), sources.begin());
        context->PSSetShader(shader, nullptr, 0);
        context->Draw(3, 0);
        ID3D11ShaderResourceView* empty[3]{};
        context->PSSetShaderResources(0, 3, empty);
        context->OMSetRenderTargets(0, nullptr, nullptr);
    }
    std::vector<float> Read(Texture& t, unsigned channels, bool half) {
        D3D11_TEXTURE2D_DESC d{}; t.tex->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> stage;
        Hr(device->CreateTexture2D(&d, nullptr, &stage), "Create staging");
        context->CopyResource(stage.Get(), t.tex.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        Hr(context->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m), "Map staging");
        std::vector<float> out(W * H * channels);
        for (unsigned y = 0; y < H; ++y) {
            auto row = static_cast<const unsigned char*>(m.pData) + y * m.RowPitch;
            for (unsigned x = 0; x < W * channels; ++x)
                out[y * W * channels + x] = half ? Half(reinterpret_cast<const uint16_t*>(row)[x])
                                                       : reinterpret_cast<const float*>(row)[x];
        }
        context->Unmap(stage.Get(), 0);
        return out;
    }
    std::vector<Pixel> ReadColor(Texture& t, bool half = false) {
        auto floats = Read(t, 4, half);
        std::vector<Pixel> result(W * H);
        std::memcpy(result.data(), floats.data(), floats.size() * sizeof(float));
        return result;
    }
public:
    Warp() {
        Hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_SINGLETHREADED,
                             nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context), "Create WARP device");
        auto code = Compile(gpu::kViewmodelDofHlsl, "vs", "vs_5_0");
        Hr(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs), "Create VS");
        blur = Shader(gpu::kViewmodelDofHlsl, "viewmodel_dof");
        fixture = Shader(R"(
            Texture2D<float> pre : register(t0);
            Texture2D<float> post : register(t1);
            Texture2D<float4> col : register(t2);
            struct O { float a : SV_Target0; float b : SV_Target1; float4 c : SV_Target2; };
            O main(float4 p : SV_Position) {
                O o; o.a = pre.Load(int3(p.xy,0)); o.b = post.Load(int3(p.xy,0));
                o.c = col.Load(int3(p.xy,0)); return o;
            })", "main");
        D3D11_BUFFER_DESC cb{}; cb.ByteWidth = 32; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Hr(device->CreateBuffer(&cb, nullptr, &constants), "Create constants");
        ID3D11Buffer* buffer = constants.Get(); context->PSSetConstantBuffers(1, 1, &buffer);
        D3D11_BLEND_DESC b{}; b.RenderTarget[0].RenderTargetWriteMask = 7;
        Hr(device->CreateBlendState(&b, &rgb), "Create RGB blend state");
        D3D11_VIEWPORT vp{0, 0, float(W), float(H), 0, 1};
        context->RSSetViewports(1, &vp);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }
    bool Supports(unsigned n) {
        for (auto format : {RGBA, R}) {
            UINT levels = 0;
            Hr(device->CheckMultisampleQualityLevels(format, n, &levels), "Query MSAA");
            if (!levels) return false;
        }
        return true;
    }
    void CompilePack(unsigned n) { if (!packs.count(n)) packs[n] = Shader(gpu::kViewmodelDofHlsl, "viewmodel_dof_pack", n); }

    // x=8 is a single covered sample; x=9..23 fully covered. The remaining samples are bright blue world.
    Result Run(unsigned samples, float amount, float z = .1f, bool invalid = false, bool cropped = false,
               bool constant_color = false, bool unchanged = false) {
        CompilePack(samples);
        Texture pre = Make(R, samples), post = Make(R, samples), color = Make(RGBA, samples);
        for (unsigned sample = 0; sample < samples; ++sample) {
            std::vector<float> before(W * H, Raw(5)), after = before;
            std::vector<Pixel> pixels(W * H);
            for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) {
                size_t i = At(x,y);
                pixels[i] = {0, .25f, 1, .2f + .05f * sample};
                if (x >= 8 && x < 24 && y >= 4 && y < 28 && (x > 8 || sample == 0)) {
                    after[i] = unchanged ? before[i] : Raw(z);
                    pixels[i] = {constant_color ? .5f : float((x + y) % 2), .125f, 0, pixels[i].a};
                }
            }
            if (invalid) {
                after[At(12, 12)] = std::numeric_limits<float>::quiet_NaN();
                after[At(13, 12)] = -1; after[At(14, 12)] = 1; after[At(15, 12)] = 2;
                before[At(16, 12)] = std::numeric_limits<float>::quiet_NaN();
                after[At(17, 12)] = before[At(17, 12)];
            }
            Texture a = Make(R, 1, before.data(), sizeof(float));
            Texture b = Make(R, 1, after.data(), sizeof(float));
            Texture c = Make(RGBA, 1, pixels.data(), sizeof(Pixel));
            // Actual OM masks force distinct samples, including WARP R32 targets that may coalesce shader invocations.
            Draw(fixture.Get(), {pre.rtv.Get(), post.rtv.Get(), color.rtv.Get()},
                 {a.srv.Get(), b.srv.Get(), c.srv.Get()}, 1u << sample);
        }
        Texture scene = Make(RGBA), output = Make(RGBA);
        if (samples > 1) context->ResolveSubresource(scene.tex.Get(), 0, color.tex.Get(), 0, RGBA);
        else context->CopyResource(scene.tex.Get(), color.tex.Get());
        context->CopyResource(output.tex.Get(), scene.tex.Get());
        Texture packed = Make(DXGI_FORMAT_R16G16B16A16_FLOAT), depth = Make(DXGI_FORMAT_R16_FLOAT);
        const float cb[8] = {-.01f, 1.001f, amount, 1, cropped ? 10.f : 0.f, 0, cropped ? 12.f : float(W), float(H)};
        context->UpdateSubresource(constants.Get(), 0, nullptr, cb, 0, 0);
        Draw(packs.at(samples).Get(), {packed.rtv.Get(), depth.rtv.Get()},
             {post.srv.Get(), pre.srv.Get(), color.srv.Get()});
        Draw(blur.Get(), {output.rtv.Get()}, {packed.srv.Get(), depth.srv.Get(), scene.srv.Get()}, ~0u, true);
        return {ReadColor(packed, true), ReadColor(scene), ReadColor(output), Read(depth, 1, true)};
    }
};
void Invariants(const Result& r) {
    for (size_t i = 0; i < r.output.size(); ++i) {
        auto a = r.output[i], b = r.original[i];
        Check(a.a == b.a, "destination alpha changed");
        Check(std::isfinite(a.r) && std::isfinite(a.g) && std::isfinite(a.b), "nonfinite output");
        Check(a.r >= -.001f && a.r <= 1.001f && a.b >= -.001f && a.b <= 1.001f, "halo/range violation");
        if (r.packed[i].a == 0) Check(std::memcmp(&a, &b, sizeof(Pixel)) == 0, "world/invalid pixel changed");
        if (r.packed[i].a > 0) Check(std::abs(a.b - b.b) < .00001f, "world blue contaminated gun blur");
    }
}
float Detail(const Result& r) {
    float contrast = 0;
    for (unsigned y = 10; y < 22; ++y) for (unsigned x = 12; x < 20; ++x)
        contrast += std::abs(r.output[At(x,y)].r - .5f);
    return contrast;
}
} // namespace

int main() {
    try {
        Warp warp;
        for (unsigned n : {1u, 2u, 4u, 8u}) {
            warp.CompilePack(n); // Compile all production variants even if this WARP adapter cannot execute one.
            if (!warp.Supports(n)) { std::printf("SKIP runtime %ux unsupported by WARP\n", n); continue; }
            auto zero = warp.Run(n, 0);
            Check(std::memcmp(zero.output.data(), zero.original.data(), W * H * sizeof(Pixel)) == 0,
                  "zero amount is not exact identity");
            auto normal = warp.Run(n, .25f); Invariants(normal);
            Check(normal.packed[At(8,16)].a == 1.f / n, "incorrect single-sample gun coverage");
            Check(normal.packed[At(9,16)].a == 1, "full gun coverage missing");
            Check(normal.depth[At(9,16)] > .099f && normal.depth[At(9,16)] < .101f, "wrong packed depth");
            Check(normal.packed[At(8,16)].b == 0, "raw MSAA world leaked into packed gun");
            Check(normal.output[At(8,16)].r >= 0 && normal.output[At(8,16)].r <= 1.f / n,
                  "partial coverage was not respected by delta composition");
            Check(Detail(normal) < Detail(zero) * .85f, "default amount did not soften gun texture");
            auto farther = warp.Run(n, .25f, 1.5f); Invariants(farther);
            Check(Detail(normal) < Detail(farther), "near hand is not softer than farther gun");
            auto maximum = warp.Run(n, 1); Invariants(maximum);
            auto flat = warp.Run(n, 1, .1f, false, false, true); Invariants(flat);
            for (size_t i = 0; i < flat.output.size(); ++i)
                Check(std::abs(flat.output[i].r - flat.original[i].r) < .00001f, "constant gun edge developed halo");
            auto bad = warp.Run(n, 1, .1f, true); Invariants(bad);
            for (unsigned x = 12; x <= 17; ++x) Check(bad.packed[At(x,12)].a == 0, "invalid/unchanged depth accepted");
            auto crop = warp.Run(n, 1, .1f, false, true); Invariants(crop);
            Check(crop.packed[At(9,16)].a == 0 && crop.packed[At(22,16)].a == 0, "viewport exterior packed");
            auto unchanged = warp.Run(n, 1, .1f, false, false, false, true); Invariants(unchanged);
            for (const auto& p : unchanged.packed) Check(p.a == 0, "unchanged world classified as gun");
            std::printf("PASS viewmodel DOF %ux: identity, alpha, coverage, guarded blur, depth falloff, invalids, viewport\n", n);
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL viewmodel DOF: %s\n", error.what()); return 1;
    }
}
