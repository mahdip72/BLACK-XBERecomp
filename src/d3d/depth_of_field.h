#pragma once
#include <cstdint>

// A queued marker, like ambient occlusion: one rect carries projection z, strength and focus distance.
namespace gpu {
constexpr uint32_t kClearDepthOfField = 0x200;
constexpr uint32_t kDepthOfFieldRects = 1;
}  // namespace gpu

namespace d3d {
// False leaves the original effect in place. The render thread receives a copy of these values and the draw state.
bool DepthOfField(const float* projection, float strength);
}  // namespace d3d
