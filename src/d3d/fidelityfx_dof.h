#pragma once

#include <cstdint>
#include <memory>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace gpu {

struct FidelityFxDofSize { uint32_t width, height; };

// Keep partial 64-pixel SDK workgroups outside the visible image. The adapter edge-extends the logical
// viewport over this processing extent, then copies back only the original viewport without rescaling CoC.
// 16384 is D3D12's maximum 2D texture dimension; reject zero/oversized inputs without arithmetic overflow.
constexpr FidelityFxDofSize FidelityFxDofProcessingSize(uint32_t width, uint32_t height) {
    if (!width || !height || width > 16384 || height > 16384) return {};
    return {(width + 63u) & ~63u, (height + 63u) & ~63u};
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
    bool Create(ID3D12Device* device, uint32_t width, uint32_t height);
    // Single-sample RGBA16F color and R32F encoded depth enter in NON_PIXEL_SHADER_RESOURCE; a distinct
    // RGBA16F ALLOW_UNORDERED_ACCESS output enters in UNORDERED_ACCESS. All three have the context extent.
    // For our signed full-resolution CoC c (near negative), depth = .5+c/(2*bound), bound >= abs(c).
    bool Dispatch(ID3D12GraphicsCommandList* command_list, ID3D12Resource* color, ID3D12Resource* encoded_depth,
                  ID3D12Resource* output, float coc_bound);
    void Reset();
    bool Ready() const;
    bool Matches(uint32_t width, uint32_t height) const;
    const char* Failure() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gpu
