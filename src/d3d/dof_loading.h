#pragma once

#include <atomic>
#include <cstdint>

namespace gpu {

// One copied render-queue rect orders the loading lifetime with native draws and presents.
constexpr uint32_t kClearDofLoading = 0x4000;
struct DofLoadingMarker {
    uint64_t generation;
    uint32_t begin;
    uint32_t reserved;
};
static_assert(sizeof(DofLoadingMarker) == 16);

inline std::atomic<uint64_t> g_dof_loading_requested_generation{0};
inline std::atomic<uint64_t> g_dof_loading_ready_generation{0};

// The backend starts work only after the first native present following a begin marker.
// It publishes readiness with release ordering after preparation or a terminal fallback.
// Readiness must cover the current graphics configuration, including changes during a load.

}  // namespace gpu

namespace d3d {

// These enqueue markers without waiting. Zero means that no request could be queued.
uint64_t BeginMissionDofPreparation();
void EndMissionDofPreparation(uint64_t generation);

// Cancelled, unavailable and superseded requests never hold the native loading transition.
inline bool MissionDofPreparationReady(uint64_t generation) {
    return !generation ||
        gpu::g_dof_loading_requested_generation.load(std::memory_order_acquire) != generation ||
        gpu::g_dof_loading_ready_generation.load(std::memory_order_acquire) >= generation;
}

}  // namespace d3d
