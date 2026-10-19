#pragma once

#include <cstdint>
#include <memory>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace gpu {

struct FidelityFxDofSize { uint32_t width, height; };

enum class FidelityFxDofPass { DownsampleDepth, DownsampleColor, Dilate, Blur, Composite };

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
                bool disable_ring_merge = false);
    // Single-sample RGBA16F color and R32F encoded depth enter in NON_PIXEL_SHADER_RESOURCE; a distinct
    // RGBA16F ALLOW_UNORDERED_ACCESS output enters in UNORDERED_ACCESS. All three have the context extent.
    // For signed CoC c in processing pixels (near negative), depth = .5+c/(2*bound), bound >= abs(c).
    bool Dispatch(ID3D12GraphicsCommandList* command_list, ID3D12Resource* color, ID3D12Resource* encoded_depth,
                  ID3D12Resource* output, float coc_bound, const FidelityFxDofTiming* timing = nullptr);
    void Reset();
    // Transfer a separately prepared context only after the receiver's GPU work has completed.
    void Swap(FidelityFxDof& other) noexcept;
    bool Ready() const;
    bool Matches(uint32_t width, uint32_t height, uint32_t rings = 4, bool disable_ring_merge = false) const;
    const char* Failure() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gpu
