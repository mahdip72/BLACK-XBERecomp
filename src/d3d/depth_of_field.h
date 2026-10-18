#pragma once
#include <cstdint>

// Two rects carry projection z, strength, focus, near/far strength, radius and backend:
// 0 Balanced, 1 FidelityFX Standard, 2 FidelityFX Cinematic.
namespace gpu {
constexpr uint32_t kClearDepthOfField = 0x200;
constexpr uint32_t kDepthOfFieldRects = 2;
// Weapon markers bracket an eligible first-person draw. The fourth float carries a uint32 serial's bits;
// the second rect carries sharp depth and transition width in camera view units, followed by two zeros.
constexpr uint32_t kClearWeaponDofBegin = 0x400;
constexpr uint32_t kClearWeaponDofEnd = 0x800;
constexpr uint32_t kWeaponDofRects = 2;
}  // namespace gpu

namespace d3d {
// False leaves the original effect in place. The render thread receives a copy of these values and the draw state.
bool DepthOfField(const float* projection, float strength);
bool BeginWeaponDepthOfField(const float* projection, float strength, uint32_t serial,
                             float sharp_depth, float transition);
bool EndWeaponDepthOfField(const float* projection, float strength, uint32_t serial,
                           float sharp_depth, float transition);
}  // namespace d3d
