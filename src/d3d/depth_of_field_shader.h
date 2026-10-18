#pragma once

// A circular aperture gather. Near and far layers have separate coverage, so a far sample cannot wash over an
// in-focus foreground edge. The original reload path draws the first-person models and HUD after this pass.
namespace gpu {

inline constexpr char kDepthOfFieldHlsl[] = R"(
// zp: view z = x / (raw depth - y), focus distance, maximum circle-of-confusion radius in target pixels.
// vp: viewport origin and size in target pixels. Constants have already been checked on the CPU.
cbuffer Dof : register(b1) { float4 zp; float4 vp; float4 tuning; };
#ifdef DOF_COMPOSE
Texture2D<float> depth : register(t0);
Texture2D<float4> scene : register(t1);
Texture2DArray<float4> layers : register(t2);
Texture2D<float> foreground_depth : register(t3);
#else
#ifdef MS
Texture2DMS<float> depth : register(t0);
Texture2DMS<float4> scene : register(t1);
float4 Scene(int2 p) { return scene.Load(p, 0); }
float Raw(int2 p) {
    float z = depth.Load(p, 0);
    [unroll] for (int i = 1; i < DOF_SAMPLES; i++) z = min(z, depth.Load(p, i));
    return z;
}
#else
Texture2D<float> depth : register(t0);
Texture2D<float4> scene : register(t1);
float4 Scene(int2 p) { return scene.Load(int3(p, 0)); }
float Raw(int2 p) { return depth.Load(int3(p, 0)); }
#endif
// RGB and covered area, kept at half precision to avoid banding in blurred gradients.
Texture2D<float4> filtered_color : register(t2);
// Positive linear view depth. Zero depth/coverage marks invalid pixels.
Texture2D<float> filtered_depth : register(t3);
struct Filter { float4 color : SV_Target0; float depth : SV_Target1; };
struct Layers { float4 background; float4 foreground; float background_depth; float foreground_depth; };
struct BlurLayers { float4 background : SV_Target0; float4 foreground : SV_Target1; float depth : SV_Target2; };
#endif

float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}

float Coc(float z) {
    // Keep a small band about the focus plane sharp. Near/far strengths are independent live controls.
    float d = z - zp.z;
    float c = clamp(sign(d) * max(abs(d) - zp.z * 0.15, 0) / z, -1, 1);
    return c * zp.w * (c < 0 ? tuning.x : tuning.y);
}

#ifdef DOF_COMPOSE
float DepthSlope(float before, float center, float after) {
    if (before <= 0 || after <= 0) return 0;
    float a = 1 / center - 1 / before;
    float b = 1 / after - 1 / center;
    // A thin feature or silhouette has opposing one-sided slopes. Do not extrapolate across that edge.
    return a * b > 0 ? (abs(a) < abs(b) ? a : b) : 0;
}

float3 FallbackBlur(int2 p, float z, float radius, float3 original) {
    int2 low = int2(vp.xy), high = int2(vp.xy + vp.zw) - 1;
    float left = depth.Load(int3(clamp(p - int2(1, 0), low, high), 0));
    float right = depth.Load(int3(clamp(p + int2(1, 0), low, high), 0));
    float above = depth.Load(int3(clamp(p - int2(0, 1), low, high), 0));
    float below = depth.Load(int3(clamp(p + int2(0, 1), low, high), 0));
    float2 slope = float2(DepthSlope(left, z, right), DepthSlope(above, z, below));
    float tolerance = max(z * 0.05, 0.05);
    bool along_x = left > 0 && right > 0 && max(abs(left - z), abs(right - z)) < tolerance;
    bool along_y = above > 0 && below > 0 && max(abs(above - z), abs(below - z)) < tolerance;
    float4 sum = 0;
    // A feature can fall between all half-resolution receivers. Only those starved, defocused pixels use this
    // bounded local filter. Inverse depth predicts a continuous plane without relaxing silhouette rejection.
    [loop] for (int i = 0; i < 8; i++) {
        float angle = i * 2.39996323;
        float2 offset = float2(cos(angle), sin(angle)) * sqrt((i + 0.5) / 8) * radius;
        // A thin surface has no usable support across its silhouette. Spend the same eight taps along its
        // continuous axis instead of leaving only two ring taps, which alias alternating texture strongly.
        if (along_x != along_y)
            offset = (along_x ? float2(1, 0) : float2(0, 1)) * (((i + 0.5) / 8) * 2 - 1) * radius;
        float2 q = clamp(p + offset, low, high);
        int2 lo = int2(floor(q));
        float2 f = frac(q);
        [unroll] for (int y = 0; y < 2; y++) {
            [unroll] for (int x = 0; x < 2; x++) {
                int2 s = clamp(lo + int2(x, y), low, high);
                float sz = depth.Load(int3(s, 0));
                float expected = 1 / max(1 / z + dot(slope, s - p), 1e-6);
                float w = (x ? f.x : 1 - f.x) * (y ? f.y : 1 - f.y);
                w *= sz > 0 ? saturate(1 - abs(sz - expected) / max(expected * 0.05, 0.05)) : 0;
                sum += float4(scene.Load(int3(s, 0)).rgb * w, w);
            }
        }
    }
    return sum.a > 0 ? sum.rgb / sum.a : original;
}

float4 depth_of_field(float4 pos : SV_Position) : SV_Target {
    int2 p = int2(pos.xy);
    float4 original = scene.Load(int3(p, 0));
    float z = depth.Load(int3(p, 0));
    if (z <= 0 || zp.w <= 0) return original;
    uint width, height, half_width, half_height, slices;
    depth.GetDimensions(width, height);
    layers.GetDimensions(half_width, half_height, slices);
    float2 q = pos.xy * float2(half_width, half_height) / float2(width, height) - 0.5;
    int2 lo = int2(floor(q));
    float2 f = frac(q);
    float center = Coc(z);
    float local_mix = smoothstep(0.25, 1.25, abs(center));
    float4 background = 0;
    float4 foreground = 0;
    [unroll] for (int y = 0; y < 2; y++) {
        [unroll] for (int x = 0; x < 2; x++) {
            int2 s = clamp(lo + int2(x, y), 0, int2(half_width, half_height) - 1);
            float w = (x ? f.x : 1 - f.x) * (y ? f.y : 1 - f.y);
            float4 b = layers.Load(int4(s, 0, 0));
            float4 n = layers.Load(int4(s, 1, 0));
            float nz = foreground_depth.Load(int3(s, 0));
            // Background reconstruction stays on the receiver's surface, or behind a blurred near receiver.
            float bg_guard = center < 0 ? saturate(1 - max(z - b.a, 0) / max(z * 0.05, 0.05)) :
                saturate(1 - abs(b.a - z) / max(z * 0.05, 0.05));
            float bw = b.a > 0 ? w * bg_guard : 0;
            background += float4(b.rgb * bw, bw);
            // The guide is the FURTHEST contributor. Rejecting it cannot expose a farther foreground surface
            // through a nearer receiver. Rejected foreground weights remain zero: never inflate their coverage.
            float nw = nz > 0 ? w * saturate(1 - max(nz - z, 0) / max(z * 0.01, 0.01)) : 0;
            foreground += n * nw;
        }
    }
    float3 blurred = background.a > 0 ? background.rgb / background.a : original.rgb;
    if (background.a == 0 && local_mix > 0) {
        if (center < 0 && foreground.a > 0) blurred = foreground.rgb / foreground.a;
        else blurred = FallbackBlur(p, z, abs(center), original.rgb);
    }
    float3 color = lerp(original.rgb, blurred, local_mix);
    float coverage = saturate(foreground.a);
    if (foreground.a > 0) color = color * (1 - coverage) + foreground.rgb * (coverage / foreground.a);
    if (local_mix == 0 && coverage == 0) discard;
    return float4(color, original.a);
}
#else
)" R"(
float ViewZ(float raw) {
    if (!isfinite(raw) || raw < 0 || raw > 1) return 0;
    float denominator = raw - zp.y;
    // An infinite far plane's clear depth is sky, with the largest far circle.
    if (abs(denominator) < 1e-7) return 1e6;
    float z = zp.x / denominator;
    return isfinite(z) && z > 0 ? min(z, 1e6) : 0;
}

// Initialize BOTH whole mips with viewport-edge extension. This keeps prefiltered coverage intact at partial
// viewport boundaries; the final pass still writes only the original viewport.
Filter depth_of_field_pack(float4 pos : SV_Position) {
    Filter result = (Filter)0;
    int2 p = int2(clamp(pos.xy, vp.xy, vp.xy + vp.zw - 1));
    float z = ViewZ(Raw(p));
    if (z > 0) {
#ifdef MS
        // Resolved color mixes different surfaces. Keep the closest continuous MSAA surface's color and area.
        float4 sum = 0;
        [unroll] for (int i = 0; i < DOF_SAMPLES; i++) {
            float sample_z = ViewZ(depth.Load(p, i));
            float w = sample_z > 0 ? saturate(1 - abs(sample_z - z) / max(z * 0.05, 0.05)) : 0;
            sum += float4(scene.Load(p, i).rgb * w, w);
        }
        result.color = float4(sum.rgb / sum.a, sum.a / DOF_SAMPLES);
#else
        result.color = float4(Scene(p).rgb, 1);
#endif
        result.depth = min(z, 60000);
    }
    return result;
}

Filter depth_of_field_downsample(float4 pos : SV_Position) {
    uint width, height;
    filtered_depth.GetDimensions(width, height);
    // An odd source extent has a fractional 2..3 texel footprint. Area weights retain the last row/column and
    // keep the mip aligned with normalized UVs instead of shifting its edge or dropping those source pixels.
    float2 scale = float2(width, height) / max(uint2(width, height) / 2, 1);
    float2 start = floor(pos.xy) * scale;
    float2 end = start + scale;
    int2 lo = int2(floor(start));
    float3 colors[9];
    float2 surfaces[9];
    float areas[9];
    float z = 60001;
    [unroll] for (int y = 0; y < 3; y++) {
        [unroll] for (int x = 0; x < 3; x++) {
            int i = y * 3 + x;
            int2 p = lo + int2(x, y);
            float2 overlap = max(min(end, p + 1) - max(start, p), 0);
            float area = overlap.x * overlap.y;
            float3 color = 0;
            float2 surface = 0;
            if (area > 0 && all(p < int2(width, height))) {
                float4 packed = filtered_color.Load(int3(p, 0));
                color = packed.rgb;
                surface = float2(filtered_depth.Load(int3(p, 0)), packed.a);
            }
            colors[i] = color;
            surfaces[i] = surface;
            areas[i] = surface.x > 0 ? area * surface.y : 0;
            if (areas[i] > 0) z = min(z, surface.x);
        }
    }
    Filter result = (Filter)0;
    if (z > 60000) return result;
    // Keep the nearest continuous surface. Background color never enters a foreground filter texel.
    float4 sum = 0;
    [unroll] for (int i = 0; i < 9; i++) {
        float w = areas[i] * saturate(1 - abs(surfaces[i].x - z) / max(z * 0.05, 0.05));
        sum += float4(colors[i] * w, w);
    }
    result.color = float4(sum.rgb / sum.a, saturate(sum.a / (scale.x * scale.y)));
    result.depth = z;
    return result;
}

float2 Weight(float2 surface, float z, float center, float distance) {
    float sample_z = surface.x;
    if (sample_z <= 0) return 0;
    float c = Coc(sample_z);
    float aperture = smoothstep(-0.5, 0.5, abs(c) - distance);
    // A foreground circle may cover farther pixels, never a nearer surface. Its area is integrated below.
    float front = saturate(1 - max(sample_z - z, 0) / max(z * 0.01, 0.01));
    float near_weight = c < 0 ? aperture * smoothstep(0.25, 1.25, -c) * front / max(c * c, 1) : 0;
    // A near pixel needs observable background behind its blurred surface, rather than an opaque near core.
    // This estimates hidden color; a solid near surface falls back to its normalized blurred near layer.
    float edge = saturate(1 - abs(sample_z - z) / max(z * 0.05, 0.05));
    float background = center < 0 ? saturate((sample_z - z) / max(z * 0.05, 0.05)) : (c > 0 ? aperture * edge : 0);
    return float2(background, near_weight) * surface.y;
}

// Bilinear color and depth rejection use the same four texels. Applying a guard only to the nearest depth would
// still let the color sampler pull background through a sharp silhouette. Return premultiplied color and weight.
)" R"(
Layers GatherLevel(float2 uv, uint level, float z, float center, float distance) {
    uint width, height, levels;
    filtered_depth.GetDimensions(level, width, height, levels);
    float2 q = uv * float2(width, height) - 0.5;
    int2 lo = clamp(int2(floor(q)), 0, int2(width, height) - 1);
    int2 hi = clamp(int2(floor(q)) + 1, 0, int2(width, height) - 1);
    float2 f = frac(q);
    float4 a = filtered_color.Load(int3(lo, level));
    float4 b = filtered_color.Load(int3(hi.x, lo.y, level));
    float4 c = filtered_color.Load(int3(lo.x, hi.y, level));
    float4 d = filtered_color.Load(int3(hi, level));
    float4 bilinear = float4((1 - f.x) * (1 - f.y), f.x * (1 - f.y), (1 - f.x) * f.y, f.x * f.y);
    float4 depths = float4(filtered_depth.Load(int3(lo, level)), filtered_depth.Load(int3(hi.x, lo.y, level)),
        filtered_depth.Load(int3(lo.x, hi.y, level)), filtered_depth.Load(int3(hi, level)));
    float2 wa = Weight(float2(depths.x, a.a), z, center, distance) * bilinear.x;
    float2 wb = Weight(float2(depths.y, b.a), z, center, distance) * bilinear.y;
    float2 wc = Weight(float2(depths.z, c.a), z, center, distance) * bilinear.z;
    float2 wd = Weight(float2(depths.w, d.a), z, center, distance) * bilinear.w;
    Layers result;
    result.background = float4(a.rgb * wa.x + b.rgb * wb.x + c.rgb * wc.x + d.rgb * wd.x, wa.x + wb.x + wc.x + wd.x);
    result.foreground = float4(a.rgb * wa.y + b.rgb * wb.y + c.rgb * wc.y + d.rgb * wd.y, wa.y + wb.y + wc.y + wd.y);
    result.background_depth = dot(depths, float4(wa.x, wb.x, wc.x, wd.x));
    result.foreground_depth = max(max(wa.y > 0 ? depths.x : 0, wb.y > 0 ? depths.y : 0),
        max(wc.y > 0 ? depths.z : 0, wd.y > 0 ? depths.w : 0));
    return result;
}

Layers Gather(float2 q, float2 inv_size, float lod, float z, float center, float distance) {
    // Extend the edge surface beyond the viewport. Treating offscreen samples as transparent would lower
    // foreground coverage and expose a wide background estimate through an otherwise opaque near interior.
    q = clamp(q, vp.xy, vp.xy + vp.zw - 1);
    float2 uv = (q + 0.5) * inv_size;
    uint level = uint(floor(lod));
    Layers a = GatherLevel(uv, level, z, center, distance);
    float f = frac(lod);
    if (f == 0) return a;
    // Trilinear interpolation combines the individually guarded colors AND weights, never unguarded depth/color.
    Layers b = GatherLevel(uv, level + 1, z, center, distance);
    a.background = lerp(a.background, b.background, f);
    a.foreground = lerp(a.foreground, b.foreground, f);
    a.background_depth = lerp(a.background_depth, b.background_depth, f);
    a.foreground_depth = max(a.foreground_depth, b.foreground_depth);
    return a;
}

BlurLayers depth_of_field_gather(float4 pos : SV_Position) {
    BlurLayers result = (BlurLayers)0;
    uint width, height, levels;
    filtered_depth.GetDimensions(0, width, height, levels);
    float2 scale = float2(width, height) / ((uint2(width, height) + 1) / 2);
    // Extend the half-resolution layers too: an edge compose footprint may include an adjacent receiver just
    // outside the viewport, and an uninitialized/zero layer there would reduce foreground coverage again.
    float2 pixel = clamp(pos.xy * scale - 0.5, vp.xy, vp.xy + vp.zw - 1);
    int2 p = int2(floor(pixel + 0.5));
    float4 original = Scene(p);
    float z = filtered_depth.Load(int3(p, 0));
    if (z <= 0) return result;
    float center = Coc(z);
    float local_mix = smoothstep(0.25, 1.25, abs(center));
    float2 inv_size = 1 / float2(width, height);
    // Prefilter to the disk's sample spacing. Small circles stay at full resolution; large circles use a smooth
    // mip blend so isolated highlights fill the aperture instead of reproducing the fixed sample pattern.
    // Each of N samples covers pi*r*r/N pixels of the disk; its prefilter width is the square root of that area.
    float local_lod = clamp(log2(max(abs(center) / sqrt(64.0 / 3.14159265), 1)), 0, float(levels - 1));
    float inner_area = center * center;
    float near_radius = zp.w * tuning.x;
    float outer_area = max(near_radius * near_radius - inner_area, 0);
    float near_lod = clamp(log2(max(sqrt(outer_area * 3.14159265 / 32), 1)), 0, float(levels - 1));
    float4 background = 0;
    float4 near_layer = 0;
    float background_depth = 0;
    float near_depth = 0;
    if (local_mix > 0) {
        // Every pixel gets a dense disk at its OWN CoC, including during the fade. Floating positions avoid the
        // old integer-tap jumps; fixed angles keep the filter stable without temporal history or random noise.
        [loop] for (int i = 0; i < 64; i++) {
            float angle = i * 2.39996323;
            float radius = sqrt((i + 0.5) / 64) * abs(center);
            float2 q = pixel + float2(cos(angle), sin(angle)) * radius;
            Layers tap = Gather(q, inv_size, local_lod, z, center, radius);
            background += tap.background;
            background_depth += tap.background_depth;
            near_layer += tap.foreground * (inner_area / 64);
            near_depth = max(near_depth, tap.foreground_depth);
        }
    }
    // The outer annulus completes the foreground aperture without counting the inner disk twice.
    float4 outer_background = 0;
    float outer_depth = 0;
    if (outer_area > 0) {
        [loop] for (int j = 0; j < 32; j++) {
            float angle = j * 2.39996323 + 0.7;
            float radius = sqrt(inner_area + ((j + 0.5) / 32) * outer_area);
            float2 q = pixel + float2(cos(angle), sin(angle)) * radius;
            Layers tap = Gather(q, inv_size, near_lod, z, center, radius);
            near_layer += tap.foreground * (outer_area / 32);
            outer_background += tap.background;
            outer_depth += tap.background_depth;
            near_depth = max(near_depth, tap.foreground_depth);
        }
    }
    if (center < 0 && background.a == 0) {
        background = outer_background;
        background_depth = outer_depth;
    }
    float3 color = background.a > 0 ? background.rgb / background.a : original.rgb;
    // No observable background means a solid near surface, not permission to reintroduce its sharp texture.
    if (center < 0 && background.a == 0 && near_layer.a > 0) color = near_layer.rgb / near_layer.a;
    result.background = float4(color, background.a > 0 ? background_depth / background.a : z);
    float coverage = saturate(near_layer.a);
    if (near_layer.a > 0) result.foreground = float4(near_layer.rgb * (coverage / near_layer.a), coverage);
    result.depth = near_depth;
    return result;
}
#endif
)";

}  // namespace gpu
