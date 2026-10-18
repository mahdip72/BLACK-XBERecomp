// Offscreen D3D12 regression for the production asynchronous DoF PSO cache.
// Uses embedded shader bytecode only; no FXC call, window, swapchain, or game.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "d3d/dof_pipeline_cache.h"
#include "d3d/dof_shader_library.h"
#include "d3d/dof_shader_source.h"

using Microsoft::WRL::ComPtr;
namespace {

constexpr uint64_t kFlags=gpu::DofShaderCompilerFlags("main","ps_5_0");
void Check(bool okay,const char* what) {
    if (!okay) throw std::runtime_error(what);
}
void Hr(HRESULT hr,const char* what) {
    if (FAILED(hr)) {
        char buffer[160];
        std::snprintf(buffer,sizeof(buffer),"%s: 0x%08lX",what,
                      static_cast<unsigned long>(hr));
        throw std::runtime_error(buffer);
    }
}
ComPtr<ID3D12Device> SelectDevice(bool hardware) {
    ComPtr<IDXGIFactory6> factory;
    Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"CreateDXGIFactory1");
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    if (hardware) {
        for (UINT index=0;;++index) {
            ComPtr<IDXGIAdapter1> candidate;
            const HRESULT found=factory->EnumAdapterByGpuPreference(
                index,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&candidate));
            if (found==DXGI_ERROR_NOT_FOUND) break;
            Hr(found,"EnumAdapterByGpuPreference");
            DXGI_ADAPTER_DESC1 desc{};
            Hr(candidate->GetDesc1(&desc),"GetDesc1(hardware)");
            if (desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            if (SUCCEEDED(D3D12CreateDevice(candidate.Get(),D3D_FEATURE_LEVEL_11_0,
                                            IID_PPV_ARGS(&device)))) {
                adapter=std::move(candidate);
                break;
            }
        }
        Check(adapter!=nullptr,"no nonsoftware high-performance D3D12 adapter");
    } else {
        Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)),"EnumWarpAdapter");
        Hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,
                             IID_PPV_ARGS(&device)),"D3D12CreateDevice(WARP)");
    }
    DXGI_ADAPTER_DESC1 desc{};
    Hr(adapter->GetDesc1(&desc),"GetDesc1(selected adapter)");
    std::printf("adapter %ls vendor=0x%04X device=0x%04X LUID=%08lX:%08lX software=%u\n",
                desc.Description,desc.VendorId,desc.DeviceId,
                static_cast<unsigned long>(desc.AdapterLuid.HighPart),
                static_cast<unsigned long>(desc.AdapterLuid.LowPart),
                !!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE));
    return device;
}
ComPtr<ID3D12RootSignature> Root(ID3D12Device* device) {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors=4;
    D3D12_ROOT_PARAMETER p[3]{};
    p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[0].DescriptorTable.NumDescriptorRanges=1;
    p[0].DescriptorTable.pDescriptorRanges=&range;
    for (UINT i=1;i<3;++i) {
        p[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[i].Descriptor.ShaderRegister=i-1;
    }
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister=0;
    sampler.MaxLOD=D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters=3;desc.pParameters=p;
    desc.NumStaticSamplers=1;desc.pStaticSamplers=&sampler;
    desc.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> serial,errors;
    Hr(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,
                                    &serial,&errors),"D3D12SerializeRootSignature");
    ComPtr<ID3D12RootSignature> root;
    Hr(device->CreateRootSignature(0,serial->GetBufferPointer(),serial->GetBufferSize(),
                                   IID_PPV_ARGS(&root)),"CreateRootSignature");
    return root;
}
struct Shaders { ComPtr<ID3DBlob> vs,ps; };
Shaders EmbeddedParticleShaders() {
    const auto vertex=gpu::DofParticleVertexSource(1);
    const auto pixel=gpu::DofParticlePixelSource(1,0,7,false,1);
    Shaders s{gpu::LoadDofShaderBytecode(vertex,"main","vs_5_0",kFlags),
              gpu::LoadDofShaderBytecode(pixel,"main","ps_5_0",kFlags)};
    Check(s.vs && s.ps,"embedded atlas source shaders missing for PSO test");
    return s;
}
D3D12_GRAPHICS_PIPELINE_STATE_DESC ParticleDesc(ID3D12RootSignature* root,
    ID3DBlob* vs,ID3DBlob* ps,const D3D12_INPUT_ELEMENT_DESC* inputs) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature=root;
    d.VS={vs->GetBufferPointer(),vs->GetBufferSize()};
    d.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    d.InputLayout={inputs,3};
    d.SampleMask=UINT_MAX;
    d.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable=TRUE;
    d.DepthStencilState.DepthEnable=FALSE;
    d.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets=1;
    d.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.SampleDesc.Count=1;
    d.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    return d;
}
std::array<D3D12_INPUT_ELEMENT_DESC,3> Inputs(const char* semantic) {
    return {{{semantic,0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
             {semantic,3,DXGI_FORMAT_B8G8R8A8_UNORM,0,16,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
             {semantic,9,DXGI_FORMAT_R32G32_FLOAT,0,20,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}}};
}
void CheckAsyncLifetime(ID3D12Device* device) {
    auto root=Root(device);
    auto shaders=EmbeddedParticleShaders();
    auto inputs=Inputs("ATTR");
    auto desc=ParticleDesc(root.Get(),shaders.vs.Get(),shaders.ps.Get(),inputs.data());
    gpu::DofPipelineCache cache(device);
    constexpr uint64_t key=0xD0F0001;
    const auto before=std::chrono::steady_clock::now();
    Check(cache.Request(key,desc,shaders.vs,shaders.ps)==nullptr,
          "first async request should return native-fallback/null");
    const double request_ms=std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now()-before).count();
    const bool immediate=cache.WaitFor(std::chrono::milliseconds{0});
    const bool bounded=cache.WaitFor(std::chrono::milliseconds{33});
    if (immediate || bounded)
        Check(cache.GetStats().pending==0,"bounded wait reported drained while pending");
    cache.Wait();
    Check(cache.WaitFor(std::chrono::milliseconds{0}),
          "zero-timeout wait did not observe drained cache");
    const auto after=cache.GetStats();
    Check(after.ready==1 && after.pending==0 && after.failed==0,
          "embedded particle PSO did not reach Ready");
    auto* ready=cache.Request(key,desc,shaders.vs,shaders.ps);
    Check(ready!=nullptr,"cached async PSO was not returned on repeated Request");
    const auto repeat_start=std::chrono::steady_clock::now();
    for (unsigned i=0;i<256;++i)
        Check(cache.Request(key,desc,shaders.vs,shaders.ps)==ready,
              "cached Request changed PSO identity");
    const double repeat_ms=std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now()-repeat_start).count();
    Check(repeat_ms<1000.0,"cached Request blocked unexpectedly");
    const auto stable=cache.GetStats();
    Check(stable.ready==1 && stable.pending==0 && stable.failed==0,
          "repeated Request scheduled duplicate pipeline work");
    std::printf("async first Request %.3f ms; WaitFor(0)=%u WaitFor(33ms)=%u; "
                "Wait reached Ready; 256 cached Requests %.3f ms\n",
                request_ms,immediate,bounded,repeat_ms);

    // Queue work behind other unique jobs, then destroy all caller-owned pointer
    // storage immediately. Job::Capture must own bytecode, root and semantic names.
    for (uint64_t i=0;i<8;++i)
        Check(cache.Request(0xD0F1000+i,desc,shaders.vs,shaders.ps)==nullptr,
              "first queued PSO Request did not return native fallback");
    const auto target_key=uint64_t{0xD0F2001};
    {
        char semantic[]="ATTR";
        auto temporary_inputs=Inputs(semantic);
        auto temporary_shaders=EmbeddedParticleShaders();
        auto temporary_root=Root(device);
        auto temporary_desc=ParticleDesc(temporary_root.Get(),temporary_shaders.vs.Get(),
                                        temporary_shaders.ps.Get(),temporary_inputs.data());
        Check(cache.Request(target_key,temporary_desc,temporary_shaders.vs,
                            temporary_shaders.ps)==nullptr,
              "temporary descriptor Request did not return native fallback");
        std::memcpy(semantic,"BAD!",5);
        for (auto& input:temporary_inputs) input.SemanticName=nullptr;
        temporary_shaders.vs.Reset();temporary_shaders.ps.Reset();
        temporary_root.Reset();
    }
    cache.Wait();
    Check(cache.Request(target_key,desc,shaders.vs,shaders.ps)!=nullptr,
          "queued PSO borrowed caller descriptor, semantic, root or shader blob");
    const auto owned=cache.GetStats();
    Check(owned.ready==10 && owned.pending==0 && owned.failed==0,
          "queued production PSO jobs did not all reach Ready");

    auto bad=desc;
    bad.SampleDesc.Count=0; // Capturable descriptor; D3D12 must reject its PSO.
    constexpr uint64_t bad_key=0xD0F3001;
    Check(cache.Request(bad_key,bad,shaders.vs,shaders.ps)==nullptr,
          "invalid descriptor unexpectedly returned a ready PSO");
    cache.Wait();
    auto failed=cache.GetStats();
    Check(failed.failed==1 && failed.pending==0,
          "invalid PSO was not cached as Failed");
    Check(cache.Request(bad_key,desc,shaders.vs,shaders.ps)==nullptr,
          "failed key unexpectedly retried with a changed descriptor");
    const auto still_failed=cache.GetStats();
    Check(still_failed.failed==failed.failed && still_failed.ready==failed.ready,
          "failed key caused repeated PSO creation");
    std::printf("temporary descriptor/blob ownership PASS; invalid PSO cached Failed\n");
}
void CheckDestructorDrains(ID3D12Device* device) {
    auto root=Root(device);
    auto shaders=EmbeddedParticleShaders();
    auto inputs=Inputs("ATTR");
    auto desc=ParticleDesc(root.Get(),shaders.vs.Get(),shaders.ps.Get(),inputs.data());
    {
        gpu::DofPipelineCache cache(device);
        for (uint64_t i=0;i<12;++i)
            Check(cache.Request(0xD0F4000+i,desc,shaders.vs,shaders.ps)==nullptr,
                  "first destructor-drain Request should return native fallback");
        // Deliberately no Wait: destructor must join the worker and finish owned jobs.
    }
    std::puts("destructor drained 12 queued offscreen PSO jobs");
}
} // namespace

int main(int argc,char** argv) {
    try {
        Check(argc==1 || (argc==2 && std::strcmp(argv[1],"--hardware")==0),
              "usage: dof_pipeline_cache_test [--hardware]");
        const bool hardware=argc==2;
        auto device=SelectDevice(hardware);
        CheckAsyncLifetime(device.Get());
        CheckDestructorDrains(device.Get());
        std::printf("embedded asynchronous DOF pipeline cache %s checks PASS\n",
                    hardware?"hardware":"WARP");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr,"embedded DOF pipeline cache FAILED: %s\n",error.what());
        return 1;
    }
}
