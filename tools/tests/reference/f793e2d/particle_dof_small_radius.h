#pragma once

#include <array>
#include <cmath>
#include <cstddef>

namespace gpu {

// For 0 <= radius <= 1, each of the production 64 disk taps bilinearly samples
// only the 3x3 texels around an atlas pixel center. These coefficients sum the
// same ideal bilinear footprints, including the production division by 64.
// Row-major order corresponds to texel offsets (-1,-1) through (+1,+1).
// The coefficients come from kParticleDofDiskHlsl in particle_dof_shader.h.
struct ParticleDofSmallRadiusCoefficients {
    float constant;
    float linear;
    float quadratic;
};

inline constexpr std::array<ParticleDofSmallRadiusCoefficients, 9> kParticleDofSmallRadiusCoefficients{{
    {0.0f, 0.0f,  0.04107645295882298f},
    {0.0f, 0.21237217964531244f, -0.07963070810963187f},
    {0.0f, 0.0f,  0.03855425515080888f},
    {0.0f, 0.20780363688124998f, -0.07960205895015963f},
    {1.0f, -0.8496441522214061f, 0.15990160635179074f},
    {0.0f, 0.21652290114328127f, -0.08029954740163112f},
    {0.0f, 0.0f,  0.03852560599133665f},
    {0.0f, 0.21294543455156256f, -0.08027089824215886f},
    {0.0f, 0.0f,  0.04174529225082223f},
}};

struct ParticleDofSmallRadiusWeights {
    std::array<float, 9> texel{};
};

inline bool BuildParticleDofSmallRadiusWeights(float radius, ParticleDofSmallRadiusWeights& out) {
    if (!std::isfinite(radius) || radius < 0.0f || radius > 1.0f) return false;
    ParticleDofSmallRadiusWeights result;
    for (std::size_t i = 0; i < result.texel.size(); ++i) {
        const auto& c = kParticleDofSmallRadiusCoefficients[i];
        result.texel[i] = (c.quadratic * radius + c.linear) * radius + c.constant;
    }
    out = result;
    return true;
}

// Append this text after particleSourceAtlas is declared in the atlas filter.
// The caller must first apply the existing cell-guard check and use this only
// for a finite radius in [0,1]. No source position can cross the atlas cell,
// and the neighboring guard texels are intentionally included in the 3x3 load.
// SampleLevel hardware can quantize bilinear fractions; this is algebraically
// exact for ideal bilinear filtering, not guaranteed bitwise identical on GPU.
inline constexpr char kParticleDofSmallRadiusHlsl[] = R"(
static const float3 particleSmallRadiusCoefficients[9] = {
    float3(0.0, 0.0,  0.04107645295882298),
    float3(0.0, 0.21237217964531244, -0.07963070810963187),
    float3(0.0, 0.0,  0.03855425515080888),
    float3(0.0, 0.20780363688124998, -0.07960205895015963),
    float3(1.0, -0.8496441522214061, 0.15990160635179074),
    float3(0.0, 0.21652290114328127, -0.08029954740163112),
    float3(0.0, 0.0,  0.03852560599133665),
    float3(0.0, 0.21294543455156256, -0.08027089824215886),
    float3(0.0, 0.0,  0.04174529225082223)
};
float4 ParticleSmallRadiusFilter(float2 atlasPixel, float radius) {
    int2 pixel = int2(atlasPixel);
    float4 sum = 0;
    [unroll] for (int tap = 0; tap < 9; ++tap) {
        float3 c = particleSmallRadiusCoefficients[tap];
        float weight = (c.z * radius + c.y) * radius + c.x;
        int2 offset = int2(tap % 3 - 1, tap / 3 - 1);
        sum += particleSourceAtlas.Load(int3(pixel + offset, 0)) * weight;
    }
    return sum;
}
)";

} // namespace gpu
