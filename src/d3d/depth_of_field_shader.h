#pragma once

// A circular aperture gather. Near and far layers have separate coverage, so a far sample cannot wash over an
// in-focus foreground edge. The original reload path draws the first-person models and HUD after this pass.
namespace gpu {

inline constexpr char kDepthOfFieldHlsl[] = R"(
// zp: view z = x / (raw depth - y), focus distance, maximum circle-of-confusion radius in target pixels.
// vp: viewport origin and size in target pixels. Constants have already been checked on the CPU.
cbuffer Dof : register(b1) { float4 zp; float4 vp; };
#ifdef MS
Texture2DMS<float> depth : register(t0);
float Raw(int2 p) {
    float z = depth.Load(p, 0);
    [unroll] for (int i = 1; i < DOF_SAMPLES; i++) z = min(z, depth.Load(p, i));
    return z;
}
#else
Texture2D<float> depth : register(t0);
float Raw(int2 p) { return depth.Load(int3(p, 0)); }
#endif
Texture2D<float4> scene : register(t1);

float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}

float ViewZ(int2 p) {
    float raw = Raw(p);
    if (!isfinite(raw) || raw < 0 || raw > 1) return 0;
    float denominator = raw - zp.y;
    // An infinite far plane's clear depth is sky, with the largest far circle.
    if (abs(denominator) < 1e-7) return 1e6;
    float z = zp.x / denominator;
    return isfinite(z) && z > 0 ? min(z, 1e6) : 0;
}

float Coc(float z) {
    // Keep a small band about the focus plane sharp. The sign separates foreground from background bokeh.
    float d = z - zp.z;
    return clamp(sign(d) * max(abs(d) - zp.z * 0.15, 0) / z, -1, 1) * zp.w;
}

float4 depth_of_field(float4 pos : SV_Position) : SV_Target {
    int2 p = int2(pos.xy);
    float4 original = scene.Load(int3(p, 0));
    float z = ViewZ(p);
    if (z <= 0 || zp.w < 0.75) return original;
    float center = Coc(z);
    float3 far_sum = 0;
    float far_weight = 0;
    float3 near_sum = 0;
    float near_weight = 0;
    const int taps = 48;
    [unroll] for (int i = 0; i < taps; i++) {
        // Fixed disk samples avoid temporal noise and need no history across reloads, resizes or camera cuts.
        float angle = i * 2.39996323;
        float radius = sqrt(float(i) / (taps - 1)) * zp.w;
        int2 q = p + int2(round(float2(cos(angle), sin(angle)) * radius));
        // Omit taps beyond this viewport; clamping them would accumulate an artificial bright border.
        if (any(q < int2(vp.xy)) || any(q >= int2(vp.xy + vp.zw))) continue;
        float sample_z = ViewZ(q);
        if (sample_z <= 0) continue;
        float sample_coc = Coc(sample_z);
        float distance = length(float2(q - p));
        // Background samples only mix within a continuous depth surface; near silhouettes use their own layer.
        float edge = saturate(1 - abs(sample_z - z) / max(z * 0.05, 0.05));
        float fw = center > 0 && sample_coc > 0
            ? saturate(min(center, sample_coc) - distance + 0.5) * edge : 0;
        // Foreground circles may spread over farther pixels, but a farther circle cannot cover a nearer one.
        float nw = sample_coc < -0.75 && sample_z <= z + max(z * 0.01, 0.01)
            ? saturate(-sample_coc - distance + 0.5) / max(sample_coc * sample_coc, 1) : 0;
        if (fw > 0 || nw > 0) {
            float3 color = scene.Load(int3(q, 0)).rgb;
            far_sum += color * fw;
            far_weight += fw;
            near_sum += color * nw;
            near_weight += nw;
        }
    }
    float3 color = far_weight > 0 ? lerp(original.rgb, far_sum / far_weight, saturate(center - 0.5)) : original.rgb;
    // Normalize colors separately from coverage. Constant colors retain their energy, including at image edges.
    float coverage = saturate(near_weight * zp.w * zp.w / taps);
    if (center < -0.75) coverage = max(coverage, saturate(-center - 0.5));
    if (near_weight > 0) color = lerp(color, near_sum / near_weight, coverage);
    // Leave untouched MSAA samples intact wherever no circle contributes.
    if (center <= 0.5 && coverage == 0) discard;
    return float4(color, original.a);
}
)";

}  // namespace gpu
