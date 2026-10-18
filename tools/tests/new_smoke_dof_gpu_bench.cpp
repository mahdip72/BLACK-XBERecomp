// Offscreen D3D12 timestamp benchmark of the frozen a5777a9 smoke shader and the
// optimized production shader. This executable deliberately requires hardware D3D12.
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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "d3d/particle_dof.h"
#include "d3d/particle_dof_geometry.h"
#include "d3d/particle_dof_wrapper.h"
#include "gpu/shaders.h"
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
constexpr uint32_t W = 1280, H = 720, Samples = 8, Quads = 12, Batches = 4;
void Require(bool value, const char* what) { if (!value) throw std::runtime_error(what); }
void Hr(HRESULT value, const char* what) {
    if (FAILED(value)) {
        char message[160];
        std::snprintf(message, sizeof(message), "%s: 0x%08lX", what,
                      static_cast<unsigned long>(value));
        throw std::runtime_error(message);
    }
}
ComPtr<ID3DBlob> Compile(const std::string& source, const char* entry, const char* profile) {
    Require(!source.empty(), "native smoke wrapper rejected production source");
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(source.data(), source.size(), "particle GPU benchmark",
        nullptr, nullptr, entry, profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
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
struct Intervals { double copy = 0, range = 0, draw = 0, total = 0; };
struct Result { Intervals ms; std::vector<uint16_t> rgba; };

class Bench {
public:
    Bench() {
        ComPtr<IDXGIFactory6> factory;
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> candidate;
            if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                    IID_PPV_ARGS(&candidate)) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc{};
            Hr(candidate->GetDesc1(&desc), "GetDesc1");
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
                SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0,
                                            __uuidof(ID3D12Device), nullptr))) {
                adapter_ = candidate;
                std::wprintf(L"GPU: %ls; vendor %04X device %04X LUID %08lX:%08lX\n",
                             desc.Description,desc.VendorId,desc.DeviceId,
                             DWORD(desc.AdapterLuid.HighPart),desc.AdapterLuid.LowPart);
                break;
            }
        }
        Require(adapter_.Get() != nullptr, "No hardware D3D12 adapter; benchmark not run");
        Hr(D3D12CreateDevice(adapter_.Get(), D3D_FEATURE_LEVEL_11_0,
                             IID_PPV_ARGS(&device_)), "D3D12CreateDevice(hardware)");
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{};
        q.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; q.SampleCount = Samples;
        Hr(device_->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                                        &q, sizeof(q)), "Check 8x D24S8");
        Require(q.NumQualityLevels > 0, "Hardware adapter lacks requested 8x D24S8");
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Hr(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_)), "CreateCommandQueue");
        Hr(queue_->GetTimestampFrequency(&frequency_), "GetTimestampFrequency");
        Hr(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "CreateFence");
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Require(event_ != nullptr, "CreateEvent failed");
        MakeRoot();
        MakeShadersAndPipelines();
        MakeFixture();
    }
    ~Bench() { if (event_) CloseHandle(event_); }

    Result Run(bool old) {
        // All resources, upload buffers and PSOs exist before the first timestamp.
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        Hr(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           IID_PPV_ARGS(&allocator)), "Create allocator");
        Hr(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                      nullptr, IID_PPV_ARGS(&list)), "Create list");
        auto depth = MakeTexture(TextureDesc(DXGI_FORMAT_R24G8_TYPELESS,
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, Samples),
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
        auto snapshot = MakeTexture(TextureDesc(DXGI_FORMAT_R24G8_TYPELESS,
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, Samples),
            D3D12_RESOURCE_STATE_COPY_DEST);
        auto range = MakeTexture(TextureDesc(DXGI_FORMAT_R32G32_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET), D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto target = MakeTexture(TextureDesc(DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET), D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto descriptors = MakeDescriptors(snapshot.Get(), range.Get(), target.Get());
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
        dsv_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
        device_->CreateDepthStencilView(depth.Get(), &dsv_desc, descriptors.dsv);
        list->ClearDepthStencilView(descriptors.dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
        auto depth_barrier = Barrier(depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                     D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &depth_barrier);

        D3D12_QUERY_HEAP_DESC qh{};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qh.Count = 2*(2+Batches*2)+2;
        ComPtr<ID3D12QueryHeap> queries;
        Hr(device_->CreateQueryHeap(&qh, IID_PPV_ARGS(&queries)), "Create timestamp heap");
        const auto timing = MakeBuffer(qh.Count*sizeof(uint64_t), D3D12_HEAP_TYPE_READBACK,
                                       D3D12_RESOURCE_STATE_COPY_DEST);
        UINT ticks = 0;
        auto stamp = [&] { list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, ticks++); };
        std::vector<std::pair<UINT,UINT>> copies, ranges, draws;
        auto timed = [&](auto& bucket, auto&& operation) {
            const UINT first = ticks; stamp(); operation(); stamp();
            bucket.emplace_back(first, ticks-1);
        };
        const UINT all_start = ticks; stamp();
        auto copy = [&] {
            list->CopyResource(snapshot.Get(), depth.Get());
            auto b = Barrier(snapshot.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            list->ResourceBarrier(1, &b);
        };
        timed(copies, copy);
        auto heap = descriptors.srv_heap.Get();
        list->SetDescriptorHeaps(1, &heap);
        list->SetGraphicsRootSignature(root_.Get());
        list->SetGraphicsRootDescriptorTable(0, descriptors.srv_heap->GetGPUDescriptorHandleForHeapStart());
        list->SetGraphicsRootConstantBufferView(1, vcb_->GetGPUVirtualAddress());
        list->SetGraphicsRootConstantBufferView(2, dcb_->GetGPUVirtualAddress());
        const D3D12_VIEWPORT viewport{0,0,float(W),float(H),0,1};
        const D3D12_RECT scissor{0,0,LONG(W),LONG(H)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (!old) {
            timed(ranges, [&] {
                list->SetPipelineState(range_pso_.Get());
                list->OMSetRenderTargets(1, &descriptors.range_rtv, FALSE, nullptr);
                list->DrawInstanced(3, 1, 0, 0);
                auto b = Barrier(range.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                list->ResourceBarrier(1, &b);
            });
        }
        const float clear[] = {.02f,.03f,.05f,.1f};
        list->ClearRenderTargetView(descriptors.target_rtv, clear, 0, nullptr);
        const D3D12_VERTEX_BUFFER_VIEW vbv{vb_->GetGPUVirtualAddress(),
            UINT(sizeof(gpu::NativeParticleVertex)*expanded_.size()),
            sizeof(gpu::NativeParticleVertex)};
        const D3D12_INDEX_BUFFER_VIEW ibv{ib_->GetGPUVirtualAddress(),
            UINT(sizeof(uint16_t)*indices_.size()), DXGI_FORMAT_R16_UINT};
        list->IASetVertexBuffers(0,1,&vbv); list->IASetIndexBuffer(&ibv);
        for (uint32_t batch = 0; batch < Batches; ++batch) {
            if (old && batch) {
                timed(copies, [&] {
                    auto b = Barrier(snapshot.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                     D3D12_RESOURCE_STATE_COPY_DEST);
                    list->ResourceBarrier(1, &b);
                    copy();
                });
            }
            timed(draws, [&] {
                list->SetPipelineState(old ? old_pso_.Get() : new_pso_.Get());
                list->OMSetRenderTargets(1,&descriptors.target_rtv,FALSE,nullptr);
                list->DrawIndexedInstanced(UINT(indices_.size()),1,0,0,0);
            });
        }
        const UINT all_end = ticks; stamp();
        list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP,0,ticks,
                               timing.Get(),0);
        auto target_barrier = Barrier(target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                      D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1,&target_barrier);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT rows=0; UINT64 row_bytes=0, total=0;
        auto desc = target->GetDesc();
        device_->GetCopyableFootprints(&desc,0,1,0,&fp,&rows,&row_bytes,&total);
        auto pixels = MakeBuffer(total,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};
        src.pResource=target.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=pixels.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint=fp;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        Hr(list->Close(),"Close benchmark list");
        ID3D12CommandList* lists[]{list.Get()}; queue_->ExecuteCommandLists(1,lists);
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
        auto duration = [&](UINT a,UINT b) { return 1000.0*double(t[b]-t[a])/frequency_; };
        for (auto [a,b]:copies) result.ms.copy += duration(a,b);
        for (auto [a,b]:ranges) result.ms.range += duration(a,b);
        for (auto [a,b]:draws) result.ms.draw += duration(a,b);
        result.ms.total=duration(all_start,all_end);
        const D3D12_RANGE no_write{0,0}; timing->Unmap(0,&no_write);
        uint8_t* pixel_bytes=nullptr;
        const D3D12_RANGE pr{0,SIZE_T(fp.Footprint.RowPitch)*(H-1)+W*8};
        Hr(pixels->Map(0,&pr,reinterpret_cast<void**>(&pixel_bytes)),"Map RGBA readback");
        result.rgba.resize(size_t(W)*H*4);
        for (uint32_t y=0;y<H;++y)
            std::memcpy(result.rgba.data()+size_t(y)*W*4,
                        pixel_bytes+size_t(y)*fp.Footprint.RowPitch,W*8);
        pixels->Unmap(0,&no_write);
        return result;
    }

private:
    struct Descriptors {
        ComPtr<ID3D12DescriptorHeap> srv_heap,rtv_heap,dsv_heap;
        D3D12_CPU_DESCRIPTOR_HANDLE range_rtv{},target_rtv{},dsv{};
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
    ComPtr<ID3D12PipelineState> Pipeline(ID3DBlob* vs,ID3DBlob* ps,bool range) {
        D3D12_INPUT_ELEMENT_DESC attrs[]{
            {"ATTR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"ATTR",3,DXGI_FORMAT_B8G8R8A8_UNORM,0,16,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"ATTR",9,DXGI_FORMAT_R32G32_FLOAT,0,20,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
        p.pRootSignature=root_.Get();
        p.VS={vs->GetBufferPointer(),vs->GetBufferSize()};
        p.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        if (!range) p.InputLayout={attrs,3};
        p.SampleMask=UINT_MAX;
        p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;
        p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        p.RasterizerState.DepthClipEnable=TRUE;
        p.DepthStencilState.DepthEnable=FALSE;
        p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        p.NumRenderTargets=1;
        p.RTVFormats[0]=range?DXGI_FORMAT_R32G32_FLOAT:DXGI_FORMAT_R16G16B16A16_FLOAT;
        p.SampleDesc.Count=1;
        p.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        if (!range) {
            auto& b=p.BlendState.RenderTarget[0];
            b.BlendEnable=TRUE;
            b.SrcBlend=b.SrcBlendAlpha=D3D12_BLEND_SRC_ALPHA;
            b.DestBlend=b.DestBlendAlpha=D3D12_BLEND_INV_SRC_ALPHA;
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
        const auto vs=Compile(gpu::MakeParticleDofVertexShader(native_vs),"main","vs_5_0");
        const auto old_vs=Compile(smoke_reference::gpu::MakeParticleDofVertexShader(native_vs),
                                  "main","vs_5_0");
        const auto ps=Compile(gpu::MakeParticleDofPixelShader(native_ps,Samples,0,7),"main","ps_5_0");
        const auto old_ps=Compile(smoke_reference::gpu::MakeParticleDofPixelShader(native_ps,Samples),
                                  "main","ps_5_0");
        const auto range_source=std::string("#define PARTICLE_DOF_SAMPLES 8\n")+
                                gpu::kParticleDofDepthRangeHlsl;
        const auto range_vs=Compile(range_source,"vs","vs_5_0");
        const auto range_ps=Compile(range_source,"range_ps","ps_5_0");
        new_pso_=Pipeline(vs.Get(),ps.Get(),false);
        old_pso_=Pipeline(old_vs.Get(),old_ps.Get(),false);
        range_pso_=Pipeline(range_vs.Get(),range_ps.Get(),true);
    }
    void MakeFixture() {
        gpu::ParticleDofGeometryParams p{};
        p.globals.optics[0]=5;p.globals.optics[1]=16;
        p.globals.optics[2]=1;p.globals.optics[3]=.45f;
        p.globals.viewport[2]=p.globals.target[0]=p.guest_size[0]=float(W);
        p.globals.viewport[3]=p.globals.target[1]=p.guest_size[1]=float(H);
        p.globals.target[2]=float(Quads);
        p.c97[2]=1;p.c98[0]=p.c98[1]=p.c98[3]=1;p.c98[2]=-1;
        std::vector<gpu::NativeParticleVertex> native;
        for (uint32_t q=0;q<Quads;++q) {
            const float x=260+float(q%4)*5, y=120+float(q/4)*5;
            const float z=1.0f/3.0f;
            const uint32_t color=0x38FFE0C0;
            native.push_back({{x,y,z,2.25f},color,{0,0},0});
            native.push_back({{x,y+480,z,2.25f},color,{0,1},0});
            native.push_back({{x+760,y+480,z,2.25f},color,{1,1},0});
            native.push_back({{x+760,y,z,2.25f},color,{1,0},0});
            const uint16_t k=uint16_t(q*4);
            indices_.insert(indices_.end(),{k,uint16_t(k+1),uint16_t(k+2),
                                              k,uint16_t(k+2),uint16_t(k+3)});
        }
        gpu::ParticleDofGeometryStats stats{};
        Require(gpu::BuildParticleDofGeometry(native,p,expanded_,metadata_,stats) &&
                stats.active==Quads,"benchmark geometry failed production expansion");
        vb_=Upload(expanded_.data(),expanded_.size()*sizeof(expanded_[0]));
        ib_=Upload(indices_.data(),indices_.size()*sizeof(indices_[0]));
        std::array<std::array<float,4>,192> vc{};
        for (int j=0;j<4;++j) {vc[97][j]=p.c97[j];vc[98][j]=p.c98[j];}
        vcb_=Upload(vc.data(),sizeof(vc));
        constexpr size_t bytes=sizeof(gpu::DrawConstants)+sizeof(gpu::ParticleDofGlobals)+
                               gpu::kParticleDofMaxQuads*sizeof(gpu::ParticleDofQuad);
        std::vector<uint8_t> cb(bytes);
        gpu::DrawConstants dc{};
        dc.surf[0]=float(W);dc.surf[1]=float(H);dc.surf[2]=1;
        dc.misc[1]=7;dc.texsize[0][0]=dc.texsize[0][1]=1;
        std::memcpy(cb.data(),&dc,sizeof(dc));
        std::memcpy(cb.data()+sizeof(dc),&p.globals,sizeof(p.globals));
        std::memcpy(cb.data()+sizeof(dc)+sizeof(p.globals),metadata_.data(),
                    metadata_.size()*sizeof(metadata_[0]));
        dcb_=Upload(cb.data(),cb.size());
        // 1x1 opaque RGBA texture is immutable across both timed modes.
        texture_=MakeTexture(TextureDesc(DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_FLAG_NONE,1,1,1),D3D12_RESOURCE_STATE_COPY_DEST);
        std::array<uint8_t,256> texel{};
        texel[0]=texel[1]=texel[2]=texel[3]=255;
        auto staging=Upload(texel.data(),texel.size());
        ComPtr<ID3D12CommandAllocator> a;ComPtr<ID3D12GraphicsCommandList> l;
        Hr(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a)),"Tex allocator");
        Hr(device_->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,a.Get(),nullptr,
            IID_PPV_ARGS(&l)),"Tex list");
        D3D12_TEXTURE_COPY_LOCATION dst{},src{};
        dst.pResource=texture_.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource=staging.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        src.PlacedFootprint.Footprint.Width=src.PlacedFootprint.Footprint.Height=1;
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
                                ID3D12Resource* target) {
        Descriptors d;
        D3D12_DESCRIPTOR_HEAP_DESC h{};
        h.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;h.NumDescriptors=4;
        h.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Hr(device_->CreateDescriptorHeap(&h,IID_PPV_ARGS(&d.srv_heap)),"SRV heap");
        auto cpu=d.srv_heap->GetCPUDescriptorHandleForHeapStart();
        const UINT step=device_->GetDescriptorHandleIncrementSize(h.Type);
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Texture2D.MipLevels=1;v.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        device_->CreateShaderResourceView(texture_.Get(),&v,cpu);
        cpu.ptr+=step;device_->CreateShaderResourceView(nullptr,&v,cpu);
        cpu.ptr+=step;v.Format=DXGI_FORMAT_R32G32_FLOAT;
        device_->CreateShaderResourceView(range,&v,cpu);
        cpu.ptr+=step;v.Format=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        v.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DMS;
        device_->CreateShaderResourceView(snapshot,&v,cpu);
        h={};h.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;h.NumDescriptors=2;
        Hr(device_->CreateDescriptorHeap(&h,IID_PPV_ARGS(&d.rtv_heap)),"RTV heap");
        d.range_rtv=d.rtv_heap->GetCPUDescriptorHandleForHeapStart();
        d.target_rtv=d.range_rtv;
        d.target_rtv.ptr+=device_->GetDescriptorHandleIncrementSize(h.Type);
        device_->CreateRenderTargetView(range,nullptr,d.range_rtv);
        device_->CreateRenderTargetView(target,nullptr,d.target_rtv);
        h={};h.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;h.NumDescriptors=1;
        Hr(device_->CreateDescriptorHeap(&h,IID_PPV_ARGS(&d.dsv_heap)),"DSV heap");
        d.dsv=d.dsv_heap->GetCPUDescriptorHandleForHeapStart();
        return d;
    }
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Fence> fence_;
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> old_pso_,new_pso_,range_pso_;
    ComPtr<ID3D12Resource> vb_,ib_,vcb_,dcb_,texture_;
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
void Print(const char* name,const std::vector<Result>& runs) {
    std::vector<double> copy,range,draw,total;
    for (const auto& r:runs) {
        copy.push_back(r.ms.copy);range.push_back(r.ms.range);
        draw.push_back(r.ms.draw);total.push_back(r.ms.total);
    }
    std::printf("%s GPU ms med/p90: copy %.3f/%.3f, range %.3f/%.3f, draw %.3f/%.3f, total %.3f/%.3f\n",
        name,Percentile(copy,.5),Percentile(copy,.9),Percentile(range,.5),Percentile(range,.9),
        Percentile(draw,.5),Percentile(draw,.9),Percentile(total,.5),Percentile(total,.9));
}
} // namespace

int main() {
    try {
        Bench bench;
        bench.Run(true);bench.Run(false); // separate warmups, excluded from metrics
        std::vector<Result> old,newer;
        for (bool old_mode:{true,false,false,true,true,false,false,true}) {
            auto result=bench.Run(old_mode);
            (old_mode?old:newer).push_back(std::move(result));
        }
        Require(old.size()==newer.size(),"ABBA pairing failed");
        float max_rgba=0;double rms=0;
        for (size_t i=0;i<old[0].rgba.size();++i) {
            const float a=FromHalf(old[0].rgba[i]),b=FromHalf(newer[0].rgba[i]);
            Require(std::isfinite(a)&&std::isfinite(b),"benchmark RGBA is nonfinite");
            const float delta=std::abs(a-b);
            max_rgba=std::max(max_rgba,delta);rms+=delta*delta;
        }
        rms=std::sqrt(rms/old[0].rgba.size());
        std::printf("RGBA16F readback old/new: max absolute %.6f, RMS %.6f\n",max_rgba,rms);
        Require(max_rgba<.02f && rms<.003f,
                "optimized smoke benchmark changed image materially");
        Print("old per-batch copy+draw",old);
        Print("new cached copy+range+draw",newer);
        std::puts("Hardware timestamp benchmark complete: 1280x720, 8x D24S8, 12 overlapping quads, four unchanged-depth batches; old 4 copies, new 1 copy + 1 range. No game was launched.");
        return 0;
    } catch(const std::exception& e) {
        std::fprintf(stderr,"smoke GPU benchmark FAILED: %s\n",e.what());
        return 1;
    }
}
