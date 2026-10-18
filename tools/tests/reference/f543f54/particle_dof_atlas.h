#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "particle_dof_geometry.h"

namespace gpu {

inline constexpr uint32_t kParticleDofAtlasGuard = 2;

struct ParticleDofAtlasPage {
    uint32_t first_quad = 0;
    uint32_t quad_count = 0;
};

// Packed metadata uses low 16 bits for x/width and high 16 bits for y/height.
// atlas_xy is the WHOLE cell origin, atlas_size includes both transparent guards.
// Content starts at atlas_xy + (2,2); screen_xy is its full-target pixel origin.
// atlas_scale maps full-target pixels to atlas texels. Zero retains the existing shader path.
inline std::vector<ParticleDofAtlasPage> BuildParticleDofAtlasPages(
    std::span<const NativeParticleVertex> expanded, const ParticleDofGeometryParams& p,
    std::span<ParticleDofQuad> metadata, uint32_t pageWidth = 4096, uint32_t pageHeight = 4096) {
    for (auto& m : metadata) {
        m.atlas_xy = m.screen_xy = m.atlas_size = 0;
        m.atlas_scale = 0;
    }
    if (metadata.empty()) return {};
    // The native uploader is bounded to 1024 quads. Even malformed parameters retain a draw
    // chunk covering every supplied quad; the caller must not silently lose a fallback draw.
    std::vector<ParticleDofAtlasPage> pages{{0, uint32_t(metadata.size())}};
    if (metadata.size() > kParticleDofMaxQuads || expanded.size() != metadata.size() * 4 ||
        pageWidth <= 2 * kParticleDofAtlasGuard || pageHeight <= 2 * kParticleDofAtlasGuard ||
        pageWidth > 65535 || pageHeight > 65535) return pages;
    for (float x : p.c97) if (!std::isfinite(x)) return pages;
    for (float x : p.c98) if (!std::isfinite(x)) return pages;
    for (float x : p.globals.optics) if (!std::isfinite(x)) return pages;
    for (float x : p.globals.viewport) if (!std::isfinite(x)) return pages;
    if (p.globals.optics[0] <= 0 || p.globals.optics[1] < 0 || p.globals.optics[2] < 0 ||
        p.globals.optics[3] < 0 || p.globals.viewport[2] <= 0 || p.globals.viewport[3] <= 0) return pages;
    double clipMin[2], clipMax[2];
    for (unsigned j = 0; j < 2; ++j) {
        if (!std::isfinite(p.guest_size[j]) || p.guest_size[j] <= 0 || p.c98[j] == 0 ||
            !std::isfinite(p.globals.target[j]) || p.globals.target[j] <= 0 ||
            p.globals.target[j] > 65535) return pages;
        clipMin[j] = std::max(0.0, double(p.globals.viewport[j]));
        clipMax[j] = std::min(double(p.globals.target[j]),
                            double(p.globals.viewport[j]) + double(p.globals.viewport[j + 2]));
        if (clipMin[j] >= clipMax[j]) return pages;
    }

    uint32_t cursorX = 0, cursorY = 0, rowHeight = 0;
    for (uint32_t q = 0; q < uint32_t(metadata.size()); ++q) {
        auto& m = metadata[q];
        bool valid = true;
        for (float x : m.uv_bounds) valid &= std::isfinite(x);
        if (!valid || m.uv_bounds[0] == m.uv_bounds[2] || m.uv_bounds[1] == m.uv_bounds[3]) continue;
        const auto vertices = expanded.subspan(size_t(q) * 4, 4);
        const float viewZ = 1.0f / vertices[0].position[2];
        if (!std::isfinite(viewZ) || viewZ <= 0) continue;
        const float radius = ParticleDofRadius(viewZ, p.globals);
        if (!std::isfinite(radius) || radius <= 0) continue;
        // Half resolution is reserved for apertures wide enough to tolerate the reduced sampling grid.
        const float scale = radius >= 4.0f ? 0.5f : 1.0f;
        double lo[2]{}, hi[2]{};
        for (unsigned k = 0; k < 4; ++k) {
            const auto& v = vertices[k];
            for (float x : v.position) valid &= std::isfinite(x);
            for (float x : v.uv) valid &= std::isfinite(x);
            if (v.position[2] != vertices[0].position[2] ||
                v.position[3] != vertices[0].position[3]) valid = false;
            for (unsigned j = 0; j < 2; ++j) {
                // Match ExpandParticleDofQuad: native affine projection, with no divide by W.
                const float guest = v.position[j] * p.c98[j] + p.c97[j];
                const float screen = guest * (p.globals.target[j] / p.guest_size[j]);
                if (!std::isfinite(screen)) valid = false;
                if (!k) lo[j] = hi[j] = screen;
                else { lo[j] = std::min(lo[j], double(screen)); hi[j] = std::max(hi[j], double(screen)); }
            }
        }
        const float clipW = vertices[0].position[3] * p.c98[3] + p.c97[3];
        if (!valid || !std::isfinite(clipW) || clipW <= 0) continue;
        uint32_t origin[2]{}, cell[2]{};
        for (unsigned j = 0; j < 2; ++j) {
            const double begin = std::max(lo[j], clipMin[j]);
            const double end = std::min(hi[j], clipMax[j]);
            if (begin >= end) { valid = false; break; }
            const double pixelBegin = std::floor(begin), pixelEnd = std::ceil(end);
            const double extent = pixelEnd - pixelBegin;
            const double cellSize = std::ceil(extent * scale) + 2 * kParticleDofAtlasGuard;
            if (pixelBegin < 0 || pixelBegin > 65535 || !std::isfinite(cellSize) || cellSize > 65535) {
                valid = false; break;
            }
            origin[j] = uint32_t(pixelBegin);
            cell[j] = uint32_t(cellSize);
        }
        if (!valid || cell[0] > pageWidth || cell[1] > pageHeight) continue;

        uint32_t nextX = cursorX, nextY = cursorY, nextRowHeight = rowHeight;
        if (nextX + cell[0] > pageWidth) {
            nextX = 0;
            nextY += nextRowHeight;
            nextRowHeight = 0;
        }
        if (nextY + cell[1] > pageHeight) {
            // Split before the overflowing quad. Earlier inactive/fallback quads remain in the
            // preceding chunk, and this quad is retried at the new page origin without reordering.
            pages.back().quad_count = q - pages.back().first_quad;
            pages.push_back({q, uint32_t(metadata.size()) - q});
            nextX = nextY = nextRowHeight = 0;
        }
        m.atlas_xy = nextX | (nextY << 16);
        m.screen_xy = origin[0] | (origin[1] << 16);
        m.atlas_size = cell[0] | (cell[1] << 16);
        m.atlas_scale = scale;
        cursorX = nextX + cell[0];
        cursorY = nextY;
        rowHeight = std::max(nextRowHeight, cell[1]);
    }
    return pages;
}

} // namespace gpu
