#pragma once

namespace gpu {

// Render once per current MS depth snapshot into R32G32_FLOAT. No filtering, resolution change or depth
// conversion is allowed: these bounds only bypass the raw sample loop when every comparison agrees.
inline constexpr char kParticleDofDepthRangeHlsl[] = R"(
Texture2DMS<float> particleRangeDepth : register(t3);
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float2 range_ps(float4 pos : SV_Position) : SV_Target {
    float minimum = 1, maximum = 0;
    bool valid = true;
    [unroll] for (uint sampleIndex = 0; sampleIndex < PARTICLE_DOF_SAMPLES; ++sampleIndex) {
        float depth = particleRangeDepth.Load(int2(pos.xy), sampleIndex);
        valid = valid && (asuint(depth) & 0x7f800000u) != 0x7f800000u && depth >= 0 && depth <= 1;
        minimum = min(minimum, depth);
        maximum = max(maximum, depth);
    }
    // R24_UNORM is always valid. The sentinel also keeps generic test inputs and future formats safe.
    return valid ? float2(minimum, maximum) : float2(-1, -1);
}
)";

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
#ifndef PARTICLE_DOF_BLEND
#define PARTICLE_DOF_BLEND 0
#endif
#if PARTICLE_DOF_SAMPLES > 1
Texture2DMS<float> particleSceneDepth : register(t3);
// Exact R32 min/max of every sample in the current R24 depth snapshot. Never a resolved/half-float depth.
Texture2D<float2> particleDepthBounds : register(t2);
#else
Texture2D<float> particleSceneDepth : register(t3);
#endif

bool ParticleFinite(float x) { return (asuint(x) & 0x7f800000u) != 0x7f800000u; }
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
float ParticleVisible(float2 sourcePixel, float particleDepth, float2 low, float2 high, bool lessEqual) {
    if (any(sourcePixel < low) || any(sourcePixel >= high)) return 0;
    int2 p = int2(sourcePixel);
#if PARTICLE_DOF_SAMPLES > 1
    float2 bounds = particleDepthBounds.Load(int3(p, 0));
    if (ParticleFinite(bounds.x) && ParticleFinite(bounds.y) && bounds.x >= 0 &&
        bounds.y <= 1 && bounds.x <= bounds.y) {
        if (lessEqual ? particleDepth <= bounds.x : particleDepth < bounds.x) return 1;
        if (lessEqual ? particleDepth > bounds.y : particleDepth >= bounds.y) return 0;
    }
#endif
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
            visible += lessEqual ? (particleDepth <= opaque) : (particleDepth < opaque);
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
)";

} // namespace gpu
