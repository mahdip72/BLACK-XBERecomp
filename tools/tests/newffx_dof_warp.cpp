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
#include "d3d/dof_gpu_timing.h"

using Microsoft::WRL::ComPtr;

namespace {

struct Pixel { float r, g, b, a; };
struct Fixture {
    uint32_t width, height;
    float bound;
    std::vector<Pixel> color;
    std::vector<float> depth;
    std::vector<uint16_t> color_bits; // Optional exact FP16 payloads for representation contracts.
};
struct CapturedTexture {
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0, height = 0;
    std::vector<std::vector<uint8_t>> mips;
};
struct PendingTextureCapture {
    ComPtr<ID3D12Resource> readback;
    CapturedTexture texture;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    std::vector<UINT64> row_bytes;
    UINT64 bytes = 0;
};
struct AdapterPersistentOutput {
    ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
};
struct AdapterResult {
    std::vector<Pixel> destination;
    std::vector<Pixel> prepared_color;
    std::vector<float> encoded_depth;
    std::vector<Pixel> sdk_output;
    std::vector<uint16_t> sdk_half_bits;
    std::vector<uint16_t> destination_bits;
    std::vector<uint16_t> prepared_bits;
    std::array<CapturedTexture,4> reuse_resources; // dirty hierarchy, certificate, near, far
    std::array<CapturedTexture,4> world_cache_resources; // predicate, color/depth history, unsafe groups
    bool world_cache_history_valid = false;
    uint64_t world_cache_generation = 0, world_cache_queue = 0;
    uint32_t reuse_group_width = 0, reuse_group_height = 0;
    bool reuse_history_valid = false;
    gpu::DofGpuTiming::FrameSample timing{};
    uint64_t timing_frequency = 0;
    uint32_t timing_repetitions = 0;
    bool has_timing = false;
};

struct SdkTimingHooks {
    gpu::DofGpuTiming* timing = nullptr;
    std::array<gpu::DofGpuTiming::Scope, 5> scopes{};
    std::array<bool, 5> started{};
    std::array<bool, 5> ended{};
    uint32_t next = 0;
    bool invalid = false;

    static void Mark(void* user, ID3D12GraphicsCommandList* list,
                     gpu::FidelityFxDofPass pass, bool begin) noexcept {
        auto& self = *static_cast<SdkTimingHooks*>(user);
        // The SDK parent interval includes candidate comparison/reduction work. Preserve
        // the five original child labels and their order for the existing benchmark schema.
        if(pass==gpu::FidelityFxDofPass::CompareHistory || pass==gpu::FidelityFxDofPass::ReduceDirty ||
           pass==gpu::FidelityFxDofPass::WorldCompare) return;
        const uint32_t index = static_cast<uint32_t>(pass);
        if (!self.timing || !list || index >= self.scopes.size() || index != self.next) {
            self.invalid = true;
            return;
        }
        constexpr gpu::DofGpuTiming::Stage stages[] = {
            gpu::DofGpuTiming::Stage::SdkDownsampleDepth,
            gpu::DofGpuTiming::Stage::SdkDownsampleColor,
            gpu::DofGpuTiming::Stage::SdkDilate,
            gpu::DofGpuTiming::Stage::SdkBlur,
            gpu::DofGpuTiming::Stage::SdkComposite,
        };
        if (begin) {
            if (self.started[index] || self.ended[index]) { self.invalid = true; return; }
            self.started[index] = true;
            self.scopes[index] = self.timing->Measure(stages[index], list);
        } else {
            if (!self.started[index] || self.ended[index]) { self.invalid = true; return; }
            self.scopes[index].End();
            self.ended[index] = true;
            ++self.next;
        }
    }
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
    explicit Warp(bool hardware = false) {
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
        if (hardware) {
            for (UINT index = 0;; ++index) {
                ComPtr<IDXGIAdapter1> candidate;
                const HRESULT enumeration = factory_->EnumAdapterByGpuPreference(
                    index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate));
                if (enumeration == DXGI_ERROR_NOT_FOUND) break;
                Hr(enumeration, "EnumAdapterByGpuPreference");
                DXGI_ADAPTER_DESC1 candidate_desc{};
                Hr(candidate->GetDesc1(&candidate_desc), "hardware GetDesc1");
                if (candidate_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
                adapter_ = std::move(candidate);
                break;
            }
            Check(adapter_.Get() != nullptr, "no hardware D3D12 adapter was found");
        } else {
            Hr(factory_->EnumWarpAdapter(IID_PPV_ARGS(&adapter_)), "EnumWarpAdapter");
        }
        DXGI_ADAPTER_DESC1 adapter_desc{};
        Hr(adapter_->GetDesc1(&adapter_desc), hardware ? "hardware GetDesc1" : "WARP GetDesc1");
        if (hardware) {
            std::wprintf(L"Hardware adapter: %ls\n", adapter_desc.Description);
        } else {
            Check((adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0,
                  "DXGI adapter is not software WARP");
        }
        Hr(D3D12CreateDevice(adapter_.Get(), D3D_FEATURE_LEVEL_12_0,
                             IID_PPV_ARGS(&device_)), hardware ? "D3D12CreateDevice(hardware)" :
                                                                 "D3D12CreateDevice(WARP)");
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
           hardware ? "CreateCommandQueue(hardware)" : "CreateCommandQueue(WARP)");
        Hr(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "CreateFence");
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Check(event_ != nullptr, "CreateEvent failed");
        if (!hardware)
            std::printf("WARP software adapter; D3D12 debug layer %s\n",
                        debug_enabled_ ? "enabled" : "unavailable (validation limitation)");
    }

    ~Warp() {
        if (event_) CloseHandle(event_);
    }

    ID3D12Device* Device() const { return device_.Get(); }
    std::wstring AdapterName() const {
        DXGI_ADAPTER_DESC1 desc{};
        Hr(adapter_->GetDesc1(&desc), "Get adapter name");
        return desc.Description;
    }

    void Wait(gpu::DofGpuTiming* timing = nullptr, uint64_t frame_id = 0) {
        const uint64_t target = ++fence_value_;
        Hr(queue_->Signal(fence_.Get(), target), "Signal");
        if (timing) timing->Submitted(target, frame_id, true);
        if (fence_->GetCompletedValue() < target) {
            Hr(fence_->SetEventOnCompletion(target, event_), "SetEventOnCompletion");
            Check(WaitForSingleObject(event_, 30000) == WAIT_OBJECT_0,
                  "WARP dispatch timed out after 30 seconds");
        }
        if (timing) timing->Collect(target);
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
                             bool no_merge=false, bool collect_timing=false,
                             uint32_t timing_repetitions=1,
                             gpu::FidelityFxDofMode mode=gpu::FidelityFxDofMode::Materialized,
                             bool single_global_reset=false, bool capture_sdk_output=false,
                             gpu::FidelityFxDofReuseMode reuse=gpu::FidelityFxDofReuseMode::Disabled,
                             bool capture_reuse=false, bool capture_native_bits=false,
                             const gpu::FidelityFxDofDispatchHistory* dispatch_history=nullptr,
                             AdapterPersistentOutput* persistent_output=nullptr, bool capture_world_cache=false,
                             bool world_cache_predicate_only=false) {
        Check(timing_repetitions >= 1 && timing_repetitions <= 20,
              "timing repetitions must be between 1 and 20");
        const auto processing=gpu::FidelityFxDofProcessingSize(fixture.width,fixture.height,scale);
        Check(processing.width && processing.height &&
              dof.Matches(processing.width,processing.height,rings,no_merge,mode,single_global_reset,reuse),
              "adapter DoF processing extent mismatch");
        Check(mode==gpu::FidelityFxDofMode::Materialized || scale==2,
              "virtual adapter modes require scale 2");
        const auto input_size=gpu::FidelityFxDofInputSize(processing,mode);
        const auto output_size=gpu::FidelityFxDofOutputSize(processing,mode);
        const auto output_format=mode==gpu::FidelityFxDofMode::VirtualInputAndOutput
            ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
        gpu::DofGpuTiming gpu_timing;
        constexpr uint64_t timing_frame = 1;
        if (collect_timing) {
            Check(gpu_timing.Init(device_.Get(), queue_.Get()),
                  "D3D12 GPU timestamp query initialization failed");
            gpu_timing.BeginFrame(timing_frame, true, fence_->GetCompletedValue());
        }
        Check(rect.left >= 0 && rect.top >= 0 && rect.right <= LONG(fixture.width) &&
              rect.bottom <= LONG(fixture.height) && rect.right>rect.left &&
              rect.bottom>rect.top, "invalid adapter logical viewport");
        EnsureAdapterPipeline();
        auto scene = NewTexture(fixture.width,fixture.height,DXGI_FORMAT_R16G16B16A16_FLOAT,
                                D3D12_RESOURCE_STATE_COPY_DEST);
        auto raw_depth = NewTexture(fixture.width,fixture.height,DXGI_FORMAT_R32_FLOAT,
                                    D3D12_RESOURCE_STATE_COPY_DEST);
        auto prepared = NewTexture(input_size.width,input_size.height,DXGI_FORMAT_R16G16B16A16_FLOAT,
                                   D3D12_RESOURCE_STATE_RENDER_TARGET,
                                   D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        auto encoded = NewTexture(input_size.width,input_size.height,DXGI_FORMAT_R32_FLOAT,
                                  D3D12_RESOURCE_STATE_RENDER_TARGET,
                                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        auto filtered = persistent_output && persistent_output->resource ? persistent_output->resource :
            NewTexture(output_size.width,output_size.height,output_format,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if(persistent_output) {
            const auto desc=filtered->GetDesc();
            Check(desc.Width==output_size.width && desc.Height==output_size.height && desc.Format==output_format,
                  "persistent adapter output shape/format mismatch");
        }
        auto destination = NewTexture(fixture.width,fixture.height,DXGI_FORMAT_R16G16B16A16_FLOAT,
                                      D3D12_RESOURCE_STATE_COPY_DEST,
                                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT color_fp{}, depth_fp{},
            processing_color_fp{}, processing_depth_fp{},output_fp{};
        UINT rows = 0; UINT64 row_bytes = 0, color_bytes = 0, depth_bytes = 0,
            processing_color_bytes=0,processing_depth_bytes=0,output_bytes=0;
        const auto color_desc = scene->GetDesc(), depth_desc = raw_depth->GetDesc();
        const auto processing_color_desc=prepared->GetDesc(),processing_depth_desc=encoded->GetDesc();
        device_->GetCopyableFootprints(&color_desc,0,1,0,&color_fp,&rows,&row_bytes,&color_bytes);
        device_->GetCopyableFootprints(&depth_desc,0,1,0,&depth_fp,&rows,&row_bytes,&depth_bytes);
        device_->GetCopyableFootprints(&processing_color_desc,0,1,0,&processing_color_fp,
                                       &rows,&row_bytes,&processing_color_bytes);
        device_->GetCopyableFootprints(&processing_depth_desc,0,1,0,&processing_depth_fp,
                                       &rows,&row_bytes,&processing_depth_bytes);
        const auto output_desc=filtered->GetDesc();
        device_->GetCopyableFootprints(&output_desc,0,1,0,&output_fp,&rows,&row_bytes,&output_bytes);
        auto scene_upload = NewBuffer(color_bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        auto depth_upload = NewBuffer(depth_bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        auto destination_readback = NewBuffer(color_bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        auto prepared_readback = NewBuffer(processing_color_bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        auto encoded_readback = NewBuffer(processing_depth_bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        auto output_readback = capture_sdk_output
            ? NewBuffer(output_bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST)
            : ComPtr<ID3D12Resource>{};
        std::vector<uint16_t> rgba(fixture.color.size()*4);
        for (size_t i=0; i<fixture.color.size(); ++i) {
            const Pixel& p=fixture.color[i];
            rgba[4*i+0]=Half(p.r); rgba[4*i+1]=Half(p.g);
            rgba[4*i+2]=Half(p.b); rgba[4*i+3]=Half(p.a);
        }
        if(!fixture.color_bits.empty()) {
            Check(fixture.color_bits.size()==rgba.size(),"exact FP16 fixture color extent mismatch");
            rgba=fixture.color_bits;
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
        make_srv(2,filtered.Get(),output_format);
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
            near_scale,far_scale,fixture.bound*scale,gpu::FidelityFxDofAdapterLayout(scale,mode)};
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
        if(persistent_output && persistent_output->resource && persistent_output->state!=D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            auto barrier=Transition(filtered.Get(),persistent_output->state,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->ResourceBarrier(1,&barrier);
        }
        CopyUpload(list.Get(),scene.Get(),scene_upload.Get(),color_fp);
        CopyUpload(list.Get(),raw_depth.Get(),depth_upload.Get(),depth_fp);
        D3D12_RESOURCE_BARRIER prepare_inputs[]={
            Transition(scene.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            Transition(raw_depth.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
        list->ResourceBarrier(2,prepare_inputs);
        ID3D12DescriptorHeap* heaps[]={srv_heap.Get()};
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_VIEWPORT processing_viewport{0,0,float(input_size.width),float(input_size.height),0,1};
        D3D12_RECT full_processing{0,0,LONG(input_size.width),LONG(input_size.height)};
        list->RSSetViewports(1,&processing_viewport);
        list->RSSetScissorRects(1,&full_processing);
        list->OMSetRenderTargets(2,rtvs,FALSE,nullptr);
        D3D12_GPU_DESCRIPTOR_HANDLE present_table=srv_heap->GetGPUDescriptorHandleForHeapStart();
        present_table.ptr+=UINT64(2)*srv_increment;
        D3D12_VIEWPORT logical_viewport{0,0,float(fixture.width),float(fixture.height),0,1};
        list->RSSetViewports(1,&logical_viewport);
        list->RSSetScissorRects(1,&rect);
        list->OMSetRenderTargets(1,&rtvs[2],FALSE,nullptr);
        std::vector<SdkTimingHooks> hooks(collect_timing ? timing_repetitions : 0);
        std::vector<gpu::FidelityFxDofTiming> hook_descriptors(collect_timing ? timing_repetitions : 0);
        for (uint32_t iteration=0; iteration<timing_repetitions; ++iteration) {
            gpu::DofGpuTiming::Scope prepare_scope;
            if (collect_timing)
                prepare_scope = gpu_timing.Measure(gpu::DofGpuTiming::Stage::ScenePrepare,list.Get());
            if (iteration) {
                D3D12_RESOURCE_BARRIER reuse_inputs[]={
                    Transition(scene.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                               D3D12_RESOURCE_STATE_COPY_SOURCE),
                    Transition(destination.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,
                               D3D12_RESOURCE_STATE_COPY_DEST),
                    Transition(prepared.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                               D3D12_RESOURCE_STATE_RENDER_TARGET),
                    Transition(encoded.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                               D3D12_RESOURCE_STATE_RENDER_TARGET),
                    Transition(filtered.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                };
                list->ResourceBarrier(static_cast<UINT>(std::size(reuse_inputs)),reuse_inputs);
                list->CopyResource(destination.Get(),scene.Get());
                D3D12_RESOURCE_BARRIER restore_inputs[]={
                    Transition(scene.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                    Transition(destination.Get(),D3D12_RESOURCE_STATE_COPY_DEST,
                               D3D12_RESOURCE_STATE_RENDER_TARGET),
                };
                list->ResourceBarrier(static_cast<UINT>(std::size(restore_inputs)),restore_inputs);
            } else {
                auto scene_copy=Transition(scene.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                           D3D12_RESOURCE_STATE_COPY_SOURCE);
                list->ResourceBarrier(1,&scene_copy);
                list->CopyResource(destination.Get(),scene.Get());
                D3D12_RESOURCE_BARRIER restore_inputs[]={
                    Transition(scene.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                    Transition(destination.Get(),D3D12_RESOURCE_STATE_COPY_DEST,
                               D3D12_RESOURCE_STATE_RENDER_TARGET),
                };
                list->ResourceBarrier(static_cast<UINT>(std::size(restore_inputs)),restore_inputs);
            }
            list->SetDescriptorHeaps(1,heaps);
            list->SetGraphicsRootSignature(adapter_root_.Get());
            list->SetGraphicsRootDescriptorTable(0,srv_heap->GetGPUDescriptorHandleForHeapStart());
            list->SetGraphicsRootConstantBufferView(1,constants->GetGPUVirtualAddress());
            list->SetPipelineState(prepare_pso_.Get());
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
            prepare_scope.End();

            SdkTimingHooks* current_hooks = collect_timing ? &hooks[iteration] : nullptr;
            gpu::FidelityFxDofTiming* current_sdk_timing = nullptr;
            if (collect_timing) {
                current_hooks->timing = &gpu_timing;
                hook_descriptors[iteration] = {current_hooks,&SdkTimingHooks::Mark};
                current_sdk_timing = &hook_descriptors[iteration];
            }
            auto sdk_scope = collect_timing
                ? gpu_timing.Measure(gpu::DofGpuTiming::Stage::Sdk,list.Get())
                : gpu::DofGpuTiming::Scope{};
            if (!dof.Dispatch(list.Get(),prepared.Get(),encoded.Get(),filtered.Get(),fixture.bound*scale,
                              current_sdk_timing,dispatch_history))
                throw std::runtime_error(std::string("adapter FidelityFX Dispatch: ")+dof.Failure());
            sdk_scope.End();
            if (collect_timing) {
                Check(!current_hooks->invalid && current_hooks->next == 5 &&
                      std::all_of(current_hooks->started.begin(),current_hooks->started.end(),[](bool v){return v;}) &&
                      std::all_of(current_hooks->ended.begin(),current_hooks->ended.end(),[](bool v){return v;}),
                      "FidelityFX timing callbacks were missing, duplicated, or out of pass order");
            }
            // The SDK changes heaps/root bindings, so restore them before the production present shader.
            list->SetDescriptorHeaps(1,heaps);
            list->SetGraphicsRootSignature(adapter_root_.Get());
            list->SetGraphicsRootDescriptorTable(0,present_table);
            list->SetGraphicsRootConstantBufferView(1,constants->GetGPUVirtualAddress());
            D3D12_RESOURCE_BARRIER for_present[]={
                Transition(filtered.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                Transition(prepared.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            };
            list->ResourceBarrier(2,for_present);
            auto compose_scope = collect_timing
                ? gpu_timing.Measure(gpu::DofGpuTiming::Stage::SceneCompose,list.Get())
                : gpu::DofGpuTiming::Scope{};
            list->SetPipelineState(present_pso_.Get());
            list->RSSetViewports(1,&logical_viewport);
            list->RSSetScissorRects(1,&rect);
            list->OMSetRenderTargets(1,&rtvs[2],FALSE,nullptr);
            list->DrawInstanced(3,1,0,0);
            compose_scope.End();
        }
        gpu::FidelityFxDofReuseDiagnostics reuse_diagnostics{};
        std::array<PendingTextureCapture,4> reuse_captures;
        if(capture_reuse) {
            Check(timing_repetitions==1 && dof.GetReuseDiagnostics(reuse_diagnostics),
                  "candidate SDK reuse diagnostics unavailable or capture spans multiple dispatches");
            const gpu::FidelityFxDofDiagnosticResource resources[]={reuse_diagnostics.dirtyHierarchy,
                reuse_diagnostics.certificate,reuse_diagnostics.nearColor,reuse_diagnostics.farColor};
            for(size_t i=0;i<reuse_captures.size();++i)
                reuse_captures[i]=CaptureTexture(list.Get(),resources[i]);
        }
        gpu::FidelityFxDofWorldCacheDiagnostics world_diagnostics{};
        std::array<PendingTextureCapture,4> world_captures;
        if(capture_world_cache) {
            Check(timing_repetitions==1 && dof.GetWorldCacheDiagnostics(world_diagnostics),
                  "whole-world cache diagnostics unavailable or capture spans multiple dispatches");
            const gpu::FidelityFxDofDiagnosticResource resources[]={world_diagnostics.predicate,
                world_diagnostics.historyColor,world_diagnostics.historyDepth,world_diagnostics.previousUnsafe};
            for(size_t i=0;i<(world_cache_predicate_only?1:world_captures.size());++i)
                world_captures[i]=CaptureTexture(list.Get(),resources[i]);
        }
        D3D12_RESOURCE_BARRIER for_readback[]={
            Transition(destination.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_COPY_SOURCE),
            Transition(prepared.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_COPY_SOURCE),
            Transition(encoded.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_COPY_SOURCE),
            Transition(filtered.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_COPY_SOURCE)};
        list->ResourceBarrier(capture_sdk_output?4:3,for_readback);
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
        if(capture_sdk_output) copy_to_buffer(filtered.Get(),output_readback.Get(),output_fp);
        if (collect_timing) gpu_timing.Resolve(list.Get());
        Hr(list->Close(),"Close adapter list");
        ID3D12CommandList* lists[]={list.Get()};
        queue_->ExecuteCommandLists(1,lists);
        Wait(collect_timing ? &gpu_timing : nullptr, timing_frame);
        if(persistent_output) {
            persistent_output->resource=filtered;
            persistent_output->state=capture_sdk_output?D3D12_RESOURCE_STATE_COPY_SOURCE:D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
        AdapterResult result;
        if(capture_world_cache) {
            for(size_t i=0;i<(world_cache_predicate_only?1:world_captures.size());++i)
                result.world_cache_resources[i]=ReadCapture(world_captures[i]);
            result.world_cache_history_valid=world_diagnostics.historyValid;
            result.world_cache_generation=world_diagnostics.generation;
            result.world_cache_queue=world_diagnostics.queueId;
            if(!world_cache_predicate_only)
                result.prepared_bits=ReadColorBits(prepared_readback.Get(),processing_color_fp,input_size.width,input_size.height);
        }
        if(capture_reuse) {
            for(size_t i=0;i<reuse_captures.size();++i)
                result.reuse_resources[i]=ReadCapture(reuse_captures[i]);
            result.reuse_group_width=reuse_diagnostics.groupWidth;
            result.reuse_group_height=reuse_diagnostics.groupHeight;
            result.reuse_history_valid=reuse_diagnostics.historyValid;
            Check(reuse_diagnostics.hierarchyMipCount==result.reuse_resources[0].mips.size(),
                  "candidate hierarchy metadata disagrees with actual GPU resource");
        }
        result.destination=ReadColor(destination_readback.Get(),color_fp,fixture.width,fixture.height);
        if(capture_native_bits) result.destination_bits=ReadColorBits(destination_readback.Get(),color_fp,fixture.width,fixture.height);
        result.prepared_color=ReadColor(prepared_readback.Get(),processing_color_fp,
                                        input_size.width,input_size.height);
        result.encoded_depth=ReadFloat(encoded_readback.Get(),processing_depth_fp,
                                        input_size.width,input_size.height);
        if(capture_sdk_output)
            result.sdk_output=ReadColor(output_readback.Get(),output_fp,output_size.width,output_size.height);
        if(capture_sdk_output && output_format==DXGI_FORMAT_R16G16B16A16_FLOAT)
            result.sdk_half_bits=ReadColorBits(output_readback.Get(),output_fp,output_size.width,output_size.height);
        if (collect_timing) {
            const auto report = gpu_timing.TakeReport();
            Check(report.available && report.complete_frames == 1 && report.invalid_frames == 0 &&
                  report.frame_sample_count == 1,
                  "GPU timing report did not contain one valid completed frame");
            result.timing = report.frame_samples[0];
            result.timing_frequency = report.frequency;
            result.timing_repetitions = timing_repetitions;
            result.has_timing = true;
        }
        return result;
    }

    void CheckMixedMsaaPrepare(uint32_t samples=4, bool strict=false) {
        EnsureAdapterPipeline();
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{};
        quality.Format=DXGI_FORMAT_R32_FLOAT;
        quality.SampleCount=samples;
        Hr(device_->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                                        &quality,sizeof(quality)),
           "CheckFeatureSupport(R32F,4x)");
        if (!quality.NumQualityLevels) {
            std::printf("%ux mixed-MSAA adapter diagnostic UNSUPPORTED on this adapter\n",samples);
            Check(!strict,"requested MSAA strict contract is unsupported");
            return;
        }
        constexpr uint32_t width=16,height=16;
        auto ms_desc=TextureDesc(width,height,DXGI_FORMAT_R32_FLOAT,
                                 D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        ms_desc.SampleDesc.Count=samples;
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
        const float cb[12]={-0.1001001f,1.001001f,5.0f,strict?16.0f:8.0f,
                            strict?1.0f:0,strict?3.0f:0,strict?11.0f:float(width),
                            strict?8.0f:float(height),1.0f,0.45f,strict?16.0f:8.0f,strict?3.0f:0};
        uint8_t* mapped=nullptr;
        D3D12_RANGE no_read{0,0};
        Hr(constants->Map(0,&no_read,reinterpret_cast<void**>(&mapped)),"Map MSAA constants");
        std::memcpy(mapped,cb,sizeof(cb));
        constants->Unmap(0,nullptr);

        // Sample-frequency shader writes one near sample and three far samples.
        const std::string ms_source=std::string(
            "float4 vs(uint id:SV_VertexID):SV_Position {"
            "float2 t=float2((id<<1)&2,id&2);return float4(t*float2(2,-2)+float2(-1,1),0,1); }")+
            (strict ? "float ps(float4 p:SV_Position,uint sample:SV_SampleIndex):SV_Target {"
                "uint k=((uint)p.x+(uint)p.y)%3; if(k==2) return -1;"
                "if(sample=="+std::to_string(samples-1)+") return k==0?0.9676343:0.9943277;"
                "return sample==0?-1:0.9998888; }" :
                "float ps(uint sample:SV_SampleIndex):SV_Target {"
                "return sample==0 ? 0.9676343 : 0.9943277; }");
        auto compile_ms=[&](const char* entry,const char* profile) {
            ComPtr<ID3DBlob> blob,errors;
            HRESULT hr=D3DCompile(ms_source.data(),ms_source.size(),"MSAA fixture",nullptr,nullptr,
                                  entry,profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&blob,&errors);
            if(FAILED(hr)&&errors) std::fprintf(stderr,"MSAA fixture: %s\n",
                 static_cast<const char*>(errors->GetBufferPointer()));
            Hr(hr,"Compile MSAA initializer");
            return blob;
        };
        auto vs=compile_ms("vs","vs_5_0"),init_ps=compile_ms("ps","ps_5_0");
        const auto sample_define=std::to_string(samples);
        const D3D_SHADER_MACRO ms4[]={{"MS","1"},{"DOF_SAMPLES",sample_define.c_str()},{nullptr,nullptr}};
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
        p.SampleDesc.Count=samples;
        p.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        ComPtr<ID3D12PipelineState> init_pso,ms_prepare_pso;
        Hr(device_->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&init_pso)),
           "Create MSAA initializer PSO");
        std::vector<ComPtr<ID3D12PipelineState>> masked_init_psos;
        if(strict) for(uint32_t sample=0;sample<samples;++sample) {
            // Initialize each physical sample with an explicit pipeline mask. This avoids
            // relying on SV_SampleIndex execution when constructing the fixture itself.
            std::string source=ms_source;
            const std::string signature="float ps(float4 p:SV_Position,uint sample:SV_SampleIndex):SV_Target {";
            const size_t at=source.find(signature);
            Check(at!=std::string::npos,"MSAA initializer shader seam changed");
            source.replace(at,signature.size(),"float ps(float4 p:SV_Position):SV_Target { uint sample="+
                           std::to_string(sample)+";");
            ComPtr<ID3DBlob> code,errors;
            Hr(D3DCompile(source.data(),source.size(),"masked MSAA initializer",nullptr,nullptr,
                          "ps","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&errors),
               "Compile masked MSAA initializer");
            p.PS={code->GetBufferPointer(),code->GetBufferSize()};
            p.SampleMask=1u<<sample;
            ComPtr<ID3D12PipelineState> masked_pso;
            Hr(device_->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&masked_pso)),
               "Create masked MSAA initializer PSO");
            masked_init_psos.push_back(std::move(masked_pso));
        }
        p.SampleMask=UINT_MAX;
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
        std::vector<ComPtr<ID3D12PipelineState>> sample_psos;
        std::vector<ComPtr<ID3D12Resource>> sample_readbacks;
        if(strict) for(uint32_t sample=0;sample<samples;++sample) {
            // Inspect the actual initialized samples with the same clamped source address,
            // before applying ViewZ/min/CoC. This distinguishes fixture writes from reduction.
            std::string source=gpu::kFidelityFxDofHlsl;
            const std::string assignment="result.depth = saturate(0.5 + c / (2 * tuning.z));";
            const size_t at=source.find(assignment);
            Check(at!=std::string::npos,"MSAA diagnostic shader seam changed");
            source.replace(at,assignment.size(),"result.depth = depth.Load(p, "+std::to_string(sample)+");");
            ComPtr<ID3DBlob> code,errors;
            Hr(D3DCompile(source.data(),source.size(),"MSAA sample readback",ms4,nullptr,
                          "ffx_dof_prepare","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&errors),
               "Compile MSAA sample readback");
            p.PS={code->GetBufferPointer(),code->GetBufferSize()};
            ComPtr<ID3D12PipelineState> sample_pso;
            Hr(device_->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&sample_pso)),
               "Create MSAA sample readback PSO");
            sample_psos.push_back(std::move(sample_pso));
            sample_readbacks.push_back(NewBuffer(depth_bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST));
        }

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
        if(strict) for(const auto& init:masked_init_psos) {
            list->SetPipelineState(init.Get());
            list->DrawInstanced(3,1,0,0);
        } else list->DrawInstanced(3,1,0,0);
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
        for(uint32_t sample=0;sample<sample_psos.size();++sample) {
            list->SetPipelineState(sample_psos[sample].Get());
            list->DrawInstanced(3,1,0,0);
            auto barrier=Transition(encoded.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1,&barrier);
            D3D12_TEXTURE_COPY_LOCATION dst_sample{},src_sample{};
            src_sample.pResource=encoded.Get(); src_sample.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst_sample.pResource=sample_readbacks[sample].Get();
            dst_sample.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst_sample.PlacedFootprint=depth_fp;
            list->CopyTextureRegion(&dst_sample,0,0,0,&src_sample,nullptr);
            barrier=Transition(encoded.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->ResourceBarrier(1,&barrier);
        }
        list->SetPipelineState(ms_prepare_pso.Get());
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
        if(strict) {
            std::vector<std::vector<float>> raw_samples;
            for(const auto& readback:sample_readbacks)
                raw_samples.push_back(ReadFloat(readback.Get(),depth_fp,width,height));
            for(uint32_t y=0;y<height;++y) for(uint32_t x=0;x<width;++x) {
                const uint32_t px=std::clamp(x,1u,11u),py=std::clamp(y,3u,10u);
                const uint32_t k=(px+py)%3;
                const float raw=k==0?0.9676343f:0.9943277f;
                const float z=-0.1001001f/(raw-1.001001f),delta=z-5.0f;
                const float coc=std::clamp((delta<0?-1.0f:1.0f)*
                    std::max(std::fabs(delta)-0.75f,0.0f)/z,-1.0f,1.0f)*
                    16.0f*(delta<0?1.0f:0.45f);
                const float expected=k==2?0.5f:std::clamp(0.5f+coc/32.0f,0.0f,1.0f);
                const size_t i=size_t(y)*width+x;
                for(uint32_t sample=0;sample<samples;++sample) {
                    const float expected_raw=k==2?-1.0f:sample==samples-1?raw:sample==0?-1.0f:0.9998888f;
                    if(raw_samples[sample][i]!=expected_raw) {
                        std::fprintf(stderr,"MSAA %ux initialized sample %u pixel %u,%u -> source %u,%u: "
                                     "expected raw %.9g actual %.9g\n",samples,sample,x,y,px,py,
                                     expected_raw,raw_samples[sample][i]);
                        Check(false,"MSAA sample initializer did not write its exact raw-depth fixture");
                    }
                }
                Check(color[i].r==0.25f && color[i].g==0 && color[i].b==0.75f &&
                      color[i].a==(k==2?0.0f:1.0f),"MSAA exact RGB/validity or viewport padding mismatch");
                if(!(std::fabs(depth[i]-expected)<0.000002f)) {
                    std::fprintf(stderr,"MSAA %ux prepare pixel %u,%u -> source %u,%u kind %u: "
                                 "raw %.9g z %.9g, expected %.9g actual %.9g error %.9g\n",
                                 samples,x,y,px,py,k,raw,z,expected,depth[i],depth[i]-expected);
                    Check(false,"MSAA nearest valid sample did not encode exact near/far/invalid CoC");
                }
            }
            std::printf("%ux MSAA strict virtual prepare: nearest depth, RGB, validity and offset padding PASS\n",samples);
            return;
        }
        const Pixel center=color[size_t(8)*width+8];
        const float d=depth[size_t(8)*width+8];
        std::printf("4x mixed MSAA prepare: resolved RGB %.4f/%.4f/%.4f, validity %.4f, encoded CoC %.4f\n",
                    center.r,center.g,center.b,center.a,d);
        Check(std::fabs(center.r-0.25f)<0.005f && std::fabs(center.b-0.75f)<0.005f &&
              center.a>0.99f && d<0.48f,
              "MSAA adapter did not pair nearest raw depth with resolved mixed color");
    }

private:
    static std::vector<uint16_t> ReadColorBits(ID3D12Resource* readback,
                                              const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
                                              uint32_t width,uint32_t height) {
        Check(footprint.Footprint.Format==DXGI_FORMAT_R16G16B16A16_FLOAT,"native bit capture requires RGBA16F");
        uint8_t* mapped=nullptr;
        const D3D12_RANGE range{0,SIZE_T(footprint.Offset)+SIZE_T(footprint.Footprint.RowPitch)*(height-1)+SIZE_T(width)*8};
        Hr(readback->Map(0,&range,reinterpret_cast<void**>(&mapped)),"Map exact native FP16 bits");
        std::vector<uint16_t> bits(size_t(width)*height*4);
        for(uint32_t y=0;y<height;++y)
            std::memcpy(bits.data()+size_t(y)*width*4,mapped+footprint.Offset+
                        size_t(y)*footprint.Footprint.RowPitch,size_t(width)*8);
        const D3D12_RANGE no_write{0,0}; readback->Unmap(0,&no_write);
        return bits;
    }

    PendingTextureCapture CaptureTexture(ID3D12GraphicsCommandList* list,
                                         const gpu::FidelityFxDofDiagnosticResource& source) {
        Check(source.resource!=nullptr,"reuse diagnostic resource is null");
        const auto desc=source.resource->GetDesc();
        if(desc.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER) {
            PendingTextureCapture capture;
            Check(desc.Width>0 && desc.Width<=UINT32_MAX,"unexpected reuse diagnostic buffer size");
            capture.texture.width=uint32_t(desc.Width); capture.texture.height=1; capture.bytes=desc.Width;
            capture.readback=NewBuffer(capture.bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            const auto state=static_cast<D3D12_RESOURCE_STATES>(source.state);
            auto barrier=Transition(source.resource,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1,&barrier);
            list->CopyBufferRegion(capture.readback.Get(),0,source.resource,0,capture.bytes);
            barrier=Transition(source.resource,D3D12_RESOURCE_STATE_COPY_SOURCE,state);
            list->ResourceBarrier(1,&barrier);
            return capture;
        }
        Check(desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.DepthOrArraySize==1 &&
              desc.SampleDesc.Count==1 && desc.MipLevels>0,"unexpected reuse diagnostic resource shape");
        PendingTextureCapture capture;
        capture.texture.format=desc.Format;
        capture.texture.width=uint32_t(desc.Width); capture.texture.height=desc.Height;
        capture.footprints.resize(desc.MipLevels); capture.row_bytes.resize(desc.MipLevels);
        device_->GetCopyableFootprints(&desc,0,desc.MipLevels,0,capture.footprints.data(),nullptr,
                                       capture.row_bytes.data(),&capture.bytes);
        capture.readback=NewBuffer(capture.bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        const auto state=static_cast<D3D12_RESOURCE_STATES>(source.state);
        auto barrier=Transition(source.resource,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1,&barrier);
        for(UINT mip=0;mip<desc.MipLevels;++mip) {
            D3D12_TEXTURE_COPY_LOCATION src{},dst{};
            src.pResource=source.resource; src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex=mip;
            dst.pResource=capture.readback.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint=capture.footprints[mip];
            list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        }
        barrier=Transition(source.resource,D3D12_RESOURCE_STATE_COPY_SOURCE,state);
        list->ResourceBarrier(1,&barrier);
        return capture;
    }

    static CapturedTexture ReadCapture(PendingTextureCapture& capture) {
        uint8_t* mapped=nullptr;
        const D3D12_RANGE range{0,SIZE_T(capture.bytes)};
        Hr(capture.readback->Map(0,&range,reinterpret_cast<void**>(&mapped)),"Map reuse diagnostic GPU copy");
        if(capture.footprints.empty()) {
            capture.texture.mips.emplace_back(mapped,mapped+size_t(capture.bytes));
            const D3D12_RANGE no_write{0,0}; capture.readback->Unmap(0,&no_write);
            return std::move(capture.texture);
        }
        capture.texture.mips.resize(capture.footprints.size());
        for(size_t mip=0;mip<capture.footprints.size();++mip) {
            const auto& footprint=capture.footprints[mip];
            const size_t row_bytes=size_t(capture.row_bytes[mip]);
            auto& bytes=capture.texture.mips[mip];
            bytes.resize(row_bytes*footprint.Footprint.Height);
            for(uint32_t y=0;y<footprint.Footprint.Height;++y)
                std::memcpy(bytes.data()+size_t(y)*row_bytes,mapped+footprint.Offset+
                            size_t(y)*footprint.Footprint.RowPitch,row_bytes);
        }
        const D3D12_RANGE no_write{0,0}; capture.readback->Unmap(0,&no_write);
        return std::move(capture.texture);
    }

    static std::vector<Pixel> ReadColor(ID3D12Resource* readback,
                                        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
                                        uint32_t width, uint32_t height) {
        uint8_t* mapped=nullptr;
        const bool full_float=footprint.Footprint.Format==DXGI_FORMAT_R32G32B32A32_FLOAT;
        const D3D12_RANGE range{0,SIZE_T(footprint.Footprint.RowPitch)*(height-1)+SIZE_T(width)*(full_float?16:8)};
        Hr(readback->Map(0,&range,reinterpret_cast<void**>(&mapped)),"Map adapter color readback");
        std::vector<Pixel> result(size_t(width)*height);
        for(uint32_t y=0;y<height;++y) {
            if(full_float) {
                std::memcpy(result.data()+size_t(y)*width,mapped+y*footprint.Footprint.RowPitch,
                            size_t(width)*sizeof(Pixel));
                continue;
            }
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

bool SamePixel(const Pixel& a,const Pixel& b) {
    return a.r==b.r && a.g==b.g && a.b==b.b && a.a==b.a;
}

// Preserve the pinned SDK's A(TL)+B(TR)+D(BL)+C(BR) evaluation order.
float SdkNativeMean(float a,float b,float d,float c) {
    volatile float ab=a+b;
    volatile float abd=ab+d;
    volatile float abdc=abd+c;
    return abdc*0.25f;
}

Pixel SdkNativeMean(const Pixel& a,const Pixel& b,const Pixel& d,const Pixel& c) {
    // The readback has already rounded each individual materialized pixel to FP16.
    return {SdkNativeMean(a.r,b.r,d.r,c.r),SdkNativeMean(a.g,b.g,d.g,c.g),
            SdkNativeMean(a.b,b.b,d.b,c.b),SdkNativeMean(a.a,b.a,d.a,c.a)};
}

void CheckPreparedContract(const Fixture& f,const D3D12_RECT& rect,
                           const AdapterResult& materialized,const AdapterResult& candidate,
                           gpu::FidelityFxDofMode mode) {
    const auto processing=gpu::FidelityFxDofProcessingSize(f.width,f.height,2);
    const auto physical=gpu::FidelityFxDofInputSize(processing,mode);
    Check(candidate.prepared_color.size()==size_t(physical.width)*physical.height &&
          candidate.encoded_depth.size()==candidate.prepared_color.size(),
          "physical prepared readback extent mismatch");
    for(uint32_t y=0;y<physical.height;++y) for(uint32_t x=0;x<physical.width;++x) {
        const uint32_t virtual_x=mode==gpu::FidelityFxDofMode::Materialized?x:2*x;
        const uint32_t virtual_y=mode==gpu::FidelityFxDofMode::Materialized?y:2*y;
        const uint32_t px=std::clamp(virtual_x/2,uint32_t(rect.left),uint32_t(rect.right-1));
        const uint32_t py=std::clamp(virtual_y/2,uint32_t(rect.top),uint32_t(rect.bottom-1));
        const size_t src=At(f,px,py),out=size_t(y)*physical.width+x;
        const size_t baseline=size_t(virtual_y)*processing.width+virtual_x;
        const float raw=f.depth[src];
        float z=0;
        if(std::isfinite(raw) && raw>=0 && raw<=1) {
            const float denominator=raw-1.001001f;
            z=std::fabs(denominator)<1e-7f?1e6f:-0.1001001f/denominator;
            if(!std::isfinite(z)||z<=0) z=0;
            z=std::min(z,1e6f);
        }
        const bool valid=z>0;
        const Pixel& source=f.color[src];
        const Pixel expected{FloatFromHalf(Half(source.r)),FloatFromHalf(Half(source.g)),
                             FloatFromHalf(Half(source.b)),valid?1.0f:0.0f};
        const float delta=z-5.0f;
        const float coc=valid?std::clamp((delta<0?-1.0f:delta>0?1.0f:0.0f)*
            std::max(std::fabs(delta)-0.75f,0.0f)/z,-1.0f,1.0f)*16.0f*
            (delta<0?1.0f:0.45f):0;
        const float encoded=std::clamp(0.5f+coc/(4*f.bound),0.0f,1.0f);
        Check(SamePixel(candidate.prepared_color[out],expected),
              "prepared FP16 RGB/validity or offset/padded viewport mapping changed");
        Check(std::fabs(candidate.encoded_depth[out]-encoded)<0.000002f,
              "prepared signed near/far/invalid encoded depth changed");
        Check(SamePixel(candidate.prepared_color[out],materialized.prepared_color[baseline]) &&
              candidate.encoded_depth[out]==materialized.encoded_depth[baseline],
              "virtual prepared inputs differ from exact discrete materialized values");
    }
}

void CheckWorldOutputContract(const Fixture& f,const D3D12_RECT& rect,
                              const AdapterResult& baseline,const AdapterResult& candidate,
                              gpu::FidelityFxDofMode mode) {
    const auto processing=gpu::FidelityFxDofProcessingSize(f.width,f.height,2);
    const auto output=gpu::FidelityFxDofOutputSize(processing,mode);
    Check(candidate.sdk_output.size()==size_t(output.width)*output.height,
          "SDK output readback physical extent mismatch");
    for(uint32_t y=0;y<output.height;++y) for(uint32_t x=0;x<output.width;++x) {
        Pixel expected;
        if(mode==gpu::FidelityFxDofMode::VirtualInputAndOutput) {
            const size_t a=size_t(2*y)*processing.width+2*x,b=a+1,d=a+processing.width,c=d+1;
            expected=SdkNativeMean(baseline.sdk_output[a],baseline.sdk_output[b],
                                   baseline.sdk_output[d],baseline.sdk_output[c]);
        } else expected=baseline.sdk_output[size_t(y)*processing.width+x];
        const Pixel& actual=candidate.sdk_output[size_t(y)*output.width+x];
        if(!SamePixel(actual,expected)) {
            std::fprintf(stderr,"SDK mode %u pixel %u,%u: expected %.9g/%.9g/%.9g/%.9g, "
                         "actual %.9g/%.9g/%.9g/%.9g\n",uint32_t(mode),x,y,
                         expected.r,expected.g,expected.b,expected.a,actual.r,actual.g,actual.b,actual.a);
            Check(false,"SDK output differs from exact materialized reconstruction/FP16-rounded quad mean");
        }
    }
    CheckFiniteAndBounded(candidate.destination);
    Check(candidate.destination.size()==f.color.size(),"native destination extent changed");
    for(uint32_t y=0;y<f.height;++y) for(uint32_t x=0;x<f.width;++x) {
        const size_t i=At(f,x,y);
        const Pixel& p=candidate.destination[i];
        Check(SamePixel(p,baseline.destination[i]),"native composed output changed in storage adaptation");
        Check(p.a==FloatFromHalf(Half(f.color[i].a)),"native destination alpha changed");
        const bool outside=x<uint32_t(rect.left)||x>=uint32_t(rect.right)||
                           y<uint32_t(rect.top)||y>=uint32_t(rect.bottom);
        if(outside || f.depth[i]<0 || f.depth[i]>1 || !std::isfinite(f.depth[i])) {
            const Pixel& source=f.color[i];
            Check(p.r==FloatFromHalf(Half(source.r)) && p.g==FloatFromHalf(Half(source.g)) &&
                  p.b==FloatFromHalf(Half(source.b)),"scissor/invalid-depth preservation changed");
        }
    }
}

void CheckWorldStorageContracts(Warp& warp) {
    using Mode=gpu::FidelityFxDofMode;
    const auto processing=gpu::FidelityFxDofProcessingSize(79,65,2);
    // Independent discrete address oracle for the pinned even-boundary depth gathers.
    // A replicated virtual gather uses floor((8*tile+step+corner)/2), with
    // corner -1/0. The physical normalized gather uses 4*tile+step/2-1/0.
    for(int tile=0;tile<int(processing.width/8);++tile) {
        std::array<bool,5> seen{};
        const int first=std::max(0,4*tile-1);
        for(int step:{0,2,4,6}) for(int corner:{-1,0}) {
            const int virtual_texel=8*tile+step+corner;
            const int replicated=virtual_texel<0?(virtual_texel-1)/2:virtual_texel/2;
            const int native=4*tile+step/2+corner;
            Check(replicated==native,"even virtual gather lost the preceding native row/column");
            const int clamped=std::clamp(native,0,int(processing.width/2)-1);
            seen[size_t(clamped-first)]=true;
        }
        for(int i=0;i<(tile==0?4:5);++i)
            Check(seen[size_t(i)],"depth gather did not cover its clamped five-texel native axis");
    }
    gpu::FidelityFxDof materialized,virtual_input,fused,reset;
    Check(materialized.Create(warp.Device(),processing.width,processing.height,8,true,Mode::Materialized,false),
          materialized.Failure());
    Check(virtual_input.Create(warp.Device(),processing.width,processing.height,8,true,Mode::VirtualInput,false),
          virtual_input.Failure());
    Check(fused.Create(warp.Device(),processing.width,processing.height,8,true,Mode::VirtualInputAndOutput,false),
          fused.Failure());
    Check(reset.Create(warp.Device(),processing.width,processing.height,8,true,Mode::Materialized,true),reset.Failure());
    Check(!materialized.Matches(processing.width,processing.height,8,true,Mode::Materialized,true) &&
          !virtual_input.Matches(processing.width,processing.height,8,true,Mode::Materialized,false),
          "mode/reset configuration was omitted from context matching");
    // No internal-mip seam is exposed. These nonuniform end-to-end fixtures deliberately put
    // extrema in the previous row/column: virtual 8x8 gathers at 0/2/4/6 span native
    // [4*tile-1,4*tile+3] (5x5), including first-tile sampler clamping.
    // Reuse every context through large -> zero -> small -> large CoC to expose stale reset state.
    const uint32_t scenes[]={0,1,2,0,3,4,5};
    for(uint32_t pass=0;pass<std::size(scenes);++pass) {
        const uint32_t scene=scenes[pass];
        auto f=MakeFixture(79,65,8.0f);
        const D3D12_RECT rect=scene==4?D3D12_RECT{3,5,74,61}:D3D12_RECT{0,0,79,65};
        for(uint32_t y=0;y<f.height;++y) for(uint32_t x=0;x<f.width;++x) {
            const size_t i=At(f,x,y);
            f.color[i]={0.1f+float((x*37+y*13)%701)/997.0f,
                        0.1f+float((x*11+y*47)%701)/997.0f,
                        0.1f+float((x*53+y*7)%701)/997.0f,0.375f};
            const bool preceding=x%4==3 || y%4==3;
            const float mixed=preceding?((x+y)&1?0.5f:90.0f):
                              2.0f+float((x*19+y*23)%211)/7.0f;
            const float z=scene==1?5.0f:scene==2?5.8f:scene==3?90.0f:mixed;
            f.depth[i]=RawDepth(z);
            if(scene>=4 && (x*7+y*11)%31==0) f.depth[i]=-1.0f;
            if(scene==4 && x==17 && y==19) f.depth[i]=NAN;
            if(scene==4 && x==21 && y==23) f.depth[i]=INFINITY;
            if(scene==4 && x==25 && y==27) f.depth[i]=1.25f;
            if(scene==5 && x==0) f.depth[i]=RawDepth(0.5f);
            if(scene==5 && y==0) f.depth[i]=RawDepth(90.0f);
        }
        const auto run=[&](gpu::FidelityFxDof& context,Mode mode,bool single_reset=false) {
            return warp.RunAdapter(context,f,rect,2,8.0f,1.0f,0.45f,8,true,false,1,mode,single_reset,true);
        };
        const auto baseline=run(materialized,Mode::Materialized);
        CheckPreparedContract(f,rect,baseline,baseline,Mode::Materialized);
        const auto check=[&](gpu::FidelityFxDof& context,Mode mode,bool single_reset=false) {
            const auto candidate=run(context,mode,single_reset);
            CheckPreparedContract(f,rect,baseline,candidate,mode);
            CheckWorldOutputContract(f,rect,baseline,candidate,mode);
        };
        check(virtual_input,Mode::VirtualInput);
        check(fused,Mode::VirtualInputAndOutput);
        check(reset,Mode::Materialized,true);
        std::printf("world storage contract scene %u pass %u: exact prepared/SDK/native outputs PASS\n",scene,pass);
    }
    for(uint32_t samples:{2u,4u,8u}) warp.CheckMixedMsaaPrepare(samples,true);
}

void CheckBitwise(const void* reference,const void* actual,size_t bytes,const char* label) {
    if(bytes && std::memcmp(reference,actual,bytes)!=0) {
        const auto* a=static_cast<const uint8_t*>(reference);
        const auto* b=static_cast<const uint8_t*>(actual);
        size_t first=0;
        while(first<bytes && a[first]==b[first]) ++first;
        std::fprintf(stderr,"%s: first differing byte %zu, expected 0x%02x actual 0x%02x\n",
                     label,first,unsigned(a[first]),unsigned(b[first]));
        Check(false,label);
    }
}

void CheckBitwisePixels(const AdapterResult& reference,const AdapterResult& actual,const char* label) {
    Check(!reference.destination_bits.empty() && reference.destination_bits.size()==actual.destination_bits.size(),
          "reuse native bit capture absent or extent mismatch");
    CheckBitwise(reference.destination_bits.data(),actual.destination_bits.data(),
                 reference.destination_bits.size()*sizeof(uint16_t),label);
}

void CheckBitwiseTexture(const CapturedTexture& reference,const CapturedTexture& actual,const char* label) {
    Check(reference.width==actual.width && reference.height==actual.height && reference.format==actual.format &&
          reference.mips.size()==actual.mips.size(),"reuse diagnostic texture description mismatch");
    for(size_t mip=0;mip<reference.mips.size();++mip) {
        Check(reference.mips[mip].size()==actual.mips[mip].size(),"reuse diagnostic mip byte count mismatch");
        CheckBitwise(reference.mips[mip].data(),actual.mips[mip].data(),reference.mips[mip].size(),label);
    }
}

uint32_t ReuseWord(const CapturedTexture& texture,uint32_t mip,uint32_t x,uint32_t y) {
    Check(texture.format==DXGI_FORMAT_R32_UINT && mip<texture.mips.size(),"invalid reuse uint texture readback");
    const uint32_t width=std::max(1u,texture.width>>mip),height=std::max(1u,texture.height>>mip);
    Check(x<width && y<height && texture.mips[mip].size()==size_t(width)*height*sizeof(uint32_t),
          "reuse uint texture coordinate or packed row size mismatch");
    uint32_t value=0;
    std::memcpy(&value,texture.mips[mip].data()+(size_t(y)*width+x)*sizeof(value),sizeof(value));
    return value;
}

struct ReuseGpuStats { uint32_t dirty=0,unsafe=0,rejected=0,eligible=0; };

ReuseGpuStats CheckReuseGpuDiagnostics(const AdapterResult& result,bool incoming_history,bool all_dirty) {
    const auto& hierarchy=result.reuse_resources[0];
    const auto& certificate=result.reuse_resources[1];
    const uint32_t width=result.reuse_group_width,height=result.reuse_group_height;
    Check(result.reuse_history_valid && width && height && hierarchy.width==hierarchy.height &&
          hierarchy.width>=width && hierarchy.height>=height && certificate.width==hierarchy.width &&
          certificate.height==hierarchy.height && certificate.mips.size()==1,
          "reuse GPU diagnostic shape/history metadata mismatch");
    ReuseGpuStats stats;
    for(uint32_t y=0;y<hierarchy.height;++y) for(uint32_t x=0;x<hierarchy.width;++x) {
        const uint32_t dirty=ReuseWord(hierarchy,0,x,y),cert=ReuseWord(certificate,0,x,y);
        if(x>=width || y>=height) {
            Check(dirty==0 && cert==0,"reuse padded hierarchy/certificate cells were not zeroed");
            continue;
        }
        Check((dirty&~0x80000001u)==0,"unknown reuse dirty bits");
        Check(cert<=1u,"unknown reuse certificate bits");
        stats.dirty+=(dirty&1u)!=0; stats.unsafe+=(dirty&0x80000000u)!=0; stats.rejected+=cert!=0;
        if(all_dirty) Check((dirty&1u)!=0,"first/invalidated/changed-CB reuse frame was not all dirty");
        int lo_x=std::max(int(x)-16,0),lo_y=std::max(int(y)-16,0);
        int hi_x=std::min(int(x)+16,int(width)-1),hi_y=std::min(int(y)+16,int(height)-1);
        const uint32_t span=uint32_t(std::max(hi_x-lo_x+1,hi_y-lo_y+1));
        uint32_t level=0; while((1u<<level)<span) ++level;
        const uint32_t a_x=uint32_t(lo_x)>>level,a_y=uint32_t(lo_y)>>level;
        const uint32_t b_x=uint32_t(hi_x)>>level,b_y=uint32_t(hi_y)>>level;
        const uint32_t queried=ReuseWord(hierarchy,level,a_x,a_y)|ReuseWord(hierarchy,level,b_x,a_y)|
                               ReuseWord(hierarchy,level,a_x,b_y)|ReuseWord(hierarchy,level,b_x,b_y);
        stats.eligible+=incoming_history && (queried&1u)==0;
    }
    for(uint32_t mip=1;mip<hierarchy.mips.size();++mip) {
        const uint32_t side=std::max(1u,hierarchy.width>>mip);
        for(uint32_t y=0;y<side;++y) for(uint32_t x=0;x<side;++x) {
            const uint32_t expected=ReuseWord(hierarchy,mip-1,2*x,2*y)|ReuseWord(hierarchy,mip-1,2*x+1,2*y)|
                ReuseWord(hierarchy,mip-1,2*x,2*y+1)|ReuseWord(hierarchy,mip-1,2*x+1,2*y+1);
            Check(ReuseWord(hierarchy,mip,x,y)==expected,"actual GPU dirty mip is not the OR of its four children");
        }
    }
    return stats;
}

void CheckExactReuseContracts(Warp& warp) {
    using Mode=gpu::FidelityFxDofMode;
    using Reuse=gpu::FidelityFxDofReuseMode;
    constexpr uint32_t width=192,height=128;
    constexpr Mode mode=Mode::VirtualInput;
    const auto processing=gpu::FidelityFxDofProcessingSize(width,height,2);
    gpu::FidelityFxDof disabled,compare,guarded;
    const auto create=[&]() {
        Check(disabled.Create(warp.Device(),processing.width,processing.height,8,true,mode,false,Reuse::Disabled),disabled.Failure());
        Check(compare.Create(warp.Device(),processing.width,processing.height,8,true,mode,false,Reuse::CompareOnly),compare.Failure());
        Check(guarded.Create(warp.Device(),processing.width,processing.height,8,true,mode,false,Reuse::Guarded),guarded.Failure());
        Check(!compare.Matches(processing.width,processing.height,8,true,mode,false,Reuse::Guarded),
              "reuse mode omitted from context matching");
    };
    create();
    // Each entry starts from the same source scene, so actor movement also changes its old footprint.
    // Repeated scenes expose hits; changes cross 8px groups, mip boundaries and near/far paths.
    struct Frame { const char* name; uint32_t kind; float radius,bound; bool invalidate,reset,all_dirty; };
    const Frame frames[]={
        {"first",0,8,8,false,false,true}, {"same",0,8,8,false,false,false},
        {"one-half-bit",1,8,8,false,false,false}, {"same-half-bit",1,8,8,false,false,false},
        {"local-depth",2,8,8,false,false,false}, {"actor-left",3,8,8,false,false,false},
        {"actor-right",4,8,8,false,false,false}, {"changed-sdk-cb",0,8,12,false,false,true},
        {"same-sdk-cb",0,8,12,false,false,false}, {"explicit-invalidation",0,8,12,true,false,true},
        {"reset-first",0,8,8,false,true,true}, {"reset-same",0,8,8,false,false,false},
        {"zero-coc-prime",0,0,8,false,false,false}, {"real-after-prime",0,8,8,true,false,true},
        {"small-coc",0,2,8,false,false,false}, {"large-coc",0,16,8,false,false,false},
        {"positive-zero",5,8,8,false,false,false}, {"same-zero",5,8,8,false,false,false},
        {"negative-zero",6,8,8,false,false,false}, {"half-subnormal",7,8,8,false,false,false},
        {"nonfinite",8,8,8,false,false,false}, {"unsafe-to-safe",0,8,8,false,false,false},
        {"safe-again",0,8,8,false,false,false}};
    uint64_t eligible_total=0,dirty_total=0,unsafe_total=0;
    bool incoming_history=false;
    for(uint32_t frame=0;frame<std::size(frames);++frame) {
        const auto& step=frames[frame];
        if(step.reset) { disabled.Reset(); compare.Reset(); guarded.Reset(); create(); incoming_history=false; }
        if(step.invalidate) { disabled.InvalidateReuseHistory(); compare.InvalidateReuseHistory(); guarded.InvalidateReuseHistory(); incoming_history=false; }
        auto fixture=MakeFixture(width,height,step.bound);
        for(uint32_t y=0;y<height;++y) for(uint32_t x=0;x<width;++x) {
            const size_t i=At(fixture,x,y);
            fixture.color[i]={0.25f,0.5f,0.75f,0.375f};
            fixture.depth[i]=RawDepth(x<width/3?3.0f:x<2*width/3?5.0f:15.0f);
            const bool local=x>=63 && x<79 && y>=31 && y<47;
            if(step.kind==1 && local) fixture.color[i].r=FloatFromHalf(0x3401u);
            if(step.kind==2 && local) fixture.depth[i]=RawDepth(1.5f);
            if(step.kind==3 || step.kind==4) {
                const uint32_t actor_x=step.kind==3?55u:72u;
                if(x>=actor_x && x<actor_x+19 && y>=29 && y<83) {
                    fixture.color[i]={0.875f,0.125f,0.0625f,0.375f}; fixture.depth[i]=RawDepth(1.5f);
                }
            }
            if(step.kind>=5) fixture.color[i].r=step.kind==5?0.0f:step.kind==6?-0.0f:
                step.kind==7?FloatFromHalf(1u):((x+y)&1?INFINITY:-INFINITY);
        }
        const D3D12_RECT full{0,0,LONG(width),LONG(height)};
        if(step.kind==8) {
            fixture.color_bits.resize(fixture.color.size()*4);
            for(size_t i=0;i<fixture.color.size();++i) {
                const uint16_t nonfinite[]={0x7c00u,0xfc00u,0x7e01u};
                fixture.color_bits[4*i]=nonfinite[i%3];
                fixture.color_bits[4*i+1]=Half(fixture.color[i].g);
                fixture.color_bits[4*i+2]=Half(fixture.color[i].b);
                fixture.color_bits[4*i+3]=Half(fixture.color[i].a);
            }
        }
        const auto run=[&](gpu::FidelityFxDof& context,Reuse reuse,bool capture) {
            return warp.RunAdapter(context,fixture,full,2,step.radius,1.0f,0.45f,8,true,false,1,
                                   mode,false,false,reuse,capture,true);
        };
        std::printf("reuse contract frame %u %s\n",frame,step.name);
        const auto baseline=run(disabled,Reuse::Disabled,false);
        const auto observed=run(compare,Reuse::CompareOnly,true);
        const auto cached=run(guarded,Reuse::Guarded,true);
        CheckBitwisePixels(baseline,observed,"Disabled vs CompareOnly final output changed bits");
        CheckBitwisePixels(observed,cached,"CompareOnly vs Guarded final output changed bits");
        for(size_t resource=0;resource<observed.reuse_resources.size();++resource)
            CheckBitwiseTexture(observed.reuse_resources[resource],cached.reuse_resources[resource],
                                "CompareOnly vs Guarded dirty/certificate/near/far texture changed bits");
        // A CB change invalidates history inside the SDK even though the public getter records
        // successful outgoing history. All-dirty is checked from the actual GPU base map.
        const auto stats=CheckReuseGpuDiagnostics(cached,incoming_history && !step.all_dirty,step.all_dirty);
        CheckReuseGpuDiagnostics(observed,incoming_history && !step.all_dirty,step.all_dirty);
        if(frame==1 || frame==3 || frame==8 || frame==11 || frame==22)
            Check(stats.eligible>0,"identical safe frame exposed no GPU-verified reuse eligibility");
        if(frame==2 || frame==4 || frame==5 || frame==6)
            Check(stats.dirty>0,"local changed fixture produced no actual GPU dirty groups");
        if(step.kind==8) Check(stats.unsafe>0,"nonfinite actual stored history was not marked unsafe by the GPU");
        eligible_total+=stats.eligible; dirty_total+=stats.dirty; unsafe_total+=stats.unsafe;
        std::printf("reuse GPU readback: dirty %u unsafe %u certificate-rejected %u eligible %u of %u groups\n",
                    stats.dirty,stats.unsafe,stats.rejected,stats.eligible,cached.reuse_group_width*cached.reuse_group_height);
        incoming_history=true;
    }
    Check(eligible_total>0 && dirty_total>0 && unsafe_total>0,"reuse fixtures did not exercise eligibility and fallback");
    std::puts("PASS: reuse exact final/near/far bits and actual GPU dirty hierarchy/certificate contracts");
}

uint64_t WorldCachePredicate(const AdapterResult& result) {
    const auto& resource=result.world_cache_resources[0];
    Check(resource.format==DXGI_FORMAT_UNKNOWN && resource.width==8 && resource.height==1 &&
          resource.mips.size()==1 && resource.mips[0].size()==8,"world cache predicate is not the actual eight-byte buffer");
    uint32_t words[2]{};
    std::memcpy(words,resource.mips[0].data(),sizeof(words));
    Check(words[1]==0 && words[0]<=1,"world cache predicate high word or dirty polarity is invalid");
    return uint64_t(words[0])|(uint64_t(words[1])<<32);
}

void CheckWorldCacheHistories(const AdapterResult& result,bool unsafe_input) {
    const auto& color=result.world_cache_resources[1];
    const auto& depth=result.world_cache_resources[2];
    const auto& unsafe=result.world_cache_resources[3];
    Check(color.format==DXGI_FORMAT_R16G16B16A16_FLOAT && color.mips.size()==1 &&
          depth.format==DXGI_FORMAT_R32_UINT && depth.mips.size()==1 &&
          color.width==depth.width && color.height==depth.height &&
          color.mips[0].size()==result.prepared_bits.size()*sizeof(uint16_t) &&
          depth.mips[0].size()==result.encoded_depth.size()*sizeof(float),
          "world cache native history description mismatch");
    if(!unsafe_input)
        CheckBitwise(result.prepared_bits.data(),color.mips[0].data(),color.mips[0].size(),
                     "actual GPU world color history differs from exact prepared RGBA16F bits");
    CheckBitwise(result.encoded_depth.data(),depth.mips[0].data(),depth.mips[0].size(),
                 "actual GPU world depth history differs from exact prepared R32F bits");
    Check(unsafe.format==DXGI_FORMAT_R32_UINT && unsafe.mips.size()==1 &&
          unsafe.width==(color.width+7)/8 && unsafe.height==(color.height+7)/8,
          "world cache unsafe-group resource shape mismatch");
    uint32_t unsafe_count=0;
    for(uint32_t y=0;y<unsafe.height;++y) for(uint32_t x=0;x<unsafe.width;++x) {
        const uint32_t flag=ReuseWord(unsafe,0,x,y);
        Check(flag<=1,"unknown world cache unsafe-group bit"); unsafe_count+=flag;
    }
    if(unsafe_input) Check(unsafe_count>0,"unsafe actual native input did not set any GPU unsafe-group flag");
    else Check(unsafe_count==0,"safe actual native input unexpectedly set GPU unsafe-group flags");
}

void CheckWorldCacheContracts(Warp& warp) {
    using Mode=gpu::FidelityFxDofMode;
    using Reuse=gpu::FidelityFxDofReuseMode;
    constexpr uint32_t width=192,height=128;
    constexpr Mode mode=Mode::VirtualInputAndOutput;
    const auto processing=gpu::FidelityFxDofProcessingSize(width,height,2);
    gpu::FidelityFxDof disabled,cached;
    AdapterPersistentOutput retained;
    const auto create=[&]() {
        Check(disabled.Create(warp.Device(),processing.width,processing.height,8,true,mode,true,Reuse::Disabled),disabled.Failure());
        Check(cached.Create(warp.Device(),processing.width,processing.height,8,true,mode,true,Reuse::WholeWorld),cached.Failure());
    };
    create();
    struct Frame {
        const char* name; uint32_t kind; bool hit;
        uint64_t generation,queue; bool preserved,replace,invalidate,reset,missing; float bound;
    };
    const Frame frames[]={
        {"first-warm",0,false,1,7,true,false,false,false,false,8},
        {"same-hit",0,true,1,7,true,false,false,false,false,8},
        {"color-bit-miss",1,false,1,7,true,false,false,false,false,8},
        {"same-color-hit",1,true,1,7,true,false,false,false,false,8},
        {"depth-bit-miss",2,false,1,7,true,false,false,false,false,8},
        {"same-depth-hit",2,true,1,7,true,false,false,false,false,8},
        {"preservation-false",2,false,1,7,false,false,false,false,false,8},
        {"preservation-restored",2,true,1,7,true,false,false,false,false,8},
        {"generation-change",2,false,2,7,true,false,false,false,false,8},
        {"generation-hit",2,true,2,7,true,false,false,false,false,8},
        {"output-identity-change",2,false,2,7,true,true,false,false,false,8},
        {"new-output-hit",2,true,2,7,true,false,false,false,false,8},
        {"queue-identity-change",2,false,2,9,true,false,false,false,false,8},
        {"new-queue-hit",2,true,2,9,true,false,false,false,false,8},
        {"missing-queue",2,false,2,0,true,false,false,false,false,8},
        {"missing-generation",2,false,0,9,true,false,false,false,false,8},
        {"missing-metadata",2,false,2,9,true,false,false,false,true,8},
        {"metadata-restored",2,false,2,9,true,false,false,false,false,8},
        {"metadata-hit",2,true,2,9,true,false,false,false,false,8},
        {"effective-cb-change",2,false,2,9,true,false,false,false,false,12},
        {"effective-cb-hit",2,true,2,9,true,false,false,false,false,12},
        {"explicit-invalidate",2,false,2,9,true,false,true,false,false,12},
        {"post-invalidate-hit",2,true,2,9,true,false,false,false,false,12},
        {"positive-zero",3,false,2,9,true,false,false,false,false,12},
        {"positive-zero-hit",3,true,2,9,true,false,false,false,false,12},
        {"negative-zero",4,false,2,9,true,false,false,false,false,12},
        {"same-negative-zero",4,false,2,9,true,false,false,false,false,12},
        {"half-subnormal",5,false,2,9,true,false,false,false,false,12},
        {"nonfinite",6,false,2,9,true,false,false,false,false,12},
        {"same-nonfinite",6,false,2,9,true,false,false,false,false,12},
        {"unsafe-to-safe",0,false,2,9,true,false,false,false,false,12},
        {"safe-again-hit",0,true,2,9,true,false,false,false,false,12},
        {"reset-first",0,false,3,9,true,false,false,true,false,8},
        {"reset-hit",0,true,3,9,true,false,false,false,false,8}};
    uint32_t hits=0,misses=0;
    std::vector<Pixel> previous_sdk;
    std::vector<float> previous_depth;
    std::vector<ComPtr<ID3D12Resource>> retired_outputs;
    for(uint32_t index=0;index<std::size(frames);++index) {
        const auto& step=frames[index];
        if(step.reset) { disabled.Reset(); cached.Reset(); create(); }
        if(step.invalidate) cached.InvalidateReuseHistory();
        if(step.replace) {
            // Keep the old object alive so the allocator cannot reuse its pointer identity.
            retired_outputs.push_back(retained.resource); retained={};
        }
        auto fixture=MakeFixture(width,height,step.bound);
        for(uint32_t y=0;y<height;++y) for(uint32_t x=0;x<width;++x) {
            const size_t i=At(fixture,x,y);
            fixture.color[i]={0.25f,0.5f,0.75f,0.375f};
            fixture.depth[i]=RawDepth(x<width/3?3.0f:x<2*width/3?5.0f:15.0f);
            if((step.kind==1 || step.kind==2) && x>=63 && x<79 && y>=31 && y<47)
                fixture.color[i].r=FloatFromHalf(0x3401u);
            if(step.kind==2 && x>=63 && x<79 && y>=31 && y<47) {
                const uint32_t bits=std::bit_cast<uint32_t>(fixture.depth[i]);
                fixture.depth[i]=std::bit_cast<float>(bits+1u);
            }
            if(step.kind>=3) fixture.color[i].r=step.kind==3?0.0f:step.kind==4?-0.0f:
                step.kind==5?FloatFromHalf(1u):INFINITY;
        }
        if(step.kind==6) {
            fixture.color_bits.resize(size_t(width)*height*4);
            for(size_t i=0;i<fixture.color.size();++i) {
                const uint16_t nonfinite[]={0x7c00u,0xfc00u,0x7e01u};
                fixture.color_bits[4*i]=nonfinite[i%3]; fixture.color_bits[4*i+1]=Half(0.5f);
                fixture.color_bits[4*i+2]=Half(0.75f); fixture.color_bits[4*i+3]=Half(0.375f);
            }
        }
        const gpu::FidelityFxDofDispatchHistory history{step.queue,step.generation,step.preserved};
        const D3D12_RECT full{0,0,LONG(width),LONG(height)};
        std::printf("whole-world contract %u %s expected %s\n",index,step.name,step.hit?"HIT":"MISS");
        const auto baseline=warp.RunAdapter(disabled,fixture,full,2,8,1,0.45f,8,true,false,1,
            mode,true,true,Reuse::Disabled,false,true);
        const auto result=warp.RunAdapter(cached,fixture,full,2,8,1,0.45f,8,true,false,1,
            mode,true,true,Reuse::WholeWorld,false,true,step.missing?nullptr:&history,&retained,true);
        CheckBitwisePixels(baseline,result,"whole-world final native RGBA16F bits differ from Disabled");
        Check(baseline.sdk_output.size()==result.sdk_output.size(),"whole-world fused output extent mismatch");
        CheckBitwise(baseline.sdk_output.data(),result.sdk_output.data(),result.sdk_output.size()*sizeof(Pixel),
                     "whole-world fused RGBA32F bits differ from original five-pass output");
        const uint64_t predicate=WorldCachePredicate(result);
        Check((predicate==0)==step.hit,"actual 64-bit GPU predicate has wrong hit/miss polarity");
        Check(result.world_cache_history_valid && result.world_cache_generation==(step.missing?0:step.generation) &&
              result.world_cache_queue==(step.missing?0:step.queue),"whole-world recorded history metadata mismatch");
        CheckWorldCacheHistories(result,step.kind>=4);
        if(step.hit) {
            Check(previous_sdk.size()==result.sdk_output.size(),"world hit lacks previous persistent output");
            CheckBitwise(previous_sdk.data(),result.sdk_output.data(),previous_sdk.size()*sizeof(Pixel),
                         "zero-predicate hit changed retained fused output bits");
            ++hits;
        } else ++misses;
        if(index==2) {
            Check(previous_sdk.size()==result.sdk_output.size() &&
                  std::memcmp(previous_sdk.data(),result.sdk_output.data(),previous_sdk.size()*sizeof(Pixel))!=0,
                  "changed-input miss did not update the persistent output");
        }
        if(index==4)
            Check(previous_depth.size()==result.encoded_depth.size() &&
                  std::memcmp(previous_depth.data(),result.encoded_depth.data(),previous_depth.size()*sizeof(float))!=0,
                  "one-bit raw depth fixture did not change actual prepared native depth bits");
        previous_sdk=result.sdk_output;
        previous_depth=result.encoded_depth;
        std::printf("whole-world GPU predicate low=%llu high=0, native histories verified\n",
                    static_cast<unsigned long long>(predicate));
    }
    // The public wrapper has no partial-recording fault-injection seam. A rejected
    // null-argument call never records GPU work and cannot test predicate cleanup.
    // Cover fresh all-sharp recovery here; root separately validates callback failure paths.
    const gpu::FidelityFxDofDispatchHistory history{9,3,true};
    cached.Reset();
    Check(cached.Create(warp.Device(),processing.width,processing.height,8,true,mode,true,Reuse::WholeWorld),cached.Failure());
    auto recovery=MakeFixture(width,height,8);
    std::fill(recovery.color.begin(),recovery.color.end(),Pixel{0.25f,0.5f,0.75f,0.375f});
    std::fill(recovery.depth.begin(),recovery.depth.end(),RawDepth(5));
    const D3D12_RECT full{0,0,LONG(width),LONG(height)};
    const auto original=warp.RunAdapter(disabled,recovery,full,2,8,1,0.45f,8,true,false,1,
        mode,true,true,Reuse::Disabled,false,true);
    const auto recovered=warp.RunAdapter(cached,recovery,full,2,8,1,0.45f,8,true,false,1,
        mode,true,true,Reuse::WholeWorld,false,true,&history,&retained,true);
    Check(WorldCachePredicate(recovered)==1,"first all-sharp dispatch after reset was not a miss");
    CheckBitwisePixels(original,recovered,"world-cache reset recovery native bits differ");
    CheckBitwise(original.sdk_output.data(),recovered.sdk_output.data(),original.sdk_output.size()*sizeof(Pixel),
                 "world-cache reset recovery fused bits differ");
    Check(hits>0 && misses>0,"world-cache contract did not exercise both predicate polarities");
    std::printf("PASS: exact whole-world cache GPU predicate/output/history contracts (%u hits, %u misses)\n",hits,misses+1);
}

void CheckCacheCreationFallback(Warp& warp,gpu::FidelityFxDof& supported_cache) {
    using Mode=gpu::FidelityFxDofMode;
    using Reuse=gpu::FidelityFxDofReuseMode;
    constexpr uint32_t width=192,height=128;
    const auto processing=gpu::FidelityFxDofProcessingSize(width,height,2);
    Check(supported_cache.Ready() && supported_cache.EffectiveReuseMode()==Reuse::WholeWorldMulti &&
          supported_cache.Matches(processing.width,processing.height,8,true,Mode::VirtualInputAndOutput,true,Reuse::WholeWorldMulti),
          "fallback Swap reference is not an effective supported multi-entry cache");
    for(const auto mode:{Mode::Materialized,Mode::VirtualInput}) {
        // Both layouts are supported by Disabled and forbidden by the cache SDK.
        // This forces an actual layout-creation failure without simulating OOM.
        gpu::FidelityFxDof original,fallback;
        Check(original.Create(warp.Device(),processing.width,processing.height,8,true,mode,true,Reuse::Disabled),original.Failure());
        const auto create_fallback=[&]() {
            Check(fallback.Create(warp.Device(),processing.width,processing.height,8,true,mode,true,Reuse::WholeWorldMulti),fallback.Failure());
            Check(fallback.Ready() && fallback.EffectiveReuseMode()==Reuse::Disabled,
                  "unsupported cache layout failed to create the exact Disabled fallback");
            Check(fallback.Matches(processing.width,processing.height,8,true,mode,true,Reuse::WholeWorldMulti) &&
                  !fallback.Matches(processing.width,processing.height,8,true,mode,true,Reuse::Disabled),
                  "fallback context lost the requested cache configuration");
            gpu::FidelityFxDofWorldCacheDiagnostics unavailable{};
            Check(!fallback.GetWorldCacheDiagnostics(unavailable),"Disabled fallback exposed active cache diagnostics");
        };
        create_fallback();
        const auto compare_frame=[&](gpu::FidelityFxDof& candidate,uint32_t frame) {
            auto fixture=MakeFixture(width,height,8);
            for(uint32_t y=0;y<height;++y) for(uint32_t x=0;x<width;++x) {
                const size_t i=At(fixture,x,y);
                const float red=((x/5+y/7)&1)?0.75f:0.25f;
                fixture.color[i]={red,0.5f,1.0f-red,0.375f};
                fixture.depth[i]=RawDepth(x<width/3?3.0f:x<2*width/3?5.0f:15.0f);
                if(frame==1 && x>=63 && x<91 && y>=29 && y<83) {
                    fixture.color[i]={0.875f,0.125f,0.0625f,0.625f}; fixture.depth[i]=RawDepth(1.5f);
                }
                if(frame==2) { fixture.color[i]={0.625f,0.25f,0.125f,0.5f}; fixture.depth[i]=RawDepth(5); }
            }
            const D3D12_RECT full{0,0,LONG(width),LONG(height)};
            const auto baseline=warp.RunAdapter(original,fixture,full,2,8,1,0.45f,8,true,false,1,
                mode,true,true,Reuse::Disabled,false,true);
            const auto result=warp.RunAdapter(candidate,fixture,full,2,8,1,0.45f,8,true,false,1,
                mode,true,true,Reuse::WholeWorldMulti,false,true);
            CheckBitwisePixels(baseline,result,"cache-creation fallback native RGBA16F bits differ from Disabled");
            Check(!baseline.sdk_half_bits.empty() && baseline.sdk_half_bits.size()==result.sdk_half_bits.size(),
                  "fallback full SDK RGBA16F bit capture is absent or has the wrong extent");
            CheckBitwise(baseline.sdk_half_bits.data(),result.sdk_half_bits.data(),
                         baseline.sdk_half_bits.size()*sizeof(uint16_t),
                         "cache-creation fallback full SDK RGBA bits differ from Disabled");
            CheckFiniteAndBounded(result.destination);
        };
        compare_frame(fallback,0);
        // Matches is the public Create fast-path contract. No Disabled-context
        // identity/creation counter exists, so output parity alone is not an identity proof.
        create_fallback();
        create_fallback();
        compare_frame(fallback,1);
        gpu::FidelityFxDofWorldCacheDiagnostics before_swap{};
        Check(supported_cache.GetWorldCacheDiagnostics(before_swap),"supported cache lost diagnostics before Swap");
        fallback.Swap(supported_cache);
        Check(fallback.EffectiveReuseMode()==Reuse::WholeWorldMulti &&
              fallback.Matches(processing.width,processing.height,8,true,Mode::VirtualInputAndOutput,true,Reuse::WholeWorldMulti),
              "Swap did not transfer effective cache and requested storage configuration");
        Check(supported_cache.EffectiveReuseMode()==Reuse::Disabled &&
              supported_cache.Matches(processing.width,processing.height,8,true,mode,true,Reuse::WholeWorldMulti),
              "Swap did not transfer requested cache plus effective Disabled fallback");
        gpu::FidelityFxDofWorldCacheDiagnostics after_swap{};
        Check(fallback.GetWorldCacheDiagnostics(after_swap) &&
              before_swap.predicate.resource==after_swap.predicate.resource,
              "Swap recreated or lost the supported cache resource identity");
        compare_frame(supported_cache,2);
        fallback.Swap(supported_cache);
        Check(fallback.EffectiveReuseMode()==Reuse::Disabled &&
              supported_cache.EffectiveReuseMode()==Reuse::WholeWorldMulti,
              "reverse Swap lost requested/effective cache modes");
        fallback.Reset();
        Check(!fallback.Ready() && fallback.EffectiveReuseMode()==Reuse::Disabled &&
              !fallback.Matches(processing.width,processing.height,8,true,mode,true,Reuse::WholeWorldMulti),
              "Reset retained an active requested/effective fallback context");
        create_fallback();
        compare_frame(fallback,2);
        std::printf("cache creation fallback layout %u: requested Multi/effective Disabled, repeated Create, Swap, Reset and exact bits PASS\n",
                    uint32_t(mode));
    }
}

void CheckWorldMultiCacheContracts(Warp& warp) {
    using Mode=gpu::FidelityFxDofMode;
    using Reuse=gpu::FidelityFxDofReuseMode;
    enum class Input { A,B,C,D,ColorBit,DepthBit,Nonfinite,PositiveZero,NegativeZero,Subnormal };
    constexpr uint32_t width=192,height=128;
    constexpr Mode mode=Mode::VirtualInputAndOutput;
    const auto processing=gpu::FidelityFxDofProcessingSize(width,height,2);
    gpu::FidelityFxDof disabled,cached;
    AdapterPersistentOutput retained;
    const auto create=[&]() {
        Check(disabled.Create(warp.Device(),processing.width,processing.height,8,true,mode,true,Reuse::Disabled),disabled.Failure());
        Check(cached.Create(warp.Device(),processing.width,processing.height,8,true,mode,true,Reuse::WholeWorldMulti),cached.Failure());
        Check(!cached.Matches(processing.width,processing.height,8,true,mode,true,Reuse::WholeWorld),
              "multi-entry cache mode omitted from context matching");
    };
    create();
    struct Frame {
        const char* name; Input input; bool hit;
        uint64_t generation,queue; bool preserved,replace,invalidate,reset,missing; float bound;
    };
    const Frame frames[]={
        {"A-warm-slot0",Input::A,false,1,7,true,false,false,false,false,8},
        {"B-warm-slot1",Input::B,false,1,7,true,false,false,false,false,8},
        {"C-warm-slot2",Input::C,false,1,7,true,false,false,false,false,8},
        {"A-restore",Input::A,true,1,7,true,false,false,false,false,8},
        {"B-restore",Input::B,true,1,7,true,false,false,false,false,8},
        {"C-restore",Input::C,true,1,7,true,false,false,false,false,8},
        {"unsafe-no-admission",Input::Nonfinite,false,1,7,true,false,false,false,false,8},
        {"A-survives-unsafe",Input::A,true,1,7,true,false,false,false,false,8},
        {"B-survives-unsafe",Input::B,true,1,7,true,false,false,false,false,8},
        {"C-survives-unsafe",Input::C,true,1,7,true,false,false,false,false,8},
        {"D-FIFO-evicts-A",Input::D,false,1,7,true,false,false,false,false,8},
        {"B-survives-D",Input::B,true,1,7,true,false,false,false,false,8},
        {"C-survives-D",Input::C,true,1,7,true,false,false,false,false,8},
        {"A-FIFO-miss-evicts-B",Input::A,false,1,7,true,false,false,false,false,8},
        {"D-survives-A",Input::D,true,1,7,true,false,false,false,false,8},
        {"B-FIFO-miss-evicts-C",Input::B,false,1,7,true,false,false,false,false,8},
        {"A-before-one-bit",Input::A,true,1,7,true,false,false,false,false,8},
        {"A-one-color-half-bit",Input::ColorBit,false,1,7,true,false,false,false,false,8},
        {"same-color-bit-hit",Input::ColorBit,true,1,7,true,false,false,false,false,8},
        {"A-one-depth-raw-bit",Input::DepthBit,false,1,7,true,false,false,false,false,8},
        {"same-depth-bit-hit",Input::DepthBit,true,1,7,true,false,false,false,false,8},
        {"effective-CB-invalidates-all",Input::DepthBit,false,1,7,true,false,false,false,false,12},
        {"effective-CB-hit",Input::DepthBit,true,1,7,true,false,false,false,false,12},
        {"gap-explicit-invalidation",Input::DepthBit,false,1,7,true,false,true,false,false,12},
        {"gap-rewarm-hit",Input::DepthBit,true,1,7,true,false,false,false,false,12},
        {"generation-invalidates-all",Input::DepthBit,false,2,7,true,false,false,false,false,12},
        {"generation-hit",Input::DepthBit,true,2,7,true,false,false,false,false,12},
        {"output-replacement",Input::DepthBit,false,2,7,true,true,false,false,false,12},
        {"output-replacement-hit",Input::DepthBit,true,2,7,true,false,false,false,false,12},
        {"queue-change",Input::DepthBit,false,2,9,true,false,false,false,false,12},
        {"queue-change-hit",Input::DepthBit,true,2,9,true,false,false,false,false,12},
        {"preservation-false",Input::DepthBit,false,2,9,false,false,false,false,false,12},
        {"preservation-restored-hit",Input::DepthBit,true,2,9,true,false,false,false,false,12},
        {"positive-zero-admitted",Input::PositiveZero,false,2,9,true,false,false,false,false,12},
        {"positive-zero-hit",Input::PositiveZero,true,2,9,true,false,false,false,false,12},
        {"negative-zero-not-admitted",Input::NegativeZero,false,2,9,true,false,false,false,false,12},
        {"same-negative-zero-miss",Input::NegativeZero,false,2,9,true,false,false,false,false,12},
        {"safe-zero-survives",Input::PositiveZero,true,2,9,true,false,false,false,false,12},
        {"subnormal-not-admitted",Input::Subnormal,false,2,9,true,false,false,false,false,12},
        {"safe-zero-survives-subnormal",Input::PositiveZero,true,2,9,true,false,false,false,false,12},
        {"nonfinite-not-admitted",Input::Nonfinite,false,2,9,true,false,false,false,false,12},
        {"safe-zero-survives-nonfinite",Input::PositiveZero,true,2,9,true,false,false,false,false,12},
        {"recreate-first",Input::A,false,3,9,true,false,false,true,false,8},
        {"recreate-hit",Input::A,true,3,9,true,false,false,false,false,8},
        {"missing-metadata",Input::A,false,3,9,true,false,false,false,true,8},
        {"metadata-restored-miss",Input::A,false,3,9,true,false,false,false,false,8},
        {"metadata-restored-hit",Input::A,true,3,9,true,false,false,false,false,8},
        {"zero-queue",Input::A,false,3,0,true,false,false,false,false,8},
        {"queue-restored-miss",Input::A,false,3,9,true,false,false,false,false,8},
        {"queue-restored-hit",Input::A,true,3,9,true,false,false,false,false,8},
        {"zero-generation",Input::A,false,0,9,true,false,false,false,false,8},
        {"generation-restored-miss",Input::A,false,3,9,true,false,false,false,false,8},
        {"generation-restored-hit",Input::A,true,3,9,true,false,false,false,false,8}};
    std::vector<ComPtr<ID3D12Resource>> retired_outputs;
    uint32_t hits=0,misses=0,restores=0;
    std::vector<Pixel> previous_sdk;
    Input previous_input=Input::A;
    for(uint32_t index=0;index<std::size(frames);++index) {
        const auto& step=frames[index];
        if(step.reset) { disabled.Reset(); cached.Reset(); create(); }
        if(step.invalidate) cached.InvalidateReuseHistory();
        if(step.replace) { retired_outputs.push_back(retained.resource); retained={}; }
        auto fixture=MakeFixture(width,height,step.bound);
        const Pixel colors[]={{0.25f,0.5f,0.75f,0.375f},{0.75f,0.25f,0.125f,0.375f},
                              {0.125f,0.75f,0.5f,0.375f},{0.875f,0.125f,0.0625f,0.375f}};
        const uint32_t color_index=uint32_t(step.input)<=uint32_t(Input::D)?uint32_t(step.input):0;
        for(uint32_t y=0;y<height;++y) for(uint32_t x=0;x<width;++x) {
            const size_t i=At(fixture,x,y);
            fixture.color[i]=colors[color_index];
            fixture.depth[i]=RawDepth(x<width/3?3.0f:x<2*width/3?5.0f:15.0f);
            const bool local=x>=63 && x<79 && y>=31 && y<47;
            if(step.input==Input::ColorBit && local) fixture.color[i].r=FloatFromHalf(0x3401u);
            if(step.input==Input::DepthBit && local)
                fixture.depth[i]=std::bit_cast<float>(std::bit_cast<uint32_t>(fixture.depth[i])+1u);
            if(step.input==Input::PositiveZero) fixture.color[i].r=0.0f;
            if(step.input==Input::NegativeZero) fixture.color[i].r=-0.0f;
            if(step.input==Input::Subnormal) fixture.color[i].r=FloatFromHalf(1u);
        }
        if(step.input==Input::Nonfinite) {
            fixture.color_bits.resize(size_t(width)*height*4);
            for(size_t i=0;i<fixture.color.size();++i) {
                const uint16_t nonfinite[]={0x7c00u,0xfc00u,0x7e01u};
                fixture.color_bits[4*i]=nonfinite[i%3]; fixture.color_bits[4*i+1]=Half(0.5f);
                fixture.color_bits[4*i+2]=Half(0.75f); fixture.color_bits[4*i+3]=Half(0.375f);
            }
        }
        const gpu::FidelityFxDofDispatchHistory history{step.queue,step.generation,step.preserved};
        const D3D12_RECT full{0,0,LONG(width),LONG(height)};
        std::printf("world multi contract %u %s expected %s\n",index,step.name,step.hit?"HIT":"MISS");
        const auto baseline=warp.RunAdapter(disabled,fixture,full,2,8,1,0.45f,8,true,false,1,
            mode,true,true,Reuse::Disabled,false,true);
        const auto result=warp.RunAdapter(cached,fixture,full,2,8,1,0.45f,8,true,false,1,
            mode,true,true,Reuse::WholeWorldMulti,false,true,step.missing?nullptr:&history,&retained,true,true);
        CheckBitwisePixels(baseline,result,"three-entry cache native RGBA16F bits differ from Disabled");
        Check(baseline.sdk_output.size()==result.sdk_output.size(),"three-entry fused output extent mismatch");
        CheckBitwise(baseline.sdk_output.data(),result.sdk_output.data(),result.sdk_output.size()*sizeof(Pixel),
                     "three-entry cache fused RGBA32F bits differ from Disabled");
        const uint64_t predicate=WorldCachePredicate(result);
        if((predicate==0)!=step.hit) {
            std::fprintf(stderr,"three-entry cache frame %u %s: actual dirty64=%llu, expected %u\n",
                         index,step.name,static_cast<unsigned long long>(predicate),step.hit?0u:1u);
            Check(false,"three-entry actual GPU miss predicate violates warmup/FIFO/invalidation contract");
        }
        if(step.hit) {
            Check(result.world_cache_history_valid,"three-entry GPU hit has invalid recorded history");
            ++hits;
            if(uint32_t(step.input)<=uint32_t(Input::D) && uint32_t(previous_input)<=uint32_t(Input::D) &&
               step.input!=previous_input) {
                Check(previous_sdk.size()==result.sdk_output.size() &&
                      std::memcmp(previous_sdk.data(),result.sdk_output.data(),previous_sdk.size()*sizeof(Pixel))!=0,
                      "nonconsecutive cache hit failed to restore a different cached world output");
                ++restores;
            }
        } else ++misses;
        previous_sdk=result.sdk_output; previous_input=step.input;
        std::printf("three-entry GPU predicate low=%llu high=0, full fused/native bits exact\n",
                    static_cast<unsigned long long>(predicate));
    }
    Check(hits>=3 && misses>=4 && restores>=3,"three-entry tests did not exercise warm restore and eviction");
    CheckCacheCreationFallback(warp,cached);
    std::printf("PASS: exact three-entry world cache GPU contracts (%u hits, %u misses, %u nonconsecutive restores)\n",
                hits,misses,restores);
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

Fixture MakeTimingFixture(uint32_t width, uint32_t height, bool mixed) {
    Fixture f = MakeFixture(width, height, 32.0f);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const float fx = float(x) / float(std::max(1u, width - 1));
            const float fy = float(y) / float(std::max(1u, height - 1));
            const float checker = ((x / 7 + y / 5) & 1) ? 0.12f : 0.0f;
            const float edge = ((x / std::max(1u, width / 12)) & 1) ? 0.23f : 0.0f;
            f.color[At(f,x,y)] = {std::clamp(0.12f + 0.68f * fx + checker, 0.0f, 1.0f),
                                  std::clamp(0.10f + 0.70f * fy + edge, 0.0f, 1.0f),
                                  std::clamp(0.18f + 0.44f * (1.0f - fx) + checker, 0.0f, 1.0f),
                                  1.0f};
            if (!mixed) {
                f.depth[At(f,x,y)] = fx < 0.30f ? 0.27f : fx < 0.70f ? 0.50f : 0.74f;
            } else {
                const uint32_t region = ((x / std::max(1u, width / 8)) +
                                         2 * (y / std::max(1u, height / 6))) % 3;
                const float plane = region == 0 ? 0.24f : region == 1 ? 0.50f : 0.77f;
                const float texture = (((x * 13u + y * 7u) & 15u) < 8u) ? -0.025f : 0.025f;
                f.depth[At(f,x,y)] = std::clamp(plane + texture, 0.18f, 0.84f);
            }
        }
    }
    return f;
}

void CompareTimingImages(const std::vector<Pixel>& reference, const std::vector<Pixel>& timed) {
    Check(reference.size() == timed.size(), "timed/untimed output sizes differ");
    float max_error = 0.0f;
    double squared = 0.0;
    uint64_t values = 0;
    for (size_t i = 0; i < reference.size(); ++i) {
        const float a[] = {reference[i].r, reference[i].g, reference[i].b, reference[i].a};
        const float b[] = {timed[i].r, timed[i].g, timed[i].b, timed[i].a};
        for (size_t c = 0; c < 4; ++c) {
            const float error = std::fabs(a[c] - b[c]);
            max_error = std::max(max_error, error);
            squared += double(error) * error;
            ++values;
        }
    }
    const double rms = std::sqrt(squared / double(values));
    std::printf("timing parity: max %.8f RMS %.8f\n", max_error, rms);
    Check(max_error <= 1.0e-3f && rms <= 1.0e-5,
          "enabling FidelityFX GPU timing callbacks changed output pixels");
}

using SdkTicks = std::array<uint64_t, 5>;
constexpr gpu::DofGpuTiming::Stage kTimedSdkStages[] = {
    gpu::DofGpuTiming::Stage::SdkDownsampleDepth,
    gpu::DofGpuTiming::Stage::SdkDownsampleColor,
    gpu::DofGpuTiming::Stage::SdkDilate,
    gpu::DofGpuTiming::Stage::SdkBlur,
    gpu::DofGpuTiming::Stage::SdkComposite,
};
constexpr const char* kTimedSdkNames[] = {
    "downsample_depth", "downsample_color", "dilate", "blur", "composite",
};

SdkTicks GetSdkTicks(const gpu::DofGpuTiming::FrameSample& sample) {
    SdkTicks ticks{};
    for (size_t i = 0; i < ticks.size(); ++i)
        ticks[i] = sample.ticks[static_cast<uint32_t>(kTimedSdkStages[i])];
    return ticks;
}

double TicksToMs(uint64_t ticks, uint64_t frequency) {
    return double(ticks) * 1000.0 / double(frequency);
}

struct WorldBenchSample {
    uint64_t total_ticks = 0;
    SdkTicks sdk_ticks{};
};

struct WorldBenchCase {
    const char* name = nullptr;
    uint32_t width = 0, height = 0;
    uint32_t processing_width = 0, processing_height = 0;
    bool mixed = false;
    uint32_t scale = 1, rings = 4;
    uint32_t iterations = 0;
    bool no_merge = false;
    gpu::FidelityFxDofMode mode = gpu::FidelityFxDofMode::Materialized;
    bool single_global_reset = false;
    uint64_t frequency = 0;
    std::vector<WorldBenchSample> samples;
};

void CheckTimingHierarchy(const AdapterResult& timed) {
    Check(timed.has_timing && timed.timing_frequency != 0,
          "timed adapter run did not return a GPU timestamp sample");
    const uint64_t total = timed.timing.ticks[static_cast<uint32_t>(gpu::DofGpuTiming::Stage::DofTotal)];
    const auto sdk = GetSdkTicks(timed.timing);
    uint64_t child_sum = 0;
    for (size_t i = 0; i < sdk.size(); ++i) {
        Check(timed.timing.intervals[static_cast<uint32_t>(kTimedSdkStages[i])] == timed.timing_repetitions,
              "SDK timing sample count does not match measured repetitions");
        child_sum += sdk[i];
    }
    Check(total + 1 >= child_sum,
          "prepare + SDK + present total is shorter than its five nested SDK passes");
}

std::vector<WorldBenchCase> RunWorldBenchmark(Warp& warp, uint32_t repetitions, bool world_modes=false) {
    std::vector<WorldBenchCase> cases;
    using Mode=gpu::FidelityFxDofMode;
    struct Storage { Mode mode; bool reset; const char* name; };
    constexpr Storage storage[]={
        {Mode::Materialized,false,"materialized"},
        {Mode::VirtualInput,false,"virtual-input"},
        {Mode::VirtualInputAndOutput,false,"virtual-input-output"},
        {Mode::Materialized,true,"materialized-single-reset"}};
    constexpr std::pair<uint32_t,uint32_t> extents[] = {{1280,720},{2880,2160}};
    for (const auto [width,height] : extents) {
        for (const bool mixed : {false,true}) {
          std::vector<Pixel> materialized_reference;
          for(size_t mode_index=0;mode_index<(world_modes?std::size(storage):1);++mode_index) {
            const auto selected=storage[mode_index];
            WorldBenchCase bench{};
            bench.name = world_modes?selected.name:(mixed ? "mixed" : "typical");
            bench.width = width;
            bench.height = height;
            bench.mixed = mixed;
            // Production High/Cinematic profile: 2x processing extent, eight SDK rings, no merge.
            bench.scale = 2;
            bench.rings = 8;
            bench.no_merge = true;
            bench.mode = selected.mode;
            bench.single_global_reset = selected.reset;
            bench.iterations = repetitions;
            const auto processing = gpu::FidelityFxDofProcessingSize(width,height,bench.scale);
            Check(processing.width && processing.height,"benchmark processing extent is unsupported");
            bench.processing_width = processing.width;
            bench.processing_height = processing.height;
            gpu::FidelityFxDof dof;
            Check(dof.Create(warp.Device(),processing.width,processing.height,bench.rings,bench.no_merge,
                             bench.mode,bench.single_global_reset),
                  dof.Failure());
            const Fixture fixture = MakeTimingFixture(width,height,mixed);
            const D3D12_RECT full{0,0,LONG(width),LONG(height)};
            // The untimed pass warms the SDK context/PSOs and provides a same-input parity reference.
            std::vector<Pixel> untimed_reference;
            {
                auto warmup = warp.RunAdapter(dof,fixture,full,bench.scale,12.0f,1.0f,0.45f,
                                              bench.rings,bench.no_merge,false,1,
                                              bench.mode,bench.single_global_reset);
                CheckFiniteAndBounded(warmup.destination);
                if(world_modes) {
                    if(mode_index==0) materialized_reference=warmup.destination;
                    else {
                        Check(materialized_reference.size()==warmup.destination.size(),
                              "storage benchmark native parity extent mismatch");
                        for(size_t i=0;i<materialized_reference.size();++i)
                            Check(SamePixel(materialized_reference[i],warmup.destination[i]),
                                  "storage benchmark differs from materialized native output");
                    }
                }
                untimed_reference = std::move(warmup.destination);
            }
            // RunAdapter allocates its input/output/query resources and binds its PSOs before this
            // repeated command-list loop. The loop reuses those objects and excludes context setup.
            const auto timed = warp.RunAdapter(dof,fixture,full,bench.scale,12.0f,1.0f,0.45f,
                                               bench.rings,bench.no_merge,true,repetitions,
                                               bench.mode,bench.single_global_reset);
            CheckTimingHierarchy(timed);
            CompareTimingImages(untimed_reference,timed.destination);
            const uint32_t total_index = static_cast<uint32_t>(gpu::DofGpuTiming::Stage::DofTotal);
            bench.frequency = timed.timing_frequency;
            bench.samples.push_back({timed.timing.ticks[total_index] / repetitions,
                                     GetSdkTicks(timed.timing)});
            for (uint64_t& ticks : bench.samples.back().sdk_ticks) ticks /= repetitions;
            std::printf("world timing native %ux%u -> processing %ux%u %s, scale %u, %u rings, "
                        "merge disabled, 1x scene/depth inputs: ",
                        width,height,processing.width,processing.height,bench.name,bench.scale,bench.rings);
            const auto& last = bench.samples.back();
            std::printf("total %.3f ms; SDK",TicksToMs(last.total_ticks,bench.frequency));
            for (size_t i = 0; i < last.sdk_ticks.size(); ++i)
                std::printf(" %s=%.3f",kTimedSdkNames[i],TicksToMs(last.sdk_ticks[i],bench.frequency));
            std::puts(" ms");
            cases.push_back(std::move(bench));
          }
        }
    }
    return cases;
}

std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8,0,value.data(),int(value.size()),nullptr,0,nullptr,nullptr);
    Check(bytes > 0,"failed to encode adapter name as UTF-8");
    std::string out(size_t(bytes),'\0');
    Check(WideCharToMultiByte(CP_UTF8,0,value.data(),int(value.size()),out.data(),bytes,nullptr,nullptr) == bytes,
          "failed to encode adapter name as UTF-8");
    return out;
}

std::string JsonEscape(const std::string& value) {
    std::string out;
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(char(c)); }
        else if (c < 0x20) out.push_back(' ');
        else out.push_back(char(c));
    }
    return out;
}

void WriteWorldBenchmarkJson(const std::filesystem::path& path, const std::wstring& adapter,
                             const std::vector<WorldBenchCase>& cases) {
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path,std::ios::binary);
    Check(bool(out),"cannot create world timing JSON output");
    out << "{\n  \"adapter\":\"" << JsonEscape(Utf8(adapter)) << "\",\n"
        << "  \"timing_source\":\"D3D12 timestamp queries\",\n"
        << "  \"sample_count\":1,\n  \"cases\":[\n";
    for (size_t c = 0; c < cases.size(); ++c) {
        const auto& bench = cases[c];
        out << "    {\"name\":\"" << bench.name << "\",\"width\":" << bench.width
            << ",\"height\":" << bench.height << ",\"scene\":\""
            << (bench.mixed ? "mixed" : "typical") << "\",\"processing_width\":"
            << bench.processing_width << ",\"processing_height\":" << bench.processing_height
            << ",\"scale\":" << bench.scale
            << ",\"storage_mode\":" << uint32_t(bench.mode)
            << ",\"single_global_reset\":" << (bench.single_global_reset?"true":"false")
            << ",\"profile\":\"production High/Cinematic 2x, eight-ring\",\"rings\":" << bench.rings
            << ",\"input_samples\":1,\"iterations_per_sample\":"
            << bench.iterations << ",\"ring_merge_disabled\":"
            << (bench.no_merge ? "true" : "false") << ",\"frequency_hz\":" << bench.frequency
            << ",\"samples\":[";
        for (size_t i = 0; i < bench.samples.size(); ++i) {
            const auto& sample = bench.samples[i];
            if (i) out << ',';
            uint64_t child_sum = 0;
            for (const uint64_t ticks : sample.sdk_ticks) child_sum += ticks;
            out << "{\"pipeline_total_ms\":" << TicksToMs(sample.total_ticks,bench.frequency)
                << ",\"pipeline_total_ticks\":" << sample.total_ticks
                << ",\"sdk_pass_sum_ms\":" << TicksToMs(child_sum,bench.frequency)
                << ",\"hierarchy_margin_ms\":" << TicksToMs(sample.total_ticks-child_sum,bench.frequency)
                << ",\"sdk_passes\":[";
            for (size_t j = 0; j < sample.sdk_ticks.size(); ++j) {
                if (j) out << ',';
                out << "{\"name\":\"" << kTimedSdkNames[j] << "\",\"order\":" << j
                    << ",\"ticks\":" << sample.sdk_ticks[j]
                    << ",\"milliseconds\":" << TicksToMs(sample.sdk_ticks[j],bench.frequency) << '}';
            }
            out << "]}";
        }
        out << "]}" << (c + 1 == cases.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
    Check(bool(out),"failed while writing world timing JSON output");
}

} // namespace

int main(int argc, char** argv) {
    try {
        bool hardware = false;
        bool benchmark = false;
        bool world_contracts = false;
        bool world_modes = false;
        bool msaa_contracts = false;
        bool reuse_contracts = false;
        bool world_cache_contracts = false;
        bool world_multi_cache_contracts = false;
        uint32_t repetitions = 3;
        std::filesystem::path json_path = "out/tests/fidelityfx-dof/world-timing.json";
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--hardware") hardware = true;
            else if (arg == "--benchmark") benchmark = true;
            else if (arg == "--world-contracts") world_contracts = true;
            else if (arg == "--world-modes") world_modes = true;
            else if (arg == "--msaa-contracts") msaa_contracts = true;
            else if (arg == "--reuse-contracts") reuse_contracts = true;
            else if (arg == "--world-cache-contracts") world_cache_contracts = true;
            else if (arg == "--world-multi-cache-contracts") world_multi_cache_contracts = true;
            else if (arg == "--json" && i + 1 < argc) json_path = argv[++i];
            else if (arg == "--samples" && i + 1 < argc) {
                const int parsed = std::stoi(argv[++i]);
                Check(parsed >= 1 && parsed <= 20,"--samples must be between 1 and 20");
                repetitions = uint32_t(parsed);
            } else {
                throw std::runtime_error("usage: newffx_dof_warp [--hardware] [--benchmark --hardware] "
                                         "[--samples 1..20] [--json path] [--world-contracts] [--world-modes] "
                                         "[--msaa-contracts] [--reuse-contracts] [--world-cache-contracts] "
                                         "[--world-multi-cache-contracts]");
            }
        }
        Check(!benchmark || hardware,"--benchmark requires explicit --hardware; WARP timing is not performance data");
        Check(!benchmark || !world_contracts,"--world-contracts and --benchmark are separate runs");
        Check(!world_modes || benchmark,"--world-modes requires --benchmark --hardware");
        Check(!reuse_contracts || (!benchmark && !world_contracts && !msaa_contracts),
              "--reuse-contracts must run separately from other contract/benchmark modes");
        Check(!world_cache_contracts || (!benchmark && !world_contracts && !msaa_contracts && !reuse_contracts),
              "--world-cache-contracts must run separately from other contract/benchmark modes");
        Check(!world_multi_cache_contracts ||
              (!benchmark && !world_contracts && !msaa_contracts && !reuse_contracts && !world_cache_contracts),
              "--world-multi-cache-contracts must run separately from other contract/benchmark modes");
        CheckAdapterCompilation();
        Warp warp(hardware);
        if(world_multi_cache_contracts) {
            CheckWorldMultiCacheContracts(warp);
            return 0;
        }
        if(world_cache_contracts) {
            CheckWorldCacheContracts(warp);
            return 0;
        }
        if(reuse_contracts) {
            CheckExactReuseContracts(warp);
            return 0;
        }
        if(msaa_contracts) {
            for(uint32_t samples:{2u,4u,8u}) warp.CheckMixedMsaaPrepare(samples,true);
            std::puts("PASS: exact raw-MSAA sample initializer and prepare contracts");
            return 0;
        }
        if(world_contracts) {
            CheckWorldStorageContracts(warp);
            std::puts("PASS: exact world storage and isolated single-global-reset contracts");
            return 0;
        }
        if (benchmark) {
            const auto cases = RunWorldBenchmark(warp,repetitions,world_modes);
            WriteWorldBenchmarkJson(json_path,warp.AdapterName(),cases);
            std::printf("PASS: hardware world timing benchmark (%u samples/case), JSON %s\n",
                        repetitions,json_path.string().c_str());
            return 0;
        }
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
        std::puts(hardware ? "PASS: production FidelityFX DoF on D3D12 hardware"
                           : "PASS: production FidelityFX DoF on D3D12 WARP");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
