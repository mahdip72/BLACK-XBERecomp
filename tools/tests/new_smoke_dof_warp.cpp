// Headless tests of the production particle geometry and native-shader DoF wrapper.
// The only graphics adapter this executable can create is DXGI's software WARP adapter.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "d3d/particle_dof.h"
#include "d3d/particle_dof_geometry.h"
#include "d3d/particle_dof_wrapper.h"
#include "gpu/shaders.h"

// These two headers are byte-for-byte copies from commit a5777a9. Nesting their
// namespace keeps the old generated wrapper callable beside the current one.
namespace smoke_reference {
namespace gpu {
using ::gpu::PixelKey;
using ::gpu::TexInfo;
using ::gpu::kTex2D;
using ::gpu::kTexWide;
}
#include "reference/particle_dof_shader.h"
#define ParticleDofNativePixelKey A5777a9ParticleDofNativePixelKey
#include "reference/particle_dof_wrapper.h"
#undef ParticleDofNativePixelKey
}

using Microsoft::WRL::ComPtr;

namespace {

void Check(bool okay, const char* why) {
    if (!okay) throw std::runtime_error(why);
}

void Hr(HRESULT hr, const char* action) {
    if (FAILED(hr)) {
        char text[160];
        std::snprintf(text, sizeof(text), "%s failed: 0x%08lX", action,
                      static_cast<unsigned long>(hr));
        throw std::runtime_error(text);
    }
}

gpu::NativeParticleVertex Vertex(float x, float y, float reciprocal_z, float w,
                                  uint32_t argb, float u, float v) {
    return {{x, y, reciprocal_z, w}, argb, {u, v}, 0};
}

std::array<gpu::NativeParticleVertex, 4> Quad(float left, float top, float right,
                                              float bottom, float reciprocal_z,
                                              float w = 1.0f) {
    return {Vertex(left, top, reciprocal_z, w, 0xE0FF4040, 0, 0),
            Vertex(left, bottom, reciprocal_z, w, 0xB040FF40, 0, 1),
            Vertex(right, bottom, reciprocal_z, w, 0x80FFFF40, 1, 1),
            Vertex(right, top, reciprocal_z, w, 0x60FF40FF, 1, 0)};
}

gpu::ParticleDofGeometryParams Params(uint32_t width, uint32_t height, float radius = 16) {
    gpu::ParticleDofGeometryParams p{};
    p.globals.optics[0] = 5.0f;
    p.globals.optics[1] = radius;
    p.globals.optics[2] = 1.0f;
    p.globals.optics[3] = 0.45f;
    p.globals.viewport[2] = float(width);
    p.globals.viewport[3] = float(height);
    p.globals.target[0] = float(width);
    p.globals.target[1] = float(height);
    p.globals.target[3] = 0; // native LESS
    // Native particle program maps reciprocal view Z to raw depth: z = 1 - 1/viewZ.
    p.c97[2] = 1.0f;
    p.c98[0] = p.c98[1] = p.c98[3] = 1.0f;
    p.c98[2] = -1.0f;
    p.guest_size[0] = float(width);
    p.guest_size[1] = float(height);
    return p;
}

void CheckGeometry() {
    constexpr float epsilon = 1e-4f;
    auto p = Params(128, 64);
    auto q = Quad(42, 18, 62, 38, 1.0f / 3.0f, 2.25f);
    gpu::NativeParticleVertex raw[4];
    std::memcpy(raw, q.data(), sizeof(raw));
    gpu::NativeParticleVertex expanded[4]{};
    gpu::ParticleDofQuad meta{};
    auto result = gpu::ExpandParticleDofQuad(raw, p, 7.5f, expanded, meta);
    Check(result == gpu::ParticleDofGeometryResult::Ready, "affine W!=1 quad was rejected");
    Check(std::fabs(expanded[0].position[0] - 34.5f) < epsilon &&
          std::fabs(expanded[0].position[1] - 10.5f) < epsilon &&
          std::fabs(expanded[2].position[0] - 69.5f) < epsilon &&
          std::fabs(expanded[2].position[1] - 45.5f) < epsilon,
          "expanded quad did not cover the circular aperture");
    for (int k = 0; k < 4; ++k)
        Check(expanded[k].position[2] == q[k].position[2] &&
              expanded[k].position[3] == q[k].position[3] &&
              meta.argb[k] == q[k].argb,
              "geometry expansion changed depth, W, or original corner diffuse");

    q[0].uv[0] = q[1].uv[0] = 1;
    q[2].uv[0] = q[3].uv[0] = 0;
    std::memcpy(raw, q.data(), sizeof(raw));
    result = gpu::ExpandParticleDofQuad(raw, p, 7.5f, expanded, meta);
    Check(result == gpu::ParticleDofGeometryResult::Ready &&
          meta.uv_bounds[0] == 1 && meta.uv_bounds[2] == 0,
          "flipped full-texture UV should retain signed extent");

    q[0].uv[0] = q[1].uv[0] = 0.25f;
    q[2].uv[0] = q[3].uv[0] = 0.75f;
    std::vector<gpu::NativeParticleVertex> output;
    std::vector<gpu::ParticleDofQuad> metadata;
    gpu::ParticleDofGeometryStats stats{};
    Check(!gpu::BuildParticleDofGeometry(q, p, output, metadata, stats) &&
          stats.invalid == 1 && std::memcmp(output.data(), q.data(), sizeof(q)) == 0 &&
          metadata[0].uv_bounds[0] == metadata[0].uv_bounds[2],
          "partial atlas rectangle must remain byte-native and carry invalid metadata");

    q = Quad(42, 18, 62, 38, 0.2f, 2.25f); // exactly focused at five world units
    Check(!gpu::BuildParticleDofGeometry(q, p, output, metadata, stats) &&
          stats.focused == 1 && stats.active == 0 &&
          std::memcmp(output.data(), q.data(), sizeof(q)) == 0,
          "focused quad changed its native geometry");
    q = Quad(42, 18, 62, 38, 1.0f / 3.0f, 2.25f);
    q[2].position[3] = 2.0f;
    Check(!gpu::BuildParticleDofGeometry(q, p, output, metadata, stats) &&
          stats.invalid == 1 && std::memcmp(output.data(), q.data(), sizeof(q)) == 0,
          "non-affine varying W should be left on native path");
    std::puts("production smoke geometry: W!=1 aperture support, UV flips, atlas/invalid/focus fallback PASS");
}

struct Pixel { float r, g, b, a; };

float FromHalf(uint16_t bits) {
    const unsigned exponent = (bits >> 10) & 31u;
    const unsigned fraction = bits & 1023u;
    const float magnitude = exponent == 0 ? std::ldexp(float(fraction), -24) :
                            exponent == 31 ? INFINITY :
                            std::ldexp(float(1024u + fraction), int(exponent) - 25);
    return bits & 0x8000u ? -magnitude : magnitude;
}

D3D12_RESOURCE_DESC TextureDesc(uint32_t width, uint32_t height, DXGI_FORMAT format,
                                D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = flags;
    return d;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

ComPtr<ID3DBlob> Compile(const std::string& source, const char* profile,
                         const char* entry = "main") {
    Check(!source.empty(), "production particle shader wrapper rejected native source");
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(source.data(), source.size(), "production particle shader",
                                  nullptr, nullptr, entry, profile,
                                  D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
    if (FAILED(hr) && errors)
        std::fprintf(stderr, "%s: %s\n", profile,
                     static_cast<const char*>(errors->GetBufferPointer()));
    Hr(hr, "D3DCompile(production particle shader)");
    return code;
}

struct SmokeScene {
    uint32_t width = 96, height = 64;
    uint32_t tex_width = 32, tex_height = 32;
    std::vector<uint32_t> texture;
    std::vector<float> source_depth;
    std::vector<gpu::NativeParticleVertex> vertices;
    gpu::ParticleDofGeometryParams geometry = Params(96, 64);
    Pixel clear{0.05f, 0.08f, 0.15f, 1};
    float alpha_reference = 0;
    float alpha_function = 7;
    D3D12_RECT scissor{0, 0, 96, 64};
    D3D12_RECT sample_occluder{40, 24, 56, 40};
};

class Warp {
public:
    Warp() {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            debug_enabled_ = true;
        }
        HRESULT factory_hr = CreateDXGIFactory2(debug_enabled_ ? DXGI_CREATE_FACTORY_DEBUG : 0,
                                                 IID_PPV_ARGS(&factory_));
        if (FAILED(factory_hr) && debug_enabled_)
            factory_hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_));
        Hr(factory_hr, "CreateDXGIFactory2");
        Hr(factory_->EnumWarpAdapter(IID_PPV_ARGS(&adapter_)), "EnumWarpAdapter");
        DXGI_ADAPTER_DESC1 desc{};
        Hr(adapter_->GetDesc1(&desc), "GetDesc1(WARP)");
        Check((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0,
              "smoke test selected a hardware adapter");
        Hr(D3D12CreateDevice(adapter_.Get(), D3D_FEATURE_LEVEL_11_0,
                             IID_PPV_ARGS(&device_)), "D3D12CreateDevice(WARP)");
        if (debug_enabled_)
            Hr(device_.As(&info_queue_), "ID3D12InfoQueue(debug layer)");
        D3D12_COMMAND_QUEUE_DESC q{};
        q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Hr(device_->CreateCommandQueue(&q, IID_PPV_ARGS(&queue_)), "CreateCommandQueue(WARP)");
        Hr(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "CreateFence");
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Check(event_ != nullptr, "CreateEvent failed");

        const std::vector<uint32_t> native_program(gpu::kParticleDofNativeProgram.begin(),
                                                   gpu::kParticleDofNativeProgram.end());
        native_vs_ = gpu::VertexProgramHlsl(native_program, 0x209, 0);
        native_ps_ = gpu::PixelHlsl(gpu::ParticleDofNativePixelKey(gpu::TexInfo{gpu::kTex2D}));
        Check(native_vs_.find("q.xyz *= p.w") != std::string::npos,
              "actual native VS lost its W-canceling raster transform");
        wrapped_vs_ = gpu::MakeParticleDofVertexShader(native_vs_);
        wrapped_ps_[0] = gpu::MakeParticleDofPixelShader(native_ps_, 1, 0, 7);
        wrapped_ps_[1] = gpu::MakeParticleDofPixelShader(native_ps_, 2, 0, 7);
        wrapped_ps_[2] = gpu::MakeParticleDofPixelShader(native_ps_, 4, 0, 7);
        wrapped_ps_[3] = gpu::MakeParticleDofPixelShader(native_ps_, 8, 0, 7);
        for (uint32_t i = 0; i < wrapped_ps_.size(); ++i)
            additive_ps_[i] = gpu::MakeParticleDofPixelShader(native_ps_, 1u << i, 1, 7);
        reference_vs_ = smoke_reference::gpu::MakeParticleDofVertexShader(native_vs_);
        reference_ps_[0] = smoke_reference::gpu::MakeParticleDofPixelShader(native_ps_, 1);
        reference_ps_[1] = smoke_reference::gpu::MakeParticleDofPixelShader(native_ps_, 2);
        reference_ps_[2] = smoke_reference::gpu::MakeParticleDofPixelShader(native_ps_, 4);
        reference_ps_[3] = smoke_reference::gpu::MakeParticleDofPixelShader(native_ps_, 8);
        Compile(native_vs_, "vs_5_0"); Compile(native_ps_, "ps_5_0");
        Compile(wrapped_vs_, "vs_5_0");
        for (const auto& source : wrapped_ps_) Compile(source, "ps_5_0");
        for (const auto& source : additive_ps_) Compile(source, "ps_5_0");
        for (uint32_t samples : {2u, 4u, 8u}) {
            const std::string range = "#define PARTICLE_DOF_SAMPLES " +
                std::to_string(samples) + "\n" + gpu::kParticleDofDepthRangeHlsl;
            Compile(range, "vs_5_0", "vs");
            Compile(range, "ps_5_0", "range_ps");
        }
        Compile(reference_vs_, "vs_5_0");
        for (const auto& source : reference_ps_) Compile(source, "ps_5_0");
        auto assembly = [&](const std::string& source) {
            const auto code = Compile(source, "ps_5_0");
            ComPtr<ID3DBlob> disassembled;
            Hr(D3DDisassemble(code->GetBufferPointer(), code->GetBufferSize(), 0, nullptr,
                              &disassembled), "D3DDisassemble(smoke pixel shader)");
            return std::string(static_cast<const char*>(disassembled->GetBufferPointer()),
                               disassembled->GetBufferSize());
        };
        const auto old_assembly = assembly(reference_ps_[3]);
        const auto new_assembly = assembly(wrapped_ps_[3]);
        auto count_op = [](const std::string& source, const char* op) {
            size_t n = 0, at = 0;
            while ((at = source.find(op, at)) != std::string::npos) { ++n; at += std::strlen(op); }
            return n;
        };
        const auto old_trig = count_op(old_assembly, "sincos");
        const auto new_trig = count_op(new_assembly, "sincos");
        const auto old_sqrt = count_op(old_assembly, "sqrt");
        const auto new_sqrt = count_op(new_assembly, "sqrt");
        Check(old_trig > 0 && new_trig == 0 && new_sqrt == 0 &&
              wrapped_ps_[3].find("#define PARTICLE_DOF_TAPS 64") != std::string::npos,
              "64-tap LUT optimization did not remove per-tap transcendental instructions");
        std::printf("64-tap PS disassembly: frozen sincos/sqrt %zu/%zu, optimized %zu/%zu\n",
                    old_trig, old_sqrt, new_trig, new_sqrt);
        std::puts("actual native particle VS/PS and current/reference 1x/2x/4x/8x wrappers compiled");
        MakeRoot();
    }

    ~Warp() { if (event_) CloseHandle(event_); }

    bool SupportsDepthSamples(uint32_t samples) const {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{};
        quality.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        quality.SampleCount = samples;
        return SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                         &quality, sizeof(quality))) && quality.NumQualityLevels > 0;
    }

    std::vector<Pixel> Render(const SmokeScene& scene, bool wrapped, bool blend,
                              uint32_t depth_samples = 1,
                              bool one_sample_occluded = false,
                              bool reference = false,
                              bool additive = false) {
        Check(!additive || blend, "additive smoke needs its native blend state");
        Check(!additive || !reference, "the frozen reference predates additive smoke");
        Check(scene.width && scene.height && scene.tex_width && scene.tex_height &&
              scene.texture.size() == size_t(scene.tex_width) * scene.tex_height &&
              scene.source_depth.size() == size_t(scene.width) * scene.height &&
              !scene.vertices.empty() && scene.vertices.size() % 4 == 0,
              "invalid smoke fixture dimensions");
        Check(scene.vertices.size() / 4 <= gpu::kParticleDofMaxQuads,
              "smoke fixture exceeds production metadata capacity");
        Check(depth_samples == 1 || depth_samples == 2 || depth_samples == 4 ||
              depth_samples == 8,
              "unsupported smoke depth sample count");
        Check(!one_sample_occluded || depth_samples > 1,
              "fractional source visibility requires multisampled depth");
        if (info_queue_) info_queue_->ClearStoredMessages();

        std::vector<gpu::NativeParticleVertex> vertices = scene.vertices;
        std::vector<gpu::ParticleDofQuad> metadata(vertices.size() / 4);
        gpu::ParticleDofGeometryStats stats{};
        if (wrapped) gpu::BuildParticleDofGeometry(scene.vertices, scene.geometry,
                                                   vertices, metadata, stats);
        ComPtr<ID3D12Resource> texture = NewTexture(scene.tex_width, scene.tex_height,
                                                    DXGI_FORMAT_R8G8B8A8_UNORM,
                                                    D3D12_RESOURCE_STATE_COPY_DEST);
        ComPtr<ID3D12Resource> depth = NewDepth(scene.width, scene.height,
                                                depth_samples);
        ComPtr<ID3D12Resource> depth_copy = NewDepthCopy(scene.width, scene.height,
                                                          depth_samples);
        ComPtr<ID3D12Resource> depth_range;
        if (depth_samples > 1)
            depth_range = NewTexture(scene.width, scene.height,
                                     DXGI_FORMAT_R32G32_FLOAT,
                                     D3D12_RESOURCE_STATE_RENDER_TARGET,
                                     D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        ComPtr<ID3D12Resource> target = NewTexture(scene.width, scene.height,
                                                   DXGI_FORMAT_R16G16B16A16_FLOAT,
                                                   D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                   D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        auto tex_upload = UploadTexture(texture.Get(), scene.texture.data(), scene.tex_width * 4);
        auto vb = UploadBuffer(vertices.data(), vertices.size() * sizeof(vertices[0]));
        std::vector<uint16_t> indices;
        for (uint16_t i = 0; i < vertices.size() / 4; ++i) {
            const uint16_t v = uint16_t(4 * i);
            indices.insert(indices.end(), {v, uint16_t(v + 1), uint16_t(v + 2),
                                           v, uint16_t(v + 2), uint16_t(v + 3)});
        }
        auto ib = UploadBuffer(indices.data(), indices.size() * sizeof(indices[0]));

        std::array<std::array<float, 4>, 192> vc{};
        for (int j = 0; j < 4; ++j) {
            vc[97][j] = scene.geometry.c97[j];
            vc[98][j] = scene.geometry.c98[j];
        }
        auto vcb = UploadBuffer(vc.data(), sizeof(vc));
        static_assert(sizeof(gpu::DrawConstants) == 2080);
        constexpr size_t constant_bytes = sizeof(gpu::DrawConstants) +
            sizeof(gpu::ParticleDofGlobals) +
            gpu::kParticleDofMaxQuads * sizeof(gpu::ParticleDofQuad);
        std::vector<uint8_t> constants(constant_bytes);
        gpu::DrawConstants dc{};
        dc.surf[0] = scene.geometry.guest_size[0];
        dc.surf[1] = scene.geometry.guest_size[1];
        dc.surf[2] = 1.0f;
        dc.misc[0] = scene.alpha_reference;
        dc.misc[1] = scene.alpha_function;
        dc.texsize[0][0] = float(scene.tex_width);
        dc.texsize[0][1] = float(scene.tex_height);
        auto globals = scene.geometry.globals;
        globals.target[2] = float(metadata.size());
        std::memcpy(constants.data(), &dc, sizeof(dc));
        std::memcpy(constants.data() + sizeof(dc), &globals, sizeof(globals));
        std::memcpy(constants.data() + sizeof(dc) + sizeof(globals), metadata.data(),
                    metadata.size() * sizeof(metadata[0]));
        auto dcb = UploadBuffer(constants.data(), constants.size());

        D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc{};
        srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srv_heap_desc.NumDescriptors = 4;
        srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> srvs;
        Hr(device_->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&srvs)), "Create SRV heap");
        const UINT increment = device_->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto cpu = srvs->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        device_->CreateShaderResourceView(texture.Get(), &view, cpu);
        cpu.ptr += increment;
        device_->CreateShaderResourceView(nullptr, &view, cpu);
        cpu.ptr += increment;
        view.Format = DXGI_FORMAT_R32G32_FLOAT;
        if (depth_samples > 1)
            device_->CreateShaderResourceView(depth_range.Get(), &view, cpu);
        else
            device_->CreateShaderResourceView(nullptr, &view, cpu);
        cpu.ptr += increment;
        view.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        view.ViewDimension = depth_samples == 1 ? D3D12_SRV_DIMENSION_TEXTURE2D :
                                                  D3D12_SRV_DIMENSION_TEXTURE2DMS;
        device_->CreateShaderResourceView(depth_copy.Get(), &view, cpu);

        D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
        rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv_heap_desc.NumDescriptors = 2;
        ComPtr<ID3D12DescriptorHeap> rtvs;
        Hr(device_->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtvs)), "Create RTV heap");
        const auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
        device_->CreateRenderTargetView(target.Get(), nullptr, rtv);
        auto range_rtv = rtv;
        range_rtv.ptr += device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        if (depth_samples > 1)
            device_->CreateRenderTargetView(depth_range.Get(), nullptr, range_rtv);

        D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc{};
        dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsv_heap_desc.NumDescriptors = 1;
        ComPtr<ID3D12DescriptorHeap> dsvs;
        Hr(device_->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(&dsvs)),
           "Create smoke DSV heap");
        const auto dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
        dsv_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dsv_desc.ViewDimension = depth_samples == 1 ? D3D12_DSV_DIMENSION_TEXTURE2D :
                                                    D3D12_DSV_DIMENSION_TEXTURE2DMS;
        device_->CreateDepthStencilView(depth.Get(), &dsv_desc, dsv);

        const uint32_t ps_index = depth_samples == 8 ? 3 : depth_samples == 4 ? 2 :
                                  depth_samples == 2 ? 1 : 0;
        const auto vs = Compile(wrapped ? (reference ? reference_vs_ : wrapped_vs_) : native_vs_,
                                "vs_5_0");
        const int alpha_function = int(scene.alpha_function);
        Check(alpha_function >= 0 && alpha_function <= 7 &&
              float(alpha_function) == scene.alpha_function,
              "smoke fixture alpha function is not a native 0..7 state");
        const std::string specialized = wrapped && !reference && alpha_function != 7 ?
            gpu::MakeParticleDofPixelShader(native_ps_, depth_samples,
                                            additive ? 1 : 0, alpha_function) : std::string{};
        const auto ps = Compile(wrapped ? (reference ? reference_ps_[ps_index] :
                                          (alpha_function != 7 ? specialized :
                                           additive ? additive_ps_[ps_index] :
                                                      wrapped_ps_[ps_index])) : native_ps_,
                                "ps_5_0");
        ComPtr<ID3D12PipelineState> pso = Pipeline(vs.Get(), ps.Get(), blend, additive);
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        Hr(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
        Hr(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(),
                                      IID_PPV_ARGS(&list)), "CreateCommandList");
        CopyUpload(list.Get(), texture.Get(), tex_upload);
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        for (uint32_t y = 0; y < scene.height; ++y) {
            for (uint32_t x = 0; x < scene.width;) {
                const float z = scene.source_depth[size_t(y)*scene.width+x];
                if (z >= 1.0f) { ++x; continue; }
                const uint32_t begin = x++;
                while (x < scene.width && scene.source_depth[size_t(y)*scene.width+x] == z) ++x;
                const D3D12_RECT rectangle{LONG(begin),LONG(y),LONG(x),LONG(y+1)};
                list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, z, 0, 1,
                                             &rectangle);
            }
        }
        ComPtr<ID3D12PipelineState> sample_initializer;
        if (one_sample_occluded)
            sample_initializer = DrawOneSampleOccluder(list.Get(), dsv, scene,
                                                       depth_samples);
        auto depth_to_copy = Transition(depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                        D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &depth_to_copy);
        list->CopyResource(depth_copy.Get(), depth.Get());
        D3D12_RESOURCE_BARRIER to_read[2] = {Transition(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                           Transition(depth_copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
        list->ResourceBarrier(2, to_read);
        ComPtr<ID3D12PipelineState> range_pso;
        if (depth_samples > 1) {
            const std::string source = "#define PARTICLE_DOF_SAMPLES " +
                std::to_string(depth_samples) + "\n" + gpu::kParticleDofDepthRangeHlsl;
            const auto range_vs = Compile(source, "vs_5_0", "vs");
            const auto range_ps = Compile(source, "ps_5_0", "range_ps");
            range_pso = RangePipeline(range_vs.Get(), range_ps.Get());
            list->SetGraphicsRootSignature(root_.Get());
            ID3D12DescriptorHeap* range_heaps[] = {srvs.Get()};
            list->SetDescriptorHeaps(1, range_heaps);
            list->SetGraphicsRootDescriptorTable(0, srvs->GetGPUDescriptorHandleForHeapStart());
            list->SetPipelineState(range_pso.Get());
            list->OMSetRenderTargets(1, &range_rtv, FALSE, nullptr);
            D3D12_VIEWPORT full{0, 0, float(scene.width), float(scene.height), 0, 1};
            const D3D12_RECT full_scissor{0, 0, LONG(scene.width), LONG(scene.height)};
            list->RSSetViewports(1, &full);
            list->RSSetScissorRects(1, &full_scissor);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->DrawInstanced(3, 1, 0, 0);
            const auto range_to_read = Transition(depth_range.Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            list->ResourceBarrier(1, &range_to_read);
        }
        const float clear[] = {scene.clear.r, scene.clear.g, scene.clear.b, scene.clear.a};
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        D3D12_VIEWPORT viewport{0, 0, float(scene.width), float(scene.height), 0, 1};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scene.scissor);
        list->SetGraphicsRootSignature(root_.Get());
        ID3D12DescriptorHeap* heaps[] = {srvs.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootDescriptorTable(0, srvs->GetGPUDescriptorHandleForHeapStart());
        list->SetGraphicsRootConstantBufferView(1, vcb->GetGPUVirtualAddress());
        list->SetGraphicsRootConstantBufferView(2, dcb->GetGPUVirtualAddress());
        list->SetPipelineState(pso.Get());
        D3D12_VERTEX_BUFFER_VIEW vview{vb->GetGPUVirtualAddress(),
            UINT(vertices.size() * sizeof(vertices[0])), sizeof(vertices[0])};
        D3D12_INDEX_BUFFER_VIEW iview{ib->GetGPUVirtualAddress(),
            UINT(indices.size() * sizeof(indices[0])), DXGI_FORMAT_R16_UINT};
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->IASetVertexBuffers(0, 1, &vview);
        list->IASetIndexBuffer(&iview);
        list->DrawIndexedInstanced(UINT(indices.size()), 1, 0, 0, 0);

        auto footprint = Footprint(target.Get());
        auto readback = NewBuffer(footprint.bytes, D3D12_HEAP_TYPE_READBACK,
                                  D3D12_RESOURCE_STATE_COPY_DEST);
        auto barrier = Transition(target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                  D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
        source.pResource = target.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.pResource = readback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint.placed;
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        Hr(list->Close(), "Close smoke command list");
        ID3D12CommandList* lists[] = {list.Get()};
        queue_->ExecuteCommandLists(1, lists);
        const UINT64 value = ++fence_value_;
        Hr(queue_->Signal(fence_.Get(), value), "Signal smoke fence");
        if (fence_->GetCompletedValue() < value) {
            Hr(fence_->SetEventOnCompletion(value, event_), "SetEventOnCompletion");
            Check(WaitForSingleObject(event_, 30000) == WAIT_OBJECT_0,
                  "WARP smoke draw timed out");
        }
        CheckDebugMessages();
        return ReadPixels(readback.Get(), footprint.placed, scene.width, scene.height);
    }

private:
    struct Upload {
        ComPtr<ID3D12Resource> buffer;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed{};
    };
    struct CopyFootprint { D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed{}; UINT64 bytes = 0; };

    ComPtr<ID3D12Resource> NewTexture(uint32_t width, uint32_t height, DXGI_FORMAT format,
        D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        const auto desc = TextureDesc(width, height, format, flags);
        ComPtr<ID3D12Resource> result;
        Hr(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
                                            nullptr, IID_PPV_ARGS(&result)), "Create smoke texture");
        return result;
    }

    ComPtr<ID3D12Resource> NewDepth(uint32_t width, uint32_t height, uint32_t samples) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        auto desc = TextureDesc(width, height, DXGI_FORMAT_R24G8_TYPELESS,
                                D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        desc.SampleDesc.Count = samples;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        clear.DepthStencil.Depth = 1;
        ComPtr<ID3D12Resource> result;
        Hr(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&result)),
            "Create D24S8 smoke source depth");
        return result;
    }

    ComPtr<ID3D12Resource> NewDepthCopy(uint32_t width, uint32_t height,
                                       uint32_t samples) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        auto desc = TextureDesc(width, height, DXGI_FORMAT_R24G8_TYPELESS,
                                D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        desc.SampleDesc.Count = samples;
        ComPtr<ID3D12Resource> result;
        Hr(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&result)),
            "Create D24S8 smoke source snapshot");
        return result;
    }

    ComPtr<ID3D12PipelineState> DrawOneSampleOccluder(ID3D12GraphicsCommandList* list,
                               D3D12_CPU_DESCRIPTOR_HANDLE dsv, const SmokeScene& scene,
                               uint32_t samples) {
        const auto vs = Compile(
            "float4 main(uint id:SV_VertexID):SV_Position { return float4(id==2?3:-1,id==1?-3:1,0,1); }",
            "vs_5_0");
        const auto ps = Compile("float main():SV_Depth { return 0.1; }", "ps_5_0");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
        p.pRootSignature = root_.Get();
        p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        p.SampleMask = 1; // only sample 0 is hidden; other samples remain visible
        p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        p.RasterizerState.DepthClipEnable = TRUE;
        p.RasterizerState.MultisampleEnable = TRUE;
        p.DepthStencilState.DepthEnable = TRUE;
        p.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        p.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        p.SampleDesc.Count = samples;
        ComPtr<ID3D12PipelineState> init;
        Hr(device_->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&init)),
           "Create 4x source-visibility initializer");
        list->SetPipelineState(init.Get());
        list->SetGraphicsRootSignature(root_.Get());
        list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        D3D12_VIEWPORT viewport{0, 0, float(scene.width), float(scene.height), 0, 1};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scene.sample_occluder);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        return init;
    }

    ComPtr<ID3D12Resource> NewBuffer(UINT64 bytes, D3D12_HEAP_TYPE type,
                                     D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = type;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = bytes;
        d.Height = 1;
        d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> result;
        Hr(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state,
                                            nullptr, IID_PPV_ARGS(&result)), "Create smoke buffer");
        return result;
    }

    ComPtr<ID3D12Resource> UploadBuffer(const void* data, size_t bytes) {
        auto buffer = NewBuffer(bytes, D3D12_HEAP_TYPE_UPLOAD,
                                D3D12_RESOURCE_STATE_GENERIC_READ);
        uint8_t* mapped = nullptr;
        const D3D12_RANGE no_read{0, 0};
        Hr(buffer->Map(0, &no_read, reinterpret_cast<void**>(&mapped)), "Map smoke upload");
        std::memcpy(mapped, data, bytes);
        buffer->Unmap(0, nullptr);
        return buffer;
    }

    CopyFootprint Footprint(ID3D12Resource* texture) {
        CopyFootprint out{};
        const auto desc = texture->GetDesc();
        UINT rows = 0;
        UINT64 row_bytes = 0;
        device_->GetCopyableFootprints(&desc, 0, 1, 0, &out.placed, &rows,
                                       &row_bytes, &out.bytes);
        return out;
    }

    Upload UploadTexture(ID3D12Resource* texture, const void* data, size_t row_bytes) {
        const auto desc = texture->GetDesc();
        auto layout = Footprint(texture);
        Upload out{NewBuffer(layout.bytes, D3D12_HEAP_TYPE_UPLOAD,
                             D3D12_RESOURCE_STATE_GENERIC_READ), layout.placed};
        uint8_t* mapped = nullptr;
        const D3D12_RANGE no_read{0, 0};
        Hr(out.buffer->Map(0, &no_read, reinterpret_cast<void**>(&mapped)),
           "Map smoke texture upload");
        for (uint32_t y = 0; y < desc.Height; ++y)
            std::memcpy(mapped + y * out.placed.Footprint.RowPitch,
                        static_cast<const uint8_t*>(data) + y * row_bytes, row_bytes);
        out.buffer->Unmap(0, nullptr);
        return out;
    }

    static void CopyUpload(ID3D12GraphicsCommandList* list, ID3D12Resource* texture,
                           const Upload& upload) {
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = texture;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = upload.buffer.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = upload.placed;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    static std::vector<Pixel> ReadPixels(ID3D12Resource* readback,
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
        uint32_t width, uint32_t height) {
        const D3D12_RANGE range{0, SIZE_T(footprint.Footprint.RowPitch) * (height - 1) +
                                  SIZE_T(width) * 8};
        uint8_t* mapped = nullptr;
        Hr(readback->Map(0, &range, reinterpret_cast<void**>(&mapped)),
           "Map smoke readback");
        std::vector<Pixel> result(size_t(width) * height);
        for (uint32_t y = 0; y < height; ++y) {
            auto* row = reinterpret_cast<const uint16_t*>(mapped +
                        y * footprint.Footprint.RowPitch);
            for (uint32_t x = 0; x < width; ++x)
                result[size_t(y) * width + x] = {FromHalf(row[4*x]), FromHalf(row[4*x+1]),
                    FromHalf(row[4*x+2]), FromHalf(row[4*x+3])};
        }
        const D3D12_RANGE no_write{0, 0};
        readback->Unmap(0, &no_write);
        return result;
    }

    void MakeRoot() {
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 4;
        range.BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER parameters[3]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].DescriptorTable.NumDescriptorRanges = 1;
        parameters[0].DescriptorTable.pDescriptorRanges = &range;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        for (int i = 1; i <= 2; ++i) {
            parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            parameters[i].Descriptor.ShaderRegister = UINT(i - 1);
        }
        D3D12_STATIC_SAMPLER_DESC samplers[4]{};
        for (uint32_t i = 0; i < 4; ++i) {
            samplers[i].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW =
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
            samplers[i].ShaderRegister = i;
            samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }
        D3D12_ROOT_SIGNATURE_DESC signature{};
        signature.NumParameters = 3;
        signature.pParameters = parameters;
        signature.NumStaticSamplers = 4;
        signature.pStaticSamplers = samplers;
        signature.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> serialized, errors;
        const HRESULT hr = D3D12SerializeRootSignature(&signature,
            D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
        if (FAILED(hr) && errors)
            std::fprintf(stderr, "smoke root signature: %s\n",
                         static_cast<const char*>(errors->GetBufferPointer()));
        Hr(hr, "D3D12SerializeRootSignature(smoke)");
        Hr(device_->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(&root_)), "CreateRootSignature(smoke)");
    }

    ComPtr<ID3D12PipelineState> Pipeline(ID3DBlob* vs, ID3DBlob* ps, bool blend,
                                          bool additive = false) {
        D3D12_INPUT_ELEMENT_DESC attributes[] = {
            {"ATTR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"ATTR", 3, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 16,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"ATTR", 9, DXGI_FORMAT_R32G32_FLOAT, 0, 20,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
        p.pRootSignature = root_.Get();
        p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        p.InputLayout = {attributes, 3};
        p.SampleMask = UINT_MAX;
        p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        p.RasterizerState.DepthClipEnable = TRUE;
        p.DepthStencilState.DepthEnable = FALSE;
        p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        p.NumRenderTargets = 1;
        p.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        p.SampleDesc.Count = 1;
        auto& b = p.BlendState.RenderTarget[0];
        b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        b.BlendEnable = blend;
        b.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        b.DestBlend = additive ? D3D12_BLEND_ONE : D3D12_BLEND_INV_SRC_ALPHA;
        b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
        b.DestBlendAlpha = additive ? D3D12_BLEND_ONE : D3D12_BLEND_INV_SRC_ALPHA;
        b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        ComPtr<ID3D12PipelineState> result;
        Hr(device_->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&result)),
           "CreateGraphicsPipelineState(smoke)");
        return result;
    }

    ComPtr<ID3D12PipelineState> RangePipeline(ID3DBlob* vs, ID3DBlob* ps) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
        p.pRootSignature = root_.Get();
        p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        p.SampleMask = UINT_MAX;
        p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        p.RasterizerState.DepthClipEnable = TRUE;
        p.DepthStencilState.DepthEnable = FALSE;
        p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        p.NumRenderTargets = 1;
        p.RTVFormats[0] = DXGI_FORMAT_R32G32_FLOAT;
        p.SampleDesc.Count = 1;
        p.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        ComPtr<ID3D12PipelineState> result;
        Hr(device_->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&result)),
           "CreateGraphicsPipelineState(smoke depth range)");
        return result;
    }

    void CheckDebugMessages() {
        if (!info_queue_) return;
        const UINT64 count = info_queue_->GetNumStoredMessages();
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T size = 0;
            Hr(info_queue_->GetMessage(i, nullptr, &size), "GetMessage size");
            std::vector<uint8_t> bytes(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
            Hr(info_queue_->GetMessage(i, message, &size), "GetMessage");
            if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
                std::fprintf(stderr, "smoke D3D12 debug: %s\n", message->pDescription);
                throw std::runtime_error("D3D12 debug layer reported smoke error");
            }
        }
        info_queue_->ClearStoredMessages();
    }

    ComPtr<IDXGIFactory6> factory_;
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Fence> fence_;
    ComPtr<ID3D12InfoQueue> info_queue_;
    ComPtr<ID3D12RootSignature> root_;
    HANDLE event_ = nullptr;
    UINT64 fence_value_ = 0;
    bool debug_enabled_ = false;
    std::string native_vs_, native_ps_, wrapped_vs_;
    std::array<std::string, 4> wrapped_ps_;
    std::array<std::string, 4> additive_ps_;
    std::string reference_vs_;
    std::array<std::string, 4> reference_ps_;
};

size_t At(const SmokeScene& scene, uint32_t x, uint32_t y) {
    return size_t(y) * scene.width + x;
}

uint32_t Rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) |
           (uint32_t(a) << 24);
}

SmokeScene Scene(float view_z = 3.0f) {
    SmokeScene s;
    s.texture.assign(size_t(s.tex_width) * s.tex_height, Rgba(255, 0, 0, 255));
    s.source_depth.assign(size_t(s.width) * s.height, 1.0f);
    s.clear = {0, 0, 0, 0};
    auto q = Quad(40, 24, 56, 40, 1.0f / view_z, 2.25f);
    for (auto& v : q) v.argb = 0xC0FFFFFF;
    s.vertices.assign(q.begin(), q.end());
    return s;
}

void WriteBmp(const char* name, const SmokeScene& scene,
              const std::vector<Pixel>& pixels, bool straight_layer = true) {
    const auto directory = std::filesystem::path("out") / "reports" / "smoke-dof";
    std::filesystem::create_directories(directory);
    std::ofstream file(directory / name, std::ios::binary);
    Check(bool(file), "cannot write smoke fixture BMP");
    const uint32_t pitch = (scene.width * 3 + 3) & ~3u;
    const uint32_t bytes = 54 + pitch * scene.height;
    auto u16 = [&](uint16_t n) { file.put(char(n & 255)); file.put(char((n >> 8) & 255)); };
    auto u32 = [&](uint32_t n) { u16(uint16_t(n)); u16(uint16_t(n >> 16)); };
    file.write("BM", 2); u32(bytes); u32(0); u32(54);
    u32(40); u32(scene.width); u32(scene.height); u16(1); u16(24);
    u32(0); u32(pitch * scene.height); u32(2835); u32(2835); u32(0); u32(0);
    std::vector<char> row(pitch, 0);
    auto byte = [](float x) { return char(int(std::round(std::clamp(x, 0.0f, 1.0f) * 255))); };
    for (uint32_t iy = 0; iy < scene.height; ++iy) {
        const uint32_t y = scene.height - 1 - iy;
        for (uint32_t x = 0; x < scene.width; ++x) {
            const auto& p = pixels[At(scene, x, y)];
            // The no-blend readback is straight RGBA. Display it over a neutral
            // checker so invisible RGB at alpha zero cannot masquerade as a halo.
            const float background = ((x / 8 + y / 8) & 1) ? 0.12f : 0.06f;
            const float alpha = straight_layer ? std::clamp(p.a, 0.0f, 1.0f) : 1.0f;
            row[3*x] = byte(p.b * alpha + background * (1 - alpha));
            row[3*x+1] = byte(p.g * alpha + background * (1 - alpha));
            row[3*x+2] = byte(p.r * alpha + background * (1 - alpha));
        }
        file.write(row.data(), row.size());
    }
    Check(bool(file), "smoke fixture BMP was truncated");
}

void CheckFinite(const std::vector<Pixel>& pixels) {
    for (const auto& p : pixels)
        Check(std::isfinite(p.r) && std::isfinite(p.g) &&
              std::isfinite(p.b) && std::isfinite(p.a) &&
              p.a >= -0.001f && p.a <= 1.001f,
              "particle DoF output is nonfinite or has invalid opacity");
}

float AlphaEnergy(const std::vector<Pixel>& pixels) {
    float sum = 0;
    for (const auto& p : pixels) sum += p.a;
    return sum;
}

void CheckFocusAndMixedBatch(Warp& warp) {
    auto s = Scene(5.0f);
    const auto native = warp.Render(s, false, false);
    const auto wrapped = warp.Render(s, true, false);
    CheckFinite(native); CheckFinite(wrapped);
    float max_difference = 0;
    for (size_t i = 0; i < native.size(); ++i) {
        max_difference = std::max({max_difference,
            std::abs(native[i].r - wrapped[i].r),
            std::abs(native[i].g - wrapped[i].g),
            std::abs(native[i].b - wrapped[i].b),
            std::abs(native[i].a - wrapped[i].a)});
    }
    Check(native[At(s, 48, 32)].a > 0.3f, "native focused smoke failed to rasterize");
    Check(max_difference < 0.003f,
          "focus/zero-CoC production wrapper differs from native shader");

    auto near_quad = Quad(68, 24, 84, 40, 1.0f / 3.0f, 2.25f);
    for (auto& v : near_quad) v.argb = 0xC0FFFFFF;
    s.vertices.insert(s.vertices.end(), near_quad.begin(), near_quad.end());
    const auto mixed = warp.Render(s, true, false);
    float focused_difference = 0;
    for (uint32_t y = 23; y <= 41; ++y)
        for (uint32_t x = 39; x <= 57; ++x)
            focused_difference = std::max(focused_difference,
                std::abs(mixed[At(s,x,y)].a - native[At(s,x,y)].a));
    Check(focused_difference < 0.003f,
          "focused quad in active batch lost exact native appearance");
    std::printf("focused native/wrapped max difference %.6f, mixed-batch focus %.6f\n",
                max_difference, focused_difference);
}

void CheckBlurSupportAndAlpha(Warp& warp) {
    auto s = Scene(3.0f);
    // The border is bright green but transparent: filtering RGB without matching alpha creates green halos.
    for (uint32_t y = 0; y < s.tex_height; ++y)
        for (uint32_t x = 0; x < s.tex_width; ++x)
            if (x < 2 || y < 2 || x + 2 >= s.tex_width || y + 2 >= s.tex_height)
                s.texture[size_t(y) * s.tex_width + x] = Rgba(0, 255, 0, 0);
    const auto native = warp.Render(s, false, false);
    const auto blurred = warp.Render(s, true, false);
    CheckFinite(native); CheckFinite(blurred);
    WriteBmp("near-native-before.bmp", s, native);
    WriteBmp("near-blurred-after.bmp", s, blurred);
    const float native_energy = AlphaEnergy(native);
    const float blur_energy = AlphaEnergy(blurred);
    const auto halo = blurred[At(s, 38, 32)];
    Check(native[At(s, 38, 32)].a < 0.002f && halo.a > 0.012f,
          "out-of-focus smoke stayed sharp or expanded quad support was clipped");
    Check(blur_energy > native_energy * 0.75f &&
          blur_energy < native_energy * 1.25f,
          "blurred smoke opacity energy inflated or disappeared");
    Check(halo.r > halo.g * 4 && halo.r > halo.b * 4,
          "transparent atlas exterior leaked bright RGB into smoke halo");
    std::printf("near smoke: native alpha energy %.3f, blurred %.3f, halo alpha %.4f, halo RGB %.4f/%.4f/%.4f\n",
                native_energy, blur_energy, halo.a, halo.r, halo.g, halo.b);

    auto distant_scene = Scene(12.0f);
    distant_scene.texture = s.texture;
    const auto far_blurred = warp.Render(distant_scene, true, false);
    CheckFinite(far_blurred);
    const float near_tail = blurred[At(s, 36,32)].a;
    const float far_tail = far_blurred[At(distant_scene,36,32)].a;
    Check(near_tail > far_tail + 0.006f,
          "near and far smoke lost independent depth-dependent blur radii");
    std::printf("near/far tail x36: %.4f / %.4f\n", near_tail, far_tail);
}

void CheckNativeDiagonal(Warp& warp) {
    auto s = Scene(3.0f);
    s.geometry.globals.optics[1] = 4.0f; // narrow but nonzero aperture
    std::fill(s.texture.begin(), s.texture.end(), Rgba(255,255,255,255));
    // Only corner 2 has red; triangle 0/1/2 gives ~0.5 at the diagonal,
    // whereas bilinear interpolation gives ~0.25.
    s.vertices[0].argb = 0xFF000000;
    s.vertices[1].argb = 0xFF000000;
    s.vertices[2].argb = 0xFFFF0000;
    s.vertices[3].argb = 0xFF000000;
    const auto native = warp.Render(s, false, false);
    const auto blurred = warp.Render(s, true, false);
    CheckFinite(blurred);
    const auto a = native[At(s,48,32)], b = blurred[At(s,48,32)];
    Check(a.r > 0.40f && b.r > 0.35f && std::abs(a.r - b.r) < 0.12f,
          "varying-corner diffuse did not follow native 012/023 triangle interpolation");
    std::printf("native triangle diagonal red %.4f, active wrapper %.4f\n", a.r, b.r);
}

void CheckAlphaTestSourceTaps(Warp& warp) {
    auto s = Scene(3.0f);
    s.alpha_reference = 128;
    s.alpha_function = 4; // native GREATER: alpha byte must exceed reference
    for (uint32_t y = 0; y < s.tex_height; ++y)
        for (uint32_t x = 0; x < s.tex_width / 2; ++x)
            s.texture[size_t(y) * s.tex_width + x] = Rgba(255, 0, 0, 48);
    const auto native = warp.Render(s, false, false);
    const auto blurred = warp.Render(s, true, false);
    CheckFinite(blurred);
    const auto p = At(s, 46, 32);
    Check(native[p].a < 0.002f && blurred[p].a > 0.01f &&
          blurred[p].r > blurred[p].g * 4,
          "alpha-tested source taps discarded the destination instead of adding transparent coverage");
    WriteBmp("alpha-tested-after.bmp", s, blurred);
    std::printf("alpha-tested source: native/blurred coverage %.4f/%.4f\n",
                native[p].a, blurred[p].a);
}

void CheckOccludedSources(Warp& warp) {
    auto s = Scene(3.0f);
    const auto open = warp.Render(s, true, false);
    for (uint32_t y = 24; y < 40; ++y)
        for (uint32_t x = 40; x < 56; ++x)
            s.source_depth[At(s,x,y)] = 0.1f;
    const auto hidden = warp.Render(s, true, false);
    CheckFinite(hidden);
    const float visible_halo = open[At(s,58,32)].a;
    const float hidden_halo = hidden[At(s,58,32)].a;
    Check(visible_halo > 0.01f && hidden_halo < visible_halo * 0.08f,
          "hidden smoke source leaked through opaque silhouette to visible destination");
    WriteBmp("wall-occluded-after.bmp", s, hidden);
    std::printf("opaque-source wall: open/hidden halo alpha %.4f/%.4f\n",
                visible_halo, hidden_halo);
}

void CheckLayerOrderAndBlend(Warp& warp) {
    auto front_scene = Scene(3.0f);
    auto back_scene = Scene(12.0f);
    std::fill(front_scene.texture.begin(), front_scene.texture.end(), Rgba(255,255,255,255));
    back_scene.texture = front_scene.texture;
    auto near_quad = Quad(36, 21, 54, 42, 1.0f / 3.0f, 2.25f);
    auto far_quad = Quad(44, 23, 62, 44, 1.0f / 12.0f, 2.25f);
    for (auto& v : near_quad) v.argb = 0xA0FF2020;
    for (auto& v : far_quad) v.argb = 0xA02040FF;
    front_scene.vertices.assign(near_quad.begin(), near_quad.end());
    back_scene.vertices.assign(far_quad.begin(), far_quad.end());
    const auto near_raw = warp.Render(front_scene, true, false);
    const auto far_raw = warp.Render(back_scene, true, false);
    CheckFinite(near_raw); CheckFinite(far_raw);

    auto combined = front_scene;
    combined.vertices.insert(combined.vertices.end(), back_scene.vertices.begin(), back_scene.vertices.end());
    combined.clear = {0.09f, 0.13f, 0.18f, 1};
    const auto alpha = warp.Render(combined, true, true);
    CheckFinite(alpha);
    const auto p = At(combined, 49, 32);
    const auto& a = near_raw[p];
    const auto& b = far_raw[p];
    Check(a.a > 0.1f && b.a > 0.1f,
          "depth-separated near/far smoke did not overlap in fixture");
    auto expected_alpha = [&](float bg, float ac, float bc) {
        return bc * b.a + (ac * a.a + bg * (1 - a.a)) * (1 - b.a);
    };
    Check(std::abs(alpha[p].r - expected_alpha(combined.clear.r,a.r,b.r)) < 0.035f &&
          std::abs(alpha[p].g - expected_alpha(combined.clear.g,a.g,b.g)) < 0.035f &&
          std::abs(alpha[p].b - expected_alpha(combined.clear.b,a.b,b.b)) < 0.035f,
          "depth-separated smoke alpha blend lost order or double-multiplied opacity");
    auto reversed = back_scene;
    reversed.vertices.insert(reversed.vertices.end(), front_scene.vertices.begin(), front_scene.vertices.end());
    reversed.clear = combined.clear;
    const auto reversed_alpha = warp.Render(reversed, true, true);
    Check(std::abs(reversed_alpha[p].r - alpha[p].r) > 0.015f ||
          std::abs(reversed_alpha[p].b - alpha[p].b) > 0.015f,
          "overlapping near/far smoke collapsed to a single order-independent layer");
    WriteBmp("near-far-overlap-after.bmp", combined, alpha, false);
    std::printf("two-depth alpha smoke: near/far alpha %.3f/%.3f, output RGB %.3f/%.3f/%.3f\n",
                a.a,b.a,alpha[p].r,alpha[p].g,alpha[p].b);
}

void CheckViewportAndDownscale(Warp& warp) {
    auto s = Scene(3.0f);
    s.geometry.guest_size[0] = 192;
    s.geometry.guest_size[1] = 128;
    for (auto& v : s.vertices) {
        v.position[0] *= 2;
        v.position[1] *= 2;
    }
    auto edge = Quad(18, 22, 42, 58, 1.0f/3.0f, 2.25f);
    for (auto& v : edge) v.argb = 0xC0FFFFFF;
    s.vertices.insert(s.vertices.end(), edge.begin(), edge.end());
    s.scissor = {7, 9, 87, 57};
    s.geometry.globals.viewport[0] = 7;
    s.geometry.globals.viewport[1] = 9;
    s.geometry.globals.viewport[2] = 80;
    s.geometry.globals.viewport[3] = 48;
    const auto output = warp.Render(s, true, false);
    CheckFinite(output);
    Check(output[At(s,6,20)].a == 0 && output[At(s,87,32)].a == 0 &&
          output[At(s,8,20)].a > 0.01f && output[At(s,48,32)].a > 0.1f,
          "offset/downscaled viewport leaked or shifted smoke support");
    WriteBmp("offset-downscaled-after.bmp", s, output);
    std::puts("odd-offset, 2x downscaled smoke viewport PASS");
}

void CheckMixedMsaaVisibility(Warp& warp) {
    for (uint32_t samples : {4u, 8u}) {
        if (!warp.SupportsDepthSamples(samples)) {
            std::printf("%ux D24S8 source visibility UNSUPPORTED on this software adapter\n",
                        samples);
            continue;
        }
        auto s = Scene(3.0f);
        const auto open = warp.Render(s, true, false, samples);
        const auto one_hidden = warp.Render(s, true, false, samples, true);
        CheckFinite(open); CheckFinite(one_hidden);
        const float open_alpha = open[At(s,48,32)].a;
        const float mixed_alpha = one_hidden[At(s,48,32)].a;
        const float ratio = mixed_alpha / std::max(open_alpha, 1e-6f);
        const float expected = float(samples-1) / float(samples);
        Check(open_alpha > 0.25f && std::abs(ratio-expected) < 0.055f,
              "MSAA source-depth visibility lost fractional sample coverage");
        if (samples == 4) WriteBmp("mixed-msaa-source-after.bmp", s, one_hidden);
        std::printf("%ux D24S8 source samples: open alpha %.4f, one hidden %.4f, ratio %.4f (expected %.4f)\n",
                    samples, open_alpha, mixed_alpha, ratio, expected);
    }
}

void CompareToFrozenReference(Warp& warp, const char* name, const SmokeScene& scene,
                              uint32_t samples = 1, bool mixed = false,
                              bool blend = false) {
    const auto before = warp.Render(scene, true, blend, samples, mixed, true);
    const auto after = warp.Render(scene, true, blend, samples, mixed, false);
    CheckFinite(before); CheckFinite(after);
    float max_rgb = 0, max_alpha = 0, squared = 0;
    float before_alpha = 0, after_alpha = 0;
    for (size_t i = 0; i < before.size(); ++i) {
        const auto& a = before[i];
        const auto& b = after[i];
        const float dr = std::abs(a.r-b.r), dg = std::abs(a.g-b.g),
                    db = std::abs(a.b-b.b), da = std::abs(a.a-b.a);
        max_rgb = std::max({max_rgb, dr, dg, db});
        max_alpha = std::max(max_alpha, da);
        squared += dr*dr + dg*dg + db*db;
        before_alpha += a.a;
        after_alpha += b.a;
    }
    const float rms = std::sqrt(squared / (3 * before.size()));
    const float relative_alpha = std::abs(after_alpha-before_alpha) /
        std::max(before_alpha, 1.0f);
    std::printf("reference %-18s %ux: max RGB %.6f, max alpha %.6f, RGB RMS %.6f, alpha energy delta %.5f%%\n",
                name, samples, max_rgb, max_alpha, rms, relative_alpha*100);
    Check(max_rgb < 0.02f && max_alpha < 0.012f && rms < 0.003f &&
          relative_alpha < 0.01f,
          "optimized smoke output differs materially from frozen pre-optimization shader");
    if (std::strcmp(name,"near") == 0 && samples == 1) {
        WriteBmp("reference-near-before.bmp", scene, before, !blend);
        WriteBmp("optimized-near-after.bmp", scene, after, !blend);
    }
}

void CheckReferenceQuality(Warp& warp) {
    auto near_scene = Scene(3.0f);
    CompareToFrozenReference(warp,"near",near_scene);
    CompareToFrozenReference(warp,"focus",Scene(5.0f));
    CompareToFrozenReference(warp,"far",Scene(12.0f));
    auto occluded = near_scene;
    for (uint32_t y = 24; y < 40; ++y)
        for (uint32_t x = 40; x < 56; ++x)
            occluded.source_depth[At(occluded,x,y)] = 0.1f;
    CompareToFrozenReference(warp,"occluded",occluded);
    for (uint32_t samples : {2u,4u,8u}) {
        if (warp.SupportsDepthSamples(samples))
            CompareToFrozenReference(warp,"mixed-source",near_scene,samples,true);
        else std::printf("reference %ux D24S8: UNSUPPORTED on WARP\n", samples);
    }
}

void CheckAlphaFunctionModes(Warp& warp) {
    auto scene = Scene(3.0f);
    scene.alpha_reference = 127;
    for (auto& v : scene.vertices) v.argb = 0xFFFFFFFF;
    // These texels straddle the integer alpha-test reference after round(a*255).
    // The old dynamic shader is the independent oracle for all eight Xbox states.
    constexpr uint8_t alphas[]{0, 126, 127, 128, 255};
    for (uint32_t y=0; y<scene.tex_height; ++y)
        for (uint32_t x=0; x<scene.tex_width; ++x)
            scene.texture[size_t(y)*scene.tex_width+x] =
                Rgba(255, 80, 32, alphas[std::min<size_t>(4, x*5/scene.tex_width)]);
    for (int state=0; state<8; ++state) {
        scene.alpha_function = float(state);
        char label[32];
        std::snprintf(label, sizeof(label), "alpha-state-%d", state);
        CompareToFrozenReference(warp, label, scene);
    }
    std::puts("all eight specialized native alpha functions at byte reference 127 PASS");
}

void CheckAdditiveFire(Warp& warp) {
    // The traced native additive-particle material uses SRC_ALPHA/ONE for RGB and alpha.
    // A uniform-alpha quad gives an independent moment identity:
    // E[a^2 v] = a * E[a v], including partial MSAA source visibility.
    auto fire = Scene(3.0f);
    std::fill(fire.texture.begin(), fire.texture.end(), Rgba(255,255,255,255));
    for (auto& v : fire.vertices) v.argb = 0x80FF6020;
    fire.clear = {0.04f, 0.07f, 0.11f, 0.15f};
    auto focus = fire;
    focus.geometry.globals.optics[0] = 3.0f;
    const auto native_focus = warp.Render(focus, false, true, 1, false, false, true);
    const auto wrapper_focus = warp.Render(focus, true, true, 1, false, false, true);
    CheckFinite(native_focus); CheckFinite(wrapper_focus);
    float max_focus = 0;
    for (size_t i = 0; i < native_focus.size(); ++i)
        max_focus = std::max({max_focus,
            std::abs(native_focus[i].r-wrapper_focus[i].r),
            std::abs(native_focus[i].g-wrapper_focus[i].g),
            std::abs(native_focus[i].b-wrapper_focus[i].b),
            std::abs(native_focus[i].a-wrapper_focus[i].a)});
    Check(max_focus < 0.003f, "additive fire zero-CoC changed exact native drawing");
    std::printf("additive fire zero-CoC native/wrapper max RGBA %.6f\n", max_focus);

    for (uint32_t samples : {1u, 4u, 8u}) {
        if (samples > 1 && !warp.SupportsDepthSamples(samples)) {
            std::printf("additive fire %ux D24S8 UNSUPPORTED on WARP\n", samples);
            continue;
        }
        const bool mixed = samples > 1;
        const auto raw_standard = warp.Render(fire, true, false, samples, mixed);
        const auto additive = warp.Render(fire, true, true, samples, mixed, false, true);
        CheckFinite(raw_standard); CheckFinite(additive);
        const float native_alpha = 128.0f / 255.0f;
        float max_rgb_energy = 0, max_alpha_energy = 0;
        size_t covered = 0;
        for (size_t i = 0; i < additive.size(); ++i) {
            const auto& r = raw_standard[i];
            const auto& a = additive[i];
            // Outside the rasterized aperture, an unblended readback is the
            // fixture's clear color, not the standard shader's source value.
            if (std::abs(r.r-fire.clear.r)<0.0001f &&
                std::abs(r.g-fire.clear.g)<0.0001f &&
                std::abs(r.b-fire.clear.b)<0.0001f &&
                std::abs(r.a-fire.clear.a)<0.0001f) continue;
            if (r.a <= 0.04f) continue;
            ++covered;
            max_rgb_energy = std::max({max_rgb_energy,
                std::abs(a.r - (fire.clear.r + r.r*r.a)),
                std::abs(a.g - (fire.clear.g + r.g*r.a)),
                std::abs(a.b - (fire.clear.b + r.b*r.a))});
            max_alpha_energy = std::max(max_alpha_energy,
                std::abs(a.a - (fire.clear.a + native_alpha*r.a)));
        }
        std::printf("additive fire %ux%s: covered %zu, max RGB energy %.5f, alpha energy %.5f\n",
                    samples, mixed ? " mixed" : "", covered,
                    max_rgb_energy, max_alpha_energy);
        Check(covered > 50 && max_rgb_energy < 0.04f && max_alpha_energy < 0.04f,
              "additive fire lost RGB or alpha energy under native SRC_ALPHA/ONE blending");
    }
}

} // namespace

int main() {
    try {
        CheckGeometry();
        Warp warp;
        CheckFocusAndMixedBatch(warp);
        CheckBlurSupportAndAlpha(warp);
        CheckNativeDiagonal(warp);
        CheckAlphaTestSourceTaps(warp);
        CheckOccludedSources(warp);
        CheckLayerOrderAndBlend(warp);
        CheckViewportAndDownscale(warp);
        CheckMixedMsaaVisibility(warp);
        CheckReferenceQuality(warp);
        CheckAlphaFunctionModes(warp);
        CheckAdditiveFire(warp);
        std::puts("production particle smoke DoF WARP checks PASS");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "smoke DoF WARP test FAILED: %s\n", e.what());
        return 1;
    }
}
