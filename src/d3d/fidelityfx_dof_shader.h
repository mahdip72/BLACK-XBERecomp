#pragma once

namespace gpu {

// Input/output adapter only. Bokeh filtering is performed by the unmodified official FidelityFX kernels.
inline constexpr char kFidelityFxDofHlsl[] = R"(
cbuffer Dof : register(b1) { float4 zp; float4 vp; float4 tuning; };
#ifdef FFX_DOF_PRESENT
Texture2D<float4> image : register(t0);
Texture2D<float4> prepared : register(t1);
#else
#ifdef MS
Texture2DMS<float> depth : register(t0);
#else
Texture2D<float> depth : register(t0);
#endif
Texture2D<float4> scene : register(t1);
#endif

float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}

#ifdef FFX_DOF_PRESENT
float4 ffx_dof_present(float4 pos : SV_Position) : SV_Target {
    int3 p = int3(int2(pos.xy), 0);
    // Invalid original depth stays untouched. Destination alpha is protected by the RGB-only PSO mask.
    if (prepared.Load(p).a <= 0) discard;
    float3 rgb = image.Load(p).rgb;
    if (!all(isfinite(rgb))) discard;
    return float4(rgb, 0);
}
#else
float ViewZ(float raw) {
    if (!isfinite(raw) || raw < 0 || raw > 1) return 0;
    float denominator = raw - zp.y;
    if (abs(denominator) < 1e-7) return 1e6;
    float z = zp.x / denominator;
    return isfinite(z) && z > 0 ? min(z, 1e6) : 0;
}

float Coc(float z) {
    float d = z - zp.z;
    float c = clamp(sign(d) * max(abs(d) - zp.z * 0.15, 0) / z, -1, 1);
    return c * zp.w * (c < 0 ? tuning.x : tuning.y);
}

struct Prepared { float4 color : SV_Target0; float depth : SV_Target1; };
Prepared ffx_dof_prepare(float4 pos : SV_Position) {
    // Extend the viewport before the SDK's downsample/filter passes. Copyback scissors the original viewport.
    int2 p = int2(clamp(pos.xy, vp.xy, vp.xy + vp.zw - 1));
    float z = 1e7;
#ifdef MS
    [unroll] for (int i = 0; i < DOF_SAMPLES; i++) {
        float sample_z = ViewZ(depth.Load(p, i));
        if (sample_z > 0) z = min(z, sample_z);
    }
#else
    z = ViewZ(depth.Load(int3(p, 0)));
#endif
    bool valid = z > 0 && z <= 1e6;
    Prepared result;
    // SDK uses RGB and generates its own opacity. Resolved color plus one nearest depth is its MSAA adapter;
    // it does not reproduce the Balanced filter's per-sample foreground coverage at mixed MSAA silhouettes.
    result.color = float4(scene.Load(int3(p, 0)).rgb, valid ? 1 : 0);
    float c = valid ? Coc(z) : 0;
    result.depth = saturate(0.5 + c / (2 * tuning.z));
    return result;
}
#endif
)";

}  // namespace gpu
