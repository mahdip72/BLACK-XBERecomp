// Default: offscreen D3D11 WARP checks. Explicit --hardware --benchmark enables
// timestamp measurements on the default hardware adapter; never creates a window or starts a game.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d11_3.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
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
#include "d3d/weapon_dof_region.h"

using Microsoft::WRL::ComPtr;
namespace {
// Deliberately not multiples of the production tile size.
constexpr unsigned W = 35, H = 33;
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
    ComPtr<ID3D11VertexShader> tile_vs;
    ComPtr<ID3D11VertexShader> geometry_vs;
    ComPtr<ID3D11InputLayout> geometry_layout;
    ComPtr<ID3D11PixelShader> coverage_ps;
    ComPtr<ID3D11RasterizerState2> conservative;
    ComPtr<ID3D11PixelShader> fixture, blur;
    std::map<unsigned, ComPtr<ID3D11PixelShader>> packs;
    std::map<unsigned, ComPtr<ID3D11PixelShader>> tile_masks;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11BlendState> rgb;
    unsigned draw_width = W, draw_height = H;
    std::map<unsigned, ComPtr<ID3D11PixelShader>> sample_readers;

    ComPtr<ID3D11PixelShader> Shader(const char* source, const char* entry, unsigned samples = 1) {
        auto code = Compile(source, entry, "ps_5_0", samples);
        ComPtr<ID3D11PixelShader> shader;
        Hr(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader),
           "CreatePixelShader");
        return shader;
    }
    Texture Make(DXGI_FORMAT format, unsigned samples = 1, const void* data = nullptr, unsigned stride = 0,
                 unsigned width = W, unsigned height = H) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = width; d.Height = height; d.MipLevels = d.ArraySize = 1;
        d.Format = format; d.SampleDesc.Count = samples; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        D3D11_SUBRESOURCE_DATA init{data, width * stride, 0};
        Texture t;
        Hr(device->CreateTexture2D(&d, data ? &init : nullptr, &t.tex), "CreateTexture2D");
        Hr(device->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv), "CreateShaderResourceView");
        Hr(device->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv), "CreateRenderTargetView");
        return t;
    }
    void Draw(ID3D11PixelShader* shader, std::initializer_list<ID3D11RenderTargetView*> targets,
              std::initializer_list<ID3D11ShaderResourceView*> sources, unsigned mask = ~0u,
              bool rgb_only = false, ID3D11ShaderResourceView* tiles = nullptr) {
        context->OMSetRenderTargets(UINT(targets.size()), targets.begin(), nullptr);
        context->OMSetBlendState(rgb_only ? rgb.Get() : nullptr, nullptr, mask);
        context->PSSetShaderResources(0, UINT(sources.size()), sources.begin());
        context->PSSetShader(shader, nullptr, 0);
        context->VSSetShader(tiles ? tile_vs.Get() : vs.Get(), nullptr, 0);
        if (tiles) {
            context->VSSetShaderResources(3, 1, &tiles);
            const unsigned tile_count = ((draw_width + 7) / 8) * ((draw_height + 7) / 8);
            context->DrawInstanced(6, tile_count, 0, 0);
            ID3D11ShaderResourceView* empty_tile = nullptr;
            context->VSSetShaderResources(3, 1, &empty_tile);
        } else context->Draw(3, 0);
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
        std::vector<float> out(d.Width * d.Height * channels);
        for (unsigned y = 0; y < d.Height; ++y) {
            auto row = static_cast<const unsigned char*>(m.pData) + y * m.RowPitch;
            for (unsigned x = 0; x < d.Width * channels; ++x)
                out[y * d.Width * channels + x] = half ? Half(reinterpret_cast<const uint16_t*>(row)[x])
                                                       : reinterpret_cast<const float*>(row)[x];
        }
        context->Unmap(stage.Get(), 0);
        return out;
    }
    std::vector<Pixel> ReadColor(Texture& t, bool half = false) {
        auto floats = Read(t, 4, half);
        std::vector<Pixel> result(floats.size() / 4);
        std::memcpy(result.data(), floats.data(), floats.size() * sizeof(float));
        return result;
    }
    std::vector<Pixel> ReadSamples(Texture& t, unsigned samples) {
        D3D11_TEXTURE2D_DESC desc{}; t.tex->GetDesc(&desc);
        Texture sample_target = Make(RGBA, 1, nullptr, 0, desc.Width, desc.Height);
        UINT viewport_count = 1;
        D3D11_VIEWPORT saved_viewport{};
        context->RSGetViewports(&viewport_count, &saved_viewport);
        const D3D11_VIEWPORT sample_viewport{0, 0, float(desc.Width), float(desc.Height), 0, 1};
        context->RSSetViewports(1, &sample_viewport);
        std::vector<Pixel> result;
        for (unsigned s = 0; s < samples; ++s) {
            if (!sample_readers.count(s)) {
                const std::string source = "Texture2DMS<float4> tex : register(t0);"
                    "float4 main(float4 p : SV_Position) : SV_Target { return tex.Load(int2(p.xy), " +
                    std::to_string(s) + "); }";
                sample_readers[s] = Shader(source.c_str(), "main");
            }
            Draw(sample_readers.at(s).Get(), {sample_target.rtv.Get()}, {t.srv.Get()});
            const auto pixels = ReadColor(sample_target);
            result.insert(result.end(), pixels.begin(), pixels.end());
        }
        context->RSSetViewports(viewport_count, &saved_viewport);
        return result;
    }
    void CheckLaterBlending(Texture& reference, Texture& optimized, Texture& rejected_sparse,
                            Texture& post_depth, unsigned samples) {
        // Populate an actual per-sample DSV from the same depth as the weapon
        // pass. Later geometry at z=1 passes world samples and fails gun samples.
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = W; desc.Height = H; desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_D32_FLOAT; desc.SampleDesc.Count = samples;
        desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ComPtr<ID3D11Texture2D> depth;
        ComPtr<ID3D11DepthStencilView> dsv;
        Hr(device->CreateTexture2D(&desc, nullptr, &depth), "Create later-blend depth");
        Hr(device->CreateDepthStencilView(depth.Get(), nullptr, &dsv), "Create later-blend DSV");
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        ComPtr<ID3D11DepthStencilState> fill_state, test_state;
        Hr(device->CreateDepthStencilState(&dd, &fill_state), "Create depth-fill state");
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; dd.DepthFunc = D3D11_COMPARISON_LESS;
        Hr(device->CreateDepthStencilState(&dd, &test_state), "Create later-depth state");
        auto fill = Shader(R"(
            Texture2DMS<float> source : register(t0);
            float main(float4 p : SV_Position, uint s : SV_SampleIndex) : SV_Depth {
                return source.Load(int2(p.xy), s);
            })", "main");
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->OMSetRenderTargets(0, nullptr, dsv.Get());
        context->OMSetDepthStencilState(fill_state.Get(), 0);
        context->OMSetBlendState(nullptr, nullptr, ~0u);
        ID3D11ShaderResourceView* depth_source = post_depth.srv.Get();
        context->PSSetShaderResources(0, 1, &depth_source);
        context->PSSetShader(fill.Get(), nullptr, 0);
        context->Draw(3, 0);
        depth_source = nullptr;
        context->PSSetShaderResources(0, 1, &depth_source);
        auto overlay = Shader(R"(
            struct Out { float4 color : SV_Target; float depth : SV_Depth; };
            Out main(float4 p : SV_Position) {
                // A byte-style alpha test rejects a spatial subset.
                float alpha = ((int(p.x) + int(p.y)) % 4 == 0) ? 0 : 0.4;
                if (round(alpha * 255) <= 0) discard;
                Out o; o.color = float4(0.8, 0.25, 0.1, alpha); o.depth = 1.001 - .01; return o;
            })", "main");
        const auto before = ReadSamples(reference, samples);
        for (bool additive : {false, true}) {
            D3D11_BLEND_DESC bd{};
            auto& target_blend = bd.RenderTarget[0];
            target_blend.BlendEnable = TRUE;
            target_blend.SrcBlend = target_blend.SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
            target_blend.DestBlend = target_blend.DestBlendAlpha = additive ? D3D11_BLEND_ONE : D3D11_BLEND_INV_SRC_ALPHA;
            target_blend.BlendOp = target_blend.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            target_blend.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            ComPtr<ID3D11BlendState> blend;
            Hr(device->CreateBlendState(&bd, &blend), "Create later blend state");
            for (Texture* target : {&reference, &optimized, &rejected_sparse}) {
                ID3D11RenderTargetView* rtv = target->rtv.Get();
                context->OMSetRenderTargets(1, &rtv, dsv.Get());
                context->OMSetDepthStencilState(test_state.Get(), 0);
                context->OMSetBlendState(blend.Get(), nullptr, additive ? 0xaaaaaaaau : 0x55555555u);
                context->VSSetShader(vs.Get(), nullptr, 0);
                context->PSSetShader(overlay.Get(), nullptr, 0);
                context->Draw(3, 0);
            }
            context->OMSetRenderTargets(0, nullptr, nullptr);
            context->OMSetDepthStencilState(nullptr, 0);
            const auto a = ReadSamples(reference, samples), b = ReadSamples(optimized, samples);
            Check(std::memcmp(a.data(), b.data(), a.size() * sizeof(Pixel)) == 0,
                  "raw MSAA samples differ after later alpha/additive depth-tested blend");
            Check(std::memcmp(a.data(), before.data(), a.size() * sizeof(Pixel)) != 0,
                  "later blending fixture changed no samples");
            auto unchanged_at = [&](unsigned x, unsigned y, unsigned s) {
                const size_t i = size_t(s) * W * H + At(x, y);
                return std::memcmp(&a[i], &before[i], sizeof(Pixel)) == 0;
            };
            Check(unchanged_at(9, 16, 0) && unchanged_at(8, 15, 0),
                  "later fixture did not reject covered gun depth samples");
            Check(unchanged_at(0, 0, 0), "later fixture did not reject zero alpha");
            Check(!unchanged_at(1, 0, additive ? 1u : 0u),
                  "later fixture did not apply its covered world sample");
            if (additive) Check(!unchanged_at(8, 15, 1),
                                "later fixture did not blend world behind a partial weapon sample");
        }
        Texture a = Make(RGBA), b = Make(RGBA);
        context->ResolveSubresource(a.tex.Get(), 0, reference.tex.Get(), 0, RGBA);
        context->ResolveSubresource(b.tex.Get(), 0, rejected_sparse.tex.Get(), 0, RGBA);
        const auto correct = ReadColor(a), incorrect = ReadColor(b);
        Check(std::memcmp(correct.data(), incorrect.data(), correct.size() * sizeof(Pixel)) != 0,
              "negative control failed to expose later-blend effect of sparse MSAA composition");
    }
public:
    explicit Warp(bool hardware = false) {
        // A null list requests only the legacy default feature levels. Request
        // the highest supported level so optional raster capabilities are visible.
        constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                                D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL selected_level{};
        HRESULT create_hr = E_FAIL;
        for (unsigned first = 0; first < std::size(levels); ++first) {
            create_hr = D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP,
                                        nullptr, D3D11_CREATE_DEVICE_SINGLETHREADED,
                                        levels + first, UINT(std::size(levels) - first), D3D11_SDK_VERSION,
                                        device.ReleaseAndGetAddressOf(), &selected_level,
                                        context.ReleaseAndGetAddressOf());
            std::printf("D3D11CreateDevice highest requested=0x%04x HRESULT=0x%08x selected=0x%04x\n",
                        unsigned(levels[first]), unsigned(create_hr), unsigned(selected_level));
            // Older runtimes reject a list containing an unknown feature level.
            // Other failures are real device errors, not capability fallbacks.
            if (create_hr != E_INVALIDARG) break;
        }
        Hr(create_hr, "Create offscreen device");
        if (hardware) {
            ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter; DXGI_ADAPTER_DESC desc{};
            Hr(device.As(&dxgi), "Get DXGI device"); Hr(dxgi->GetAdapter(&adapter), "Get adapter");
            Hr(adapter->GetDesc(&desc), "Get adapter description");
            std::printf("Hardware adapter: %ls (vendor %04x device %04x), offscreen D3D11\n",
                        desc.Description, desc.VendorId, desc.DeviceId);
        }
        auto code = Compile(gpu::kViewmodelDofHlsl, "vs", "vs_5_0");
        Hr(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs), "Create VS");
        code = Compile(gpu::kViewmodelDofHlsl, "viewmodel_dof_tiles_vs", "vs_5_0");
        Hr(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &tile_vs), "Create tile VS");
        code = Compile("float4 main(float4 p : POSITION) : SV_Position { return p; }", "main", "vs_5_0");
        Hr(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &geometry_vs), "Create geometry VS");
        const D3D11_INPUT_ELEMENT_DESC position{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
        Hr(device->CreateInputLayout(&position, 1, code->GetBufferPointer(), code->GetBufferSize(), &geometry_layout), "Create geometry layout");
        coverage_ps = Shader(gpu::kViewmodelDofHlsl, "viewmodel_dof_coverage");
        D3D11_FEATURE_DATA_D3D11_OPTIONS2 options{};
        ComPtr<ID3D11Device3> device3;
        const HRESULT options_hr = device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &options, sizeof(options));
        const HRESULT device3_hr = device.As(&device3);
        std::printf("D3D11 OPTIONS2 HRESULT=0x%08x; ID3D11Device3 HRESULT=0x%08x; featureLevel=0x%04x\n",
                    unsigned(options_hr), unsigned(device3_hr), unsigned(device->GetFeatureLevel()));
        if (SUCCEEDED(options_hr) && SUCCEEDED(device3_hr) &&
            options.ConservativeRasterizationTier >= D3D11_CONSERVATIVE_RASTERIZATION_TIER_2) {
            D3D11_RASTERIZER_DESC2 raster{};
            raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE;
            raster.ScissorEnable = TRUE; raster.DepthClipEnable = FALSE;
            raster.ConservativeRaster = D3D11_CONSERVATIVE_RASTERIZATION_MODE_ON;
            Hr(device3->CreateRasterizerState2(&raster, &conservative), "Create conservative rasterizer");
        }
        std::printf("Conservative raster tier %u; geometry tile path %s\n",
                    unsigned(options.ConservativeRasterizationTier), conservative ? "available" : "unavailable (full-pack fallback)");
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
        D3D11_BUFFER_DESC cb{}; cb.ByteWidth = 64; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Hr(device->CreateBuffer(&cb, nullptr, &constants), "Create constants");
        ID3D11Buffer* buffer = constants.Get(); context->PSSetConstantBuffers(1, 1, &buffer);
        context->VSSetConstantBuffers(1, 1, &buffer);
        D3D11_BLEND_DESC b{}; b.RenderTarget[0].RenderTargetWriteMask = 7;
        Hr(device->CreateBlendState(&b, &rgb), "Create RGB blend state");
        D3D11_VIEWPORT vp{0, 0, float(W), float(H), 0, 1};
        context->RSSetViewports(1, &vp);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }
    bool Supports(unsigned n) {
        for (auto format : {RGBA, R, DXGI_FORMAT_D32_FLOAT}) {
            UINT levels = 0;
            Hr(device->CheckMultisampleQualityLevels(format, n, &levels), "Query MSAA");
            if (!levels) return false;
        }
        return true;
    }
    void CompilePack(unsigned n) {
        if (!packs.count(n)) packs[n] = Shader(gpu::kViewmodelDofHlsl, "viewmodel_dof_pack", n);
        if (!tile_masks.count(n)) tile_masks[n] = Shader(gpu::kViewmodelDofHlsl, "viewmodel_dof_tiles", n);
    }

    ComPtr<ID3D11Buffer> Geometry(const std::vector<std::array<float, 4>>& vertices) {
        D3D11_BUFFER_DESC desc{}; desc.ByteWidth = UINT(vertices.size() * sizeof(vertices[0]));
        desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA data{vertices.data(), 0, 0};
        ComPtr<ID3D11Buffer> buffer;
        Hr(device->CreateBuffer(&desc, &data, &buffer), "Create geometry vertices");
        return buffer;
    }
    void BindGeometry(ID3D11Buffer* buffer) {
        const UINT stride = 16, offset = 0;
        context->IASetInputLayout(geometry_layout.Get());
        context->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(geometry_vs.Get(), nullptr, 0);
    }
    void GeometryCoverage() {
        gpu::WeaponDofDepthEncoding encoding;
        encoding.Begin(10); Check(!encoding.Complete(10), "missing encoding did not retain native rendering");
        encoding.Observe(10, true, true, .08f);
        Check(encoding.Complete(11) && encoding.plane == .08f, "near encoding was not captured without ROI");
        encoding.Observe(11, true, true, .08f); Check(encoding.Complete(12), "consistent near encoding rejected");
        encoding.Observe(12, true, false, 0); Check(!encoding.Complete(13), "mixed near/legacy encoding accepted");
        encoding.Begin(20); encoding.Observe(20, true, false, 0); Check(encoding.Complete(21), "legacy encoding rejected");
        encoding.Observe(21, true, true, .08f); Check(!encoding.Complete(22), "mixed legacy/near encoding accepted");
        encoding.Begin(30); encoding.Observe(30, true, true, .08f); encoding.Observe(31, true, true, .1f);
        Check(!encoding.Complete(32), "mixed near planes accepted");
        encoding.Begin(40); encoding.Observe(40, false, true, .08f); Check(!encoding.Complete(41), "unmatched draw accepted");
        encoding.Begin(50); encoding.Observe(51, true, true, .08f); Check(!encoding.Complete(52), "untracked depth mutation accepted");
        for (float plane : {0.f, -1.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
            encoding.Begin(60); encoding.Observe(60, true, true, plane);
            Check(!encoding.Complete(61), "invalid near plane accepted");
        }
        // This is the production serial state machine, including the fallback
        // when any copy/clear/raw-list mutation was not a replayed native draw.
        gpu::WeaponDofRegion region;
        region.Begin(10, true); Check(region.AcceptDraw(10, true) && region.Complete(11), "tracked draw did not retain coverage");
        Check(!region.Complete(12), "depth clear/copy did not force full-pack fallback");
        Check(!region.AcceptDraw(12, true) && !region.Complete(13), "later draw incorrectly repaired an invalid mask");
        region.Begin(20, true); Check(!region.AcceptDraw(20, false) && !region.Complete(21), "unsupported draw did not force fallback");
        region.Begin(30, false); Check(!region.Complete(30), "unsupported hardware did not force fallback");
        region.Begin(40, true); region.Invalidate(); Check(!region.Complete(40), "unknown mutation did not force fallback");
        Check(gpu::WeaponDofViewportSupported(0, 0, 1280, 720) &&
              gpu::WeaponDofViewportSupported(8, 16, 32, 24), "whole-tile viewport rejected");
        for (const auto& viewport : std::array<std::array<float, 4>, 7>{{
                 {0, 0, float(W), float(H)}, {.375f, .625f, W - 1.25f, H - 1.5f},
                 {1, 0, 32, 32}, {0, 1, 32, 32}, {0, 0, 0, 32},
                 {0, 0, std::numeric_limits<float>::infinity(), 32},
                 {0, 0, 32, std::numeric_limits<float>::quiet_NaN()}}}) {
            region.Begin(50, gpu::WeaponDofViewportSupported(viewport[0], viewport[1], viewport[2], viewport[3]));
            Check(!region.Complete(50) && !region.AcceptDraw(50, true), "unsupported viewport did not retain full fallback");
        }
        if (!conservative) { std::printf("SKIP conservative geometry GPU fixture; serial fallback checks passed\n"); return; }
        std::vector<std::array<float, 4>> vertices;
        auto add = [&](float x, float y, float z = .5f) { vertices.push_back({2 * x / W - 1, 1 - 2 * y / H, z, 1}); };
        add(4, 4); add(19, 4); add(4, 25); add(19, 4); add(19, 25); add(4, 25);
        // At the standard 8x sample (31+.0625, 7+.4375), all three vertices
        // quantize to one point at tile resolution. Tier 1 may drop this.
        add(31.0565f, 7.4315f); add(31.0745f, 7.4315f); add(31.0565f, 7.4495f);
        add(7.99f, 1); add(8.10f, 31); add(7.99f, 31);
        add(-2, 15, -.1f); add(10, 16, .7f); add(0, 27, .7f);
        add(W - .95f, H - .95f); add(W + 1, H - .95f); add(W - .95f, H + 1);
        // Retain primitive identity in the native target so a failed superset
        // check identifies the actual last writer, not a CPU coverage estimate.
        auto diagnostic_ps = Shader("float4 main(uint primitive_id : SV_PrimitiveID) : SV_Target { return float4(1, primitive_id + 1, 0, 1); }", "main");
        const char* triangle_names[] = {"rectangle-a", "rectangle-b", "post-snap-tiny", "skinny-tile-edge", "near-clipped", "odd-bottom-right"};
        const char* case_names[] = {"full-odd-extent-fallback", "fractional-viewport-fallback", "aligned-one-pixel-scissor", "aligned-full", "aligned-offset", "eligible-target32", "eligible-target32-one-pixel-scissor"};
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE; raster.ScissorEnable = TRUE; raster.MultisampleEnable = TRUE;
        ComPtr<ID3D11RasterizerState> native;
        Hr(device->CreateRasterizerState(&raster, &native), "Create native coverage rasterizer");
        for (unsigned samples : {1u, 2u, 4u, 8u}) {
            if (!Supports(samples)) continue;
            for (unsigned case_index = 0; case_index < 7; ++case_index) {
                const unsigned target_width = case_index >= 5 ? 32 : W, target_height = case_index >= 5 ? 32 : H;
                const unsigned columns = (target_width + 7) / 8, rows = (target_height + 7) / 8;
                Texture actual = Make(RGBA, samples, nullptr, 0, target_width, target_height);
                Texture mask = Make(R, 1, nullptr, 0, columns, rows);
                D3D11_VIEWPORT viewport = case_index == 1 ? D3D11_VIEWPORT{.375f, .625f, W - 1.25f, H - 1.5f, 0, 1}
                                                         : D3D11_VIEWPORT{0, 0, float(W), float(H), 0, 1};
                if (case_index >= 2) viewport = case_index == 4 ? D3D11_VIEWPORT{8, 8, 24, 24, 0, 1}
                                                                               : D3D11_VIEWPORT{0, 0, 32, 32, 0, 1};
                D3D11_RECT scissor = case_index == 2 || case_index == 6 ? D3D11_RECT{8, 0, 9, LONG(target_height)}
                                                               : D3D11_RECT{0, 0, LONG(target_width), LONG(target_height)};
                const auto native_viewport = viewport;
                const auto native_scissor = scissor;
                const bool eligible = gpu::WeaponDofViewportSupported(viewport.TopLeftX, viewport.TopLeftY, viewport.Width, viewport.Height);
                region.Begin(60, eligible);
                Check(eligible == (case_index >= 2), "viewport policy disagrees with fixture eligibility");
                if (!eligible) {
                    Check(!region.Complete(60), "unsupported viewport enabled geometry mask");
                    std::printf("PASS full-pack fallback selected: samples=%u case=%s\n", samples, case_names[case_index]);
                    continue; // Full pack/compose numerical parity is checked in Run below.
                }
                // Preserve intended screen-space positions (especially the
                // exact 8x sample) when testing a different supported viewport.
                auto case_vertices = vertices;
                for (auto& p : case_vertices) {
                    const float px = (p[0] + 1) * .5f * W, py = (1 - p[1]) * .5f * H;
                    p[0] = 2 * (px - viewport.TopLeftX) / viewport.Width - 1;
                    p[1] = 1 - 2 * (py - viewport.TopLeftY) / viewport.Height;
                }
                if (case_index >= 5) {
                    // Exercise the actual eligible target's bottom-right tile,
                    // including clipping beyond both physical target edges.
                    const std::array<std::array<float, 2>, 3> edge{{{31.05f, 31.05f}, {33, 31.05f}, {31.05f, 33}}};
                    for (unsigned corner = 0; corner < 3; ++corner)
                        case_vertices[15 + corner] = {2 * edge[corner][0] / 32 - 1, 1 - 2 * edge[corner][1] / 32, .5f, 1};
                }
                auto buffer = Geometry(case_vertices);
                const float zero[4]{};
                context->ClearRenderTargetView(actual.rtv.Get(), zero); context->ClearRenderTargetView(mask.rtv.Get(), zero);
                BindGeometry(buffer.Get());
                context->OMSetBlendState(nullptr, nullptr, ~0u);
                context->RSSetState(native.Get()); context->RSSetViewports(1, &viewport); context->RSSetScissorRects(1, &scissor);
                ID3D11RenderTargetView* target = actual.rtv.Get(); context->OMSetRenderTargets(1, &target, nullptr);
                context->PSSetShader(diagnostic_ps.Get(), nullptr, 0); context->Draw(UINT(vertices.size()), 0);
                viewport.TopLeftX /= 8; viewport.TopLeftY /= 8; viewport.Width /= 8; viewport.Height /= 8;
                scissor = {scissor.left / 8, scissor.top / 8, (scissor.right + 7) / 8, (scissor.bottom + 7) / 8};
                context->RSSetState(conservative.Get()); context->RSSetViewports(1, &viewport); context->RSSetScissorRects(1, &scissor);
                target = mask.rtv.Get(); context->OMSetRenderTargets(1, &target, nullptr);
                context->PSSetShader(coverage_ps.Get(), nullptr, 0);
                context->Draw(UINT(vertices.size()), 0);
                context->OMSetRenderTargets(0, nullptr, nullptr); context->RSSetState(nullptr); context->IASetInputLayout(nullptr);
                const D3D11_VIEWPORT full{0, 0, float(W), float(H), 0, 1}; context->RSSetViewports(1, &full);
                const auto tiles = Read(mask, 1, false);
                const auto pixels = samples > 1 ? ReadSamples(actual, samples) : ReadColor(actual);
                unsigned covered = 0, tiny = 0, edge_covered = 0, missed = 0;
                for (size_t i = 0; i < pixels.size(); ++i) if (pixels[i].r > 0) {
                    const unsigned plane = target_width * target_height;
                    const unsigned x = unsigned(i % plane) % target_width, y = unsigned(i % plane) / target_width;
                    ++covered; if (x == 31 && y == 7) ++tiny;
                    if (x == 31 && y == 31) ++edge_covered;
                    const float tile = tiles[(y / 8) * columns + x / 8];
                    if (tile != 1) {
                        const unsigned triangle = unsigned(pixels[i].g) - 1;
                        if (missed++ < 32) {
                            std::printf("COVERAGE MISS case=%u(%s) samples=%u pixel=(%u,%u) sample=%u tile=(%u,%u) mask=%.9g triangle=%u(%s) nativeRGBA=(%.9g,%.9g,%.9g,%.9g)\n",
                                case_index, case_names[case_index], samples, x, y, unsigned(i / plane), x / 8, y / 8,
                                tile, triangle, triangle < std::size(triangle_names) ? triangle_names[triangle] : "INVALID",
                                pixels[i].r, pixels[i].g, pixels[i].b, pixels[i].a);
                            if (triangle < vertices.size() / 3) for (unsigned corner = 0; corner < 3; ++corner) {
                                const auto& p = case_vertices[triangle * 3 + corner];
                                const float px = native_viewport.TopLeftX + (p[0] / p[3] + 1) * .5f * native_viewport.Width;
                                const float py = native_viewport.TopLeftY + (1 - p[1] / p[3]) * .5f * native_viewport.Height;
                                std::printf("  vertex%u clip=(%.9g,%.9g,%.9g,%.9g) nativeXY=(%.9g,%.9g) tileXY=(%.9g,%.9g)\n",
                                            corner, p[0], p[1], p[2], p[3], px, py, px / 8, py / 8);
                            }
                        }
                    }
                }
                if (missed) {
                    std::printf("COVERAGE FAILURE misses=%u covered=%u nativeViewport=(%.9g,%.9g,%.9g,%.9g) maskViewport=(%.9g,%.9g,%.9g,%.9g) nativeScissor=(%ld,%ld,%ld,%ld) maskScissor=(%ld,%ld,%ld,%ld)\n",
                        missed, covered, native_viewport.TopLeftX, native_viewport.TopLeftY, native_viewport.Width, native_viewport.Height,
                        viewport.TopLeftX, viewport.TopLeftY, viewport.Width, viewport.Height,
                        native_scissor.left, native_scissor.top, native_scissor.right, native_scissor.bottom,
                        scissor.left, scissor.top, scissor.right, scissor.bottom);
                    for (unsigned y = 0; y < rows; ++y) {
                        std::printf("  mask row %u:", y);
                        for (unsigned x = 0; x < columns; ++x) std::printf(" %.3g", tiles[y * columns + x]);
                        std::printf("\n");
                    }
                    std::fflush(stdout);
                }
                Check(missed == 0, "geometry mask missed a native covered sample");
                Check(covered > 0, "geometry fixture has no native coverage");
                if (case_index == 5) Check(edge_covered > 0, "eligible target edge fixture has no native coverage");
                if (samples == 8 && (case_index == 3 || case_index == 5)) Check(tiny > 0, "post-snap degenerate fixture did not hit its native sample");
            }
        }
        std::printf("PASS geometry mask: supported whole-tile viewport native sample superset, 8x post-snap degenerate, skinny/tile-edge triangles, aligned offset, 1px scissor; odd/fractional viewport and serial fallbacks\n");
    }

    void Benchmark() {
        if (!conservative) { std::printf("SKIP geometry-mask benchmark: Tier 2 unavailable; production retains full pack\n"); return; }
        std::printf("Benchmark: old full pack/compose vs conservative geometry replay + sparse pack + 1x sparse/MSAA full compose.\n"
                    "Includes identical full depth copy and scene feedback, plus optimized packed-target clears.\n"
                    "D3D11 offscreen synthetic weapon (~17%% screen), BGRA8 color/D24S8 depth, amount .25, focus 1/.75.\n"
                    "8 measured runs/mode in ABBA order after 2 warmups/mode; timestamps exclude fixture setup, shader compilation, readback.\n"
                    "Replay fixture is one quad (6 vertices) with a trivial VS; real weapon VS/draw overhead is not represented.\n"
                    "These are shader/pass GPU timings, not game frame times or D3D12 barrier/CPU timing.\n");
        auto fixture_shader = Shader(R"(
            cbuffer Constants : register(b1) { float4 zp; float4 vp; float4 focus; };
            struct Out { float4 color : SV_Target; float depth : SV_Depth; };
            Out main(float4 p : SV_Position, uint s : SV_SampleIndex) {
                bool gun = p.x >= floor(focus.z * .3) && p.x < floor(focus.z * .7) &&
                           p.y >= floor(focus.w * .55) && p.y < floor(focus.w * .98);
                if (int(p.x) == int(floor(focus.z * .3)) && s != 0) gun = false;
                float z = gun ? lerp(.1, .8, saturate((p.x / focus.z - .3) / .4)) : 5;
                Out o; o.depth = 1.001 - .01 / z;
                o.color = gun ? float4(float((int(p.x) + int(p.y)) & 1), .125, 0, .7)
                              : float4(.03 * s, .2 + .04 * s, .6 + .03 * s, .2 + .05 * s);
                return o;
            })", "main");
        auto background_shader = Shader(R"(
            float4 main(float4 p : SV_Position, uint s : SV_SampleIndex) : SV_Target {
                return float4(.03 * s, .2 + .04 * s, .6 + .03 * s, .2 + .05 * s);
            })", "main");
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        ComPtr<ID3D11DepthStencilState> fill_state;
        Hr(device->CreateDepthStencilState(&dd, &fill_state), "Create benchmark depth state");
        constexpr std::array<std::array<unsigned, 2>, 2> extents{{{1280, 720}, {2880, 2160}}};
        for (const auto extent : extents) {
            draw_width = extent[0]; draw_height = extent[1];
            const unsigned columns = (draw_width + 7) / 8, rows = (draw_height + 7) / 8;
            const D3D11_VIEWPORT full_vp{0, 0, float(draw_width), float(draw_height), 0, 1};
            const D3D11_VIEWPORT tile_vp{0, 0, float(draw_width) / 8, float(draw_height) / 8, 0, 1};
            const D3D11_RECT tile_scissor{0, 0, LONG(columns), LONG(rows)};
            const float x0 = std::floor(draw_width * .3f), x1 = std::floor(draw_width * .7f);
            const float y0 = std::floor(draw_height * .55f), y1 = std::floor(draw_height * .98f);
            auto position = [&](float x, float y) { return std::array<float, 4>{2*x/draw_width-1, 1-2*y/draw_height, .5f, 1}; };
            auto weapon_vertices = Geometry({position(x0,y0),position(x1,y0),position(x0,y1),
                                             position(x0,y1),position(x1,y0),position(x1,y1)});
            for (unsigned samples : {1u, 8u}) {
                bool supported = true;
                for (DXGI_FORMAT format : {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_D24_UNORM_S8_UINT}) {
                    UINT levels = 0;
                    Hr(device->CheckMultisampleQualityLevels(format, samples, &levels), "Check benchmark MSAA");
                    supported &= levels != 0;
                }
                if (!supported) { std::printf("SKIP benchmark %ux%u %ux unsupported\n", draw_width, draw_height, samples); continue; }
                CompilePack(samples);
                auto make = [&](DXGI_FORMAT format, unsigned count = 1) {
                    return Make(format, count, nullptr, 0, draw_width, draw_height);
                };
                auto make_depth = [&](ComPtr<ID3D11DepthStencilView>& dsv) {
                    Texture t;
                    D3D11_TEXTURE2D_DESC d{};
                    d.Width = draw_width; d.Height = draw_height; d.MipLevels = d.ArraySize = 1;
                    d.Format = DXGI_FORMAT_R24G8_TYPELESS; d.SampleDesc.Count = samples;
                    d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
                    Hr(device->CreateTexture2D(&d, nullptr, &t.tex), "Create benchmark depth");
                    D3D11_DEPTH_STENCIL_VIEW_DESC dv{}; dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
                    dv.ViewDimension = samples > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
                    Hr(device->CreateDepthStencilView(t.tex.Get(), &dv, &dsv), "Create benchmark DSV");
                    D3D11_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
                    sv.ViewDimension = samples > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
                    if (samples == 1) sv.Texture2D.MipLevels = 1;
                    Hr(device->CreateShaderResourceView(t.tex.Get(), &sv, &t.srv), "Create benchmark depth SRV");
                    return t;
                };
                ComPtr<ID3D11DepthStencilView> world_dsv, before_dsv, post_dsv;
                Texture world = make_depth(world_dsv), before = make_depth(before_dsv), post = make_depth(post_dsv);
                Texture color = make(DXGI_FORMAT_B8G8R8A8_UNORM, samples);
                Texture output = make(DXGI_FORMAT_B8G8R8A8_UNORM, samples);
                Texture scene = make(DXGI_FORMAT_B8G8R8A8_UNORM);
                Texture packed = make(DXGI_FORMAT_R16G16B16A16_FLOAT), depth = make(DXGI_FORMAT_R16_FLOAT);
                Texture tiles = Make(DXGI_FORMAT_R8_UNORM, 1, nullptr, 0, columns, rows);
                const float cb[16] = {-.01f, 1.001f, .25f, float(draw_height) / 1080,
                                     0, 0, float(draw_width), float(draw_height), 1, .75f,
                                     float(draw_width), float(draw_height)};
                context->UpdateSubresource(constants.Get(), 0, nullptr, cb, 0, 0);
                context->ClearDepthStencilView(world_dsv.Get(), D3D11_CLEAR_DEPTH, Raw(5), 0);
                context->ClearDepthStencilView(post_dsv.Get(), D3D11_CLEAR_DEPTH, Raw(5), 0);
                context->RSSetViewports(1, &full_vp);
                Draw(background_shader.Get(), {color.rtv.Get()}, {});
                ID3D11RenderTargetView* color_view = color.rtv.Get();
                context->OMSetRenderTargets(1, &color_view, post_dsv.Get());
                context->OMSetDepthStencilState(fill_state.Get(), 0);
                context->OMSetBlendState(nullptr, nullptr, ~0u);
                BindGeometry(weapon_vertices.Get());
                context->PSSetShader(fixture_shader.Get(), nullptr, 0);
                context->Draw(6, 0);
                context->OMSetRenderTargets(0, nullptr, nullptr);
                context->OMSetDepthStencilState(nullptr, 0);
                context->IASetInputLayout(nullptr);

                ComPtr<ID3D11Query> disjoint;
                D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
                Hr(device->CreateQuery(&qd, &disjoint), "Create disjoint query");
                std::array<ComPtr<ID3D11Query>, 6> stamps;
                qd.Query = D3D11_QUERY_TIMESTAMP;
                for (auto& stamp : stamps) Hr(device->CreateQuery(&qd, &stamp), "Create timestamp");
                auto get_data = [&](ID3D11Query* query, void* data, UINT bytes) {
                    const ULONGLONG deadline = GetTickCount64() + 30000;
                    HRESULT hr;
                    while ((hr = context->GetData(query, data, bytes, D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE) {
                        Check(GetTickCount64() < deadline, "GPU timestamp timeout"); Sleep(1);
                    }
                    Hr(hr, "Read timestamp");
                };
                auto run = [&](bool optimized) {
                    // Reset destination outside measurement: production writes the
                    // existing scene target, so this fixture-only copy is not a pass cost.
                    context->CopyResource(output.tex.Get(), color.tex.Get());
                    context->Begin(disjoint.Get()); context->End(stamps[0].Get());
                    context->CopyResource(before.tex.Get(), world.tex.Get());
                    context->End(stamps[1].Get());
                    if (samples > 1) context->ResolveSubresource(scene.tex.Get(), 0, color.tex.Get(), 0, DXGI_FORMAT_B8G8R8A8_UNORM);
                    else context->CopyResource(scene.tex.Get(), color.tex.Get());
                    context->End(stamps[2].Get());
                    if (optimized) {
                        const float zero[4]{};
                        context->ClearRenderTargetView(tiles.rtv.Get(), zero);
                        context->RSSetViewports(1, &tile_vp);
                        context->RSSetScissorRects(1, &tile_scissor);
                        context->RSSetState(conservative.Get());
                        BindGeometry(weapon_vertices.Get());
                        ID3D11RenderTargetView* mask_view = tiles.rtv.Get();
                        context->OMSetRenderTargets(1, &mask_view, nullptr);
                        context->OMSetBlendState(nullptr, nullptr, ~0u);
                        context->PSSetShader(coverage_ps.Get(), nullptr, 0);
                        context->Draw(6, 0);
                        context->OMSetRenderTargets(0, nullptr, nullptr);
                        context->RSSetState(nullptr); context->IASetInputLayout(nullptr);
                    }
                    context->End(stamps[3].Get());
                    context->RSSetViewports(1, &full_vp);
                    if (optimized) {
                        const float zero[4] = {};
                        context->ClearRenderTargetView(packed.rtv.Get(), zero);
                        context->ClearRenderTargetView(depth.rtv.Get(), zero);
                    }
                    Draw(packs.at(samples).Get(), {packed.rtv.Get(), depth.rtv.Get()},
                         {post.srv.Get(), before.srv.Get(), color.srv.Get()}, ~0u, false,
                         optimized ? tiles.srv.Get() : nullptr);
                    context->End(stamps[4].Get());
                    Draw(blur.Get(), {output.rtv.Get()}, {packed.srv.Get(), depth.srv.Get(), scene.srv.Get()}, ~0u, true,
                         optimized && samples == 1 ? tiles.srv.Get() : nullptr);
                    context->End(stamps[5].Get()); context->End(disjoint.Get()); context->Flush();
                    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency{};
                    get_data(disjoint.Get(), &frequency, sizeof(frequency));
                    Check(!frequency.Disjoint && frequency.Frequency != 0, "disjoint GPU timing; rerun benchmark");
                    std::array<UINT64, 6> ticks{};
                    for (unsigned i = 0; i < ticks.size(); ++i) get_data(stamps[i].Get(), &ticks[i], sizeof(ticks[i]));
                    std::array<double, 6> elapsed{};
                    for (unsigned i = 0; i < 5; ++i) elapsed[i] = double(ticks[i + 1] - ticks[i]) * 1000 / frequency.Frequency;
                    elapsed[5] = double(ticks[5] - ticks[0]) * 1000 / frequency.Frequency;
                    return elapsed;
                };
                for (bool mode : {false, true, true, false}) (void)run(mode);
                std::array<std::vector<std::array<double, 6>>, 2> results;
                for (unsigned repeat = 0; repeat < 4; ++repeat)
                    for (bool mode : {false, true, true, false}) results[unsigned(mode)].push_back(run(mode));
                auto percentile = [&](unsigned mode, unsigned stage, double fraction) {
                    std::vector<double> values;
                    for (const auto& row : results[mode]) values.push_back(row[stage]);
                    std::sort(values.begin(), values.end());
                    const double at = fraction * (values.size() - 1);
                    const size_t lo = size_t(at), hi = (std::min)(lo + 1, values.size() - 1);
                    return values[lo] + (values[hi] - values[lo]) * (at - lo);
                };
                for (unsigned mode = 0; mode < 2; ++mode)
                    std::printf("BENCH %ux%u %ux %s ms p50: depth_copy=%.4f scene=%.4f geometry_mask=%.4f pack=%.4f compose=%.4f total=%.4f total_p90=%.4f\n",
                        draw_width, draw_height, samples, mode ? "tiles" : "full",
                        percentile(mode, 0, .5), percentile(mode, 1, .5), percentile(mode, 2, .5),
                        percentile(mode, 3, .5), percentile(mode, 4, .5), percentile(mode, 5, .5), percentile(mode, 5, .9));
                std::printf("DELTA %ux%u %ux total_p50 %.2f%% (negative is faster)\n", draw_width, draw_height, samples,
                    (percentile(1, 5, .5) / percentile(0, 5, .5) - 1) * 100);
                auto read_output = [&] {
                    if (samples > 1) context->ResolveSubresource(scene.tex.Get(), 0, output.tex.Get(), 0, DXGI_FORMAT_B8G8R8A8_UNORM);
                    else context->CopyResource(scene.tex.Get(), output.tex.Get());
                    D3D11_TEXTURE2D_DESC sd{}; scene.tex->GetDesc(&sd);
                    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    ComPtr<ID3D11Texture2D> stage;
                    Hr(device->CreateTexture2D(&sd, nullptr, &stage), "Create benchmark readback");
                    context->CopyResource(stage.Get(), scene.tex.Get());
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    Hr(context->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map benchmark readback");
                    std::vector<uint8_t> bytes(size_t(draw_width) * draw_height * 4);
                    for (unsigned y = 0; y < draw_height; ++y)
                        std::memcpy(bytes.data() + size_t(y) * draw_width * 4,
                                    static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch,
                                    size_t(draw_width) * 4);
                    context->Unmap(stage.Get(), 0);
                    return bytes;
                };
                (void)run(false); const auto expected = read_output();
                (void)run(true); const auto actual = read_output();
                Check(expected == actual, "benchmark production-format output differs between modes");
                std::printf("QUALITY %ux%u %ux: resolved BGRA8 output byte-identical (outside timing)\n",
                            draw_width, draw_height, samples);
                std::fflush(stdout);
            }
        }
    }

    // x=8 is a single covered sample; x=9..23 fully covered. The remaining samples are bright blue world.
    Result Run(unsigned samples, float amount, float z = .1f, bool invalid = false, bool cropped = false,
               bool constant_color = false, bool unchanged = false, float focus = 1.0f,
               float width = .75f, bool depth_ramp = false, bool sparse_edges = false, bool ms_context = false,
               float near_plane = 0, float projection_numerator = -.01f) {
        CompilePack(samples);
        const auto projected_depth = [&](float view_z) { return 1.001f + projection_numerator / view_z; };
        const auto native_depth = [&](float view_z) {
            constexpr float front_share = 1.f / 4096;
            return near_plane > 0 ? (view_z < near_plane ? front_share * view_z / near_plane :
                                     front_share + (1 - front_share) * projected_depth(view_z)) : projected_depth(view_z);
        };
        Texture pre = Make(R, samples), post = Make(R, samples), color = Make(RGBA, samples);
        for (unsigned sample = 0; sample < samples; ++sample) {
            std::vector<float> before(W * H, projected_depth(5)), after = before;
            std::vector<Pixel> pixels(W * H);
            for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) {
                size_t i = At(x,y);
                pixels[i] = {0, .25f, 1, .2f + .05f * sample};
                if (ms_context) pixels[i] = {.03f * sample, .2f + .04f * sample, .6f + .03f * sample, pixels[i].a};
                const bool covered = sparse_edges
                    ? ((x == W - 1 && y == H - 1 && sample == samples - 1) ||
                       (x >= 6 && x <= 9 && y >= 6 && y <= 9 && sample == 0))
                    : (x >= 8 && x < 24 && y >= 4 && y < 28 && (x > 8 || sample == 0));
                if (covered) {
                    float gun_z = depth_ramp ? (y < 12 ? .1f : y < 20 ? .8f : 1.0f) : z;
                    after[i] = unchanged ? before[i] : native_depth(gun_z);
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
        const float cb[16] = {projection_numerator, 1.001f, amount, 1,
                              cropped ? 10.f : 0.f, 0, cropped ? 12.f : float(W), float(H),
                              focus, width, float(W), float(H), near_plane, 0, 0, 0};
        context->UpdateSubresource(constants.Get(), 0, nullptr, cb, 0, 0);
        Draw(packs.at(samples).Get(), {packed.rtv.Get(), depth.rtv.Get()},
             {post.srv.Get(), pre.srv.Get(), color.srv.Get()});
        Draw(blur.Get(), {output.rtv.Get()}, {packed.srv.Get(), depth.srv.Get(), scene.srv.Get()}, ~0u, true);
        Result reference{ReadColor(packed, true), ReadColor(scene), ReadColor(output), Read(depth, 1, true)};
        Texture output_ms, resolved_ms, reference_raw;
        std::vector<Pixel> reference_ms;
        if (samples > 1) {
            output_ms = Make(RGBA, samples);
            resolved_ms = Make(RGBA);
            context->CopyResource(output_ms.tex.Get(), color.tex.Get());
            Draw(blur.Get(), {output_ms.rtv.Get()}, {packed.srv.Get(), depth.srv.Get(), scene.srv.Get()}, ~0u, true);
            context->ResolveSubresource(resolved_ms.tex.Get(), 0, output_ms.tex.Get(), 0, RGBA);
            reference_ms = ReadColor(resolved_ms);
            if (ms_context) {
                reference_raw = Make(RGBA, samples);
                context->CopyResource(reference_raw.tex.Get(), output_ms.tex.Get());
            }
        }

        // Unsupported host viewports must execute the unchanged full path. A
        // poisoned packed target makes accidental masked work observable; compare
        // raw MSAA samples as well as resolved output, preserving later blends.
        const auto full_raw = samples > 1 ? ReadSamples(output_ms, samples) : std::vector<Pixel>{};
        for (const auto& host : std::array<std::array<float, 4>, 2>{{
                 {0, 0, float(W), float(H)}, {.375f, .625f, W - 1.25f, H - 1.5f}}}) {
            gpu::WeaponDofRegion fallback;
            fallback.Begin(1, gpu::WeaponDofViewportSupported(host[0], host[1], host[2], host[3]));
            Check(!fallback.Complete(1), "unsupported host viewport selected sparse packing");
            const float poison_fallback[4] = {31, 7, 13, 1};
            context->ClearRenderTargetView(packed.rtv.Get(), poison_fallback);
            context->ClearRenderTargetView(depth.rtv.Get(), poison_fallback);
            Draw(packs.at(samples).Get(), {packed.rtv.Get(), depth.rtv.Get()},
                 {post.srv.Get(), pre.srv.Get(), color.srv.Get()});
            context->CopyResource(output.tex.Get(), scene.tex.Get());
            Draw(blur.Get(), {output.rtv.Get()}, {packed.srv.Get(), depth.srv.Get(), scene.srv.Get()}, ~0u, true);
            const auto fallback_packed = ReadColor(packed, true), fallback_output = ReadColor(output);
            const auto fallback_depth = Read(depth, 1, true);
            Check(std::memcmp(fallback_packed.data(), reference.packed.data(), W * H * sizeof(Pixel)) == 0 &&
                  std::memcmp(fallback_depth.data(), reference.depth.data(), W * H * sizeof(float)) == 0 &&
                  std::memcmp(fallback_output.data(), reference.output.data(), W * H * sizeof(Pixel)) == 0,
                  "unsupported viewport full fallback differs from reference");
            if (samples > 1) {
                context->CopyResource(output_ms.tex.Get(), color.tex.Get());
                Draw(blur.Get(), {output_ms.rtv.Get()}, {packed.srv.Get(), depth.srv.Get(), scene.srv.Get()}, ~0u, true);
                const auto fallback_raw = ReadSamples(output_ms, samples);
                Check(std::memcmp(fallback_raw.data(), full_raw.data(), full_raw.size() * sizeof(Pixel)) == 0,
                      "unsupported viewport fallback changed a raw MSAA sample");
            }
        }

        // Exercise the sparse pack consumer independently using the test-only
        // depth oracle, even on odd extents that production geometry rejects.
        // Full zero clears are required: guarded bilinear taps may cross into an
        // inactive tile, including one that contained geometry on a prior frame.
        constexpr unsigned columns = (W + 7) / 8, rows = (H + 7) / 8;
        Texture tiles = Make(R, 1, nullptr, 0, columns, rows);
        D3D11_VIEWPORT tile_viewport{0, 0, float(columns), float(rows), 0, 1};
        context->RSSetViewports(1, &tile_viewport);
        Draw(tile_masks.at(samples).Get(), {tiles.rtv.Get()}, {post.srv.Get(), pre.srv.Get()});
        const auto mask = Read(tiles, 1, false);
        unsigned active = 0;
        for (float value : mask) { Check(value == 0 || value == 1, "tile mask is not binary"); active += value != 0; }
        Check(active < columns * rows, "fixture did not eliminate any tiles");
        for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x)
            if (reference.packed[At(x,y)].a > 0)
                Check(mask[(y / 8) * columns + x / 8] == 1, "tile mask omitted a covered pixel");
        if (unchanged) Check(active == 0, "unchanged world activated a tile");
        if (sparse_edges) Check(active == 5, "sparse tile coverage or odd final tile missing");

        D3D11_VIEWPORT full_viewport{0, 0, float(W), float(H), 0, 1};
        context->RSSetViewports(1, &full_viewport);
        const float poison[4] = {31, 7, 13, 1}, zero[4] = {};
        context->ClearRenderTargetView(packed.rtv.Get(), poison);
        context->ClearRenderTargetView(depth.rtv.Get(), poison);
        context->ClearRenderTargetView(packed.rtv.Get(), zero);
        context->ClearRenderTargetView(depth.rtv.Get(), zero);
        context->CopyResource(output.tex.Get(), scene.tex.Get());
        Draw(packs.at(samples).Get(), {packed.rtv.Get(), depth.rtv.Get()},
             {post.srv.Get(), pre.srv.Get(), color.srv.Get()}, ~0u, false, tiles.srv.Get());
        Draw(blur.Get(), {output.rtv.Get()}, {packed.srv.Get(), depth.srv.Get(), scene.srv.Get()},
             ~0u, true, tiles.srv.Get());
        Result result{ReadColor(packed, true), reference.original, ReadColor(output), Read(depth, 1, true)};
        Check(std::memcmp(result.packed.data(), reference.packed.data(), W * H * sizeof(Pixel)) == 0,
              "sparse pack differs from full-screen pack");
        Check(std::memcmp(result.depth.data(), reference.depth.data(), W * H * sizeof(float)) == 0,
              "sparse depth differs from full-screen depth");
        Check(std::memcmp(result.output.data(), reference.output.data(), W * H * sizeof(Pixel)) == 0,
              "sparse composition differs from full-screen composition");
        if (samples > 1) {
            context->CopyResource(output_ms.tex.Get(), color.tex.Get());
            // Production deliberately retains full composition for MSAA. Even
            // world-only tiles must broadcast resolved RGB exactly as before,
            // because subsequent coverage/depth-dependent blending sees samples.
            Draw(blur.Get(), {output_ms.rtv.Get()}, {packed.srv.Get(), depth.srv.Get(), scene.srv.Get()},
                 ~0u, true);
            context->ResolveSubresource(resolved_ms.tex.Get(), 0, output_ms.tex.Get(), 0, RGBA);
            const auto sparse_ms = ReadColor(resolved_ms);
            Check(std::memcmp(sparse_ms.data(), reference_ms.data(), W * H * sizeof(Pixel)) == 0,
                  "resolved MSAA production policy differs from full-screen reference");
            for (size_t i = 0; i < sparse_ms.size(); ++i)
                Check(sparse_ms[i].a == reference.original[i].a, "MSAA destination alpha changed");
            if (ms_context) {
                const auto expected = ReadSamples(reference_raw, samples), actual = ReadSamples(output_ms, samples);
                Check(std::memcmp(expected.data(), actual.data(), expected.size() * sizeof(Pixel)) == 0,
                      "raw MSAA production-policy samples differ before later blending");
                Texture rejected_sparse = Make(RGBA, samples);
                context->CopyResource(rejected_sparse.tex.Get(), color.tex.Get());
                Draw(blur.Get(), {rejected_sparse.rtv.Get()}, {packed.srv.Get(), depth.srv.Get(), scene.srv.Get()},
                     ~0u, true, tiles.srv.Get());
                const auto rejected = ReadSamples(rejected_sparse, samples);
                Check(std::memcmp(expected.data(), rejected.data(), expected.size() * sizeof(Pixel)) != 0,
                      "negative control failed to expose raw-sample difference from sparse MSAA composition");
                CheckLaterBlending(reference_raw, output_ms, rejected_sparse, post, samples);
            }
        }
        return result;
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
float DetailBand(const Result& r, unsigned y0, unsigned y1) {
    float contrast = 0;
    for (unsigned y = y0; y < y1; ++y) for (unsigned x = 12; x < 20; ++x)
        contrast += std::abs(r.output[At(x,y)].r - .5f);
    return contrast;
}
float Detail(const Result& r) { return DetailBand(r, 10, 22); }
float ChangeBand(const Result& r, unsigned y0, unsigned y1) {
    float change = 0;
    for (unsigned y = y0; y < y1; ++y) for (unsigned x = 12; x < 20; ++x)
        change += std::abs(r.output[At(x,y)].r - r.original[At(x,y)].r);
    return change;
}
void ExactIdentity(const Result& r, const char* why) {
    for (size_t i = 0; i < r.output.size(); ++i) {
        const Pixel& got = r.output[i];
        const Pixel& want = r.original[i];
        if (std::memcmp(&got, &want, sizeof(Pixel)) == 0) continue;
        char detail[256];
        std::snprintf(detail, sizeof(detail), "%s at (%u,%u): got %.6g/%.6g/%.6g/%.6g, expected %.6g/%.6g/%.6g/%.6g",
                      why, unsigned(i % W), unsigned(i / W), got.r, got.g, got.b, got.a,
                      want.r, want.g, want.b, want.a);
        throw std::runtime_error(detail);
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        bool hardware = false, benchmark = false;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--hardware") == 0) hardware = true;
            else if (std::strcmp(argv[i], "--benchmark") == 0) benchmark = true;
            else throw std::runtime_error("usage: viewmodel_dof_warp [--hardware --benchmark]");
        }
        Check(hardware == benchmark, "hardware benchmarking requires both --hardware and --benchmark");
        Warp warp(hardware);
        warp.GeometryCoverage();
        if (benchmark) { warp.Benchmark(); return 0; }
        for (unsigned n : {1u, 2u, 4u, 8u}) {
            warp.CompilePack(n); // Compile all production variants even if this WARP adapter cannot execute one.
            if (!warp.Supports(n)) { std::printf("SKIP runtime %ux unsupported by WARP\n", n); continue; }
            auto zero = warp.Run(n, 0);
            ExactIdentity(zero, "zero amount is not exact identity");
            auto normal = warp.Run(n, .25f); Invariants(normal);
            // The widened-viewmodel path keeps the original projection's Z
            // coefficients (near=.08 here); its XY scaling does not enter the
            // depth inverse. Encode the exact native front/rear piecewise map.
            for (float z : {.03f, .08f, .1f, .25f, .8f}) {
                const auto remapped = warp.Run(n, .25f, z, false, false, false, false,
                                              .25f, .25f, false, false, false, .08f, -.08008f);
                Invariants(remapped);
                Check(std::abs(remapped.depth[At(9,16)] - z) < std::max(.0001f, z * .001f),
                      "native near-depth encoding reconstructed the wrong focus distance");
                Check(remapped.packed[At(8,16)].a == 1.f / n && remapped.packed[At(9,16)].a == 1,
                      "near-depth mapping changed MSAA coverage");
                const auto legacy = warp.Run(n, .25f, z, false, false, false, false, .25f, .25f);
                float maximum_difference = 0;
                for (size_t i = 0; i < legacy.output.size(); ++i)
                    maximum_difference = std::max(maximum_difference, std::abs(remapped.output[i].r - legacy.output[i].r));
                Check(maximum_difference < .003f, "near-depth mapping changed the physical-depth focus curve");
                if (z >= .25f) ExactIdentity(remapped, "remapped weapon at/beyond focus did not remain sharp");
                std::printf("PASS near-depth %ux z=%.3g plane=.08 focus=.25 maxRGBdifference=%.7g\n", n, z, maximum_difference);
            }
            ExactIdentity(warp.Run(n, 0, .03f, false, false, false, false, .25f, .25f,
                                   false, false, false, .08f, -.08008f), "near-depth zero amount changed native output");
            Check(normal.packed[At(8,16)].a == 1.f / n, "incorrect single-sample gun coverage");
            Check(normal.packed[At(9,16)].a == 1, "full gun coverage missing");
            Check(normal.depth[At(9,16)] > .099f && normal.depth[At(9,16)] < .101f, "wrong packed depth");
            Check(normal.packed[At(8,16)].b == 0, "raw MSAA world leaked into packed gun");
            Check(normal.output[At(8,16)].r >= 0 && normal.output[At(8,16)].r <= 1.f / n,
                  "partial coverage was not respected by delta composition");
            Check(Detail(normal) < Detail(zero) * .85f, "default amount did not soften gun texture");
            auto faint = warp.Run(n, .01f); Invariants(faint);
            Check(ChangeBand(faint, 10, 22) > 0 &&
                  ChangeBand(faint, 10, 22) < ChangeBand(normal, 10, 22) * .15f,
                  "amount did not fade continuously from exact zero to normal blur");
            auto farther = warp.Run(n, .25f, 1.5f); Invariants(farther);
            ExactIdentity(farther, "gun beyond focus did not remain exactly sharp");
            Check(Detail(normal) < Detail(farther), "near hand is not softer than farther gun");
            auto at_focus = warp.Run(n, 1, 1.0f); Invariants(at_focus);
            ExactIdentity(at_focus, "gun at focus did not remain exactly sharp");
            auto maximum = warp.Run(n, 1); Invariants(maximum);
            auto ramp = warp.Run(n, .25f, .1f, false, false, false, false, 1.0f, .75f, true);
            Invariants(ramp);
            float near_detail = DetailBand(ramp, 6, 10);
            float middle_detail = DetailBand(ramp, 14, 18);
            float focus_detail = DetailBand(ramp, 22, 26);
            Check(near_detail < middle_detail && middle_detail < focus_detail,
                  "identically textured depth bands did not soften progressively toward focus");
            for (unsigned y = 22; y < 26; ++y) for (unsigned x = 12; x < 20; ++x) {
                const auto& a = ramp.output[At(x, y)];
                const auto& b = ramp.original[At(x, y)];
                Check(std::memcmp(&a, &b, sizeof(Pixel)) == 0, "focus band was not exactly sharp");
            }
            auto ramp_close_focus = warp.Run(n, .25f, .1f, false, false, false, false, .7f, .7f, true);
            Invariants(ramp_close_focus);
            Check(DetailBand(ramp_close_focus, 14, 18) > DetailBand(ramp, 14, 18),
                  "moving focus toward the same textured depth band did not sharpen it");
            auto ramp_narrow = warp.Run(n, .25f, .1f, false, false, false, false, 1.0f, .25f, true);
            Invariants(ramp_narrow);
            Check(DetailBand(ramp_narrow, 14, 18) < DetailBand(ramp, 14, 18),
                  "narrowing the focus transition did not soften the same depth band");
            ExactIdentity(warp.Run(n, 1, .1f, false, false, false, false, 0, .75f),
                          "nonpositive focus did not fail closed");
            ExactIdentity(warp.Run(n, 1, .1f, false, false, false, false, 1, 0),
                          "zero focus width did not fail closed");
            ExactIdentity(warp.Run(n, 1, .1f, false, false, false, false, 1, 1.25f),
                          "focus width beyond focus did not fail closed");
            ExactIdentity(warp.Run(n, 1, .1f, false, false, false, false,
                                   std::numeric_limits<float>::quiet_NaN(), .75f),
                          "nonfinite focus did not fail closed");
            auto flat = warp.Run(n, 1, .1f, false, false, true); Invariants(flat);
            for (size_t i = 0; i < flat.output.size(); ++i)
                Check(std::abs(flat.output[i].r - flat.original[i].r) < .00001f, "constant gun edge developed halo");
            auto bad = warp.Run(n, 1, .1f, true); Invariants(bad);
            for (unsigned x = 12; x <= 17; ++x) Check(bad.packed[At(x,12)].a == 0, "invalid/unchanged depth accepted");
            auto crop = warp.Run(n, 1, .1f, false, true); Invariants(crop);
            Check(crop.packed[At(9,16)].a == 0 && crop.packed[At(22,16)].a == 0, "viewport exterior packed");
            auto unchanged = warp.Run(n, 1, .1f, false, false, false, true); Invariants(unchanged);
            for (const auto& p : unchanged.packed) Check(p.a == 0, "unchanged world classified as gun");
            auto sparse = warp.Run(n, 1, .1f, false, false, false, false, 1, .75f, false, true);
            Invariants(sparse);
            Check(sparse.packed[At(W - 1, H - 1)].a == 1.f / n,
                  "last sample at odd target edge lost coverage");
            if (n > 1) Invariants(warp.Run(n, .25f, .1f, false, false, false, false, 1, .75f, false, false, true));
            std::printf("PASS viewmodel DOF %ux: alpha, coverage, guarded blur, focus ramp and identity, amount fade, invalids, viewport; sparse pack/1x compose parity, MSAA full compose raw-sample and later-blend parity\n", n);
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL viewmodel DOF: %s\n", error.what()); return 1;
    }
}
