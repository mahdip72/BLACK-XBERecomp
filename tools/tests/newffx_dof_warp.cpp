// Headless integration checks for the production FidelityFX DoF wrapper.
// The adapter is explicitly DXGI WARP; this never opens a window or runs BLACK.
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
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "d3d/fidelityfx_dof.h"
#include "d3d/fidelityfx_dof_shader.h"

using Microsoft::WRL::ComPtr;

namespace {

struct Pixel { float r, g, b, a; };
struct Fixture {
    uint32_t width, height;
    float bound;
    std::vector<Pixel> color;
    std::vector<float> depth;
};
struct AdapterResult {
    std::vector<Pixel> destination;
    std::vector<Pixel> prepared_color;
    std::vector<float> encoded_depth;
};

void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

void Hr(HRESULT result, const char* action) {
    if (FAILED(result)) {
        char text[180];
        std::snprintf(text, sizeof(text), "%s failed: 0x%08lX", action,
                      static_cast<unsigned long>(result));
        throw std::runtime_error(text);
    }
}

void WriteBmp(const char* name, uint32_t width, uint32_t height,
              const std::vector<Pixel>& pixels) {
    const auto directory=std::filesystem::path("out")/"tests"/"fidelityfx-dof"/"fixtures";
    std::filesystem::create_directories(directory);
    std::ofstream file(directory/name,std::ios::binary);
    Check(bool(file),"cannot open FidelityFX fixture BMP");
    const uint32_t pitch=(width*3+3)&~3u;
    const uint32_t bytes=54+pitch*height;
    auto u16=[&](uint16_t n){file.put(char(n&255));file.put(char((n>>8)&255));};
    auto u32=[&](uint32_t n){u16(uint16_t(n));u16(uint16_t(n>>16));};
    file.write("BM",2); u32(bytes); u32(0); u32(54);
    u32(40); u32(width); u32(height); u16(1); u16(24);
    u32(0); u32(pitch*height); u32(2835); u32(2835); u32(0); u32(0);
    std::vector<char> row(pitch,0);
    auto channel=[](float v){return char(int(std::round(std::clamp(v,0.0f,1.0f)*255.0f)));};
    for(uint32_t iy=0;iy<height;++iy) {
        const uint32_t y=height-1-iy;
        for(uint32_t x=0;x<width;++x) {
            const Pixel& p=pixels[size_t(y)*width+x];
            row[3*x]=channel(p.b); row[3*x+1]=channel(p.g); row[3*x+2]=channel(p.r);
        }
        file.write(row.data(),row.size());
    }
    Check(bool(file),"cannot write FidelityFX fixture BMP");
}

size_t At(const Fixture& f, uint32_t x, uint32_t y) {
    return size_t(y) * f.width + x;
}

// Inputs are finite fixture colors in [0,1]. Round to the nearest IEEE binary16.
uint16_t Half(float value) {
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int exponent = int((bits >> 23) & 255u) - 127 + 15;
    const uint32_t mantissa = bits & 0x7fffffu;
    if (exponent <= 0) {
        if (exponent < -10) return uint16_t(sign);
        const uint32_t shifted = (mantissa | 0x800000u) >> (1 - exponent);
        return uint16_t(sign | ((shifted + 0x1000u) >> 13));
    }
    if (exponent >= 31) return uint16_t(sign | 0x7c00u);
    return uint16_t(sign | (uint32_t(exponent) << 10) |
                    ((mantissa + 0x1000u) >> 13));
}

float FloatFromHalf(uint16_t bits) {
    const unsigned exp = (bits >> 10) & 31u;
    const unsigned fraction = bits & 1023u;
    const float magnitude = exp == 0 ? std::ldexp(float(fraction), -24)
                          : exp == 31 ? INFINITY
                          : std::ldexp(float(1024u + fraction), int(exp) - 25);
    return bits & 0x8000u ? -magnitude : magnitude;
}

ComPtr<ID3DBlob> Compile(const char* entry, const char* profile,
                         const D3D_SHADER_MACRO* macros = nullptr) {
    ComPtr<ID3DBlob> bytecode, errors;
    const HRESULT result = D3DCompile(gpu::kFidelityFxDofHlsl,
        std::strlen(gpu::kFidelityFxDofHlsl), "production FidelityFX adapter",
        macros, nullptr, entry, profile, D3DCOMPILE_ENABLE_STRICTNESS, 0,
        &bytecode, &errors);
    if (FAILED(result)) {
        if (errors) std::fprintf(stderr, "%s %s: %s\n", entry, profile,
                                 static_cast<const char*>(errors->GetBufferPointer()));
        Hr(result, "D3DCompile(production FidelityFX adapter)");
    }
    return bytecode;
}

void CheckAdapterCompilation() {
    Compile("vs", "vs_5_0");
    Compile("ffx_dof_prepare", "ps_5_0");
    const D3D_SHADER_MACRO ms2[] = {{"MS","1"},{"DOF_SAMPLES","2"},{nullptr,nullptr}};
    const D3D_SHADER_MACRO ms4[] = {{"MS","1"},{"DOF_SAMPLES","4"},{nullptr,nullptr}};
    const D3D_SHADER_MACRO ms8[] = {{"MS","1"},{"DOF_SAMPLES","8"},{nullptr,nullptr}};
    const D3D_SHADER_MACRO present[] = {{"FFX_DOF_PRESENT","1"},{nullptr,nullptr}};
    Compile("ffx_dof_prepare", "ps_5_0", ms2);
    Compile("ffx_dof_prepare", "ps_5_0", ms4);
    Compile("ffx_dof_prepare", "ps_5_0", ms8);
    Compile("ffx_dof_present", "ps_5_0", present);
    std::puts("production adapter HLSL: VS, prepare 1x/2x/4x/8x, present compiled");
}

D3D12_RESOURCE_DESC TextureDesc(uint32_t width, uint32_t height, DXGI_FORMAT format,
                                D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    return desc;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

class Warp {
public:
    Warp() {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            debug_enabled_ = true;
        }
        UINT factory_flags = debug_enabled_ ? DXGI_CREATE_FACTORY_DEBUG : 0;
        HRESULT result = CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory_));
        if (FAILED(result) && factory_flags)
            result = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_));
        Hr(result, "CreateDXGIFactory2");
        Hr(factory_->EnumWarpAdapter(IID_PPV_ARGS(&adapter_)), "EnumWarpAdapter");
        DXGI_ADAPTER_DESC1 adapter_desc{};
        Hr(adapter_->GetDesc1(&adapter_desc), "WARP GetDesc1");
        Check((adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0,
              "DXGI adapter is not software WARP");
        Hr(D3D12CreateDevice(adapter_.Get(), D3D_FEATURE_LEVEL_12_0,
                             IID_PPV_ARGS(&device_)), "D3D12CreateDevice(WARP)");
        D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_2};
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 options{};
        if (FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model))) ||
            model.HighestShaderModel < D3D_SHADER_MODEL_6_2 ||
            FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options, sizeof(options))) ||
            !options.WaveOps)
            throw std::runtime_error("UNSUPPORTED: WARP lacks Shader Model 6.2 or wave operations");
        if (debug_enabled_) {
            Hr(device_.As(&info_queue_), "ID3D12InfoQueue(debug layer)");
            info_queue_->ClearStoredMessages();
        }
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Hr(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_)),
           "CreateCommandQueue(WARP)");
        Hr(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "CreateFence");
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Check(event_ != nullptr, "CreateEvent failed");
        std::printf("WARP software adapter; D3D12 debug layer %s\n",
                    debug_enabled_ ? "enabled" : "unavailable (validation limitation)");
    }

    ~Warp() {
        if (event_) CloseHandle(event_);
    }

    ID3D12Device* Device() const { return device_.Get(); }

    void Wait() {
        const uint64_t target = ++fence_value_;
        Hr(queue_->Signal(fence_.Get(), target), "Signal");
        if (fence_->GetCompletedValue() < target) {
            Hr(fence_->SetEventOnCompletion(target, event_), "SetEventOnCompletion");
            Check(WaitForSingleObject(event_, 30000) == WAIT_OBJECT_0,
                  "WARP dispatch timed out after 30 seconds");
        }
        CheckDebugMessages();
    }

    std::vector<Pixel> Run(gpu::FidelityFxDof& dof, const Fixture& fixture) {
        Check(fixture.color.size() == size_t(fixture.width) * fixture.height &&
              fixture.depth.size() == fixture.color.size(), "invalid fixture dimensions");
        Check(dof.Matches(fixture.width, fixture.height), "DoF context extent mismatch");
        auto color = NewTexture(fixture.width, fixture.height, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                D3D12_RESOURCE_STATE_COPY_DEST);
        auto depth = NewTexture(fixture.width, fixture.height, DXGI_FORMAT_R32_FLOAT,
                                D3D12_RESOURCE_STATE_COPY_DEST);
        auto output = NewTexture(fixture.width, fixture.height, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        std::vector<uint16_t> rgba(fixture.color.size() * 4);
        for (size_t i = 0; i < fixture.color.size(); ++i) {
            const Pixel& p = fixture.color[i];
            rgba[4*i + 0] = Half(p.r);
            rgba[4*i + 1] = Half(p.g);
            rgba[4*i + 2] = Half(p.b);
            rgba[4*i + 3] = Half(p.a);
        }
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT color_fp{}, depth_fp{}, output_fp{};
        UINT num_rows = 0;
        UINT64 row_size = 0, color_bytes = 0, depth_bytes = 0, output_bytes = 0;
        const auto color_desc = color->GetDesc();
        const auto depth_desc = depth->GetDesc();
        device_->GetCopyableFootprints(&color_desc, 0, 1, 0, &color_fp,
                                       &num_rows, &row_size, &color_bytes);
        device_->GetCopyableFootprints(&depth_desc, 0, 1, 0, &depth_fp,
                                       &num_rows, &row_size, &depth_bytes);
        device_->GetCopyableFootprints(&color_desc, 0, 1, 0, &output_fp,
                                       &num_rows, &row_size, &output_bytes);
        auto color_upload = NewBuffer(color_bytes, D3D12_HEAP_TYPE_UPLOAD,
                                      D3D12_RESOURCE_STATE_GENERIC_READ);
        auto depth_upload = NewBuffer(depth_bytes, D3D12_HEAP_TYPE_UPLOAD,
                                      D3D12_RESOURCE_STATE_GENERIC_READ);
        auto readback = NewBuffer(output_bytes, D3D12_HEAP_TYPE_READBACK,
                                  D3D12_RESOURCE_STATE_COPY_DEST);
        FillUpload(color_upload.Get(), color_fp, rgba.data(), fixture.width * 8,
                   fixture.height);
        FillUpload(depth_upload.Get(), depth_fp, fixture.depth.data(), fixture.width * 4,
                   fixture.height);

        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> command_list;
        Hr(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
        Hr(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                      nullptr, IID_PPV_ARGS(&command_list)), "CreateCommandList");
        CopyUpload(command_list.Get(), color.Get(), color_upload.Get(), color_fp);
        CopyUpload(command_list.Get(), depth.Get(), depth_upload.Get(), depth_fp);
        auto b1 = Transition(color.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto b2 = Transition(depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        D3D12_RESOURCE_BARRIER inputs[] = {b1, b2};
        command_list->ResourceBarrier(2, inputs);
        if (!dof.Dispatch(command_list.Get(), color.Get(), depth.Get(), output.Get(),
                          fixture.bound))
            throw std::runtime_error(std::string("production FidelityFX Dispatch: ") + dof.Failure());
        // These transitions check the helper's three entry-state restoration promises.
        D3D12_RESOURCE_BARRIER restored[] = {
            Transition(color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_COPY_SOURCE),
            Transition(depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_COPY_SOURCE),
            Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_COPY_SOURCE),
        };
        command_list->ResourceBarrier(3, restored);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = output.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = output_fp;
        command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Hr(command_list->Close(), "Close command list");
        ID3D12CommandList* lists[] = {command_list.Get()};
        queue_->ExecuteCommandLists(1, lists);
        Wait();

        uint8_t* mapped = nullptr;
        D3D12_RANGE read_range{0, SIZE_T(output_bytes)};
        Hr(readback->Map(0, &read_range, reinterpret_cast<void**>(&mapped)), "Map readback");
        std::vector<Pixel> result(fixture.color.size());
        for (uint32_t y = 0; y < fixture.height; ++y) {
            const auto* row = reinterpret_cast<const uint16_t*>(mapped + y * output_fp.Footprint.RowPitch);
            for (uint32_t x = 0; x < fixture.width; ++x) {
                Pixel& p = result[At(fixture, x, y)];
                p = {FloatFromHalf(row[4*x]), FloatFromHalf(row[4*x+1]),
                     FloatFromHalf(row[4*x+2]), FloatFromHalf(row[4*x+3])};
            }
        }
        D3D12_RANGE no_write{0, 0};
        readback->Unmap(0, &no_write);
        return result;
    }

    AdapterResult RunAdapter(gpu::FidelityFxDof& dof, const Fixture& fixture,
                             const D3D12_RECT& rect, uint32_t scale=1,
                             float visible_radius=8.0f, float near_scale=1.0f,
                             float far_scale=0.45f,uint32_t rings=4,
                             bool no_merge=false) {
        const auto processing=gpu::FidelityFxDofProcessingSize(fixture.width,fixture.height,scale);
        Check(processing.width && processing.height &&
              dof.Matches(processing.width,processing.height,rings,no_merge),
              "adapter DoF processing extent mismatch");
        Check(rect.left >= 0 && rect.top >= 0 && rect.right <= LONG(fixture.width) &&
              rect.bottom <= LONG(fixture.height) && rect.right>rect.left &&
              rect.bottom>rect.top, "invalid adapter logical viewport");
        EnsureAdapterPipeline();
        auto scene = NewTexture(fixture.width,fixture.height,DXGI_FORMAT_R16G16B16A16_FLOAT,
                                D3D12_RESOURCE_STATE_COPY_DEST);
        auto raw_depth = NewTexture(fixture.width,fixture.height,DXGI_FORMAT_R32_FLOAT,
                                    D3D12_RESOURCE_STATE_COPY_DEST);
        auto prepared = NewTexture(processing.width,processing.height,DXGI_FORMAT_R16G16B16A16_FLOAT,
                                   D3D12_RESOURCE_STATE_RENDER_TARGET,
                                   D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        auto encoded = NewTexture(processing.width,processing.height,DXGI_FORMAT_R32_FLOAT,
                                  D3D12_RESOURCE_STATE_RENDER_TARGET,
                                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        auto filtered = NewTexture(processing.width,processing.height,DXGI_FORMAT_R16G16B16A16_FLOAT,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto destination = NewTexture(fixture.width,fixture.height,DXGI_FORMAT_R16G16B16A16_FLOAT,
                                      D3D12_RESOURCE_STATE_COPY_DEST,
                                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT color_fp{}, depth_fp{},
            processing_color_fp{}, processing_depth_fp{};
        UINT rows = 0; UINT64 row_bytes = 0, color_bytes = 0, depth_bytes = 0,
            processing_color_bytes=0,processing_depth_bytes=0;
        const auto color_desc = scene->GetDesc(), depth_desc = raw_depth->GetDesc();
        const auto processing_color_desc=prepared->GetDesc(),processing_depth_desc=encoded->GetDesc();
        device_->GetCopyableFootprints(&color_desc,0,1,0,&color_fp,&rows,&row_bytes,&color_bytes);
        device_->GetCopyableFootprints(&depth_desc,0,1,0,&depth_fp,&rows,&row_bytes,&depth_bytes);
        device_->GetCopyableFootprints(&processing_color_desc,0,1,0,&processing_color_fp,
                                       &rows,&row_bytes,&processing_color_bytes);
        device_->GetCopyableFootprints(&processing_depth_desc,0,1,0,&processing_depth_fp,
                                       &rows,&row_bytes,&processing_depth_bytes);
        auto scene_upload = NewBuffer(color_bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        auto depth_upload = NewBuffer(depth_bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        auto destination_readback = NewBuffer(color_bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        auto prepared_readback = NewBuffer(processing_color_bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        auto encoded_readback = NewBuffer(processing_depth_bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        std::vector<uint16_t> rgba(fixture.color.size()*4);
        for (size_t i=0; i<fixture.color.size(); ++i) {
            const Pixel& p=fixture.color[i];
            rgba[4*i+0]=Half(p.r); rgba[4*i+1]=Half(p.g);
            rgba[4*i+2]=Half(p.b); rgba[4*i+3]=Half(p.a);
        }
        FillUpload(scene_upload.Get(),color_fp,rgba.data(),fixture.width*8,fixture.height);
        FillUpload(depth_upload.Get(),depth_fp,fixture.depth.data(),fixture.width*4,fixture.height);

        D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
        srv_desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srv_desc.NumDescriptors=4;
        srv_desc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> srv_heap;
        Hr(device_->CreateDescriptorHeap(&srv_desc,IID_PPV_ARGS(&srv_heap)),"Create adapter SRV heap");
        const UINT srv_increment=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto srv_cpu=srv_heap->GetCPUDescriptorHandleForHeapStart();
        auto make_srv=[&](UINT slot, ID3D12Resource* texture, DXGI_FORMAT format) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format=format;
            sv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sv.Texture2D.MipLevels=1;
            D3D12_CPU_DESCRIPTOR_HANDLE handle{srv_cpu.ptr+SIZE_T(slot)*srv_increment};
            device_->CreateShaderResourceView(texture,&sv,handle);
        };
        make_srv(0,raw_depth.Get(),DXGI_FORMAT_R32_FLOAT);
        make_srv(1,scene.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT);
        make_srv(2,filtered.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT);
        make_srv(3,prepared.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT);

        D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
        rtv_desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv_desc.NumDescriptors=3;
        ComPtr<ID3D12DescriptorHeap> rtv_heap;
        Hr(device_->CreateDescriptorHeap(&rtv_desc,IID_PPV_ARGS(&rtv_heap)),"Create adapter RTV heap");
        const UINT rtv_increment=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        auto rtv_cpu=rtv_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[]={{rtv_cpu.ptr},
             {rtv_cpu.ptr+SIZE_T(rtv_increment)}, {rtv_cpu.ptr+SIZE_T(2)*rtv_increment}};
        device_->CreateRenderTargetView(prepared.Get(),nullptr,rtvs[0]);
        device_->CreateRenderTargetView(encoded.Get(),nullptr,rtvs[1]);
        device_->CreateRenderTargetView(destination.Get(),nullptr,rtvs[2]);

        auto constants=NewBuffer(256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        constexpr float projection_x=-0.1001001f, projection_y=1.001001f;
        const float cb[12]={projection_x,projection_y,5.0f,visible_radius*scale,
            float(rect.left),float(rect.top),float(rect.right-rect.left),float(rect.bottom-rect.top),
            near_scale,far_scale,fixture.bound*scale,float(scale)};
        uint8_t* cb_mapped=nullptr;
        D3D12_RANGE no_read{0,0};
        Hr(constants->Map(0,&no_read,reinterpret_cast<void**>(&cb_mapped)),"Map adapter constants");
        std::memcpy(cb_mapped,cb,sizeof(cb));
        constants->Unmap(0,nullptr);

        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        Hr(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),
           "Create adapter allocator");
        Hr(device_->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,
                                      IID_PPV_ARGS(&list)),"Create adapter list");
        CopyUpload(list.Get(),scene.Get(),scene_upload.Get(),color_fp);
        CopyUpload(list.Get(),raw_depth.Get(),depth_upload.Get(),depth_fp);
        auto scene_copy=Transition(scene.Get(),D3D12_RESOURCE_STATE_COPY_DEST,
                                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1,&scene_copy);
        list->CopyResource(destination.Get(),scene.Get());
        D3D12_RESOURCE_BARRIER prepare_inputs[]={
            Transition(scene.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            Transition(raw_depth.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
        list->ResourceBarrier(2,prepare_inputs);
        ID3D12DescriptorHeap* heaps[]={srv_heap.Get()};
        list->SetDescriptorHeaps(1,heaps);
        list->SetGraphicsRootSignature(adapter_root_.Get());
        list->SetGraphicsRootDescriptorTable(0,srv_heap->GetGPUDescriptorHandleForHeapStart());
        list->SetGraphicsRootConstantBufferView(1,constants->GetGPUVirtualAddress());
        list->SetPipelineState(prepare_pso_.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_VIEWPORT processing_viewport{0,0,float(processing.width),float(processing.height),0,1};
        D3D12_RECT full_processing{0,0,LONG(processing.width),LONG(processing.height)};
        list->RSSetViewports(1,&processing_viewport);
        list->RSSetScissorRects(1,&full_processing);
        list->OMSetRenderTargets(2,rtvs,FALSE,nullptr);
        list->DrawInstanced(3,1,0,0);
        D3D12_RESOURCE_BARRIER for_compute[]={
            Transition(prepared.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Transition(encoded.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
        list->ResourceBarrier(2,for_compute);
        if (!dof.Dispatch(list.Get(),prepared.Get(),encoded.Get(),filtered.Get(),fixture.bound*scale))
            throw std::runtime_error(std::string("adapter FidelityFX Dispatch: ")+dof.Failure());
        // The SDK changes heaps/root bindings, so the caller must explicitly restore them.
        list->SetDescriptorHeaps(1,heaps);
        list->SetGraphicsRootSignature(adapter_root_.Get());
        D3D12_GPU_DESCRIPTOR_HANDLE present_table=srv_heap->GetGPUDescriptorHandleForHeapStart();
        present_table.ptr+=UINT64(2)*srv_increment;
        list->SetGraphicsRootDescriptorTable(0,present_table);
        list->SetGraphicsRootConstantBufferView(1,constants->GetGPUVirtualAddress());
        D3D12_RESOURCE_BARRIER for_present[]={
            Transition(filtered.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            Transition(prepared.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            Transition(destination.Get(),D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_RENDER_TARGET)};
        list->ResourceBarrier(3,for_present);
        list->SetPipelineState(present_pso_.Get());
        D3D12_VIEWPORT logical_viewport{0,0,float(fixture.width),float(fixture.height),0,1};
        list->RSSetViewports(1,&logical_viewport);
        list->RSSetScissorRects(1,&rect);
        list->OMSetRenderTargets(1,&rtvs[2],FALSE,nullptr);
        list->DrawInstanced(3,1,0,0);
        D3D12_RESOURCE_BARRIER for_readback[]={
            Transition(destination.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_COPY_SOURCE),
            Transition(prepared.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_COPY_SOURCE),
            Transition(encoded.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_COPY_SOURCE)};
        list->ResourceBarrier(3,for_readback);
        auto copy_to_buffer=[&](ID3D12Resource* texture, ID3D12Resource* buffer,
                                const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint) {
            D3D12_TEXTURE_COPY_LOCATION src{},dst{};
            src.pResource=texture; src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.pResource=buffer; dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint=footprint;
            list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        };
        copy_to_buffer(destination.Get(),destination_readback.Get(),color_fp);
        copy_to_buffer(prepared.Get(),prepared_readback.Get(),processing_color_fp);
        copy_to_buffer(encoded.Get(),encoded_readback.Get(),processing_depth_fp);
        Hr(list->Close(),"Close adapter list");
        ID3D12CommandList* lists[]={list.Get()};
        queue_->ExecuteCommandLists(1,lists);
        Wait();
        AdapterResult result;
        result.destination=ReadColor(destination_readback.Get(),color_fp,fixture.width,fixture.height);
        result.prepared_color=ReadColor(prepared_readback.Get(),processing_color_fp,
                                        processing.width,processing.height);
        result.encoded_depth=ReadFloat(encoded_readback.Get(),processing_depth_fp,
                                        processing.width,processing.height);
        return result;
    }

    void CheckMixedMsaaPrepare() {
        EnsureAdapterPipeline();
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{};
        quality.Format=DXGI_FORMAT_R32_FLOAT;
        quality.SampleCount=4;
        Hr(device_->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                                        &quality,sizeof(quality)),
           "CheckFeatureSupport(R32F,4x)");
        if (!quality.NumQualityLevels) {
            std::puts("4x mixed-MSAA adapter diagnostic UNSUPPORTED on this WARP adapter");
            return;
        }
        constexpr uint32_t width=16,height=16;
        auto ms_desc=TextureDesc(width,height,DXGI_FORMAT_R32_FLOAT,
                                 D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        ms_desc.SampleDesc.Count=4;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> ms_depth;
        Hr(device_->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&ms_desc,
             D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&ms_depth)),
           "Create 4x WARP raw depth RT");
        auto scene=NewTexture(width,height,DXGI_FORMAT_R16G16B16A16_FLOAT,
                              D3D12_RESOURCE_STATE_COPY_DEST);
        auto prepared=NewTexture(width,height,DXGI_FORMAT_R16G16B16A16_FLOAT,
                                 D3D12_RESOURCE_STATE_RENDER_TARGET,
                                 D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        auto encoded=NewTexture(width,height,DXGI_FORMAT_R32_FLOAT,
                                D3D12_RESOURCE_STATE_RENDER_TARGET,
                                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        const auto color_desc=scene->GetDesc(),depth_desc=encoded->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT color_fp{},depth_fp{};
        UINT rows=0; UINT64 row_bytes=0,color_bytes=0,depth_bytes=0;
        device_->GetCopyableFootprints(&color_desc,0,1,0,&color_fp,&rows,&row_bytes,&color_bytes);
        device_->GetCopyableFootprints(&depth_desc,0,1,0,&depth_fp,&rows,&row_bytes,&depth_bytes);
        auto upload=NewBuffer(color_bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        auto color_readback=NewBuffer(color_bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        auto depth_readback=NewBuffer(depth_bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        std::vector<uint16_t> colors(size_t(width)*height*4);
        for(size_t i=0;i<size_t(width)*height;++i) {
            colors[4*i]=Half(0.25f); colors[4*i+1]=Half(0);
            colors[4*i+2]=Half(0.75f); colors[4*i+3]=Half(0.625f);
        }
        FillUpload(upload.Get(),color_fp,colors.data(),width*8,height);
        D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
        rtv_desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv_desc.NumDescriptors=3;
        ComPtr<ID3D12DescriptorHeap> rtvs;
        Hr(device_->CreateDescriptorHeap(&rtv_desc,IID_PPV_ARGS(&rtvs)),"Create MSAA RTV heap");
        const UINT rtv_step=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        auto first_rtv=rtvs->GetCPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE handles[]={{first_rtv.ptr},
            {first_rtv.ptr+SIZE_T(rtv_step)}, {first_rtv.ptr+SIZE_T(2)*rtv_step}};
        device_->CreateRenderTargetView(ms_depth.Get(),nullptr,handles[0]);
        device_->CreateRenderTargetView(prepared.Get(),nullptr,handles[1]);
        device_->CreateRenderTargetView(encoded.Get(),nullptr,handles[2]);
        D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
        srv_desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srv_desc.NumDescriptors=2;
        srv_desc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> srvs;
        Hr(device_->CreateDescriptorHeap(&srv_desc,IID_PPV_ARGS(&srvs)),"Create MSAA SRV heap");
        const UINT srv_step=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format=DXGI_FORMAT_R32_FLOAT;
        sv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DMS;
        sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        auto first_srv=srvs->GetCPUDescriptorHandleForHeapStart();
        device_->CreateShaderResourceView(ms_depth.Get(),&sv,first_srv);
        sv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        sv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels=1;
        D3D12_CPU_DESCRIPTOR_HANDLE scene_srv{first_srv.ptr+SIZE_T(srv_step)};
        device_->CreateShaderResourceView(scene.Get(),&sv,scene_srv);
        auto constants=NewBuffer(256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        const float cb[12]={-0.1001001f,1.001001f,5.0f,8.0f,
                            0,0,float(width),float(height),1.0f,0.45f,8.0f,0};
        uint8_t* mapped=nullptr;
        D3D12_RANGE no_read{0,0};
        Hr(constants->Map(0,&no_read,reinterpret_cast<void**>(&mapped)),"Map MSAA constants");
        std::memcpy(mapped,cb,sizeof(cb));
        constants->Unmap(0,nullptr);

        // Sample-frequency shader writes one near sample and three far samples.
        static constexpr char kMsSource[] =
            "float4 vs(uint id:SV_VertexID):SV_Position {"
            "float2 t=float2((id<<1)&2,id&2);return float4(t*float2(2,-2)+float2(-1,1),0,1); }"
            "float ps(uint sample:SV_SampleIndex):SV_Target {"
            "return sample==0 ? 0.9676343 : 0.9943277; }";
        auto compile_ms=[&](const char* entry,const char* profile) {
            ComPtr<ID3DBlob> blob,errors;
            HRESULT hr=D3DCompile(kMsSource,sizeof(kMsSource)-1,"MSAA fixture",nullptr,nullptr,
                                  entry,profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&blob,&errors);
            if(FAILED(hr)&&errors) std::fprintf(stderr,"MSAA fixture: %s\n",
                 static_cast<const char*>(errors->GetBufferPointer()));
            Hr(hr,"Compile MSAA initializer");
            return blob;
        };
        auto vs=compile_ms("vs","vs_5_0"),init_ps=compile_ms("ps","ps_5_0");
        const D3D_SHADER_MACRO ms4[]={{"MS","1"},{"DOF_SAMPLES","4"},{nullptr,nullptr}};
        auto prepare_ps=Compile("ffx_dof_prepare","ps_5_0",ms4);
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
        p.pRootSignature=adapter_root_.Get();
        p.VS={vs->GetBufferPointer(),vs->GetBufferSize()};
        p.PS={init_ps->GetBufferPointer(),init_ps->GetBufferSize()};
        p.SampleMask=UINT_MAX;
        p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;
        p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        p.RasterizerState.DepthClipEnable=TRUE;
        p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        p.NumRenderTargets=1;
        p.RTVFormats[0]=DXGI_FORMAT_R32_FLOAT;
        p.SampleDesc.Count=4;
        p.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        ComPtr<ID3D12PipelineState> init_pso,ms_prepare_pso;
        Hr(device_->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&init_pso)),
           "Create MSAA initializer PSO");
        auto adapter_vs=Compile("vs","vs_5_0");
        p.VS={adapter_vs->GetBufferPointer(),adapter_vs->GetBufferSize()};
        p.PS={prepare_ps->GetBufferPointer(),prepare_ps->GetBufferSize()};
        p.NumRenderTargets=2;
        p.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;
        p.RTVFormats[1]=DXGI_FORMAT_R32_FLOAT;
        p.SampleDesc.Count=1;
        p.BlendState.RenderTarget[1].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        Hr(device_->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&ms_prepare_pso)),
           "Create MSAA prepare PSO");

        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        Hr(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),
           "Create MSAA allocator");
        Hr(device_->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,
             IID_PPV_ARGS(&list)),"Create MSAA command list");
        CopyUpload(list.Get(),scene.Get(),upload.Get(),color_fp);
        const D3D12_VIEWPORT viewport{0,0,float(width),float(height),0,1};
        const D3D12_RECT scissor{0,0,LONG(width),LONG(height)};
        list->RSSetViewports(1,&viewport);
        list->RSSetScissorRects(1,&scissor);
        list->SetGraphicsRootSignature(adapter_root_.Get());
        list->SetPipelineState(init_pso.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->OMSetRenderTargets(1,&handles[0],FALSE,nullptr);
        list->DrawInstanced(3,1,0,0);
        D3D12_RESOURCE_BARRIER inputs[]={
            Transition(ms_depth.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            Transition(scene.Get(),D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
        list->ResourceBarrier(2,inputs);
        ID3D12DescriptorHeap* heaps[]={srvs.Get()};
        list->SetDescriptorHeaps(1,heaps);
        list->SetGraphicsRootDescriptorTable(0,srvs->GetGPUDescriptorHandleForHeapStart());
        list->SetGraphicsRootConstantBufferView(1,constants->GetGPUVirtualAddress());
        list->SetPipelineState(ms_prepare_pso.Get());
        D3D12_CPU_DESCRIPTOR_HANDLE pair[]={handles[1],handles[2]};
        list->OMSetRenderTargets(2,pair,FALSE,nullptr);
        list->DrawInstanced(3,1,0,0);
        D3D12_RESOURCE_BARRIER reads[]={
            Transition(prepared.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_COPY_SOURCE),
            Transition(encoded.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_COPY_SOURCE)};
        list->ResourceBarrier(2,reads);
        D3D12_TEXTURE_COPY_LOCATION dst{},src{};
        src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.pResource=prepared.Get(); dst.pResource=color_readback.Get();
        dst.PlacedFootprint=color_fp;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        src.pResource=encoded.Get(); dst.pResource=depth_readback.Get();
        dst.PlacedFootprint=depth_fp;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        Hr(list->Close(),"Close MSAA diagnostic list");
        ID3D12CommandList* lists[]={list.Get()};
        queue_->ExecuteCommandLists(1,lists);
        Wait();
        const auto color=ReadColor(color_readback.Get(),color_fp,width,height);
        const auto depth=ReadFloat(depth_readback.Get(),depth_fp,width,height);
        const Pixel center=color[size_t(8)*width+8];
        const float d=depth[size_t(8)*width+8];
        std::printf("4x mixed MSAA prepare: resolved RGB %.4f/%.4f/%.4f, validity %.4f, encoded CoC %.4f\n",
                    center.r,center.g,center.b,center.a,d);
        Check(std::fabs(center.r-0.25f)<0.005f && std::fabs(center.b-0.75f)<0.005f &&
              center.a>0.99f && d<0.48f,
              "MSAA adapter did not pair nearest raw depth with resolved mixed color");
    }

private:
    static std::vector<Pixel> ReadColor(ID3D12Resource* readback,
                                        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
                                        uint32_t width, uint32_t height) {
        uint8_t* mapped=nullptr;
        const D3D12_RANGE range{0,SIZE_T(footprint.Footprint.RowPitch)*(height-1)+SIZE_T(width)*8};
        Hr(readback->Map(0,&range,reinterpret_cast<void**>(&mapped)),"Map adapter color readback");
        std::vector<Pixel> result(size_t(width)*height);
        for(uint32_t y=0;y<height;++y) {
            const auto* row=reinterpret_cast<const uint16_t*>(mapped+y*footprint.Footprint.RowPitch);
            for(uint32_t x=0;x<width;++x)
                result[size_t(y)*width+x]={FloatFromHalf(row[4*x]),FloatFromHalf(row[4*x+1]),
                                            FloatFromHalf(row[4*x+2]),FloatFromHalf(row[4*x+3])};
        }
        const D3D12_RANGE no_write{0,0};
        readback->Unmap(0,&no_write);
        return result;
    }

    static std::vector<float> ReadFloat(ID3D12Resource* readback,
                                        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
                                        uint32_t width, uint32_t height) {
        uint8_t* mapped=nullptr;
        const D3D12_RANGE range{0,SIZE_T(footprint.Footprint.RowPitch)*(height-1)+SIZE_T(width)*4};
        Hr(readback->Map(0,&range,reinterpret_cast<void**>(&mapped)),"Map adapter depth readback");
        std::vector<float> result(size_t(width)*height);
        for(uint32_t y=0;y<height;++y)
            std::memcpy(result.data()+size_t(y)*width,
                        mapped+y*footprint.Footprint.RowPitch,width*sizeof(float));
        const D3D12_RANGE no_write{0,0};
        readback->Unmap(0,&no_write);
        return result;
    }

    void EnsureAdapterPipeline() {
        if (adapter_root_) return;
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors=2;
        range.BaseShaderRegister=0;
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].DescriptorTable.NumDescriptorRanges=1;
        parameters[0].DescriptorTable.pDescriptorRanges=&range;
        parameters[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[1].Descriptor.ShaderRegister=1;
        parameters[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC signature{};
        signature.NumParameters=2;
        signature.pParameters=parameters;
        signature.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> serialized,errors;
        HRESULT hr=D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,
                                               &serialized,&errors);
        if(FAILED(hr) && errors)
            std::fprintf(stderr,"adapter root signature: %s\n",
                         static_cast<const char*>(errors->GetBufferPointer()));
        Hr(hr,"D3D12SerializeRootSignature(adapter)");
        Hr(device_->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),
                                        IID_PPV_ARGS(&adapter_root_)),"CreateRootSignature(adapter)");
        auto vs=Compile("vs","vs_5_0");
        auto prepare=Compile("ffx_dof_prepare","ps_5_0");
        const D3D_SHADER_MACRO present_define[]={{"FFX_DOF_PRESENT","1"},{nullptr,nullptr}};
        auto present=Compile("ffx_dof_present","ps_5_0",present_define);
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature=adapter_root_.Get();
        pipeline.VS={vs->GetBufferPointer(),vs->GetBufferSize()};
        pipeline.PS={prepare->GetBufferPointer(),prepare->GetBufferSize()};
        pipeline.SampleMask=UINT_MAX;
        pipeline.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;
        pipeline.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        pipeline.RasterizerState.DepthClipEnable=TRUE;
        pipeline.DepthStencilState.DepthEnable=FALSE;
        pipeline.DepthStencilState.StencilEnable=FALSE;
        pipeline.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipeline.NumRenderTargets=2;
        pipeline.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;
        pipeline.RTVFormats[1]=DXGI_FORMAT_R32_FLOAT;
        pipeline.SampleDesc.Count=1;
        pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        pipeline.BlendState.RenderTarget[1].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        Hr(device_->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(&prepare_pso_)),
           "CreateGraphicsPipelineState(adapter prepare)");
        pipeline.PS={present->GetBufferPointer(),present->GetBufferSize()};
        pipeline.NumRenderTargets=1;
        pipeline.RTVFormats[1]=DXGI_FORMAT_UNKNOWN;
        pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask=
            D3D12_COLOR_WRITE_ENABLE_RED|D3D12_COLOR_WRITE_ENABLE_GREEN|
            D3D12_COLOR_WRITE_ENABLE_BLUE;
        Hr(device_->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(&present_pso_)),
           "CreateGraphicsPipelineState(adapter present)");
    }

    ComPtr<ID3D12Resource> NewTexture(uint32_t width, uint32_t height, DXGI_FORMAT format,
                                     D3D12_RESOURCE_STATES state,
                                     D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        const auto desc = TextureDesc(width, height, format, flags);
        ComPtr<ID3D12Resource> resource;
        Hr(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
                                            nullptr, IID_PPV_ARGS(&resource)),
           "CreateCommittedResource(texture)");
        return resource;
    }

    ComPtr<ID3D12Resource> NewBuffer(UINT64 bytes, D3D12_HEAP_TYPE heap_type,
                                    D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = heap_type;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> resource;
        Hr(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
                                            nullptr, IID_PPV_ARGS(&resource)),
           "CreateCommittedResource(buffer)");
        return resource;
    }

    static void FillUpload(ID3D12Resource* upload,
                           const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
                           const void* source, uint32_t source_pitch, uint32_t height) {
        uint8_t* mapped = nullptr;
        D3D12_RANGE no_read{0, 0};
        Hr(upload->Map(0, &no_read, reinterpret_cast<void**>(&mapped)), "Map upload");
        const uint8_t* input = static_cast<const uint8_t*>(source);
        for (uint32_t y = 0; y < height; ++y)
            std::memcpy(mapped + footprint.Offset + y * footprint.Footprint.RowPitch,
                        input + y * source_pitch, source_pitch);
        upload->Unmap(0, nullptr);
    }

    static void CopyUpload(ID3D12GraphicsCommandList* list, ID3D12Resource* texture,
                           ID3D12Resource* upload,
                           const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint) {
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = texture;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = upload;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprint;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
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
                std::fprintf(stderr, "D3D12 debug: %s\n", message->pDescription);
                throw std::runtime_error("D3D12 debug layer reported an error");
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
    ComPtr<ID3D12RootSignature> adapter_root_;
    ComPtr<ID3D12PipelineState> prepare_pso_, present_pso_;
    HANDLE event_ = nullptr;
    uint64_t fence_value_ = 0;
    bool debug_enabled_ = false;
};

Fixture MakeFixture(uint32_t width, uint32_t height, float bound) {
    Fixture f{width, height, bound};
    f.color.resize(size_t(width) * height);
    f.depth.resize(f.color.size());
    return f;
}

void CheckFiniteAndBounded(const std::vector<Pixel>& output) {
    for (const Pixel& p : output)
        Check(std::isfinite(p.r) && std::isfinite(p.g) && std::isfinite(p.b) &&
              p.r > -0.05f && p.g > -0.05f && p.b > -0.05f &&
              p.r < 1.05f && p.g < 1.05f && p.b < 1.05f,
              "nonfinite or out-of-range FidelityFX RGB");
}

void CheckFocusIdentity(Warp& warp, gpu::FidelityFxDof& dof,
                        uint32_t width, uint32_t height) {
    auto f = MakeFixture(width, height, 16.0f);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            const size_t i = At(f, x, y);
            const float v = ((x + y) & 1) ? 0.75f : 0.25f;
            f.color[i] = {v, 1.0f-v, 0.25f+v*0.5f, 0.625f};
            f.depth[i] = 0.5f; // exactly zero signed CoC
        }
    const auto output = warp.Run(dof, f);
    CheckFiniteAndBounded(output);
    double max_error = 0;
    for (size_t i = 0; i < output.size(); ++i) {
        max_error = std::max(max_error, double(std::fabs(output[i].r-f.color[i].r)));
        max_error = std::max(max_error, double(std::fabs(output[i].g-f.color[i].g)));
        max_error = std::max(max_error, double(std::fabs(output[i].b-f.color[i].b)));
    }
    std::printf("focus identity %ux%u: max RGB error %.6f\n", width, height, max_error);
    Check(max_error < 0.02, "zero-CoC focus texture was changed");
}

void CheckUniform(Warp& warp, gpu::FidelityFxDof& dof,
                  uint32_t width, uint32_t height) {
    auto f = MakeFixture(width, height, 16.0f);
    std::fill(f.color.begin(), f.color.end(), Pixel{0.25f, 0.5f, 0.75f, 0.375f});
    // Exercise near, focal, and far tiles in the same dispatch.
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
            f.depth[At(f,x,y)] = x < width/3 ? 0.25f : x < 2*width/3 ? 0.5f : 0.75f;
    const auto output = warp.Run(dof, f);
    CheckFiniteAndBounded(output);
    double max_error = 0;
    for (const Pixel& p : output) {
        max_error = std::max(max_error, double(std::fabs(p.r-0.25f)));
        max_error = std::max(max_error, double(std::fabs(p.g-0.5f)));
        max_error = std::max(max_error, double(std::fabs(p.b-0.75f)));
    }
    std::printf("uniform color %ux%u: max RGB error %.6f\n", width, height, max_error);
    Check(max_error < 0.035, "uniform color gained blur energy or dark halos");
}

double VerticalContrast(const Fixture& f, const std::vector<Pixel>& output,
                        uint32_t first_x, uint32_t last_x) {
    double sum = 0;
    uint32_t count = 0;
    for (uint32_t x = first_x; x < last_x; ++x)
        for (uint32_t y = 12; y+1 < f.height-12; ++y) {
            const Pixel& a = output[At(f,x,y)];
            const Pixel& b = output[At(f,x,y+1)];
            sum += std::fabs(double(a.r)-b.r);
            ++count;
        }
    return sum/count;
}

void CheckThreePlanes(Warp& warp, gpu::FidelityFxDof& dof) {
    // At height 512 the SDK's 45/1080 CoC limit is 21.33 full-resolution pixels,
    // so the requested eight-pixel near/far blur is not silently clipped.
    auto f = MakeFixture(128, 512, 16.0f);
    const uint32_t near_end=f.width/3, focus_end=2*f.width/3;
    for (uint32_t y = 0; y < f.height; ++y)
        for (uint32_t x = 0; x < f.width; ++x) {
            const size_t i = At(f,x,y);
            const float v = (y & 1) ? 0.75f : 0.25f;
            f.color[i] = {v, v, v, 0.5f};
            f.depth[i] = x < near_end ? 0.25f : x < focus_end ? 0.5f : 0.75f;
        }
    const auto output = warp.Run(dof, f);
    CheckFiniteAndBounded(output);
    const uint32_t near_center=near_end/2;
    const uint32_t focus_center=(near_end+focus_end)/2;
    const uint32_t far_center=(focus_end+f.width)/2;
    const double near_detail = VerticalContrast(f,output,near_center-4,near_center+4);
    const double focus_detail = VerticalContrast(f,output,focus_center-4,focus_center+4);
    const double far_detail = VerticalContrast(f,output,far_center-4,far_center+4);
    std::printf("three planes: near %.4f, focus %.4f, far %.4f vertical contrast\n",
                near_detail, focus_detail, far_detail);
    Check(focus_detail > 0.3, "focused plane lost high-frequency detail");
    Check(near_detail < focus_detail*0.8 && far_detail < focus_detail*0.8,
          "defocused near/far planes did not soften relative to focus");
}

void CheckContinuousDepth(Warp& warp, gpu::FidelityFxDof& dof) {
    auto f = MakeFixture(128, 512, 16.0f);
    for (uint32_t y = 0; y < f.height; ++y)
        for (uint32_t x = 0; x < f.width; ++x) {
            const size_t i = At(f,x,y);
            const float v = (y & 1) ? 0.75f : 0.25f;
            f.color[i] = {v,v,v,0.5f};
            f.depth[i] = 0.2f + 0.6f * float(x) / float(f.width-1);
        }
    const auto output = warp.Run(dof, f);
    WriteBmp("continuous-before.bmp",f.width,f.height,f.color);
    WriteBmp("continuous-after.bmp",f.width,f.height,output);
    CheckFiniteAndBounded(output);
    const uint32_t near_center=f.width/6,focus_center=f.width/2,
                   far_center=5*f.width/6;
    const double near_detail = VerticalContrast(f,output,near_center-4,near_center+4);
    const double focus_detail = VerticalContrast(f,output,focus_center-4,focus_center+4);
    const double far_detail = VerticalContrast(f,output,far_center-4,far_center+4);
    double largest_mean_jump = 0;
    double previous_mean = 0;
    for (uint32_t x = 8; x < f.width-8; ++x) {
        double mean = 0;
        for (uint32_t y = 12; y < 52; ++y) mean += output[At(f,x,y)].r;
        mean /= 40;
        if (x > 8) largest_mean_jump = std::max(largest_mean_jump,
                                                  std::fabs(mean-previous_mean));
        previous_mean = mean;
    }
    std::printf("continuous CoC ramp: near %.4f, focus %.4f, far %.4f contrast; "
                "largest mean-column jump %.4f\n", near_detail, focus_detail,
                far_detail, largest_mean_jump);
    Check(focus_detail > 0.28 && near_detail < focus_detail*0.85 &&
          far_detail < focus_detail*0.85,
          "continuous depth ramp did not preserve a sharper focus region");
    Check(largest_mean_jump < 0.18,
          "continuous depth ramp introduced a visible mean-color seam");
}

void CheckForegroundEdge(Warp& warp, gpu::FidelityFxDof& dof) {
    auto f = MakeFixture(128, 512, 16.0f);
    const uint32_t edge=f.width/2;
    for (uint32_t y = 0; y < f.height; ++y)
        for (uint32_t x = 0; x < f.width; ++x) {
            const size_t i = At(f,x,y);
            f.color[i] = x < edge ? Pixel{0.9f,0.1f,0.05f,0.5f}
                                : Pixel{0.05f,0.1f,0.9f,0.5f};
            f.depth[i] = x < edge ? 0.125f : 0.875f;
        }
    const auto output = warp.Run(dof, f);
    WriteBmp("foreground-before.bmp",f.width,f.height,f.color);
    WriteBmp("foreground-after.bmp",f.width,f.height,output);
    CheckFiniteAndBounded(output);
    const uint32_t cy=f.height/2;
    const Pixel left = output[At(f,edge-36,cy)];
    const Pixel right = output[At(f,edge+36,cy)];
    Check(left.r > left.b+0.5f && right.b > right.r+0.5f,
          "near/far interiors lost their dominant colors");
    double min_red = 1, max_red = 0;
    for (uint32_t x = edge-8; x < edge+8; ++x) {
        min_red = std::min(min_red, double(output[At(f,x,cy)].r));
        max_red = std::max(max_red, double(output[At(f,x,cy)].r));
    }
    double spill=0, strongest_spill=0;
    for(uint32_t x=edge;x<edge+32;++x) {
        const double excess=std::max(0.0,double(output[At(f,x,cy)].r)-0.05);
        spill+=excess;
        if(x<edge+12) strongest_spill=std::max(strongest_spill,excess);
    }
    const uint32_t distant_x=edge+27;
    const double distant_spill=output[At(f,distant_x,cy)].r-0.05;
    double bottom_seam=0;
    // This scene is vertically invariant. Its near/far silhouette should not
    // change at the bottom tile boundary; an unwritten SDK tile row violates it.
    for(uint32_t y=f.height-4;y<f.height;++y)
        for(uint32_t x=edge-12;x<edge+12;++x) {
            const Pixel& bottom=output[At(f,x,y)];
            const Pixel& middle=output[At(f,x,cy)];
            bottom_seam=std::max(bottom_seam,double(std::fabs(bottom.r-middle.r)));
            bottom_seam=std::max(bottom_seam,double(std::fabs(bottom.g-middle.g)));
            bottom_seam=std::max(bottom_seam,double(std::fabs(bottom.b-middle.b)));
        }
    std::printf("foreground edge: interior near red %.4f, far blue %.4f; edge red range %.4f..%.4f; "
                "near spill %.4f integrated, local peak %.4f, x%u excess %.4f; bottom seam %.4f\n",
                left.r, right.b, min_red, max_red, spill, strongest_spill,distant_x,distant_spill,
                bottom_seam);
    Check(max_red-min_red > 0.2, "foreground edge collapsed to a flat or invalid color");
    Check(strongest_spill > 0.08, "out-of-focus near foreground did not spread past its edge");
    Check(spill < 12.0 && distant_spill < 0.08,
          "near foreground spread exceeded the expected 12-pixel aperture");
    Check(bottom_seam < 0.10,
          "near/far silhouette changed at the bottom SDK tile boundary");
}

float RawDepth(float view_z) {
    return 1.001001f - 0.1001001f/view_z;
}

void CheckAdapterFlow(Warp& warp, gpu::FidelityFxDof& dof, uint32_t scale=1) {
    auto f=MakeFixture(96,64,8.0f);
    const auto processing=gpu::FidelityFxDofProcessingSize(f.width,f.height,scale);
    const auto at_processed=[&](uint32_t x,uint32_t y){
        return size_t(y*scale)*processing.width+x*scale;
    };
    const D3D12_RECT rect{7,9,89,55};
    for(uint32_t y=0;y<f.height;++y)
        for(uint32_t x=0;x<f.width;++x) {
            const size_t i=At(f,x,y);
            const float v=(y&1)?0.75f:0.25f;
            const bool outside=x<uint32_t(rect.left)||x>=uint32_t(rect.right)||
                               y<uint32_t(rect.top)||y>=uint32_t(rect.bottom);
            f.color[i]=outside?Pixel{0.875f,0.125f,0.0625f,0.375f}
                              :Pixel{v,v,v,0.625f};
            const float z=x<34?3.0f:x<62?5.0f:15.0f;
            f.depth[i]=RawDepth(z);
        }
    f.depth[At(f,48,32)]=-1.0f; // invalid raw depth: present must discard.
    const auto result=warp.RunAdapter(dof,f,rect,scale,8.0f,1.0f,0.45f,
                                      scale==2?8u:4u,scale==2);
    CheckFiniteAndBounded(result.destination);
    double max_alpha_error=0, max_outside_error=0;
    for(uint32_t y=0;y<f.height;++y)
        for(uint32_t x=0;x<f.width;++x) {
            const size_t i=At(f,x,y);
            const Pixel& p=result.destination[i], &source=f.color[i];
            max_alpha_error=std::max(max_alpha_error,double(std::fabs(p.a-source.a)));
            const bool outside=x<uint32_t(rect.left)||x>=uint32_t(rect.right)||
                               y<uint32_t(rect.top)||y>=uint32_t(rect.bottom);
            if(outside) {
                max_outside_error=std::max(max_outside_error,double(std::fabs(p.r-source.r)));
                max_outside_error=std::max(max_outside_error,double(std::fabs(p.g-source.g)));
                max_outside_error=std::max(max_outside_error,double(std::fabs(p.b-source.b)));
            }
        }
    const size_t invalid=At(f,48,32);
    Check(result.prepared_color[at_processed(48,32)].a==0.0f,
          "prepare did not mark invalid raw depth");
    Check(std::fabs(result.destination[invalid].r-f.color[invalid].r)<0.001f,
          "present overwrote invalid-depth source pixel");
    const float near_d=result.encoded_depth[at_processed(20,32)];
    const float focus_d=result.encoded_depth[at_processed(48,31)];
    const float far_d=result.encoded_depth[at_processed(75,32)];
    std::printf("adapter %ux prepare depth: near %.4f, focus %.4f, far %.4f; "
                "max alpha error %.6f, outside RGB error %.6f\n",
                scale,near_d,focus_d,far_d,max_alpha_error,max_outside_error);
    Check(near_d<0.48f && std::fabs(focus_d-0.5f)<0.005f && far_d>0.52f,
          "prepare did not encode signed near/focus/far CoC");
    Check(max_alpha_error<0.001 && max_outside_error<0.001,
          "present changed destination alpha or outside-scissor color");
    Check(result.prepared_color[At(f,0,0)].a>0.99f,
          "prepare did not extend clamped viewport across SDK input");
}

void CheckOddAdapter(Warp& warp, gpu::FidelityFxDof& dof, uint32_t scale=1) {
    auto f=MakeFixture(79,65,8.0f);
    const auto processing=gpu::FidelityFxDofProcessingSize(f.width,f.height,scale);
    Check(processing.width==(scale==1?128u:192u) &&
          processing.height==(scale==1?128u:192u) &&
          dof.Matches(processing.width,processing.height,
                      scale==2?8u:4u,scale==2),
          "odd logical extent did not use the production 64-aligned processing size");
    const D3D12_RECT full{0,0,LONG(f.width),LONG(f.height)};
    for(uint32_t y=0;y<f.height;++y)
        for(uint32_t x=0;x<f.width;++x) {
            const size_t i=At(f,x,y);
            const float v=((x+y)&1)?0.75f:0.25f;
            f.color[i]={v,1-v,0.5f,0.375f};
            f.depth[i]=RawDepth(5.0f);
        }
    auto focus=warp.RunAdapter(dof,f,full,scale,8.0f,1.0f,0.45f,
                               scale==2?8u:4u,scale==2);
    CheckFiniteAndBounded(focus.destination);
    double max_focus_error=0,max_alpha_error=0;
    for(size_t i=0;i<f.color.size();++i) {
        max_focus_error=std::max(max_focus_error,double(std::fabs(focus.destination[i].r-f.color[i].r)));
        max_focus_error=std::max(max_focus_error,double(std::fabs(focus.destination[i].g-f.color[i].g)));
        max_alpha_error=std::max(max_alpha_error,double(std::fabs(focus.destination[i].a-f.color[i].a)));
    }
    std::printf("odd 79x65 %ux padded %ux%u focus: max RGB error %.6f, alpha %.6f\n",
                scale,processing.width,processing.height,max_focus_error,max_alpha_error);
    Check(max_focus_error<0.02 && max_alpha_error<0.001,
          "odd logical focus or destination alpha changed through padded adapter");
    std::fill(f.color.begin(),f.color.end(),Pixel{0.25f,0.5f,0.75f,0.375f});
    for(uint32_t y=0;y<f.height;++y)
        for(uint32_t x=0;x<f.width;++x)
            f.depth[At(f,x,y)]=RawDepth(x<f.width/3?3.0f:x<2*f.width/3?5.0f:15.0f);
    auto uniform=warp.RunAdapter(dof,f,full,scale,8.0f,1.0f,0.45f,
                                 scale==2?8u:4u,scale==2);
    CheckFiniteAndBounded(uniform.destination);
    double max_uniform_error=0;
    for(const Pixel& p:uniform.destination) {
        max_uniform_error=std::max(max_uniform_error,double(std::fabs(p.r-0.25f)));
        max_uniform_error=std::max(max_uniform_error,double(std::fabs(p.g-0.5f)));
        max_uniform_error=std::max(max_uniform_error,double(std::fabs(p.b-0.75f)));
        max_alpha_error=std::max(max_alpha_error,double(std::fabs(p.a-0.375f)));
    }
    const Pixel padded_corner=uniform.prepared_color[size_t(processing.height-1)*processing.width+
                                                     processing.width-1];
    std::printf("odd 79x65 %ux padded %ux%u uniform: max RGB error %.6f, alpha %.6f, "
                "prepared padded corner %.4f/%.4f/%.4f validity %.4f\n",
                scale,processing.width,processing.height,max_uniform_error,max_alpha_error,
                padded_corner.r,padded_corner.g,
                padded_corner.b,padded_corner.a);
    Check(padded_corner.a>0.99f && std::fabs(padded_corner.r-0.25f)<0.002f &&
          std::fabs(padded_corner.b-0.75f)<0.002f,
          "prepare failed to extend logical edge into aligned padding");
    Check(max_uniform_error<0.035 && max_alpha_error<0.001,
          "odd logical uniform scene gained blur energy or changed destination alpha");
}

void CheckCinematicOddBottomEdge(Warp& warp, gpu::FidelityFxDof& dof) {
    auto f=MakeFixture(79,65,8.0f);
    const uint32_t edge=f.width/2;
    for(uint32_t y=0;y<f.height;++y)
        for(uint32_t x=0;x<f.width;++x) {
            const size_t i=At(f,x,y);
            f.color[i]=x<edge?Pixel{0.9f,0.1f,0.05f,0.375f}
                              :Pixel{0.05f,0.1f,0.9f,0.375f};
            f.depth[i]=RawDepth(x<edge?3.0f:90.0f);
        }
    const D3D12_RECT full{0,0,LONG(f.width),LONG(f.height)};
    const auto output=warp.RunAdapter(dof,f,full,2,8.0f,1.0f,0.45f,8,true).destination;
    CheckFiniteAndBounded(output);
    double seam=0;
    for(uint32_t y=f.height-4;y<f.height;++y)
        for(uint32_t x=edge-8;x<edge+8;++x) {
            const Pixel& bottom=output[At(f,x,y)];
            const Pixel& middle=output[At(f,x,f.height/2)];
            seam=std::max(seam,double(std::fabs(bottom.r-middle.r)));
            seam=std::max(seam,double(std::fabs(bottom.g-middle.g)));
            seam=std::max(seam,double(std::fabs(bottom.b-middle.b)));
        }
    std::printf("Cinematic 2x odd 79x65 foreground bottom seam %.5f\n",seam);
    Check(seam<0.10,"Cinematic scale2 odd viewport gained a bottom-edge tile seam");
}

enum class QualityKind { Texture, StripePhases, SlantedEdge, Highlight };

const char* QualityName(QualityKind kind) {
    switch(kind) {
    case QualityKind::Texture: return "texture";
    case QualityKind::StripePhases: return "stripe-phases";
    case QualityKind::SlantedEdge: return "slanted-edge";
    case QualityKind::Highlight: return "highlight";
    }
    return "unknown";
}

Fixture QualityFixture(QualityKind kind) {
    auto f=MakeFixture(128,384,30.0f);
    for(uint32_t y=0;y<f.height;++y)
        for(uint32_t x=0;x<f.width;++x) {
            float v=0;
            switch(kind) {
            case QualityKind::Texture: {
                // Nonperiodic high-frequency texture exposes half-resolution grid phase.
                uint32_t hash=x*0x9E3779B1u ^ y*0x85EBCA77u;
                hash^=hash>>16; hash*=0x7FEB352Du;
                hash^=hash>>15; hash*=0x846CA68Bu;
                hash^=hash>>16;
                v=0.2f+0.6f*float(hash&255u)/255.0f;
                break;
            }
            case QualityKind::StripePhases:
                // Two isolated 1px vertical features differing only by pixel phase.
                v=(y>=32 && y<160 && x==63) || (y>=224 && y<352 && x==64)
                    ? 0.98f : 0.02f;
                break;
            case QualityKind::SlantedEdge: v=x<32+y/6 ? 0.1f:0.9f; break;
            case QualityKind::Highlight: v=x==64 && y==192 ? 0.98f:0.01f; break;
            }
            const size_t i=At(f,x,y);
            f.color[i]={v,v,v,0.625f};
            f.depth[i]=RawDepth(90.0f);
        }
    return f;
}

float VisibleFarCoc(float radius) {
    // Same depth/focus/focus-band/far-scale input as the production adapter.
    return radius * 0.45f * (90.0f-5.0f-5.0f*0.15f)/90.0f;
}

std::vector<Pixel> CpuDiscReference(const Fixture& f, float radius) {
    struct Offset { int dx,dy; };
    std::vector<Offset> aperture;
    const int reach=int(std::ceil(radius));
    for(int dy=-reach;dy<=reach;++dy)
        for(int dx=-reach;dx<=reach;++dx)
            if(float(dx*dx+dy*dy)<=radius*radius)
                aperture.push_back({dx,dy});
    Check(!aperture.empty(),"empty CPU aperture reference");
    std::vector<Pixel> reference(f.color.size());
    for(uint32_t y=0;y<f.height;++y)
        for(uint32_t x=0;x<f.width;++x) {
            double red=0,green=0,blue=0;
            for(const auto& tap:aperture) {
                const auto sx=uint32_t(std::clamp(int(x)+tap.dx,0,int(f.width)-1));
                const auto sy=uint32_t(std::clamp(int(y)+tap.dy,0,int(f.height)-1));
                const Pixel& sample=f.color[At(f,sx,sy)];
                red+=sample.r; green+=sample.g; blue+=sample.b;
            }
            const double normalization=1.0/double(aperture.size());
            reference[At(f,x,y)]={float(red*normalization),float(green*normalization),
                                  float(blue*normalization),f.color[At(f,x,y)].a};
        }
    return reference;
}

double DiscRmse(const Fixture& f, const std::vector<Pixel>& output,
                const std::vector<Pixel>& reference) {
    double squared=0;
    uint64_t count=0;
    for(uint32_t y=16;y<f.height-16;++y)
        for(uint32_t x=16;x<f.width-16;++x) {
            const double difference=double(output[At(f,x,y)].r)-reference[At(f,x,y)].r;
            squared+=difference*difference;
            ++count;
        }
    return std::sqrt(squared/count);
}

struct QualityMetrics {
    double rmse=0, detail=0, texture_mean=0, texture_stddev=0;
    double stripe_even_energy=0, stripe_odd_energy=0, stripe_phase_error=0;
    double edge_max_step=0, edge_left=0, edge_right=0, edge_width=0;
    double highlight_peak=0, highlight_energy=0, highlight_background=0;
};

QualityMetrics MeasureQuality(QualityKind kind, const Fixture& f,
                              const std::vector<Pixel>& output,
                              const std::vector<Pixel>& reference,float visible_coc) {
    CheckFiniteAndBounded(output);
    QualityMetrics metric;
    metric.rmse=DiscRmse(f,output,reference);
    double max_alpha_error=0;
    for(size_t i=0;i<output.size();++i)
        max_alpha_error=std::max(max_alpha_error,
            double(std::fabs(output[i].a-f.color[i].a)));
    Check(max_alpha_error<0.001,"FidelityFX present changed destination alpha");
    switch(kind) {
    case QualityKind::Texture: {
        metric.detail=VerticalContrast(f,output,48,80);
        double sum=0,sum_squared=0; uint64_t n=0;
        for(uint32_t y=32;y<f.height-32;++y)
            for(uint32_t x=32;x<f.width-32;++x) {
                const double v=output[At(f,x,y)].r;
                sum+=v; sum_squared+=v*v; ++n;
            }
        metric.texture_mean=sum/n;
        metric.texture_stddev=std::sqrt(std::max(0.0,sum_squared/n-
                                                metric.texture_mean*metric.texture_mean));
        Check(metric.detail<0.30,"defocused far texture retained excessive one-pixel detail");
        Check(metric.texture_mean>0.43 && metric.texture_mean<0.57 &&
              metric.texture_stddev>0.003,
              "defocused far texture lost its mean or all spatial structure");
        break;
    }
    case QualityKind::StripePhases: {
        double phase_difference=0;
        for(uint32_t x=24;x<103;++x) {
            metric.stripe_even_energy+=std::max(0.0,double(output[At(f,x,96)].r)-0.02);
            metric.stripe_odd_energy+=std::max(0.0,double(output[At(f,x,288)].r)-0.02);
            if(x<102) phase_difference+=std::fabs(double(output[At(f,x,96)].r)-
                                                   output[At(f,x+1,288)].r);
        }
        metric.stripe_phase_error=phase_difference/
            std::max(0.001,0.5*(metric.stripe_even_energy+metric.stripe_odd_energy));
        Check(metric.stripe_even_energy>0.20 && metric.stripe_even_energy<2.50 &&
              metric.stripe_odd_energy>0.20 && metric.stripe_odd_energy<2.50,
              "one-pixel far stripes lost or amplified most of their energy");
        break;
    }
    case QualityKind::SlantedEdge:
        for(uint32_t y=40;y<f.height-40;y+=16)
            for(uint32_t x=24;x<104;++x)
                metric.edge_max_step=std::max(metric.edge_max_step,
                    double(std::fabs(output[At(f,x+1,y)].r-output[At(f,x,y)].r)));
        Check(metric.edge_max_step<0.72,
              "defocused slanted edge stayed near an unblurred one-pixel step");
        metric.edge_left=output[At(f,24,192)].r;
        metric.edge_right=output[At(f,104,192)].r;
        Check(metric.edge_left<0.25 && metric.edge_right>0.75,
              "slanted-edge blur flattened the foreground/background contrast");
        {
            int low=-1,high=-1;
            for(uint32_t x=24;x<104;++x) {
                const float value=output[At(f,x,192)].r;
                if(low<0 && value>=0.18f) low=int(x);
                if(high<0 && value>=0.82f) high=int(x);
            }
            Check(low>=0 && high>=low,"slanted-edge 10-90 crossings missing");
            metric.edge_width=high-low;
            Check(metric.edge_width>=2 && metric.edge_width<=2*visible_coc+8,
                  "slanted-edge transition width is too sharp or too broad");
        }
        break;
    case QualityKind::Highlight:
        // Use the actual FP16/SDK floor away from the impulse. Subtracting
        // decimal 0.01 over 49k pixels adds false positive energy.
        metric.highlight_background=output[At(f,12,12)].r;
        for(uint32_t y=128;y<256;++y)
            for(uint32_t x=16;x<112;++x) {
                const Pixel& p=output[At(f,x,y)];
                metric.highlight_peak=std::max(metric.highlight_peak,double(p.r));
                metric.highlight_energy+=std::max(0.0,
                    double(p.r)-metric.highlight_background);
            }
        Check(metric.highlight_peak<0.60 && metric.highlight_energy>0.20 &&
              metric.highlight_energy<2.50,
              "isolated far highlight did not spread with bounded energy");
        break;
    }
    return metric;
}

Fixture ShiftFixture(const Fixture& original,int dx,int dy) {
    Fixture shifted=original;
    for(uint32_t y=0;y<original.height;++y)
        for(uint32_t x=0;x<original.width;++x) {
            const uint32_t sx=uint32_t(std::clamp(int(x)-dx,0,int(original.width)-1));
            const uint32_t sy=uint32_t(std::clamp(int(y)-dy,0,int(original.height)-1));
            shifted.color[At(original,x,y)]=original.color[At(original,sx,sy)];
        }
    return shifted;
}

double PhaseRms(const Fixture& f, const std::vector<Pixel>& original,
                const std::vector<Pixel>& shifted, int dx,int dy,
                std::vector<Pixel>* difference_image=nullptr) {
    // Exclude more than the visible aperture plus reconstruction footprint.
    constexpr uint32_t margin=24;
    double squared=0; uint64_t count=0;
    if(difference_image)
        difference_image->assign(f.color.size(),Pixel{0,0,0,1});
    for(uint32_t y=margin;y<f.height-margin;++y)
        for(uint32_t x=margin;x<f.width-margin;++x) {
            const Pixel& a=original[At(f,x,y)];
            const Pixel& b=shifted[At(f,uint32_t(int(x)+dx),uint32_t(int(y)+dy))];
            const double dr=double(a.r)-b.r,dg=double(a.g)-b.g,db=double(a.b)-b.b;
            squared+=dr*dr+dg*dg+db*db;
            count+=3;
            if(difference_image)
                (*difference_image)[At(f,x,y)]={float(std::min(1.0,std::fabs(dr)*8)),
                    float(std::min(1.0,std::fabs(dg)*8)),float(std::min(1.0,std::fabs(db)*8)),1};
        }
    return std::sqrt(squared/count);
}

void CompareQualityModes(Warp& warp, gpu::FidelityFxDof& dof) {
    struct Mode { const char* name; uint32_t scale,rings; bool no_merge; };
    constexpr Mode modes[]={
        {"native4-merge",1,4,false}, {"native4-no-merge",1,4,true},
        {"native8-no-merge",1,8,true}, {"scale2-8-no-merge",2,8,true},
    };
    struct Scene { QualityKind kind; float visible_radius; const char* band; };
    constexpr Scene scenes[]={
        {QualityKind::Texture,30.0f,"large"},
        {QualityKind::StripePhases,30.0f,"large"},
        {QualityKind::SlantedEdge,30.0f,"large"},
        {QualityKind::Highlight,30.0f,"large"},
        {QualityKind::Texture,9.5f,"small"},
        {QualityKind::SlantedEdge,9.5f,"small"},
    };
    struct Case { Scene scene; Fixture fixture; std::vector<Pixel> reference; };
    std::vector<Case> cases;
    cases.reserve(std::size(scenes));
    for(const Scene& scene:scenes) {
        auto fixture=QualityFixture(scene.kind);
        auto reference=CpuDiscReference(fixture,VisibleFarCoc(scene.visible_radius));
        const std::string common=std::string("quality-")+scene.band+"-"+
                                 QualityName(scene.kind);
        WriteBmp((common+"-input.bmp").c_str(),fixture.width,fixture.height,fixture.color);
        WriteBmp((common+"-cpu-disc.bmp").c_str(),fixture.width,fixture.height,reference);
        cases.push_back({scene,std::move(fixture),std::move(reference)});
    }
    for(const Mode& mode:modes) {
        const auto processing=gpu::FidelityFxDofProcessingSize(128,384,mode.scale);
        dof.Reset();
        Check(dof.Create(warp.Device(),processing.width,processing.height,
                         mode.rings,mode.no_merge),dof.Failure());
        Check(dof.Matches(processing.width,processing.height,mode.rings,mode.no_merge),
              "FidelityFX quality configuration did not match requested mode");
        const D3D12_RECT full{0,0,128,384};
        for(const Case& test:cases) {
            const Scene& scene=test.scene;
            const Fixture& fixture=test.fixture;
            const auto& reference=test.reference;
            const auto output=warp.RunAdapter(dof,fixture,full,mode.scale,
                                              scene.visible_radius,1.0f,0.45f,
                                              mode.rings,mode.no_merge).destination;
            const std::string prefix=std::string("quality-")+mode.name+"-"+
                                     scene.band+"-"+QualityName(scene.kind);
            WriteBmp((prefix+"-output.bmp").c_str(),fixture.width,fixture.height,output);
            const float coc=VisibleFarCoc(scene.visible_radius);
            const auto m=MeasureQuality(scene.kind,fixture,output,reference,coc);
            std::printf("quality %s %s %s CoC %.3fpx: discRMSE %.5f detail %.5f "
                        "textureMean %.4f stddev %.4f stripeEnergy %.4f/%.4f "
                        "phaseError %.4f edgeStep %.4f edgeWidth %.1f "
                        "highlightPeak %.4f energy %.4f background %.6f\n",
                mode.name,scene.band,QualityName(scene.kind),coc,
                m.rmse,m.detail,m.texture_mean,m.texture_stddev,
                m.stripe_even_energy,m.stripe_odd_energy,m.stripe_phase_error,
                m.edge_max_step,m.edge_width,m.highlight_peak,m.highlight_energy,
                m.highlight_background);
            if(scene.kind==QualityKind::Texture && scene.visible_radius==9.5f) {
                constexpr int shifts[3][2]={{1,0},{0,1},{1,1}};
                double rms[3]{};
                for(int j=0;j<3;++j) {
                    const auto shifted_fixture=ShiftFixture(fixture,shifts[j][0],shifts[j][1]);
                    const auto shifted_output=warp.RunAdapter(dof,shifted_fixture,full,
                                                              mode.scale,scene.visible_radius,
                                                              1.0f,0.45f,mode.rings,
                                                              mode.no_merge).destination;
                    CheckFiniteAndBounded(shifted_output);
                    std::vector<Pixel> heatmap;
                    rms[j]=PhaseRms(fixture,output,shifted_output,
                                    shifts[j][0],shifts[j][1],j==2?&heatmap:nullptr);
                    if(j==2)
                        WriteBmp((prefix+"-phase-difference-8x.bmp").c_str(),
                                 fixture.width,fixture.height,heatmap);
                }
                std::printf("phase %s %.3fpx CoC: shiftX %.6f shiftY %.6f shiftXY %.6f RMS RGB\n",
                            mode.name,coc,rms[0],rms[1],rms[2]);
            }
        }
    }
    dof.Reset();
}

} // namespace

int main() {
    try {
        CheckAdapterCompilation();
        Warp warp;
        gpu::FidelityFxDof dof;
        Check(dof.Create(warp.Device(), 96, 64), dof.Failure());
        Check(dof.Ready() && dof.Matches(96,64), "initial FidelityFX context not ready");
        CheckFocusIdentity(warp,dof,96,64);
        CheckUniform(warp,dof,96,64);
        dof.Reset();
        const auto adapter_size=gpu::FidelityFxDofProcessingSize(96,64);
        Check(dof.Create(warp.Device(),adapter_size.width,adapter_size.height),dof.Failure());
        CheckAdapterFlow(warp,dof);
        warp.CheckMixedMsaaPrepare();
        // Run() waits for the GPU before context destruction/recreation.
        dof.Reset();
        Check(!dof.Ready(), "Reset left context active");
        Check(dof.Create(warp.Device(),128,512), dof.Failure());
        CheckThreePlanes(warp,dof);
        CheckContinuousDepth(warp,dof);
        CheckForegroundEdge(warp,dof);
        dof.Reset();
        const auto odd_size=gpu::FidelityFxDofProcessingSize(79,65);
        Check(dof.Create(warp.Device(),odd_size.width,odd_size.height), dof.Failure());
        Check(dof.Matches(128,128) && !dof.Matches(128,512),
              "odd-resolution recreation retained stale extent");
        CheckOddAdapter(warp,dof);
        dof.Reset();
        const auto cinematic_adapter=gpu::FidelityFxDofProcessingSize(96,64,2);
        Check(dof.Create(warp.Device(),cinematic_adapter.width,cinematic_adapter.height,8,true),
              dof.Failure());
        Check(dof.Matches(cinematic_adapter.width,cinematic_adapter.height,8,true),
              "Cinematic adapter context did not retain 8 rings without merge");
        CheckAdapterFlow(warp,dof,2);
        dof.Reset();
        const auto cinematic_odd=gpu::FidelityFxDofProcessingSize(79,65,2);
        Check(dof.Create(warp.Device(),cinematic_odd.width,cinematic_odd.height,8,true),
              dof.Failure());
        CheckOddAdapter(warp,dof,2);
        CheckCinematicOddBottomEdge(warp,dof);
        dof.Reset();
        CompareQualityModes(warp,dof);
        std::puts("PASS: production FidelityFX DoF on D3D12 WARP");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
