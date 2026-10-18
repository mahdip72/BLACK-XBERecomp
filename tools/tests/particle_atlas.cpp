// CPU-only checks of the production atlas planner. No device or shader compiler is created.
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

#include "d3d/particle_dof_atlas.h"

namespace {
using gpu::NativeParticleVertex;
using gpu::ParticleDofAtlasPage;
using gpu::ParticleDofGeometryParams;
using gpu::ParticleDofQuad;

void Check(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
uint32_t X(uint32_t packed) { return packed & 65535; }
uint32_t Y(uint32_t packed) { return packed >> 16; }

ParticleDofGeometryParams Params(float width = 128, float height = 128, float aperture = 1) {
    ParticleDofGeometryParams p{};
    p.globals.optics[0] = 1;
    p.globals.optics[1] = aperture;
    p.globals.optics[2] = p.globals.optics[3] = 1;
    p.globals.target[0] = p.globals.viewport[2] = p.guest_size[0] = width;
    p.globals.target[1] = p.globals.viewport[3] = p.guest_size[1] = height;
    p.c98[0] = p.c98[1] = p.c98[3] = 1;
    return p;
}

std::array<NativeParticleVertex, 4> Quad(float left, float top, float right, float bottom, float w = 1) {
    return {NativeParticleVertex{{left, top, .2f, w}, 0x10203040, {0, 0}, 7},
            NativeParticleVertex{{left, bottom, .2f, w}, 0x50607080, {0, 1}, 8},
            NativeParticleVertex{{right, bottom, .2f, w}, 0x90A0B0C0, {1, 1}, 9},
            NativeParticleVertex{{right, top, .2f, w}, 0xD0E0F000, {1, 0}, 10}};
}

ParticleDofQuad Metadata() {
    ParticleDofQuad m{};
    m.uv_bounds[2] = m.uv_bounds[3] = 1;
    m.argb[0] = 0x10203040; m.argb[1] = 0x50607080;
    m.argb[2] = 0x90A0B0C0; m.argb[3] = 0xD0E0F000;
    return m;
}

void Append(std::vector<NativeParticleVertex>& vertices, const std::array<NativeParticleVertex, 4>& q) {
    vertices.insert(vertices.end(), q.begin(), q.end());
}

void Coverage(const std::vector<ParticleDofAtlasPage>& pages, uint32_t count) {
    uint32_t end = 0;
    for (const auto& page : pages) {
        Check(page.first_quad == end && page.quad_count > 0, "page chunks lost or reordered a quad");
        end += page.quad_count;
    }
    Check(end == count, "page chunks failed to cover the original draw");
}

void TestPackingAndOrder() {
    auto p = Params();
    std::vector<NativeParticleVertex> vertices;
    std::vector<ParticleDofQuad> metadata(9, Metadata());
    for (unsigned q = 0; q < metadata.size(); ++q) Append(vertices, Quad(0, 0, 4, 4));
    const auto before = vertices;
    auto pages = gpu::BuildParticleDofAtlasPages(vertices, p, metadata, 16, 16);
    const auto nativeMetadata = Metadata();
    Coverage(pages, 9);
    Check(pages.size() == 3 && pages[0].quad_count == 4 && pages[1].quad_count == 4 &&
          pages[2].quad_count == 1, "exact-fit rows or page overflow were packed incorrectly");
    for (unsigned q = 0; q < metadata.size(); ++q) {
        Check(X(metadata[q].atlas_xy) == (q % 2) * 8 && Y(metadata[q].atlas_xy) == ((q % 4) / 2) * 8,
              "cell order changed across rows/pages");
        Check(X(metadata[q].atlas_size) == 8 && Y(metadata[q].atlas_size) == 8 &&
              metadata[q].atlas_scale == 1, "whole-cell dimensions omitted transparent guard");
        Check(std::memcmp(metadata[q].uv_bounds, nativeMetadata.uv_bounds, 16) == 0 &&
              std::memcmp(metadata[q].argb, nativeMetadata.argb, 16) == 0,
              "packed tile changed the original UV/color metadata");
    }
    Check(std::memcmp(vertices.data(), before.data(), vertices.size() * sizeof(NativeParticleVertex)) == 0,
          "atlas packing changed expanded geometry");
    const auto first = metadata;
    pages = gpu::BuildParticleDofAtlasPages(vertices, p, metadata, 16, 16);
    Check(std::memcmp(first.data(), metadata.data(), metadata.size() * sizeof(ParticleDofQuad)) == 0,
          "packing was not deterministic on metadata reuse");

    vertices.clear(); metadata.assign(6, Metadata());
    for (unsigned q = 0; q < metadata.size(); ++q) Append(vertices, q == 4 ? Quad(0, 0, 100, 100) : Quad(0, 0, 6, 6));
    metadata[0].uv_bounds[2] = metadata[2].uv_bounds[2] = 0;
    pages = gpu::BuildParticleDofAtlasPages(vertices, p, metadata, 16, 16);
    Coverage(pages, 6);
    Check(pages.size() == 3 && pages[0].first_quad == 0 && pages[0].quad_count == 3 &&
          pages[1].first_quad == 3 && pages[1].quad_count == 2 && pages[2].first_quad == 5,
          "inactive or oversized quads disappeared at a page boundary");
    Check(metadata[0].atlas_scale == 0 && metadata[2].atlas_scale == 0 && metadata[4].atlas_scale == 0 &&
          metadata[1].atlas_scale == 1 && metadata[3].atlas_scale == 1 && metadata[5].atlas_scale == 1,
          "oversized fallback altered neighboring active tiles");
    for (auto& m : metadata) m.uv_bounds[2] = 0;
    pages = gpu::BuildParticleDofAtlasPages(vertices, p, metadata, 16, 16);
    Coverage(pages, 6);
    Check(pages.size() == 1, "fully inactive draw should retain a single native chunk");
}

void TestProjectionClippingAndScale() {
    auto p = Params(101, 77);
    p.globals.viewport[0] = 3.25f; p.globals.viewport[1] = 5.5f;
    p.globals.viewport[2] = 93.25f; p.globals.viewport[3] = 68.25f;
    auto q = Quad(-100, -100, 200, 200, 2.25f);
    std::vector<ParticleDofQuad> metadata(1, Metadata());
    auto pages = gpu::BuildParticleDofAtlasPages(q, p, metadata);
    Coverage(pages, 1);
    Check(X(metadata[0].screen_xy) == 3 && Y(metadata[0].screen_xy) == 5 &&
          X(metadata[0].atlas_size) == 98 && Y(metadata[0].atlas_size) == 73,
          "fractional viewport clipping did not conservatively align to target pixels");
    Check(metadata[0].atlas_scale == 1, "small aperture was downsampled");
    p.globals.optics[1] = 20;
    pages = gpu::BuildParticleDofAtlasPages(q, p, metadata);
    Check(metadata[0].atlas_scale == .5f && X(metadata[0].atlas_size) == 51 &&
          Y(metadata[0].atlas_size) == 39, "odd extents at half scale were rounded down");

    p = Params(101, 77);
    p.guest_size[0] = 50.5f; p.guest_size[1] = 154;
    p.c97[0] = 2; p.c97[1] = 4; p.c98[0] = 3; p.c98[1] = -2;
    q = Quad(1, 2, 5, 8, 2.25f);
    pages = gpu::BuildParticleDofAtlasPages(q, p, metadata);
    Check(metadata[0].atlas_scale == 0, "offscreen affine quad should not reserve a tile");
    p.c97[1] = 20;
    pages = gpu::BuildParticleDofAtlasPages(q, p, metadata);
    Check(X(metadata[0].screen_xy) == 10 && Y(metadata[0].screen_xy) == 2 &&
          X(metadata[0].atlas_size) == 28 && Y(metadata[0].atlas_size) == 10,
          "projection used perspective W or failed target/guest scaling");

    p = Params();
    q = Quad(0, 0, 1, 1, 3.5f);
    q[0].position[0] = 25; q[0].position[1] = 15;
    q[1].position[0] = 15; q[1].position[1] = 25;
    q[2].position[0] = 25; q[2].position[1] = 35;
    q[3].position[0] = 35; q[3].position[1] = 25;
    pages = gpu::BuildParticleDofAtlasPages(q, p, metadata);
    Check(X(metadata[0].screen_xy) == 15 && Y(metadata[0].screen_xy) == 15 &&
          X(metadata[0].atlas_size) == 24 && Y(metadata[0].atlas_size) == 24,
          "rotated affine quad did not use all four corners");
    p.globals.optics[1] = 3.999f / .77f;
    gpu::BuildParticleDofAtlasPages(q, p, metadata);
    Check(metadata[0].atlas_scale == 1, "aperture below four target pixels was downsampled");
    p.globals.optics[1] = 4.001f / .77f;
    gpu::BuildParticleDofAtlasPages(q, p, metadata);
    Check(metadata[0].atlas_scale == .5f, "wide aperture did not select the half-resolution tile");
}

void TestFallbacksAndUnchangedNativeData() {
    const auto base = Params();
    const auto source = Quad(10, 20, 30, 40);
    auto q = source;
    std::vector<ParticleDofQuad> metadata(1, Metadata());
    auto fallback = [&](const ParticleDofGeometryParams& p, uint32_t width = 4096, uint32_t height = 4096) {
        metadata[0].atlas_xy = metadata[0].atlas_size = metadata[0].screen_xy = ~0u;
        metadata[0].atlas_scale = 1;
        const auto native = metadata[0];
        auto pages = gpu::BuildParticleDofAtlasPages(q, p, metadata, width, height);
        Coverage(pages, 1);
        Check(metadata[0].atlas_scale == 0 && metadata[0].atlas_xy == 0 &&
              metadata[0].atlas_size == 0 && metadata[0].screen_xy == 0, "fallback retained stale atlas fields");
        Check(std::memcmp(metadata[0].uv_bounds, native.uv_bounds, sizeof(native.uv_bounds)) == 0 &&
              std::memcmp(metadata[0].argb, native.argb, sizeof(native.argb)) == 0,
              "fallback changed native UV/color metadata");
    };
    fallback(base, 4, 4096); fallback(base, 4096, 0); fallback(base, 65536, 4096);
    auto p = base; p.globals.target[0] = std::numeric_limits<float>::max(); fallback(p);
    p = base; p.globals.viewport[2] = INFINITY; fallback(p);
    p = base; p.globals.viewport[0] = 1000; fallback(p);
    p = base; p.guest_size[0] = 0; fallback(p);
    p = base; p.c98[0] = NAN; fallback(p);
    p = base; p.globals.optics[1] = 0; fallback(p);
    p = base; p.c98[0] = std::numeric_limits<float>::max(); fallback(p);
    q[0].position[0] = INFINITY; fallback(base);
    q = source; q[2].position[3] = 2; fallback(base);
    q = source; q[0].position[2] = 0; fallback(base);
    q = source; q[0].uv[0] = NAN; fallback(base);
    q = source; metadata[0].uv_bounds[2] = 0; fallback(base);
    metadata[0] = Metadata();
    auto pages = gpu::BuildParticleDofAtlasPages(std::span<const NativeParticleVertex>(q).first(3), base, metadata);
    Coverage(pages, 1);
    Check(metadata[0].atlas_scale == 0, "mismatched vertex span was packed");
    Check(gpu::BuildParticleDofAtlasPages({}, base, {}).empty(), "empty input created a draw page");

    std::vector<NativeParticleVertex> mixed;
    auto invalid = source; invalid[0].uv[0] = invalid[1].uv[0] = .25f;
    Append(mixed, invalid);
    auto focused = source;
    for (auto& v : focused) v.position[2] = 1; // focus at one view unit
    Append(mixed, focused);
    Append(mixed, source);
    std::vector<NativeParticleVertex> expanded;
    gpu::ParticleDofGeometryStats stats;
    Check(gpu::BuildParticleDofGeometry(mixed, base, expanded, metadata, stats), "fixture active quad failed expansion");
    const auto native = expanded;
    const auto originalMetadata = metadata;
    pages = gpu::BuildParticleDofAtlasPages(expanded, base, metadata);
    Coverage(pages, 3);
    Check(metadata[0].atlas_scale == 0 && metadata[1].atlas_scale == 0 && metadata[2].atlas_scale == 1,
          "unsupported/focused native quads were packed");
    Check(std::memcmp(expanded.data(), native.data(), native.size() * sizeof(NativeParticleVertex)) == 0,
          "packing mutated native/expanded vertices");
    for (size_t i = 0; i < metadata.size(); ++i)
        Check(std::memcmp(metadata[i].uv_bounds, originalMetadata[i].uv_bounds, 16) == 0 &&
              std::memcmp(metadata[i].argb, originalMetadata[i].argb, 16) == 0,
              "packing changed per-quad native metadata");
}
} // namespace

int main() {
    try {
        TestPackingAndOrder();
        TestProjectionClippingAndScale();
        TestFallbacksAndUnchangedNativeData();
        std::puts("PASS particle atlas CPU packing/order, projection, guards and native fallback");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL particle atlas CPU: %s\n", e.what());
        return 1;
    }
}
