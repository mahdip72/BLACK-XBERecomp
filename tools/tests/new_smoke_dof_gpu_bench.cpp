// Offscreen D3D12 timestamp benchmark of native, frozen v25d4110, and atlas smoke.
// This executable deliberately requires hardware D3D12.
// The WARP correctness executable remains software-only and is run separately.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "d3d/particle_dof.h"
#include "d3d/particle_dof_atlas.h"
#include "d3d/particle_dof_clear.h"
#include "d3d/particle_dof_geometry.h"
#include "d3d/particle_dof_small_radius.h"
#include "d3d/particle_dof_shared_coverage.h"
#include "d3d/particle_dof_visibility.h"
#include "d3d/particle_dof_wrapper.h"
#include "d3d/dof_shader_source.h"
#include "gpu/shaders.h"
namespace smoke_v25 {
namespace gpu {
using ::gpu::PixelKey;
using ::gpu::TexInfo;
using ::gpu::kTex2D;
using ::gpu::kTexWide;
}
#include "reference/v25d4110/particle_dof_shader.h"
#define ParticleDofNativePixelKey V25ParticleDofNativePixelKey
#include "reference/v25d4110/particle_dof_wrapper.h"
#undef ParticleDofNativePixelKey
}
namespace smoke_f543 {
#include "reference/f543f54/particle_dof_shader.h"
}
namespace smoke_c591 {
#include "reference/c591162/particle_dof_shader.h"
}
namespace smoke_f793 {
#include "reference/f793e2d/particle_dof_shader.h"
}
namespace smoke_bc30216 {
#include "reference/bc30216/particle_dof_shader.h"
}
using Microsoft::WRL::ComPtr;

namespace {
uint32_t W = 1280, H = 720, AtlasExtent = 2048;
float TargetRadius = 4.0f;
float FocusDistance=5.0f,NearScale=1.0f,FarScale=.45f;
float WallZ=4.0f;
bool ZeroInputW=false;
float QuadScale = 1.0f;
UINT CandidateOptimization=D3DCOMPILE_OPTIMIZATION_LEVEL1;
uint32_t Samples = 8, Quads = 12, Batches = 4;
bool CoverageBenchmark=false,F793Benchmark=false,DepthLessEqual=false,SceneVisible=false;
bool SharedOff=false;
bool GatherBenchmark=false;
bool VisibilityBenchmark=false;
bool GroupColorPages=false;
bool MixedCacheEligibility=false;
bool CompareColorBatching=false;
bool CompareProducerEpochs=false;
bool CompareCacheShader=false;
bool MixedSamples=false,AdditiveBenchmark=false,WarpCorrectness=false;
uint32_t Warmups=8,MeasuredPairs=16;
void Require(bool value, const char* what) { if (!value) throw std::runtime_error(what); }
void CheckAtlasFilterCbufferLayout(ID3DBlob* shader, size_t expected_prefix,
                                   size_t expected_globals) {
    Microsoft::WRL::ComPtr<ID3D11ShaderReflection> reflection;
    const HRESULT hr=D3DReflect(shader->GetBufferPointer(),shader->GetBufferSize(),
        IID_ID3D11ShaderReflection,reinterpret_cast<void**>(reflection.GetAddressOf()));
    Require(SUCCEEDED(hr) && reflection,
            "D3DReflect failed on atlas-filter shader ABI probe");
    auto* cbuffer=reflection->GetConstantBufferByName("ParticleAtlasDraw");
    Require(cbuffer != nullptr,
            "atlas-filter shader omitted ParticleAtlasDraw cbuffer");
    const char* names[]{"particleTarget","particleAtlasTarget","particleQuads"};
    const UINT offsets[]{UINT(expected_prefix+32),UINT(expected_prefix+48),
                         UINT(expected_prefix+expected_globals)};
    for (size_t i=0;i<3;++i) {
        D3D11_SHADER_VARIABLE_DESC variable_desc{};
        auto* variable=cbuffer->GetVariableByName(names[i]);
        Require(variable && SUCCEEDED(variable->GetDesc(&variable_desc)) &&
                variable_desc.StartOffset==offsets[i],
                "atlas-filter shader constants do not match the selected legacy/current prefix");
    }
}
void CheckVisibilityCbufferLayout(ID3DBlob* shader) {
    ComPtr<ID3D11ShaderReflection> reflection;
    Require(SUCCEEDED(D3DReflect(shader->GetBufferPointer(),shader->GetBufferSize(),
        IID_ID3D11ShaderReflection,reinterpret_cast<void**>(reflection.GetAddressOf()))) &&
        reflection,"Reflect Vq shader");
    auto* cbuffer=reflection->GetConstantBufferByName("VC");
    Require(cbuffer!=nullptr,"Vq shader omitted native vertex constants");
    D3D11_SHADER_BUFFER_DESC buffer{};
    Require(SUCCEEDED(cbuffer->GetDesc(&buffer)),"Describe Vq vertex constants");
    Require(buffer.Size==11280,"Vq b0 size differs from native+target+1024 maps ABI");
    const char* names[]{"particleVisibilityTarget","particleVisibilityMaps"};
    const UINT offsets[]{3072,3088};
    for (size_t i=0;i<2;++i) {
        auto* variable=cbuffer->GetVariableByName(names[i]);
        D3D11_SHADER_VARIABLE_DESC desc{};
        Require(variable && SUCCEEDED(variable->GetDesc(&desc)) &&
                desc.StartOffset==offsets[i],
                "Vq shader constant-buffer offsets differ from CPU upload layout");
    }
}
void ReportParticlePixelBytecode(const char* label,ID3DBlob* shader) {
    ComPtr<ID3D11ShaderReflection> reflection;
    Require(SUCCEEDED(D3DReflect(shader->GetBufferPointer(),shader->GetBufferSize(),
        IID_ID3D11ShaderReflection,reinterpret_cast<void**>(reflection.GetAddressOf()))) &&
        reflection,"Reflect coverage particle pixel shader");
    D3D11_SHADER_DESC desc{};
    Require(SUCCEEDED(reflection->GetDesc(&desc)),
            "Describe coverage particle pixel shader");
    std::printf("%s PS: instructions %u, temp registers %u, temp arrays %u, "
                "dynamic flow %u, texture loads %u, bytecode %zu bytes\n",
                label,desc.InstructionCount,desc.TempRegisterCount,
                desc.TempArrayCount,desc.DynamicFlowControlCount,
                desc.TextureLoadInstructions,shader->GetBufferSize());
    ComPtr<ID3DBlob> assembly;
    Require(SUCCEEDED(D3DDisassemble(shader->GetBufferPointer(),shader->GetBufferSize(),
        0,nullptr,&assembly)) && assembly,
        "Disassemble coverage particle pixel shader");
    const auto dir=std::filesystem::path("out")/"reports"/"particle-optimization";
    std::filesystem::create_directories(dir);
    std::ofstream output(dir/(std::string(label)+"-pass2.asm"),std::ios::binary);
    Require(bool(output),"Open coverage particle disassembly");
    output.write(static_cast<const char*>(assembly->GetBufferPointer()),
                 std::streamsize(assembly->GetBufferSize()));
    Require(bool(output),"Save coverage particle disassembly");
}
// The frozen v25/f543 reference wrappers use the old 130-float4 b1 prefix. Their
// native kTex2D fixtures do not read rtshift, so remove only that ABI tail field.
std::string LegacyDrawConstantsShader(std::string source) {
    constexpr std::string_view field = "    float4 rtshift;\n";
    const auto at = source.find(field);
    Require(at != std::string::npos && source.find(field, at + field.size()) == std::string::npos,
            "native shader does not contain exactly one current rtshift cbuffer field");
    source.erase(at, field.size());
    Require(source.find("rtshift") == std::string::npos,
            "legacy native shader unexpectedly depends on the appended rtshift field");
    return source;
}
static_assert(offsetof(gpu::DrawConstants, rtshift) == 2080);
static_assert(sizeof(gpu::DrawConstants) == 2096);
void Hr(HRESULT value, const char* what) {
    if (FAILED(value)) {
        char message[160];
        std::snprintf(message, sizeof(message), "%s: 0x%08lX", what,
                      static_cast<unsigned long>(value));
        throw std::runtime_error(message);
    }
}
ComPtr<ID3DBlob> Compile(const std::string& source, const char* entry, const char* profile,
                        UINT optimization=D3DCOMPILE_OPTIMIZATION_LEVEL1,
                        bool shipped_flags=false) {
    Require(!source.empty(), "native smoke wrapper rejected production source");
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(source.data(), source.size(), "particle GPU benchmark",
        nullptr, nullptr, entry, profile,
        (shipped_flags?0u:D3DCOMPILE_ENABLE_STRICTNESS) | optimization,
        0, &code, &errors);
    if (FAILED(hr) && errors)
        std::fprintf(stderr, "%s/%s: %s\n", entry, profile,
            static_cast<const char*>(errors->GetBufferPointer()));
    Hr(hr, "D3DCompile(smoke benchmark)");
    return code;
}
D3D12_RESOURCE_BARRIER Barrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES from,
                               D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}
D3D12_RESOURCE_BARRIER BarrierMip(ID3D12Resource* resource,UINT mip,
                                  D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
    auto b=Barrier(resource,from,to);
    b.Transition.Subresource=mip;
    return b;
}
D3D12_RESOURCE_DESC TextureDesc(DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                                uint32_t samples = 1, uint32_t width = W,
                                uint32_t height = H) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width; d.Height = height; d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = format; d.SampleDesc.Count = samples;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; d.Flags = flags;
    return d;
}
enum class Mode {
    Native,V25,Atlas,Candidate,CoverageBefore,CoverageAfter,
    F793Before,F793After
};
bool UsesAtlas(Mode mode) {
    return mode==Mode::Atlas || mode==Mode::Candidate ||
           mode==Mode::CoverageBefore || mode==Mode::CoverageAfter ||
           mode==Mode::F793Before || mode==Mode::F793After;
}
struct Intervals { double copy = 0, range = 0, prepare = 0, filter = 0,
                   support = 0, transmission = 0, composite = 0, total = 0; };
struct Result {
    Intervals ms;
    std::vector<uint16_t> rgba,sample_rgba;
    uint64_t image_hash=14695981039346656037ull;
};

class Bench {
public:
    Bench() {
        ComPtr<IDXGIFactory6> factory;
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
        if (WarpCorrectness) {
            Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter_)),"EnumWarpAdapter");
            DXGI_ADAPTER_DESC1 desc{};
            Hr(adapter_->GetDesc1(&desc),"WARP GetDesc1");
            Require((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)!=0,
                    "explicit --warp did not select a software adapter");
            std::printf("WARP correctness adapter: %ls\n",desc.Description);
        }
        for (UINT i = 0;!WarpCorrectness; ++i) {
            ComPtr<IDXGIAdapter1> candidate;
            if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                    IID_PPV_ARGS(&candidate)) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc{};
            Hr(candidate->GetDesc1(&desc), "GetDesc1");
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
                SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0,
                                            __uuidof(ID3D12Device), nullptr))) {
                adapter_ = candidate;
                std::printf("GPU: %ls; vendor %04X device %04X LUID %08lX:%08lX\n",
                             desc.Description,desc.VendorId,desc.DeviceId,
                             DWORD(desc.AdapterLuid.HighPart),desc.AdapterLuid.LowPart);
                break;
            }
        }
        Require(adapter_.Get() != nullptr, "No requested D3D12 adapter; benchmark not run");
        Hr(D3D12CreateDevice(adapter_.Get(), D3D_FEATURE_LEVEL_11_0,
                             IID_PPV_ARGS(&device_)), "D3D12CreateDevice(offscreen)");
        if (GatherBenchmark) {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT format{};
            format.Format=DXGI_FORMAT_R32G32_FLOAT;
            Hr(device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&format,sizeof(format)),
               "Check R32G32_FLOAT gather support");
            Require((format.Support1&D3D12_FORMAT_SUPPORT1_SHADER_GATHER)!=0 &&
                    (format.Support1&D3D12_FORMAT_SUPPORT1_RENDER_TARGET)!=0 &&
                    (format.Support1&D3D12_FORMAT_SUPPORT1_SHADER_LOAD)!=0,
                    "hardware adapter lacks RG32_FLOAT gather/render/load support");
        }
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{};
        q.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; q.SampleCount = Samples;
        Hr(device_->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                                        &q, sizeof(q)), "Check 8x D24S8");
        Require(q.NumQualityLevels > 0, "Hardware adapter lacks requested 8x D24S8");
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Hr(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_)), "CreateCommandQueue");
        Hr(queue_->GetTimestampFrequency(&frequency_), "GetTimestampFrequency");
        std::printf("D3D12 direct-queue timestamp frequency %llu Hz\n",
                    static_cast<unsigned long long>(frequency_));
        Hr(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "CreateFence");
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Require(event_ != nullptr, "CreateEvent failed");
        MakeRoot();
        MakeShadersAndPipelines();
        MakeFixture();
    }
    ~Bench() { if (event_) CloseHandle(event_); }

    Result Run(Mode mode,bool capture_samples=false) {
        const bool coverage=mode==Mode::CoverageBefore || mode==Mode::CoverageAfter ||
                            mode==Mode::F793Before || mode==Mode::F793After;
        const bool float_range=mode==Mode::V25 || mode==Mode::CoverageBefore ||
                               VisibilityBenchmark ||
                               (GatherBenchmark && mode==Mode::F793After);
        const bool shared=mode==Mode::F793After && Samples==2 && !SharedOff;
        const bool visibility=VisibilityBenchmark &&
            (mode==Mode::F793After ||
             ((CompareColorBatching || CompareProducerEpochs || CompareCacheShader) &&
              mode==Mode::F793Before));
        const bool use_epochs=CompareCacheShader ||
                              (CompareProducerEpochs && mode==Mode::F793After);
        const auto visibility_cb=use_epochs?epoch_visibility_vcb_.Get():visibility_vcb_.Get();
        const uint32_t range_levels=1;
        Require(!capture_samples || coverage,"sample capture requires an MSAA coverage mode");
        // Resource creation, uploads and PSO creation are deliberately outside GPU timestamps.
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        Hr(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           IID_PPV_ARGS(&allocator)), "Create allocator");
        Hr(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                      nullptr, IID_PPV_ARGS(&list)), "Create list");
        auto depth = MakeTexture(TextureDesc(DXGI_FORMAT_R24G8_TYPELESS,
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, Samples), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        auto snapshot = MakeTexture(TextureDesc(DXGI_FORMAT_R24G8_TYPELESS,
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, Samples), D3D12_RESOURCE_STATE_COPY_DEST);
        auto range_desc=TextureDesc(float_range?DXGI_FORMAT_R32G32_FLOAT:
                                               DXGI_FORMAT_R32G32_UINT,
                                    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        range_desc.MipLevels=UINT16(range_levels);
        auto range=MakeTexture(range_desc,D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto target = MakeTexture(TextureDesc(DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,coverage?Samples:1),
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        constexpr uint32_t sample_width=64,sample_height=16;
        ComPtr<ID3D12Resource> sample_target;
        if (capture_samples)
            sample_target=MakeTexture(TextureDesc(DXGI_FORMAT_R16G16B16A16_FLOAT,
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,1,sample_width,
                sample_height*Samples),D3D12_RESOURCE_STATE_RENDER_TARGET);
        ComPtr<ID3D12Resource> source_atlas, filtered_atlas;
        ComPtr<ID3D12Resource> support,transmission,visibility_atlas;
        if (UsesAtlas(mode)) {
            const auto desc = TextureDesc(DXGI_FORMAT_R16G16B16A16_FLOAT,
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, 1, AtlasExtent, AtlasExtent);
            source_atlas = MakeTexture(desc, D3D12_RESOURCE_STATE_RENDER_TARGET);
            filtered_atlas = MakeTexture(desc, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        if (shared) {
            support=MakeTexture(TextureDesc(DXGI_FORMAT_R8_UNORM,
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,2),
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            transmission=MakeTexture(TextureDesc(DXGI_FORMAT_R32G32_FLOAT,
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        if (visibility)
            visibility_atlas=MakeTexture(TextureDesc(DXGI_FORMAT_R16_FLOAT,
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,1,AtlasExtent,AtlasExtent),
                D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto descriptors = MakeDescriptors(snapshot.Get(), range.Get(), target.Get(),
                                            source_atlas.Get(), filtered_atlas.Get(),
                                            sample_target.Get(),float_range,range_levels,
                                             support.Get(),transmission.Get(),
                                             visibility_atlas.Get());
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
        dsv_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
        device_->CreateDepthStencilView(depth.Get(), &dsv_desc, descriptors.dsv);
        if (coverage) {
            dsv_desc.Flags=D3D12_DSV_FLAG_READ_ONLY_DEPTH;
            device_->CreateDepthStencilView(depth.Get(),&dsv_desc,descriptors.dsv_read);
        }
        const float wall_depth=1.0f-1.0f/WallZ;
        list->ClearDepthStencilView(descriptors.dsv,D3D12_CLEAR_FLAG_DEPTH,
                                    coverage&&!SceneVisible?wall_depth:1.0f,
                                    0,0,nullptr);
        if (coverage&&!SceneVisible) {
            // Repeating openings keep hidden z12 particle source energy near
            // each defocused z4 wall edge. The blocked destination path then
            // evaluates the expensive Ro/Rp transmission over real MS depth.
            const uint32_t period=std::max(4u,uint32_t(std::round(4*TargetRadius)));
            const uint32_t opening=std::max(2u,period/2);
            for (uint32_t x=0;x<W;x+=period) {
                const D3D12_RECT aperture{LONG(x),0,LONG(std::min(W,x+opening)),LONG(H)};
                list->ClearDepthStencilView(descriptors.dsv,D3D12_CLEAR_FLAG_DEPTH,
                                            1.0f,0,1,&aperture);
            }
        }
        const float clear[] = {.02f,.03f,.05f,.1f};
        list->ClearRenderTargetView(descriptors.target_rtv, clear, 0, nullptr);
        if (MixedSamples) {
            const D3D12_VIEWPORT mixed_viewport{0,0,float(W),float(H),0,1};
            const D3D12_RECT mixed_scissor{LONG(W/2-32),0,LONG(W/2+32),LONG(H)};
            list->RSSetViewports(1,&mixed_viewport);
            list->RSSetScissorRects(1,&mixed_scissor);
            list->SetGraphicsRootSignature(root_.Get());
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->OMSetRenderTargets(1,&descriptors.target_rtv,FALSE,&descriptors.dsv);
            list->SetPipelineState(mixed_sample0_pso_.Get());
            list->DrawInstanced(3,1,0,0);
            list->SetPipelineState(mixed_sample1_pso_.Get());
            list->DrawInstanced(3,1,0,0);
        }

        D3D12_QUERY_HEAP_DESC qh{};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = UINT(2 + 2 * (3 + Batches *
                         std::max<size_t>({1,pages_.size(),visibility_pages_.size()}) * 5));
        ComPtr<ID3D12QueryHeap> queries;
        Hr(device_->CreateQueryHeap(&qh, IID_PPV_ARGS(&queries)), "Create timestamp heap");
        const auto timing = MakeBuffer(qh.Count*sizeof(uint64_t), D3D12_HEAP_TYPE_READBACK,
                                       D3D12_RESOURCE_STATE_COPY_DEST);
        UINT ticks = 0;
        auto stamp = [&] {
            Require(ticks < qh.Count, "Timestamp query capacity exceeded");
            list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, ticks++);
        };
        std::vector<std::pair<UINT,UINT>> copies,ranges,prepares,filters,
                                           supports,transmissions,composites;
        auto timed = [&](auto& bucket, auto&& operation) {
            const UINT first = ticks; stamp(); operation(); stamp();
            bucket.emplace_back(first, ticks-1);
        };
        auto heap = descriptors.srv_heap.Get();
        list->SetDescriptorHeaps(1, &heap);
        list->SetGraphicsRootSignature(root_.Get());
        list->SetGraphicsRootDescriptorTable(0, descriptors.native_srv);
        list->SetGraphicsRootConstantBufferView(1, vcb_->GetGPUVirtualAddress());
        list->SetGraphicsRootConstantBufferView(2, old_dcb_->GetGPUVirtualAddress());
        const D3D12_VIEWPORT viewport{0,0,float(W),float(H),0,1};
        const D3D12_RECT scissor{0,0,LONG(W),LONG(H)};
        const D3D12_VIEWPORT atlas_viewport{0,0,float(AtlasExtent),float(AtlasExtent),0,1};
        const D3D12_RECT atlas_scissor{0,0,LONG(AtlasExtent),LONG(AtlasExtent)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_VERTEX_BUFFER_VIEW vbv{
            (mode == Mode::Native ? native_vb_ : vb_)->GetGPUVirtualAddress(),
            UINT(sizeof(gpu::NativeParticleVertex)*expanded_.size()), sizeof(gpu::NativeParticleVertex)};
        const D3D12_VERTEX_BUFFER_VIEW native_vbv{
            native_vb_->GetGPUVirtualAddress(),
            UINT(sizeof(gpu::NativeParticleVertex)*expanded_.size()),
            sizeof(gpu::NativeParticleVertex)};
        const D3D12_INDEX_BUFFER_VIEW ibv{ib_->GetGPUVirtualAddress(),
            UINT(sizeof(uint16_t)*indices_.size()), DXGI_FORMAT_R16_UINT};
        list->IASetVertexBuffers(0, 1, &vbv);
        list->IASetIndexBuffer(&ibv);

        const UINT all_start = ticks; stamp();
        if (mode != Mode::Native) {
            // v25d4110 already caches opaque depth: BOTH DoF modes use exactly one copy and range pass.
            timed(copies, [&] {
                const auto before = Barrier(depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                             D3D12_RESOURCE_STATE_COPY_SOURCE);
                list->ResourceBarrier(1, &before);
                list->CopyResource(snapshot.Get(), depth.Get());
                const D3D12_RESOURCE_BARRIER after[]{
                    Barrier(snapshot.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                    Barrier(depth.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                            D3D12_RESOURCE_STATE_DEPTH_WRITE)};
                list->ResourceBarrier(2, after);
            });
            timed(ranges, [&] {
                list->SetPipelineState(float_range?float_range_pso_.Get():range_pso_.Get());
                list->OMSetRenderTargets(1, &descriptors.range_rtv, FALSE, nullptr);
                list->DrawInstanced(3, 1, 0, 0);
                auto b = BarrierMip(range.Get(),0,D3D12_RESOURCE_STATE_RENDER_TARGET,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                list->ResourceBarrier(1, &b);
            });
        }
        if (coverage) {
            const auto read_depth=Barrier(depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                          D3D12_RESOURCE_STATE_DEPTH_READ);
            list->ResourceBarrier(1,&read_depth);
        }
        D3D12_RESOURCE_STATES source_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        D3D12_RESOURCE_STATES filtered_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        D3D12_RESOURCE_STATES support_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        D3D12_RESOURCE_STATES transmission_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        D3D12_RESOURCE_STATES visibility_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES& state,
                              D3D12_RESOURCE_STATES next) {
            if (state == next) return;
            const auto barrier = Barrier(resource, state, next);
            list->ResourceBarrier(1, &barrier); state = next;
        };
        const bool group_color_pages=visibility && GroupColorPages &&
            (mode==Mode::F793After || CompareProducerEpochs || CompareCacheShader);
        for (uint32_t batch = 0; batch < Batches; ++batch) {
            if (!UsesAtlas(mode)) {
                timed(composites, [&] {
                    list->SetPipelineState(mode == Mode::Native ? native_pso_.Get() : old_pso_.Get());
                    list->OMSetRenderTargets(1, &descriptors.target_rtv, FALSE, nullptr);
                    list->DrawIndexedInstanced(UINT(indices_.size()), 1, 0, 0, 0);
                });
                continue;
            }
            const auto& active_rects=visibility && !group_color_pages?
                visibility_rects_:page_rects_;
            const auto& active_cb=visibility && !group_color_pages?
                visibility_page_dcbs_:page_dcbs_;
            const size_t page_count=visibility && !group_color_pages?
                visibility_pages_.size():pages_.size();
            size_t next_visibility_chunk=0, next_visibility_epoch=0;
            for (size_t page_index = 0; page_index < page_count; ++page_index) {
                const auto page=visibility && !group_color_pages?
                    gpu::ParticleDofAtlasPage{visibility_pages_[page_index].first_quad,
                                              visibility_pages_[page_index].quad_count}:
                    pages_[page_index];
                const auto page_cb=active_cb[page_index]->GetGPUVirtualAddress();
                timed(prepares, [&] {
                    transition(source_atlas.Get(), source_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    // Production clears only allocated cells. The filtered pass writes every cell itself.
                    const auto& rectangles = active_rects[page_index];
                    gpu::ClearParticleDofAtlasCells(list.Get(), descriptors.source_rtv, rectangles);
                    list->RSSetViewports(1, &atlas_viewport);
                    list->RSSetScissorRects(1, &atlas_scissor);
                    list->SetGraphicsRootDescriptorTable(0, descriptors.native_srv);
                    list->SetGraphicsRootConstantBufferView(1, vcb_->GetGPUVirtualAddress());
                    list->SetGraphicsRootConstantBufferView(2, page_cb);
                    list->SetPipelineState(mode==Mode::CoverageBefore?
                        coverage_before_source_pso_.Get():
                        mode==Mode::F793Before || visibility?f793_source_pso_.Get():source_pso_.Get());
                    list->OMSetRenderTargets(1, &descriptors.source_rtv, FALSE, nullptr);
                    list->DrawIndexedInstanced(page.quad_count * 6, 1, page.first_quad * 6, 0, 0);
                    transition(source_atlas.Get(), source_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                });
                timed(filters, [&] {
                    transition(filtered_atlas.Get(), filtered_state,
                               D3D12_RESOURCE_STATE_RENDER_TARGET);
                    list->SetGraphicsRootDescriptorTable(0, descriptors.filter_srv);
                    list->SetGraphicsRootConstantBufferView(1, radii_cb_->GetGPUVirtualAddress());
                    list->SetGraphicsRootConstantBufferView(2,
                        mode==Mode::Atlas ? legacy_page_dcbs_[page_index]->GetGPUVirtualAddress() :
                                            page_cb);
                    list->SetPipelineState(mode==Mode::Candidate || coverage ? candidate_pso_.Get() :
                                           filter_pso_.Get());
                    list->OMSetRenderTargets(1, &descriptors.filtered_rtv, FALSE, nullptr);
                    list->DrawInstanced(6, page.quad_count, 0, 0);
                    transition(filtered_atlas.Get(), filtered_state,
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                });
                if (shared) {
                    timed(supports,[&] {
                        transition(support.Get(),support_state,D3D12_RESOURCE_STATE_RENDER_TARGET);
                        const float empty[]{0,0,0,0};
                        list->ClearRenderTargetView(descriptors.support_rtv,empty,0,nullptr);
                        list->RSSetViewports(1,&viewport);
                        list->RSSetScissorRects(1,&scissor);
                        list->SetGraphicsRootDescriptorTable(0,descriptors.composite_srv);
                        list->SetGraphicsRootConstantBufferView(1,vcb_->GetGPUVirtualAddress());
                        list->SetGraphicsRootConstantBufferView(2,page_cb);
                        list->SetPipelineState(support_pso_.Get());
                        list->OMSetRenderTargets(1,&descriptors.support_rtv,FALSE,nullptr);
                        list->DrawIndexedInstanced(page.quad_count*6,1,page.first_quad*6,0,0);
                        transition(support.Get(),support_state,
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    });
                    timed(transmissions,[&] {
                        transition(transmission.Get(),transmission_state,
                                   D3D12_RESOURCE_STATE_RENDER_TARGET);
                        const float empty[]{0,0,0,0};
                        list->ClearRenderTargetView(descriptors.transmission_rtv,empty,0,nullptr);
                        list->RSSetViewports(1,&viewport);
                        const auto& bounds=shared_infos_[page_index].bounds;
                        const D3D12_RECT field_scissor{
                            LONG(std::max(0,bounds[0]-1)),LONG(std::max(0,bounds[1]-1)),
                            LONG(std::min(int(W),bounds[2]+1)),LONG(std::min(int(H),bounds[3]+1))};
                        list->RSSetScissorRects(1,&field_scissor);
                        list->SetGraphicsRootDescriptorTable(0,descriptors.transmission_srv);
                        list->SetGraphicsRootConstantBufferView(1,vcb_->GetGPUVirtualAddress());
                        list->SetGraphicsRootConstantBufferView(2,
                            field_page_dcbs_[page_index]->GetGPUVirtualAddress());
                        const D3D12_VERTEX_BUFFER_VIEW synthetic{
                            shared_vbs_[page_index]->GetGPUVirtualAddress(),
                            UINT(4*sizeof(gpu::NativeParticleVertex)),
                            sizeof(gpu::NativeParticleVertex)};
                        list->IASetVertexBuffers(0,1,&synthetic);
                        list->SetPipelineState(transmission_pso_.Get());
                        list->OMSetRenderTargets(1,&descriptors.transmission_rtv,FALSE,nullptr);
                        list->DrawIndexedInstanced(6,1,0,0,0);
                        list->IASetVertexBuffers(0,1,&vbv);
                        transition(transmission.Get(),transmission_state,
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    });
                }
                size_t chunk_begin=page_index,chunk_end=page_index+1;
                if (group_color_pages) {
                    chunk_begin=next_visibility_chunk;
                    uint32_t next_quad=page.first_quad;
                    const uint32_t page_end=page.first_quad+page.quad_count;
                    while (next_visibility_chunk<visibility_pages_.size() &&
                           visibility_pages_[next_visibility_chunk].first_quad<page_end) {
                        const auto& chunk=visibility_pages_[next_visibility_chunk++];
                        Require(chunk.first_quad==next_quad && chunk.quad_count>0 &&
                                chunk.quad_count<=page_end-next_quad,
                                "visibility chunk escaped its original color page or changed draw order");
                        next_quad+=chunk.quad_count;
                    }
                    chunk_end=next_visibility_chunk;
                    Require(chunk_begin<chunk_end && next_quad==page_end,
                            "visibility chunks did not cover their original color page");
                }
                for (size_t chunk_index=chunk_begin;chunk_index<chunk_end;++chunk_index) {
                    const auto draw_page=group_color_pages?
                        gpu::ParticleDofAtlasPage{visibility_pages_[chunk_index].first_quad,
                                                  visibility_pages_[chunk_index].quad_count}:page;
                    const auto draw_cb=group_color_pages?
                        visibility_page_dcbs_[chunk_index]->GetGPUVirtualAddress():page_cb;
                    const bool cached=visibility && visibility_pages_[chunk_index].cached;
                    bool produce=cached;
                    auto producer_page=draw_page;
                    auto producer_cb=draw_cb;
                    if (use_epochs) {
                        produce=false;
                        if (next_visibility_epoch<visibility_epochs_.size() &&
                            visibility_epochs_[next_visibility_epoch].first_chunk==chunk_index) {
                            const auto& epoch=visibility_epochs_[next_visibility_epoch++];
                            Require(epoch.first_quad==draw_page.first_quad &&
                                    epoch.chunk_count>0 &&
                                    epoch.first_chunk+epoch.chunk_count<=visibility_pages_.size(),
                                    "producer epoch did not start at its first native chunk");
                            produce=epoch.cached;
                            producer_page={epoch.first_quad,epoch.quad_count};
                            producer_cb=page_cb;
                        }
                    }
                    if (produce) {
                        timed(transmissions,[&] {
                            transition(visibility_atlas.Get(),visibility_state,
                                       D3D12_RESOURCE_STATE_RENDER_TARGET);
                            list->RSSetViewports(1,&atlas_viewport);
                            list->RSSetScissorRects(1,&atlas_scissor);
                            list->SetGraphicsRootDescriptorTable(0,descriptors.native_srv);
                            list->SetGraphicsRootConstantBufferView(1,
                                visibility_cb->GetGPUVirtualAddress());
                            list->SetGraphicsRootConstantBufferView(2,producer_cb);
                            list->IASetVertexBuffers(0,1,&native_vbv);
                            list->SetPipelineState(visibility_source_pso_.Get());
                            list->OMSetRenderTargets(1,&descriptors.visibility_rtv,FALSE,nullptr);
                            list->DrawIndexedInstanced(producer_page.quad_count*6,1,
                                                       producer_page.first_quad*6,0,0);
                            list->IASetVertexBuffers(0,1,&vbv);
                            transition(visibility_atlas.Get(),visibility_state,
                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                        });
                    }
                    timed(composites, [&] {
                        list->RSSetViewports(1, &viewport);
                        list->RSSetScissorRects(1, &scissor);
                        list->SetGraphicsRootDescriptorTable(0,
                            cached?descriptors.visibility_composite_srv:
                            shared?descriptors.shared_composite_srv:descriptors.composite_srv);
                        list->SetGraphicsRootConstantBufferView(1,
                            visibility?visibility_cb->GetGPUVirtualAddress():
                                       vcb_->GetGPUVirtualAddress());
                        list->SetGraphicsRootConstantBufferView(2, draw_cb);
                        list->SetPipelineState(visibility?
                            (cached?(CompareCacheShader && mode==Mode::F793Before?
                                     visibility_composite_old_pso_.Get():
                                     visibility_composite_pso_.Get()):
                                    coverage_after_pso_.Get()):
                            mode==Mode::CoverageBefore?coverage_before_pso_.Get():
                            mode==Mode::CoverageAfter?coverage_after_pso_.Get():
                            mode==Mode::F793Before?f793_composite_pso_.Get():
                            mode==Mode::F793After?
                                (shared?shared_composite_pso_.Get():coverage_after_pso_.Get()):
                            composite_pso_.Get());
                        list->OMSetRenderTargets(1,&descriptors.target_rtv,FALSE,
                                                  coverage?&descriptors.dsv_read:nullptr);
                        list->DrawIndexedInstanced(draw_page.quad_count * 6, 1,
                                                   draw_page.first_quad * 6, 0, 0);
                    });
                }
            }
            if (group_color_pages)
                Require(next_visibility_chunk==visibility_pages_.size(),
                        "grouped color pages left visibility chunks undrawn");
            if (use_epochs)
                Require(next_visibility_epoch==visibility_epochs_.size(),
                        "producer epochs did not cover every original color page");
        }
        const UINT all_end = ticks; stamp();
        list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP,0,ticks,
                               timing.Get(),0);
        ComPtr<ID3D12Resource> resolved;
        ID3D12Resource* copy_source=target.Get();
        if (coverage) {
            resolved=MakeTexture(TextureDesc(DXGI_FORMAT_R16G16B16A16_FLOAT,
                D3D12_RESOURCE_FLAG_NONE),D3D12_RESOURCE_STATE_RESOLVE_DEST);
            const auto to_resolve=Barrier(target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,
                                          D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            list->ResourceBarrier(1,&to_resolve);
            list->ResolveSubresource(resolved.Get(),0,target.Get(),0,
                                     DXGI_FORMAT_R16G16B16A16_FLOAT);
            copy_source=resolved.Get();
        }
        auto target_barrier = Barrier(copy_source,
            coverage?D3D12_RESOURCE_STATE_RESOLVE_DEST:D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1,&target_barrier);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT rows=0; UINT64 row_bytes=0, total=0;
        auto desc = copy_source->GetDesc();
        device_->GetCopyableFootprints(&desc,0,1,0,&fp,&rows,&row_bytes,&total);
        auto pixels = MakeBuffer(total,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};
        src.pResource=copy_source;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=pixels.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint=fp;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT sample_fp{};
        ComPtr<ID3D12Resource> sample_readback;
        if (capture_samples) {
            auto b=Barrier(target.Get(),D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            list->ResourceBarrier(1,&b);
            const D3D12_VIEWPORT sample_viewport{0,0,float(sample_width),
                                                  float(sample_height*Samples),0,1};
            const D3D12_RECT sample_scissor{0,0,LONG(sample_width),
                                             LONG(sample_height*Samples)};
            list->RSSetViewports(1,&sample_viewport);
            list->RSSetScissorRects(1,&sample_scissor);
            list->SetPipelineState(sample_pso_.Get());
            list->SetGraphicsRootDescriptorTable(0,descriptors.sample_srv);
            list->OMSetRenderTargets(1,&descriptors.sample_rtv,FALSE,nullptr);
            list->DrawInstanced(3,1,0,0);
            b=Barrier(sample_target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1,&b);
            auto sample_desc=sample_target->GetDesc();
            UINT sample_rows=0;UINT64 sample_row_bytes=0,sample_total=0;
            device_->GetCopyableFootprints(&sample_desc,0,1,0,&sample_fp,
                                           &sample_rows,&sample_row_bytes,&sample_total);
            sample_readback=MakeBuffer(sample_total,D3D12_HEAP_TYPE_READBACK,
                                       D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION sample_src{},sample_dst{};
            sample_src.pResource=sample_target.Get();
            sample_src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            sample_dst.pResource=sample_readback.Get();
            sample_dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            sample_dst.PlacedFootprint=sample_fp;
            list->CopyTextureRegion(&sample_dst,0,0,0,&sample_src,nullptr);
        }
        Hr(list->Close(),"Close benchmark list");
        ID3D12CommandList* lists[]{list.Get()};queue_->ExecuteCommandLists(1,lists);
        const UINT64 fence_value=++fence_value_;
        Hr(queue_->Signal(fence_.Get(),fence_value),"Signal benchmark fence");
        if (fence_->GetCompletedValue()<fence_value) {
            Hr(fence_->SetEventOnCompletion(fence_value,event_),"Benchmark fence event");
            Require(WaitForSingleObject(event_,120000)==WAIT_OBJECT_0,
                    "Hardware timestamp benchmark timed out");
        }
        Result result;
        uint64_t* t=nullptr;
        const D3D12_RANGE tr{0,ticks*sizeof(uint64_t)};
        Hr(timing->Map(0,&tr,reinterpret_cast<void**>(&t)),"Map timestamp readback");
        auto collect = [&](const auto& intervals) {
            double ms = 0;
            for (auto [a,b]:intervals) ms += 1000.0*double(t[b]-t[a])/frequency_;
            return ms;
        };
        result.ms.copy=collect(copies); result.ms.range=collect(ranges);
        result.ms.prepare=collect(prepares); result.ms.filter=collect(filters);
        result.ms.support=collect(supports);result.ms.transmission=collect(transmissions);
        result.ms.composite=collect(composites);
        result.ms.total=1000.0*double(t[all_end]-t[all_start])/frequency_;
        const D3D12_RANGE no_write{0,0};timing->Unmap(0,&no_write);
        uint8_t* pixel_bytes=nullptr;
        const D3D12_RANGE pr{0,SIZE_T(fp.Footprint.RowPitch)*(H-1)+W*8};
        Hr(pixels->Map(0,&pr,reinterpret_cast<void**>(&pixel_bytes)),"Map RGBA readback");
        result.rgba.resize(size_t(W)*H*4);
        for (uint32_t y=0;y<H;++y)
            std::memcpy(result.rgba.data()+size_t(y)*W*4,
                        pixel_bytes+size_t(y)*fp.Footprint.RowPitch,W*8);
        pixels->Unmap(0,&no_write);
        for (uint16_t channel:result.rgba) {
            result.image_hash^=channel;
            result.image_hash*=1099511628211ull;
        }
        if (capture_samples) {
            uint8_t* sample_bytes=nullptr;
            const D3D12_RANGE sample_range{0,
                SIZE_T(sample_fp.Footprint.RowPitch)*(sample_height*Samples-1)+sample_width*8};
            Hr(sample_readback->Map(0,&sample_range,
                                    reinterpret_cast<void**>(&sample_bytes)),
               "Map MSAA sample inspection readback");
            result.sample_rgba.resize(size_t(sample_width)*sample_height*Samples*4);
            for (uint32_t y=0;y<sample_height*Samples;++y)
                std::memcpy(result.sample_rgba.data()+size_t(y)*sample_width*4,
                    sample_bytes+size_t(y)*sample_fp.Footprint.RowPitch,sample_width*8);
            sample_readback->Unmap(0,&no_write);
        }
        return result;
    }

private:
    struct Descriptors {
        ComPtr<ID3D12DescriptorHeap> srv_heap,rtv_heap,dsv_heap;
        D3D12_CPU_DESCRIPTOR_HANDLE range_rtv{},target_rtv{},source_rtv{},filtered_rtv{},
                                    sample_rtv{},support_rtv{},transmission_rtv{},
                                    visibility_rtv{},dsv{},dsv_read{};
        D3D12_GPU_DESCRIPTOR_HANDLE sample_srv{};
        D3D12_GPU_DESCRIPTOR_HANDLE native_srv{},filter_srv{},composite_srv{},
                                    transmission_srv{},shared_composite_srv{},
                                    visibility_composite_srv{};
    };
    ComPtr<ID3D12Resource> MakeBuffer(UINT64 bytes,D3D12_HEAP_TYPE type,
                                      D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type=type;
        D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width=bytes; d.Height=d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;
        d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> out;
        Hr(device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,
                                            IID_PPV_ARGS(&out)),"Create benchmark buffer");
        return out;
    }
    ComPtr<ID3D12Resource> MakeTexture(const D3D12_RESOURCE_DESC& d,
                                      D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> out;
        Hr(device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,
                                            IID_PPV_ARGS(&out)),"Create benchmark texture");
        return out;
    }
    ComPtr<ID3D12Resource> Upload(const void* data,size_t bytes) {
        auto out=MakeBuffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped=nullptr; const D3D12_RANGE none{0,0};
        Hr(out->Map(0,&none,&mapped),"Map benchmark upload");
        std::memcpy(mapped,data,bytes); out->Unmap(0,nullptr); return out;
    }
    void MakeRoot() {
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors=4;
        D3D12_ROOT_PARAMETER p[3]{};
        p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[0].DescriptorTable.NumDescriptorRanges=1;
        p[0].DescriptorTable.pDescriptorRanges=&range;
        p[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        for (UINT i=1;i<3;++i) {
            p[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
            p[i].Descriptor.ShaderRegister=i-1;
        }
        D3D12_STATIC_SAMPLER_DESC samplers[4]{};
        for (UINT i=0;i<4;++i) {
            samplers[i].Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samplers[i].AddressU=samplers[i].AddressV=samplers[i].AddressW=
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[i].MaxLOD=D3D12_FLOAT32_MAX;
            samplers[i].ShaderRegister=i;
            samplers[i].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        }
        D3D12_ROOT_SIGNATURE_DESC d{};
        d.NumParameters=3;d.pParameters=p;
        d.NumStaticSamplers=4;d.pStaticSamplers=samplers;
        d.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> serialized,errors;
        Hr(D3D12SerializeRootSignature(&d,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors),
           "Serialize benchmark root");
        Hr(device_->CreateRootSignature(0,serialized->GetBufferPointer(),
            serialized->GetBufferSize(),IID_PPV_ARGS(&root_)),"Create benchmark root");
    }
    ComPtr<ID3D12PipelineState> Pipeline(ID3DBlob* vs,ID3DBlob* ps,
                                        DXGI_FORMAT format,bool blend,bool vertices,
                                        uint32_t samples=1,bool attach_depth=false,
                                        bool additive=false,bool write_depth=false) {
        D3D12_INPUT_ELEMENT_DESC attrs[]{
            {"ATTR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"ATTR",3,DXGI_FORMAT_B8G8R8A8_UNORM,0,16,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"ATTR",9,DXGI_FORMAT_R32G32_FLOAT,0,20,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
        p.pRootSignature=root_.Get();
        p.VS={vs->GetBufferPointer(),vs->GetBufferSize()};
        p.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        if (vertices) p.InputLayout={attrs,3};
        p.SampleMask=UINT_MAX;
        p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;
        p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        p.RasterizerState.DepthClipEnable=TRUE;
        p.RasterizerState.MultisampleEnable=samples>1;
        p.DepthStencilState.DepthEnable=write_depth?TRUE:FALSE;
        p.DepthStencilState.DepthWriteMask=write_depth?
            D3D12_DEPTH_WRITE_MASK_ALL:D3D12_DEPTH_WRITE_MASK_ZERO;
        p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        p.DSVFormat=attach_depth?DXGI_FORMAT_D24_UNORM_S8_UINT:DXGI_FORMAT_UNKNOWN;
        p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        p.NumRenderTargets=1;
        p.RTVFormats[0]=format;
        p.SampleDesc.Count=samples;
        p.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        if (blend) {
            auto& b=p.BlendState.RenderTarget[0];
            b.BlendEnable=TRUE;
            b.SrcBlend=b.SrcBlendAlpha=D3D12_BLEND_SRC_ALPHA;
            b.DestBlend=b.DestBlendAlpha=additive?
                D3D12_BLEND_ONE:D3D12_BLEND_INV_SRC_ALPHA;
            b.BlendOp=b.BlendOpAlpha=D3D12_BLEND_OP_ADD;
        }
        ComPtr<ID3D12PipelineState> out;
        Hr(device_->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&out)),"Create benchmark PSO");
        return out;
    }
    void MakeShadersAndPipelines() {
        const std::vector<uint32_t> program(gpu::kParticleDofNativeProgram.begin(),
                                             gpu::kParticleDofNativeProgram.end());
        const auto native_vs=gpu::VertexProgramHlsl(program,0x209,0);
        const auto native_ps=gpu::PixelHlsl(gpu::ParticleDofNativePixelKey(gpu::TexInfo{gpu::kTex2D}));
        const auto legacy_native_vs=LegacyDrawConstantsShader(native_vs);
        const auto legacy_native_ps=LegacyDrawConstantsShader(native_ps);
        const auto native_vertex=Compile(native_vs,"main","vs_5_0");
        const auto native_pixel=Compile(native_ps,"main","ps_5_0");
        const auto source_vs=Compile(VisibilityBenchmark?
            gpu::DofParticleVertexSource(1):gpu::MakeParticleDofVertexShader(native_vs,1),
            "main","vs_5_0");
        const uint32_t blend=AdditiveBenchmark?1u:0u;
        const auto source_ps=Compile(gpu::MakeParticleDofPixelShader(native_ps,Samples,blend,7,1),
                                     "main","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL1,
                                     CoverageBenchmark);
        ComPtr<ID3D12PipelineState> feature_source_pso;
        const auto composite_vs=Compile(VisibilityBenchmark?
            gpu::DofParticleVertexSource(2):gpu::MakeParticleDofVertexShader(native_vs,2),
            "main","vs_5_0");
        const auto composite_ps=Compile(gpu::MakeParticleDofPixelShader(native_ps,Samples,blend,7,2),"main","ps_5_0");
        if (CoverageBenchmark) {
            const char* frozen_quad=VisibilityBenchmark?
                                    smoke_bc30216::gpu::kParticleDofQuadHlsl:
                                    F793Benchmark?smoke_f793::gpu::kParticleDofQuadHlsl:
                                                    smoke_c591::gpu::kParticleDofQuadHlsl;
            const char* frozen_disk=VisibilityBenchmark?
                                    smoke_bc30216::gpu::kParticleDofDiskHlsl:
                                    F793Benchmark?smoke_f793::gpu::kParticleDofDiskHlsl:
                                                    smoke_c591::gpu::kParticleDofDiskHlsl;
            const char* frozen_aperture=VisibilityBenchmark?
                                        smoke_bc30216::gpu::kParticleDofHlsl:
                                        F793Benchmark?smoke_f793::gpu::kParticleDofHlsl:
                                                        smoke_c591::gpu::kParticleDofHlsl;
            Require(GatherBenchmark || (std::strcmp(gpu::kParticleDofQuadHlsl,frozen_quad)==0 &&
                    std::strcmp(gpu::kParticleDofDiskHlsl,frozen_disk)==0),
                    "frozen coverage comparison changed quad ABI or aperture taps");
            if (VisibilityBenchmark)
                Require(gpu::ParticleDofAtlasFilterSource(true,true)==
                            smoke_bc30216::gpu::ParticleDofAtlasFilterSource(true,true),
                        "visibility candidate changed the matched atlas filter");
            const auto frozen_core=[&](std::string source) {
                const std::string current_core=gpu::kParticleDofHlsl;
                const size_t at=source.rfind(current_core);
                Require(at!=std::string::npos && at+current_core.size()==source.size(),
                        "production particle wrapper no longer ends with its aperture core");
                source.replace(at,current_core.size(),frozen_aperture);
                return source;
            };
            const std::string current_composite=
                gpu::MakeParticleDofPixelShader(native_ps,Samples,blend,7,2);
            const std::string float_prefix="#define PARTICLE_DOF_FLOAT_BOUNDS 1\n";
            const std::string before=VisibilityBenchmark?
                float_prefix+frozen_core(current_composite):GatherBenchmark?
                gpu::DofParticlePixelSource(Samples,blend,7,false,2,false):
                frozen_core(current_composite);
            const std::string after=VisibilityBenchmark?
                float_prefix+current_composite:GatherBenchmark?
                gpu::DofParticlePixelSource(Samples,blend,7,false,2,true):current_composite;
            const std::string before_source=VisibilityBenchmark?
                float_prefix+frozen_core(
                    gpu::MakeParticleDofPixelShader(native_ps,Samples,blend,7,1)):GatherBenchmark?
                gpu::DofParticlePixelSource(Samples,blend,7,false,1,false):
                frozen_core(gpu::MakeParticleDofPixelShader(native_ps,Samples,blend,7,1));
            const std::string after_source=VisibilityBenchmark?
                before_source:GatherBenchmark?
                gpu::DofParticlePixelSource(Samples,blend,7,false,1,true):
                gpu::MakeParticleDofPixelShader(native_ps,Samples,blend,7,1);
            if (GatherBenchmark) {
                const std::string switch_prefix="#define PARTICLE_DOF_FLOAT_BOUNDS 1\n";
                Require(after==switch_prefix+before &&
                        after_source==switch_prefix+before_source,
                        "production gather factories changed code beyond the bounds switch");
            }
            const auto before_ps=Compile(before,"main","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL1,true);
            const auto after_ps=Compile(after,"main","ps_5_0",CandidateOptimization,true);
            if (VisibilityBenchmark) {
                Require(before.starts_with(float_prefix) && after.starts_with(float_prefix) &&
                        before_source.starts_with(float_prefix) &&
                        after_source==before_source,
                        "visibility comparison did not keep a matched FLOAT source pass");
            }
            if (F793Benchmark) {
                ReportParticlePixelBytecode(VisibilityBenchmark?"bc30216-direct-pass2":
                                           GatherBenchmark?"production-uint-pass2":
                                           "f793-baseline",before_ps.Get());
                ReportParticlePixelBytecode(VisibilityBenchmark?"visibility-direct-fallback-pass2":
                                           GatherBenchmark?"production-float-gather-pass2":
                                           "candidate-direct-fallback-pass2",after_ps.Get());
            }
            const auto before_source_ps=Compile(before_source,"main","ps_5_0",
                                                D3DCOMPILE_OPTIMIZATION_LEVEL1,true);
            const auto after_source_ps=Compile(after_source,"main","ps_5_0",
                                               CandidateOptimization,true);
            coverage_before_source_pso_=Pipeline(source_vs.Get(),before_source_ps.Get(),
                DXGI_FORMAT_R16G16B16A16_FLOAT,false,true);
            coverage_before_pso_=Pipeline(composite_vs.Get(),before_ps.Get(),
                DXGI_FORMAT_R16G16B16A16_FLOAT,true,true,Samples,true,AdditiveBenchmark);
            coverage_after_pso_=Pipeline(composite_vs.Get(),after_ps.Get(),
                DXGI_FORMAT_R16G16B16A16_FLOAT,true,true,Samples,true,AdditiveBenchmark);
            if (F793Benchmark) {
                f793_source_pso_=coverage_before_source_pso_;
                f793_composite_pso_=coverage_before_pso_;
                feature_source_pso=Pipeline(source_vs.Get(),after_source_ps.Get(),
                    DXGI_FORMAT_R16G16B16A16_FLOAT,false,true);
            }
            // Read a small interior ROI from each physical MS sample, after the
            // timed work. A resolved image alone can conceal sample-mask errors.
            const std::string inspect_source=
                "#define N " + std::to_string(Samples) + "\n"
                "#define OX " + std::to_string(W/2-32) + "\n"
                "#define OY " + std::to_string(H/2-8) + "\n"
                "Texture2DMS<float4,N> scene : register(t0);\n"
                "float4 inspect(float4 p : SV_Position) : SV_Target {\n"
                "  uint2 q = uint2(p.xy);\n"
                "  return scene.Load(int2(OX + q.x, OY + q.y/N), q.y%N);\n"
                "}\n";
            const auto inspect_ps=Compile(inspect_source,"inspect","ps_5_0",
                                          D3DCOMPILE_OPTIMIZATION_LEVEL1,true);
            const std::string inspect_vs_source=
                "float4 inspect_vs(uint id : SV_VertexID) : SV_Position {\n"
                "  return float4((id==2 ? 3 : -1), (id==1 ? -3 : 1), 0, 1);\n"
                "}\n";
            const auto inspect_vs=Compile(inspect_vs_source,"inspect_vs","vs_5_0",
                                          D3DCOMPILE_OPTIMIZATION_LEVEL1,true);
            sample_pso_=Pipeline(inspect_vs.Get(),inspect_ps.Get(),
                                 DXGI_FORMAT_R16G16B16A16_FLOAT,false,false);
        }
        const auto old_vs=Compile(smoke_v25::gpu::MakeParticleDofVertexShader(legacy_native_vs),
                                  "main","vs_5_0");
        const auto old_ps=Compile(smoke_v25::gpu::MakeParticleDofPixelShader(legacy_native_ps,Samples,0,7),
                                  "main","ps_5_0");
        const auto range_source=GatherBenchmark?gpu::DofParticleRangeSource(Samples,false):
            std::string("#define PARTICLE_DOF_SAMPLES ")+
                                std::to_string(Samples)+"\n"+
                                gpu::kParticleDofDepthRangeHlsl;
        const auto range_vs=Compile(range_source,"vs","vs_5_0");
        const auto range_ps=Compile(range_source,"range_ps","ps_5_0",
                                    D3DCOMPILE_OPTIMIZATION_LEVEL1,CoverageBenchmark);
        const auto float_range_source=(GatherBenchmark || VisibilityBenchmark)?
            gpu::DofParticleRangeSource(Samples,true):
            std::string("#define PARTICLE_DOF_SAMPLES ")+
                                      std::to_string(Samples)+"\n"+
                                      smoke_c591::gpu::kParticleDofDepthRangeHlsl;
        if (GatherBenchmark)
            Require(float_range_source=="#define PARTICLE_DOF_FLOAT_BOUNDS 1\n"+range_source,
                    "production range factory changed code beyond the bounds switch");
        if (VisibilityBenchmark)
            Require(std::strcmp(gpu::kParticleDofDepthRangeHlsl,
                                smoke_bc30216::gpu::kParticleDofDepthRangeHlsl)==0,
                    "visibility comparison changed frozen base range shader");
        const auto float_range_vs=Compile(float_range_source,"vs","vs_5_0");
        const auto float_range_ps=Compile(float_range_source,"range_ps","ps_5_0",
                                          D3DCOMPILE_OPTIMIZATION_LEVEL1,CoverageBenchmark);
        const auto filter_vs=Compile(smoke_f543::gpu::kParticleDofAtlasFilterHlsl,"vs","vs_5_0");
        const auto filter_ps=Compile(smoke_f543::gpu::kParticleDofAtlasFilterHlsl,
                                     "filter_ps","ps_5_0");
        const auto candidate_source=gpu::ParticleDofAtlasFilterSource(true,true);
        const auto candidate_vs=Compile(candidate_source,"vs","vs_5_0");
        const auto candidate_ps=Compile(candidate_source,"filter_ps","ps_5_0",CandidateOptimization);
        CheckAtlasFilterCbufferLayout(filter_vs.Get(),2080,64);
        CheckAtlasFilterCbufferLayout(candidate_vs.Get(),2096,80);
        native_pso_=Pipeline(native_vertex.Get(),native_pixel.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,true,true);
        old_pso_=Pipeline(old_vs.Get(),old_ps.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,true,true);
        source_pso_=F793Benchmark?feature_source_pso:
            Pipeline(source_vs.Get(),source_ps.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,false,true);
        filter_pso_=Pipeline(filter_vs.Get(),filter_ps.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,false,false);
        candidate_pso_=Pipeline(candidate_vs.Get(),candidate_ps.Get(),
                                DXGI_FORMAT_R16G16B16A16_FLOAT,false,false);
        composite_pso_=Pipeline(composite_vs.Get(),composite_ps.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,true,true);
        if (MixedSamples) {
            const std::string vs_text=
                "float4 mixed_vs(uint id:SV_VertexID):SV_Position { "
                "return float4((id==2?3:-1),(id==1?-3:1),0,1); }";
            const auto mixed_vs=Compile(vs_text,"mixed_vs","vs_5_0");
            const auto make_mixed=[&](uint32_t mask,const char* color,const char* depth) {
                const std::string ps_text=
                    "struct O { float4 color:SV_Target; float depth:SV_Depth; "
                    "uint coverage:SV_Coverage; }; O mixed_ps() { O o; o.color="+
                    std::string(color)+"; o.depth="+depth+"; o.coverage="+
                    std::to_string(mask)+"; return o; }";
                const auto ps=Compile(ps_text,"mixed_ps","ps_5_0");
                return Pipeline(mixed_vs.Get(),ps.Get(),
                    DXGI_FORMAT_R16G16B16A16_FLOAT,false,false,2,true,false,true);
            };
            char wall_literal[32]{};
            std::snprintf(wall_literal,sizeof(wall_literal),"%.9g",1.0f-1.0f/WallZ);
            mixed_sample0_pso_=make_mixed(1,"float4(.72,.08,.04,.2)",wall_literal);
            mixed_sample1_pso_=make_mixed(2,"float4(.02,.14,.8,.8)","1.0");
        }
        if (F793Benchmark && Samples==2 && !GatherBenchmark && !VisibilityBenchmark) {
            auto make=[&](uint32_t pass,uint32_t blend) {
                const auto vertex=Compile(gpu::MakeParticleDofVertexShader(native_vs,pass),
                                          "main","vs_5_0",CandidateOptimization,true);
                const auto pixel=Compile(gpu::MakeParticleDofPixelShader(native_ps,2,blend,0,pass),
                                         "main","ps_5_0",CandidateOptimization,true);
                return std::pair{vertex,pixel};
            };
            const auto [support_vs,support_ps]=make(5,0);
            const auto [field_vs,field_ps]=make(3,0);
            const auto [consumer_vs,consumer_ps]=make(4,0);
            ReportParticlePixelBytecode("shared-support-pass5",support_ps.Get());
            ReportParticlePixelBytecode("shared-field-pass3",field_ps.Get());
            ReportParticlePixelBytecode("shared-consumer-pass4",consumer_ps.Get());
            support_pso_=Pipeline(support_vs.Get(),support_ps.Get(),
                                  DXGI_FORMAT_R8_UNORM,false,true,2);
            transmission_pso_=Pipeline(field_vs.Get(),field_ps.Get(),
                                       DXGI_FORMAT_R32G32_FLOAT,false,true);
            shared_composite_pso_=Pipeline(consumer_vs.Get(),consumer_ps.Get(),
                DXGI_FORMAT_R16G16B16A16_FLOAT,true,true,2,true);
        }
        range_pso_=Pipeline(range_vs.Get(),range_ps.Get(),DXGI_FORMAT_R32G32_UINT,false,false);
        float_range_pso_=Pipeline(float_range_vs.Get(),float_range_ps.Get(),
                                  DXGI_FORMAT_R32G32_FLOAT,false,false);
        if (VisibilityBenchmark) {
            const auto producer_vs=Compile(gpu::DofParticleVertexSource(6),"main","vs_5_0",
                                           D3DCOMPILE_OPTIMIZATION_LEVEL1,true);
            const auto producer_ps=Compile(gpu::DofParticlePixelSource(2,0,0,false,6,true),
                                           "main","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL1,true);
            const auto consumer_vs=Compile(gpu::DofParticleVertexSource(7),"main","vs_5_0",
                                           D3DCOMPILE_OPTIMIZATION_LEVEL1,true);
            const auto consumer_ps=Compile(gpu::DofParticlePixelSource(2,blend,0,false,7,true),
                                           "main","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL1,true);
            CheckVisibilityCbufferLayout(producer_vs.Get());
            CheckVisibilityCbufferLayout(consumer_ps.Get());
            visibility_source_pso_=Pipeline(producer_vs.Get(),producer_ps.Get(),
                                            DXGI_FORMAT_R16_FLOAT,false,true);
            visibility_composite_pso_=Pipeline(consumer_vs.Get(),consumer_ps.Get(),
                DXGI_FORMAT_R16G16B16A16_FLOAT,true,true,2,true,AdditiveBenchmark);
            if (CompareCacheShader) {
                const std::string old_source=
                    "#define PARTICLE_DOF_CACHE_APERTURE_FAST 0\n"+
                    gpu::DofParticlePixelSource(2,blend,0,false,7,true);
                const auto old_ps=Compile(old_source,"main","ps_5_0",
                                          D3DCOMPILE_OPTIMIZATION_LEVEL1,true);
                Require(old_ps->GetBufferSize()!=consumer_ps->GetBufferSize() ||
                        std::memcmp(old_ps->GetBufferPointer(),consumer_ps->GetBufferPointer(),
                                    old_ps->GetBufferSize())!=0,
                        "consumer macro did not produce distinct cached-aperture bytecode");
                visibility_composite_old_pso_=Pipeline(consumer_vs.Get(),old_ps.Get(),
                    DXGI_FORMAT_R16G16B16A16_FLOAT,true,true,2,true,AdditiveBenchmark);
                ReportParticlePixelBytecode("visibility-consumer-original-pass7",old_ps.Get());
            }
            ReportParticlePixelBytecode("visibility-producer-pass6",producer_ps.Get());
            ReportParticlePixelBytecode("visibility-consumer-pass7",consumer_ps.Get());
        }
    }
    void MakeFixture() {
        gpu::ParticleDofGeometryParams p{};
        p.globals.optics[0]=FocusDistance;
        p.globals.optics[2]=NearScale;p.globals.optics[3]=FarScale;
        // Match the requested visible radius at the z12 reference particle.
        const float reference_coc=std::min(std::max(std::abs(12.0f-FocusDistance)-
            FocusDistance*.15f,0.0f)/12.0f,1.0f)*
            (12.0f<FocusDistance?NearScale:FarScale);
        Require(!CoverageBenchmark || reference_coc>0,
                "selected optics leave the z12 benchmark particle focused");
        p.globals.optics[1]=CoverageBenchmark?TargetRadius/reference_coc:TargetRadius*2.4f;
        p.globals.viewport[2]=p.globals.target[0]=p.guest_size[0]=float(W);
        p.globals.viewport[3]=p.globals.target[1]=p.guest_size[1]=float(H);
        p.globals.target[2]=float(Quads);
        p.globals.target[3]=DepthLessEqual?1.0f:0.0f;
        if (CoverageBenchmark) {
            p.globals.projection[0]=-1;
            p.globals.projection[1]=1;
            p.globals.projection[2]=1;
        }
        p.c97[2]=1;p.c97[3]=ZeroInputW?1.0f:0.0f;
        p.c98[0]=p.c98[1]=p.c98[3]=1;p.c98[2]=-1;
        std::vector<gpu::NativeParticleVertex> native;
        constexpr std::array<float,4> gather_depths{8.0f,10.0f,12.0f,16.0f};
        for (uint32_t q=0;q<Quads;++q) {
            const float x=(260+float(q%4)*5)*float(W)/1280.0f;
            const float y=(120+float(q/4)*5)*float(H)/720.0f;
            const float full_width=QuadScale*760.0f*float(W)/1280.0f;
            const float full_height=QuadScale*480.0f*float(H)/720.0f;
            // Tiny alternating native quads still draw/filter, but their
            // receiver footprint is below the production Vq admission limit.
            const float quad_width=MixedCacheEligibility && (q&1)?
                std::min(full_width,16.0f):full_width;
            const float quad_height=MixedCacheEligibility && (q&1)?
                std::min(full_height,16.0f):full_height;
            const float z=(GatherBenchmark || VisibilityBenchmark)?
                           1.0f/gather_depths[q%gather_depths.size()]:
                          CoverageBenchmark?1.0f/12.0f:1.0f/3.0f;
            const uint32_t color=0x38FFE0C0;
            const float input_w=ZeroInputW?0.0f:2.25f;
            native.push_back({{x,y,z,input_w},color,{0,0},0});
            native.push_back({{x,y+quad_height,z,input_w},color,{0,1},0});
            native.push_back({{x+quad_width,y+quad_height,z,input_w},color,{1,1},0});
            native.push_back({{x+quad_width,y,z,input_w},color,{1,0},0});
            const uint16_t k=uint16_t(q*4);
            indices_.insert(indices_.end(),{k,uint16_t(k+1),uint16_t(k+2),
                                              k,uint16_t(k+2),uint16_t(k+3)});
        }
        gpu::ParticleDofGeometryStats stats{};
        Require(gpu::BuildParticleDofGeometry(native,p,expanded_,metadata_,stats) &&
                stats.active==Quads,"benchmark geometry failed production expansion");
        native_vb_=Upload(native.data(),native.size()*sizeof(native[0]));
        vb_=Upload(expanded_.data(),expanded_.size()*sizeof(expanded_[0]));
        ib_=Upload(indices_.data(),indices_.size()*sizeof(indices_[0]));
        pages_=gpu::BuildParticleDofAtlasPages(expanded_,p,metadata_,AtlasExtent,AtlasExtent);
        Require(!pages_.empty(),"production atlas packer returned no pages");
        if (VisibilityBenchmark) {
            const float largest=p.globals.optics[1]*
                std::max(p.globals.optics[2],p.globals.optics[3]);
            visibility_halo_=uint32_t(std::ceil(largest))+2;
            visibility_pages_=gpu::BuildParticleDofVisibilityPages(
                pages_,metadata_,p.globals,AtlasExtent,visibility_halo_,visibility_maps_);
            if (CompareProducerEpochs || CompareCacheShader) {
                epoch_visibility_pages_=gpu::BuildParticleDofVisibilityPages(
                    pages_,metadata_,p.globals,AtlasExtent,visibility_halo_,
                    epoch_visibility_maps_,&visibility_epochs_);
                Require(epoch_visibility_pages_.size()==visibility_pages_.size(),
                        "epoch planner changed the native composite chunk count");
                for (size_t c=0;c<visibility_pages_.size();++c) {
                    const auto& old=visibility_pages_[c];
                    const auto& next=epoch_visibility_pages_[c];
                    Require(old.first_quad==next.first_quad &&
                            old.quad_count==next.quad_count && old.cached==next.cached,
                            "epoch planner changed native composite order or eligibility");
                }
                size_t next_chunk=0;
                for (const auto& epoch:visibility_epochs_) {
                    Require(epoch.first_chunk==next_chunk && epoch.chunk_count>0 &&
                            epoch.first_chunk+epoch.chunk_count<=visibility_pages_.size(),
                            "epoch planner omitted or duplicated a composite chunk");
                    Require(epoch.first_quad==visibility_pages_[next_chunk].first_quad,
                            "epoch producer begins at the wrong native quad");
                    next_chunk+=epoch.chunk_count;
                    const auto& last=visibility_pages_[next_chunk-1];
                    Require(last.first_quad+last.quad_count==
                            epoch.first_quad+epoch.quad_count,
                            "epoch producer omits a native fallback span");
                }
                Require(next_chunk==visibility_pages_.size(),
                        "epoch planner failed to cover the final composite chunk");
                if (MixedCacheEligibility) {
                    Require(std::any_of(visibility_epochs_.begin(),visibility_epochs_.end(),
                        [](const auto& epoch) { return epoch.cached && epoch.chunk_count>1; }),
                        "fixture did not exercise one producer across cached/fallback chunks");
                    if (WarpCorrectness && Quads>=16)
                        Require(visibility_epochs_.size()>pages_.size(),
                                "WARP epoch fixture did not cross an actual cache overflow");
                }
                std::printf("Epoch planner: %zu producers across %zu ordered chunks and "
                            "%zu original color pages\n",visibility_epochs_.size(),
                            visibility_pages_.size(),pages_.size());
            }
            Require(!visibility_pages_.empty() &&
                    std::any_of(visibility_pages_.begin(),visibility_pages_.end(),
                        [](const auto& page) { return page.cached; }),
                    "benchmark fixture did not enter the production visibility cache path");
            uint32_t next=0,cached=0;
            for (const auto& page:visibility_pages_) {
                Require(page.first_quad==next && page.quad_count>0,
                        "visibility planner changed native quad order");
                next+=page.quad_count;
                cached+=page.cached?1u:0u;
            }
            Require(next==Quads,"visibility planner lost native quads");
            if (MixedCacheEligibility) {
                Require(Quads>=2 && cached>0 && cached<visibility_pages_.size(),
                        "mixed eligibility fixture needs cached and fallback chunks");
                for (uint32_t q=0;q<Quads;++q)
                    Require((visibility_maps_[q].atlas_xy!=0xffffffffu)==!(q&1),
                            "alternating native quads did not split cache eligibility");
            }
            std::printf("Visibility planner: %zu ordered chunks, %u cached, halo %u px\n",
                        visibility_pages_.size(),cached,visibility_halo_);
        }
        std::array<float,gpu::kParticleDofMaxQuads> radii{};
        size_t mapped=0; uint64_t atlas_pixels=0;
        for (size_t q=0;q<metadata_.size();++q) {
            const auto& m=metadata_[q];
            if (m.atlas_scale<=0) continue;
            ++mapped;
            radii[q]=gpu::ParticleDofRadius(1.0f/expanded_[q*4].position[2],p.globals)*m.atlas_scale;
            atlas_pixels+=uint64_t(m.atlas_size&65535u)*(m.atlas_size>>16);
        }
        Require(mapped==Quads,"benchmark fixture did not map every quad to the production atlas");
        radii_cb_=Upload(radii.data(),sizeof(radii));
        std::array<std::array<float,4>,192> vc{};
        for (int j=0;j<4;++j) {vc[97][j]=p.c97[j];vc[98][j]=p.c98[j];}
        vcb_=Upload(vc.data(),sizeof(vc));
        if (VisibilityBenchmark) {
            constexpr size_t maps_bytes=gpu::kParticleDofMaxQuads*
                sizeof(gpu::ParticleDofVisibilityMap);
            std::vector<uint8_t> constants(sizeof(vc)+16+maps_bytes,0xff);
            std::memcpy(constants.data(),vc.data(),sizeof(vc));
            const float cache_target[4]{float(AtlasExtent),float(AtlasExtent),
                                        float(visibility_halo_),0};
            std::memcpy(constants.data()+sizeof(vc),cache_target,sizeof(cache_target));
            std::memcpy(constants.data()+sizeof(vc)+sizeof(cache_target),
                        visibility_maps_.data(),visibility_maps_.size()*
                        sizeof(gpu::ParticleDofVisibilityMap));
            static_assert(sizeof(vc)==3072 && sizeof(gpu::ParticleDofVisibilityMap)==8);
            visibility_vcb_=Upload(constants.data(),constants.size());
            if (CompareProducerEpochs || CompareCacheShader) {
                std::memcpy(constants.data()+sizeof(vc)+sizeof(cache_target),
                            epoch_visibility_maps_.data(),epoch_visibility_maps_.size()*
                            sizeof(gpu::ParticleDofVisibilityMap));
                epoch_visibility_vcb_=Upload(constants.data(),constants.size());
            }
        }
        gpu::DrawConstants dc{};
        dc.surf[0]=float(W);dc.surf[1]=float(H);dc.surf[2]=1;
        dc.misc[1]=7;dc.texsize[0][0]=dc.texsize[0][1]=32;
        // v25d4110 has a 48-byte globals block and 32-byte quad records. Never memcpy the current stride.
        constexpr size_t old_globals=48, old_quad=32;
        constexpr size_t legacy_prefix=offsetof(gpu::DrawConstants,rtshift);
        static_assert(legacy_prefix==2080 && sizeof(gpu::DrawConstants)==2096);
        static_assert(offsetof(gpu::ParticleDofGlobals,atlas_target)==48 &&
                      offsetof(gpu::ParticleDofGlobals,projection)==64 &&
                      sizeof(gpu::ParticleDofGlobals)==80 &&
                      sizeof(gpu::ParticleDofQuad)==48);
        std::vector<uint8_t> old_cb(legacy_prefix+old_globals+gpu::kParticleDofMaxQuads*old_quad);
        std::memcpy(old_cb.data(),&dc,legacy_prefix);
        std::memcpy(old_cb.data()+legacy_prefix,&p.globals,old_globals);
        for (size_t i=0;i<metadata_.size();++i)
            std::memcpy(old_cb.data()+legacy_prefix+old_globals+i*old_quad,&metadata_[i],old_quad);
        old_dcb_=Upload(old_cb.data(),old_cb.size());
        for (const auto page:pages_) {
            if (F793Benchmark && Samples==2 && !GatherBenchmark && !VisibilityBenchmark) {
                auto admission=gpu::BuildParticleSharedCoverage(expanded_,metadata_,p,
                                                                 page,Samples,true);
                std::printf("shared page %u..%u: rejection %u, sum/union %.1f/%.1f, "
                            "bounds [%d,%d,%d,%d]\n",page.first_quad,
                            page.first_quad+page.quad_count,uint32_t(admission.rejection),
                            admission.sum_area,admission.union_area,admission.bounds[0],
                            admission.bounds[1],admission.bounds[2],admission.bounds[3]);
                Require(bool(admission),
                        "benchmark fixture did not enter the production shared coverage path");
                shared_vbs_.push_back(Upload(admission.vertices.data(),
                    admission.vertices.size()*sizeof(gpu::NativeParticleVertex)));
                shared_infos_.push_back(admission);
            }
            auto globals=p.globals;
            globals.atlas_target[0]=globals.atlas_target[1]=float(AtlasExtent);
            globals.atlas_target[2]=float(page.first_quad);
            globals.atlas_target[3]=float(page.quad_count);
            std::vector<uint8_t> cb(sizeof(dc)+sizeof(globals)+
                                    gpu::kParticleDofMaxQuads*sizeof(gpu::ParticleDofQuad));
            std::memcpy(cb.data(),&dc,sizeof(dc));
            std::memcpy(cb.data()+sizeof(dc),&globals,sizeof(globals));
            std::memcpy(cb.data()+sizeof(dc)+sizeof(globals),metadata_.data(),
                        metadata_.size()*sizeof(metadata_[0]));
            page_dcbs_.push_back(Upload(cb.data(),cb.size()));
            if (F793Benchmark && Samples==2 && !GatherBenchmark && !VisibilityBenchmark) {
                auto field_cb=cb;
                const size_t first_slot=sizeof(dc)+sizeof(globals);
                auto field_globals=globals;
                field_globals.target[2]=1;
                std::memcpy(field_cb.data()+sizeof(dc),&field_globals,
                            sizeof(field_globals));
                std::memcpy(field_cb.data()+first_slot,&metadata_[page.first_quad],
                            sizeof(gpu::ParticleDofQuad));
                field_page_dcbs_.push_back(Upload(field_cb.data(),field_cb.size()));
            }
            constexpr size_t legacy_page_prefix=offsetof(gpu::DrawConstants,rtshift);
            static_assert(legacy_page_prefix==2080 && sizeof(dc)==2096);
            constexpr size_t legacy_globals=offsetof(gpu::ParticleDofGlobals,projection);
            static_assert(legacy_globals==64 && sizeof(globals)==80);
            static_assert(sizeof(gpu::ParticleDofQuad)==48);
            std::vector<uint8_t> legacy_cb(legacy_page_prefix+legacy_globals+
                gpu::kParticleDofMaxQuads*sizeof(gpu::ParticleDofQuad));
            std::memcpy(legacy_cb.data(),&dc,legacy_page_prefix);
            std::memcpy(legacy_cb.data()+legacy_page_prefix,&globals,legacy_globals);
            std::memcpy(legacy_cb.data()+legacy_page_prefix+legacy_globals,metadata_.data(),
                        metadata_.size()*sizeof(metadata_[0]));
            Require(legacy_cb.size()+(sizeof(dc)-legacy_page_prefix)+
                    (sizeof(globals)-legacy_globals)==cb.size(),
                    "frozen filter constant-buffer tail is not exactly 32 bytes shorter");
            legacy_page_dcbs_.push_back(Upload(legacy_cb.data(),legacy_cb.size()));
            std::vector<D3D12_RECT> rectangles;
            for (uint32_t q=page.first_quad;q<page.first_quad+page.quad_count;++q) {
                const auto& m=metadata_[q];
                if (m.atlas_scale<=0) continue;
                const LONG x=LONG(m.atlas_xy&65535u),y=LONG(m.atlas_xy>>16);
                rectangles.push_back({x,y,x+LONG(m.atlas_size&65535u),y+LONG(m.atlas_size>>16)});
            }
            Require(!rectangles.empty(),"benchmark page contains no source cells");
            page_rects_.push_back(std::move(rectangles));
        }
        if (VisibilityBenchmark) {
            for (const auto& page:visibility_pages_) {
                auto globals=p.globals;
                globals.atlas_target[0]=globals.atlas_target[1]=float(AtlasExtent);
                globals.atlas_target[2]=float(page.first_quad);
                globals.atlas_target[3]=float(page.quad_count);
                std::vector<uint8_t> cb(sizeof(dc)+sizeof(globals)+
                    gpu::kParticleDofMaxQuads*sizeof(gpu::ParticleDofQuad));
                std::memcpy(cb.data(),&dc,sizeof(dc));
                std::memcpy(cb.data()+sizeof(dc),&globals,sizeof(globals));
                std::memcpy(cb.data()+sizeof(dc)+sizeof(globals),metadata_.data(),
                            metadata_.size()*sizeof(metadata_[0]));
                visibility_page_dcbs_.push_back(Upload(cb.data(),cb.size()));
                std::vector<D3D12_RECT> rectangles;
                for (uint32_t q=page.first_quad;q<page.first_quad+page.quad_count;++q) {
                    const auto& m=metadata_[q];
                    if (m.atlas_scale<=0) continue;
                    const LONG x=LONG(m.atlas_xy&65535u),y=LONG(m.atlas_xy>>16);
                    rectangles.push_back({x,y,x+LONG(m.atlas_size&65535u),
                                               y+LONG(m.atlas_size>>16)});
                }
                Require(!rectangles.empty(),"visibility chunk has no color source cells");
                visibility_rects_.push_back(std::move(rectangles));
            }
        }
        std::printf("Fixture: %ux%u, actual radius %.3f px, %u quads, %u batches, "
                    "%zu production atlas pages at %ux%u; "
                    "%llu allocated texels/page-set (guards included); R16G16B16A16_FLOAT atlas\n",
                    W,H,TargetRadius,Quads,Batches,pages_.size(),AtlasExtent,AtlasExtent,
                    static_cast<unsigned long long>(atlas_pixels));
        // The same colored, partially transparent material is used by every mode.
        texture_=MakeTexture(TextureDesc(DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_FLAG_NONE,1,32,32),D3D12_RESOURCE_STATE_COPY_DEST);
        std::array<uint8_t,256*32> texels{};
        for (uint32_t y=0;y<32;++y)
            for (uint32_t x=0;x<32;++x) {
                const uint32_t hash=(x*71u+y*131u+x*y*19u)^((x+y*3u)<<4);
                const size_t offset=size_t(y)*256+x*4;
                texels[offset]=uint8_t(32+(hash&223u));
                texels[offset+1]=uint8_t(16+((hash>>8)&239u));
                texels[offset+2]=uint8_t((hash>>16)&255u);
                texels[offset+3]=(x==16 && y==16)?255:uint8_t((hash>>2)&127u);
            }
        auto staging=Upload(texels.data(),texels.size());
        ComPtr<ID3D12CommandAllocator> a;ComPtr<ID3D12GraphicsCommandList> l;
        Hr(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a)),"Tex allocator");
        Hr(device_->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,a.Get(),nullptr,
            IID_PPV_ARGS(&l)),"Tex list");
        D3D12_TEXTURE_COPY_LOCATION dst{},src{};
        dst.pResource=texture_.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource=staging.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        src.PlacedFootprint.Footprint.Width=src.PlacedFootprint.Footprint.Height=32;
        src.PlacedFootprint.Footprint.Depth=1;src.PlacedFootprint.Footprint.RowPitch=256;
        l->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        auto b=Barrier(texture_.Get(),D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        l->ResourceBarrier(1,&b);Hr(l->Close(),"Close tex list");
        ID3D12CommandList* lists[]{l.Get()};queue_->ExecuteCommandLists(1,lists);
        const UINT64 fv=++fence_value_;Hr(queue_->Signal(fence_.Get(),fv),"Tex fence");
        Hr(fence_->SetEventOnCompletion(fv,event_),"Tex event");
        Require(WaitForSingleObject(event_,30000)==WAIT_OBJECT_0,"Texture init timed out");
    }
    Descriptors MakeDescriptors(ID3D12Resource* snapshot,ID3D12Resource* range,
                                ID3D12Resource* target,ID3D12Resource* source,
                                ID3D12Resource* filtered,ID3D12Resource* sample_target,
                                bool float_range,uint32_t range_levels,
                                 ID3D12Resource* support,ID3D12Resource* transmission,
                                 ID3D12Resource* visibility) {
        Descriptors d;
        D3D12_DESCRIPTOR_HEAP_DESC h{};
        h.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        h.NumDescriptors=32+4*(range_levels-1);
        h.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Hr(device_->CreateDescriptorHeap(&h,IID_PPV_ARGS(&d.srv_heap)),"SRV heap");
        const auto cpu=d.srv_heap->GetCPUDescriptorHandleForHeapStart();
        const UINT step=device_->GetDescriptorHandleIncrementSize(h.Type);
        auto at=[&](UINT index) { auto handle=cpu;handle.ptr+=size_t(index)*step;return handle; };
        auto view2d=[&](UINT index,ID3D12Resource* resource,DXGI_FORMAT format,
                        UINT first_mip=0,UINT mip_count=1) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Texture2D.MostDetailedMip=first_mip;
            view.Texture2D.MipLevels=mip_count;view.Format=format;
            device_->CreateShaderResourceView(resource,&view,at(index));
        };
        for (UINT table=0;table<3;++table) {
            const UINT base=table*4;
            view2d(base,table==1?source:texture_.Get(),
                   table==1?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R8G8B8A8_UNORM);
            view2d(base+1,table==2?filtered:nullptr,DXGI_FORMAT_R16G16B16A16_FLOAT);
            view2d(base+2,table==1?nullptr:range,
                   float_range?DXGI_FORMAT_R32G32_FLOAT:DXGI_FORMAT_R32G32_UINT,
                   0,table==1?1:range_levels);
            D3D12_SHADER_RESOURCE_VIEW_DESC depth{};
            depth.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            depth.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DMS;
            depth.Format=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            device_->CreateShaderResourceView(table==1?nullptr:snapshot,&depth,at(base+3));
        }
        d.native_srv=d.srv_heap->GetGPUDescriptorHandleForHeapStart();
        d.filter_srv=d.native_srv;d.filter_srv.ptr+=4*UINT64(step);
        d.composite_srv=d.native_srv;d.composite_srv.ptr+=8*UINT64(step);
        if (support && transmission) {
            view2d(16,texture_.Get(),DXGI_FORMAT_R8G8B8A8_UNORM);
            D3D12_SHADER_RESOURCE_VIEW_DESC support_view{};
            support_view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            support_view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DMS;
            support_view.Format=DXGI_FORMAT_R8_UNORM;
            device_->CreateShaderResourceView(support,&support_view,at(17));
            view2d(18,range,DXGI_FORMAT_R32G32_UINT);
            D3D12_SHADER_RESOURCE_VIEW_DESC depth_view{};
            depth_view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            depth_view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DMS;
            depth_view.Format=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            device_->CreateShaderResourceView(snapshot,&depth_view,at(19));
            view2d(20,texture_.Get(),DXGI_FORMAT_R8G8B8A8_UNORM);
            view2d(21,filtered,DXGI_FORMAT_R16G16B16A16_FLOAT);
            view2d(22,transmission,DXGI_FORMAT_R32G32_FLOAT);
            device_->CreateShaderResourceView(snapshot,&depth_view,at(23));
            d.transmission_srv=d.native_srv;d.transmission_srv.ptr+=16*UINT64(step);
            d.shared_composite_srv=d.native_srv;d.shared_composite_srv.ptr+=20*UINT64(step);
        }
        if (sample_target) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sample_view{};
            sample_view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sample_view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DMS;
            sample_view.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
            device_->CreateShaderResourceView(target,&sample_view,at(24));
            for (UINT slot=25;slot<28;++slot)
                device_->CreateShaderResourceView(nullptr,&sample_view,at(slot));
            d.sample_srv=d.native_srv;d.sample_srv.ptr+=24*UINT64(step);
        }
        if (visibility) {
            view2d(28,range,DXGI_FORMAT_R32G32_FLOAT);
            view2d(29,filtered,DXGI_FORMAT_R16G16B16A16_FLOAT);
            view2d(30,visibility,DXGI_FORMAT_R16_FLOAT);
            D3D12_SHADER_RESOURCE_VIEW_DESC depth{};
            depth.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            depth.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DMS;
            depth.Format=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            device_->CreateShaderResourceView(snapshot,&depth,at(31));
            d.visibility_composite_srv=d.native_srv;
            d.visibility_composite_srv.ptr+=28*UINT64(step);
        }
        h={};h.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        h.NumDescriptors=8+(range_levels-1);
        Hr(device_->CreateDescriptorHeap(&h,IID_PPV_ARGS(&d.rtv_heap)),"RTV heap");
        const UINT rtv_step=device_->GetDescriptorHandleIncrementSize(h.Type);
        d.range_rtv=d.rtv_heap->GetCPUDescriptorHandleForHeapStart();
        d.target_rtv=d.range_rtv;d.target_rtv.ptr+=rtv_step;
        d.source_rtv=d.target_rtv;d.source_rtv.ptr+=rtv_step;
        d.filtered_rtv=d.source_rtv;d.filtered_rtv.ptr+=rtv_step;
        d.sample_rtv=d.filtered_rtv;d.sample_rtv.ptr+=rtv_step;
        d.support_rtv=d.sample_rtv;d.support_rtv.ptr+=rtv_step;
        d.transmission_rtv=d.support_rtv;d.transmission_rtv.ptr+=rtv_step;
        d.visibility_rtv=d.transmission_rtv;d.visibility_rtv.ptr+=rtv_step;
        D3D12_RENDER_TARGET_VIEW_DESC range_rtv_desc{};
        range_rtv_desc.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
        range_rtv_desc.Format=float_range?DXGI_FORMAT_R32G32_FLOAT:DXGI_FORMAT_R32G32_UINT;
        device_->CreateRenderTargetView(range,&range_rtv_desc,d.range_rtv);
        device_->CreateRenderTargetView(target,nullptr,d.target_rtv);
        if (source) device_->CreateRenderTargetView(source,nullptr,d.source_rtv);
        if (filtered) device_->CreateRenderTargetView(filtered,nullptr,d.filtered_rtv);
        if (sample_target) device_->CreateRenderTargetView(sample_target,nullptr,d.sample_rtv);
        if (support) device_->CreateRenderTargetView(support,nullptr,d.support_rtv);
        if (transmission) device_->CreateRenderTargetView(transmission,nullptr,
                                                          d.transmission_rtv);
        if (visibility) device_->CreateRenderTargetView(visibility,nullptr,d.visibility_rtv);
        h={};h.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;h.NumDescriptors=2;
        Hr(device_->CreateDescriptorHeap(&h,IID_PPV_ARGS(&d.dsv_heap)),"DSV heap");
        d.dsv=d.dsv_heap->GetCPUDescriptorHandleForHeapStart();
        d.dsv_read=d.dsv;
        d.dsv_read.ptr+=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        return d;
    }
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Fence> fence_;
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> native_pso_,old_pso_,source_pso_,filter_pso_,candidate_pso_,
                                 composite_pso_,range_pso_,coverage_before_pso_,coverage_after_pso_,
                                 coverage_before_source_pso_,float_range_pso_,sample_pso_,
                                 f793_source_pso_,f793_composite_pso_,
                                 support_pso_,transmission_pso_,shared_composite_pso_;
    ComPtr<ID3D12PipelineState> visibility_source_pso_,visibility_composite_pso_,
                                visibility_composite_old_pso_;
    ComPtr<ID3D12PipelineState> mixed_sample0_pso_,mixed_sample1_pso_;
    ComPtr<ID3D12Resource> native_vb_,vb_,ib_,vcb_,visibility_vcb_,
                          epoch_visibility_vcb_,old_dcb_,radii_cb_,texture_;
    std::vector<ComPtr<ID3D12Resource>> page_dcbs_;
    std::vector<ComPtr<ID3D12Resource>> visibility_page_dcbs_;
    std::vector<ComPtr<ID3D12Resource>> legacy_page_dcbs_;
    std::vector<ComPtr<ID3D12Resource>> field_page_dcbs_,shared_vbs_;
    std::vector<gpu::ParticleSharedCoverage> shared_infos_;
    std::vector<gpu::ParticleDofAtlasPage> pages_;
    std::vector<gpu::ParticleDofVisibilityPage> visibility_pages_;
    std::vector<gpu::ParticleDofVisibilityMap> visibility_maps_;
    std::vector<gpu::ParticleDofVisibilityPage> epoch_visibility_pages_;
    std::vector<gpu::ParticleDofVisibilityMap> epoch_visibility_maps_;
    std::vector<gpu::ParticleDofVisibilityEpoch> visibility_epochs_;
    uint32_t visibility_halo_=0;
    std::vector<std::vector<D3D12_RECT>> page_rects_;
    std::vector<std::vector<D3D12_RECT>> visibility_rects_;
    std::vector<gpu::NativeParticleVertex> expanded_;
    std::vector<gpu::ParticleDofQuad> metadata_;
    std::vector<uint16_t> indices_;
    UINT64 frequency_=0,fence_value_=0;
    HANDLE event_=nullptr;
};

double Percentile(std::vector<double> x,double fraction) {
    std::sort(x.begin(),x.end());
    const size_t i=std::min(x.size()-1,size_t(std::ceil(fraction*x.size())-1));
    return x[i];
}
float FromHalf(uint16_t bits) {
    const unsigned exponent=(bits>>10)&31u, fraction=bits&1023u;
    const float magnitude=exponent==0 ? std::ldexp(float(fraction),-24) :
                          exponent==31 ? INFINITY :
                          std::ldexp(float(1024u+fraction),int(exponent)-25);
    return bits&0x8000u ? -magnitude : magnitude;
}
double MedianTotal(const std::vector<Result>& runs) {
    std::vector<double> values;
    for (const auto& run:runs) values.push_back(run.ms.total);
    return Percentile(std::move(values),.5);
}
void Print(const char* name,const std::vector<Result>& runs) {
    std::printf("%s GPU ms median/p95:",name);
    const std::pair<const char*,double Intervals::*> fields[]{
        {"copy",&Intervals::copy},{"range",&Intervals::range},
        {"source",&Intervals::prepare},
        {"filter",&Intervals::filter},{"support",&Intervals::support},
        {"transmission",&Intervals::transmission},
        {"composite",&Intervals::composite},{"total",&Intervals::total}};
    for (const auto& [label,member]:fields) {
        std::vector<double> values;
        for (const auto& run:runs) values.push_back(run.ms.*member);
        std::printf(" %s %.3f/%.3f",label,Percentile(values,.5),Percentile(values,.95));
    }
    std::puts("");
}
struct Difference {
    float maximum=0;
    double rms=0,active_rms=0,rgb_energy_percent=0,alpha_energy_percent=0;
    size_t max_pixel=0,max_channel=0;
    float max_before=0,max_after=0;
};
Difference Compare(const Result& reference,const Result& candidate,const char* label) {
    Require(reference.rgba.size()==candidate.rgba.size(),"RGBA readback sizes differ");
    constexpr float clear[4]{.02f,.03f,.05f,.1f};
    double squared=0,active_squared=0,old_energy[4]{},new_energy[4]{};
    size_t active_pixels=0;
    Difference difference;
    for (size_t pixel=0;pixel<reference.rgba.size()/4;++pixel) {
        double pixel_squared=0;
        bool active=false;
        for (size_t channel=0;channel<4;++channel) {
            const size_t index=pixel*4+channel;
            const float a=FromHalf(reference.rgba[index]),b=FromHalf(candidate.rgba[index]);
            Require(std::isfinite(a)&&std::isfinite(b),"benchmark RGBA is nonfinite");
            const float delta=std::abs(a-b);
            if (delta>difference.maximum) {
                difference.maximum=delta;
                difference.max_pixel=pixel;
                difference.max_channel=channel;
                difference.max_before=a;
                difference.max_after=b;
            }
            pixel_squared+=double(delta)*delta;
            old_energy[channel]+=std::abs(double(a)-clear[channel]);
            new_energy[channel]+=std::abs(double(b)-clear[channel]);
            active|=std::abs(a-clear[channel])>1e-4f||std::abs(b-clear[channel])>1e-4f;
        }
        squared+=pixel_squared;
        if (active) { ++active_pixels;active_squared+=pixel_squared; }
    }
    difference.rms=std::sqrt(squared/reference.rgba.size());
    difference.active_rms=active_pixels?std::sqrt(active_squared/(active_pixels*4)):0;
    const double old_rgb=old_energy[0]+old_energy[1]+old_energy[2];
    const double new_rgb=new_energy[0]+new_energy[1]+new_energy[2];
    difference.rgb_energy_percent=100*std::abs(new_rgb-old_rgb)/std::max(old_rgb,1e-12);
    difference.alpha_energy_percent=100*std::abs(new_energy[3]-old_energy[3])/std::max(old_energy[3],1e-12);
    std::printf("%s RGBA16F: max %.6f, full RMS %.6f, active RMS %.6f (%zu pixels), "
                "absolute background-relative RGB/alpha energy delta %.4f%%/%.4f%%",
                label,difference.maximum,difference.rms,difference.active_rms,active_pixels,
                difference.rgb_energy_percent,difference.alpha_energy_percent);
    if (difference.maximum>0) {
        const size_t row_width=reference.rgba.size()/4==size_t(W)*H?W:64;
        std::printf("; max at (%zu,%zu) channel %zu: %.8f -> %.8f",
                    difference.max_pixel%row_width,difference.max_pixel/row_width,
                    difference.max_channel,difference.max_before,difference.max_after);
    }
    std::puts("");
    return difference;
}

size_t CountBlurredWallPixels(const Result& reference) {
    Require(reference.rgba.size()==size_t(W)*H*4,"coverage occupancy readback size differs");
    const uint32_t period=std::max(4u,uint32_t(std::round(4*TargetRadius)));
    const uint32_t opening=std::max(2u,period/2);
    size_t count=0;
    for (uint32_t y=H*3/10;y<H*7/10;++y)
        for (uint32_t x=W*35/100;x<W*65/100;++x) {
            if (x%period<opening) continue;
            const auto pixel=(size_t(y)*W+x)*4;
            if (std::abs(FromHalf(reference.rgba[pixel])-.02f)>.002f)
                ++count;
        }
    std::printf("Blocked-wall pixels receiving blur in central smoke ROI: %zu\n",count);
    Require(count>size_t(W)*H/500,
            "coverage fixture did not exercise blurred source behind blocked wall");
    return count;
}

void WriteBmp(const char* name,const Result& result) {
    Require(result.rgba.size()==size_t(W)*H*4,"BMP readback extent differs");
    const auto dir=std::filesystem::path("out")/"reports"/"particle-optimization";
    std::filesystem::create_directories(dir);
    std::ofstream out(dir/name,std::ios::binary);
    Require(bool(out),"cannot open particle benchmark BMP");
    const uint32_t pitch=(W*3+3)&~3u;
    auto u16=[&](uint16_t x){out.put(char(x&255));out.put(char(x>>8));};
    auto u32=[&](uint32_t x){u16(uint16_t(x));u16(uint16_t(x>>16));};
    out.write("BM",2);u32(54+pitch*H);u32(0);u32(54);
    u32(40);u32(W);u32(H);u16(1);u16(24);
    u32(0);u32(pitch*H);u32(2835);u32(2835);u32(0);u32(0);
    std::vector<char> row(pitch,0);
    const auto byte=[](float v){return char(int(std::round(std::clamp(v,0.0f,1.0f)*255)));};
    for (uint32_t iy=0;iy<H;++iy) {
        const uint32_t y=H-1-iy;
        for (uint32_t x=0;x<W;++x) {
            const auto* pixel=result.rgba.data()+(size_t(y)*W+x)*4;
            row[3*x]=byte(FromHalf(pixel[2]));
            row[3*x+1]=byte(FromHalf(pixel[1]));
            row[3*x+2]=byte(FromHalf(pixel[0]));
        }
        out.write(row.data(),row.size());
    }
    Require(bool(out),"particle benchmark BMP write failed");
}

} // namespace

int main(int argc,char** argv) {
    try {
        bool samples_set=false;
        uint32_t requested_width=0;
        for (int arg=1;arg<argc;++arg) {
            if (std::strcmp(argv[arg],"--coverage")==0) {
                CoverageBenchmark=true;
            } else if (std::strcmp(argv[arg],"--coverage-baseline")==0) {
                CoverageBenchmark=F793Benchmark=true;
            } else if (std::strcmp(argv[arg],"--gather")==0) {
                GatherBenchmark=CoverageBenchmark=F793Benchmark=SharedOff=true;
            } else if (std::strcmp(argv[arg],"--visibility-cache")==0) {
                VisibilityBenchmark=CoverageBenchmark=F793Benchmark=SharedOff=true;
            } else if (std::strcmp(argv[arg],"--group-color-pages")==0) {
                GroupColorPages=true;
            } else if (std::strcmp(argv[arg],"--mixed-cache-eligibility")==0) {
                MixedCacheEligibility=true;
            } else if (std::strcmp(argv[arg],"--compare-color-batching")==0) {
                CompareColorBatching=true;
            } else if (std::strcmp(argv[arg],"--compare-producer-epochs")==0) {
                CompareProducerEpochs=true;
            } else if (std::strcmp(argv[arg],"--compare-cache-shader")==0) {
                CompareCacheShader=true;
            } else if (std::strcmp(argv[arg],"--mixed-samples")==0) {
                MixedSamples=true;
            } else if (std::strcmp(argv[arg],"--additive")==0) {
                AdditiveBenchmark=true;
            } else if (std::strcmp(argv[arg],"--warp")==0) {
                WarpCorrectness=true;
            } else if (std::strcmp(argv[arg],"--zero-w")==0) {
                ZeroInputW=true;
            } else if (std::strcmp(argv[arg],"--scene")==0 && arg+1<argc) {
                const char* scene=argv[++arg];
                Require(std::strcmp(scene,"occluded")==0 ||
                        std::strcmp(scene,"visible")==0,
                        "--scene must be occluded or visible");
                SceneVisible=std::strcmp(scene,"visible")==0;
            } else if (std::strcmp(argv[arg],"--shared-off")==0) {
                SharedOff=true;
            } else if (std::strcmp(argv[arg],"--samples")==0 && arg+1<argc) {
                const int samples=std::atoi(argv[++arg]);
                Require(samples==2 || samples==8,"--samples must be 2 or 8");
                Samples=uint32_t(samples);samples_set=true;
            } else if (std::strcmp(argv[arg],"--quads")==0 && arg+1<argc) {
                const int quads=std::atoi(argv[++arg]);
                Require(quads>=1 && quads<=64,"--quads must be in 1..64");
                Quads=uint32_t(quads);
            } else if (std::strcmp(argv[arg],"--quad-scale")==0 && arg+1<argc) {
                char* end=nullptr;
                QuadScale=std::strtof(argv[++arg],&end);
                Require(end!=argv[arg] && *end=='\0' && std::isfinite(QuadScale) &&
                        QuadScale>=.25f && QuadScale<=1.0f,
                        "--quad-scale must be in [.25,1]");
            } else if (std::strcmp(argv[arg],"--layers")==0 && arg+1<argc) {
                const int layers=std::atoi(argv[++arg]);
                Require(layers>=1 && layers<=16,"--layers must be in 1..16");
                Batches=uint32_t(layers);
            } else if (std::strcmp(argv[arg],"--warmup")==0 && arg+1<argc) {
                const int count=std::atoi(argv[++arg]);
                Require(count>=1 && count<=16,"--warmup must be in 1..16");
                Warmups=uint32_t(count);
            } else if (std::strcmp(argv[arg],"--repeat")==0 && arg+1<argc) {
                const int count=std::atoi(argv[++arg]);
                Require(count>=8 && count<=64 && count%2==0,
                        "--repeat must be an even count in 8..64 per variant");
                MeasuredPairs=uint32_t(count);
            } else if (std::strcmp(argv[arg],"--width")==0 && arg+1<argc) {
                const int width=std::atoi(argv[++arg]);
                Require(width>=64 && width<=4096,
                        "--width must be in 64..4096 pixels");
                requested_width=uint32_t(width);
            } else if (std::strcmp(argv[arg],"--lequal")==0) {
                DepthLessEqual=true;
            } else
            if (std::strcmp(argv[arg],"--height")==0 && arg+1<argc) {
                const int height=std::atoi(argv[++arg]);
                Require(height>=64 && height<=2160,"--height must be in 64..2160");
                H=uint32_t(height);W=H==720?1280:H==2160?3840:128;
                AtlasExtent=H==2160?4096:2048;
            } else if (std::strcmp(argv[arg],"--radius")==0 && arg+1<argc) {
                char* end=nullptr;
                TargetRadius=std::strtof(argv[++arg],&end);
                Require(end!=argv[arg] && *end=='\0' && std::isfinite(TargetRadius) &&
                        TargetRadius>0 && TargetRadius<=24,"--radius must be in (0,24]");
            } else if (std::strcmp(argv[arg],"--wall-z")==0 && arg+1<argc) {
                char* end=nullptr;
                WallZ=std::strtof(argv[++arg],&end);
                Require(end!=argv[arg] && *end=='\0' && std::isfinite(WallZ) &&
                        WallZ>1 && WallZ<=100,
                        "--wall-z must be finite in (1,100]");
            } else if ((std::strcmp(argv[arg],"--focus")==0 ||
                        std::strcmp(argv[arg],"--near")==0 ||
                        std::strcmp(argv[arg],"--far")==0) && arg+1<argc) {
                const char* option=argv[arg];
                char* end=nullptr;
                const float value=std::strtof(argv[++arg],&end);
                Require(end!=argv[arg] && *end=='\0' && std::isfinite(value) &&
                        value>0 && value<=100,
                        "--focus/--near/--far must be finite in (0,100]");
                if (std::strcmp(option,"--focus")==0) FocusDistance=value;
                else if (std::strcmp(option,"--near")==0) NearScale=value;
                else FarScale=value;
            } else if (std::strcmp(argv[arg],"--opt")==0 && arg+1<argc) {
                const char* level=argv[++arg];
                Require(std::strcmp(level,"O1")==0 || std::strcmp(level,"O3")==0,
                        "--opt must be O1 or O3");
                CandidateOptimization=std::strcmp(level,"O3")==0 ?
                    D3DCOMPILE_OPTIMIZATION_LEVEL3:D3DCOMPILE_OPTIMIZATION_LEVEL1;
            } else {
                throw std::runtime_error("usage: new_smoke_dof_gpu_bench "
                    "[--coverage|--coverage-baseline|--gather|--visibility-cache] "
                    "[--group-color-pages requires --visibility-cache] "
                    "[--mixed-cache-eligibility requires --visibility-cache] "
                    "[--compare-color-batching requires --visibility-cache --group-color-pages] "
                    "[--compare-producer-epochs requires --visibility-cache --group-color-pages] "
                    "[--compare-cache-shader requires --visibility-cache --group-color-pages] "
                    "[--height 720|2160; --warp also 64..2160] "
                    "[--width 64..4096] [--radius px] "
                    "[--focus worldZ] [--near scale] [--far scale] "
                    "[--wall-z worldZ] [--zero-w] "
                    "[--samples 2|8] [--quads 1..64] "
                    "[--quad-scale .25..1] "
                    "[--layers 1..16] [--warmup 1..16] [--repeat even 8..64] "
                    "[--scene occluded|visible] [--lequal] "
                    "[--mixed-samples] [--additive] [--warp] "
                    "[--shared-off for f793 fallback control] "
                    "[--opt O1|O3 candidate]");
            }
        }
        if (requested_width) W=requested_width;
        Require(WarpCorrectness || (H==720 || H==2160) && W>=320,
                "non-WARP benchmark requires 720p/2160p and width >=320");
        Require(!WarpCorrectness || VisibilityBenchmark,
                "--warp is correctness-only for --visibility-cache");
        Require(!MixedSamples || VisibilityBenchmark,
                "--mixed-samples requires --visibility-cache");
        Require(!AdditiveBenchmark || VisibilityBenchmark,
                "--additive requires --visibility-cache");
        if (WarpCorrectness) AtlasExtent=256;
        if (CoverageBenchmark && !samples_set) Samples=2;
        Require(!GatherBenchmark || Samples==2,
                "--gather requires the production 2x MSAA float-bounds variant");
        Require(!GatherBenchmark || CandidateOptimization==D3DCOMPILE_OPTIMIZATION_LEVEL1,
                "--gather compares both production shaders at O1");
        Require(!VisibilityBenchmark || Samples==2,
                "--visibility-cache requires the production 2x MSAA FLOAT bounds variant");
        Require(!GroupColorPages || VisibilityBenchmark,
                "--group-color-pages requires --visibility-cache");
        Require(!MixedCacheEligibility || VisibilityBenchmark,
                "--mixed-cache-eligibility requires --visibility-cache");
        Require(!CompareColorBatching || (VisibilityBenchmark && GroupColorPages),
                "--compare-color-batching requires --visibility-cache --group-color-pages");
        Require(!CompareProducerEpochs || (VisibilityBenchmark && GroupColorPages &&
                !CompareColorBatching && !CompareCacheShader),
                "--compare-producer-epochs requires grouped Vq color pages without another comparison mode");
        Require(!CompareCacheShader || (VisibilityBenchmark && GroupColorPages &&
                !CompareColorBatching && !CompareProducerEpochs),
                "--compare-cache-shader requires grouped Vq color pages without another comparison mode");
        Require(!VisibilityBenchmark || CandidateOptimization==D3DCOMPILE_OPTIMIZATION_LEVEL1,
                "--visibility-cache compares both particle shaders at O1");
        Require(!CoverageBenchmark || F793Benchmark ||
                    CandidateOptimization==D3DCOMPILE_OPTIMIZATION_LEVEL1,
                "non-f793 coverage comparison requires both production variants at O1");
        Require(!SharedOff || (F793Benchmark && Samples==2),
                "--shared-off requires --coverage-baseline --samples 2");
        if (CoverageBenchmark) {
            std::printf("Blur-aware coverage benchmark (%s): %ux%u, %ux D24S8 destination, "
                        "radius %.3f px, %u quads x %u layers, quad scale %.2f, %s scene, native %s, "
                        "candidate %s/%s; "
                        "%u warmups and %u measured runs/variant in ABBA order\n",
                         CompareCacheShader?"original versus guarded cached-aperture shader":
                         CompareProducerEpochs?"old versus epoch Vq producer schedule":
                         CompareColorBatching?"split versus grouped Vq color pages":
                         VisibilityBenchmark?"bc30216 versus Vq visibility cache":
                         GatherBenchmark?"production UINT versus FLOAT gather":
                        F793Benchmark?"f793 shared coverage":"c591 quantized",W,H,Samples,
                        TargetRadius,Quads,Batches,QuadScale,
                         SceneVisible?"all-visible":"wall with openings",
                        DepthLessEqual?"LEQUAL":"LESS",
                        SharedOff?"direct fallback":"shared field",
                        CandidateOptimization==D3DCOMPILE_OPTIMIZATION_LEVEL3?"O3":"O1",
                        Warmups,MeasuredPairs);
            if (GatherBenchmark)
                std::puts("Gather workload: ordered per-quad view Z 8/10/12/16, identical geometry, "
                          "textures, range/mask and O1 flags; only UINT versus FLOAT bounds switch.");
            if (VisibilityBenchmark)
                std::printf("Vq workload: ordered per-quad view Z 8/10/12/16, "
                            "focus %.3f near %.3f far %.3f, wall Z %.3f, input W %.2f; "
                            "same FLOAT D24 base range "
                            "and atlas material/filter; candidate times production cache "
                            "page splits, Vq generation and cached composition.\n",
                            FocusDistance,NearScale,FarScale,WallZ,ZeroInputW?0.0f:2.25f);
            if (GroupColorPages)
                std::puts("Grouped color pages: source/filter once per original atlas page, "
                          "then ordered visibility-cache/fallback chunks; baseline uses "
                          "the original color pages.");
            if (CompareColorBatching)
                std::puts("Color batching parity: before uses split Vq color pages and after "
                          "uses grouped parent color pages; shaders and cache maps are identical.");
            if (CompareProducerEpochs)
                std::puts("Producer epoch parity: both modes filter each original color page once; "
                          "before resets Vq packing after fallback, after keeps disjoint cells "
                          "and runs one producer across each cached epoch. Ordered composites "
                          "and all shader bytecode remain identical.");
            if (CompareCacheShader)
                std::puts("Cached-aperture parity: both modes use identical producer epochs "
                          "and maps; only PASS7's proven-interior gather shortcut changes.");
            if (MixedCacheEligibility)
                std::puts("Mixed cache eligibility: alternating native quads are 16x16 px "
                          "and must use direct fallback without losing color filtering.");
            if (MixedSamples)
                std::puts("Mixed 2x control: sample0 z4 wall/red, sample1 far/blue in a center stripe; "
                          "preparation is outside GPU timestamps for both variants.");
            if (AdditiveBenchmark)
                std::puts("Additive control: source P/Q and native SRC_ALPHA/ONE RGB+alpha blending.");
            if (WarpCorrectness)
                std::puts("WARP correctness run: software adapter timings are not GPU performance evidence.");
            const auto cold_start=std::chrono::steady_clock::now();
            Bench bench;
            const double cold_seconds=std::chrono::duration<double>(
                std::chrono::steady_clock::now()-cold_start).count();
            std::printf("Cold device/FXC/PSO/fixture setup %.3fs (excluded from GPU timestamps)\n",
                        cold_seconds);
            for (uint32_t i=0;i<Warmups;++i) {
                bench.Run(F793Benchmark?Mode::F793Before:Mode::CoverageBefore);
                bench.Run(F793Benchmark?Mode::F793After:Mode::CoverageAfter);
            }
            std::vector<Result> before,after;
            const Mode before_mode=F793Benchmark?Mode::F793Before:Mode::CoverageBefore;
            const Mode after_mode=F793Benchmark?Mode::F793After:Mode::CoverageAfter;
            const Mode order[]{before_mode,after_mode,after_mode,before_mode};
            for (uint32_t round=0;round<MeasuredPairs/2;++round)
                for (Mode mode:order) {
                    auto& bucket=mode==before_mode?before:after;
                    auto result=bench.Run(mode,bucket.empty());
                    if (!bucket.empty()) {
                        Require(result.image_hash==bucket.front().image_hash,
                                "repeated hardware image differed within the same shader variant");
                        result.rgba.clear();
                    }
                    bucket.push_back(std::move(result));
                }
            Require(before.size()==MeasuredPairs && after.size()==MeasuredPairs,
                    "coverage ABBA pairing lost a measurement");
            if (!CompareColorBatching && !CompareProducerEpochs && !CompareCacheShader) {
                Print(VisibilityBenchmark?"bc30216 FLOAT direct pass2":
                      GatherBenchmark?"production UINT bounds/direct pass2":
                      F793Benchmark?"f793e2d baseline":"c591162 blur-aware pass2",before);
                Print(VisibilityBenchmark?"Vq cache generation plus pass7":
                      GatherBenchmark?"production FLOAT gather/direct pass2":
                      F793Benchmark?"shared coverage candidate":"quantized blur-aware pass2",after);
            }
            const auto difference=Compare(before.front(),after.front(),
                CompareCacheShader?"old/new cached-aperture shader resolved":
                CompareProducerEpochs?"old/epoch Vq producer resolved":
                CompareColorBatching?"split/grouped Vq resolved":
                VisibilityBenchmark?"bc30216/Vq resolved":
                GatherBenchmark?"production UINT/FLOAT gather resolved":
                F793Benchmark?"f793/shared resolved":"c591/quantized resolved");
            if (!SceneVisible) CountBlurredWallPixels(before.front());
            Result before_samples,after_samples;
            before_samples.rgba=before.front().sample_rgba;
            after_samples.rgba=after.front().sample_rgba;
            const auto sample_difference=Compare(before_samples,after_samples,
                CompareCacheShader?"old/new cached-aperture shader physical MS samples (64x16 ROI)":
                CompareProducerEpochs?"old/epoch Vq physical MS samples (64x16 ROI)":
                CompareColorBatching?"split/grouped Vq physical MS samples (64x16 ROI)":
                VisibilityBenchmark?"bc30216/Vq physical MS samples (64x16 ROI)":
                GatherBenchmark?"production UINT/FLOAT gather physical MS samples (64x16 ROI)":
                F793Benchmark?"f793/shared physical MS samples (64x16 ROI)":
                              "c591/quantized physical MS samples (64x16 ROI)");
            const std::string suffix=std::to_string(H)+"p-r"+std::to_string(int(TargetRadius))+
                "-ms"+std::to_string(Samples)+(DepthLessEqual?"-lequal":"-less")+
                (SceneVisible?"-visible":"-occluded")+
                (VisibilityBenchmark?"-bc30216-vq-cache":
                 GatherBenchmark?"-production-float-gather":
                 F793Benchmark?"-f793-shared":
                                "-c591-quantized")+
                (GroupColorPages?"-grouped-color-pages":"")+
                (MixedCacheEligibility?"-mixed-cache-eligibility":"")+
                (CompareProducerEpochs?"-old-vs-epochs":"")+
                (CompareCacheShader?"-old-vs-cache-shader":"")+
                (CompareColorBatching?"-split-vs-grouped":"")+
                (VisibilityBenchmark?"-wallz"+std::to_string(int(WallZ)):
                                     std::string{})+
                (ZeroInputW?"-zero-w":"")+(MixedSamples?"-mixed":"")+
                (AdditiveBenchmark?"-additive":"");
            WriteBmp(("coverage-before-"+suffix+".bmp").c_str(),before.front());
            WriteBmp(("coverage-after-"+suffix+".bmp").c_str(),after.front());
            if (!CompareColorBatching && !CompareProducerEpochs && !CompareCacheShader) {
                const double old_ms=MedianTotal(before),new_ms=MedianTotal(after);
                std::printf("Coverage total median %.3f -> %.3f ms; composite median ",old_ms,new_ms);
                std::vector<double> old_composite,new_composite;
                for (const auto& r:before) old_composite.push_back(r.ms.composite);
                for (const auto& r:after) new_composite.push_back(r.ms.composite);
                std::printf("%.3f -> %.3f ms; reduction %.2f%% total, %.2f%% composite\n",
                    Percentile(old_composite,.5),Percentile(new_composite,.5),
                    100*(old_ms-new_ms)/std::max(old_ms,1e-9),
                    100*(Percentile(old_composite,.5)-Percentile(new_composite,.5))/
                        std::max(Percentile(old_composite,.5),1e-9));
            }
            if (GatherBenchmark) {
                Require(difference.maximum<=1e-5f && sample_difference.maximum<=1e-5f,
                        "production FLOAT gather changed resolved or physical-MS output");
            }
            if (CompareColorBatching) {
                Require(difference.maximum==0 && sample_difference.maximum==0,
                        "grouping color cells changed resolved or physical-MS output");
            } else if (CompareProducerEpochs) {
                Require(difference.maximum==0 && sample_difference.maximum==0,
                        "producer epoch packing changed resolved or physical-MS output");
            } else if (CompareCacheShader) {
                Require(difference.maximum==0 && sample_difference.maximum==0,
                        "cached-aperture shader changed resolved or physical-MS output");
            } else if (VisibilityBenchmark) {
                Require(difference.maximum<=1e-5f && sample_difference.maximum<=1e-5f,
                        "Vq cache changed resolved or physical-MS output");
            }
            Require(difference.maximum<=.003f && difference.rms<=.0003 &&
                    difference.active_rms<=.0006 &&
                    difference.rgb_energy_percent<=.15 && difference.alpha_energy_percent<=.15,
                    "optimized blur-aware output changed beyond predeclared same-aperture tolerance");
            Require(sample_difference.maximum<=.003f && sample_difference.rms<=.0003 &&
                    sample_difference.active_rms<=.0006 &&
                    sample_difference.rgb_energy_percent<=.15 &&
                    sample_difference.alpha_energy_percent<=.15,
                    "optimized blur-aware physical MS samples changed beyond tolerance");
            if (CompareColorBatching)
                std::puts("Color batching parity PASS: resolved and physical-MS output matched exactly.");
            else if (CompareProducerEpochs)
                std::puts("Producer epoch parity PASS: resolved and physical-MS output matched exactly.");
            else if (CompareCacheShader)
                std::puts("Cached-aperture shader parity PASS: resolved and physical-MS output matched exactly.");
            else
                std::puts("Coverage benchmark PASS; offscreen hardware timestamps are workload "
                          "measurements, not gameplay FPS; GPU clocks and power state were not fixed.");
            return 0;
        }
        // Declared before measurement. These variants use the same source/composite,
        // atlas placement, 64 disk taps and native blend. Only the filter implementation differs.
        constexpr double max_rms=.006, max_active_rms=.012, max_energy_percent=1.5;
        constexpr float max_absolute=.075f;
        std::printf("f543/candidate acceptance: max RGBA <=%.3f, full RMS <=%.3f, "
                    "active RMS <=%.3f, RGB/alpha energy delta <=%.1f%%\n",
                    max_absolute,max_rms,max_active_rms,max_energy_percent);
        std::printf("Configuration: %ux%u, radius %.3f native pixels, f543 FXC O1 / "
                    "candidate FXC %s, 8x depth; 8 warmups + 32 timestamp runs/mode, "
                    "median/p95, no fixed clocks\n",
                    W,H,TargetRadius,CandidateOptimization==D3DCOMPILE_OPTIMIZATION_LEVEL3?"O3":"O1");
        const auto cold_start=std::chrono::steady_clock::now();
        Bench bench;
        const auto cold_seconds=std::chrono::duration<double>(
            std::chrono::steady_clock::now()-cold_start).count();
        std::printf("Cold device/FXC/PSO/fixture setup %.3fs (excluded from GPU timestamps)\n",
                    cold_seconds);
        constexpr Mode candidate=Mode::Candidate;
        for (int warmup=0;warmup<8;++warmup) {
            bench.Run(Mode::Native);
            bench.Run(Mode::Atlas);
            bench.Run(candidate);
        }
        std::vector<Result> native,atlas,optimized;
        // Symmetric ABBA ordering bounds clock drift between the frozen and candidate modes.
        const Mode order[]{Mode::Native,Mode::Atlas,candidate,candidate,Mode::Atlas,Mode::Native,
                           Mode::Native,Mode::Atlas,candidate,candidate,Mode::Atlas,Mode::Native};
        for (unsigned round=0;round<8;++round) {
            for (Mode mode:order) {
                auto result=bench.Run(mode);
                auto& bucket=mode==Mode::Native?native:mode==Mode::Atlas?atlas:optimized;
                if (!bucket.empty()) result.rgba.clear(); // Retain one image per mode, all timings.
                bucket.push_back(std::move(result));
            }
        }
        Require(native.size()==32&&atlas.size()==32&&optimized.size()==32,
                "three-mode ABBA pairing failed");
        Print("native",native);
        Print("f543f54 atlas PS64",atlas);
        Print("alternate PS (nine-load <=1, guarded interior >1)",optimized);
        Compare(native.front(),atlas.front(),"native/f543 (diagnostic; blur intentionally differs)");
        const auto difference=Compare(atlas.front(),optimized.front(),"f543/candidate");
        char before_name[96],after_name[96];
        const char* opt=CandidateOptimization==D3DCOMPILE_OPTIMIZATION_LEVEL3?"O3":"O1";
        std::snprintf(before_name,sizeof(before_name),"f543-%up-r%.2f-%s.bmp",H,TargetRadius,opt);
        std::snprintf(after_name,sizeof(after_name),"candidate-%up-r%.2f-%s.bmp",H,TargetRadius,opt);
        WriteBmp(before_name,atlas.front());
        WriteBmp(after_name,optimized.front());
        const double native_ms=MedianTotal(native),old_ms=MedianTotal(atlas),new_ms=MedianTotal(optimized);
        std::printf("Total median: native %.3fms, f543 %.3fms, candidate %.3fms; "
                    "DoF overhead versus native %.3fms -> %.3fms; f543/candidate %.3fx (%.2f%% reduction)\n",
                    native_ms,old_ms,new_ms,old_ms-native_ms,new_ms-native_ms,
                    old_ms/std::max(new_ms,1e-9),100*(old_ms-new_ms)/std::max(old_ms,1e-9));
        Require(difference.maximum<=max_absolute && difference.rms<=max_rms &&
                difference.active_rms<=max_active_rms &&
                difference.rgb_energy_percent<=max_energy_percent &&
                difference.alpha_energy_percent<=max_energy_percent,
                "candidate image exceeded predeclared same-kernel quality gates");
        std::puts("Hardware timestamp benchmark PASS: selectable 720p/2160p, 8x D24S8 source depth, 1x color, "
                  "12 overlapping quads, four unchanged-depth batches. Native: no DoF preparation. "
                  "Both DoF paths: one depth copy + one range pass; atlas includes every batch/page's "
                  "source clear/evaluation, filter, transitions and composite. "
                  "Clear depth and colored texture exercise dense overlapping particle cost; destination "
                  "depth is disabled because color is single-sampled. WARP separately covers occlusion, "
                  "textured materials, blending and correctness. GPU clocks/power state were not fixed; "
                  "timestamps are workload evidence, not gameplay FPS. No game was launched or accessed.");
        return 0;
    } catch(const std::exception& e) {
        std::fprintf(stderr,"smoke GPU benchmark FAILED: %s\n",e.what());
        return 1;
    }
}
