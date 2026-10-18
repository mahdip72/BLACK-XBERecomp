#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#include "particle_dof.h"

namespace gpu {

enum class ParticleDofGeometryResult { Ready, ZeroRadius, InvalidInput, UnsupportedQuad };
enum class ParticleDofGeometryRejection { None, InvalidInput, Depth, Uv, Degenerate, NonAffine, Count };
using ParticleDofGeometryRejections =
    std::array<uint32_t, size_t(ParticleDofGeometryRejection::Count)>;

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
    float radius, NativeParticleVertex (&expanded)[4], ParticleDofQuad& metadata,
    ParticleDofGeometryRejection* rejection = nullptr) {
    std::memcpy(expanded, original, sizeof(original));
    metadata = {};
    if (rejection) *rejection = ParticleDofGeometryRejection::None;
    const auto reject = [&](ParticleDofGeometryRejection why) {
        if (rejection) *rejection = why;
        return why == ParticleDofGeometryRejection::InvalidInput ?
            ParticleDofGeometryResult::InvalidInput : ParticleDofGeometryResult::UnsupportedQuad;
    };
    if (!std::isfinite(radius) || radius < 0) return reject(ParticleDofGeometryRejection::InvalidInput);
    for (const auto& v : original) {
        for (float x : v.position) if (!std::isfinite(x)) return reject(ParticleDofGeometryRejection::InvalidInput);
        for (float x : v.uv) if (!std::isfinite(x)) return reject(ParticleDofGeometryRejection::InvalidInput);
    }
    if (radius == 0) return ParticleDofGeometryResult::ZeroRadius;
    for (float x : p.c97) if (!std::isfinite(x)) return reject(ParticleDofGeometryRejection::InvalidInput);
    for (float x : p.c98) if (!std::isfinite(x)) return reject(ParticleDofGeometryRejection::InvalidInput);
    for (int j = 0; j < 2; ++j)
        if (!std::isfinite(p.guest_size[j]) || p.guest_size[j] <= 0 ||
            !std::isfinite(p.globals.target[j]) || p.globals.target[j] <= 0 || p.c98[j] == 0)
            return reject(ParticleDofGeometryRejection::InvalidInput);
    const double projected_w = double(original[0].position[3]) * p.c98[3] + p.c97[3];
    constexpr double float_max = std::numeric_limits<float>::max();
    if (!std::isfinite(projected_w) || std::abs(projected_w) > float_max)
        return reject(ParticleDofGeometryRejection::InvalidInput);
    if (original[0].position[2] <= 0 || projected_w <= 0)
        return reject(ParticleDofGeometryRejection::Depth);
    for (int k = 1; k < 4; ++k)
        if (original[k].position[2] != original[0].position[2] ||
            original[k].position[3] != original[0].position[3])
            return reject(ParticleDofGeometryRejection::Depth);
    const float u0 = original[0].uv[0], v0 = original[0].uv[1];
    const float u1 = original[3].uv[0], v1 = original[1].uv[1];
    if (u0 == u1 || v0 == v1 || original[1].uv[0] != u0 || original[3].uv[1] != v0 ||
        original[2].uv[0] != u1 || original[2].uv[1] != v1 ||
        std::min(u0, u1) != 0 || std::max(u0, u1) != 1 ||
        std::min(v0, v1) != 0 || std::max(v0, v1) != 1)
        return reject(ParticleDofGeometryRejection::Uv);
    double screen[4][2];
    for (int k = 0; k < 4; ++k) for (int j = 0; j < 2; ++j) {
        // Keep the native affine projection, without rounding tiny edges at a large screen offset.
        const float position = original[k].position[j];
        const double scale = double(p.globals.target[j]) / p.guest_size[j];
        const double guest = double(position) * p.c98[j] + p.c97[j];
        screen[k][j] = guest * scale;
        // Double precision must not admit a projection that overflows the native float shader.
        if (!std::isfinite(screen[k][j]) || std::abs(double(position) * p.c98[j]) > float_max ||
            std::abs(guest) > float_max || scale > float_max || std::abs(screen[k][j]) > float_max)
            return reject(ParticleDofGeometryRejection::InvalidInput);
    }
    const double ux = screen[3][0] - screen[0][0], uy = screen[3][1] - screen[0][1];
    const double vx = screen[1][0] - screen[0][0], vy = screen[1][1] - screen[0][1];
    const double lu = std::hypot(ux, uy), lv = std::hypot(vx, vy), det = ux * vy - uy * vx;
    if (!std::isfinite(det) || lu <= 1e-4 || lv <= 1e-4 || std::abs(det) <= lu * lv * 1e-6)
        return reject(ParticleDofGeometryRejection::Degenerate);
    for (int j = 0; j < 2; ++j) {
        const double residual = std::abs(screen[2][j] - screen[1][j] - screen[3][j] + screen[0][j]);
        const double tolerance = std::min(1.0 / 64, std::max(1e-4, (lu + lv) * 1e-5));
        if (residual <= tolerance) continue;
        if (residual > 1.0 / 64) return reject(ParticleDofGeometryRejection::NonAffine);
        // The eight native emitters add paired +/- offsets to a float center. Permit only their
        // input-rounding residual, capped at 1/64 pixel even for extreme coordinates. Ordinary
        // affine quads take the fast path above without calculating eight neighboring floats.
        double quantization = 0;
        for (const auto& v : original) {
            const float position = v.position[j];
            const double ulp = std::max(
                double(std::nextafter(position, std::numeric_limits<float>::infinity())) - position,
                double(position) - std::nextafter(position, -std::numeric_limits<float>::infinity()));
            quantization += .5 * ulp;
        }
        quantization *= std::abs(double(p.c98[j]) * p.globals.target[j] / p.guest_size[j]);
        if (!std::isfinite(quantization)) return reject(ParticleDofGeometryRejection::InvalidInput);
        if (residual > std::min(1.0 / 64, quantization))
            return reject(ParticleDofGeometryRejection::NonAffine);
    }
    // Row lengths of the inverse screen Jacobian bound a circular kernel, including rotated/skewed quads
    // and targets with different horizontal and vertical scale factors.
    const double pu = radius * lv / std::abs(det), pv = radius * lu / std::abs(det);
    if (!std::isfinite(pu) || !std::isfinite(pv)) return reject(ParticleDofGeometryRejection::InvalidInput);
    const double local[4][2] = {{-pu, -pv}, {-pu, 1 + pv}, {1 + pu, 1 + pv}, {1 + pu, -pv}};
    for (int k = 0; k < 4; ++k) {
        for (int j = 0; j < 2; ++j) {
            expanded[k].position[j] = float(double(original[0].position[j]) +
                local[k][0] * (double(original[3].position[j]) - original[0].position[j]) +
                local[k][1] * (double(original[1].position[j]) - original[0].position[j]));
            if (!std::isfinite(expanded[k].position[j])) {
                std::memcpy(expanded, original, sizeof(original));
                return reject(ParticleDofGeometryRejection::InvalidInput);
            }
        }
        expanded[k].uv[0] = float(u0 + local[k][0] * (u1 - u0));
        expanded[k].uv[1] = float(v0 + local[k][1] * (v1 - v0));
        if (!std::isfinite(expanded[k].uv[0]) || !std::isfinite(expanded[k].uv[1])) {
            std::memcpy(expanded, original, sizeof(original));
            return reject(ParticleDofGeometryRejection::InvalidInput);
        }
    }
    metadata.uv_bounds[0] = u0; metadata.uv_bounds[1] = v0;
    metadata.uv_bounds[2] = u1; metadata.uv_bounds[3] = v1;
    for (int k = 0; k < 4; ++k) metadata.argb[k] = original[k].argb;
    return ParticleDofGeometryResult::Ready;
}

inline bool BuildParticleDofGeometry(std::span<const NativeParticleVertex> vertices,
    const ParticleDofGeometryParams& p, std::vector<NativeParticleVertex>& expanded,
    std::vector<ParticleDofQuad>& metadata, ParticleDofGeometryStats& stats,
    ParticleDofGeometryRejections* rejections = nullptr) {
    stats = {};
    if (rejections) rejections->fill(0);
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
        ParticleDofGeometryRejection rejection;
        const auto result = ExpandParticleDofQuad(input, p, radius, output, metadata[k], &rejection);
        if (result == ParticleDofGeometryResult::Ready) {
            std::memcpy(expanded.data() + k * 4, output, sizeof(output));
            ++stats.active;
        } else if (result == ParticleDofGeometryResult::ZeroRadius) ++stats.focused;
        else {
            ++stats.invalid;
            if (rejections) ++(*rejections)[size_t(rejection)];
        }
    }
    return stats.active != 0;
}

} // namespace gpu
