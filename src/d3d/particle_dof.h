#pragma once

#include <cstdint>

namespace gpu {

// Copied render-queue markers around the game's known billboard upload only.
constexpr uint32_t kClearParticleDofBegin = 0x1000;
constexpr uint32_t kClearParticleDofEnd = 0x2000;
constexpr uint32_t kParticleDofMaxQuads = 1024;

struct NativeParticleVertex {
    float position[4];
    uint32_t argb;
    float uv[2];
    uint32_t pad;
};

struct ParticleDofQuad {
    // Signed endpoints preserve UV flips. A zero extent selects the native path.
    float uv_bounds[4];
    uint32_t argb[4];
    uint32_t atlas_xy = 0;    // cell origin, uint16 x/y; includes the two-pixel guard
    uint32_t screen_xy = 0;   // clipped expanded screen rectangle origin, uint16 x/y
    uint32_t atlas_size = 0;  // whole cell width/height including guards, uint16 x/y
    float atlas_scale = 0;   // zero keeps the legacy filter; otherwise one or one half
};

struct ParticleDofGlobals {
    float optics[4];    // focus, eased radius in this target's pixels, near scale, far scale
    float viewport[4];  // x, y, width, height in this target's pixels
    float target[4];    // width, height, quad count, 0=LESS / 1=LEQUAL
    float atlas_target[4]; // atlas width/height, first quad and count for the filter draw
    // ViewZ = x / (raw depth - y). z enables verified destination coverage; w is reserved.
    float projection[4];
};

struct ParticleDofGeometryParams {
    ParticleDofGlobals globals;
    float c97[4];
    float c98[4];
    float guest_size[2];
};

struct ParticleDofGeometryStats {
    uint32_t active = 0;
    uint32_t focused = 0;
    uint32_t invalid = 0;
};

static_assert(sizeof(NativeParticleVertex) == 32);
static_assert(sizeof(ParticleDofQuad) == 48);
static_assert(sizeof(ParticleDofGlobals) == 80);

}  // namespace gpu
