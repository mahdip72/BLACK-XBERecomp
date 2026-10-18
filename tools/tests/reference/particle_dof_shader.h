#pragma once

namespace gpu {

// Appended to the existing b1 layout only by the verified native-particle shader wrapper. The wrapper
// declares ParticleDofQuad { float4 uv_bounds; uint4 argb; } before the common constant buffer.
inline constexpr char kParticleDofCbufferFields[] = R"(
    float4 particleOptics;
    float4 particleViewport;
    float4 particleTarget;
    ParticleDofQuad particleQuads[1024];
)";

// Wrapper contract: ParticleNativeExact preserves the complete original PS; ParticleNativeTap evaluates
// it with explicit original UV gradients and returns transparent for an alpha-test failure (no discard).
// Both take VOut and front-facing state. The VS passes a flat particleQuad and particleViewZ=1/input.z.
inline constexpr char kParticleDofHlsl[] = R"(
#define PARTICLE_DOF_TAPS 64
#if PARTICLE_DOF_SAMPLES > 1
Texture2DMS<float> particleSceneDepth : register(t3);
#else
Texture2D<float> particleSceneDepth : register(t3);
#endif

bool ParticleFinite(float x) { return (asuint(x) & 0x7f800000u) != 0x7f800000u; }
float ParticleRadius(float z) {
    float d = z - particleOptics.x;
    float c = min(max(abs(d) - particleOptics.x * .15, 0) / z, 1);
    return c * particleOptics.y * (d < 0 ? particleOptics.z : particleOptics.w);
}
float4 ParticleColor(uint argb) {
    return float4((argb >> 16) & 255, (argb >> 8) & 255, argb & 255, argb >> 24) / 255.0;
}
float4 ParticleDiffuse(uint4 colors, float2 local) {
    // The native index buffer is 0,1,2 / 0,2,3, with corners (0,0),(0,1),(1,1),(1,0).
    // Four-corner bilinear interpolation is different and changes emitter 15BF30's varying tint/alpha.
    if (local.y >= local.x)
        return ParticleColor(colors.x) * (1 - local.y) + ParticleColor(colors.y) * (local.y - local.x) +
            ParticleColor(colors.z) * local.x;
    return ParticleColor(colors.x) * (1 - local.x) + ParticleColor(colors.z) * local.y +
        ParticleColor(colors.w) * (local.x - local.y);
}
float ParticleVisible(float2 sourcePixel, float particleDepth) {
    if (any(sourcePixel < particleViewport.xy) ||
        any(sourcePixel >= particleViewport.xy + particleViewport.zw) ||
        any(sourcePixel < 0) || any(sourcePixel >= particleTarget.xy)) return 0;
    int2 p = int2(sourcePixel);
    float visible = 0;
    [unroll] for (uint s = 0; s < PARTICLE_DOF_SAMPLES; ++s) {
#if PARTICLE_DOF_SAMPLES > 1
        float opaque = particleSceneDepth.Load(p, s);
#else
        float opaque = particleSceneDepth.Load(int3(p, 0));
#endif
        // Compare the native biased hardware depth, with the actual draw's LESS/LEQUAL convention.
        // This source test supplements the unchanged destination depth test on the expanded quad.
        if (ParticleFinite(opaque) && opaque >= 0 && opaque <= 1)
            visible += particleTarget.w > .5 ? (particleDepth <= opaque) : (particleDepth < opaque);
    }
    return visible / PARTICLE_DOF_SAMPLES;
}
float4 main(VOut i, bool front : SV_IsFrontFace) : SV_Target {
    // Derivatives precede divergent branches. Native tc0.w is constant on the accepted affine quads.
    float2 uv = i.tc0.xy / i.tc0.w;
    float2 uvDx = ddx(uv), uvDy = ddy(uv);
    if (i.particleQuad >= 1024 || i.particleQuad >= (uint)particleTarget.z)
        return ParticleNativeExact(i, front);
    ParticleDofQuad quad = particleQuads[i.particleQuad];
    float2 extent = quad.uv_bounds.zw - quad.uv_bounds.xy;
    if (any(extent == 0) || !ParticleFinite(i.particleViewZ) || i.particleViewZ <= 0)
        return ParticleNativeExact(i, front);
    if (any((asuint(particleOptics) & 0x7f800000u) == 0x7f800000u) ||
        particleOptics.x <= 0 || any(particleOptics.yzw < 0)) return ParticleNativeExact(i, front);
    float radius = ParticleRadius(i.particleViewZ);
    if (!ParticleFinite(radius) || radius <= 0) return ParticleNativeExact(i, front);

    // Equal-area disk samples integrate the ORIGINAL finite quad; expansion is only raster support.
    // Rejected support/occlusion/alpha taps keep their kernel weight, so coverage is never inflated.
    float4 sum = 0;
    [loop] for (int tap = 0; tap < PARTICLE_DOF_TAPS; ++tap) {
        float angle = tap * 2.39996323;
        float2 offset = float2(cos(angle), sin(angle)) * sqrt((tap + .5) / PARTICLE_DOF_TAPS) * radius;
        float2 sourceUv = uv + uvDx * offset.x + uvDy * offset.y;
        float2 local = (sourceUv - quad.uv_bounds.xy) / extent;
        if (any(local < 0) || any(local > 1)) continue;
        float2 sourcePixel = i.pos.xy + offset;
        float visible = ParticleVisible(sourcePixel, i.pos.z);
        if (visible == 0) continue;
        VOut source = i;
        source.pos.xy = sourcePixel;
        source.tc0.xy = sourceUv * source.tc0.w;
        source.d0 = ParticleDiffuse(quad.argb, local);
        float4 color = ParticleNativeTap(source, front, uvDx, uvDy);
        if (any((asuint(color) & 0x7f800000u) == 0x7f800000u)) continue;
        float alpha = saturate(color.a) * visible;
        sum += float4(color.rgb * alpha, alpha);
    }
    // Native blending remains SRC_ALPHA/INV_SRC_ALPHA: return straight RGB after premultiplied gathering.
    // Zero alpha carries zero RGB, including expanded support with no surviving source contribution.
    return sum.a > 0 ? float4(sum.rgb / sum.a, sum.a / PARTICLE_DOF_TAPS) : float4(0, 0, 0, 0);
}
)";

} // namespace gpu
