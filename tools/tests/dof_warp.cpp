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
#include <cmath>
#include <cstdio>
#include <cstring>
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

// A conventional 0.1 m near / 10 m far projection: viewZ = x / (rawDepth - y).
constexpr float kProjectionX = -0.101010101f;
constexpr float kProjectionY = 1.01010101f;
float RawDepth(float metres) { return kProjectionY + kProjectionX / metres; }

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
    }

    bool SupportsMsaa(unsigned samples) const {
        UINT levels = 0;
        Hr(device_->CheckMultisampleQualityLevels(kDepthFormat, samples, &levels),
           "CheckMultisampleQualityLevels");
        return levels > 0;
    }

    void CompileVariant(unsigned samples) { PixelShader(samples); }

    std::vector<Pixel> Render(const std::vector<Pixel>& color,
                              const std::vector<float>& raw_depth, float radius,
                              Rect rect, unsigned samples = 1,
                              bool sample_zero_focused = false) {
        Check(color.size() == kWidth * kHeight, "bad color fixture size");
        Check(raw_depth.size() == kWidth * kHeight, "bad depth fixture size");
        Check(rect.left >= 0 && rect.top >= 0 && rect.right <= LONG(kWidth) &&
              rect.bottom <= LONG(kHeight) && rect.right > rect.left &&
              rect.bottom > rect.top, "bad viewport fixture");
        Check(samples == 1 || SupportsMsaa(samples), "requested MSAA format unsupported");

        ComPtr<ID3D11Texture2D> scene, depth, output, staging;
        ComPtr<ID3D11ShaderResourceView> scene_srv, depth_srv;
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

        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        Hr(device_->CreateTexture2D(&td, &pixels, &output), "CreateTexture2D(output)");
        Hr(device_->CreateRenderTargetView(output.Get(), nullptr, &output_rtv),
           "CreateRenderTargetView(output)");

        td.BindFlags = 0;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Hr(device_->CreateTexture2D(&td, nullptr, &staging), "CreateTexture2D(staging)");

        const float cb[8] = {kProjectionX, kProjectionY, 1.0f, radius,
                             float(rect.left), float(rect.top),
                             float(rect.right - rect.left), float(rect.bottom - rect.top)};
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(cb);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA cb_data{cb, 0, 0};
        Hr(device_->CreateBuffer(&bd, &cb_data, &constants), "CreateBuffer(Dof)");

        td.Format = kDepthFormat;
        td.SampleDesc.Count = samples;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.CPUAccessFlags = 0;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (samples > 1 ? D3D11_BIND_RENDER_TARGET : 0);
        D3D11_SUBRESOURCE_DATA depths{raw_depth.data(), UINT(kWidth * sizeof(float)), 0};
        Hr(device_->CreateTexture2D(&td, samples == 1 ? &depths : nullptr, &depth),
           "CreateTexture2D(depth)");
        if (samples > 1) {
            // The MS fixture uses one uniform depth in every sample. The pixel shader
            // still executes its real Texture2DMS minimum-depth path.
            Check(raw_depth.front() == raw_depth.back(), "MS fixture must have uniform depth");
            ComPtr<ID3D11RenderTargetView> depth_rtv;
            Hr(device_->CreateRenderTargetView(depth.Get(), nullptr, &depth_rtv),
               "CreateRenderTargetView(MS depth)");
            if (sample_zero_focused) {
                // SV_SampleIndex forces per-sample invocations. A focused sample
                // mixed with far samples exercises the production minimum gather.
                constexpr char fixture[] = R"(
                    cbuffer B : register(b1) { float4 zp; };
                    float sample_depth(uint sample : SV_SampleIndex) : SV_Target {
                        return zp.y + zp.x / (sample == 0 ? 1.0 : 4.0);
                    })";
                ComPtr<ID3DBlob> binary, errors;
                HRESULT hr = D3DCompile(fixture, sizeof(fixture) - 1, "sample fixture",
                                        nullptr, nullptr, "sample_depth", "ps_5_0",
                                        D3DCOMPILE_ENABLE_STRICTNESS, 0, &binary, &errors);
                if (FAILED(hr))
                    throw std::runtime_error(errors
                        ? static_cast<const char*>(errors->GetBufferPointer())
                        : "sample fixture did not compile");
                ComPtr<ID3D11PixelShader> fixture_ps;
                Hr(device_->CreatePixelShader(binary->GetBufferPointer(), binary->GetBufferSize(),
                                              nullptr, &fixture_ps),
                   "CreatePixelShader(sample fixture)");
                ID3D11RenderTargetView* target = depth_rtv.Get();
                ID3D11Buffer* buffer = constants.Get();
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
                context_->PSSetConstantBuffers(1, 1, &buffer);
                context_->Draw(3, 0);
                context_->OMSetRenderTargets(0, nullptr, nullptr);
            } else {
                const float clear[4] = {raw_depth.front(), 0, 0, 0};
                context_->ClearRenderTargetView(depth_rtv.Get(), clear);
            }
        }
        Hr(device_->CreateShaderResourceView(depth.Get(), nullptr, &depth_srv),
           "CreateShaderResourceView(depth)");

        ID3D11RenderTargetView* rtv = output_rtv.Get();
        ID3D11ShaderResourceView* srvs[2] = {depth_srv.Get(), scene_srv.Get()};
        ID3D11Buffer* buffer = constants.Get();
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
        context_->PSSetShader(PixelShader(samples), nullptr, 0);
        context_->PSSetShaderResources(0, 2, srvs);
        context_->PSSetConstantBuffers(1, 1, &buffer);
        context_->Draw(3, 0);
        ID3D11ShaderResourceView* empty[2] = {};
        context_->PSSetShaderResources(0, 2, empty);
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        context_->CopyResource(staging.Get(), output.Get());

        D3D11_MAPPED_SUBRESOURCE map{};
        Hr(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map), "Map(output)");
        std::vector<Pixel> result(kWidth * kHeight);
        for (unsigned y = 0; y < kHeight; ++y)
            std::memcpy(result.data() + At(0, y),
                        static_cast<const unsigned char*>(map.pData) + size_t(y) * map.RowPitch,
                        kWidth * sizeof(Pixel));
        context_->Unmap(staging.Get(), 0);
        return result;
    }

private:
    ComPtr<ID3DBlob> Compile(const char* entry, const char* profile, unsigned samples) {
        const std::string sample_count = std::to_string(samples);
        const D3D_SHADER_MACRO defines[] = {{"MS", "1"}, {"DOF_SAMPLES", sample_count.c_str()},
                                            {nullptr, nullptr}};
        ComPtr<ID3DBlob> binary, errors;
        HRESULT result = D3DCompile(gpu::kDepthOfFieldHlsl,
                                    std::strlen(gpu::kDepthOfFieldHlsl),
                                    "src/d3d/depth_of_field_shader.h",
                                    samples > 1 ? defines : nullptr, nullptr, entry, profile,
                                    D3DCOMPILE_ENABLE_STRICTNESS, 0, &binary, &errors);
        if (FAILED(result)) {
            std::string message = errors
                ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                              errors->GetBufferSize()) : "no compiler diagnostics";
            throw std::runtime_error(std::string("D3DCompile(") + entry + ", " +
                                     std::to_string(samples) + "x): " + message);
        }
        return binary;
    }

    ID3D11PixelShader* PixelShader(unsigned samples) {
        auto& shader = shaders_[samples];
        if (!shader) {
            auto bytecode = Compile("depth_of_field", "ps_5_0", samples);
            Hr(device_->CreatePixelShader(bytecode->GetBufferPointer(),
                                          bytecode->GetBufferSize(), nullptr, &shader),
               "CreatePixelShader(depth_of_field)");
        }
        return shader.Get();
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11RasterizerState> raster_;
    ComPtr<ID3D11BlendState> rgb_only_;
    std::map<unsigned, ComPtr<ID3D11PixelShader>> shaders_;
};

void Near(float actual, float expected, float tolerance, const char* message) {
    Check(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}

void Same(const std::vector<Pixel>& actual, const std::vector<Pixel>& expected,
          float tolerance, const char* message) {
    for (size_t i = 0; i < actual.size(); ++i) {
        Near(actual[i].r, expected[i].r, tolerance, message);
        Near(actual[i].g, expected[i].g, tolerance, message);
        Near(actual[i].b, expected[i].b, tolerance, message);
        Near(actual[i].a, expected[i].a, 0, "alpha changed despite RGB-only write mask");
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
    Same(warp.Render(color, depth, 8, whole), color, 2e-5f,
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
    Check(output[At(31, 32)].b < 0.01f, "far blue bled across foreground edge");
    Check(output[At(32, 32)].r > 0.01f, "near red failed to cover adjacent background");
    Near(output[At(31, 32)].a, 0.2f, 0, "foreground alpha changed");
    Near(output[At(32, 32)].a, 0.8f, 0, "background alpha changed");
    std::puts("PASS far blur spread and foreground/background edge ordering");
}

void TestViewport(Warp& warp) {
    const Rect rect{8, 8, 56, 56};
    std::vector<Pixel> color(kWidth * kHeight, {0.8f, 0.1f, 0.9f, 0.6f});
    std::vector<float> depth(kWidth * kHeight, RawDepth(4));
    for (unsigned y = rect.top; y < unsigned(rect.bottom); ++y)
        for (unsigned x = rect.left; x < unsigned(rect.right); ++x)
            color[At(x, y)] = {0.1f, 0.7f, 0.2f, 0.3f};
    auto output = warp.Render(color, depth, 8, rect);
    Same(output, color, 2e-5f, "viewport sampled outside or wrote beyond scissor");
    std::puts("PASS viewport gather bounds and RGB-only scissor write");
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
        Same(output, pattern, 0,
             "MSAA minimum depth did not keep one focused sample sharp");
        std::printf("PASS %ux MSAA blur and mixed-sample focused-depth minimum\n", samples);
    }
}

} // namespace

int main() {
    try {
        Warp warp;
        TestIdentity(warp);
        TestUniformAndInvalid(warp);
        TestBlurAndEdges(warp);
        TestViewport(warp);
        TestMsaa(warp);
        std::puts("PASS depth-of-field WARP checks");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL depth-of-field WARP checks: %s\n", error.what());
        return 1;
    }
}
