// CPU regressions for native particle coverage. No renderer, device, or shader compiler is used.
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

#include "d3d/particle_dof_geometry.h"

namespace {
using Quad = std::array<gpu::NativeParticleVertex, 4>;
using Result = gpu::ParticleDofGeometryResult;
using Rejection = gpu::ParticleDofGeometryRejection;

void Check(bool value, const char* why) { if (!value) throw std::runtime_error(why); }

gpu::ParticleDofGeometryParams Params() {
    gpu::ParticleDofGeometryParams p{};
    p.globals.optics[0] = 2;
    p.globals.optics[1] = 12;
    p.globals.optics[2] = p.globals.optics[3] = 1;
    p.globals.target[0] = p.globals.viewport[2] = p.guest_size[0] = 4096;
    p.globals.target[1] = p.globals.viewport[3] = p.guest_size[1] = 4096;
    p.c98[0] = p.c98[1] = p.c98[3] = 1;
    return p;
}

Quad SmallQuad() {
    return {gpu::NativeParticleVertex{{2048, 2048, .02f, 1}, 0x10203040, {0, 0}, 7},
            gpu::NativeParticleVertex{{2048.125f, 2048.25f, .02f, 1}, 0x50607080, {0, 1}, 8},
            gpu::NativeParticleVertex{{2048.375f, 2048.125f, .02f, 1}, 0x90A0B0C0, {1, 1}, 9},
            gpu::NativeParticleVertex{{2048.25f, 2047.875f, .02f, 1}, 0xD0E0F000, {1, 0}, 10}};
}

struct Expansion {
    Quad vertices{};
    gpu::ParticleDofQuad metadata{};
    Rejection rejection = Rejection::None;
    Result result = Result::InvalidInput;
};

Expansion Expand(const Quad& q, const gpu::ParticleDofGeometryParams& p, float radius = 4) {
    gpu::NativeParticleVertex input[4], output[4];
    std::memcpy(input, q.data(), sizeof(input));
    Expansion e;
    e.result = gpu::ExpandParticleDofQuad(input, p, radius, output, e.metadata, &e.rejection);
    std::memcpy(e.vertices.data(), output, sizeof(output));
    return e;
}

// The old guard rounded each projected corner to float before subtracting the common screen offset.
// Retain only that arithmetic here to establish that the precision fixture exercises the regression.
bool OldAffineGuardRejects(const Quad& q, const gpu::ParticleDofGeometryParams& p) {
    float screen[4][2];
    for (int k = 0; k < 4; ++k) for (int j = 0; j < 2; ++j) {
        volatile float guest = q[k].position[j] * p.c98[j] + p.c97[j];
        screen[k][j] = guest * (p.globals.target[j] / p.guest_size[j]);
    }
    const float lu = std::hypot(screen[3][0] - screen[0][0], screen[3][1] - screen[0][1]);
    const float lv = std::hypot(screen[1][0] - screen[0][0], screen[1][1] - screen[0][1]);
    for (int j = 0; j < 2; ++j)
        if (std::abs(screen[2][j] - screen[1][j] - screen[3][j] + screen[0][j]) >
            std::max(1e-4f, (lu + lv) * 1e-5f)) return true;
    return false;
}

void TestTinyFarOffsetQuad() {
    auto p = Params();
    p.c98[0] = 1.1f;
    const auto q = SmallQuad();
    Check(OldAffineGuardRejects(q, p), "precision fixture does not reproduce the old affine rejection");
    const auto e = Expand(q, p);
    Check(e.result == Result::Ready && e.rejection == Rejection::None,
          "subpixel affine particle at a large screen offset was rejected");
    for (size_t k = 0; k < q.size(); ++k) {
        Check(e.vertices[k].position[2] == q[k].position[2] &&
              e.vertices[k].position[3] == q[k].position[3] &&
              e.vertices[k].argb == q[k].argb && e.vertices[k].pad == q[k].pad &&
              e.metadata.argb[k] == q[k].argb, "expansion changed native depth, W, color or padding");
        for (float v : e.vertices[k].position) Check(std::isfinite(v), "expanded position is non-finite");
        for (float v : e.vertices[k].uv) Check(std::isfinite(v), "expanded UV is non-finite");
    }
    Check(e.vertices[0].uv[0] < 0 && e.vertices[0].uv[1] < 0 &&
          e.vertices[2].uv[0] > 1 && e.vertices[2].uv[1] > 1, "accepted quad did not acquire aperture support");
}

void TestNativeInputQuantization() {
    auto q = SmallQuad();
    // The native callbacks form four corners by adding paired +/- float offsets to the center.
    // At this power-of-two boundary, +offset and -offset round with different float spacing.
    q[0].position[0] = 2048.0f + .01133f;
    q[1].position[0] = 2048.0f + .00970f;
    q[2].position[0] = 2048.0f - .01133f;
    q[3].position[0] = 2048.0f - .00970f;
    q[0].position[1] = 2048.0f - .25f;
    q[1].position[1] = 2048.0f + .125f;
    q[2].position[1] = 2048.0f + .25f;
    q[3].position[1] = 2048.0f - .125f;
    const double residual = std::abs(double(q[2].position[0]) - q[1].position[0] -
                                     q[3].position[0] + q[0].position[0]);
    Check(residual == 1.0 / 4096, "native fixture lost its input-rounding residual");
    Check(OldAffineGuardRejects(q, Params()), "native rounding fixture does not exercise the ULP bound");
    Check(Expand(q, Params()).result == Result::Ready, "native center-plus-offset rounding was rejected");
}

void ExpectFallback(const Quad& q, const gpu::ParticleDofGeometryParams& p,
                    Rejection why, float radius = 4) {
    const auto e = Expand(q, p, radius);
    Check(e.result != Result::Ready && e.result != Result::ZeroRadius && e.rejection == why,
          "unsupported geometry returned the wrong rejection category");
    Check(std::memcmp(q.data(), e.vertices.data(), sizeof(q)) == 0,
          "native fallback altered the original vertices");
    const gpu::ParticleDofQuad empty{};
    Check(std::memcmp(&empty, &e.metadata, sizeof(empty)) == 0, "native fallback left active metadata");
}

void TestStrictFallbacks() {
    const auto p = Params();
    auto q = SmallQuad();
    q[2].position[0] += .1f;
    ExpectFallback(q, p, Rejection::NonAffine);

    // An enormous float ULP must not allow a visibly non-affine quad through the 1/64px cap.
    q = SmallQuad();
    for (auto& v : q) v.position[0] += 1048576.0f;
    q[2].position[0] += .125f;
    ExpectFallback(q, p, Rejection::NonAffine);

    q = SmallQuad(); q[2].position[2] += .001f;
    ExpectFallback(q, p, Rejection::Depth);
    q = SmallQuad(); q[1].position[3] = 2;
    ExpectFallback(q, p, Rejection::Depth);
    q = SmallQuad(); for (auto& v : q) v.position[2] = -1;
    ExpectFallback(q, p, Rejection::Depth);
    q = SmallQuad(); q[2].uv[0] = .5f;
    ExpectFallback(q, p, Rejection::Uv);
    q = SmallQuad(); for (auto& v : q) v.position[1] = 2048;
    ExpectFallback(q, p, Rejection::Degenerate);
    q = SmallQuad(); q[0].position[0] = std::numeric_limits<float>::quiet_NaN();
    ExpectFallback(q, p, Rejection::InvalidInput);
    ExpectFallback(SmallQuad(), p, Rejection::InvalidInput, std::numeric_limits<float>::infinity());
    auto invalid = p; invalid.guest_size[0] = 0;
    ExpectFallback(SmallQuad(), invalid, Rejection::InvalidInput);
    invalid = p; invalid.c98[0] = std::numeric_limits<float>::max();
    ExpectFallback(SmallQuad(), invalid, Rejection::InvalidInput);
}

void TestFocusedIdentityAndCounts() {
    auto focused = SmallQuad();
    for (auto& v : focused) v.position[2] = .5f;
    focused[2].position[0] += .1f;
    focused[1].uv[0] = .2f;
    const auto e = Expand(focused, Params(), 0);
    Check(e.result == Result::ZeroRadius && e.rejection == Rejection::None &&
          std::memcmp(focused.data(), e.vertices.data(), sizeof(focused)) == 0,
          "focused geometry must stay byte-identical even when its shape is unsupported");

    std::vector<gpu::NativeParticleVertex> input;
    auto append = [&](const Quad& q) { input.insert(input.end(), q.begin(), q.end()); };
    append(SmallQuad()); append(focused);
    auto q = SmallQuad(); q[2].position[0] += .1f; append(q);
    q = SmallQuad(); q[1].position[2] += .001f; append(q);
    q = SmallQuad(); q[2].uv[0] = .5f; append(q);
    q = SmallQuad(); for (auto& v : q) v.position[1] = 2048; append(q);
    q = SmallQuad(); q[0].position[0] = std::numeric_limits<float>::infinity(); append(q);
    std::vector<gpu::NativeParticleVertex> expanded;
    std::vector<gpu::ParticleDofQuad> metadata;
    gpu::ParticleDofGeometryStats stats;
    gpu::ParticleDofGeometryRejections reasons;
    reasons.fill(123);
    Check(gpu::BuildParticleDofGeometry(input, Params(), expanded, metadata, stats, &reasons),
          "mixed batch lost its supported quad");
    Check(stats.active == 1 && stats.focused == 1 && stats.invalid == 5, "mixed coverage counts are wrong");
    Check(reasons[size_t(Rejection::None)] == 0, "focused quads were counted as rejections");
    for (size_t reason = size_t(Rejection::InvalidInput); reason < size_t(Rejection::Count); ++reason)
        Check(reasons[reason] == 1, "rejection counters were not reset or categorized correctly");
    Check(expanded.size() == input.size() && metadata.size() == 7 &&
          std::memcmp(input.data() + 4, expanded.data() + 4, (input.size() - 4) * sizeof(input[0])) == 0,
          "mixed batch changed native fallback order or vertices");
}
} // namespace

int main() {
    try {
        TestTinyFarOffsetQuad();
        TestNativeInputQuantization();
        TestStrictFallbacks();
        TestFocusedIdentityAndCounts();
        std::puts("particle DOF geometry: precision, bounded native quantization and fallback checks passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "particle DOF geometry: %s\n", e.what());
        return 1;
    }
}
