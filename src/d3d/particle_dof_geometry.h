#pragma once
#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <vector>

#include "particle_dof.h"

namespace gpu {

enum class ParticleDofGeometryResult { Ready, ZeroRadius, InvalidInput, UnsupportedQuad };

// The same focus band and signed CoC used by the world adapter, in current target pixels.
inline float ParticleDofRadius(float view_z, const ParticleDofGlobals& g) {
    const float d = view_z - g.optics[0];
    const float coc = std::min(std::max(std::abs(d) - g.optics[0] * .15f, 0.0f) / view_z, 1.0f);
    return coc * g.optics[1] * g.optics[d < 0 ? 2 : 3];
}

// Keep the original quad's local coordinates separate from texture coordinates, including full-texture
// flips. Atlas rectangles remain native: the native half-texel/filter footprint can reach adjacent cells.
inline ParticleDofGeometryResult ExpandParticleDofQuad(
    const NativeParticleVertex (&original)[4], const ParticleDofGeometryParams& p,
    float radius, NativeParticleVertex (&expanded)[4], ParticleDofQuad& metadata) {
    std::memcpy(expanded, original, sizeof(original));
    metadata = {};
    if (!std::isfinite(radius) || radius < 0) return ParticleDofGeometryResult::InvalidInput;
    for (const auto& v : original) {
        for (float x : v.position) if (!std::isfinite(x)) return ParticleDofGeometryResult::InvalidInput;
        for (float x : v.uv) if (!std::isfinite(x)) return ParticleDofGeometryResult::InvalidInput;
    }
    if (radius == 0) return ParticleDofGeometryResult::ZeroRadius;
    for (float x : p.c97) if (!std::isfinite(x)) return ParticleDofGeometryResult::InvalidInput;
    for (float x : p.c98) if (!std::isfinite(x)) return ParticleDofGeometryResult::InvalidInput;
    for (int j = 0; j < 2; ++j)
        if (!std::isfinite(p.guest_size[j]) || p.guest_size[j] <= 0 ||
            !std::isfinite(p.globals.target[j]) || p.globals.target[j] <= 0 || p.c98[j] == 0)
            return ParticleDofGeometryResult::InvalidInput;
    if (original[0].position[2] <= 0 ||
        original[0].position[3] * p.c98[3] + p.c97[3] <= 0)
        return ParticleDofGeometryResult::UnsupportedQuad;
    for (int k = 1; k < 4; ++k)
        if (original[k].position[2] != original[0].position[2] ||
            original[k].position[3] != original[0].position[3])
            return ParticleDofGeometryResult::UnsupportedQuad;
    const float u0 = original[0].uv[0], v0 = original[0].uv[1];
    const float u1 = original[3].uv[0], v1 = original[1].uv[1];
    if (u0 == u1 || v0 == v1 || original[1].uv[0] != u0 || original[3].uv[1] != v0 ||
        original[2].uv[0] != u1 || original[2].uv[1] != v1 ||
        std::min(u0, u1) != 0 || std::max(u0, u1) != 1 ||
        std::min(v0, v1) != 0 || std::max(v0, v1) != 1)
        return ParticleDofGeometryResult::UnsupportedQuad;
    float screen[4][2];
    for (int k = 0; k < 4; ++k) for (int j = 0; j < 2; ++j) {
        // Match the native shader's affine projection before scaling into the bound target.
        const float guest = original[k].position[j] * p.c98[j] + p.c97[j];
        screen[k][j] = guest * (p.globals.target[j] / p.guest_size[j]);
        if (!std::isfinite(screen[k][j])) return ParticleDofGeometryResult::InvalidInput;
    }
    const float ux = screen[3][0] - screen[0][0], uy = screen[3][1] - screen[0][1];
    const float vx = screen[1][0] - screen[0][0], vy = screen[1][1] - screen[0][1];
    const float lu = std::hypot(ux, uy), lv = std::hypot(vx, vy), det = ux * vy - uy * vx;
    if (!std::isfinite(det) || lu <= 1e-4f || lv <= 1e-4f || std::abs(det) <= lu * lv * 1e-6f)
        return ParticleDofGeometryResult::UnsupportedQuad;
    for (int j = 0; j < 2; ++j)
        if (std::abs(screen[2][j] - screen[1][j] - screen[3][j] + screen[0][j]) >
            std::max(1e-4f, (lu + lv) * 1e-5f)) return ParticleDofGeometryResult::UnsupportedQuad;
    // Row lengths of the inverse screen Jacobian bound a circular kernel, including rotated/skewed quads
    // and targets with different horizontal and vertical scale factors.
    const float pu = radius * lv / std::abs(det), pv = radius * lu / std::abs(det);
    if (!std::isfinite(pu) || !std::isfinite(pv)) return ParticleDofGeometryResult::InvalidInput;
    const float local[4][2] = {{-pu, -pv}, {-pu, 1 + pv}, {1 + pu, 1 + pv}, {1 + pu, -pv}};
    for (int k = 0; k < 4; ++k) {
        for (int j = 0; j < 2; ++j) {
            expanded[k].position[j] = original[0].position[j] +
                local[k][0] * (original[3].position[j] - original[0].position[j]) +
                local[k][1] * (original[1].position[j] - original[0].position[j]);
            if (!std::isfinite(expanded[k].position[j])) {
                std::memcpy(expanded, original, sizeof(original));
                return ParticleDofGeometryResult::InvalidInput;
            }
        }
        expanded[k].uv[0] = u0 + local[k][0] * (u1 - u0);
        expanded[k].uv[1] = v0 + local[k][1] * (v1 - v0);
        if (!std::isfinite(expanded[k].uv[0]) || !std::isfinite(expanded[k].uv[1])) {
            std::memcpy(expanded, original, sizeof(original));
            return ParticleDofGeometryResult::InvalidInput;
        }
    }
    metadata.uv_bounds[0] = u0; metadata.uv_bounds[1] = v0;
    metadata.uv_bounds[2] = u1; metadata.uv_bounds[3] = v1;
    for (int k = 0; k < 4; ++k) metadata.argb[k] = original[k].argb;
    return ParticleDofGeometryResult::Ready;
}

inline bool BuildParticleDofGeometry(std::span<const NativeParticleVertex> vertices,
    const ParticleDofGeometryParams& p, std::vector<NativeParticleVertex>& expanded,
    std::vector<ParticleDofQuad>& metadata, ParticleDofGeometryStats& stats) {
    stats = {};
    expanded.assign(vertices.begin(), vertices.end());
    metadata.assign(vertices.size() / 4, {});
    if (vertices.empty() || vertices.size() % 4 || vertices.size() > 4096) return false;
    for (float x : p.globals.optics) if (!std::isfinite(x)) return false;
    if (p.globals.optics[0] <= 0 || p.globals.optics[1] < 0 ||
        p.globals.optics[2] < 0 || p.globals.optics[3] < 0) return false;
    for (size_t k = 0; k < vertices.size() / 4; ++k) {
        NativeParticleVertex input[4], output[4];
        std::memcpy(input, vertices.data() + k * 4, sizeof(input));
        const float view_z = 1.0f / input[0].position[2];
        const float radius = std::isfinite(view_z) && view_z > 0 ?
            ParticleDofRadius(view_z, p.globals) : -1.0f;
        const auto result = ExpandParticleDofQuad(input, p, radius, output, metadata[k]);
        if (result == ParticleDofGeometryResult::Ready) {
            std::memcpy(expanded.data() + k * 4, output, sizeof(output));
            ++stats.active;
        } else if (result == ParticleDofGeometryResult::ZeroRadius) ++stats.focused;
        else ++stats.invalid;
    }
    return stats.active != 0;
}

} // namespace gpu
