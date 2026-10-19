#pragma once

#include <array>
#include <bit>
#include <limits>

#include "particle_dof_atlas.h"

namespace gpu {

enum class ParticleSharedReject { None, Samples, Projection, Count, Mapping, Depth, Bounds, Overlap, Total };
struct ParticleSharedCoverage {
    ParticleSharedReject rejection = ParticleSharedReject::Bounds;
    std::array<int32_t, 4> bounds{}; // native-target pixels, exclusive right/bottom
    std::array<NativeParticleVertex, 4> vertices{};
    double sum_area = 0, union_area = 0;
    explicit operator bool() const { return rejection == ParticleSharedReject::None; }
};

// Cost admission only; never merges/reorders the page's native composites. Every
// covered pixel still receives its original material and ordered blend operation.
inline ParticleSharedCoverage BuildParticleSharedCoverage(std::span<const NativeParticleVertex> vertices,
    std::span<const ParticleDofQuad> quads, const ParticleDofGeometryParams& p,
    ParticleDofAtlasPage page, uint32_t samples, bool verified_projection) {
    ParticleSharedCoverage out;
    const auto reject = [&](ParticleSharedReject why) { out.rejection = why; return out; };
    if (samples != 2) return reject(ParticleSharedReject::Samples);
    if (!verified_projection || !(p.globals.projection[2] > 0)) return reject(ParticleSharedReject::Projection);
    if (page.quad_count < 4 || page.first_quad > quads.size() ||
        page.quad_count > quads.size() - page.first_quad || vertices.size() != quads.size() * 4)
        return reject(ParticleSharedReject::Count);
    double clip_lo[2], clip_hi[2];
    for (uint32_t j = 0; j < 2; ++j) {
        if (!std::isfinite(p.c97[j]) || !std::isfinite(p.c98[j]) || p.c98[j] == 0 ||
            !std::isfinite(p.guest_size[j]) || p.guest_size[j] <= 0 ||
            !std::isfinite(p.globals.target[j]) || p.globals.target[j] <= 0 || p.globals.target[j] > 16384 ||
            !std::isfinite(p.globals.viewport[j]) || !std::isfinite(p.globals.viewport[j + 2])) return out;
        clip_lo[j] = std::max(0.0, double(p.globals.viewport[j]));
        clip_hi[j] = std::min(double(p.globals.target[j]), double(p.globals.viewport[j]) + p.globals.viewport[j + 2]);
        if (clip_lo[j] >= clip_hi[j]) return out;
    }
    const auto& representative = vertices[size_t(page.first_quad) * 4];
    const float clip_w = representative.position[3] * p.c98[3] + p.c97[3];
    if (!std::isfinite(representative.position[2]) || representative.position[2] <= 0 ||
        !std::isfinite(clip_w) || clip_w <= 0) return reject(ParticleSharedReject::Depth);
    const uint32_t z = std::bit_cast<uint32_t>(representative.position[2]);
    const uint32_t w = std::bit_cast<uint32_t>(representative.position[3]);
    out.bounds = {16384, 16384, 0, 0};
    for (uint32_t q = page.first_quad; q < page.first_quad + page.quad_count; ++q) {
        const auto& map = quads[q];
        for (float coordinate : map.uv_bounds)
            if (!std::isfinite(coordinate)) return reject(ParticleSharedReject::Mapping);
        if (!(map.atlas_scale > 0) || !std::isfinite(map.atlas_scale) ||
            (map.atlas_size & 0xffffu) <= 4 || (map.atlas_size >> 16) <= 4 ||
            map.uv_bounds[0] == map.uv_bounds[2] || map.uv_bounds[1] == map.uv_bounds[3])
            return reject(ParticleSharedReject::Mapping);
        const float radius = ParticleDofRadius(1.0f / vertices[size_t(q) * 4].position[2], p.globals);
        if (!std::isfinite(radius) || radius <= 0) return reject(ParticleSharedReject::Mapping);
        double lo[2]{}, hi[2]{};
        for (uint32_t k = 0; k < 4; ++k) {
            const auto& v = vertices[size_t(q) * 4 + k];
            if (std::bit_cast<uint32_t>(v.position[2]) != z || std::bit_cast<uint32_t>(v.position[3]) != w)
                return reject(ParticleSharedReject::Depth);
            for (uint32_t j = 0; j < 2; ++j) {
                const float pixel = (v.position[j] * p.c98[j] + p.c97[j]) * (p.globals.target[j] / p.guest_size[j]);
                if (!std::isfinite(pixel)) return reject(ParticleSharedReject::Bounds);
                if (!k) lo[j] = hi[j] = pixel;
                else { lo[j] = std::min(lo[j], double(pixel)); hi[j] = std::max(hi[j], double(pixel)); }
            }
        }
        int32_t box[4];
        for (uint32_t j = 0; j < 2; ++j) {
            const double begin = std::max(lo[j], clip_lo[j]), end = std::min(hi[j], clip_hi[j]);
            if (begin >= end) return reject(ParticleSharedReject::Bounds);
            box[j] = int32_t(std::floor(begin)); box[j + 2] = int32_t(std::ceil(end));
            out.bounds[j] = std::min(out.bounds[j], box[j]);
            out.bounds[j + 2] = std::max(out.bounds[j + 2], box[j + 2]);
        }
        out.sum_area += double(box[2] - box[0]) * (box[3] - box[1]);
    }
    out.union_area = double(out.bounds[2] - out.bounds[0]) * (out.bounds[3] - out.bounds[1]);
    if (out.sum_area < 3 * out.union_area) return reject(ParticleSharedReject::Overlap);
    // One extra pixel beyond each side makes the synthetic rectangle conservative
    // under native float projection/half-pixel rounding; the union scissor limits work.
    for (uint32_t k = 0; k < 4; ++k) {
        out.vertices[k] = representative;
        for (uint32_t j = 0; j < 2; ++j) {
            const bool high = j == 0 ? k >= 2 : (k == 1 || k == 2);
            const double pixel = out.bounds[j + (high ? 2 : 0)] + (high ? 1 : -1);
            const double native = (pixel * p.guest_size[j] / p.globals.target[j] - p.c97[j]) / p.c98[j];
            if (!std::isfinite(native) || std::abs(native) > std::numeric_limits<float>::max()) return reject(ParticleSharedReject::Bounds);
            out.vertices[k].position[j] = float(native);
        }
    }
    out.rejection = ParticleSharedReject::None;
    return out;
}

} // namespace gpu
