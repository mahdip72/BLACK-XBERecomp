// Offscreen D3D12 timing regression. The test waits on fences; the production
// helper never does. Run with --hardware for the preferred hardware adapter.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <stdexcept>
#include <vector>

#include "d3d/dof_gpu_timing.h"
#include "d3d/dof_profile.h"

using Microsoft::WRL::ComPtr;
namespace {

void Check(bool okay, const char* what) {
    if (!okay) throw std::runtime_error(what);
}
void Hr(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char message[160];
        std::snprintf(message, sizeof(message), "%s: 0x%08lX", what, static_cast<unsigned long>(hr));
        throw std::runtime_error(message);
    }
}
ComPtr<ID3D12Device> Device(bool hardware) {
    ComPtr<IDXGIFactory6> factory;
    Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
    ComPtr<IDXGIAdapter1> adapter;
    if (hardware) {
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> candidate;
            HRESULT result = factory->EnumAdapterByGpuPreference(
                i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate));
            if (result == DXGI_ERROR_NOT_FOUND) break;
            Hr(result, "EnumAdapterByGpuPreference");
            DXGI_ADAPTER_DESC1 desc{};
            Hr(candidate->GetDesc1(&desc), "GetDesc1");
            ComPtr<ID3D12Device> probe;
            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0,
                                            IID_PPV_ARGS(&probe)))) {
                adapter = candidate;
                break;
            }
        }
        Check(adapter != nullptr, "no D3D12 hardware adapter");
    } else Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "EnumWarpAdapter");
    ComPtr<ID3D12Device> device;
    Hr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                         IID_PPV_ARGS(&device)), "D3D12CreateDevice");
    DXGI_ADAPTER_DESC1 adapter_desc{};
    Hr(adapter->GetDesc1(&adapter_desc), "Get selected adapter description");
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    const HRESULT options_result = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,
                                                               &options, sizeof(options));
    std::printf("D3D12 adapter %ls, conservative raster tier %u, query 0x%08lX\n",
                adapter_desc.Description, unsigned(options.ConservativeRasterizationTier),
                static_cast<unsigned long>(options_result));
    return device;
}
ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, uint64_t bytes,
                              D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> resource;
    Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
                                       nullptr, IID_PPV_ARGS(&resource)), "CreateCommittedResource");
    return resource;
}

struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Resource> source, destination;
    std::vector<ComPtr<ID3D12CommandAllocator>> allocators;
    std::vector<ComPtr<ID3D12GraphicsCommandList>> lists;
    uint64_t signaled = 0;

    explicit Gpu(bool hardware) : device(Device(hardware)) {
        D3D12_COMMAND_QUEUE_DESC q{};
        q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Hr(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
        constexpr uint64_t bytes = 1024 * 1024;
        source = Buffer(device.Get(), bytes, D3D12_RESOURCE_STATE_COPY_SOURCE);
        destination = Buffer(device.Get(), bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    void Wait(uint64_t value) {
        if (fence->GetCompletedValue() >= value) return;
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Check(event != nullptr, "CreateEventW");
        Hr(fence->SetEventOnCompletion(value, event), "SetEventOnCompletion");
        Check(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0, "GPU fence timeout");
        CloseHandle(event);
    }
    template<class Record>
    void Submit(gpu::DofGpuTiming& timer, uint64_t frame, bool final, Record record) {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
        Hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                     nullptr, IID_PPV_ARGS(&list)), "CreateCommandList");
        record(list.Get());
        timer.Resolve(list.Get());
        Hr(list->Close(), "Close command list");
        ID3D12CommandList* execute[] = {list.Get()};
        queue->ExecuteCommandLists(1, execute);
        Hr(queue->Signal(fence.Get(), ++signaled), "Signal");
        timer.Submitted(signaled, frame, final);
        allocators.push_back(allocator);
        lists.push_back(list);
    }
    void Copy(ID3D12GraphicsCommandList* list, uint32_t repetitions) {
        for (uint32_t i = 0; i < repetitions; ++i)
            list->CopyBufferRegion(destination.Get(), 0, source.Get(), 0, 1024 * 1024);
    }
};

void Run(bool hardware) {
    Gpu gpu(hardware);
    gpu::DofGpuTiming timer;
    using Stage = gpu::DofGpuTiming::Stage;
    static_assert(uint32_t(Stage::ParticleDirect) == 14 &&
                  uint32_t(Stage::ParticleHierarchy) == 15 &&
                  uint32_t(Stage::SdkDownsampleDepth) == 16 &&
                  uint32_t(Stage::SdkComposite) == 20 &&
                  uint32_t(Stage::ParticleSupport) == 21 &&
                  uint32_t(Stage::ParticleTransmission) == 22);
    Check(gpu::DofGpuTiming::CountsTowardDofTotal(Stage::ParticleHierarchy) &&
          gpu::DofGpuTiming::CountsTowardDofTotal(Stage::ParticleSupport) &&
          gpu::DofGpuTiming::CountsTowardDofTotal(Stage::ParticleTransmission) &&
          !gpu::DofGpuTiming::CountsTowardDofTotal(Stage::SdkBlur) &&
          gpu::DofGpuTiming::IsSdkSubpass(Stage::SdkBlur),
          "top-level and nested stage classification invalid");
    Check(timer.Init(gpu.device.Get(), gpu.queue.Get()), "timestamp timer unavailable");
    Check(!gpu::DofGpuProfilingEnabled(), "profiling must default off");
    gpu::SetDofGpuProfilingEnabled(true);
    Check(gpu::DofGpuProfilingEnabled(), "profiling toggle did not enable");

    timer.BeginFrame(1, true, 0);
    gpu.Submit(timer, 1, false, [&](ID3D12GraphicsCommandList* list) {
        {
            auto scope = timer.Measure(Stage::ScenePrepare, list);
            gpu.Copy(list, 32);
        }
        {
            auto scope = timer.Measure(Stage::ParticleHierarchy, list);
            gpu.Copy(list, 4);
        }
        {
            auto scope = timer.Measure(Stage::ParticleSupport, list);
            gpu.Copy(list, 4);
        }
        {
            auto scope = timer.Measure(Stage::ParticleTransmission, list);
            gpu.Copy(list, 4);
        }
    });
    gpu.Submit(timer, 1, true, [&](ID3D12GraphicsCommandList* list) {
        auto sdk_whole = timer.Measure(Stage::Sdk, list);
        for (Stage child : {Stage::SdkDownsampleDepth, Stage::SdkDownsampleColor,
                            Stage::SdkDilate, Stage::SdkBlur, Stage::SdkComposite}) {
            auto scope = timer.Measure(child, list);
            gpu.Copy(list, 8);
        }
    });
    // A stale completed-fence value must not expose an in-flight readback.
    timer.Collect(0);
    auto before = timer.TakeReport();
    Check(before.complete_frames == 0 && before.pending_frames == 1 &&
          before.frame_sample_count == 0,
          "in-flight frame was published");
    gpu.Wait(gpu.signaled);
    timer.Collect(gpu.fence->GetCompletedValue());
    auto report = timer.TakeReport();
    const auto& prepare = report.stage[uint32_t(Stage::ScenePrepare)];
    const auto& sdk = report.stage[uint32_t(Stage::Sdk)];
    const auto& hierarchy = report.stage[uint32_t(Stage::ParticleHierarchy)];
    const auto& support = report.stage[uint32_t(Stage::ParticleSupport)];
    const auto& transmission = report.stage[uint32_t(Stage::ParticleTransmission)];
    const auto& total = report.stage[uint32_t(Stage::DofTotal)];
    const auto& zero = report.stage[uint32_t(Stage::WeaponPack)];
    Check(report.complete_frames == 1 && report.invalid_frames == 0, "two-submit frame invalid");
    Check(prepare.intervals == 1 && sdk.intervals == 1 && hierarchy.intervals == 1 &&
          support.intervals == 1 && transmission.intervals == 1 && total.intervals == 5,
          "stage intervals across submissions lost");
    Check(prepare.total_ms > 0 && sdk.total_ms > 0 && hierarchy.total_ms > 0 &&
          std::fabs(total.total_ms - prepare.total_ms - sdk.total_ms - hierarchy.total_ms -
                    support.total_ms - transmission.total_ms) < 1e-5,
          "timestamp conversion or derived total invalid");
    for (Stage child : {Stage::SdkDownsampleDepth, Stage::SdkDownsampleColor,
                        Stage::SdkDilate, Stage::SdkBlur, Stage::SdkComposite})
        Check(report.stage[uint32_t(child)].intervals == 1,
              "SDK subpass marker missing");
    Check(zero.total_ms == 0 && zero.median_frame_ms == 0 && zero.percentile_frames == 1,
          "inactive stage was not a zero frame sample");
    Check(report.frame_sample_count == 1 && report.frame_sample_evicted == 0 &&
          report.frequency != 0, "completed frame sample missing");
    const auto& sample = report.frame_samples[0];
    Check(sample.frame_id == 1 && sample.fence == gpu.signaled &&
          sample.intervals[uint32_t(Stage::DofTotal)] == 5 &&
          sample.intervals[uint32_t(Stage::Sdk)] == 1 &&
          sample.intervals[uint32_t(Stage::SdkComposite)] == 1 &&
          sample.ticks[uint32_t(Stage::WeaponPack)] == 0 &&
          sample.intervals[uint32_t(Stage::WeaponPack)] == 0,
          "frame sample identity, intervals, or zero stage invalid");
    Check(sample.ticks[uint32_t(Stage::DofTotal)] ==
          sample.ticks[uint32_t(Stage::ScenePrepare)] +
          sample.ticks[uint32_t(Stage::Sdk)] +
          sample.ticks[uint32_t(Stage::ParticleHierarchy)] +
          sample.ticks[uint32_t(Stage::ParticleSupport)] +
          sample.ticks[uint32_t(Stage::ParticleTransmission)],
          "nested SDK children were double-counted in frame total");
    gpu::PublishDofGpuReport(report);
    Check(gpu::GetDofGpuReport().complete_frames == 1, "published report mismatch");

    timer.BeginFrame(2, true, gpu.fence->GetCompletedValue());
    gpu.Submit(timer, 2, true, [&](ID3D12GraphicsCommandList* list) {
        for (uint32_t i = 0; i <= gpu::DofGpuTiming::kIntervalsPerFrame; ++i) {
            auto scope = timer.Measure(Stage::ParticleSource, list);
        }
    });
    gpu.Wait(gpu.signaled);
    timer.Collect(gpu.fence->GetCompletedValue());
    report = timer.TakeReport();
    Check(report.complete_frames == 0 && report.invalid_frames == 1 &&
          report.overflow_frames == 1 && report.dropped_intervals == 1 &&
          report.frame_sample_count == 0,
          "overflow frame was treated as a complete sample");

    // Hold completed-fence observation at zero to deterministically occupy
    // every slot, even if WARP or the hardware queue finishes very quickly.
    for (uint64_t frame = 3; frame < 3 + gpu::DofGpuTiming::kSlots; ++frame) {
        timer.BeginFrame(frame, true, 0);
        gpu.Submit(timer, frame, true, [&](ID3D12GraphicsCommandList* list) {
            auto scope = timer.Measure(Stage::WeaponClassify, list);
            gpu.Copy(list, 1);
        });
    }
    const uint64_t busy_frame = 3 + gpu::DofGpuTiming::kSlots;
    timer.BeginFrame(busy_frame, true, 0);
    timer.Submitted(gpu.signaled, busy_frame, true);
    report = timer.TakeReport();
    Check(report.busy_frames == 1 && report.invalid_frames == 1 && report.pending_frames == 8,
          "all-busy ring did not drop the frame explicitly");
    gpu.Wait(gpu.signaled);
    timer.Collect(gpu.fence->GetCompletedValue());
    report = timer.TakeReport();
    Check(report.complete_frames == gpu::DofGpuTiming::kSlots && report.invalid_frames == 0,
          "completed ring slots were not collected");
    Check(report.frame_sample_count == gpu::DofGpuTiming::kSlots &&
          report.frame_sample_evicted == 0, "completed ring frame samples missing");
    for (uint32_t i = 0; i < gpu::DofGpuTiming::kSlots; ++i)
        Check(report.frame_samples[i].frame_id == uint64_t(3 + i),
              "completed frame samples are not ordered by frame ID");

    gpu::SetDofGpuProfilingEnabled(false);
    timer.BeginFrame(busy_frame + 1, gpu::DofGpuProfilingEnabled(), gpu.fence->GetCompletedValue());
    gpu.Submit(timer, busy_frame + 1, true, [&](ID3D12GraphicsCommandList* list) {
        auto scope = timer.Measure(Stage::ScenePrepare, list);
        gpu.Copy(list, 1);
    });
    gpu.Wait(gpu.signaled);
    timer.Collect(gpu.fence->GetCompletedValue());
    report = timer.TakeReport();
    Check(report.complete_frames == 0 && report.invalid_frames == 0 &&
          report.frame_sample_count == 0,
          "disabled profiler emitted a frame");
    std::printf("DOF GPU timing %s PASS: two submits, fence gating, SDK children, "
                "frame samples, derived total, zero stage, overflow, busy ring, opt-out\n",
                hardware ? "hardware" : "WARP");
}
} // namespace

int main(int argc, char** argv) {
    try {
        Check(argc <= 2, "usage: dof_gpu_timing_test [--hardware]");
        const bool hardware = argc == 2 && std::strcmp(argv[1], "--hardware") == 0;
        Check(argc == 1 || hardware, "usage: dof_gpu_timing_test [--hardware]");
        Run(hardware);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "DOF GPU timing test FAIL: %s\n", error.what());
        return 1;
    }
}
