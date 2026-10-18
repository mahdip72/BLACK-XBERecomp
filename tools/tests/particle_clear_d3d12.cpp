// Standalone hardware-D3D12 regression for bounded atlas clears. No game or shader is used.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "d3d/particle_dof_clear.h"

using Microsoft::WRL::ComPtr;
namespace {

constexpr UINT kExtent = 2048;
constexpr UINT kCellsPerAxis = 32;
constexpr UINT kSpacing = 64;
constexpr UINT kCellExtent = 32;
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr std::array<uint16_t,4> kBaseHalf{0x3000,0x3400,0x3800,0x3c00};
constexpr std::array<UINT,8> kCounts{12,16,32,33,38,64,128,1024};

void Require(bool okay, const char* what) {
    if (!okay) throw std::runtime_error(what);
}
void Hr(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char message[192];
        std::snprintf(message,sizeof(message),"%s: 0x%08lX",what,
                      static_cast<unsigned long>(hr));
        throw std::runtime_error(message);
    }
}
D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource,D3D12_RESOURCE_STATES from,
                                  D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource=resource;
    b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore=from;
    b.Transition.StateAfter=to;
    return b;
}

class ClearTest {
public:
    ClearTest() {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            debug_enabled_=true;
        }
        ComPtr<IDXGIFactory6> factory;
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"CreateDXGIFactory1");
        for (UINT i=0;;++i) {
            ComPtr<IDXGIAdapter1> candidate;
            if (factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                    IID_PPV_ARGS(&candidate))==DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_ADAPTER_DESC1 desc{};
            Hr(candidate->GetDesc1(&desc),"GetDesc1");
            if ((desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)==0 &&
                SUCCEEDED(D3D12CreateDevice(candidate.Get(),D3D_FEATURE_LEVEL_11_0,
                                            __uuidof(ID3D12Device),nullptr))) {
                adapter_=candidate;
                std::wprintf(L"hardware adapter: %ls; vendor %04X device %04X LUID %08lX:%08lX\n",
                             desc.Description,desc.VendorId,desc.DeviceId,
                             DWORD(desc.AdapterLuid.HighPart),desc.AdapterLuid.LowPart);
                break;
            }
        }
        Require(adapter_.Get()!=nullptr,"no hardware D3D12 adapter; clear test did not run");
        Hr(D3D12CreateDevice(adapter_.Get(),D3D_FEATURE_LEVEL_11_0,
                             IID_PPV_ARGS(&device_)),"D3D12CreateDevice");
        if (debug_enabled_) device_.As(&messages_);
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        Hr(device_->CreateCommandQueue(&queue_desc,IID_PPV_ARGS(&queue_)),"CreateCommandQueue");
        Hr(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           IID_PPV_ARGS(&allocator_)),"CreateCommandAllocator");
        Hr(device_->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator_.Get(),
                                      nullptr,IID_PPV_ARGS(&list_)),"CreateCommandList");
        Hr(list_->Close(),"Close initial list");
        Hr(device_->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence_)),"CreateFence");
        event_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        Require(event_!=nullptr,"CreateEventW failed");

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC texture_desc{};
        texture_desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture_desc.Width=kExtent;
        texture_desc.Height=kExtent;
        texture_desc.DepthOrArraySize=texture_desc.MipLevels=1;
        texture_desc.Format=kFormat;
        texture_desc.SampleDesc.Count=1;
        texture_desc.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texture_desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        Hr(device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture_desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&target_)),
            "Create atlas RTV");
        UINT rows=0;UINT64 row_bytes=0,total_bytes=0;
        device_->GetCopyableFootprints(&texture_desc,0,1,0,&footprint_,&rows,
                                       &row_bytes,&total_bytes);
        Require(rows==kExtent && row_bytes==UINT64(kExtent)*8 &&
                total_bytes<=UINT64(32)*1024*1024,
                "unexpected RGBA16F readback footprint");
        heap.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer_desc{};
        buffer_desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer_desc.Width=total_bytes;
        buffer_desc.Height=buffer_desc.DepthOrArraySize=buffer_desc.MipLevels=1;
        buffer_desc.SampleDesc.Count=1;
        buffer_desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Hr(device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer_desc,
            D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback_)),
            "Create readback buffer");
        D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
        rtv_desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv_desc.NumDescriptors=1;
        Hr(device_->CreateDescriptorHeap(&rtv_desc,IID_PPV_ARGS(&rtv_heap_)),
           "Create RTV heap");
        rtv_=rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        device_->CreateRenderTargetView(target_.Get(),nullptr,rtv_);
        std::printf("D3D12 debug layer: %s; target/readback %.1f/%.1f MiB\n",
                    debug_enabled_?"enabled":"unavailable",
                    double(kExtent)*kExtent*8/(1024*1024),
                    double(total_bytes)/(1024*1024));
    }
    ~ClearTest() { if (event_) CloseHandle(event_); }

    void Run(UINT count) {
        Require(count<=kCellsPerAxis*kCellsPerAxis,"invalid clear cardinality");
        std::vector<D3D12_RECT> rects;
        rects.reserve(count);
        for (UINT i=0;i<count;++i) {
            const LONG x=LONG((i%kCellsPerAxis)*kSpacing);
            const LONG y=LONG((i/kCellsPerAxis)*kSpacing);
            rects.push_back({x,y,x+LONG(kCellExtent),y+LONG(kCellExtent)});
        }
        if (messages_) messages_->ClearStoredMessages();
        Hr(allocator_->Reset(),"Reset allocator");
        Hr(list_->Reset(allocator_.Get(),nullptr),"Reset list");
        constexpr float base[4]{.125f,.25f,.5f,1.0f};
        list_->ClearRenderTargetView(rtv_,base,0,nullptr);
        gpu::ClearParticleDofAtlasCells(list_.Get(),rtv_,std::span<const D3D12_RECT>(rects));
        auto to_copy=Transition(target_.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,
                                D3D12_RESOURCE_STATE_COPY_SOURCE);
        list_->ResourceBarrier(1,&to_copy);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};
        src.pResource=target_.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=readback_.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint=footprint_;
        list_->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        auto to_render=Transition(target_.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,
                                  D3D12_RESOURCE_STATE_RENDER_TARGET);
        list_->ResourceBarrier(1,&to_render);
        Hr(list_->Close(),"Close clear list");
        ID3D12CommandList* commands[]{list_.Get()};
        queue_->ExecuteCommandLists(1,commands);
        const UINT64 value=++fence_value_;
        Hr(queue_->Signal(fence_.Get(),value),"Signal clear fence");
        if (fence_->GetCompletedValue()<value) {
            Hr(fence_->SetEventOnCompletion(value,event_),"SetEventOnCompletion");
            Require(WaitForSingleObject(event_,120000)==WAIT_OBJECT_0,
                    "clear GPU fence timed out");
        }
        CheckDebugMessages();
        uint8_t* pixels=nullptr;
        const D3D12_RANGE read{0,SIZE_T(footprint_.Footprint.RowPitch)*kExtent};
        Hr(readback_->Map(0,&read,reinterpret_cast<void**>(&pixels)),"Map clear readback");
        UINT64 cleared=0;
        for (UINT y=0;y<kExtent;++y) {
            const auto* row=reinterpret_cast<const uint16_t*>(
                pixels+SIZE_T(y)*footprint_.Footprint.RowPitch);
            for (UINT x=0;x<kExtent;++x) {
                const UINT cell=(y/kSpacing)*kCellsPerAxis+x/kSpacing;
                const bool inside=cell<count && x%kSpacing<kCellExtent &&
                                  y%kSpacing<kCellExtent;
                const auto* actual=row+size_t(x)*4;
                const uint16_t* expected=inside?zero_.data():kBaseHalf.data();
                if (std::memcmp(actual,expected,8)!=0) {
                    std::fprintf(stderr,
                        "%u rect clear mismatch at (%u,%u), cell %u: got %04X %04X %04X %04X\n",
                        count,x,y,cell,actual[0],actual[1],actual[2],actual[3]);
                    const D3D12_RANGE no_write{0,0};readback_->Unmap(0,&no_write);
                    throw std::runtime_error("atlas clear changed an unrequested pixel or missed a cell");
                }
                cleared+=inside;
            }
        }
        const D3D12_RANGE no_write{0,0};readback_->Unmap(0,&no_write);
        Require(cleared==UINT64(count)*kCellExtent*kCellExtent,
                "clear fixture did not cover the intended pixel count");
        std::printf("%4u rectangles PASS: %llu zero pixels, all other pixels exact half-float base\n",
                    count,static_cast<unsigned long long>(cleared));
    }

private:
    void CheckDebugMessages() {
        if (!messages_) return;
        const UINT64 n=messages_->GetNumStoredMessages();
        for (UINT64 i=0;i<n;++i) {
            SIZE_T bytes=0;
            Hr(messages_->GetMessage(i,nullptr,&bytes),"GetMessage size");
            std::vector<uint8_t> storage(bytes);
            auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            Hr(messages_->GetMessage(i,message,&bytes),"GetMessage");
            if (message->Severity==D3D12_MESSAGE_SEVERITY_ERROR ||
                message->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION) {
                std::fprintf(stderr,"D3D12 debug: %s\n",message->pDescription);
                throw std::runtime_error("D3D12 debug layer reported a clear error");
            }
        }
    }
    static constexpr std::array<uint16_t,4> zero_{};
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12InfoQueue> messages_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    ComPtr<ID3D12Resource> target_,readback_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_{};
    UINT64 fence_value_=0;
    HANDLE event_=nullptr;
    bool debug_enabled_=false;
};

} // namespace

int main() {
    try {
        ClearTest test;
        for (UINT count:kCounts) test.Run(count);
        std::puts("production particle atlas clear D3D12 checks PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr,"particle atlas clear D3D12 FAILED: %s\n",error.what());
        return 1;
    }
}
