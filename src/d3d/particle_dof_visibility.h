#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "particle_dof_atlas.h"

namespace gpu {

struct ParticleDofVisibilityMap {
    uint32_t atlas_xy = 0xffffffffu;
    uint32_t screen_xy = 0;
};

struct ParticleDofVisibilityPage {
    uint32_t first_quad = 0;
    uint32_t quad_count = 0;
    bool cached = false;
};

static_assert(sizeof(ParticleDofVisibilityMap) == 8);

// Color pages must describe ordered, nonoverlapping ranges. Each remains a separate
// packing domain; malformed ranges are clipped to available quads and use fallback.
// Maps hold uint16 x/y pairs. Visibility cells include the full native-pixel halo,
// independently of material alpha; an unmapped quad keeps the existing composite.
inline std::vector<ParticleDofVisibilityPage> BuildParticleDofVisibilityPages(
    std::span<const ParticleDofAtlasPage> color_pages, std::span<const ParticleDofQuad> quads,
    const ParticleDofGlobals& globals, uint32_t cache_extent, uint32_t halo,
    std::vector<ParticleDofVisibilityMap>& maps) {
    maps.assign(quads.size(), ParticleDofVisibilityMap{});
    std::vector<ParticleDofVisibilityPage> pages;
    bool usable = cache_extent > 0 && cache_extent <= 65535;
    for (float x : globals.optics) usable &= std::isfinite(x);
    const float max_radius = globals.optics[1] * std::max(globals.optics[2], globals.optics[3]);
    usable &= globals.optics[0] > 0 && globals.optics[1] > 0 &&
              globals.optics[2] >= 0 && globals.optics[3] >= 0 &&
              std::isfinite(max_radius) && max_radius > 0 &&
              std::ceil(double(max_radius)) + 2.0 <= double(halo);
    uint32_t target[2]{};
    for (unsigned j = 0; j < 2; ++j) {
        const float size = globals.target[j];
        if (!std::isfinite(size) || size <= 0 || size > 65535 || std::floor(size) != size)
            usable = false;
        else target[j] = uint32_t(size);
    }

    for (const auto& color_page : color_pages) {
        const size_t first = std::min(size_t(color_page.first_quad), quads.size());
        const size_t count = std::min(size_t(color_page.quad_count), quads.size() - first);
        if (!count) continue;
        if (!usable || first != color_page.first_quad || count != color_page.quad_count) {
            pages.push_back({uint32_t(first), uint32_t(count), false});
            continue;
        }
        const size_t page_begin = pages.size();
        uint32_t cursor_x = 0, cursor_y = 0, row_height = 0;
        for (size_t q = first; q < first + count; ++q) {
            const auto& quad = quads[q];
            bool valid = quad.atlas_scale == 1.0f || quad.atlas_scale == 0.5f;
            for (float x : quad.uv_bounds) valid &= std::isfinite(x);
            valid &= quad.uv_bounds[0] != quad.uv_bounds[2] &&
                     quad.uv_bounds[1] != quad.uv_bounds[3];
            const uint32_t color_size[2] = {quad.atlas_size & 0xffffu, quad.atlas_size >> 16};
            const uint32_t color_origin[2] = {quad.atlas_xy & 0xffffu, quad.atlas_xy >> 16};
            const uint32_t source[2] = {quad.screen_xy & 0xffffu, quad.screen_xy >> 16};
            uint32_t cache_first[2]{}, cell[2]{}, receiver[2]{};
            for (unsigned j = 0; j < 2 && valid; ++j) {
                if (color_size[j] <= 2 * kParticleDofAtlasGuard ||
                    color_origin[j] + color_size[j] > 65535 || source[j] >= target[j]) {
                    valid = false;
                    break;
                }
                const uint32_t content = (color_size[j] - 2 * kParticleDofAtlasGuard) *
                                         (quad.atlas_scale == 0.5f ? 2u : 1u);
                receiver[j] = std::min(content, target[j] - source[j]);
                cache_first[j] = source[j] > halo ? source[j] - halo : 0;
                const uint32_t end = uint32_t(std::min(uint64_t(target[j]),
                                                     uint64_t(source[j]) + content + halo));
                cell[j] = end - cache_first[j];
                valid &= cell[j] > 0 && cell[j] <= cache_extent;
            }
            valid &= uint64_t(receiver[0]) * receiver[1] >= 4096;
            if (!valid) {
                if (pages.size() == page_begin || pages.back().cached)
                    pages.push_back({uint32_t(q), 1, false});
                else ++pages.back().quad_count;
                cursor_x = cursor_y = row_height = 0;
                continue;
            }

            uint32_t x = cursor_x, y = cursor_y, height = row_height;
            if (x + cell[0] > cache_extent) {
                x = 0;
                y += height;
                height = 0;
            }
            const bool overflow = y + cell[1] > cache_extent;
            if (overflow) x = y = height = 0;
            if (pages.size() == page_begin || !pages.back().cached || overflow)
                pages.push_back({uint32_t(q), 1, true});
            else ++pages.back().quad_count;
            maps[q] = {x | (y << 16), cache_first[0] | (cache_first[1] << 16)};
            cursor_x = x + cell[0];
            cursor_y = y;
            row_height = std::max(height, cell[1]);
        }
    }
    return pages;
}

} // namespace gpu
