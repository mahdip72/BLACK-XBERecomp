// CPU-only checks. Actual shader equivalence and timing belong to the WARP/hardware suites.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>
#include "d3d/particle_dof_shared_coverage.h"
#include "d3d/particle_dof_visibility.h"

namespace {
constexpr uint32_t kMaximum = 0xffffffu, kInvalid = 0xffffffffu;
struct Bounds { uint32_t minimum, maximum; };
void Check(bool okay, const char* why) { if (!okay) throw std::runtime_error(why); }
bool Pass(uint32_t q, uint32_t depth, bool le) { return depth <= kMaximum && (le ? q <= depth : q < depth); }
float Raw2(uint32_t a, uint32_t b, uint32_t q, bool le) { return (float(Pass(q, a, le)) + float(Pass(q, b, le))) / 2; }
float Shortcut2(Bounds bounds, uint32_t a, uint32_t b, uint32_t q, bool le) {
    if (bounds.minimum <= bounds.maximum && bounds.maximum <= kMaximum) {
        if (le ? q <= bounds.minimum : q < bounds.minimum) return 1;
        if (le ? q > bounds.maximum : q >= bounds.maximum) return 0;
        return .5f;
    }
    return Raw2(a, b, q, le);
}
void CheckPair(uint32_t a, uint32_t b) {
    const Bounds bounds = a == kInvalid || b == kInvalid ? Bounds{kInvalid, 0} : Bounds{std::min(a, b), std::max(a, b)};
    for (uint32_t q : {0u, a, b, a ? a - 1 : 0u, b ? b - 1 : 0u,
                       std::min(a + 1, kMaximum), std::min(b + 1, kMaximum), kMaximum}) {
        if (q > kMaximum) continue;
        for (bool le : {false, true}) {
            const float raw = Raw2(a, b, q, le);
            Check(raw == Shortcut2(bounds, a, b, q, le), "mixed2 equality mismatch");
            Check(raw == Shortcut2({kInvalid, 0}, a, b, q, le), "invalid sentinel did not retain raw fallback");
            Check(raw == Shortcut2({0, kInvalid}, a, b, q, le), "out-of-range bounds did not retain raw fallback");
        }
    }
}
void TestTwoSamples() {
    const std::array<uint32_t, 10> codes{0, 1, 2, 127, 128, 0x7fffff, 0x800000, kMaximum - 1, kMaximum, kInvalid};
    for (auto a : codes) for (auto b : codes) CheckPair(a, b);
    uint32_t state = 0x913fe523;
    for (unsigned i = 0; i < 65536; ++i) {
        state = state * 1664525u + 1013904223u; const uint32_t a = state & kMaximum;
        state = state * 1664525u + 1013904223u; const uint32_t b = state & kMaximum;
        CheckPair(a, b); CheckPair(a, std::min(a + 1, kMaximum));
    }
}

struct Fixture {
    gpu::ParticleDofGeometryParams p{};
    std::vector<gpu::NativeParticleVertex> vertices;
    std::vector<gpu::ParticleDofQuad> quads;
    gpu::ParticleDofAtlasPage page{};
    explicit Fixture(uint32_t count = 4) : vertices(size_t(count) * 4), quads(count), page{0, count} {
        p.globals.optics[0] = 5; p.globals.optics[1] = 12; p.globals.optics[2] = 1; p.globals.optics[3] = .45f;
        p.globals.target[0] = p.globals.viewport[2] = p.guest_size[0] = 128;
        p.globals.target[1] = p.globals.viewport[3] = p.guest_size[1] = 96;
        p.globals.target[2] = float(count);
        p.globals.projection[0] = -1; p.globals.projection[1] = p.globals.projection[2] = 1;
        p.c98[0] = p.c98[1] = p.c98[3] = 1;
        for (uint32_t q = 0; q < count; ++q) {
            quads[q].uv_bounds[2] = quads[q].uv_bounds[3] = 1;
            for (uint32_t k = 0; k < 4; ++k) {
                auto& v = vertices[q * 4 + k];
                v.position[2] = 1.f / 12; v.position[3] = 1; v.argb = 0xffffffff;
                v.uv[0] = k >= 2 ? 1.f : 0.f; v.uv[1] = k == 1 || k == 2 ? 1.f : 0.f;
            }
            Rect(q, 20, 15, 70, 65);
        }
        Map();
    }
    void Rect(uint32_t q, float left, float top, float right, float bottom) {
        for (uint32_t k = 0; k < 4; ++k) for (uint32_t axis = 0; axis < 2; ++axis) {
            const bool high = axis == 0 ? k >= 2 : k == 1 || k == 2;
            const float pixel = axis == 0 ? (high ? right : left) : (high ? bottom : top);
            vertices[q * 4 + k].position[axis] = (pixel * p.guest_size[axis] / p.globals.target[axis] - p.c97[axis]) / p.c98[axis];
        }
    }
    void Map() { (void)gpu::BuildParticleDofAtlasPages(vertices, p, quads); }
    gpu::ParticleSharedCoverage Plan(uint32_t samples = 2, bool projection = true) const {
        return gpu::BuildParticleSharedCoverage(vertices, quads, p, page, samples, projection);
    }
};
template<class Change> void Reject(Change change, gpu::ParticleSharedReject reason, const char* message) {
    Fixture f; change(f); const auto result = f.Plan();
    Check(!result && result.rejection == reason, message);
}
void CheckSynthetic(const Fixture& f, const gpu::ParticleSharedCoverage& result) {
    Check(bool(result), "expected shared coverage admission");
    const auto& original = f.vertices[size_t(f.page.first_quad) * 4];
    for (uint32_t k = 0; k < 4; ++k) {
        for (uint32_t axis = 2; axis < 4; ++axis)
            Check(std::bit_cast<uint32_t>(result.vertices[k].position[axis]) == std::bit_cast<uint32_t>(original.position[axis]),
                  "synthetic quad changed native depth/W bits");
        for (uint32_t axis = 0; axis < 2; ++axis) {
            const bool high = axis == 0 ? k >= 2 : k == 1 || k == 2;
            const double expected = result.bounds[axis + (high ? 2 : 0)] + (high ? 1 : -1);
            const float actual = (result.vertices[k].position[axis] * f.p.c98[axis] + f.p.c97[axis]) *
                (f.p.globals.target[axis] / f.p.guest_size[axis]);
            Check(std::abs(actual - expected) <= 1e-4, "synthetic rectangle lost conservative border");
        }
    }
}
void TestAdmission() {
    Fixture f; const auto oldVertices = f.vertices; const auto oldQuads = f.quads;
    auto plan = f.Plan(); CheckSynthetic(f, plan);
    Check(plan.bounds == std::array<int32_t, 4>{20, 15, 70, 65}, "page union bounds mismatch");
    Check(plan.sum_area == 10000 && plan.union_area == 2500, "cost admission areas mismatch");
    Check(std::memcmp(oldVertices.data(), f.vertices.data(), oldVertices.size() * sizeof(oldVertices[0])) == 0 &&
          std::memcmp(oldQuads.data(), f.quads.data(), oldQuads.size() * sizeof(oldQuads[0])) == 0, "admission mutated native data");
    for (auto samples : {1u, 4u, 8u}) Check(f.Plan(samples).rejection == gpu::ParticleSharedReject::Samples, "unsupported MSAA admitted");
    Check(f.Plan(2, false).rejection == gpu::ParticleSharedReject::Projection, "unknown provenance admitted");

    f = Fixture(); f.p.guest_size[0] = 256; f.p.guest_size[1] = 192;
    f.p.c98[0] = -2; f.p.c98[1] = .5f; f.p.c97[0] = 8; f.p.c97[1] = -4;
    f.p.globals.viewport[0] = 7.25f; f.p.globals.viewport[1] = 9.5f;
    f.p.globals.viewport[2] = 50.5f; f.p.globals.viewport[3] = 48.25f;
    for (uint32_t q = 0; q < 4; ++q) f.Rect(q, -10, -10, 100, 100);
    f.Map(); plan = f.Plan(); CheckSynthetic(f, plan);
    Check(plan.bounds == std::array<int32_t, 4>{7, 9, 58, 58}, "fractional viewport clipping mismatch");
    f = Fixture(); f.p.c97[3] = 1;
    for (auto& v : f.vertices) v.position[3] = 0;
    f.Map(); CheckSynthetic(f, f.Plan()); // Native rawW0 is valid when transformedW is positive.
    f = Fixture(6); f.page = {1, 4}; f.quads[0].atlas_scale = f.quads[5].atlas_scale = 0;
    for (uint32_t k = 0; k < 4; ++k) f.vertices[k].position[2] = .5f;
    CheckSynthetic(f, f.Plan()); // Unselected pages must not change admission or representative depth.
}
void TestRejections() {
    using R = gpu::ParticleSharedReject;
    Reject([](auto& f) { f.p.globals.projection[2] = 0; }, R::Projection, "disabled aware coverage admitted");
    Reject([](auto& f) { f.page.quad_count = 3; }, R::Count, "too few quads admitted");
    Reject([](auto& f) { f.page.first_quad = 5; }, R::Count, "out-of-range page admitted");
    Reject([](auto& f) { f.vertices.pop_back(); }, R::Count, "truncated vertices admitted");
    Reject([](auto& f) { f.quads[2].atlas_scale = 0; }, R::Mapping, "direct-fallback quad admitted");
    Reject([](auto& f) { f.quads[1].atlas_scale = std::numeric_limits<float>::quiet_NaN(); }, R::Mapping, "invalid atlas scale admitted");
    Reject([](auto& f) { f.quads[1].atlas_size = 4u | (16u << 16); }, R::Mapping, "guard-only atlas cell admitted");
    Reject([](auto& f) { f.quads[1].uv_bounds[2] = 0; }, R::Mapping, "unsupported UV mapping admitted");
    Reject([](auto& f) { f.p.globals.optics[0] = 12; }, R::Mapping, "focused quad admitted");
    Reject([](auto& f) { f.vertices[7].position[2] = std::nextafter(f.vertices[7].position[2], 1.f); }, R::Depth, "depth ULP mismatch admitted");
    Reject([](auto& f) { f.vertices[4].position[3] = std::nextafter(1.f, 2.f); }, R::Depth, "W ULP mismatch admitted");
    Reject([](auto& f) { f.p.c97[3] = 1; for (auto& v : f.vertices) v.position[3] = 0; f.vertices[7].position[3] = -0.f; },
           R::Depth, "nonidentical signed-zero W admitted");
    Reject([](auto& f) { f.p.c98[3] = -1; }, R::Depth, "negative projected W admitted");
    Reject([](auto& f) { f.vertices[7].position[0] = std::numeric_limits<float>::infinity(); }, R::Bounds, "nonfinite position admitted");
    Reject([](auto& f) { f.p.c98[0] = 0; }, R::Bounds, "noninvertible projection admitted");
    Reject([](auto& f) { f.p.globals.viewport[2] = 0; }, R::Bounds, "empty viewport admitted");
    Reject([](auto& f) { f.p.globals.target[0] = 16385; }, R::Bounds, "unsupported target admitted");
    Reject([](auto& f) { for (uint32_t q = 0; q < 4; ++q) f.Rect(q, -100, -100, -50, -50); }, R::Bounds, "clipped page admitted");
    Reject([](auto& f) { for (uint32_t q = 0; q < 4; ++q) f.Rect(q, float(q * 30), 20, float(q * 30 + 10), 40); },
           R::Overlap, "low-overlap page admitted");
}

struct VisibilityFixture {
    gpu::ParticleDofGlobals globals{};
    std::vector<gpu::ParticleDofQuad> quads;
    std::vector<gpu::ParticleDofAtlasPage> color_pages;
    std::vector<gpu::ParticleDofVisibilityMap> maps;
    uint32_t extent = 256, halo = 14;
    explicit VisibilityFixture(uint32_t count = 1) : quads(count), color_pages{{0, count}} {
        globals.optics[0] = 5; globals.optics[1] = 12;
        globals.optics[2] = 1; globals.optics[3] = .45f;
        globals.target[0] = globals.viewport[2] = 513;
        globals.target[1] = globals.viewport[3] = 385;
        globals.target[2] = float(count);
        for (uint32_t q = 0; q < count; ++q) Rect(q, 32, 32, 64, 64);
    }
    void Rect(uint32_t q, uint32_t x, uint32_t y, uint32_t width, uint32_t height, float scale = 1) {
        auto& quad = quads[q];
        quad = {};
        quad.uv_bounds[2] = quad.uv_bounds[3] = 1;
        quad.screen_xy = x | (y << 16);
        quad.atlas_size = (uint32_t(std::ceil(width * scale)) + 4) |
                         ((uint32_t(std::ceil(height * scale)) + 4) << 16);
        quad.atlas_scale = scale;
    }
    std::vector<gpu::ParticleDofVisibilityPage> Plan() {
        return gpu::BuildParticleDofVisibilityPages(color_pages, quads, globals, extent, halo, maps);
    }
};

void CheckVisibilityPartition(const VisibilityFixture& f, std::span<const gpu::ParticleDofVisibilityPage> plan) {
    Check(f.maps.size() == f.quads.size(), "visibility map count mismatch");
    size_t cursor = 0;
    uint32_t next = 0;
    for (const auto& color : f.color_pages) {
        Check(color.first_quad == next, "invalid test color-page partition");
        const uint32_t end = color.first_quad + color.quad_count;
        while (next < end) {
            Check(cursor < plan.size(), "visibility plan omitted native quads");
            const auto& page = plan[cursor++];
            Check(page.first_quad == next && page.quad_count > 0 && page.quad_count <= end - next,
                  "visibility plan reordered quads or crossed a color-page boundary");
            for (uint32_t q = next; q < next + page.quad_count; ++q) {
                Check((f.maps[q].atlas_xy != kInvalid) == page.cached, "visibility map disagrees with page mode");
                if (!page.cached) Check(f.maps[q].screen_xy == 0, "fallback retained stale screen mapping");
            }
            next += page.quad_count;
        }
    }
    Check(cursor == plan.size() && next == f.quads.size(), "visibility plan duplicated or omitted quads");
}

// Expected screen rectangles are supplied independently, including the complete halo.
// Their translated atlas rectangles must fit and remain disjoint within each page.
void CheckVisibilityCells(const VisibilityFixture& f, std::span<const gpu::ParticleDofVisibilityPage> plan,
                          std::span<const std::array<uint32_t, 4>> boxes) {
    Check(boxes.size() == f.quads.size(), "invalid test halo rectangle count");
    for (const auto& page : plan) {
        if (!page.cached) continue;
        for (uint32_t q = page.first_quad; q < page.first_quad + page.quad_count; ++q) {
            const auto& box = boxes[q];
            const auto& map = f.maps[q];
            Check((map.screen_xy & 0xffffu) == box[0] && (map.screen_xy >> 16) == box[1],
                  "visibility cell lost its native-screen halo origin");
            const uint32_t x = map.atlas_xy & 0xffffu, y = map.atlas_xy >> 16;
            const uint32_t width = box[2] - box[0], height = box[3] - box[1];
            Check(width > 0 && height > 0 && x + width <= f.extent && y + height <= f.extent,
                  "visibility halo extends outside its atlas page");
            for (uint32_t other = page.first_quad; other < q; ++other) {
                const uint32_t ox = f.maps[other].atlas_xy & 0xffffu, oy = f.maps[other].atlas_xy >> 16;
                const auto& ob = boxes[other];
                Check(x + width <= ox || ox + ob[2] - ob[0] <= x ||
                      y + height <= oy || oy + ob[3] - ob[1] <= y,
                      "visibility cells overlap within one atlas page");
            }
        }
    }
}

void TestVisibilityCells() {
    VisibilityFixture f(6);
    f.Rect(0, 0, 0, 64, 64);
    f.Rect(1, 17, 19, 66, 70);
    f.Rect(2, 449, 321, 64, 64);
    f.Rect(3, 448, 320, 65, 65, .5f);
    f.Rect(4, 14, 14, 64, 64, .5f);
    f.Rect(5, 32, 32, 65, 67, .5f);
    f.quads[2].uv_bounds[0] = f.quads[2].uv_bounds[1] = 1;
    f.quads[2].uv_bounds[2] = f.quads[2].uv_bounds[3] = 0;
    const auto old_quads = f.quads;
    const auto old_globals = f.globals;
    const auto plan = f.Plan();
    CheckVisibilityPartition(f, plan);
    const std::array<std::array<uint32_t, 4>, 6> boxes{{
        {0, 0, 78, 78}, {3, 5, 97, 103}, {435, 307, 513, 385},
        {434, 306, 513, 385}, {0, 0, 92, 92}, {18, 18, 112, 114}
    }};
    CheckVisibilityCells(f, plan, boxes);
    for (const auto& page : plan) Check(page.cached, "eligible odd-target or half-scale cell rejected");
    Check(std::memcmp(old_quads.data(), f.quads.data(), old_quads.size() * sizeof(old_quads[0])) == 0 &&
          std::memcmp(&old_globals, &f.globals, sizeof(old_globals)) == 0, "visibility planner mutated native inputs");
    // All vertex colors are transparent: material alpha must not trim the visibility halo.
    for (const auto& map : f.maps) Check(map.atlas_xy != kInvalid, "transparent material removed visibility support");
}

void TestVisibilityPages() {
    VisibilityFixture f(12);
    f.extent = 184; // Four complete 92x92 cells fit exactly; the fifth needs a new page.
    f.color_pages = {{0, 8}, {8, 2}, {10, 2}};
    for (uint32_t q : {5u, 6u, 9u, 10u}) f.quads[q].atlas_scale = 0;
    const auto plan = f.Plan();
    CheckVisibilityPartition(f, plan);
    const std::array<gpu::ParticleDofVisibilityPage, 8> expected{{
        {0, 4, true}, {4, 1, true}, {5, 2, false}, {7, 1, true},
        {8, 1, true}, {9, 1, false}, {10, 1, false}, {11, 1, true}
    }};
    Check(plan.size() == expected.size(), "visibility chunk count mismatch");
    for (size_t i = 0; i < expected.size(); ++i)
        Check(plan[i].first_quad == expected[i].first_quad && plan[i].quad_count == expected[i].quad_count &&
              plan[i].cached == expected[i].cached, "visibility packing/fallback partition mismatch");
    const std::vector<std::array<uint32_t, 4>> boxes(f.quads.size(), {18, 18, 110, 110});
    CheckVisibilityCells(f, plan, boxes);
    for (uint32_t q : {0u, 4u, 7u, 8u, 11u})
        Check(f.maps[q].atlas_xy == 0, "visibility packing did not reset at a new cached chunk");

    for (auto& quad : f.quads) quad.atlas_scale = 0;
    const auto fallback = f.Plan(); // Reusing the map vector must clear every previous cached entry.
    CheckVisibilityPartition(f, fallback);
    Check(fallback.size() == f.color_pages.size(), "fallback chunks crossed original color pages");
    for (const auto& page : fallback) Check(!page.cached, "stale cached page survived replanning");
}

template<class Change> void RejectVisibility(Change change, const char* message) {
    VisibilityFixture f;
    change(f);
    const auto plan = f.Plan();
    CheckVisibilityPartition(f, plan);
    Check(plan.size() == 1 && !plan[0].cached && f.maps[0].atlas_xy == kInvalid, message);
}

void TestVisibilityRejections() {
    RejectVisibility([](auto& f) { f.quads[0].atlas_scale = 0; }, "unmapped visibility cell admitted");
    for (float scale : {.25f, std::nextafter(1.f, 2.f), std::numeric_limits<float>::quiet_NaN()})
        RejectVisibility([scale](auto& f) { f.quads[0].atlas_scale = scale; }, "unsupported visibility scale admitted");
    RejectVisibility([](auto& f) { f.quads[0].uv_bounds[2] = 0; }, "zero UV extent admitted to visibility cache");
    RejectVisibility([](auto& f) { f.quads[0].uv_bounds[1] = std::numeric_limits<float>::infinity(); }, "invalid UV admitted to visibility cache");
    RejectVisibility([](auto& f) { f.quads[0].atlas_size = 4u | (68u << 16); }, "guard-only visibility cell admitted");
    RejectVisibility([](auto& f) { f.quads[0].atlas_xy = 65535; }, "overflowing color mapping admitted");
    RejectVisibility([](auto& f) { f.Rect(0, 513, 32, 64, 64); }, "off-target visibility cell admitted");
    RejectVisibility([](auto& f) { f.Rect(0, 32, 32, 63, 65); }, "subthreshold visibility receiver admitted");
    RejectVisibility([](auto& f) { f.Rect(0, 450, 320, 128, 128); }, "receiver area counted off-target pixels");
    RejectVisibility([](auto& f) { f.extent = 128; f.Rect(0, 32, 32, 128, 64); }, "oversized visibility halo admitted");
    for (float size : {0.f, -1.f, 513.5f, 65536.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
        RejectVisibility([size](auto& f) { f.globals.target[0] = size; }, "invalid visibility target admitted");
    RejectVisibility([](auto& f) { f.globals.optics[0] = 0; }, "invalid visibility focus admitted");
    RejectVisibility([](auto& f) { f.globals.optics[1] = 0; }, "zero visibility radius admitted");
    RejectVisibility([](auto& f) { f.globals.optics[2] = -1; }, "negative visibility scale admitted");
    RejectVisibility([](auto& f) { f.globals.optics[2] = f.globals.optics[3] = 0; }, "zero visibility radius bound admitted");
    RejectVisibility([](auto& f) { f.globals.optics[3] = std::numeric_limits<float>::quiet_NaN(); }, "nonfinite visibility optics admitted");
    RejectVisibility([](auto& f) { f.globals.optics[1] = std::numeric_limits<float>::max(); f.globals.optics[2] = 2; },
                     "overflowing visibility radius admitted");
    RejectVisibility([](auto& f) { f.halo = 13; }, "insufficient visibility halo admitted");
    RejectVisibility([](auto& f) { f.halo = std::numeric_limits<uint32_t>::max(); }, "visibility halo arithmetic wrapped");
    for (uint32_t extent : {0u, 65536u})
        RejectVisibility([extent](auto& f) { f.extent = extent; }, "invalid visibility atlas dimensions admitted");

    Fixture focused(1);
    focused.Rect(0, 0, 0, 128, 96); focused.Map();
    const std::array color_pages{focused.page};
    std::vector<gpu::ParticleDofVisibilityMap> maps;
    auto plan = gpu::BuildParticleDofVisibilityPages(color_pages, focused.quads, focused.p.globals, 256, 14, maps);
    Check(plan.size() == 1 && plan[0].cached, "unfocused control missing visibility cache");
    focused.p.globals.optics[0] = 12; focused.Map();
    Check(focused.quads[0].atlas_scale == 0, "focused atlas control remained mapped");
    plan = gpu::BuildParticleDofVisibilityPages(color_pages, focused.quads, focused.p.globals, 256, 14, maps);
    Check(plan.size() == 1 && !plan[0].cached && maps[0].atlas_xy == kInvalid, "focused quad entered visibility cache");
}

void TestVisibilityMalformedPages() {
    VisibilityFixture f(3);
    f.color_pages = {{1, std::numeric_limits<uint32_t>::max()}};
    auto plan = f.Plan();
    Check(plan.size() == 1 && plan[0].first_quad == 1 && plan[0].quad_count == 2 && !plan[0].cached,
          "malformed visibility page was not clipped to fallback");
    for (const auto& map : f.maps) Check(map.atlas_xy == kInvalid && map.screen_xy == 0, "malformed page retained a cache map");
    f.color_pages = {{std::numeric_limits<uint32_t>::max(), 3}};
    Check(f.Plan().empty(), "out-of-range visibility page retained unavailable quads");
    f.quads.clear();
    Check(f.Plan().empty() && f.maps.empty(), "empty visibility input retained stale state");
}
} // namespace
int main() {
    try {
        TestTwoSamples(); TestAdmission(); TestRejections();
        TestVisibilityCells(); TestVisibilityPages(); TestVisibilityRejections(); TestVisibilityMalformedPages();
        std::puts("particle coverage CPU PASS: mixed2 equality/invalid; shared admission; visibility order, halos, packing, fallback");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "particle coverage CPU FAIL: %s\n", error.what()); return 1; }
}
