#pragma once
#include <string>
#include "particle_dof_small_radius.h"

namespace gpu {

// Render once per current MS depth snapshot into R32G32_UINT. Exact D24 codes let every visibility probe
// compare these bounds directly; mixed samples still use the original raw depth snapshot.
inline constexpr char kParticleDofDepthRangeHlsl[] = R"(
Texture2DMS<float> particleRangeDepth : register(t3);
uint ParticleRangeD24Code(float depth) {
    // Keep identical to ParticleD24Code below: no float multiply/add rounding at odd D24 codes.
    uint bits = asuint(saturate(depth));
    uint exponent = (bits >> 23) & 255u;
    uint mantissa = (bits & 0x7fffffu) | 0x800000u;
    if (exponent >= 127u) return 0xffffffu;
    if (exponent == 126u) return mantissa - (bits == 0x3f000000u ? 0u : 1u);
    if (exponent < 102u) return 0;
    uint shift = 126u - exponent;
    uint remainder = mantissa & ((1u << shift) - 1u);
    return (mantissa >> shift) + (remainder > (1u << (shift - 1u)) ? 1u : 0u);
}
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
uint2 range_ps(float4 pos : SV_Position) : SV_Target {
    uint minimum = 0xffffffu, maximum = 0;
    bool valid = true;
    [unroll] for (uint sampleIndex = 0; sampleIndex < PARTICLE_DOF_SAMPLES; ++sampleIndex) {
        float depth = particleRangeDepth.Load(int2(pos.xy), sampleIndex);
        bool sampleValid = (asuint(depth) & 0x7f800000u) != 0x7f800000u && depth >= 0 && depth <= 1;
        valid = valid && sampleValid;
        if (sampleValid) {
            uint code = ParticleRangeD24Code(depth);
            minimum = min(minimum, code);
            maximum = max(maximum, code);
        }
    }
    // R24_UNORM is always valid. The sentinel also keeps generic test inputs and future formats safe.
    return valid ? uint2(minimum, maximum) : uint2(0xffffffffu, 0);
}
)";

inline constexpr char kParticleDofQuadHlsl[] = R"(
struct ParticleDofQuad {
    float4 uv_bounds; uint4 argb;
    uint atlas_xy; uint screen_xy; uint atlas_size; float atlas_scale;
};
)";

inline constexpr char kParticleDofDiskHlsl[] = R"(
#ifndef PARTICLE_DOF_TAPS
#define PARTICLE_DOF_TAPS 64
#endif
// The original 64-tap golden-angle disk, evaluated in float32 once instead of sin/cos/sqrt per fragment.
// Float32 transcendental implementations can differ by a few ulps; sample count, radii and weights match.
static const float2 particleDisk[PARTICLE_DOF_TAPS] = {
    float2(0.0883883461, 0.00000000),
    float2(-0.112886086, 0.103412926),
    float2(0.0172789898, -0.196885601),
    float2(0.142285705, 0.185586318),
    float2(-0.261111587, -0.0461868681),
    float2(0.247347668, -0.157342404),
    float2(-0.0827326477, 0.307762593),
    float2(-0.157781005, -0.303797066),
    float2(0.342321157, 0.125014856),
    float2(-0.356127948, 0.147004679),
    float2(0.171677098, -0.366864353),
    float2(0.126865238, 0.404465973),
    float2(-0.382373393, -0.221592173),
    float2(0.448566824, -0.0986170396),
    float2(-0.273752779, 0.389386594),
    float2(-0.0632441491, -0.488044739),
    float2(0.388252854, 0.327218950),
    float2(-0.522465944, 0.0216062758),
    float2(0.381098419, -0.379244655),
    float2(-0.0254964456, 0.551395893),
    float2(-0.362617105, -0.434535801),
    float2(0.574425042, 0.0772876814),
    float2(-0.486708939, 0.338639796),
    float2(0.132996708, -0.591184735),
    float2(0.307617158, 0.536827922),
    float2(-0.601358533, -0.191847205),
    float2(0.584141612, -0.269890904),
    float2(-0.253062993, 0.604687214),
    float2(-0.225857258, -0.627933919),
    float2(0.600977182, 0.315854251),
    float2(-0.667532682, 0.175961733),
    float2(0.379428744, -0.590102792),
    float2(0.120701097, 0.702313125),
    float2(-0.572009027, -0.442993462),
    float2(0.731701791, -0.0606218800),
    float2(-0.505758941, 0.546713233),
    float2(0.00368232187, -0.755181432),
    float2(0.514306188, 0.566945076),
    float2(-0.772294998, -0.0715745986),
    float2(0.625786424, -0.474951416),
    float2(-0.142379269, 0.782649815),
    float2(-0.428885013, -0.681538880),
    float2(0.785920441, 0.215387046),
    float2(-0.733485043, 0.376413524),
    float2(0.289860845, -0.781852424),
    float2(0.317912191, 0.780941367),
    float2(-0.770264208, -0.365041852),
    float2(0.823263168, -0.253821373),
    float2(-0.440150440, 0.751052678),
    float2(-0.184650064, -0.859849870),
    float2(0.724181354, 0.514416039),
    float2(-0.890156507, 0.110945404),
    float2(0.587049127, -0.689699829),
    float2(0.0333266184, 0.913688600),
    float2(-0.647731721, -0.657271743),
    float2(0.930014491, 0.0475456901),
    float2(-0.724318981, 0.598476827),
    float2(0.130968973, -0.938767612),
    float2(0.542210042, 0.787445664),
    float2(-0.939650416, -0.216204986),
    float2(0.845933735, -0.479279250),
    float2(-0.302486151, 0.932437479),
    float2(-0.410102487, -0.899098635),
    float2(0.916978121, 0.389022589)
};
)";

// Appended to the existing b1 layout only by the verified native-particle shader wrapper. The wrapper
// declares ParticleDofQuad { float4 uv_bounds; uint4 argb; } before the common constant buffer.
inline constexpr char kParticleDofCbufferFields[] = R"(
    float4 particleOptics;
    float4 particleViewport;
    float4 particleTarget;
    float4 particleAtlasTarget;
    float4 particleProjection; // world depth numerator/offset, aware final coverage enabled, reserved
    ParticleDofQuad particleQuads[1024];
)";

// Wrapper contract: ParticleNativeExact preserves the complete original PS; ParticleNativeTap evaluates
// it with explicit original UV gradients and returns transparent for an alpha-test failure (no discard).
// Both take VOut and front-facing state. The VS passes a flat particleQuad and particleViewZ=1/input.z.
inline constexpr char kParticleDofHlsl[] = R"(
#ifndef PARTICLE_DOF_PASS
#define PARTICLE_DOF_PASS 0
#endif
#ifndef PARTICLE_DOF_BLEND
#define PARTICLE_DOF_BLEND 0
#endif
#if PARTICLE_DOF_SAMPLES > 1
Texture2DMS<float> particleSceneDepth : register(t3);
#if PARTICLE_DOF_PASS != 4
// Exact D24 min/max codes of every sample in the current snapshot, prequantized once per depth copy.
Texture2D<uint2> particleDepthBounds : register(t2);
#else
Texture2D<float2> particleTransmissionField : register(t2);
#endif
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
float4 ParticleDiffuseColors(float4 c0, float4 c1, float4 c2, float4 c3, float2 local) {
    // The native index buffer is 0,1,2 / 0,2,3, with corners (0,0),(0,1),(1,1),(1,0).
    // Four-corner bilinear interpolation is different and changes emitter 15BF30's varying tint/alpha.
    if (local.y >= local.x)
        return c0 * (1 - local.y) + c1 * (local.y - local.x) + c2 * local.x;
    return c0 * (1 - local.x) + c2 * local.y + c3 * (local.x - local.y);
}
float4 ParticleDiffuse(uint4 colors, float2 local) {
    return ParticleDiffuseColors(ParticleColor(colors.x), ParticleColor(colors.y),
        ParticleColor(colors.z), ParticleColor(colors.w), local);
}
uint ParticleD24Code(float depth) {
    // Exact floor(saturate(depth)*16777215 + .5) for a finite float input.
    // Float multiply/add would lose odd codes at large depths. Instead use
    // x*(2^24-1)=x*2^24-x: below .5 a mantissa remainder strictly ABOVE half
    // rounds up; at .5 the exact tie rounds up; above .5 subtract one code.
    uint bits = asuint(saturate(depth));
    uint exponent = (bits >> 23) & 255u;
    uint mantissa = (bits & 0x7fffffu) | 0x800000u;
    if (exponent >= 127u) return 0xffffffu;
    if (exponent == 126u) return mantissa - (bits == 0x3f000000u ? 0u : 1u);
    if (exponent < 102u) return 0;
    uint shift = 126u - exponent;
    uint remainder = mantissa & ((1u << shift) - 1u);
    return (mantissa >> shift) + (remainder > (1u << (shift - 1u)) ? 1u : 0u);
}
bool ParticleDepthPassCode(uint incomingCode, float opaque, bool lessEqual) {
    if (!ParticleFinite(opaque) || opaque < 0 || opaque > 1) return false;
    // Hardware compares D24 codes, whereas SV_Position.z is still unsnapped.
    // Recover the sampled D24 code too, avoiding a float read-conversion tie.
    uint opaqueCode = ParticleD24Code(opaque);
    return lessEqual ? incomingCode <= opaqueCode : incomingCode < opaqueCode;
}
bool ParticleDepthPass(float particleDepth, float opaque, bool lessEqual) {
    return ParticleFinite(particleDepth) && ParticleDepthPassCode(ParticleD24Code(particleDepth), opaque, lessEqual);
}
float ParticleVisibleCode(float2 sourcePixel, uint incomingCode, float2 low, float2 high, bool lessEqual) {
    if (any(sourcePixel < low) || any(sourcePixel >= high)) return 0;
    int2 p = int2(sourcePixel);
#if PARTICLE_DOF_SAMPLES > 1 && PARTICLE_DOF_PASS != 4
    uint2 bounds = particleDepthBounds.Load(int3(p, 0));
    if (bounds.x <= bounds.y && bounds.y <= 0xffffffu) {
        if (lessEqual ? incomingCode <= bounds.x : incomingCode < bounds.x) return 1;
        if (lessEqual ? incomingCode > bounds.y : incomingCode >= bounds.y) return 0;
#if PARTICLE_DOF_SAMPLES == 2 && PARTICLE_DOF_PASS == 3
        // With two valid samples, an unresolved min/max interval contains exactly
        // one passing sample, including LESS/LEQUAL equality at either endpoint.
        // Keep this specialization in the shared field: common fallback register
        // allocation regressed in matched GPU tests with the extra branch.
        return .5;
#endif
    }
#endif
    float visible = 0;
    // Bounds above handle uniform depths. Keep the rare mixed-sample path compact:
    // unrolling repeats the exact D24 conversion inside every visibility probe.
    [loop] for (uint s = 0; s < PARTICLE_DOF_SAMPLES; ++s) {
#if PARTICLE_DOF_SAMPLES > 1
        float opaque = particleSceneDepth.Load(p, s);
#else
        float opaque = particleSceneDepth.Load(int3(p, 0));
#endif
        // Compare the native biased hardware depth, with the actual draw's LESS/LEQUAL convention.
        // This is the source visibility field; aware final coverage must not apply it a second time.
        if (ParticleFinite(opaque) && opaque >= 0 && opaque <= 1) {
            uint opaqueCode = ParticleD24Code(opaque);
            visible += lessEqual ? (incomingCode <= opaqueCode) : (incomingCode < opaqueCode);
        }
    }
    return visible / PARTICLE_DOF_SAMPLES;
}
float ParticleVisible(float2 sourcePixel, float particleDepth, float2 low, float2 high, bool lessEqual) {
    if (!ParticleFinite(particleDepth)) return 0;
    return ParticleVisibleCode(sourcePixel, ParticleD24Code(particleDepth), low, high, lessEqual);
}

#if PARTICLE_DOF_PASS != 1
struct ParticleFinal { float4 color : SV_Target; uint coverage : SV_Coverage; };
#if PARTICLE_DOF_PASS != 4 && PARTICLE_DOF_PASS != 5
float ParticleRawDepth(int2 pixel, uint sampleIndex) {
#if PARTICLE_DOF_SAMPLES > 1
    return particleSceneDepth.Load(pixel, sampleIndex);
#else
    return particleSceneDepth.Load(int3(pixel, 0));
#endif
}
float ParticleActiveRadius(VOut i) {
    if (i.particleQuad >= 1024 || i.particleQuad >= (uint)particleTarget.z ||
        !ParticleFinite(i.particleViewZ) || i.particleViewZ <= 0) return 0;
    ParticleDofQuad quad = particleQuads[i.particleQuad];
    if (any(quad.uv_bounds.zw == quad.uv_bounds.xy) ||
        any((asuint(particleOptics) & 0x7f800000u) == 0x7f800000u) ||
        particleOptics.x <= 0 || any(particleOptics.yzw < 0)) return 0;
    float radius = ParticleRadius(i.particleViewZ);
    return ParticleFinite(radius) && radius > 0 ? radius : 0;
}
float ParticleBilinearVisibility(int2 center, float2 offset, uint incomingCode,
                                 float2 low, float2 high, bool lessEqual) {
    // Pixel centers are p+.5. Floor/fraction share the relative offset so FXC
    // cannot round an absolute sum in only one half of the bilinear footprint.
    int2 p = center + int2(floor(offset));
    float2 f = frac(offset);
    return lerp(lerp(ParticleVisibleCode(float2(p) + .5, incomingCode, low, high, lessEqual),
                     ParticleVisibleCode(float2(p + int2(1, 0)) + .5, incomingCode, low, high, lessEqual), f.x),
                lerp(ParticleVisibleCode(float2(p + int2(0, 1)) + .5, incomingCode, low, high, lessEqual),
                     ParticleVisibleCode(float2(p + 1) + .5, incomingCode, low, high, lessEqual), f.x), f.y);
}
float2 ParticleOccluderTransmission(int2 pixel, float opaque, uint incomingCode, float particleRadius,
                                    float2 low, float2 high, bool lessEqual, float particleVisibility) {
    if (ParticleDepthPassCode(incomingCode, opaque, lessEqual)) return float2(1, particleVisibility);
    if (!ParticleFinite(opaque) || opaque < 0 || opaque > 1 ||
        any((asuint(particleProjection.xy) & 0x7f800000u) == 0x7f800000u)) return float2(0, particleVisibility);
    float denominator = opaque - particleProjection.y;
    if (abs(denominator) < 1e-7) return float2(0, particleVisibility);
    float opaqueZ = particleProjection.x / denominator;
    if (!ParticleFinite(opaqueZ) || opaqueZ <= 0) return float2(0, particleVisibility);
    float opaqueRadius = ParticleRadius(opaqueZ);
    // Focused occluders retain their exact per-sample hard rejection.
    if (!ParticleFinite(opaqueRadius) || opaqueRadius <= .5) return float2(0, particleVisibility);
    // Source energy already carries visibility filtered by the particle aperture.
    // A broader opaque aperture must not independently multiply that mask again.
    if (opaqueRadius >= particleRadius) return float2(1, particleVisibility);
    // A previously evaluated empty denominator cannot transmit. Keep this after
    // the native-visible/focused/broader-aperture gates, whose results are independent.
    if (particleVisibility >= 0 && particleVisibility <= 1e-6) return float2(0, particleVisibility);
    float opaqueVisibility = 0;
    [loop] for (int tap = 0; tap < 32; ++tap) {
        // First 32 equal-area positions of the 64 disk, normalized to unit radius.
        float2 disk = particleDisk[tap] * 1.41421356237;
        opaqueVisibility += ParticleBilinearVisibility(pixel, disk * opaqueRadius, incomingCode, low, high, lessEqual);
    }
    // Zero numerator is exactly blocked regardless of the denominator. Leave an
    // unevaluated denominator negative so another MSAA depth may still request it.
    if (opaqueVisibility == 0) return float2(0, particleVisibility);
    if (particleVisibility < 0) {
        float particleSum = 0;
        [loop] for (int tap = 0; tap < 32; ++tap) {
            float2 disk = particleDisk[tap] * 1.41421356237;
            particleSum += ParticleBilinearVisibility(pixel, disk * particleRadius, incomingCode, low, high, lessEqual);
        }
        particleVisibility = particleSum;
    }
    // Both sums have the same 32-tap denominator. No amplification or invented
    // energy when the sampled source visibility is empty/invalid.
    if (!ParticleFinite(particleVisibility) || particleVisibility <= 1e-6 || !ParticleFinite(opaqueVisibility))
        return float2(0, particleVisibility);
    return float2(saturate(opaqueVisibility / particleVisibility), particleVisibility);
}
)" R"(
float2 ParticleCoverage(VOut i, float alpha, uint rasterCoverage) {
    rasterCoverage &= (1u << PARTICLE_DOF_SAMPLES) - 1u;
    // Return mask/transmission as ordinary values. FXC O1 drops the computed
    // mask when it crosses this helper's early returns through an out parameter.
    // Supported sample masks are <=255, exactly representable as float.
    if (!(particleProjection.z > 0)) return float2(rasterCoverage, 1);
    float2 low = max(particleViewport.xy, 0);
    float2 high = min(particleViewport.xy + particleViewport.zw, particleTarget.xy);
    uint finalCoverage = 0;
    if (any(i.pos.xy < low) || any(i.pos.xy >= high) || !ParticleFinite(i.pos.z)) return 0;
    bool lessEqual = particleTarget.w > .5;
    // This native depth is constant for every bilinear corner and both apertures.
    uint incomingCode = ParticleD24Code(i.pos.z);
    float radius = ParticleActiveRadius(i);
    int2 pixel = int2(i.pos.xy);
    // Native/focused fallback preserves exact sample bits; no averaging changes
    // its alpha test, material or blend behavior when hardware depth is disabled.
    if (radius <= 0) {
        [unroll] for (uint sampleIndex = 0; sampleIndex < PARTICLE_DOF_SAMPLES; ++sampleIndex)
            if ((rasterCoverage & (1u << sampleIndex)) && ParticleDepthPassCode(incomingCode, ParticleRawDepth(pixel, sampleIndex), lessEqual))
                finalCoverage |= 1u << sampleIndex;
        return float2(finalCoverage, finalCoverage ? 1 : 0);
    }
    if (alpha <= 0) return 0; // fully hidden source energy remains empty
#if PARTICLE_DOF_SAMPLES > 1
    // The existing exact depth bounds can prove every covered sample visible,
    // avoiding raw loads and the per-sample deduplication path without changing bits.
    uint2 bounds = particleDepthBounds.Load(int3(pixel, 0));
    if (bounds.x <= bounds.y && bounds.y <= 0xffffffu) {
        if (lessEqual ? incomingCode <= bounds.x : incomingCode < bounds.x)
            return float2(rasterCoverage, rasterCoverage ? 1 : 0);
    }
#endif
    float depths[PARTICLE_DOF_SAMPLES], transmission[PARTICLE_DOF_SAMPLES];
    float total = 0, particleVisibility = -1;
    [loop] for (uint sampleIndex = 0; sampleIndex < PARTICLE_DOF_SAMPLES; ++sampleIndex) {
        depths[sampleIndex] = 0; transmission[sampleIndex] = 0;
        if (!(rasterCoverage & (1u << sampleIndex))) continue;
        float opaque = ParticleRawDepth(pixel, sampleIndex);
        float value = -1;
        // Most MSAA pixels have one depth. Reuse the complete coverage estimate
        // across equal samples, including its 32 paired visibility probes.
        [loop] for (uint previous = 0; previous < sampleIndex; ++previous)
            if ((rasterCoverage & (1u << previous)) && asuint(depths[previous]) == asuint(opaque)) {
                value = transmission[previous]; break;
            }
        if (value < 0) {
            float2 estimate = ParticleOccluderTransmission(pixel, opaque, incomingCode, radius, low, high, lessEqual, particleVisibility);
            value = estimate.x;
            particleVisibility = estimate.y;
        }
        depths[sampleIndex] = opaque; transmission[sampleIndex] = value;
        if (value > 0) { finalCoverage |= 1u << sampleIndex; total += value; }
    }
    if (!finalCoverage) return 0;
    float meanTransmission = saturate(total / countbits(finalCoverage));
    // Pixel-frequency approximation: preserve resolved source energy and exact
    // focused sample rejection, while spreading nonzero Ti uniformly over its
    // surviving samples. Subsequent overlapping blends can see this difference.
    return float2(finalCoverage, meanTransmission);
}
#endif
float4 ParticleTransmit(float4 color, float2 coverage) {
    if (coverage.x == 0) return 0;
#if PARTICLE_DOF_BLEND == 1
    // P=C*a and Q=a*a both scale by T before reconstruction: a'=a*sqrt(T), C'=C*sqrt(T).
    return color * sqrt(coverage.y);
#else
    // P and A both scale by T, so straight color is unchanged and a'=a*T.
    return float4(color.rgb, color.a * coverage.y);
#endif
}
#endif
#if PARTICLE_DOF_PASS == 0 || PARTICLE_DOF_PASS == 2
float4 ParticleLegacy(VOut i, bool front) {
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
    float4 c0 = ParticleColor(quad.argb.x), c1 = ParticleColor(quad.argb.y);
    float4 c2 = ParticleColor(quad.argb.z), c3 = ParticleColor(quad.argb.w);
    float2 low = max(particleViewport.xy, 0);
    float2 high = min(particleViewport.xy + particleViewport.zw, particleTarget.xy);
    bool lessEqual = particleTarget.w > .5;

    // Equal-area disk samples integrate the ORIGINAL finite quad; expansion is only raster support.
    // Rejected support/occlusion/alpha taps keep their kernel weight, so coverage is never inflated.
    float4 sum = 0;
#if PARTICLE_DOF_BLEND == 1
    float alphaSecondMoment = 0;
#endif
    [loop] for (int tap = 0; tap < PARTICLE_DOF_TAPS; ++tap) {
        float2 offset = particleDisk[tap] * radius;
        float2 sourceUv = uv + uvDx * offset.x + uvDy * offset.y;
        float2 local = (sourceUv - quad.uv_bounds.xy) / extent;
        if (any(local < 0) || any(local > 1)) continue;
        float2 sourcePixel = i.pos.xy + offset;
        float visible = ParticleVisible(sourcePixel, i.pos.z, low, high, lessEqual);
        if (visible == 0) continue;
        VOut source = i;
        source.pos.xy = sourcePixel;
        source.tc0.xy = sourceUv * source.tc0.w;
        source.d0 = ParticleDiffuseColors(c0, c1, c2, c3, local);
        float4 color = ParticleNativeTap(source, front, uvDx, uvDy);
        if (any((asuint(color) & 0x7f800000u) == 0x7f800000u)) continue;
        float alpha = saturate(color.a) * visible;
        sum += float4(color.rgb * alpha, alpha);
#if PARTICLE_DOF_BLEND == 1
        alphaSecondMoment += saturate(color.a) * alpha;
#endif
    }
#if PARTICLE_DOF_BLEND == 1
    // SRC_ALPHA/ONE applies source alpha to both RGB and alpha. Preserve both filtered native additions:
    // RGB energy = E[C*a*v], alpha energy = E[a*a*v], where v is source MSAA visibility.
    // Choosing output alpha sqrt(E[a*a*v]) lets the unchanged blend state reproduce both moments.
    float outputAlpha = sqrt(alphaSecondMoment / PARTICLE_DOF_TAPS);
    return outputAlpha > 0 ? float4(sum.rgb / (PARTICLE_DOF_TAPS * outputAlpha), outputAlpha) : 0;
#else
    // Native blending remains SRC_ALPHA/INV_SRC_ALPHA: return straight RGB after premultiplied gathering.
    // Zero alpha carries zero RGB, including expanded support with no surviving source contribution.
    return sum.a > 0 ? float4(sum.rgb / sum.a, sum.a / PARTICLE_DOF_TAPS) : float4(0, 0, 0, 0);
#endif
}
#endif

)" R"(
float2 ParticleUnpack(uint packed) { return float2(packed & 65535u, packed >> 16); }

#if PARTICLE_DOF_PASS == 1
float4 ParticleAtlasSourceTap(VOut i, bool front, ParticleDofQuad quad,
                              float2 uv, float2 uvDx, float2 uvDy, float2 screen) {
    float2 extent = quad.uv_bounds.zw - quad.uv_bounds.xy;
    if (any(extent == 0)) return 0;
    float2 local = (uv - quad.uv_bounds.xy) / extent;
    if (any(local < 0) || any(local > 1)) return 0;
    float visible = ParticleVisible(screen, i.pos.z, max(particleViewport.xy, 0),
        min(particleViewport.xy + particleViewport.zw, particleTarget.xy), particleTarget.w > .5);
    if (visible == 0) return 0;
    i.pos.xy = screen;
    i.tc0.xy = uv * i.tc0.w;
    i.d0 = ParticleDiffuse(quad.argb, local);
    float4 color = ParticleNativeTap(i, front, uvDx, uvDy);
    if (any((asuint(color) & 0x7f800000u) == 0x7f800000u)) return 0;
    float alpha = saturate(color.a);
    float4 contribution = float4(color.rgb * alpha, alpha) * visible;
#if PARTICLE_DOF_BLEND == 1
    contribution.a *= alpha;
#endif
    return contribution;
}

float4 main(VOut i, bool front : SV_IsFrontFace) : SV_Target {
    // Atlas derivatives are coarser than native screen derivatives when the source is reduced.
    float2 uv = i.tc0.xy / i.tc0.w;
    float2 atlasDx = ddx(uv), atlasDy = ddy(uv);
    if (i.particleQuad >= 1024 || i.particleQuad >= (uint)particleTarget.z) discard;
    ParticleDofQuad quad = particleQuads[i.particleQuad];
    float scale = quad.atlas_scale;
    if ((scale != 1 && scale != .5) || !ParticleFinite(i.particleViewZ) || i.particleViewZ <= 0) discard;
    float2 origin = ParticleUnpack(quad.atlas_xy), size = ParticleUnpack(quad.atlas_size);
    // A clipped screen rectangle can leave the original projected quad outside its allocated cell.
    // Discard there; writing transparent would overwrite a neighboring particle's source image.
    if (any(i.pos.xy < origin + 2) || any(i.pos.xy >= origin + size - 2)) discard;
    float2 screen = ParticleUnpack(quad.screen_xy) + (i.pos.xy - origin - 2) / scale;
    float2 uvDx = atlasDx * scale, uvDy = atlasDy * scale;
    if (scale == 1) return ParticleAtlasSourceTap(i, front, quad, uv, uvDx, uvDy, screen);
    float4 sum = 0;
    [unroll] for (int y = 0; y < 2; ++y) {
        [unroll] for (int x = 0; x < 2; ++x) {
            float2 offset = float2(x, y) - .5;
            sum += ParticleAtlasSourceTap(i, front, quad, uv + uvDx * offset.x + uvDy * offset.y,
                uvDx, uvDy, screen + offset);
        }
    }
    // Four actual native pixel centers, each individually clipped and depth-tested. Missing contributors
    // keep their area, including odd-width/height cells and viewport edges.
    return sum * .25;
}
#elif PARTICLE_DOF_PASS == 3
// A page with one proven native depth/radius shares the expensive visibility
// integrals. Store individual sample transmissions, never an aggregate mask.
Texture2DMS<float, 2> particleFieldSupport : register(t1);
float2 main(VOut i, bool front : SV_IsFrontFace) : SV_Target {
    int2 pixel = int2(i.pos.xy);
    if (!(particleProjection.z > 0) || !ParticleFinite(i.pos.z)) return 0;
    float2 low = max(particleViewport.xy, 0);
    float2 high = min(particleViewport.xy + particleViewport.zw, particleTarget.xy);
    if (any(i.pos.xy < low) || any(i.pos.xy >= high)) return 0;
    if (particleFieldSupport.Load(pixel, 0) <= 0 && particleFieldSupport.Load(pixel, 1) <= 0) return 0;
    bool lessEqual = particleTarget.w > .5;
    uint incomingCode = ParticleD24Code(i.pos.z);
    float radius = ParticleActiveRadius(i);
    if (radius <= 0)
        return float2(ParticleDepthPassCode(incomingCode, ParticleRawDepth(pixel, 0), lessEqual),
                      ParticleDepthPassCode(incomingCode, ParticleRawDepth(pixel, 1), lessEqual));
    uint2 bounds = particleDepthBounds.Load(int3(pixel, 0));
    if (bounds.x <= bounds.y && bounds.y <= 0xffffffu &&
        (lessEqual ? incomingCode <= bounds.x : incomingCode < bounds.x)) return 1;
    float depth0 = ParticleRawDepth(pixel, 0), depth1 = ParticleRawDepth(pixel, 1);
    float2 first = ParticleOccluderTransmission(pixel, depth0, incomingCode, radius, low, high, lessEqual, -1);
    if (asuint(depth0) == asuint(depth1)) return first.xx;
    float2 second = ParticleOccluderTransmission(pixel, depth1, incomingCode, radius, low, high, lessEqual, first.y);
    return float2(first.x, second.x);
}
#elif PARTICLE_DOF_PASS == 2 || PARTICLE_DOF_PASS == 4 || PARTICLE_DOF_PASS == 5
Texture2D<float4> particleFilteredAtlas : register(t1);
float4 ParticleAtlasLoad(int2 p, int2 low, int2 high) {
    return all(p >= low) && all(p < high) ? particleFilteredAtlas.Load(int3(p, 0)) : 0;
}
float4 ParticleAtlasComposite(VOut i, bool front) {
    if (i.particleQuad >= 1024 || i.particleQuad >= (uint)particleTarget.z) {
#if PARTICLE_DOF_PASS == 2
        return ParticleLegacy(i, front);
#else
        return 0; // The shared-field path requires every page quad to be mapped.
#endif
    }
    ParticleDofQuad quad = particleQuads[i.particleQuad];
    if (quad.atlas_scale != 1 && quad.atlas_scale != .5) {
#if PARTICLE_DOF_PASS == 2
        return ParticleLegacy(i, front);
#else
        return 0;
#endif
    }
    int2 origin = int2(ParticleUnpack(quad.atlas_xy));
    int2 high = origin + int2(ParticleUnpack(quad.atlas_size));
    float2 atlas = origin + 2 + (i.pos.xy - ParticleUnpack(quad.screen_xy)) * quad.atlas_scale - .5;
    int2 p = int2(floor(atlas));
    float2 f = frac(atlas);
    // Interpolate premultiplied energy/coverage, not straight color; every contributor is cell-bounded.
    float4 value = lerp(lerp(ParticleAtlasLoad(p, origin, high),
                              ParticleAtlasLoad(p + int2(1, 0), origin, high), f.x),
                        lerp(ParticleAtlasLoad(p + int2(0, 1), origin, high),
                              ParticleAtlasLoad(p + int2(1, 1), origin, high), f.x), f.y);
#if PARTICLE_DOF_BLEND == 1
    float alpha = sqrt(max(value.a, 0));
#else
    float alpha = max(value.a, 0);
#endif
    return alpha > 0 ? float4(value.rgb / alpha, alpha) : 0;
}
#if PARTICLE_DOF_PASS == 5
float main(VOut i, bool front : SV_IsFrontFace) : SV_Target {
    // Native2x raster coverage writes a conservative page union. No depth,
    // stencil or blending is enabled, so later quads cannot erase support.
    if (!(ParticleAtlasComposite(i, front).a > 0)) discard;
    return 1;
}
#elif PARTICLE_DOF_PASS == 4
ParticleFinal main(VOut i, bool front : SV_IsFrontFace, uint rasterCoverage : SV_Coverage) {
    ParticleFinal output = (ParticleFinal)0;
    float4 color = ParticleAtlasComposite(i, front);
    if (color.a <= 0) return output;
    float2 transmission = particleTransmissionField.Load(int3(int2(i.pos.xy), 0));
    if (any((asuint(transmission) & 0x7f800000u) == 0x7f800000u) ||
        any(transmission < 0) || any(transmission > 1)) return output;
    uint originalCoverage = rasterCoverage & 3u, finalCoverage = 0;
    float total = 0;
    if ((originalCoverage & 1u) && transmission.x > 0) { finalCoverage |= 1u; total += transmission.x; }
    if ((originalCoverage & 2u) && transmission.y > 0) { finalCoverage |= 2u; total += transmission.y; }
    if (!finalCoverage) return output;
    float2 coverage = float2(finalCoverage, saturate(total / countbits(finalCoverage)));
    output.color = ParticleTransmit(color, coverage);
    output.coverage = finalCoverage;
    return output;
}
#else
ParticleFinal main(VOut i, bool front : SV_IsFrontFace, uint rasterCoverage : SV_Coverage) {
    float4 color = ParticleAtlasComposite(i, front);
    float2 coverage = ParticleCoverage(i, color.a, rasterCoverage);
    ParticleFinal output = (ParticleFinal)0;
    output.color = ParticleTransmit(color, coverage);
    output.coverage = (uint)coverage.x;
    return output;
}
#endif
#else
ParticleFinal main(VOut i, bool front : SV_IsFrontFace, uint rasterCoverage : SV_Coverage) {
    float4 color = ParticleLegacy(i, front);
    float2 coverage = ParticleCoverage(i, color.a, rasterCoverage);
    ParticleFinal output = (ParticleFinal)0;
    output.color = ParticleTransmit(color, coverage);
    output.coverage = (uint)coverage.x;
    return output;
}
#endif
)";

// Filter-only b0 is independent of the native vertex constants. Each packed float4 contains four radii
// already scaled into atlas pixels. b1 keeps the production DrawConstants prefix and shared metadata ABI.
inline std::string ParticleDofAtlasFilterSource(bool small_radius, bool interior) {
    return std::string(kParticleDofQuadHlsl) + R"(
cbuffer ParticleAtlasRadii : register(b0) { float4 particleRadii[256]; };
cbuffer ParticleAtlasDraw : register(b1) { float4 drawPadding[131];
)" + kParticleDofCbufferFields + R"(
};
)" + kParticleDofDiskHlsl + R"(
Texture2D<float4> particleSourceAtlas : register(t0);)" +
        (small_radius ? std::string(kParticleDofSmallRadiusHlsl) : std::string{}) + R"(
SamplerState particleAtlasSampler : register(s0);
struct AtlasOut { float4 pos : SV_Position; nointerpolation uint quad : TEXCOORD0; };
float2 AtlasUnpack(uint packed) { return float2(packed & 65535u, packed >> 16); }
AtlasOut vs(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID) {
    AtlasOut o;
    o.quad = (uint)particleAtlasTarget.z + instanceId;
    o.pos = float4(2, 2, 0, 1);
    if (instanceId >= (uint)particleAtlasTarget.w || o.quad >= 1024 ||
        o.quad >= (uint)particleTarget.z) return o;
    ParticleDofQuad quad = particleQuads[o.quad];
    if (quad.atlas_scale <= 0) return o;
    const float2 corners[6] = {float2(0, 0), float2(0, 1), float2(1, 1),
                              float2(0, 0), float2(1, 1), float2(1, 0)};
    float2 pixel = AtlasUnpack(quad.atlas_xy) + corners[vertexId % 6] * AtlasUnpack(quad.atlas_size);
    o.pos.xy = pixel / particleAtlasTarget.xy * float2(2, -2) + float2(-1, 1);
    return o;
}
float4 filter_ps(AtlasOut i) : SV_Target {
    ParticleDofQuad quad = particleQuads[i.quad];
    float2 low = AtlasUnpack(quad.atlas_xy), high = low + AtlasUnpack(quad.atlas_size);
    // Write the entire allocated cell, including transparent guards; no separate filtered-atlas clear.
    if (any(i.pos.xy < low + 2) || any(i.pos.xy >= high - 2)) return 0;
    float radius = particleRadii[i.quad / 4][i.quad % 4];
    if ((asuint(radius) & 0x7f800000u) == 0x7f800000u || radius < 0) return 0;)" +
        (small_radius ? std::string("\n    if (radius <= 1.0) return ParticleSmallRadiusFilter(i.pos.xy, radius);\n") : std::string{}) +
        (interior ? std::string(R"(
    // Every original disk sample and its bilinear footprint are inside this cell.
    // The edge path below retains its original rejection, clamping and denominator.
    [branch] if (all(i.pos.xy - radius >= low + .5) && all(i.pos.xy + radius <= high - .5)) {
        float2 centerUV = i.pos.xy / particleAtlasTarget.xy;
        float2 radiusUV = radius / particleAtlasTarget.xy;
        float4 interiorSum = 0;
        [loop] for (int tap = 0; tap < PARTICLE_DOF_TAPS; ++tap)
            interiorSum += particleSourceAtlas.SampleLevel(particleAtlasSampler,
                centerUV + particleDisk[tap] * radiusUV, 0);
        return interiorSum / PARTICLE_DOF_TAPS;
    }
)") : std::string{}) + R"(
    float4 sum = 0;
    [loop] for (int tap = 0; tap < PARTICLE_DOF_TAPS; ++tap) {
        float2 source = i.pos.xy + particleDisk[tap] * radius;
        if (any(source < low) || any(source >= high)) continue;
        // Cell guards are zero. Clamp the complete bilinear footprint to this cell, so a tap immediately
        // inside its outer edge cannot read an adjacent particle even though the atlas sampler is global.
        source = clamp(source, low + .5, high - .5);
        sum += particleSourceAtlas.SampleLevel(particleAtlasSampler, source / particleAtlasTarget.xy, 0);
    }
    return sum / PARTICLE_DOF_TAPS;
}
)";
}

// Small apertures combine the same bilinear weights; interior pixels retain all 64 disk taps.
inline const std::string kParticleDofAtlasFilterHlsl = ParticleDofAtlasFilterSource(true, true);

} // namespace gpu
