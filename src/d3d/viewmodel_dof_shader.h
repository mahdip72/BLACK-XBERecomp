#pragma once

namespace gpu {

inline constexpr unsigned kViewmodelDofTileSize = 8;

// Interior-only softening for depth-writing first-person geometry. The renderer captures depth immediately
// before the early gun draw and invokes this pass only during normal gameplay, before the HUD.
inline constexpr char kViewmodelDofHlsl[] = R"(
// zp = projection numerator, projection depth offset, amount [0,1], target height / 1080.
// The projection numerator uses the renderer's view-depth scale; these depths do not claim physical metres.
// vp = viewport origin and size. Sparse pack requires both complete targets cleared to zero first.
// focus = sharp depth, transition width, target width, target height. Depths use the reconstructed view scale.
// encoding.x = captured native NearDepth plane, or zero for legacy depth.
cbuffer ViewmodelDof : register(b1) { float4 zp; float4 vp; float4 focus; float4 encoding; };
#ifdef MS
Texture2DMS<float> after_depth : register(t0);
Texture2DMS<float> before_depth : register(t1);
Texture2DMS<float4> raw_color : register(t2);
float After(int2 p, int i) { return after_depth.Load(p, i); }
float Before(int2 p, int i) { return before_depth.Load(p, i); }
float3 RawColor(int2 p, int i) { return raw_color.Load(p, i).rgb; }
#else
#ifndef DOF_SAMPLES
#define DOF_SAMPLES 1
#endif
Texture2D<float> after_depth : register(t0);
Texture2D<float> before_depth : register(t1);
Texture2D<float4> raw_color : register(t2);
float After(int2 p, int i) { return after_depth.Load(int3(p, 0)); }
float Before(int2 p, int i) { return before_depth.Load(int3(p, 0)); }
float3 RawColor(int2 p, int i) { return raw_color.Load(int3(p, 0)).rgb; }
#endif
// These aliases are used only by the final entry point; unused pack resources are eliminated at compile time.
Texture2D<float4> gun_color : register(t0);
Texture2D<float> gun_depth : register(t1);
Texture2D<float4> resolved_scene : register(t2);
Texture2D<float> weapon_tiles : register(t3);
struct Packed { float4 color : SV_Target0; float depth : SV_Target1; };

float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}

// Replayed native weapon geometry supplies the VS/IA/constants. Conservative
// rasterization marks a superset at 1/8 resolution without reading scene depth.
float viewmodel_dof_coverage() : SV_Target { return 1; }

// Test-only depth-mask oracle; the renderer must not dispatch this serial scan.
// The tile classifier is conservative: invalid changed samples may activate a
// tile, but every sample accepted by ChangedDepth necessarily activates it.
// It scans actual paired depth, never an assumed on-screen weapon rectangle.
float viewmodel_dof_tiles(float4 pos : SV_Position) : SV_Target {
    int2 first = int2(pos.xy) * 8;
    [loop] for (int y = 0; y < 8; ++y) {
        [loop] for (int x = 0; x < 8; ++x) {
            int2 p = first + int2(x, y);
            float2 center = float2(p) + 0.5;
            if (any(p >= int2(focus.zw)) || any(center < vp.xy) || any(center >= vp.xy + vp.zw)) continue;
            [unroll] for (int s = 0; s < DOF_SAMPLES; ++s)
                if (asuint(After(p, s)) != asuint(Before(p, s))) return 1;
        }
    }
    return 0;
}

// Draw six vertices per tile, one instance for every tile. Empty tiles become
// degenerate triangles before rasterization. All changed tiles are packed,
// including focused geometry that neighboring blur taps may need; no dilation
// is required because the original effect does not expand the gun silhouette.
// Use this VS for pack at every sample count, but composition only at 1x.
// MSAA composition must retain the full-screen VS: its resolved-RGB broadcast
// into unchanged world samples affects later coverage-dependent blending.
float4 viewmodel_dof_tiles_vs(uint id : SV_VertexID, uint instance : SV_InstanceID) : SV_Position {
    uint columns, rows;
    weapon_tiles.GetDimensions(columns, rows);
    uint2 tile = uint2(instance % columns, instance / columns);
    if (tile.y >= rows || weapon_tiles.Load(int3(tile, 0)) == 0) return float4(2, 2, 0, 1);
    static const uint2 corners[6] = {
        uint2(0, 0), uint2(1, 0), uint2(0, 1),
        uint2(0, 1), uint2(1, 0), uint2(1, 1)
    };
    float2 pixel = min(float2((tile + corners[id]) * 8), focus.zw);
    return float4(pixel / focus.zw * float2(2, -2) + float2(-1, 1), 0, 1);
}

float ChangedDepth(int2 p, int sample_index) {
    float raw = After(p, sample_index);
    float before = Before(p, sample_index);
    if (!isfinite(raw) || !isfinite(before) || raw < 0 || raw >= 1 || before < 0 || before > 1 || raw == before)
        return 0;
    if ((asuint(encoding.x) & 0x7f800000u) == 0x7f800000u || encoding.x < 0) return 0;
    if (encoding.x > 0) {
        const float front_share = 1.0 / 4096;
        if (raw < front_share) {
            float front_z = encoding.x * (raw / front_share);
            return front_z > 0 && front_z <= 60000 ? front_z : 0;
        }
        raw = (raw - front_share) / (1 - front_share);
    }
    float denominator = raw - zp.y;
    if (abs(denominator) < 1e-7) return 0;
    float z = zp.x / denominator;
    return isfinite(z) && z > 0 && z <= 60000 ? z : 0;
}

Packed viewmodel_dof_pack(float4 pos : SV_Position) {
    Packed result = (Packed)0;
    if (any(pos.xy < vp.xy) || any(pos.xy >= vp.xy + vp.zw)) return result;
    int2 p = int2(pos.xy);
    float nearest = 60001;
    [unroll] for (int i = 0; i < DOF_SAMPLES; i++) {
        float z = ChangedDepth(p, i);
        if (z > 0) nearest = min(nearest, z);
    }
    if (nearest > 60000) return result;
    float4 sum = 0;
    [unroll] for (int j = 0; j < DOF_SAMPLES; j++) {
        float z = ChangedDepth(p, j);
        float3 color = RawColor(p, j);
        // Keep the nearest continuous changed surface, with exact MSAA covered area. Unchanged world samples
        // never enter this color, even when they share a pixel with the gun silhouette.
        if (z > 0 && abs(z - nearest) <= max(nearest * 0.05, 0.001) && all(isfinite(color)))
            sum += float4(color, 1);
    }
    if (sum.a > 0) {
        result.color = float4(sum.rgb / sum.a, sum.a / DOF_SAMPLES);
        result.depth = nearest;
    }
    return result;
}

float4 GuardedTexel(int2 p, float z) {
    if (any(p < vp.xy) || any(p >= vp.xy + vp.zw)) return 0;
    float4 color = gun_color.Load(int3(p, 0));
    float depth = gun_depth.Load(int3(p, 0));
    if (depth <= 0 || color.a <= 0) return 0;
    float weight = color.a * saturate(1 - abs(depth - z) / max(z * 0.10, 0.001));
    return float4(color.rgb * weight, weight);
}

// Guard every bilinear contributor before interpolation; a nearest-depth test with an ordinary bilinear
// color sample would still contaminate edges with neighboring world or differently deep gun parts.
float4 Gather(float2 q, float z) {
    int2 p = int2(floor(q));
    float2 f = frac(q);
    return GuardedTexel(p, z) * ((1 - f.x) * (1 - f.y))
         + GuardedTexel(p + int2(1, 0), z) * (f.x * (1 - f.y))
         + GuardedTexel(p + int2(0, 1), z) * ((1 - f.x) * f.y)
         + GuardedTexel(p + 1, z) * (f.x * f.y);
}

float4 viewmodel_dof(float4 pos : SV_Position) : SV_Target {
    int2 p = int2(pos.xy);
    float4 original = resolved_scene.Load(int3(p, 0));
    float4 center = gun_color.Load(int3(p, 0));
    float z = gun_depth.Load(int3(p, 0));
    if (zp.z <= 0 || z <= 0 || center.a <= 0 || any(pos.xy < vp.xy) || any(pos.xy >= vp.xy + vp.zw))
        return original;
    // Inspect exponent bits so the NaN/Inf guard survives shader optimization without IEEE strictness.
    if (any((asuint(focus.xy) & 0x7f800000u) == 0x7f800000u)) return original;
    if (focus.x <= 0 || focus.y <= 0 || focus.y > focus.x || z >= focus.x)
        return original;
    // Full strength before the transition, smoothly fading to exact sharpness at the selected depth.
    // A transition as wide as the sharp depth starts at zero; there is no residual blur floor at the tip.
    float start = max(focus.x - focus.y, 0);
    float falloff = 1 - smoothstep(start, focus.x, z);
    float radius = 6 * saturate(zp.z) * max(zp.w, 0) * falloff;
    if (radius <= 0) return original;
    static const float2 disk[12] = {
        float2(.35, 0), float2(0, .35), float2(-.35, 0), float2(0, -.35),
        float2(.85, 0), float2(.60104, .60104), float2(0, .85), float2(-.60104, .60104),
        float2(-.85, 0), float2(-.60104, -.60104), float2(0, -.85), float2(.60104, -.60104)
    };
    float4 sum = float4(center.rgb * center.a, center.a);
    [loop] for (int i = 0; i < 12; i++) sum += Gather(p + disk[i] * radius, z);
    float3 blurred = sum.rgb / sum.a;
    // Replace only the covered gun contribution. No silhouette expansion, world blur, or alpha change.
    return float4(original.rgb + center.a * (blurred - center.rgb), original.a);
}
)";

}  // namespace gpu
