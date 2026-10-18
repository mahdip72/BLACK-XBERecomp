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
};

struct ParticleDofGlobals {
    float optics[4];    // focus, eased radius in this target's pixels, near scale, far scale
    float viewport[4];  // x, y, width, height in this target's pixels
    float target[4];    // width, height, quad count, 0=LESS / 1=LEQUAL
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
static_assert(sizeof(ParticleDofQuad) == 32);
static_assert(sizeof(ParticleDofGlobals) == 48);

}  // namespace gpu
