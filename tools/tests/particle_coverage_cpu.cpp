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
} // namespace
int main() {
    try {
        TestTwoSamples(); TestAdmission(); TestRejections();
        std::puts("particle coverage CPU PASS: mixed2 equality/invalid; shared admission, depth, mapping, viewport, overlap");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "particle coverage CPU FAIL: %s\n", error.what()); return 1; }
}
