#pragma once
#include <cstdint>

// Two rects carry projection z, reload strength, focus distance, near/far strength, radius and a reserved value.
namespace gpu {
constexpr uint32_t kClearDepthOfField = 0x200;
constexpr uint32_t kDepthOfFieldRects = 2;
// Weapon markers bracket the early first-person draw. The fourth float carries a uint32 serial's bits.
constexpr uint32_t kClearWeaponDofBegin = 0x400;
constexpr uint32_t kClearWeaponDofEnd = 0x800;
constexpr uint32_t kWeaponDofRects = 1;
}  // namespace gpu

namespace d3d {
// False leaves the original effect in place. The render thread receives a copy of these values and the draw state.
bool DepthOfField(const float* projection, float strength);
bool BeginWeaponDepthOfField(const float* projection, float strength, uint32_t serial);
bool EndWeaponDepthOfField(const float* projection, float strength, uint32_t serial);
}  // namespace d3d
