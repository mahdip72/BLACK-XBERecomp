#pragma once

#include <cstdint>
#include <memory>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace gpu {

struct FidelityFxDofSize { uint32_t width, height; };

// Benchmarkable storage adaptations; all modes keep the same virtual context, gather grid and kernel.
enum class FidelityFxDofMode : uint32_t { Materialized, VirtualInput, VirtualInputAndOutput };

// CompareOnly records exact input changes while running the complete instrumented blur.
// Guarded additionally permits whole-group reuse with a valid prior address certificate.
enum class FidelityFxDofReuseMode : uint32_t { Disabled, CompareOnly, Guarded, WholeWorld, WholeWorldMulti };

constexpr bool FidelityFxDofUsesWorldCache(FidelityFxDofReuseMode mode) {
    return mode == FidelityFxDofReuseMode::WholeWorld || mode == FidelityFxDofReuseMode::WholeWorldMulti;
}

// Read once per process. Medium uses Materialized; High defaults to fused storage (2).
// BLACK_DOF_WORLD_MODE selects 0/1/2; an explicit 0 preserves materialized storage.
FidelityFxDofMode FidelityFxDofConfiguredMode();
// Read once per process: BLACK_DOF_WORLD_REUSE=0/1/2/3/4. High defaults to exact multi-entry reuse (4).
FidelityFxDofReuseMode FidelityFxDofConfiguredReuseMode();

constexpr FidelityFxDofSize FidelityFxDofInputSize(FidelityFxDofSize processing, FidelityFxDofMode mode) {
    return mode == FidelityFxDofMode::Materialized ? processing :
        FidelityFxDofSize{processing.width / 2, processing.height / 2};
}
constexpr FidelityFxDofSize FidelityFxDofOutputSize(FidelityFxDofSize processing, FidelityFxDofMode mode) {
    return mode == FidelityFxDofMode::VirtualInputAndOutput ?
        FidelityFxDofSize{processing.width / 2, processing.height / 2} : processing;
}
// Existing b1 layout: 1=Medium, 2=materialized High, 3=virtual input, 4=virtual input/fused output.
constexpr float FidelityFxDofAdapterLayout(uint32_t scale, FidelityFxDofMode mode) {
    return mode == FidelityFxDofMode::Materialized ? float(scale) :
        mode == FidelityFxDofMode::VirtualInput ? 3.0f : 4.0f;
}

enum class FidelityFxDofPass { DownsampleDepth, DownsampleColor, Dilate, Blur, Composite, CompareHistory, ReduceDirty, WorldCompare };

struct FidelityFxDofDiagnosticResource {
    ID3D12Resource* resource = nullptr;
    uint32_t state = 0;  // D3D12_RESOURCE_STATES at the end of the last recorded SDK dispatch.
};
struct FidelityFxDofReuseDiagnostics {
    FidelityFxDofDiagnosticResource dirtyHierarchy, certificate, nearColor, farColor;
    uint32_t groupWidth = 0, groupHeight = 0, hierarchyMipCount = 0;
    bool historyValid = false;
};

struct FidelityFxDofDispatchHistory {
    uint64_t ordered_queue_identity = 0;
    uint64_t output_content_generation = 0;
    bool output_contents_preserved = false;
};
struct FidelityFxDofWorldCacheDiagnostics {
    FidelityFxDofDiagnosticResource predicate, historyColor, historyDepth, previousUnsafe;
    bool historyValid = false;
    uint64_t generation = 0, queueId = 0;
};

// Optional command-recording hooks for GPU timestamps, not CPU callback durations. Hooks must only record
// timestamps on this list; they must not submit/wait, reenter the adapter, or change its graphics state.
// Each begin is paired with an end, including backend recording failures. The descriptor lives through Dispatch.
struct FidelityFxDofTiming {
    void* user = nullptr;
    void (*mark)(void*, ID3D12GraphicsCommandList*, FidelityFxDofPass, bool begin) noexcept = nullptr;
};

// Keep partial 64-pixel SDK workgroups outside the visible image. The adapter edge-extends the logical
// viewport over this processing extent. A 2x profile replicates native inputs and averages each output 2x2;
// CoC is expressed in processing pixels, so the caller scales both CoC and its encoding bound by the same factor.
// 16384 is D3D12's maximum 2D texture dimension; reject zero/oversized inputs without arithmetic overflow.
constexpr FidelityFxDofSize FidelityFxDofProcessingSize(uint32_t width, uint32_t height, uint32_t scale = 1) {
    if ((scale != 1 && scale != 2) || !width || !height || width > 16384 / scale || height > 16384 / scale) return {};
    return {(width * scale + 63u) & ~63u, (height * scale + 63u) & ~63u};
}

// Owns the official SDK context, not the caller's command list or input/output resources.
// Wait for all submitted dispatches before Reset, recreation, or destruction. Dispatch restores each external
// resource's entry state, but the caller must rebind its descriptor heaps/root signature afterward.
class FidelityFxDof {
public:
    FidelityFxDof();
    ~FidelityFxDof();
    FidelityFxDof(const FidelityFxDof&) = delete;
    FidelityFxDof& operator=(const FidelityFxDof&) = delete;

    // Production callers pass FidelityFxDofProcessingSize, not the unpadded logical target size.
    bool Create(ID3D12Device* device, uint32_t width, uint32_t height, uint32_t rings = 4,
                bool disable_ring_merge = false, FidelityFxDofMode mode = FidelityFxDofMode::Materialized,
                bool single_global_reset = false, FidelityFxDofReuseMode reuse = FidelityFxDofReuseMode::Disabled);
    // Single-sample RGBA16F color and R32F encoded depth enter in NON_PIXEL_SHADER_RESOURCE; a distinct
    // ALLOW_UNORDERED_ACCESS output enters in UNORDERED_ACCESS. Materialized resources have the context
    // extent. Virtual inputs have half that extent; fused output also has half extent and is RGBA32F
    // (ordinary output is RGBA16F). Resource descriptions always state the actual physical dimensions.
    // For signed CoC c in processing pixels (near negative), depth = .5+c/(2*bound), bound >= abs(c).
    bool Dispatch(ID3D12GraphicsCommandList* command_list, ID3D12Resource* color, ID3D12Resource* encoded_depth,
                  ID3D12Resource* output, float coc_bound, const FidelityFxDofTiming* timing = nullptr,
                  const FidelityFxDofDispatchHistory* history = nullptr);
    void Reset();
    // Call after fake priming and whenever scene/viewport/optics continuity is not established.
    void InvalidateReuseHistory();
    // Borrowed pointers; do not release them. Diagnostic copies must restore these states before
    // another SDK dispatch. The caller owns queue ordering and fencing, as for the ordinary output.
    bool GetReuseDiagnostics(FidelityFxDofReuseDiagnostics& diagnostics) const;
    bool GetWorldCacheDiagnostics(FidelityFxDofWorldCacheDiagnostics& diagnostics) const;
    // Transfer a separately prepared context only after the receiver's GPU work has completed.
    void Swap(FidelityFxDof& other) noexcept;
    bool Ready() const;
    FidelityFxDofReuseMode EffectiveReuseMode() const;
    const char* CacheFallbackReason() const;
    bool Matches(uint32_t width, uint32_t height, uint32_t rings = 4, bool disable_ring_merge = false,
                 FidelityFxDofMode mode = FidelityFxDofMode::Materialized, bool single_global_reset = false,
                 FidelityFxDofReuseMode reuse = FidelityFxDofReuseMode::Disabled) const;
    const char* Failure() const;

private:
    bool CreateInternal(ID3D12Device* device, uint32_t width, uint32_t height, uint32_t rings,
                        bool disable_ring_merge, FidelityFxDofMode mode, bool single_global_reset,
                        FidelityFxDofReuseMode reuse);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gpu
