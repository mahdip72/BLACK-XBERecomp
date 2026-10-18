// Offscreen regression checks for the production depth-of-field pixel shader.
// Uses only D3D_DRIVER_TYPE_WARP; never creates a window or starts the game.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "d3d/depth_of_field_shader.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr unsigned kWidth = 64;
constexpr unsigned kHeight = 64;
constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_R32_FLOAT;

struct Pixel {
    float r, g, b, a;
};

void Check(bool ok, const char* message);
size_t At(unsigned x, unsigned y);
void CheckRangeAndAlpha(const std::vector<Pixel>& output,
                        const std::vector<Pixel>& input);

void WriteBmp(const char* name, const std::vector<Pixel>& pixels,
              unsigned width, unsigned height, unsigned scale = 4) {
    Check(pixels.size() == size_t(width) * height, "bad BMP fixture size");
    namespace fs = std::filesystem;
    const fs::path directory = fs::path("out") / "tests" / "dof-tests" / "fixtures";
    fs::create_directories(directory);
    const unsigned output_width = width * scale;
    const unsigned output_height = height * scale;
    const unsigned pitch = (output_width * 3 + 3) & ~3u;
    std::ofstream file(directory / name, std::ios::binary);
    Check(bool(file), "cannot open fixture BMP");
    auto u16 = [&](uint16_t value) {
        file.put(char(value & 255));
        file.put(char(value >> 8));
    };
    auto u32 = [&](uint32_t value) {
        u16(uint16_t(value));
        u16(uint16_t(value >> 16));
    };
    file.write("BM", 2);
    u32(54 + pitch * output_height);
    u16(0); u16(0);
    u32(54);
    u32(40);
    u32(output_width); u32(output_height);
    u16(1); u16(24);
    u32(0); u32(pitch * output_height);
    u32(2835); u32(2835); u32(0); u32(0);
    std::vector<char> row(pitch, 0);
    auto channel = [](float value) -> char {
        return char(int(std::round(std::clamp(value, 0.0f, 1.0f) * 255.0f)));
    };
    for (unsigned y = 0; y < output_height; ++y) {
        const unsigned source_y = height - 1 - y / scale;
        for (unsigned x = 0; x < output_width; ++x) {
            const Pixel& p = pixels[size_t(source_y) * width + x / scale];
            row[3 * x + 0] = channel(p.b);
            row[3 * x + 1] = channel(p.g);
            row[3 * x + 2] = channel(p.r);
        }
        file.write(row.data(), row.size());
    }
    file.flush();
    Check(bool(file), "cannot write fixture BMP");
}

void WriteContactSheet(const std::vector<std::vector<Pixel>>& tiles,
                       unsigned columns, const char* name = "contact-sheet.bmp") {
    Check(columns > 0 && tiles.size() % columns == 0, "bad contact-sheet layout");
    const unsigned rows = unsigned(tiles.size()) / columns;
    std::vector<Pixel> sheet(size_t(columns * kWidth) * rows * kHeight,
                             {0.08f, 0.08f, 0.08f, 1});
    for (unsigned i = 0; i < tiles.size(); ++i) {
        Check(tiles[i].size() == kWidth * kHeight, "bad contact-sheet tile");
        const unsigned tile_x = i % columns;
        const unsigned tile_y = i / columns;
        for (unsigned y = 0; y < kHeight; ++y)
            for (unsigned x = 0; x < kWidth; ++x)
                sheet[size_t(tile_y * kHeight + y) * columns * kWidth +
                      tile_x * kWidth + x] = tiles[i][At(x, y)];
    }
    WriteBmp(name, sheet, columns * kWidth, rows * kHeight);
}

struct Rect {
    LONG left, top, right, bottom;
};

void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

void Hr(HRESULT result, const char* where) {
    if (FAILED(result)) {
        char message[160];
        std::snprintf(message, sizeof(message), "%s failed: 0x%08lx", where,
                      static_cast<unsigned long>(result));
        throw std::runtime_error(message);
    }
}

size_t At(unsigned x, unsigned y) { return size_t(y) * kWidth + x; }

// A 0.1-to-100 world-unit perspective projection: viewZ = x / (rawDepth - y).
constexpr float kProjectionX = -0.1001001f;
constexpr float kProjectionY = 1.001001f;
float RawDepth(float world_units) { return kProjectionY + kProjectionX / world_units; }

float HalfToFloat(uint16_t bits) {
    const unsigned exponent = (bits >> 10) & 31;
    const unsigned fraction = bits & 1023;
    const float magnitude = exponent == 0
        ? std::ldexp(float(fraction), -24)
        : exponent == 31 ? INFINITY
        : std::ldexp(float(1024 + fraction), int(exponent) - 25);
    return bits & 0x8000 ? -magnitude : magnitude;
}

class Warp {
public:
    Warp() {
        D3D_FEATURE_LEVEL level{};
        Hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                             D3D11_CREATE_DEVICE_SINGLETHREADED, nullptr, 0,
                             D3D11_SDK_VERSION, &device_, &level, &context_),
           "D3D11CreateDevice(WARP)");
        Check(level >= D3D_FEATURE_LEVEL_11_0, "WARP feature level 11_0 required");
        auto bytecode = Compile("vs", "vs_5_0", 1);
        Hr(device_->CreateVertexShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(),
                                       nullptr, &vs_), "CreateVertexShader");

        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.ScissorEnable = TRUE;
        Hr(device_->CreateRasterizerState(&raster, &raster_), "CreateRasterizerState");

        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED |
                                                  D3D11_COLOR_WRITE_ENABLE_GREEN |
                                                  D3D11_COLOR_WRITE_ENABLE_BLUE;
        Hr(device_->CreateBlendState(&blend, &rgb_only_), "CreateBlendState");

        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxAnisotropy = 1;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        Hr(device_->CreateSamplerState(&sampler, &linear_), "CreateSamplerState(linear clamp)");
    }

    bool SupportsMsaa(unsigned samples) const {
        UINT depth_levels = 0, color_levels = 0;
        Hr(device_->CheckMultisampleQualityLevels(kDepthFormat, samples, &depth_levels),
           "CheckMultisampleQualityLevels(depth)");
        Hr(device_->CheckMultisampleQualityLevels(kColorFormat, samples, &color_levels),
           "CheckMultisampleQualityLevels(color)");
        return depth_levels > 0 && color_levels > 0;
    }

    void CompileVariant(unsigned samples) {
        GatherShader();
        ComposeShader();
        PackShader(samples);
        DownsampleShader();
    }

    std::vector<Pixel> Render(const std::vector<Pixel>& color,
                              const std::vector<float>& raw_depth, float radius,
                              Rect rect, unsigned samples = 1,
                              bool sample_zero_focused = false,
                              float focus_units = 1.0f,
                              bool varied_output_alpha = false,
                              bool mixed_edge = false,
                              Pixel* packed_probe = nullptr,
                              std::vector<Pixel>* raw_probe = nullptr,
                              double* pipeline_ms = nullptr,
                              float near_scale = 1.0f,
                              float far_scale = 0.45f) {
        Check(color.size() == kWidth * kHeight, "bad color fixture size");
        Check(raw_depth.size() == kWidth * kHeight, "bad depth fixture size");
        Check(rect.left >= 0 && rect.top >= 0 && rect.right <= LONG(kWidth) &&
              rect.bottom <= LONG(kHeight) && rect.right > rect.left &&
              rect.bottom > rect.top, "bad viewport fixture");
        Check(samples == 1 || SupportsMsaa(samples), "requested MSAA format unsupported");

        ComPtr<ID3D11Texture2D> scene, depth, output, staging,
                                 filtered_color, filtered_surface,
                                 layers, foreground_guide,
                                 resolved_output;
        ComPtr<ID3D11ShaderResourceView> scene_srv, output_srv, depth_srv,
                                         filtered_color_srv, filtered_surface_srv,
                                         layers_srv, foreground_guide_srv;
        ComPtr<ID3D11RenderTargetView> output_rtv;
        ComPtr<ID3D11Buffer> constants;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = kWidth;
        td.Height = kHeight;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = kColorFormat;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA pixels{color.data(), UINT(kWidth * sizeof(Pixel)), 0};
        Hr(device_->CreateTexture2D(&td, &pixels, &scene), "CreateTexture2D(scene)");
        Hr(device_->CreateShaderResourceView(scene.Get(), nullptr, &scene_srv),
           "CreateShaderResourceView(scene)");

        td.BindFlags = D3D11_BIND_RENDER_TARGET |
                       (samples > 1 ? D3D11_BIND_SHADER_RESOURCE : 0);
        td.SampleDesc.Count = samples;
        Hr(device_->CreateTexture2D(&td, samples == 1 ? &pixels : nullptr, &output),
           "CreateTexture2D(output)");
        Hr(device_->CreateRenderTargetView(output.Get(), nullptr, &output_rtv),
           "CreateRenderTargetView(output)");
        if (samples > 1)
            Hr(device_->CreateShaderResourceView(output.Get(), nullptr, &output_srv),
               "CreateShaderResourceView(raw MS scene)");

        td.SampleDesc.Count = 1;
        td.BindFlags = 0;
        if (samples > 1)
            Hr(device_->CreateTexture2D(&td, nullptr, &resolved_output),
               "CreateTexture2D(resolved output)");

        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Hr(device_->CreateTexture2D(&td, nullptr, &staging), "CreateTexture2D(staging)");

        const float cb[12] = {kProjectionX, kProjectionY, focus_units, radius,
                             float(rect.left), float(rect.top),
                             float(rect.right - rect.left), float(rect.bottom - rect.top),
                             near_scale, far_scale, 0, 0};
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(cb);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA cb_data{cb, 0, 0};
        Hr(device_->CreateBuffer(&bd, &cb_data, &constants), "CreateBuffer(Dof)");

        std::vector<float> original_resolved_alpha;
        if (samples > 1) {
            constexpr char initializer[] = R"(
                Texture2D<float4> source : register(t0);
                float4 initialize(float4 pos : SV_Position,
                                  uint sample : SV_SampleIndex) : SV_Target {
                    float4 c = source.Load(int3(pos.xy, 0));
                #ifdef VARY_ALPHA
                    c.a += sample * 0.005;
                #endif
                #ifdef MIXED_EDGE
                    if ((int)pos.x == 32)
                        c = sample == 0 ? float4(0.9, 0.1, 0.05, 0.3)
                                        : float4(0.05, 0.1, 0.9, 0.7);
                #endif
                    return c;
                })";
            const D3D_SHADER_MACRO varied[] = {{"VARY_ALPHA", "1"}, {nullptr, nullptr}};
            const D3D_SHADER_MACRO edge[] = {{"MIXED_EDGE", "1"}, {nullptr, nullptr}};
            ComPtr<ID3DBlob> binary, errors;
            HRESULT hr = D3DCompile(initializer, sizeof(initializer) - 1,
                                    "output initializer",
                                    mixed_edge ? edge : (varied_output_alpha ? varied : nullptr),
                                    nullptr, "initialize", "ps_5_0",
                                    D3DCOMPILE_ENABLE_STRICTNESS, 0, &binary, &errors);
            if (FAILED(hr))
                throw std::runtime_error(errors
                    ? static_cast<const char*>(errors->GetBufferPointer())
                    : "output initializer did not compile");
            ComPtr<ID3D11PixelShader> init_ps;
            Hr(device_->CreatePixelShader(binary->GetBufferPointer(), binary->GetBufferSize(),
                                          nullptr, &init_ps),
               "CreatePixelShader(output initializer)");
            ID3D11RenderTargetView* target = output_rtv.Get();
            ID3D11ShaderResourceView* source = scene_srv.Get();
            const D3D11_VIEWPORT full_viewport{0, 0, float(kWidth), float(kHeight), 0, 1};
            const D3D11_RECT full_scissor{0, 0, LONG(kWidth), LONG(kHeight)};
            context_->OMSetRenderTargets(1, &target, nullptr);
            context_->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
            context_->RSSetState(raster_.Get());
            context_->RSSetViewports(1, &full_viewport);
            context_->RSSetScissorRects(1, &full_scissor);
            context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context_->VSSetShader(vs_.Get(), nullptr, 0);
            context_->PSSetShader(init_ps.Get(), nullptr, 0);
            context_->PSSetShaderResources(0, 1, &source);
            context_->Draw(3, 0);
            ID3D11ShaderResourceView* empty = nullptr;
            context_->PSSetShaderResources(0, 1, &empty);
            context_->OMSetRenderTargets(0, nullptr, nullptr);

            // Resolve once before the production pass. The comparison below is
            // exact against the same D3D resolve arithmetic, including at 8x.
            context_->ResolveSubresource(resolved_output.Get(), 0, output.Get(), 0,
                                         kColorFormat);
            context_->CopyResource(staging.Get(), resolved_output.Get());
            D3D11_MAPPED_SUBRESOURCE initial{};
            Hr(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &initial),
               "Map(initial MS output)");
            original_resolved_alpha.resize(kWidth * kHeight);
            for (unsigned y = 0; y < kHeight; ++y) {
                const auto* row = reinterpret_cast<const Pixel*>(
                    static_cast<const unsigned char*>(initial.pData) + size_t(y) * initial.RowPitch);
                for (unsigned x = 0; x < kWidth; ++x)
                    original_resolved_alpha[At(x, y)] = row[x].a;
            }
            context_->Unmap(staging.Get(), 0);
        }

        td.Format = kDepthFormat;
        td.SampleDesc.Count = samples;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.CPUAccessFlags = 0;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (samples > 1 ? D3D11_BIND_RENDER_TARGET : 0);
        D3D11_SUBRESOURCE_DATA depths{raw_depth.data(), UINT(kWidth * sizeof(float)), 0};
        Hr(device_->CreateTexture2D(&td, samples == 1 ? &depths : nullptr, &depth),
           "CreateTexture2D(depth)");
        if (samples > 1) {
            ComPtr<ID3D11RenderTargetView> depth_rtv;
            Hr(device_->CreateRenderTargetView(depth.Get(), nullptr, &depth_rtv),
               "CreateRenderTargetView(MS depth)");
            const bool uniform = std::all_of(raw_depth.begin(), raw_depth.end(),
                                             [&](float z) { return z == raw_depth.front(); });
            if (sample_zero_focused || mixed_edge || !uniform) {
                // SV_SampleIndex forces per-sample writes. In the mixed fixture,
                // sample 0 lies at focus while the other samples use input depth.
                constexpr char fixture[] = R"(
                    Texture2D<float> fixture_depth : register(t0);
                    cbuffer B : register(b1) { float4 zp; };
                    float sample_depth(float4 pos : SV_Position,
                                       uint sample : SV_SampleIndex) : SV_Target {
                        float raw = fixture_depth.Load(int3(pos.xy, 0));
                    #ifdef MIXED
                        if (sample == 0) return zp.y + zp.x / zp.z;
                    #endif
                    #ifdef MIXED_EDGE
                        if ((int)pos.x == 32)
                            return sample == 0 ? zp.y + zp.x / 3.0
                                               : zp.y + zp.x / 15.0;
                    #endif
                        return raw;
                    })";
                const D3D_SHADER_MACRO mixed[] = {{"MIXED", "1"}, {nullptr, nullptr}};
                const D3D_SHADER_MACRO edge[] = {{"MIXED_EDGE", "1"}, {nullptr, nullptr}};
                ComPtr<ID3DBlob> binary, errors;
                HRESULT hr = D3DCompile(fixture, sizeof(fixture) - 1, "sample fixture",
                                        mixed_edge ? edge : (sample_zero_focused ? mixed : nullptr),
                                        nullptr, "sample_depth", "ps_5_0",
                                        D3DCOMPILE_ENABLE_STRICTNESS, 0, &binary, &errors);
                if (FAILED(hr))
                    throw std::runtime_error(errors
                        ? static_cast<const char*>(errors->GetBufferPointer())
                        : "sample fixture did not compile");
                ComPtr<ID3D11PixelShader> fixture_ps;
                Hr(device_->CreatePixelShader(binary->GetBufferPointer(), binary->GetBufferSize(),
                                              nullptr, &fixture_ps),
                   "CreatePixelShader(sample fixture)");
                D3D11_TEXTURE2D_DESC source_desc = td;
                source_desc.SampleDesc.Count = 1;
                source_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA source_data{raw_depth.data(),
                                                   UINT(kWidth * sizeof(float)), 0};
                ComPtr<ID3D11Texture2D> fixture_source;
                ComPtr<ID3D11ShaderResourceView> fixture_srv;
                Hr(device_->CreateTexture2D(&source_desc, &source_data, &fixture_source),
                   "CreateTexture2D(sample fixture)");
                Hr(device_->CreateShaderResourceView(fixture_source.Get(), nullptr,
                                                     &fixture_srv),
                   "CreateShaderResourceView(sample fixture)");
                ID3D11RenderTargetView* target = depth_rtv.Get();
                ID3D11Buffer* buffer = constants.Get();
                ID3D11ShaderResourceView* fixture_input = fixture_srv.Get();
                const D3D11_VIEWPORT viewport{0, 0, float(kWidth), float(kHeight), 0, 1};
                const D3D11_RECT full{0, 0, LONG(kWidth), LONG(kHeight)};
                context_->OMSetRenderTargets(1, &target, nullptr);
                context_->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
                context_->RSSetState(raster_.Get());
                context_->RSSetViewports(1, &viewport);
                context_->RSSetScissorRects(1, &full);
                context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                context_->VSSetShader(vs_.Get(), nullptr, 0);
                context_->PSSetShader(fixture_ps.Get(), nullptr, 0);
                context_->PSSetShaderResources(0, 1, &fixture_input);
                context_->PSSetConstantBuffers(1, 1, &buffer);
                context_->Draw(3, 0);
                if (mixed_edge || sample_zero_focused) {
                    // Some WARP R32_FLOAT target paths run this depth PS once
                    // for all samples despite SV_SampleIndex. Force the
                    // nonzero samples with OM's actual per-sample write mask.
                    constexpr char far_override[] = R"(
                        Texture2D<float> source : register(t0);
                        cbuffer B : register(b1) { float4 zp; };
                        float far_at_edge(float4 pos : SV_Position) : SV_Target {
                        #ifdef MIXED_EDGE
                            if ((int)pos.x != 32) discard;
                            return zp.y + zp.x / 15.0;
                        #else
                            return source.Load(int3(pos.xy, 0));
                        #endif
                        })";
                    const D3D_SHADER_MACRO edge_override_define[] = {
                        {"MIXED_EDGE", "1"}, {nullptr, nullptr}};
                    ComPtr<ID3DBlob> far_binary, far_errors;
                    const HRESULT far_hr = D3DCompile(
                        far_override, sizeof(far_override) - 1,
                        "far MS depth override",
                        mixed_edge ? edge_override_define : nullptr, nullptr,
                        "far_at_edge", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS,
                        0, &far_binary, &far_errors);
                    if (FAILED(far_hr))
                        throw std::runtime_error(far_errors
                            ? static_cast<const char*>(far_errors->GetBufferPointer())
                            : "far MS depth override did not compile");
                    ComPtr<ID3D11PixelShader> far_shader;
                    Hr(device_->CreatePixelShader(far_binary->GetBufferPointer(),
                                                  far_binary->GetBufferSize(),
                                                  nullptr, &far_shader),
                       "CreatePixelShader(far MS depth override)");
                    context_->PSSetShader(far_shader.Get(), nullptr, 0);
                    context_->OMSetBlendState(nullptr, nullptr,
                                              ((1u << samples) - 1u) & ~1u);
                    context_->Draw(3, 0);
                    context_->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
                }
                ID3D11ShaderResourceView* empty = nullptr;
                context_->PSSetShaderResources(0, 1, &empty);
                context_->OMSetRenderTargets(0, nullptr, nullptr);
            } else {
                const float clear[4] = {raw_depth.front(), 0, 0, 0};
                context_->ClearRenderTargetView(depth_rtv.Get(), clear);
            }
        }
        Hr(device_->CreateShaderResourceView(depth.Get(), nullptr, &depth_srv),
           "CreateShaderResourceView(depth)");

        if (raw_probe) {
            Check(samples > 1, "raw per-sample probe requires MSAA");
            constexpr char probe_hlsl[] = R"(
                Texture2DMS<float4> raw_color : register(t0);
                Texture2DMS<float> raw_depth : register(t1);
                float4 probe(float4 pos : SV_Position) : SV_Target {
                    int i = int(pos.x);
                    float4 c = raw_color.Load(int2(32, 32), i);
                    return float4(c.r, c.b, c.a, raw_depth.Load(int2(32, 32), i));
                })";
            ComPtr<ID3DBlob> binary, errors;
            const HRESULT compiled = D3DCompile(probe_hlsl, sizeof(probe_hlsl) - 1,
                                                "raw MS probe", nullptr, nullptr,
                                                "probe", "ps_5_0",
                                                D3DCOMPILE_ENABLE_STRICTNESS, 0,
                                                &binary, &errors);
            if (FAILED(compiled))
                throw std::runtime_error(errors
                    ? static_cast<const char*>(errors->GetBufferPointer())
                    : "raw MS probe did not compile");
            ComPtr<ID3D11PixelShader> probe_ps;
            Hr(device_->CreatePixelShader(binary->GetBufferPointer(), binary->GetBufferSize(),
                                          nullptr, &probe_ps), "CreatePixelShader(raw MS probe)");
            D3D11_TEXTURE2D_DESC probe_desc{};
            probe_desc.Width = samples;
            probe_desc.Height = probe_desc.MipLevels = probe_desc.ArraySize = 1;
            probe_desc.Format = kColorFormat;
            probe_desc.SampleDesc.Count = 1;
            probe_desc.Usage = D3D11_USAGE_DEFAULT;
            probe_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            ComPtr<ID3D11Texture2D> probe_target, probe_stage;
            ComPtr<ID3D11RenderTargetView> probe_rtv;
            Hr(device_->CreateTexture2D(&probe_desc, nullptr, &probe_target),
               "CreateTexture2D(raw MS probe target)");
            Hr(device_->CreateRenderTargetView(probe_target.Get(), nullptr, &probe_rtv),
               "CreateRenderTargetView(raw MS probe)");
            probe_desc.Usage = D3D11_USAGE_STAGING;
            probe_desc.BindFlags = 0;
            probe_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            Hr(device_->CreateTexture2D(&probe_desc, nullptr, &probe_stage),
               "CreateTexture2D(raw MS probe staging)");
            ID3D11RenderTargetView* target = probe_rtv.Get();
            ID3D11ShaderResourceView* inputs[2] = {output_srv.Get(), depth_srv.Get()};
            const D3D11_VIEWPORT probe_viewport{0, 0, float(samples), 1, 0, 1};
            const D3D11_RECT probe_scissor{0, 0, LONG(samples), 1};
            context_->OMSetRenderTargets(1, &target, nullptr);
            context_->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
            context_->RSSetState(raster_.Get());
            context_->RSSetViewports(1, &probe_viewport);
            context_->RSSetScissorRects(1, &probe_scissor);
            context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context_->VSSetShader(vs_.Get(), nullptr, 0);
            context_->PSSetShader(probe_ps.Get(), nullptr, 0);
            context_->PSSetShaderResources(0, 2, inputs);
            context_->Draw(3, 0);
            ID3D11ShaderResourceView* empty[2] = {};
            context_->PSSetShaderResources(0, 2, empty);
            context_->OMSetRenderTargets(0, nullptr, nullptr);
            context_->CopyResource(probe_stage.Get(), probe_target.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            Hr(context_->Map(probe_stage.Get(), 0, D3D11_MAP_READ, 0, &mapped),
               "Map(raw MS probe)");
            const auto* values = static_cast<const Pixel*>(mapped.pData);
            raw_probe->assign(values, values + samples);
            context_->Unmap(probe_stage.Get(), 0);
        }

        D3D11_TEXTURE2D_DESC scratch{};
        scratch.Width = kWidth;
        scratch.Height = kHeight;
        scratch.MipLevels = 4;
        scratch.ArraySize = 1;
        scratch.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        scratch.SampleDesc.Count = 1;
        scratch.Usage = D3D11_USAGE_DEFAULT;
        scratch.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        Hr(device_->CreateTexture2D(&scratch, nullptr, &filtered_color),
           "CreateTexture2D(filtered color mips)");
        scratch.Format = DXGI_FORMAT_R16_FLOAT;
        Hr(device_->CreateTexture2D(&scratch, nullptr, &filtered_surface),
           "CreateTexture2D(filtered depth/coverage mips)");
        const auto pipeline_started = std::chrono::steady_clock::now();
        for (unsigned mip = 0; mip < 4; ++mip) {
            D3D11_RENDER_TARGET_VIEW_DESC view{};
            view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipSlice = mip;
            ComPtr<ID3D11RenderTargetView> color_view, surface_view;
            view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            Hr(device_->CreateRenderTargetView(filtered_color.Get(), &view, &color_view),
               "CreateRenderTargetView(filtered color mip)");
            view.Format = DXGI_FORMAT_R16_FLOAT;
            Hr(device_->CreateRenderTargetView(filtered_surface.Get(), &view, &surface_view),
               "CreateRenderTargetView(filtered depth/coverage mip)");
            ComPtr<ID3D11ShaderResourceView> previous_color, previous_surface;
            if (mip > 0) {
                D3D11_SHADER_RESOURCE_VIEW_DESC previous{};
                previous.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                previous.Texture2D.MostDetailedMip = mip - 1;
                previous.Texture2D.MipLevels = 1;
                previous.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                Hr(device_->CreateShaderResourceView(filtered_color.Get(), &previous,
                                                     &previous_color),
                   "CreateShaderResourceView(previous color mip)");
                previous.Format = DXGI_FORMAT_R16_FLOAT;
                Hr(device_->CreateShaderResourceView(filtered_surface.Get(), &previous,
                                                     &previous_surface),
                   "CreateShaderResourceView(previous depth/coverage mip)");
            }
            ID3D11RenderTargetView* targets[2] = {color_view.Get(), surface_view.Get()};
            ID3D11ShaderResourceView* resources[4] = {
                mip == 0 ? depth_srv.Get() : nullptr,
                mip == 0 ? (samples > 1 ? output_srv.Get() : scene_srv.Get()) : nullptr,
                mip > 0 ? previous_color.Get() : nullptr,
                mip > 0 ? previous_surface.Get() : nullptr};
            ID3D11Buffer* buffer = constants.Get();
            const unsigned width = std::max(1u, kWidth >> mip);
            const unsigned height = std::max(1u, kHeight >> mip);
            const D3D11_VIEWPORT full_viewport{0, 0, float(width), float(height), 0, 1};
            const D3D11_RECT full_scissor{0, 0, LONG(width), LONG(height)};
            context_->OMSetRenderTargets(2, targets, nullptr);
            context_->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
            context_->RSSetState(raster_.Get());
            context_->RSSetViewports(1, &full_viewport);
            context_->RSSetScissorRects(1, &full_scissor);
            context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context_->VSSetShader(vs_.Get(), nullptr, 0);
            context_->PSSetShader(mip == 0 ? PackShader(samples) : DownsampleShader(),
                                  nullptr, 0);
            context_->PSSetShaderResources(0, 4, resources);
            context_->PSSetConstantBuffers(1, 1, &buffer);
            context_->Draw(3, 0);
            ID3D11ShaderResourceView* empty[4] = {};
            context_->PSSetShaderResources(0, 4, empty);
            context_->OMSetRenderTargets(0, nullptr, nullptr);
        }
        if (packed_probe) {
            D3D11_TEXTURE2D_DESC probe_desc = scratch;
            probe_desc.Width = probe_desc.Height = probe_desc.MipLevels = 1;
            probe_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            probe_desc.Usage = D3D11_USAGE_STAGING;
            probe_desc.BindFlags = 0;
            probe_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> probe_texture;
            Hr(device_->CreateTexture2D(&probe_desc, nullptr, &probe_texture),
               "CreateTexture2D(packed mip0 probe)");
            const D3D11_BOX center{32, 32, 0, 33, 33, 1};
            context_->CopySubresourceRegion(probe_texture.Get(), 0, 0, 0, 0,
                                            filtered_color.Get(), 0, &center);
            D3D11_MAPPED_SUBRESOURCE probe_map{};
            Hr(context_->Map(probe_texture.Get(), 0, D3D11_MAP_READ, 0, &probe_map),
               "Map(packed mip0 probe)");
            const auto* bits = static_cast<const uint16_t*>(probe_map.pData);
            *packed_probe = {HalfToFloat(bits[0]), HalfToFloat(bits[1]),
                             HalfToFloat(bits[2]), HalfToFloat(bits[3])};
            context_->Unmap(probe_texture.Get(), 0);
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC all_mips{};
        all_mips.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        all_mips.Texture2D.MostDetailedMip = 0;
        all_mips.Texture2D.MipLevels = 4;
        all_mips.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        Hr(device_->CreateShaderResourceView(filtered_color.Get(), &all_mips,
                                             &filtered_color_srv),
           "CreateShaderResourceView(all color mips)");
        all_mips.Format = DXGI_FORMAT_R16_FLOAT;
        Hr(device_->CreateShaderResourceView(filtered_surface.Get(), &all_mips,
                                             &filtered_surface_srv),
           "CreateShaderResourceView(all depth/coverage mips)");

        // Production performs its costly 96-tap gather at half resolution and
        // reconstructs the two layers against each full-resolution receiver.
        const unsigned half_width = (kWidth + 1) / 2;
        const unsigned half_height = (kHeight + 1) / 2;
        D3D11_TEXTURE2D_DESC layer_desc{};
        layer_desc.Width = half_width;
        layer_desc.Height = half_height;
        layer_desc.MipLevels = 1;
        layer_desc.ArraySize = 2;
        layer_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        layer_desc.SampleDesc.Count = 1;
        layer_desc.Usage = D3D11_USAGE_DEFAULT;
        layer_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        Hr(device_->CreateTexture2D(&layer_desc, nullptr, &layers),
           "CreateTexture2D(half-resolution layers)");
        D3D11_RENDER_TARGET_VIEW_DESC layer_view{};
        layer_view.Format = layer_desc.Format;
        layer_view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        layer_view.Texture2DArray.ArraySize = 1;
        layer_view.Texture2DArray.MipSlice = 0;
        ComPtr<ID3D11RenderTargetView> background_view, foreground_view, guide_view;
        layer_view.Texture2DArray.FirstArraySlice = 0;
        Hr(device_->CreateRenderTargetView(layers.Get(), &layer_view, &background_view),
           "CreateRenderTargetView(half background)");
        layer_view.Texture2DArray.FirstArraySlice = 1;
        Hr(device_->CreateRenderTargetView(layers.Get(), &layer_view, &foreground_view),
           "CreateRenderTargetView(half foreground)");
        layer_desc.ArraySize = 1;
        layer_desc.Format = DXGI_FORMAT_R16_FLOAT;
        Hr(device_->CreateTexture2D(&layer_desc, nullptr, &foreground_guide),
           "CreateTexture2D(half foreground guide)");
        Hr(device_->CreateRenderTargetView(foreground_guide.Get(), nullptr, &guide_view),
           "CreateRenderTargetView(half foreground guide)");
        ID3D11RenderTargetView* gather_targets[3] = {
            background_view.Get(), foreground_view.Get(), guide_view.Get()};
        ID3D11ShaderResourceView* gather_srvs[4] = {
            nullptr, scene_srv.Get(), filtered_color_srv.Get(), filtered_surface_srv.Get()};
        ID3D11Buffer* buffer = constants.Get();
        const D3D11_VIEWPORT half_viewport{0, 0, float(half_width), float(half_height), 0, 1};
        const D3D11_RECT half_scissor{0, 0, LONG(half_width), LONG(half_height)};
        context_->OMSetRenderTargets(3, gather_targets, nullptr);
        context_->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
        context_->RSSetState(raster_.Get());
        context_->RSSetViewports(1, &half_viewport);
        context_->RSSetScissorRects(1, &half_scissor);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vs_.Get(), nullptr, 0);
        context_->PSSetShader(GatherShader(), nullptr, 0);
        context_->PSSetShaderResources(0, 4, gather_srvs);
        context_->PSSetConstantBuffers(1, 1, &buffer);
        context_->Draw(3, 0);
        ID3D11ShaderResourceView* empty[4] = {};
        context_->PSSetShaderResources(0, 4, empty);
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        Hr(device_->CreateShaderResourceView(layers.Get(), nullptr, &layers_srv),
           "CreateShaderResourceView(half layers)");
        Hr(device_->CreateShaderResourceView(foreground_guide.Get(), nullptr,
                                             &foreground_guide_srv),
           "CreateShaderResourceView(half foreground guide)");

        ID3D11RenderTargetView* rtv = output_rtv.Get();
        ID3D11ShaderResourceView* srvs[4] = {
            filtered_surface_srv.Get(), scene_srv.Get(), layers_srv.Get(),
            foreground_guide_srv.Get()};
        ID3D11SamplerState* sampler = linear_.Get();
        const float blend_factor[4] = {};
        const D3D11_VIEWPORT viewport{0, 0, float(kWidth), float(kHeight), 0, 1};
        const D3D11_RECT scissor{rect.left, rect.top, rect.right, rect.bottom};
        context_->OMSetRenderTargets(1, &rtv, nullptr);
        context_->OMSetBlendState(rgb_only_.Get(), blend_factor, 0xffffffffu);
        context_->RSSetState(raster_.Get());
        context_->RSSetViewports(1, &viewport);
        context_->RSSetScissorRects(1, &scissor);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vs_.Get(), nullptr, 0);
        context_->PSSetShader(ComposeShader(), nullptr, 0);
        context_->PSSetShaderResources(0, 4, srvs);
        context_->PSSetConstantBuffers(1, 1, &buffer);
        context_->PSSetSamplers(0, 1, &sampler);
        context_->Draw(3, 0);
        context_->PSSetShaderResources(0, 4, empty);
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        if (samples > 1) {
            context_->ResolveSubresource(resolved_output.Get(), 0, output.Get(), 0,
                                         kColorFormat);
            context_->CopyResource(staging.Get(), resolved_output.Get());
        } else {
            context_->CopyResource(staging.Get(), output.Get());
        }

        D3D11_MAPPED_SUBRESOURCE map{};
        Hr(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map), "Map(output)");
        std::vector<Pixel> result(kWidth * kHeight);
        for (unsigned y = 0; y < kHeight; ++y)
            std::memcpy(result.data() + At(0, y),
                        static_cast<const unsigned char*>(map.pData) + size_t(y) * map.RowPitch,
                        kWidth * sizeof(Pixel));
        context_->Unmap(staging.Get(), 0);
        if (samples > 1)
            for (size_t i = 0; i < result.size(); ++i)
                Check(result[i].a == original_resolved_alpha[i],
                      "RGB-only DOF changed resolved output alpha");
        if (pipeline_ms)
            *pipeline_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - pipeline_started).count();
        return result;
    }

private:
    ComPtr<ID3DBlob> Compile(const char* entry, const char* profile, unsigned samples,
                            bool compose = false) {
        const std::string sample_count = std::to_string(samples);
        const D3D_SHADER_MACRO defines[] = {{"MS", "1"}, {"DOF_SAMPLES", sample_count.c_str()},
                                            {nullptr, nullptr}};
        const D3D_SHADER_MACRO compose_defines[] = {{"DOF_COMPOSE", "1"},
                                                     {nullptr, nullptr}};
        ComPtr<ID3DBlob> binary, errors;
        const auto started = std::chrono::steady_clock::now();
        HRESULT result = D3DCompile(gpu::kDepthOfFieldHlsl,
                                    std::strlen(gpu::kDepthOfFieldHlsl),
                                    "src/d3d/depth_of_field_shader.h",
                                    compose ? compose_defines : (samples > 1 ? defines : nullptr),
                                    nullptr, entry, profile,
                                    D3DCOMPILE_ENABLE_STRICTNESS, 0, &binary, &errors);
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        std::printf("COMPILE %s %ux%s: %.1f ms\n", entry, samples,
                    compose ? " compose" : "", ms);
        std::fflush(stdout);
        if (FAILED(result)) {
            std::string message = errors
                ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                              errors->GetBufferSize()) : "no compiler diagnostics";
            throw std::runtime_error(std::string("D3DCompile(") + entry + ", " +
                                     std::to_string(samples) + "x): " + message);
        }
        return binary;
    }

    ID3D11PixelShader* GatherShader() {
        if (!gather_shader_) {
            auto bytecode = Compile("depth_of_field_gather", "ps_5_0", 1);
            Hr(device_->CreatePixelShader(bytecode->GetBufferPointer(),
                                          bytecode->GetBufferSize(), nullptr, &gather_shader_),
               "CreatePixelShader(depth_of_field_gather)");
        }
        return gather_shader_.Get();
    }

    ID3D11PixelShader* ComposeShader() {
        if (!compose_shader_) {
            auto bytecode = Compile("depth_of_field", "ps_5_0", 1, true);
            Hr(device_->CreatePixelShader(bytecode->GetBufferPointer(),
                                          bytecode->GetBufferSize(), nullptr, &compose_shader_),
               "CreatePixelShader(depth_of_field compose)");
        }
        return compose_shader_.Get();
    }

    ID3D11PixelShader* PackShader(unsigned samples) {
        auto& shader = pack_shaders_[samples];
        if (!shader) {
            auto bytecode = Compile("depth_of_field_pack", "ps_5_0", samples);
            Hr(device_->CreatePixelShader(bytecode->GetBufferPointer(),
                                          bytecode->GetBufferSize(), nullptr, &shader),
               "CreatePixelShader(depth_of_field_pack)");
        }
        return shader.Get();
    }

    ID3D11PixelShader* DownsampleShader() {
        if (!downsample_shader_) {
            auto bytecode = Compile("depth_of_field_downsample", "ps_5_0", 1);
            Hr(device_->CreatePixelShader(bytecode->GetBufferPointer(),
                                          bytecode->GetBufferSize(), nullptr,
                                          &downsample_shader_),
               "CreatePixelShader(depth_of_field_downsample)");
        }
        return downsample_shader_.Get();
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11RasterizerState> raster_;
    ComPtr<ID3D11BlendState> rgb_only_;
    ComPtr<ID3D11SamplerState> linear_;
    ComPtr<ID3D11PixelShader> gather_shader_;
    ComPtr<ID3D11PixelShader> compose_shader_;
    ComPtr<ID3D11PixelShader> downsample_shader_;
    std::map<unsigned, ComPtr<ID3D11PixelShader>> pack_shaders_;
};

void Near(float actual, float expected, float tolerance, const char* message) {
    Check(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}

void Same(const std::vector<Pixel>& actual, const std::vector<Pixel>& expected,
          float tolerance, const char* message, float alpha_tolerance = 0) {
    for (size_t i = 0; i < actual.size(); ++i) {
        if (std::abs(actual[i].r - expected[i].r) > tolerance ||
            std::abs(actual[i].g - expected[i].g) > tolerance ||
            std::abs(actual[i].b - expected[i].b) > tolerance) {
            std::printf("METRIC mismatch %s at (%zu,%zu): actual RGB "
                        "%.6f/%.6f/%.6f expected %.6f/%.6f/%.6f tol %.6f\n",
                        message, i % kWidth, i / kWidth,
                        actual[i].r, actual[i].g, actual[i].b,
                        expected[i].r, expected[i].g, expected[i].b, tolerance);
            std::fflush(stdout);
        }
        Near(actual[i].r, expected[i].r, tolerance, message);
        Near(actual[i].g, expected[i].g, tolerance, message);
        Near(actual[i].b, expected[i].b, tolerance, message);
        Near(actual[i].a, expected[i].a, alpha_tolerance,
             "alpha changed despite RGB-only write mask");
    }
}

std::vector<Pixel> Pattern() {
    std::vector<Pixel> color(kWidth * kHeight);
    for (unsigned y = 0; y < kHeight; ++y)
        for (unsigned x = 0; x < kWidth; ++x)
            color[At(x, y)] = {float(x % 11) / 11, float(y % 13) / 13,
                               float((x + y) % 17) / 17, float((x + 2 * y) % 19) / 19};
    return color;
}

void TestIdentity(Warp& warp) {
    auto color = Pattern();
    std::vector<float> depth(kWidth * kHeight, RawDepth(4));
    const Rect whole{0, 0, LONG(kWidth), LONG(kHeight)};
    Same(warp.Render(color, depth, 0, whole), color, 0, "zero-strength DOF changed color");
    std::fill(depth.begin(), depth.end(), RawDepth(1));
    Same(warp.Render(color, depth, 8, whole), color, 0, "focus plane changed color");
    std::puts("PASS zero strength and focus plane identity, including alpha");
}

void TestUniformAndInvalid(Warp& warp) {
    std::vector<Pixel> color(kWidth * kHeight, {0.23f, 0.41f, 0.67f, 0.37f});
    std::vector<float> depth(kWidth * kHeight, RawDepth(4));
    const Rect whole{0, 0, LONG(kWidth), LONG(kHeight)};
    // RGB crosses two RGBA16F layer writes. At 0.67, their combined measured
    // roundoff can exceed one 0.000488 half-precision step.
    Same(warp.Render(color, depth, 8, whole), color, 1e-3f,
         "uniform color lost energy or alpha at image boundary");

    color = Pattern();
    depth[At(20, 20)] = -0.1f;
    depth[At(21, 20)] = 1.1f;
    depth[At(22, 20)] = NAN;
    auto output = warp.Render(color, depth, 8, whole);
    for (const auto& pixel : output)
        Check(std::isfinite(pixel.r) && std::isfinite(pixel.g) && std::isfinite(pixel.b),
              "invalid depth produced nonfinite color");
    for (unsigned x = 20; x <= 22; ++x)
        Near(output[At(x, 20)].r, color[At(x, 20)].r, 0,
             "invalid depth altered center pixel");
    std::fill(depth.begin(), depth.end(), 1.0f); // valid far-plane clear value
    output = warp.Render(color, depth, 8, whole);
    for (const auto& pixel : output)
        Check(std::isfinite(pixel.r) && std::isfinite(pixel.g) && std::isfinite(pixel.b),
              "far-plane clear depth produced nonfinite color");
    std::puts("PASS uniform energy, invalid depth, and far-plane finiteness");
}

void TestBlurAndEdges(Warp& warp) {
    const Rect whole{0, 0, LONG(kWidth), LONG(kHeight)};
    std::vector<Pixel> color(kWidth * kHeight, {0, 0, 0, 0.25f});
    std::vector<float> depth(kWidth * kHeight, RawDepth(4));
    color[At(32, 32)] = {1, 1, 1, 0.7f};
    auto output = warp.Render(color, depth, 8, whole);
    Check(output[At(32, 32)].r < 0.9f, "far highlight did not blur");
    bool spread = false;
    for (unsigned y = 29; y <= 35; ++y)
        for (unsigned x = 29; x <= 35; ++x)
            if ((x != 32 || y != 32) && output[At(x, y)].r > 0.002f) spread = true;
    Check(spread, "far highlight did not spread to nearby pixels");
    Near(output[At(32, 32)].a, 0.7f, 0, "blur changed source alpha");

    for (unsigned y = 0; y < kHeight; ++y)
        for (unsigned x = 0; x < kWidth; ++x) {
            const bool foreground = x < 32;
            color[At(x, y)] = foreground ? Pixel{1, 0, 0, 0.2f} : Pixel{0, 0, 1, 0.8f};
            depth[At(x, y)] = RawDepth(foreground ? 0.5f : 4.0f);
        }
    output = warp.Render(color, depth, 8, whole);
    Check(output[At(16, 32)].b < 0.01f,
          "far blue entered opaque foreground interior");
    Check(output[At(48, 32)].r < 0.01f,
          "near red spread into distant background interior");
    Check(output[At(31, 32)].b > 0.01f && output[At(31, 32)].b < 0.8f,
          "defocused near edge did not partially reveal background");
    Check(output[At(32, 32)].r > 0.01f && output[At(32, 32)].r < 0.8f,
          "defocused near edge did not partially cover background");
    Near(output[At(31, 32)].a, 0.2f, 0, "foreground alpha changed");
    Near(output[At(32, 32)].a, 0.8f, 0, "background alpha changed");
    std::puts("PASS far blur spread and partial near/far edge coverage");
}

void TestViewport(Warp& warp) {
    std::vector<Pixel> color(kWidth * kHeight, {0.8f, 0.1f, 0.9f, 0.6f});
    std::vector<float> depth(kWidth * kHeight, RawDepth(4));
    for (const Rect rect : {Rect{8, 8, 56, 56}, Rect{7, 9, 55, 58}}) {
        std::fill(color.begin(), color.end(), Pixel{0.8f, 0.1f, 0.9f, 0.6f});
        for (unsigned y = rect.top; y < unsigned(rect.bottom); ++y)
            for (unsigned x = rect.left; x < unsigned(rect.right); ++x)
                color[At(x, y)] = {0.1f, 0.7f, 0.2f, 0.3f};
        auto output = warp.Render(color, depth, 8, rect);
        for (unsigned y = 0; y < kHeight; ++y)
            for (unsigned x = 0; x < kWidth; ++x) {
                const bool inside = x >= unsigned(rect.left) && x < unsigned(rect.right) &&
                                    y >= unsigned(rect.top) && y < unsigned(rect.bottom);
                const float tolerance = inside ? 1e-3f : 0.0f;
                const Pixel& actual = output[At(x, y)];
                const Pixel& expected = color[At(x, y)];
                if (std::abs(actual.r - expected.r) > tolerance ||
                    std::abs(actual.g - expected.g) > tolerance ||
                    std::abs(actual.b - expected.b) > tolerance)
                    std::printf("METRIC viewport (%u,%u) rect (%ld,%ld,%ld,%ld) "
                                "actual RGB %.6f/%.6f/%.6f expected %.6f/%.6f/%.6f\n",
                                x, y, rect.left, rect.top, rect.right, rect.bottom,
                                actual.r, actual.g, actual.b,
                                expected.r, expected.g, expected.b);
                Near(actual.r, expected.r, tolerance, "viewport changed red or sampled outside");
                Near(actual.g, expected.g, tolerance, "viewport changed green or sampled outside");
                Near(actual.b, expected.b, tolerance, "viewport changed blue or sampled outside");
                Near(actual.a, expected.a, 0, "viewport changed alpha");
            }
    }
    // A split surface at the last odd-viewport row catches missing near
    // coverage when packed mips dilute the edge with invalid exterior texels.
    const Rect odd{7, 9, 55, 58};
    std::fill(color.begin(), color.end(), Pixel{0.8f, 0.1f, 0.9f, 0.6f});
    std::fill(depth.begin(), depth.end(), RawDepth(15));
    for (unsigned y = odd.top; y < unsigned(odd.bottom); ++y)
        for (unsigned x = odd.left; x < unsigned(odd.right); ++x) {
            const bool near_surface = x < 31;
            color[At(x, y)] = near_surface
                ? Pixel{0.9f, 0.1f, 0.05f, 0.3f}
                : Pixel{0.05f, 0.1f, 0.9f, 0.7f};
            depth[At(x, y)] = RawDepth(near_surface ? 3.0f : 15.0f);
        }
    const auto split = warp.Render(color, depth, 18, odd, 1, false, 5);
    WriteBmp("odd-viewport-split-before.bmp", color, kWidth, kHeight);
    WriteBmp("odd-viewport-split-after.bmp", split, kWidth, kHeight);
    CheckRangeAndAlpha(split, color);
    for (unsigned y : {9u, 10u, 56u, 57u}) {
        const float blue = split[At(15, y)].b;
        const float red = split[At(47, y)].r;
        std::printf("METRIC odd viewport edge y%u: near-interior blue %.5f, "
                    "far-interior red %.5f\n", y, blue, red);
        Check(blue < 0.15f && red < 0.15f,
              "odd viewport edge diluted opaque near/far interior");
    }
    for (unsigned y = 0; y < kHeight; ++y)
        for (unsigned x = 0; x < kWidth; ++x)
            if (x < unsigned(odd.left) || x >= unsigned(odd.right) ||
                y < unsigned(odd.top) || y >= unsigned(odd.bottom)) {
                const Pixel& a = split[At(x, y)];
                const Pixel& b = color[At(x, y)];
                Check(a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a,
                      "odd viewport split changed an outside pixel");
            }
    std::puts("PASS even/odd viewport bounds, inside energy and outside RGB identity");
}

void TestMsaa(Warp& warp) {
    const Rect whole{0, 0, LONG(kWidth), LONG(kHeight)};
    std::vector<Pixel> color(kWidth * kHeight, {0, 0, 0, 0.4f});
    std::vector<float> depth(kWidth * kHeight, RawDepth(4));
    color[At(32, 32)] = {1, 1, 1, 0.75f};
    for (unsigned samples : {2u, 4u, 8u}) {
        warp.CompileVariant(samples);
        if (!warp.SupportsMsaa(samples)) {
            std::printf("PASS %ux MSAA shader compile; WARP depth format unsupported\n", samples);
            continue;
        }
        auto output = warp.Render(color, depth, 8, whole, samples);
        Check(output[At(32, 32)].r < 0.9f, "MSAA depth variant did not blur highlight");
        Near(output[At(32, 32)].a, 0.75f, 0, "MSAA variant changed alpha");
        auto pattern = Pattern();
        output = warp.Render(pattern, depth, 8, whole, samples, true);
        // Resolve averages 8 float samples in a different order from the host.
        Same(output, pattern, 1e-6f,
             "MSAA minimum depth did not keep one focused sample sharp", 1e-6f);
        output = warp.Render(pattern, depth, 8, whole, samples, true, 1.0f, true);
        const float alpha_offset = 0.005f * (samples - 1) / 2;
        for (size_t i = 0; i < pattern.size(); ++i) {
            Near(output[i].r, pattern[i].r, 1e-5f, "MS output changed focused RGB");
            Near(output[i].g, pattern[i].g, 1e-5f, "MS output changed focused RGB");
            Near(output[i].b, pattern[i].b, 1e-5f, "MS output changed focused RGB");
            Near(output[i].a, pattern[i].a + alpha_offset, 1e-5f,
                 "RGB-only MS output changed per-sample alpha");
        }
        std::printf("PASS %ux MSAA depth path, blur, and per-sample alpha preservation\n",
                    samples);
    }
}

void TestMixedMsaaEdge(Warp& warp) {
    constexpr Rect whole{0, 0, LONG(kWidth), LONG(kHeight)};
    const Pixel near_color{0.9f, 0.1f, 0.05f, 0.3f};
    const Pixel far_color{0.05f, 0.1f, 0.9f, 0.7f};
    for (unsigned samples : {4u, 8u}) {
        if (!warp.SupportsMsaa(samples)) {
            std::printf("SKIP %ux mixed-color edge render: WARP format unsupported\n", samples);
            continue;
        }
        std::vector<Pixel> scene(kWidth * kHeight);
        std::vector<float> depth(kWidth * kHeight);
        for (unsigned y = 0; y < kHeight; ++y)
            for (unsigned x = 0; x < kWidth; ++x) {
                scene[At(x, y)] = x < 32 ? near_color : far_color;
                depth[At(x, y)] = RawDepth(x < 32 ? 3.0f : 15.0f);
                if (x == 32) {
                    const float far_weight = float(samples - 1);
                    const float inv_count = 1.0f / samples;
                    scene[At(x, y)] = {
                        (near_color.r + far_weight * far_color.r) * inv_count,
                        (near_color.g + far_weight * far_color.g) * inv_count,
                        (near_color.b + far_weight * far_color.b) * inv_count,
                        (near_color.a + far_weight * far_color.a) * inv_count};
                }
            }
        // The raw MS target/depth carry one red/near sample; all remaining
        // samples are blue/far. The separate resolved scene carries their mean.
        Pixel packed{};
        std::vector<Pixel> raw_samples;
        auto output = warp.Render(scene, depth, 18, whole, samples, false, 5,
                                  false, true, &packed, &raw_samples);
        for (unsigned sample = 0; sample < samples; ++sample)
            std::printf("METRIC mixed %ux raw sample %u: red %.5f blue %.5f "
                        "alpha %.5f depth %.7f\n", samples, sample,
                        raw_samples[sample].r, raw_samples[sample].g,
                        raw_samples[sample].b, raw_samples[sample].a);
        std::fflush(stdout);
        char before[64], after[64];
        std::snprintf(before, sizeof(before), "mixed-msaa-edge-%u-before.bmp", samples);
        std::snprintf(after, sizeof(after), "mixed-msaa-edge-%u-after.bmp", samples);
        WriteBmp(before, scene, kWidth, kHeight);
        WriteBmp(after, output, kWidth, kHeight);
        std::printf("METRIC mixed %ux packed mip0 RGB/coverage "
                    "%.5f/%.5f/%.5f/%.5f (expected coverage %.5f)\n",
                    samples, packed.r, packed.g, packed.b, packed.a,
                    1.0f / samples);
        std::fflush(stdout);
        Near(packed.a, 1.0f / samples, 0.001f,
             "MS packed mip0 lost per-sample foreground coverage");
        Near(packed.r, near_color.r, 0.001f,
             "MS packed mip0 mixed far blue into near red");
        Near(packed.g, near_color.g, 0.001f,
             "MS packed mip0 changed foreground green");
        Near(packed.b, near_color.b, 0.001f,
             "MS packed mip0 mixed far blue into near sample");
        for (size_t i = 0; i < output.size(); ++i) {
            const Pixel& p = output[i];
            Check(std::isfinite(p.r) && std::isfinite(p.g) && std::isfinite(p.b) &&
                  p.r >= -1e-5f && p.g >= -1e-5f && p.b >= -1e-5f &&
                  p.r <= 1.00001f && p.g <= 1.00001f && p.b <= 1.00001f,
                  "mixed MSAA edge produced nonfinite or overshooting RGB");
            Near(p.a, scene[i].a, 1e-6f, "mixed edge changed resolved per-sample alpha");
        }
        for (unsigned y = 4; y < kHeight - 4; ++y) {
            Near(output[At(32, y)].a, scene[At(32, y)].a, 1e-6f,
                 "mixed edge lost per-sample alpha on resolve");
            // The receiver at z=3 has a 7.5 px near CoC; x=16 is 16 px
            // inside the foreground and must retain its opaque interior.
            Check(output[At(16, y)].b < 0.15f && output[At(48, y)].r < 0.15f,
                  "mixed MSAA colors entered opaque near or distant far interior");
            Check(output[At(31, y)].b > 0.01f && output[At(33, y)].r > 0.01f,
                  "mixed MSAA edge lost partial near/far coverage");
        }
        std::printf("PASS mixed %ux color/depth edge, packed coverage %.3f, "
                    "foreground RGB %.3f/%.3f/%.3f\n", samples, packed.a,
                    packed.r, packed.g, packed.b);
    }
}

float CheckerContrast(const std::vector<Pixel>& pixels, unsigned first_x,
                      unsigned last_x) {
    float sum = 0;
    unsigned count = 0;
    for (unsigned y = 16; y < 30; ++y)
        for (unsigned x = first_x; x < last_x; ++x) {
            sum += std::abs(pixels[At(x, y)].r - pixels[At(x + 1, y)].r);
            ++count;
        }
    return sum / count;
}

float BroadbandDetail(const std::vector<Pixel>& pixels, unsigned first_x,
                      unsigned last_x) {
    float sum = 0;
    unsigned count = 0;
    for (unsigned y = 40; y < 56; ++y)
        for (unsigned x = first_x; x < last_x; ++x) {
            sum += std::abs(pixels[At(x, y)].r - pixels[At(x + 1, y)].r);
            sum += std::abs(pixels[At(x, y)].r - pixels[At(x, y + 1)].r);
            count += 2;
        }
    return sum / count;
}

void CheckRangeAndAlpha(const std::vector<Pixel>& output,
                        const std::vector<Pixel>& input) {
    for (size_t i = 0; i < output.size(); ++i) {
        const Pixel& p = output[i];
        Check(std::isfinite(p.r) && std::isfinite(p.g) && std::isfinite(p.b),
              "quality fixture produced nonfinite RGB");
        Check(p.r >= -1e-5f && p.g >= -1e-5f && p.b >= -1e-5f &&
              p.r <= 1.00001f && p.g <= 1.00001f && p.b <= 1.00001f,
              "quality fixture produced halo overshoot");
        Near(p.a, input[i].a, 0, "quality fixture changed alpha");
    }
}

void TestQualityFixtures(Warp& warp) {
    constexpr Rect whole{0, 0, LONG(kWidth), LONG(kHeight)};
    constexpr float focus = 5.0f;
    std::vector<std::vector<Pixel>> sheet;

    // Four planes carry both a single-frequency checker and a broad mixture of
    // fine/mid-size details. One checker frequency can land on an aperture MTF
    // zero, so use broadband attenuation for distance ordering.
    std::vector<Pixel> planes(kWidth * kHeight);
    std::vector<float> depth(kWidth * kHeight);
    constexpr float plane_units[4] = {3, 5, 8, 15};
    for (unsigned y = 0; y < kHeight; ++y)
        for (unsigned x = 0; x < kWidth; ++x) {
            const unsigned band = x / 16;
            const unsigned local_x = x % 16;
            const unsigned hash = (local_x * 1619u + y * 31337u +
                                   (local_x * y) * 6971u) * 1013u;
            const float tone = y < 32
                ? ((x + y) & 1 ? 0.85f : 0.15f)
                : ((hash ^ (hash >> 7)) & 3u ? 0.80f : 0.20f);
            planes[At(x, y)] = {tone, tone, tone, 0.2f + 0.1f * band};
            depth[At(x, y)] = RawDepth(plane_units[band]);
        }
    auto planes_after = warp.Render(planes, depth, 10, whole, 1, false, focus);
    WriteBmp("planes-before.bmp", planes, kWidth, kHeight);
    WriteBmp("planes-after.bmp", planes_after, kWidth, kHeight);
    CheckRangeAndAlpha(planes_after, planes);
    if (warp.SupportsMsaa(4)) {
        auto planes_ms = warp.Render(planes, depth, 10, whole, 4, false, focus);
        Same(planes_ms, planes_after, 2e-5f,
             "4x depth prepass changed multi-plane quality fixture");
    }
    const float near_contrast = CheckerContrast(planes_after, 4, 10);
    const float focus_contrast = CheckerContrast(planes_after, 20, 26);
    const float middle_contrast = CheckerContrast(planes_after, 36, 42);
    const float far_contrast = CheckerContrast(planes_after, 52, 58);
    std::printf("METRIC plane contrast near/focus/mid/far %.4f/%.4f/%.4f/%.4f\n",
                near_contrast, focus_contrast, middle_contrast, far_contrast);
    const float near_detail = BroadbandDetail(planes_after, 4, 10);
    const float focus_detail = BroadbandDetail(planes_after, 20, 26);
    const float middle_detail = BroadbandDetail(planes_after, 36, 42);
    const float far_detail = BroadbandDetail(planes_after, 52, 58);
    std::printf("METRIC broadband near/focus/mid/far %.4f/%.4f/%.4f/%.4f\n",
                near_detail, focus_detail, middle_detail, far_detail);
    std::fflush(stdout);
    Check(focus_contrast > 0.55f, "focus plane lost checker detail");
    Check(near_contrast < focus_contrast * 0.9f, "near plane as sharp as focus");
    Check(middle_contrast < focus_contrast * 0.9f, "middle plane as sharp as focus");
    Check(focus_detail > middle_detail * 1.5f,
          "focus plane did not retain more broadband detail than middle plane");
    Check(far_detail < middle_detail * 0.9f,
          "far plane did not soften broadband detail beyond middle plane");
    sheet.push_back(planes);
    sheet.push_back(planes_after);
    std::printf("PASS plane contrast near/focus/mid/far %.3f/%.3f/%.3f/%.3f\n",
                near_contrast, focus_contrast, middle_contrast, far_contrast);

    // A tiny highlight, a 2x2 highlight, and a small checkerboard patch expose
    // separated bright dots and hard sample footprints more readily than gradients.
    std::vector<Pixel> features(kWidth * kHeight, {0.02f, 0.02f, 0.02f, 0.4f});
    std::fill(depth.begin(), depth.end(), RawDepth(15));
    features[At(32, 32)] = {1, 1, 1, 0.75f};
    for (unsigned y = 15; y < 17; ++y)
        for (unsigned x = 15; x < 17; ++x)
            features[At(x, y)] = {1, 0.8f, 0.4f, 0.5f};
    for (unsigned y = 9; y < 19; ++y)
        for (unsigned x = 45; x < 55; ++x)
            if ((x + y) & 1) features[At(x, y)] = {0.9f, 0.9f, 0.9f, 0.4f};
    auto features_after = warp.Render(features, depth, 10, whole, 1, false, focus);
    WriteBmp("features-before.bmp", features, kWidth, kHeight);
    WriteBmp("features-after.bmp", features_after, kWidth, kHeight);
    CheckRangeAndAlpha(features_after, features);
    Check(features_after[At(32, 32)].r < 0.9f,
          "tiny far highlight stayed pin-sharp");
    bool feature_spread = false;
    for (unsigned y = 29; y < 36; ++y)
        for (unsigned x = 29; x < 36; ++x)
            if ((x != 32 || y != 32) && features_after[At(x, y)].r > 0.025f)
                feature_spread = true;
    Check(feature_spread, "tiny far highlight had no nearby bokeh spread");
    sheet.push_back(features);
    sheet.push_back(features_after);
    std::puts("PASS tiny highlights and checkerboard bounds/alpha");

    // A sloped foreground silhouette catches colored halos at the depth edge.
    std::vector<Pixel> slope(kWidth * kHeight);
    for (unsigned y = 0; y < kHeight; ++y)
        for (unsigned x = 0; x < kWidth; ++x) {
            const bool foreground = x < 16 + y / 3;
            slope[At(x, y)] = foreground
                ? Pixel{0.9f, 0.1f, 0.05f, 0.3f}
                : Pixel{0.05f, 0.1f, 0.9f, 0.7f};
            depth[At(x, y)] = RawDepth(foreground ? 3.0f : 15.0f);
        }
    auto slope_after = warp.Render(slope, depth, 10, whole, 1, false, focus);
    WriteBmp("slope-before.bmp", slope, kWidth, kHeight);
    WriteBmp("slope-after.bmp", slope_after, kWidth, kHeight);
    CheckRangeAndAlpha(slope_after, slope);
    for (unsigned y : {20u, 32u, 44u}) {
        const unsigned boundary = 16 + y / 3;
        Check(slope_after[At(boundary - 12, y)].b < 0.15f,
              "far blue entered broad near foreground interior");
        Check(slope_after[At(boundary + 12, y)].r < 0.15f,
              "near red spread too far into background interior");
        Check(slope_after[At(boundary - 1, y)].b > 0.01f,
              "sloped defocused near edge stayed hard opaque");
        Check(slope_after[At(boundary + 1, y)].r > 0.05f,
              "sloped near silhouette did not cover adjacent background");
    }
    sheet.push_back(slope);
    sheet.push_back(slope_after);
    std::puts("PASS sloped near/far edge without colored halo");

    // A small radius step should change the image gently, without whole pixels
    // popping on or off as tap positions cross an integer coordinate.
    std::vector<Pixel> fine(kWidth * kHeight);
    std::fill(depth.begin(), depth.end(), RawDepth(15));
    for (unsigned y = 0; y < kHeight; ++y)
        for (unsigned x = 0; x < kWidth; ++x) {
            const float tone = (x + y) & 1 ? 0.9f : 0.1f;
            fine[At(x, y)] = {tone, tone, tone, 0.35f};
        }
    auto radius_4 = warp.Render(fine, depth, 4.0f, whole, 1, false, focus);
    auto radius_4_1 = warp.Render(fine, depth, 4.1f, whole, 1, false, focus);
    WriteBmp("fractional-before.bmp", fine, kWidth, kHeight);
    WriteBmp("fractional-radius-4.0.bmp", radius_4, kWidth, kHeight);
    WriteBmp("fractional-radius-4.1.bmp", radius_4_1, kWidth, kHeight);
    CheckRangeAndAlpha(radius_4, fine);
    CheckRangeAndAlpha(radius_4_1, fine);
    float mean_step = 0;
    float largest_step = 0;
    for (size_t i = 0; i < fine.size(); ++i) {
        const float step = std::abs(radius_4[i].r - radius_4_1[i].r);
        mean_step += step;
        largest_step = std::max(largest_step, step);
    }
    mean_step /= fine.size();
    Check(mean_step < 0.04f && largest_step < 0.16f,
          "fractional blur radius caused abrupt bright-pixel pop");
    sheet.push_back(radius_4);
    sheet.push_back(radius_4_1);
    WriteContactSheet(sheet, 2);
    const std::filesystem::path manifest =
        std::filesystem::path("out") / "tests" / "dof-tests" / "fixtures" / "manifest.txt";
    std::ofstream names(manifest);
    names << "contact-sheet.bmp (4x enlarged, left/right pairs)\n"
             "row 1: 3/5/8/15-unit checker planes, before/after radius 10 focus 5 units\n"
             "row 2: tiny highlights and checkerboard, before/after radius 10 focus 5 units\n"
             "row 3: sloped foreground edge, before/after radius 10 focus 5 units\n"
             "row 4: far checkerboard, radius 4.0/radius 4.1; source in fractional-before.bmp\n";
    names.flush();
    Check(bool(names), "cannot write fixture manifest");
    std::printf("PASS fractional-radius continuity mean %.4f peak %.4f; BMP contact sheet saved\n",
                mean_step, largest_step);
}

void TestContinuousDepthTexture(Warp& warp) {
    constexpr Rect whole{0, 0, LONG(kWidth), LONG(kHeight)};
    std::vector<Pixel> stone(kWidth * kHeight);
    for (unsigned y = 0; y < kHeight; ++y)
        for (unsigned x = 0; x < kWidth; ++x) {
            unsigned hash = (x * 1973u + y * 9277u + x * y * 26699u) * 1013u;
            hash ^= hash >> 11;
            const float grain = float(hash & 255u) / 255.0f;
            const float low = 0.08f * std::sin(0.28f * x) * std::sin(0.23f * y);
            const float tone = 0.24f + 0.48f * grain + low;
            stone[At(x, y)] = {tone, tone * 0.9f, tone * 0.75f, 0.2f + 0.6f * grain};
        }
    std::vector<std::vector<Pixel>> sheet;
    for (const bool near_plane : {true, false})
        for (const bool curved : {false, true}) {
            // Inverse view depth is affine across a projected planar surface.
            // The curved variant adds a smooth quadratic bend, without a depth edge.
            const float base_inv_z = near_plane ? 1.0f / 2.5f : 1.0f / 15.0f;
            const float slope_x = near_plane ? 0.0030f : 0.0008f;
            const float slope_y = near_plane ? 0.0015f : 0.0004f;
            const float curvature = near_plane ? 0.000020f : 0.000008f;
            std::vector<float> depth(kWidth * kHeight);
            for (unsigned y = 0; y < kHeight; ++y)
                for (unsigned x = 0; x < kWidth; ++x) {
                    const float dx = float(x) - 31.5f;
                    const float dy = float(y) - 31.5f;
                    const float inv_z = base_inv_z + slope_x * dx +
                        (curved ? slope_y * dy + curvature * (dx * dx - dy * dy) : 0);
                    Check(inv_z > 0, "continuous-depth fixture has invalid inverse depth");
                    depth[At(x, y)] = RawDepth(1.0f / inv_z);
                }
            auto output = warp.Render(stone, depth, 18, whole, 1, false, 5);
            const char* plane = near_plane ? "near" : "far";
            const char* shape = curved ? "curved" : "slope";
            char before[80], after[80];
            std::snprintf(before, sizeof(before), "continuous-%s-%s-before.bmp", plane, shape);
            std::snprintf(after, sizeof(after), "continuous-%s-%s-after.bmp", plane, shape);
            WriteBmp(before, stone, kWidth, kHeight);
            WriteBmp(after, output, kWidth, kHeight);
            CheckRangeAndAlpha(output, stone);
            sheet.push_back(stone);
            sheet.push_back(output);

            float worst_ratio = 0;
            float best_ratio = 1e9f;
            unsigned unchanged = 0;
            for (unsigned tile_y = 0; tile_y < 2; ++tile_y)
                for (unsigned tile_x = 0; tile_x < 2; ++tile_x) {
                    const unsigned x0 = 16 + 16 * tile_x;
                    const unsigned y0 = 16 + 16 * tile_y;
                    float input_highpass = 0, output_highpass = 0;
                    for (unsigned y = y0; y < y0 + 15; ++y)
                        for (unsigned x = x0; x < x0 + 15; ++x) {
                            const size_t i = At(x, y);
                            input_highpass +=
                                std::abs(stone[i].r - stone[At(x + 1, y)].r) +
                                std::abs(stone[i].r - stone[At(x, y + 1)].r);
                            output_highpass +=
                                std::abs(output[i].r - output[At(x + 1, y)].r) +
                                std::abs(output[i].r - output[At(x, y + 1)].r);
                            if (std::abs(output[i].r - stone[i].r) < 0.005f) ++unchanged;
                        }
                    Check(input_highpass > 0, "stone fixture lacks high-frequency texture");
                    const float ratio = output_highpass / input_highpass;
                    best_ratio = std::min(best_ratio, ratio);
                    worst_ratio = std::max(worst_ratio, ratio);
                }
            const float unchanged_fraction = float(unchanged) / (4 * 15 * 15);
            std::printf("METRIC continuous %s %s: highpass tile ratio %.3f..%.3f, "
                        "unchanged %.3f\n", plane, shape, best_ratio,
                        worst_ratio, unchanged_fraction);
            std::fflush(stdout);
            Check(worst_ratio < 0.55f,
                  "continuous depth retained sharp textured patch");
            Check(unchanged_fraction < 0.05f,
                  "continuous depth fell back to original sharp texture");
        }
    WriteContactSheet(sheet, 2, "contact-sheet-continuous-depth.bmp");
    std::puts("PASS continuous sloped/curved near/far texture blur without sharp patches");
}

void TestFarStripeParity(Warp& warp) {
    constexpr Rect whole{0, 0, LONG(kWidth), LONG(kHeight)};
    std::vector<std::vector<Pixel>> sheet;
    for (unsigned stripe_x : {32u, 33u}) {
        std::vector<Pixel> scene(kWidth * kHeight, {0.25f, 0.25f, 0.25f, 0.4f});
        std::vector<float> depth(kWidth * kHeight, RawDepth(90));
        for (unsigned y = 0; y < kHeight; ++y) {
            const float tone = y & 1 ? 0.8f : 0.2f;
            scene[At(stripe_x, y)] = {tone, tone, tone, 0.7f};
            depth[At(stripe_x, y)] = RawDepth(10);
        }
        auto output = warp.Render(scene, depth, 18, whole, 1, false, 5);
        char before[72], after[72];
        std::snprintf(before, sizeof(before), "far-stripe-x%u-before.bmp", stripe_x);
        std::snprintf(after, sizeof(after), "far-stripe-x%u-after.bmp", stripe_x);
        WriteBmp(before, scene, kWidth, kHeight);
        WriteBmp(after, output, kWidth, kHeight);
        sheet.push_back(scene);
        sheet.push_back(output);
        CheckRangeAndAlpha(output, scene);
        float contrast = 0;
        unsigned near_original = 0;
        for (unsigned y = 16; y < 48; ++y) {
            contrast += std::abs(output[At(stripe_x, y)].r -
                                 output[At(stripe_x, y + 1)].r);
            near_original += std::abs(output[At(stripe_x, y)].r -
                                      scene[At(stripe_x, y)].r) < 0.005f;
        }
        contrast /= 32;
        std::printf("METRIC far textured 1px stripe x%u: vertical contrast %.4f "
                    "(input 0.6000), unchanged %u/32\n",
                    stripe_x, contrast, near_original);
        std::fflush(stdout);
        Check(contrast < 0.3f,
              "far textured stripe retained sharp contrast at half-resolution parity");
        Check(near_original < 4,
              "far textured stripe kept original sharp pixels at half-resolution parity");
    }
    WriteContactSheet(sheet, 2, "contact-sheet-far-stripe-parity.bmp");
    std::puts("PASS even/odd far textured stripe contrast softened");
}

void TestLiveTuning(Warp& warp) {
    constexpr Rect whole{0, 0, LONG(kWidth), LONG(kHeight)};
    const auto texture = Pattern();
    std::vector<float> depth(kWidth * kHeight, RawDepth(3));
    Same(warp.Render(texture, depth, 18, whole, 1, false, 5, false, false,
                     nullptr, nullptr, nullptr, 0, 0.45f), texture, 0,
         "zero near tuning changed a uniformly near scene");
    std::fill(depth.begin(), depth.end(), RawDepth(15));
    Same(warp.Render(texture, depth, 18, whole, 1, false, 5, false, false,
                     nullptr, nullptr, nullptr, 1, 0), texture, 0,
         "zero far tuning changed a uniformly far scene");
    std::fill(depth.begin(), depth.end(), RawDepth(10));
    Same(warp.Render(texture, depth, 18, whole, 1, false, 10), texture, 0,
         "live focus distance did not keep its focus plane sharp");

    std::fill(depth.begin(), depth.end(), RawDepth(15));
    const auto gentler = warp.Render(texture, depth, 18, whole, 1, false, 5);
    const auto stronger = warp.Render(texture, depth, 18, whole, 1, false, 5,
                                      false, false, nullptr, nullptr, nullptr, 1, 1);
    auto detail = [](const std::vector<Pixel>& image) {
        float total = 0;
        for (unsigned y = 16; y < 47; ++y)
            for (unsigned x = 16; x < 47; ++x)
                total += std::abs(image[At(x, y)].r - image[At(x + 1, y)].r) +
                         std::abs(image[At(x, y)].r - image[At(x, y + 1)].r);
        return total / (31 * 31 * 2);
    };
    const float gentle_detail = detail(gentler);
    const float strong_detail = detail(stronger);
    std::printf("METRIC far tuning .45 vs 1.00: broadband detail %.4f vs %.4f\n",
                gentle_detail, strong_detail);
    std::fflush(stdout);
    WriteBmp("far-tuning-045.bmp", gentler, kWidth, kHeight);
    WriteBmp("far-tuning-100.bmp", stronger, kWidth, kHeight);
    Check(strong_detail < gentle_detail * 0.9f,
          "far strength control did not soften detail when increased");
    std::puts("PASS live near/far zero, focus, and far softness tuning");
}

void TestMaxRadiusDiagnostics(Warp& warp) {
    constexpr Rect whole{0, 0, LONG(kWidth), LONG(kHeight)};
    constexpr float focus = 5.0f;
    constexpr float far_depth = 90.0f;
    std::vector<float> depth(kWidth * kHeight, RawDepth(far_depth));
    std::vector<Pixel> point(kWidth * kHeight, {0.01f, 0.01f, 0.01f, 0.5f});
    point[At(32, 32)] = {1, 1, 1, 0.7f};
    std::vector<std::vector<Pixel>> sheet;
    std::vector<std::vector<Pixel>> exposed_sheet;
    for (unsigned radius : {18u, 24u, 36u}) {
        auto output = warp.Render(point, depth, float(radius), whole, 1, false, focus);
        char before[64], after[64];
        std::snprintf(before, sizeof(before), "max-radius-%u-before.bmp", radius);
        std::snprintf(after, sizeof(after), "max-radius-%u-after.bmp", radius);
        WriteBmp(before, point, kWidth, kHeight);
        WriteBmp(after, output, kWidth, kHeight);
        sheet.push_back(point);
        sheet.push_back(output);
        CheckRangeAndAlpha(output, point);
        const float effective = radius * (far_depth - focus - focus * 0.15f) / far_depth;
        float peak = 0;
        for (const Pixel& pixel : output) peak = std::max(peak, pixel.r - 0.01f);
        Check(peak > 0.0001f, "far highlight disappeared at maximum radius");
        std::vector<Pixel> exposed = output;
        for (Pixel& pixel : exposed) {
            const float value = std::clamp((pixel.r - 0.01f) / peak, 0.0f, 1.0f);
            pixel = {value, value, value, 1};
        }
        exposed_sheet.push_back(point);
        exposed_sheet.push_back(exposed);
        unsigned disc_pixels = 0, lit_pixels = 0;
        float disc_energy = 0;
        for (unsigned y = 0; y < kHeight; ++y)
            for (unsigned x = 0; x < kWidth; ++x) {
                const float distance = std::hypot(float(x) - 32, float(y) - 32);
                if (distance > std::min(effective, 30.0f)) continue;
                const float signal = std::max(output[At(x, y)].r - 0.01f, 0.0f);
                ++disc_pixels;
                disc_energy += signal;
                if (signal > peak * 0.2f && signal > 0.0001f) ++lit_pixels;
            }
        std::printf("METRIC %u-px far point: peak %.5f, disc energy %.5f, "
                    "20%%-peak coverage %u/%u (%.3f)\n", radius, peak, disc_energy,
                    lit_pixels, disc_pixels,
                    disc_pixels ? float(lit_pixels) / disc_pixels : 0);
        std::fflush(stdout);
    }

    // A two-pixel diagonal foreground line tests coverage at a large aperture.
    std::vector<Pixel> line(kWidth * kHeight, {0.04f, 0.04f, 0.75f, 0.6f});
    for (unsigned y = 0; y < kHeight; ++y)
        for (unsigned x = 0; x < kWidth; ++x) {
            const int centre = 12 + int(y) / 2;
            if (std::abs(int(x) - centre) <= 1) {
                line[At(x, y)] = {0.9f, 0.08f, 0.04f, 0.3f};
                depth[At(x, y)] = RawDepth(3);
            }
        }
    auto line_after = warp.Render(line, depth, 24, whole, 1, false, focus);
    WriteBmp("max-radius-line-before.bmp", line, kWidth, kHeight);
    WriteBmp("max-radius-line-after.bmp", line_after, kWidth, kHeight);
    sheet.push_back(line);
    sheet.push_back(line_after);
    exposed_sheet.push_back(line);
    exposed_sheet.push_back(line_after);
    CheckRangeAndAlpha(line_after, line);
    float thick_integrated_near = 0;
    unsigned thick_row_width = 0;
    for (unsigned y = 0; y < kHeight; ++y)
        for (unsigned x = 0; x < kWidth; ++x) {
            const float near_fraction = std::clamp(
                (line_after[At(x, y)].r - 0.04f) / 0.86f, 0.0f, 1.0f);
            thick_integrated_near += near_fraction;
            if (y == 32 && near_fraction > 0.2f) ++thick_row_width;
        }
    std::printf("METRIC 24-px 3-pixel near line: integrated near %.2f / 192 source "
                "(%.2fx), y32 width >20%% near %u px\n",
                thick_integrated_near, thick_integrated_near / 192,
                thick_row_width);

    // One-pixel near geometry is the difficult case for depth-aware mips: a
    // nearest-surface texel may become wider than the actual source geometry.
    for (unsigned radius : {24u, 36u}) {
        std::fill(depth.begin(), depth.end(), RawDepth(far_depth));
        std::fill(line.begin(), line.end(), Pixel{0.04f, 0.04f, 0.75f, 0.6f});
        unsigned source_pixels = 0;
        for (unsigned y = 0; y < kHeight; ++y) {
            const unsigned x = 12 + y / 2;
            line[At(x, y)] = {0.9f, 0.08f, 0.04f, 0.3f};
            depth[At(x, y)] = RawDepth(3);
            ++source_pixels;
        }
        auto thin_after = warp.Render(line, depth, float(radius), whole, 1, false, focus);
        char before[64], after[64];
        std::snprintf(before, sizeof(before), "thin-line-%u-before.bmp", radius);
        std::snprintf(after, sizeof(after), "thin-line-%u-after.bmp", radius);
        WriteBmp(before, line, kWidth, kHeight);
        WriteBmp(after, thin_after, kWidth, kHeight);
        sheet.push_back(line);
        sheet.push_back(thin_after);
        exposed_sheet.push_back(line);
        exposed_sheet.push_back(thin_after);
        CheckRangeAndAlpha(thin_after, line);
        float integrated_near = 0;
        unsigned row_width = 0;
        for (unsigned y = 0; y < kHeight; ++y)
            for (unsigned x = 0; x < kWidth; ++x) {
                const float near_fraction = std::clamp(
                    (thin_after[At(x, y)].r - 0.04f) / 0.86f, 0.0f, 1.0f);
                integrated_near += near_fraction;
                if (y == 32 && near_fraction > 0.2f) ++row_width;
            }
        std::printf("METRIC %u-px thin near line: integrated near %.2f / %u source "
                    "(%.2fx), y32 width >20%% near %u px\n", radius,
                    integrated_near, source_pixels,
                    integrated_near / source_pixels, row_width);
        std::fflush(stdout);
    }

    // A centered one-pixel vertical stripe avoids the sampling phase and
    // staircase ambiguity of the diagonal. For normalized aperture blur, the
    // foreground fraction integrated across a center row stays near one.
    // The near CoC is about 10/15 px at these radii, well inside 64 px.
    for (unsigned radius : {24u, 36u}) {
        std::fill(depth.begin(), depth.end(), RawDepth(far_depth));
        std::fill(line.begin(), line.end(), Pixel{0.04f, 0.04f, 0.75f, 0.6f});
        for (unsigned y = 0; y < kHeight; ++y) {
            line[At(32, y)] = {0.9f, 0.08f, 0.04f, 0.3f};
            depth[At(32, y)] = RawDepth(3);
        }
        auto stripe_after = warp.Render(line, depth, float(radius), whole, 1, false, focus);
        char before[64], after[64];
        std::snprintf(before, sizeof(before), "thin-stripe-%u-before.bmp", radius);
        std::snprintf(after, sizeof(after), "thin-stripe-%u-after.bmp", radius);
        WriteBmp(before, line, kWidth, kHeight);
        WriteBmp(after, stripe_after, kWidth, kHeight);
        sheet.push_back(line);
        sheet.push_back(stripe_after);
        exposed_sheet.push_back(line);
        exposed_sheet.push_back(stripe_after);
        CheckRangeAndAlpha(stripe_after, line);
        float row_integral = 0;
        float unclamped_integral = 0;
        float peak = 0;
        for (unsigned y = 30; y <= 33; ++y) {
            float sum = 0;
            float unclamped_sum = 0;
            for (unsigned x = 0; x < kWidth; ++x) {
                const Pixel& p = stripe_after[At(x, y)];
                const float raw_a = (p.r - 0.04f) / 0.86f;
                const float a = std::clamp(raw_a, 0.0f, 1.0f);
                sum += a;
                unclamped_sum += raw_a;
                peak = std::max(peak, a);
                Near(p.g, 0.04f + 0.04f * a, 0.025f,
                     "thin-stripe green deviated from near/far color mixture");
                Near(p.b, 0.75f - 0.71f * a, 0.025f,
                     "thin-stripe blue deviated from near/far color mixture");
            }
            row_integral += sum / 4;
            unclamped_integral += unclamped_sum / 4;
        }
        std::printf("METRIC %u-px vertical thin stripe: center-row near integral "
                    "%.3f (unclamped %.3f), peak near fraction %.3f\n",
                    radius, row_integral, unclamped_integral, peak);
        std::fflush(stdout);
        Check(row_integral >= 0.75f && row_integral <= 1.25f,
              "thin near stripe gained or lost foreground energy");
        Check(peak <= 0.25f, "defocused one-pixel foreground stripe stayed opaque");
    }
    WriteContactSheet(sheet, 2, "contact-sheet-max-radius.bmp");
    WriteContactSheet(exposed_sheet, 2, "contact-sheet-max-radius-exposed.bmp");
    const std::filesystem::path manifest =
        std::filesystem::path("out") / "tests" / "dof-tests" / "fixtures" /
        "max-radius-manifest.txt";
    std::ofstream names(manifest);
    names << "contact-sheet-max-radius.bmp: native RGB, 4x pixel enlargement\n"
             "contact-sheet-max-radius-exposed.bmp: far-point output rows normalized "
             "to each peak for shape inspection only\n"
             "rows 1-3: 1-pixel far point, radius 18/24/36, before/after\n"
             "row 4: 3-pixel near diagonal, radius 24, before/after\n"
             "rows 5-6: 1-pixel near diagonal, radius 24/36, before/after\n"
             "rows 7-8: centered 1-pixel near vertical stripe, radius 24/36\n"
             "All geometric distances and focus values are world units.\n";
    names.flush();
    Check(bool(names), "cannot write max-radius fixture manifest");
    std::puts("PASS max-radius finite/alpha diagnostics; BMP contact sheet saved");
}

void TestWarpTiming(Warp& warp) {
    constexpr Rect whole{0, 0, LONG(kWidth), LONG(kHeight)};
    const auto color = Pattern();
    std::vector<float> depth(kWidth * kHeight);
    for (const float world_depth : {15.0f, 5.0f}) {
        std::fill(depth.begin(), depth.end(), RawDepth(world_depth));
        double warmup = 0;
        warp.Render(color, depth, 18, whole, 1, false, 5, false, false,
                    nullptr, nullptr, &warmup);
        std::array<double, 3> samples{};
        for (double& ms : samples)
            warp.Render(color, depth, 18, whole, 1, false, 5, false, false,
                        nullptr, nullptr, &ms);
        std::sort(samples.begin(), samples.end());
        std::printf("METRIC WARP64 full pipeline depth %.0f radius 18 "
                    "warmed median %.3f ms (samples %.3f, %.3f, %.3f)\n",
                    world_depth, samples[1], samples[0], samples[1], samples[2]);
        std::fflush(stdout);
    }
}

} // namespace

int main() {
    try {
        Warp warp;
        std::puts("RUN identity"); std::fflush(stdout);
        TestIdentity(warp);
        std::puts("RUN uniform and invalid depth"); std::fflush(stdout);
        TestUniformAndInvalid(warp);
        std::puts("RUN blur and edges"); std::fflush(stdout);
        TestBlurAndEdges(warp);
        std::puts("RUN viewport"); std::fflush(stdout);
        TestViewport(warp);
        std::puts("RUN MSAA depth path"); std::fflush(stdout);
        TestMsaa(warp);
        std::puts("RUN mixed MSAA edge"); std::fflush(stdout);
        TestMixedMsaaEdge(warp);
        std::puts("RUN max-radius diagnostics"); std::fflush(stdout);
        TestMaxRadiusDiagnostics(warp);
        std::puts("RUN quality fixtures"); std::fflush(stdout);
        TestQualityFixtures(warp);
        std::puts("RUN continuous-depth stone texture"); std::fflush(stdout);
        TestContinuousDepthTexture(warp);
        std::puts("RUN even/odd far textured stripe"); std::fflush(stdout);
        TestFarStripeParity(warp);
        std::puts("RUN live near/far/focus tuning"); std::fflush(stdout);
        TestLiveTuning(warp);
        std::puts("RUN WARP pipeline timing (software, not game FPS)"); std::fflush(stdout);
        TestWarpTiming(warp);
        std::puts("PASS depth-of-field WARP checks");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL depth-of-field WARP checks: %s\n", error.what());
        return 1;
    }
}
