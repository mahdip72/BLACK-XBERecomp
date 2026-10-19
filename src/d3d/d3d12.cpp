// The title's Direct3D 8 calls drawn with Direct3D 12
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include "app/app.h"
#include "common/aspect.h"
#include "common/options.h"
#include "core/diag.h"
#include "core/log.h"
#include "d3d/depth_of_field.h"
#include "d3d/depth_of_field_shader.h"
#include "d3d/fidelityfx_dof.h"
#include "d3d/fidelityfx_dof_shader.h"
#include "d3d/viewmodel_dof_shader.h"
#include "d3d/weapon_dof_region.h"
#include "d3d/particle_dof_geometry.h"
#include "d3d/particle_dof_clear.h"
#include "d3d/particle_dof_atlas.h"
#include "d3d/particle_dof_shared_coverage.h"
#include "d3d/particle_dof_visibility.h"
#include "d3d/particle_dof_wrapper.h"
#include "d3d/dof_shader_source.h"
#include "d3d/dof_shader_library.h"
#include "d3d/dof_pipeline_cache.h"
#include "d3d/dof_gpu_timing.h"
#include "d3d/dof_loading.h"
#include "d3d/dof_profile.h"
#include "d3d/shader_cache.h"
#include "d3d/occlusion.h"
#include "d3d/smaa_shader.h"
#include "d3d/smaa_textures.h"
#include "d3d/texture_pack.h"
#include "gpu/backend.h"
#include "gpu/ext.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"

using Microsoft::WRL::ComPtr;

namespace gpu {

namespace {

constexpr int kFrames = 3;
constexpr uint64_t kUploadSize = 64ull << 20;
constexpr uint32_t kSrvCount = 3 * 32768;
constexpr uint32_t kSamplerCount = 2048;
constexpr uint32_t kRtvCount = 1024;
constexpr uint32_t kDsvCount = 256;
constexpr uint32_t kParticleDofSnapshots = 3;
constexpr DXGI_FORMAT kBgra = DXGI_FORMAT_B8G8R8A8_UNORM;
// HDR10 output: back buffers in PQ over BT.2020; SDR white and peak in nits when Windows can't tell
constexpr DXGI_FORMAT kHdr10Format = DXGI_FORMAT_R10G10B10A2_UNORM;
constexpr DXGI_FORMAT kOutFrameFormat = DXGI_FORMAT_R16G16B16A16_UNORM;  // the frame before PQ
constexpr int kOutFrame = 0;
constexpr int kOutPanel = 1;
constexpr int kOutTargets = 2;
constexpr float kDefaultPaperNits = 200.0f;
constexpr float kDefaultPeakNits = 1000.0f;
constexpr float kMaxPeakNits = 4000.0f;  // brighter is a display without calibration reporting PQ's 10000
constexpr float kSdrWhiteUnit = 0.08f;  // Windows' SDR white level: 1000 is 80 nits

// perf diagnostic: where a frame's time goes, logged once a second with diag::Perf
using PerfClock = std::chrono::steady_clock;
double PerfMs(PerfClock::time_point t) {
    return std::chrono::duration<double, std::milli>(PerfClock::now() - t).count();
}
struct Perf {
    // milliseconds
    double draw = 0;
    double assemble = 0;
    double bind = 0;
    double sig = 0;
    double texup = 0;
    double compile = 0;
    double clear = 0;
    double copy = 0;
    double present = 0;
    double wait = 0;
    double swap = 0;
    double sync = 0;
    // counts and bytes
    uint64_t draws = 0;
    uint64_t verts = 0;
    uint64_t upload = 0;
    uint64_t overflow = 0;
    uint64_t sigs = 0;
    uint64_t sig_bytes = 0;
    uint64_t texups = 0;
    uint64_t texup_bytes = 0;
    uint64_t newtex = 0;
    uint64_t targets = 0;
    uint64_t rtups = 0;
    uint64_t shrinks = 0;
    uint64_t syncs = 0;
    uint64_t psos = 0;
    uint64_t shaders = 0;
    uint64_t raw = 0;
    uint64_t lates = 0;
};
Perf g_perf;
struct PerfScope {
    double& acc;
    PerfClock::time_point t = PerfClock::now();
    ~PerfScope() { acc += PerfMs(t); }
};

// F3 captures only with diag::CaptureDir set
bool CaptureOn() {
    if (diag::CaptureDir().empty()) LOG_FIRST(1, "capture: set XBE_CAPTURE_DIR");
    return !diag::CaptureDir().empty();
}

const char* kBlitHlsl = R"(
cbuffer B : register(b1) { float4 uvrect; float4 opts; };
Texture2D img : register(t0);
Texture2D lut : register(t1);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uvrect.xy + t * uvrect.zw;
    return o;
}
float4 psmain(O i) : SV_Target {
    float4 c = 0;
    int n = max((int)opts.z, 1);
    float2 px = fwidth(i.uv);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) c += img.SampleLevel(ls, i.uv + px * ((float2(x, y) + 0.5) / n - 0.5), 0);
    c /= n * n;
    c.rgb *= opts.w;
    if (opts.x > 0) c.rgb = float3(lut.Sample(ps, float2(c.r, 0.5)).r, lut.Sample(ps, float2(c.g, 0.5)).g, lut.Sample(ps, float2(c.b, 0.5)).b);
    return float4(c.rgb, opts.y > 0 ? c.a : 1);
}
)";

// HDR10 output: the frame with the panel over it, its highlights stretched towards the display's peak
const char* kHdrHlsl = R"(
cbuffer P : register(b1) { float4 p0; float4 p1; };
Texture2D frame : register(t0);
Texture2D ui : register(t1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = t;
    return o;
}
static const float3 kLum = float3(0.2126, 0.7152, 0.0722);
static const float3x3 k709To2020 = {0.6274, 0.3293, 0.0433, 0.0691, 0.9195, 0.0114, 0.0164, 0.0880, 0.8956};
// gamma 2.2 as SDR monitors show it; sRGB's linear toe lifts the darks
float3 to_linear(float3 c) {
    return pow(c, 2.2);
}
// inverse Reinhard on luminance so hues keep: the brighter, the more it gains, white reaches most
float3 expand(float3 c, float most) {
    return c / (1 - dot(c, kLum) * (1 - 1 / most));
}
float3 pq(float3 nits) {
    float3 y = pow(saturate(nits / 10000), 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * y) / (1 + 18.6875 * y), 78.84375);
}
// p0: SDR white nits, peak nits
float4 hdr10(O i) : SV_Target {
    int3 px = int3(i.p.xy, 0);
    float3 f = min(expand(to_linear(frame.Load(px).rgb), max(p0.y / p0.x, 1)) * p0.x, p0.y);
    float4 u = ui.Load(px);
    float3 panel = to_linear(u.a > 0 ? u.rgb / u.a : 0) * p0.x;
    return float4(pq(mul(k709To2020, lerp(f, panel, u.a))), 1);
}
)";

const char* kSmaaConfig = R"(
cbuffer P : register(b1) { float4 metrics; };
#define SMAA_RT_METRICS metrics
#define SMAA_CUSTOM_SL
#define SMAA_THRESHOLD 0.05
#define SMAA_MAX_SEARCH_STEPS 16
#define SMAA_MAX_SEARCH_STEPS_DIAG 8
#define SMAA_CORNER_ROUNDING 25
SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
)";

// after kSmaaHlsl: edges from the frame's luma, blend weights from edges, area and search, then the frame blended
const char* kSmaaPasses = R"(
Texture2D tex0 : register(t0);
Texture2D tex1 : register(t1);
Texture2D tex2 : register(t2);
struct SmaaEdgeV { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 o[3] : TEXCOORD1; };
struct SmaaWeightV { float4 p : SV_Position; float2 uv : TEXCOORD0; float2 pix : TEXCOORD1; float4 o[3] : TEXCOORD2; };
struct SmaaBlendV { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 o : TEXCOORD1; };
float4 corner(uint id, out float2 uv) {
    uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
SmaaEdgeV vs_edges(uint id : SV_VertexID) {
    SmaaEdgeV o;
    o.p = corner(id, o.uv);
    SMAAEdgeDetectionVS(o.uv, o.o);
    return o;
}
SmaaWeightV vs_weights(uint id : SV_VertexID) {
    SmaaWeightV o;
    o.p = corner(id, o.uv);
    SMAABlendingWeightCalculationVS(o.uv, o.pix, o.o);
    return o;
}
SmaaBlendV vs_blend(uint id : SV_VertexID) {
    SmaaBlendV o;
    o.p = corner(id, o.uv);
    SMAANeighborhoodBlendingVS(o.uv, o.o);
    return o;
}
float4 smaa_edges(SmaaEdgeV i) : SV_Target { return float4(SMAALumaEdgeDetectionPS(i.uv, i.o, tex0), 0, 0); }
float4 smaa_weights(SmaaWeightV i) : SV_Target { return SMAABlendingWeightCalculationPS(i.uv, i.pix, i.o, tex0, tex1, tex2, 0); }
float4 smaa_blend(SmaaBlendV i) : SV_Target { return SMAANeighborhoodBlendingPS(i.uv, i.o, tex0, tex1); }
)";
constexpr int kSmaaTargets = 3;

// the output showing monitor mon, false when no adapter has it
bool FindOutput(IDXGIFactory4* factory, HMONITOR mon, DXGI_OUTPUT_DESC1& desc) {
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; o++) {
            ComPtr<IDXGIOutput6> output6;
            if (FAILED(output.As(&output6)) || FAILED(output6->GetDesc1(&desc))) continue;
            if (desc.Monitor == mon) return true;
        }
    }
    return false;
}

// Windows' SDR content brightness on monitor mon, in nits
float SdrWhiteNits(HMONITOR mon) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    if (!GetMonitorInfoW(mon, &info)) return kDefaultPaperNits;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) {
        return kDefaultPaperNits;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    LONG queried = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(),
                                      nullptr);
    if (queried != ERROR_SUCCESS) return kDefaultPaperNits;
    for (UINT32 i = 0; i < path_count; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
        if (std::wcscmp(source.viewGdiDeviceName, info.szDevice) != 0) continue;
        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = paths[i].targetInfo.adapterId;
        white.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) != ERROR_SUCCESS) break;
        return float(white.SDRWhiteLevel) * kSdrWhiteUnit;
    }
    return kDefaultPaperNits;
}

const char* kDepthHlsl = R"(
#ifdef MS
Texture2DMS<float> img : register(t0);
#else
Texture2D<float> img : register(t0);
#endif
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float psmain(O i) : SV_Depth {
#ifdef MS
    uint w, h, n;
    img.GetDimensions(w, h, n);
    return img.Load(int2(i.uv * float2(w, h)), 0);
#else
    return img.SampleLevel(ps, i.uv, 0);
#endif
}
)";

// from the scene's depth: occlusion pass, then a blur across and one down that multiplies it into the colour.
// The sky is far: a pixel's samples there span less than one pixel and it stays lit
const char* kAoHlsl = R"(
cbuffer A : register(b1) { float4 proj; float4 zp; float4 vp; float4 opt; float4 fog; float4 fogcolor; };
// stencil bit 7: a blended or alpha tested surface wrote the depth last, it counts as the far plane like the sky
#ifdef MS
Texture2DMS<float> depth : register(t0);
Texture2DMS<uint2> stencil : register(t2);
float Raw(int2 p) { return stencil.Load(p, 0).g & 0x80 ? 1 : depth.Load(p, 0); }
#else
Texture2D<float> depth : register(t0);
Texture2D<uint2> stencil : register(t2);
float Raw(int2 p) { return stencil.Load(int3(p, 0)).g & 0x80 ? 1 : depth.Load(int3(p, 0)); }
#endif
Texture2D<float2> ao : register(t1);
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}
// view space position of a pixel. proj: 1 / scale xy, centre xy; zp: z = x / (depth - y); vp: viewport in pixels
float3 View(float2 px) {
    int2 p = clamp(int2(px), int2(vp.xy), int2(vp.xy + vp.zw) - 1);
    float z = zp.x / (Raw(p) - zp.y);
    float2 ndc = (p + 0.5 - vp.xy) / vp.zw * float2(2, -2) + float2(-1, 1);
    return float3((ndc - proj.zw) * proj.xy * z, z);
}
// normal from the nearer neighbour each way: no bend over depth edges
float3 Normal(float2 pos, float3 c) {
    float3 l = View(pos - float2(1, 0));
    float3 r = View(pos + float2(1, 0));
    float3 u = View(pos - float2(0, 1));
    float3 d = View(pos + float2(0, 1));
    float3 dx = abs(r.z - c.z) < abs(c.z - l.z) ? r - c : c - l;
    float3 dy = abs(d.z - c.z) < abs(c.z - u.z) ? d - c : c - u;
    return normalize(cross(dx, dy));
}
static const float kDither[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
// opt: world radius, most pixels it may span; out: occlusion, view z
float2 occlusion(float4 pos : SV_Position) : SV_Target {
    float3 c = View(pos.xy);
    float2 rad = opt.x * 0.5 * vp.zw / (proj.xy * c.z);
    float span = max(rad.x, rad.y);
    if (span < 1) return float2(1, c.z);
    float3 n = Normal(pos.xy, c);
    float k = min(1, opt.y / span);
    float R = opt.x * k;
    int2 cell = int2(pos.xy) & 3;
    float turn = kDither[cell.y * 4 + cell.x] * (6.2831853 / 16);
    float a = 0;
    for (int i = 0; i < 16; i++) {
        float f = (i + 0.5) / 16;
        float t = i * 2.3999632 + turn;
        float3 v = (View(pos.xy + float2(cos(t), sin(t)) * rad * (k * f)) - c) / R;
        float vv = dot(v, v);
        float w = max(1 - vv, 0);
        a += w * w * w * max((dot(v, n) - 0.05) / (vv + 0.01), 0);
    }
    return float2(max(1 - a * 0.25, 0), c.z);
}
static const float kHalfPi = 1.5707963;
// GTAO (Jimenez 2016) at XeGTAO's high preset: 3 slices, 3 steps a side, squared step spread, falloff over the outer
// 61.5% of the radius, power 2.2. Per slice the highest horizon each side, the visible arc between integrated against
// the normal projected into the slice
float2 gtao(float4 pos : SV_Position) : SV_Target {
    float3 c = View(pos.xy);
    float2 rad = opt.x * 0.5 * vp.zw / (proj.xy * c.z);
    float span = max(rad.x, rad.y);
    if (span < 1) return float2(1, c.z);
    float3 n = Normal(pos.xy, c);
    float3 v = normalize(-c);
    float k = min(1, opt.y / span);
    float R = opt.x * k;
    int2 cell = int2(pos.xy) & 3;
    float slice_noise = kDither[cell.y * 4 + cell.x] / 16;
    float step_noise = kDither[cell.x * 4 + cell.y] / 16;
    float vis = 0;
    for (int s = 0; s < 3; s++) {
        float phi = (s + slice_noise) * (kHalfPi * 2 / 3);
        float3 dir = float3(cos(phi), sin(phi), 0);
        // in pixels; screen y runs down, view y up
        float2 omega = float2(dir.x, -dir.y) * rad * k;
        float3 ortho = dir - dot(dir, v) * v;
        float3 axis = normalize(cross(ortho, v));
        float3 pn = n - axis * dot(n, axis);
        float pn_len = max(length(pn), 1e-4);
        float cos_n = saturate(dot(pn, v) / pn_len);
        float an = sign(dot(ortho, pn)) * acos(cos_n);
        // x: the +omega side, y: the -omega side; lowest is flat along the slice's tangent
        float2 low = float2(cos(an + kHalfPi), cos(an - kHalfPi));
        float2 hc = low;
        for (int j = 0; j < 3; j++) {
            float f = (j + step_noise) / 3;
            // at least 1.3 pixels out, off the centre pixel
            float2 off = omega * (f * f + 1.3 / length(omega));
            float3 d0 = View(pos.xy + off) - c;
            float3 d1 = View(pos.xy - off) - c;
            float2 len = max(float2(length(d0), length(d1)), 1e-4);
            float2 w = saturate((R - len) / (R * 0.615));
            float2 sc = float2(dot(d0, v), dot(d1, v)) / len;
            hc = max(hc, lerp(low, sc, w));
        }
        float h0 = an + clamp(-acos(hc.y) - an, -kHalfPi, kHalfPi);
        float h1 = an + clamp(acos(hc.x) - an, -kHalfPi, kHalfPi);
        float sin_n = sin(an);
        float arc0 = (cos_n + 2 * h0 * sin_n - cos(2 * h0 - an)) / 4;
        float arc1 = (cos_n + 2 * h1 * sin_n - cos(2 * h1 - an)) / 4;
        vis += pn_len * (arc0 + arc1);
    }
    return float2(pow(saturate(vis / 3), 2.2), c.z);
}
// opt.zw: step; 5 taps average the 4 pixel dither, depth edges left out
float Smooth(float2 pos, out float z) {
    int2 p = int2(pos);
    int2 lo = int2(vp.xy);
    int2 hi = int2(vp.xy + vp.zw) - 1;
    z = ao.Load(int3(p, 0)).y;
    float sum = 0;
    float n = 0;
    for (int i = -2; i <= 2; i++) {
        float2 s = ao.Load(int3(clamp(p + i * int2(opt.zw), lo, hi), 0));
        float w = (abs(i) == 2 ? 0.5 : 1) * saturate(2 - abs(s.y - z) / (z * 0.02));
        sum += s.x * w;
        n += w;
    }
    return sum / n;
}
float2 occlusion_blur(float4 pos : SV_Position) : SV_Target {
    float z;
    float a = Smooth(pos.xy, z);
    return float2(a, z);
}
// the fog is already drawn: only the scene share under it is occluded, blended as colour * a + rgb
float4 occlusion_apply(float4 pos : SV_Position) : SV_Target {
    float z;
    float a = Smooth(pos.xy, z);
    float fogged = min(saturate(fog.x * z + fog.y), fog.z);
    return float4(fogcolor.rgb * fogged * (1 - a), a);
}
)";
constexpr int kAoTargets = 2;
constexpr int kAoConstants = 24;  // kAoHlsl's b1
constexpr DXGI_FORMAT kAoFormat = DXGI_FORMAT_R16G16_FLOAT;
constexpr uint32_t kDofLevels = 4;
constexpr uint32_t kDofTargets = 2;
constexpr uint32_t kDofLayerViews = 3;
constexpr DXGI_FORMAT kDofFormats[kDofTargets] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16_FLOAT};
constexpr float kAoRadius = 0.8f;    // world units, the engine's are metres
constexpr float kAoMaxSpan = 0.15f;  // of the view's height, the most one pixel's samples may cover

// the viewport in target pixels
D3D12_RECT ViewRect(const Viewport& v, const Target& t) {
    float sc = float(t.w) / t.gw;
    D3D12_RECT r;
    r.left = std::clamp<LONG>(LONG(v.x * sc), 0, t.w);
    r.top = std::clamp<LONG>(LONG(v.y * sc), 0, t.h);
    r.right = std::clamp<LONG>(LONG((v.x + v.w) * sc), 0, t.w);
    r.bottom = std::clamp<LONG>(LONG((v.y + v.h) * sc), 0, t.h);
    return r;
}

// b1 of kAoHlsl: proj, zp with the viewport's depth range, vp, opt (its step written per pass), fog, fog colour
void OcclusionConstants(const Viewport& v, const D3D12_RECT& r, const float p[4 * kOcclusionRects],
                        float c[kAoConstants]) {
    float zr = v.max_z - v.min_z;
    float w = float(r.right - r.left);
    float h = float(r.bottom - r.top);
    const float out[kAoConstants] = {p[0], p[1], p[2], p[3], p[4] * zr, v.min_z + p[5] * zr, 0, 0,
                                     float(r.left), float(r.top), w, h, kAoRadius, kAoMaxSpan * h, 0, 0,
                                     p[8], p[9], p[10], 0, p[12], p[13], p[14], 0};
    std::memcpy(c, out, sizeof(out));
}

// dst = dst * src.a + src; the alpha the game's blends read stays
void OcclusionBlend(D3D12_RENDER_TARGET_BLEND_DESC& b) {
    b.BlendEnable = TRUE;
    b.SrcBlend = D3D12_BLEND_ONE;
    b.SrcBlendAlpha = D3D12_BLEND_ZERO;
    b.DestBlend = D3D12_BLEND_SRC_ALPHA;
    b.DestBlendAlpha = D3D12_BLEND_ONE;
    b.BlendOp = D3D12_BLEND_OP_ADD;
    b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
}

float Bits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void Color(uint32_t argb, float* o) {
    o[0] = ((argb >> 16) & 255) / 255.0f;
    o[1] = ((argb >> 8) & 255) / 255.0f;
    o[2] = (argb & 255) / 255.0f;
    o[3] = (argb >> 24) / 255.0f;
}

D3D12_BLEND Blend(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return D3D12_BLEND_ZERO;
        case 1: return D3D12_BLEND_ONE;
        case 0x300: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
        case 0x301: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
        case 0x302: return D3D12_BLEND_SRC_ALPHA;
        case 0x303: return D3D12_BLEND_INV_SRC_ALPHA;
        case 0x304: return D3D12_BLEND_DEST_ALPHA;
        case 0x305: return D3D12_BLEND_INV_DEST_ALPHA;
        case 0x306: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
        case 0x307: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
        case 0x308: return D3D12_BLEND_SRC_ALPHA_SAT;
        case 0x8001: case 0x8003: return D3D12_BLEND_BLEND_FACTOR;
        case 0x8002: case 0x8004: return D3D12_BLEND_INV_BLEND_FACTOR;
        default: return D3D12_BLEND_ONE;
    }
}

D3D12_BLEND_OP BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return D3D12_BLEND_OP_SUBTRACT;
        case 0x800B: return D3D12_BLEND_OP_REV_SUBTRACT;
        case 0x8007: return D3D12_BLEND_OP_MIN;
        case 0x8008: return D3D12_BLEND_OP_MAX;
        default: return D3D12_BLEND_OP_ADD;
    }
}

D3D12_COMPARISON_FUNC Cmp(uint32_t v) {
    uint32_t n = v >= 0x200 && v <= 0x207 ? v - 0x200 : 7;
    return D3D12_COMPARISON_FUNC(n + 1);
}

uint32_t SimpleState(const State& s, uint32_t method);

// The same effective function feeds misc.y and the particle source-test specialization.
int EffectiveAlphaFunction(const State& s) {
    return SimpleState(s, kMAlphaTestEnable) ? int(Cmp(SimpleState(s, kMAlphaFunc))) - 1 :
        int(D3D12_COMPARISON_FUNC_ALWAYS) - 1;
}

D3D12_STENCIL_OP StencilOp(uint32_t v) {
    switch (v) {
        case 0: return D3D12_STENCIL_OP_ZERO;
        case 0x1E01: return D3D12_STENCIL_OP_REPLACE;
        case 0x1E02: return D3D12_STENCIL_OP_INCR_SAT;
        case 0x1E03: return D3D12_STENCIL_OP_DECR_SAT;
        case 0x150A: return D3D12_STENCIL_OP_INVERT;
        case 0x8507: return D3D12_STENCIL_OP_INCR;
        case 0x8508: return D3D12_STENCIL_OP_DECR;
        default: return D3D12_STENCIL_OP_KEEP;
    }
}

// The game's 2D layer (menus, text, HUD) leaves oPos.w at 0: screen space. Clamped to 2^-64 its clip space
// coordinates are near 1e-20 and AMD drops parts of those draws; w = 1 draws the same, as XYZRHW does
std::string VertexProgram(const VertexShader& vs, uint16_t regs, uint64_t decode) {
    return ApplyScreenSpaceVertexFix(VertexProgramHlsl(vs.program, regs, decode));
}

// A small draw whose every float2 input lies in 0..1: sprites, HUD pieces, text. Upscaled, the far edge's last half
// texel is drawn too and wrap blends in the opposite edge (thin lines on HUD sprites); clamp takes the edge texel.
bool UnitTexcoords(const Assembled& a) {
    constexpr float kLo = -1e-4f;
    constexpr float kHi = 1.0001f;
    constexpr uint32_t kSmallDrawVertices = 4096;
    if (!a.vertices.empty() || !a.vertex_count || a.vertex_count > kSmallDrawVertices) return false;
    bool any = false;
    for (int r = 0; r < 16; r++) {
        const VertexElement& e = a.elements[size_t(r)];
        if (!(a.regs >> r & 1) || e.format != DXGI_FORMAT_R32G32_FLOAT) continue;
        const VertexBuffer& vb = a.buffers[e.buffer];
        if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return false;
        for (uint32_t v = 0; v < a.vertex_count; v++) {
            float uv[2];
            std::memcpy(uv, vb.src + size_t(v) * vb.stride + e.offset, sizeof(uv));
            if (!(uv[0] >= kLo && uv[0] <= kHi && uv[1] >= kLo && uv[1] <= kHi)) return false;
        }
        any = true;
    }
    return any;
}

constexpr uint32_t kTaddressWrap = 1;
constexpr uint32_t kTaddressClamp = 3;

// UnitTexcoords only changes stages that wrap
bool AnyStageWraps(const State& s) {
    for (const auto& t : s.tss) {
        if (t[kTssAddressU] == kTaddressWrap || t[kTssAddressV] == kTaddressWrap) return true;
    }
    return false;
}

D3D12_TEXTURE_ADDRESS_MODE Address(uint32_t v) {
    switch (v) {
        case 2: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        case 3: case 5: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        case 4: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        default: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    }
}

DXGI_FORMAT HostFormat(Host h) {
    switch (h) {
        case Host::BC1: return DXGI_FORMAT_BC1_UNORM;
        case Host::BC2: return DXGI_FORMAT_BC2_UNORM;
        case Host::BC3: return DXGI_FORMAT_BC3_UNORM;
        case Host::R32F: return DXGI_FORMAT_R32_FLOAT;
        default: return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
}

constexpr uint32_t kFmtD16 = 0x2C;     // D3DFMT_D16
constexpr uint32_t kFmtLinD16 = 0x30;  // D3DFMT_LIN_D16

// TexInfo::shadow of a depth texture
uint8_t ShadowDepthKind(uint32_t format) { return format == kFmtD16 || format == kFmtLinD16 ? 2 : 1; }

// bytes per row of a 32 bit surface in guest memory, rounded to 64
uint32_t GuestPitch(uint32_t width) { return (width * 4 + 63) & ~63u; }

// rows of a texture upload start D3D12_TEXTURE_DATA_PITCH_ALIGNMENT apart
uint32_t UploadPitch(uint32_t row_bytes) { return (row_bytes + 255) & ~255u; }

D3D12_RESOURCE_DESC BufferDesc(uint64_t bytes) {
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return bd;
}

// one level, one sample: callers set Flags where they differ
D3D12_RESOURCE_DESC Texture2DDesc(DXGI_FORMAT fmt, uint64_t w, uint32_t h) {
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    return td;
}

// MemSig hashes a surface up to kSigWholeMax bytes whole, a bigger one as evenly spaced samples
constexpr uint32_t kSigWholeMax = 65536;
constexpr uint32_t kSigSamples = 64;
constexpr uint32_t kSigSampleBytes = 64;

uint64_t MemSig(uint32_t data, uint32_t total) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    uint64_t sig = Hash(&total, sizeof(total));
    g_perf.sigs++;
    g_perf.sig_bytes += total <= kSigWholeMax ? total : kSigSamples * kSigSampleBytes;
    if (total <= kSigWholeMax) return Hash(p, total, sig);
    for (uint32_t i = 0; i < kSigSamples; i++) {
        const uint8_t* sample = p + uint64_t(total - kSigSampleBytes) * i / (kSigSamples - 1);
        sig = Hash(sample, kSigSampleBytes, sig);
    }
    return sig;
}

// MemSig samples a big surface and misses small writes (XFONT text): after a Sync every byte is compared
bool Changed(const Target& t) {
    if (t.drawn || t.read.empty()) return false;
    uint32_t row = t.gw * 4;
    uint32_t pitch = GuestPitch(t.gw);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        if (std::memcmp(p + size_t(y) * pitch, &t.read[size_t(y) * row], row)) return true;
    }
    return false;
}

// a lock alone proves nothing, reads lock too: only changed bytes mean the CPU wrote it
bool LatchCpuWrite(Target& t) {
    uint64_t e = DirtyEpoch(t.data);
    if (e <= t.epoch) return false;
    t.epoch = e;
    uint64_t sig = MemSig(t.data, t.bytes);
    if (sig == t.sig && !Changed(t)) return false;
    t.sig = sig;
    return true;
}

uint32_t Ms(const Target& t) { return t.res->GetDesc().SampleDesc.Count; }

struct Tex {
    ComPtr<ID3D12Resource> res;
    TextureDesc d;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    uint32_t levels = 1;
    uint64_t sig = 0;
    uint64_t frame = 0;
    uint64_t palette = 0;  // hash of the colours a P8 texture was decoded with
    uint8_t flags = 0;  // Extension::Decoded's
    bool replacement = false;
};

struct PsoKey {
    uint64_t vs;
    uint64_t ps;
    uint32_t regs;
    uint32_t topo;
    uint32_t has_rt;
    uint32_t has_ds;
    uint32_t elements[16];  // per register: DXGI format, buffer << 8, offset << 16
    uint32_t blend;
    uint32_t src;
    uint32_t dst;
    uint32_t op;
    uint32_t wmask;
    uint32_t zen;
    uint32_t zwrite;
    uint32_t zfunc;
    uint32_t sten;
    uint32_t sfunc;
    uint32_t smask;
    uint32_t swmask;
    uint32_t sfail;
    uint32_t szfail;
    uint32_t spass;
    uint32_t cull;
    uint32_t ccw;
    uint32_t fill;
    uint32_t unclipped;  // no clipping at the near and far planes: NearDepthHlsl draws
    int32_t zbias;
    float zslope;
    uint32_t rtfmt;
    uint32_t msaa;     // samples of a multisampled target, 0 without
    uint32_t ao_mark;  // the stencil holds ambient occlusion's mark, not the game's state
};

// Blit's settings past the source and its rectangle
struct BlitOptions {
    bool gamma = false;       // through the gamma lookup
    bool alpha = false;       // the source's alpha, else 1
    uint32_t taps = 1;        // n x n box filter per pixel
    float mul = 1;            // colour scale
    DXGI_FORMAT rtv = kBgra;  // the target's format
    bool over = false;        // premultiplied over the target, which keeps its alpha
    uint32_t samples = 1;     // the target's
};

// Shrink's settings past the source and the size
struct ShrinkOptions {
    uint32_t taps = 1;
    // into: a copy from an earlier call to draw over again, in state into_state
    ID3D12Resource* into = nullptr;
    D3D12_RESOURCE_STATES into_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    float mul = 1;
};

// a draw's four texture stages: BindStageTexturesAndTargets fills them, the constants, views and samplers read them
struct DrawTextures {
    TexInfo info[4];
    D3D12_SHADER_RESOURCE_VIEW_DESC sv[4];
    ID3D12Resource* res[4];
    bool rt[4] = {};            // stage samples a render target
    float k[4] = {1, 1, 1, 1};  // Extension::Sample's k
    float wide[4] = {};         // host texels per console texel of a target read into a smaller one, 0 otherwise
    bool blur[4] = {};          // wide for a blur between small targets
    bool final_glow = false;   // the final glow composite, separate from reflection/reload blur
};

// a draw's shaders: what they are made from, their cache keys, the hashes Compile returned and the compiled blobs
struct DrawShaders {
    uint32_t particle_samples = 0;  // only the verified, late native smoke variant
    uint32_t particle_blend = 0;    // 0 alpha-over, 1 native SRC_ALPHA/ONE additive or reverse-subtractive
    int particle_alpha_function = -1;
    uint32_t particle_pass = 0;  // 0 direct, 1 source, 2 composite, 3 field, 4 shared composite, 5 support
    bool particle_coverage = false;  // verified depth; final shader replaces hardware depth coverage
    bool prog = false;  // a vertex program, else the fixed function pipeline
    bool fixed_transform = false;  // fixed function on untransformed vertices: the shader reads matrices and lights
    FixedKey fk;
    uint64_t vkey = 0;
    uint64_t vh = 0;
    uint64_t pkey = 0;
    uint64_t ph = 0;
    bool near_depth = false;  // the pixel shader writes the depth: NearDepthHlsl
    ID3DBlob* vs_code = nullptr;
    ID3DBlob* ps_code = nullptr;
};

// the render target's console size in pixels, or the depth buffer's without one
struct GuestExtent {
    uint32_t w;
    uint32_t h;
};

class D3D12Backend final : public Backend, public Context {
public:
    ~D3D12Backend() override;
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override;
    void Present(const Surface& front) override;
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override;
    bool DeliverLate(uint32_t data) override;
    void RecordLate(uint32_t data) override;
    void Flush() override;
    void SetGamma(const uint8_t* ramp) override;
    void WaitGpuFrame(uint64_t n);

    ID3D12Device* Device() override { return dev_.Get(); }
    // An extension borrowing the raw list can record depth writes outside the tracked helpers.
    ID3D12GraphicsCommandList* List() override { InvalidateParticleDepth(); return cl_.Get(); }
    void Release(ComPtr<ID3D12Resource> r) override { CurrentFrame().garbage.push_back(std::move(r)); }
    D3D12_CPU_DESCRIPTOR_HANDLE View(const Target& t) override { return Cpu(rtv_heap_.Get(), t.view, rtv_inc_); }
    void Unbind() override { bound_rt_ = bound_ds_ = nullptr; }
    void Capture(Target& t, const char* tag, float mul) override;
    void GuestSize(uint32_t& w, uint32_t& h) const override {
        w = guest_w_;
        h = guest_h_;
    }
    uint64_t Presents() const override { return presents_; }

private:
    struct Frame {
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12Resource> upload;
        uint8_t* map = nullptr;
        uint64_t used = 0;
        uint64_t fence = 0;
        std::vector<ComPtr<ID3D12Resource>> garbage;
        uint32_t srv = 0;
    };
    struct Alloc {
        ID3D12Resource* res;
        uint64_t off;
        uint8_t* cpu;
        D3D12_GPU_VIRTUAL_ADDRESS gpu;
    };

    Alloc Upload(uint64_t size, uint64_t align = 256);
    Frame& CurrentFrame() { return frames_[frame_ % kFrames]; }
    uint32_t AllocSrvSlots(uint32_t n);
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(ID3D12DescriptorHeap* h, uint32_t i, uint32_t inc) {
        auto c = h->GetCPUDescriptorHandleForHeapStart();
        c.ptr += size_t(i) * inc;
        return c;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(ID3D12DescriptorHeap* h, uint32_t i, uint32_t inc) {
        auto g = h->GetGPUDescriptorHandleForHeapStart();
        g.ptr += uint64_t(i) * inc;
        return g;
    }
    void Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES& cur, D3D12_RESOURCE_STATES want) override;
    void Begin();
    void Submit(bool wait, bool final_native_present = false);
    void BeginDofTiming();
    void ExportDofTiming(const DofGpuTiming::Report& report);
    void WaitFence(uint64_t value);
    void SetFullViewport(uint32_t w, uint32_t h);
    void SetBlitTarget(D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w, uint32_t h);
    // a surface's place in targets_: its key, console size and entry, end when it has none
    struct TargetSlot {
        uint32_t data;
        uint64_t key;
        uint32_t gw;
        uint32_t gh;
        std::unordered_map<uint64_t, Target>::iterator it;
        bool sized;  // the entry is there at the surface's size
    };
    Target* FindTarget(const Surface& s, bool depth);
    Target* EnsureTarget(const Surface& s, bool depth);
    // EnsureTarget's steps, in call order
    TargetSlot FindTargetSlot(const Surface& s, bool depth, uint64_t alt);
    void ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it);
    bool CreateTargetResource(Target& t, const Surface& s);
    bool CreateTargetView(Target& t);
    // UploadTarget, then its step
    void UploadTarget(Target& t, const Surface& s);
    ComPtr<ID3D12Resource> CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h);
    void Blit(ID3D12Resource* src, DXGI_FORMAT fmt, const float uvrect[4], bool gamma, bool alpha, uint32_t taps = 1,
              float mul = 1, DXGI_FORMAT rtv = kBgra) override {
        Blit(src, fmt, uvrect, BlitOptions{.gamma = gamma, .alpha = alpha, .taps = taps, .mul = mul, .rtv = rtv});
    }
    void Blit(ID3D12Resource* src, DXGI_FORMAT fmt, const float uvrect[4], const BlitOptions& o);
    uint32_t EnsureBlitSamplers();
    void DrawFullScreen(ID3D12PipelineState* pso, D3D12_GPU_VIRTUAL_ADDRESS cb, uint32_t srv_slot,
                        std::optional<uint32_t> sampler_slot = {});
    ID3D12Resource* Feedback(Target& t) override;
    bool EnsureFeedbackResource(Target& t);
    ID3D12Resource* Flat(Target& t, D3D12_RESOURCE_STATES*& st);
    uint32_t Samples(uint32_t want);
    void Resolve(Target& t) {
        if (t.range != 1 && ext_) ext_->Resolve(*this, t);
    }
    void Pass(const char* hlsl, const char* entry, DXGI_FORMAT rtv, D3D12_CPU_DESCRIPTOR_HANDLE rh, uint32_t w,
              uint32_t h, ID3D12Resource* const src[3], const DXGI_FORMAT fmt[3], const float p[8],
              float blend) override {
        Pass(hlsl, "vs", entry, rtv, rh, w, h, src, fmt, p, blend);
    }
    // Pass, then its step
    bool Pass(const char* hlsl, const char* vs_entry, const char* entry, DXGI_FORMAT rtv,
              D3D12_CPU_DESCRIPTOR_HANDLE rh, uint32_t w, uint32_t h, ID3D12Resource* const src[3],
              const DXGI_FORMAT fmt[3], const float p[8], float blend);
    ID3D12PipelineState* EnsurePassPipeline(const char* hlsl, const char* vs_entry, const char* entry, DXGI_FORMAT rtv,
                                           float blend, bool dof = false);
    bool BindTargets(const State& s);
    bool DepthCopy(const State& s);
    bool DepthPass(ID3D12Resource* src, uint32_t src_samples, D3D12_CPU_DESCRIPTOR_HANDLE dh, uint32_t w, uint32_t h,
                   uint32_t samples);
    ID3D12PipelineState* EnsureDepthCopyPipeline(bool ms, uint32_t samples);
    // one full screen pass of kAoHlsl
    struct AoPass {
        const char* entry;
        DXGI_FORMAT out = kAoFormat;
        uint32_t samples = 1;   // of the target written
        bool blend = false;  // onto that target, OcclusionBlend
        D3D12_CPU_DESCRIPTOR_HANDLE rtv;
        ID3D12Resource* depth = nullptr;  // the scene's depth and stencil, read by the first pass
        bool ms_depth = false;
        ID3D12Resource* src = nullptr;  // the pass before
        float step_x = 0;
        float step_y = 0;
    };
    void Occlusion(const State& s, const float p[4 * kOcclusionRects]);
    bool OcclusionFits(const Target* rt, const Target* ds) const;
    bool EnsureOcclusionTargets(uint32_t w, uint32_t h);
    D3D12_CPU_DESCRIPTOR_HANDLE OcclusionView(int i) { return Cpu(rtv_heap_.Get(), kRtvCount + kFrames + i, rtv_inc_); }
    ID3D12PipelineState* EnsureOcclusionPipeline(const AoPass& a);
    void OcclusionViews(uint32_t slot, const AoPass& a);
    bool OcclusionPass(const AoPass& a, float c[kAoConstants], const D3D12_RECT& rect, uint32_t w, uint32_t h);
    void DepthOfField(const State& s, const float p[4 * kDepthOfFieldRects]);
    void ParticleDofWorld(const Target& rt, const Target& ds, const Viewport& viewport, const float c[12]);
    void InvalidateParticleDepth(ID3D12Resource* resource = nullptr);
    std::array<float, 4> ParticleDepthProjection(ID3D12Resource* resource) const;
    void SetParticleDepthProjection(ID3D12Resource* resource, const std::array<float, 4>& projection);
    bool PrepareParticleDof(const State& s, Assembled& a, const PixelKey& pk, DrawShaders& sh, DrawTextures& stages);
    void ParticleDofSkipped(uint32_t reason, const char* message);
    bool EnsureParticleDofAtlas(uint32_t extent);
    bool EnsureParticleDofVisibility(uint32_t extent);
    D3D12_CPU_DESCRIPTOR_HANDLE ParticleVisibilityView() {
        return ParticleDofRangeView(kParticleDofSnapshots + 5);
    }
    bool PrepareParticleSharedCoverage(uint32_t width, uint32_t height);
    D3D12_CPU_DESCRIPTOR_HANDLE ParticleSharedView(uint32_t index) {
        return ParticleDofRangeView(kParticleDofSnapshots + 3 + index);
    }
    bool DrawParticleDofAtlas(const State& s, const Assembled& a, const PixelKey& pk,
        const DrawShaders& sh, DrawTextures& stages, const PsoKey& native_key, bool clamp);
    D3D12_CPU_DESCRIPTOR_HANDLE ParticleDofAtlasView(uint32_t index) {
        return ParticleDofRangeView(kParticleDofSnapshots + index);
    }
    DXGI_FORMAT ParticleDofRangeFormat(uint32_t samples) const {
        return samples == 2 && particle_float_bounds_ ? DXGI_FORMAT_R32G32_FLOAT : DXGI_FORMAT_R32G32_UINT;
    }
    bool EnsureParticleDofDepthRange(uint32_t snapshot_index, uint32_t w, uint32_t h, uint32_t samples);
    bool ParticleDofDepthRange(ID3D12Resource* depth, ID3D12Resource* range,
        D3D12_RESOURCE_STATES& range_state,
        uint32_t snapshot_index, uint32_t samples, uint32_t w, uint32_t h);
    D3D12_CPU_DESCRIPTOR_HANDLE ParticleDofRangeView(uint32_t index) {
        return Cpu(rtv_heap_.Get(), kRtvCount + kFrames + kAoTargets + kOutTargets + kSmaaTargets +
            kDofLevels * kDofTargets + kDofLayerViews + kDofTargets + 2 + index, rtv_inc_);
    }
    bool FidelityFxDepthOfField(Target& rt, Target& ds, const float c[12], const D3D12_RECT& rect, bool cinematic);
    bool EnsureFidelityFxTargets(uint32_t w, uint32_t h);
    void FidelityFxViews(uint32_t slot, ID3D12Resource* first, DXGI_FORMAT first_format, bool first_ms,
                        ID3D12Resource* second, DXGI_FORMAT second_format);
    D3D12_CPU_DESCRIPTOR_HANDLE FidelityFxView(uint32_t target) {
        return Cpu(rtv_heap_.Get(), kRtvCount + kFrames + kAoTargets + kOutTargets + kSmaaTargets +
            kDofLevels * kDofTargets + kDofLayerViews + kDofTargets + target, rtv_inc_);
    }
    ID3D12PipelineState* EnsureDepthOfFieldPipeline(const char* entry, DXGI_FORMAT out, uint32_t samples,
                                                  uint32_t input_samples = 1, bool tiled = false);
    bool EnsureDepthOfFieldFilter(uint32_t w, uint32_t h);
    bool EnsureDepthOfFieldLayers(uint32_t w, uint32_t h);
    D3D12_CPU_DESCRIPTOR_HANDLE DepthOfFieldView(uint32_t mip, uint32_t target) {
        return Cpu(rtv_heap_.Get(), kRtvCount + kFrames + kAoTargets + kOutTargets + kSmaaTargets +
            target * kDofLevels + mip, rtv_inc_);
    }
    void SetDepthOfFieldTarget(uint32_t mip, uint32_t w, uint32_t h);
    D3D12_CPU_DESCRIPTOR_HANDLE DepthOfFieldLayerView(uint32_t view) {
        return Cpu(rtv_heap_.Get(), kRtvCount + kFrames + kAoTargets + kOutTargets + kSmaaTargets +
            kDofLevels * kDofTargets + view, rtv_inc_);
    }
    void DepthOfFieldComposeViews(uint32_t slot, ID3D12Resource* scene, DXGI_FORMAT format);
    void DepthOfFieldViews(uint32_t slot, ID3D12Resource* depth, bool ms, ID3D12Resource* color,
                           DXGI_FORMAT color_format, uint32_t first_mip = 0, uint32_t mip_count = 0);
    void DepthOfFieldMipState(uint32_t mip, D3D12_RESOURCE_STATES state);
    void DepthOfFieldSkipped(uint32_t reason, const char* message);
    void WeaponDepthOfField(const State& s, const float p[4 * kWeaponDofRects], bool begin);
    bool EnsureWeaponDofTargets(uint32_t w, uint32_t h);
    bool EnsureWeaponDofTiles(uint32_t w, uint32_t h);
    void RecordWeaponDofCoverage(const State& s, const Assembled& a, const DrawShaders& sh,
                                 const PsoKey& k, ID3D12PipelineState* native, uint32_t guest_width);
    D3D12_CPU_DESCRIPTOR_HANDLE WeaponDofTileView() {
        return ParticleDofRangeView(kParticleDofSnapshots + 2);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE WeaponDofView(uint32_t target) {
        return Cpu(rtv_heap_.Get(), kRtvCount + kFrames + kAoTargets + kOutTargets + kSmaaTargets +
            kDofLevels * kDofTargets + kDofLayerViews + target, rtv_inc_);
    }
    void WeaponDofViews(uint32_t slot, ID3D12Resource* a, ID3D12Resource* b, ID3D12Resource* color,
                        DXGI_FORMAT format, bool pack, bool ms, ID3D12Resource* tiles = nullptr);
    void WeaponDofSkipped(uint32_t reason, const char* message);
    Target* draw_rt_ = nullptr;  // the draw's targets, the bound ones go when a texture's copy is drawn
    Target* draw_ds_ = nullptr;
    // Draw, in call order, each callee right after its caller
    bool AssembleDraw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices,
                      Assembled& a);
    void EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh);
    bool BindStageTexturesAndTargets(const State& s, DrawTextures& stages);
    ID3D12Resource* BindTexture(const State& s, int stage, DrawTextures& stages);
    ID3D12Resource* BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box,
                                      DrawTextures& stages);
    ID3D12Resource* BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                     DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key);
    void UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p, const uint32_t* colours,
                       bool same);
    void UploadTextureLevel(Tex& tx, uint32_t f, uint32_t l, const std::vector<uint8_t>& px, uint32_t pitch);
    void EnsurePixelShader(const PixelKey& pk, DrawShaders& sh);
    void BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k);
    void KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k);
    bool MarksOcclusion(const PsoKey& k) const;
    ID3D12PipelineState* EnsurePipeline(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps);
    ID3D12PipelineState* EnsureParticlePipeline(PsoKey k, ID3DBlob* vs, ID3DBlob* ps);
    void PrimeDepthOfField();
    void QueueDofPipelines(uint32_t samples);
    bool PrepareDofResources(uint32_t width, uint32_t height, bool balanced);
    void FinishDofPreparation(bool usable);
    void PublishDofPreparation();
    void FillPipelineDesc(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps, D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) const;
    float TargetScale() const;  // host pixels per console pixel of the bound target
    float RenderTargetShift(const State& s, const DrawShaders& sh) const;
    std::array<D3D12_GPU_VIRTUAL_ADDRESS, 2> UploadConstants(const State& s, const DrawShaders& sh,
        const PsoKey& k, const DrawTextures& stages, GuestExtent size, DrawConstants& c);
    void FillTextureConstants(const State& s, const DrawShaders& sh, const DrawTextures& stages, DrawConstants& c);
    void FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c);
    void CaptureDraw(const State& s, uint32_t prim, uint32_t count, const PsoKey& k, const DrawConstants& c,
                     const DrawTextures& stages);
    void CaptureTextures(const State& s, const DrawTextures& stages);
    void EndDrawCapture(const State& s, const Assembled& a, uint32_t count, const DrawConstants& c);
    void CaptureInputs(const State& s, const Assembled& a, const DrawConstants& c);
    void CaptureShaders(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk);
    void CaptureShader(const char* kind, uint64_t key, const std::string& src);
    void SetTextureViewsAndSamplers(const State& s, DrawTextures& stages, bool clamp);
    uint32_t EnsureStageSamplers(const State& s, const DrawTextures& stages, bool clamp);
    void SetViewportScissor(const State& s, uint32_t gw, bool weapon_tiles = false);
    void SetBlendFactorStencilRef(const State& s, const PsoKey& k);
    void DrawGeometry(const Assembled& a);
    void UploadVertexBuffers(const Assembled& a);
    // shaders by source hash: Compile returns the bytecode and sets hash
    ID3DBlob* Compile(const std::string& src, const char* entry, const char* profile, uint64_t& hash);
    ID3DBlob* CompileDof(const std::string& src, const char* entry, const char* profile, uint64_t& hash);
    // Blit's pipeline
    ID3D12PipelineState* EnsureBlitPipeline(DXGI_FORMAT rtv, bool over, uint32_t samples);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC FullScreenPipelineDesc(ID3DBlob* vs, ID3DBlob* ps, DXGI_FORMAT rtv) const;
    // Init, in order, each callee right after its caller
    bool CreateDevice();
    bool CreateSwapChain(HWND hwnd);
    void CreateHeaps();
    uint32_t CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE t, uint32_t n, bool visible, ComPtr<ID3D12DescriptorHeap>& out);
    void CreateBackBufferViews();
    bool CreateRootSignature();
    void CreateFrames();
    void CreateGammaLut();
    void LoadSettings();
    // Present, in call order, each callee right after its caller
    void FinishCapture(const Surface& front);
    void StartCapture();
    void UpdateHdrDisplay();
    void ResizeSwap();
    bool ResizeBackBuffers(uint32_t w, uint32_t h, DXGI_FORMAT fmt, DXGI_COLOR_SPACE_TYPE space);
    void UploadGammaRamp();
    void DrawFrame(Target* t, uint32_t bi);
    ID3D12Resource* PrepareShownFrame(Target* t);
    ID3D12Resource* Smaa(ID3D12Resource* src, DXGI_FORMAT fmt, uint32_t w, uint32_t h);
    struct SmaaLookup {
        DXGI_FORMAT fmt;
        uint32_t w;
        uint32_t h;
        uint32_t bpp;  // bytes per texel
        const uint8_t* data;
    };
    bool EnsureSmaaLookups();
    bool CreateLookupTexture(ComPtr<ID3D12Resource>& r, const SmaaLookup& l);
    bool EnsureSmaaTargets(DXGI_FORMAT fmt, uint32_t w, uint32_t h);
    bool CreateSmaaTarget(int i, DXGI_FORMAT fmt, uint32_t w, uint32_t h);
    bool DrawSmaaPasses(ID3D12Resource* src, DXGI_FORMAT fmt, uint32_t w, uint32_t h, const char* hlsl);
    D3D12_CPU_DESCRIPTOR_HANDLE SmaaView(int i) {
        return Cpu(rtv_heap_.Get(), kRtvCount + kFrames + kAoTargets + kOutTargets + i, rtv_inc_);
    }
    void DrawSdrFrame(Target* t, ID3D12Resource* r, D3D12_CPU_DESCRIPTOR_HANDLE bb);
    bool EnsureOutTargets();
    bool CreateOutTarget(int i, DXGI_FORMAT fmt);
    void DrawHdrFrame(Target* t, ID3D12Resource* r, D3D12_CPU_DESCRIPTOR_HANDLE bb);
    void BlitFrame(Target& t, ID3D12Resource* r, DXGI_FORMAT rtv);
    void RenderUi();
    D3D12_CPU_DESCRIPTOR_HANDLE OutView(int i) {
        return Cpu(rtv_heap_.Get(), kRtvCount + kFrames + kAoTargets + i, rtv_inc_);
    }
    void ReleaseOldTextures();
    void ResetFrame();
    void CountFps();
    void ApplySettings();
    float Scale() const override { return scale_; }
    ComPtr<ID3D12Resource> Shrink(ID3D12Resource* src, D3D12_RESOURCE_STATES& st, DXGI_FORMAT fmt, uint32_t w,
                                  uint32_t h, const ShrinkOptions& o = {});
    bool ReadBack(ID3D12Resource* res, uint32_t w, uint32_t h, std::vector<uint8_t>& out);
    // a resource CapSave shrinks to w x h and writes, in state *state, colour times mul
    struct CapImage {
        ID3D12Resource* res;
        D3D12_RESOURCE_STATES* state;
        DXGI_FORMAT fmt;
        uint32_t w;
        uint32_t h;
        float mul = 1;
    };
    void CapSave(const CapImage& img, const char* name);
    void CapSave(Target& t, const char* name, float mul);
    void CapStep(const Surface& rt);
    // Clear's arguments past the state
    struct ClearArgs {
        uint32_t count;
        const uint8_t* rects;
        uint32_t flags;
        uint32_t color;
        float z;
        uint32_t stencil;
    };
    void ClearTarget(Target& t, const State& s, const ClearArgs& c, std::vector<D3D12_RECT>& rs);
    // Copy's guest lists: count src_rects, dst_points 0 or one per rect
    struct CopyList {
        uint32_t count;
        uint32_t src_rects;
        uint32_t dst_points;
    };
    void CopyByDraw(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state, Target& dst_t,
                    const CopyList& l);
    void CopyByRegion(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state, Target& dst_t,
                      const CopyList& l);
    // the target as a guest sized BGRA copy source, and the shrunk copy that holds it when it is not one already
    struct CopySource {
        ID3D12Resource* res;
        ComPtr<ID3D12Resource> down;
    };
    CopySource PrepareCopySource(Target& t, const ShrinkOptions& o);
    // RecordLate's steps, under late_lock_
    struct Late;
    void ResetLate(Late& l, const Target& t);
    ID3D12Resource* PrepareLateSource(Target& t, Late& l);
    void RecordLateCopy(Late& l, uint32_t slot, ID3D12Resource* src);

    ComPtr<IDXGIFactory4> factory_;
    ComPtr<ID3D12Device> dev_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swap_;
    ComPtr<ID3D12Resource> bb_[kFrames];
    ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    uint64_t fence_value_ = 0;
    Frame frames_[kFrames];
    uint64_t frame_ = 0;
    ComPtr<ID3D12GraphicsCommandList> cl_;
    ComPtr<ID3D12DescriptorHeap> srv_heap_;
    ComPtr<ID3D12DescriptorHeap> smp_heap_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    ComPtr<ID3D12DescriptorHeap> dsv_heap_;
    ComPtr<ID3D12DescriptorHeap> ui_heap_;
    uint32_t srv_inc_ = 0;
    uint32_t smp_inc_ = 0;
    uint32_t rtv_inc_ = 0;
    uint32_t dsv_inc_ = 0;
    std::vector<uint32_t> free_rtv_;
    std::vector<uint32_t> free_dsv_;
    ComPtr<ID3D12RootSignature> root_;
    std::unordered_map<uint64_t, ComPtr<ID3DBlob>> blobs_;
    std::unordered_map<uint64_t, uint64_t> shader_src_;  // vertex and pixel program keys to source hash
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> psos_;
    std::unique_ptr<DofPipelineCache> dof_pipelines_;
    DofGpuTiming dof_timing_;
    // At most one bounded report can be in flight. Disk work never blocks the render
    // thread; overloaded export batches are counted rather than queued indefinitely.
    std::future<bool> dof_csv_write_;
    uint64_t dof_csv_dropped_ = 0;
    uint32_t dof_csv_pending_frames_ = 0;
    bool dof_csv_started_ = false, dof_csv_failed_ = false;
    uint64_t dof_prime_signature_ = 0, dof_embedded_loads_ = 0, dof_embedded_missing_ = 0;
    enum class DofPreparation { Dormant, Drain, Queue, Cpu, Resources, Warm, Gpu, Complete };
    DofPreparation dof_preparation_ = DofPreparation::Dormant;
    struct DofPreparedCpu {
        std::unique_ptr<FidelityFxDof> context;
    };
    std::future<DofPreparedCpu> dof_cpu_preparation_;
    uint64_t dof_cpu_signature_ = 0, dof_drain_fence_ = 0, dof_warm_fence_ = 0;
    uint64_t dof_loading_generation_ = 0, dof_loading_after_present_ = 0;
    bool dof_preparation_started_ = false, dof_prepared_ = false, dof_resources_quiescent_ = false;
    bool dof_warm_recorded_ = false;
    PerfClock::time_point dof_preparation_start_{};
    std::unordered_map<uint64_t, uint32_t> samplers_;
    uint32_t smp_used_ = 0;
    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint64_t, Tex> textures_;  // by data address, a P8 texture's with its palette's above
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> blit_psos_;
    Extension* ext_ = nullptr;
    // one frame capture: every draw in a text file, each target as the frame leaves it, the textures it read
    FILE* cap_ = nullptr;
    std::filesystem::path cap_dir_;
    uint32_t cap_i_ = 0;
    uint32_t cap_rt_ = 0;
    std::unordered_set<uint32_t> cap_seen_;
    std::unordered_set<uint64_t> cap_sh_;
    struct CapTex {
        ComPtr<ID3D12Resource> res;
        DXGI_FORMAT fmt;
        uint32_t data;
        uint32_t w;
        uint32_t h;
    };
    std::vector<CapTex> cap_tex_;
    ComPtr<ID3D12Resource> lut_;
    D3D12_RESOURCE_STATES lut_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
    uint8_t ramp_[768] = {};
    bool ramp_dirty_ = false;
    bool gamma_ = false;
    Target* bound_rt_ = nullptr;
    Target* bound_ds_ = nullptr;
    uint32_t guest_w_ = 640;
    uint32_t guest_h_ = 480;
    uint32_t bb_w_ = 0;
    uint32_t bb_h_ = 0;
    float scale_ = 1.0f;
    uint32_t msaa_ = 1;      // samples in use
    uint32_t msaa_set_ = 0;  // the setting they came from
    // ambient occlusion: the raw pass and its first blur, each with view z beside it
    ComPtr<ID3D12Resource> ao_[kAoTargets];
    D3D12_RESOURCE_STATES ao_state_[kAoTargets] = {};
    uint32_t ao_w_ = 0;
    uint32_t ao_h_ = 0;
    uint64_t ao_frame_ = 0;  // presents_ plus one when the pass last ran, 0 before it ever did
    ComPtr<ID3D12Resource> dof_filter_[kDofTargets];
    D3D12_RESOURCE_STATES dof_states_[kDofTargets][kDofLevels] = {};
    uint32_t dof_levels_ = 0;
    ComPtr<ID3D12Resource> dof_layers_[kDofTargets];
    D3D12_RESOURCE_STATES dof_layer_states_[kDofTargets] = {};
    uint32_t dof_skip_reported_ = 0;
    bool dof_reported_ = false;
    bool particle_float_bounds_ = false;  // immutable device capability; only the 2x path uses FLOAT bounds
    bool particle_visibility_cache_ = false;
    uint64_t particle_depth_serial_ = 1;  // actual depth mutations, never mere binding/state changes
    struct ParticleDof {
        bool marker = false;
        bool world_valid = false;
        uint64_t world_frame = 0;
        uint32_t world_w = 0, world_h = 0;
        float optics[4] = {};
        struct DepthProjection {
            ID3D12Resource* resource = nullptr;
            uint64_t frame = 0;
            std::array<float, 4> value{};  // raw-depth numerator/offset, viewport min/max Z
        } depth_projection[16];
        ParticleDofGlobals globals{};
        ParticleDofGeometryParams params{};
        std::vector<NativeParticleVertex> vertices;
        std::vector<ParticleDofQuad> quads;
        ComPtr<ID3D12Resource> atlas[2];
        D3D12_RESOURCE_STATES atlas_states[2] = {};
        uint32_t atlas_extent = 0, atlas_failed = 0;
        ComPtr<ID3D12Resource> visibility;
        D3D12_RESOURCE_STATES visibility_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        uint32_t visibility_extent = 0, visibility_failed = 0;
        bool visibility_active = false;
        float visibility_target[4] = {};
        std::vector<ParticleDofVisibilityMap> visibility_maps;
        uint64_t visibility_pages = 0, visibility_quads = 0;
        uint64_t atlas_batches = 0, atlas_pages = 0, atlas_quads = 0, atlas_fallback = 0;
        ComPtr<ID3D12Resource> shared[2]; // R8 2MSAA support, RG32F single-sample transmission
        D3D12_RESOURCE_STATES shared_states[2] = {};
        uint32_t shared_width = 0, shared_height = 0;
        std::array<uint64_t, size_t(ParticleSharedReject::Total)> shared_admission{};
        uint64_t shared_pages = 0, shared_unavailable = 0;
        double shared_sum_area = 0, shared_union_area = 0;
        std::vector<D3D12_RECT> atlas_rects;
        struct Snapshot {
            ComPtr<ID3D12Resource> depth;
            ComPtr<ID3D12Resource> range;
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
            D3D12_RESOURCE_STATES range_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            ID3D12Resource* source = nullptr;
            uint64_t serial = 0, frame = ~0ull;
            uint64_t used = 0;
            bool range_view_used = false;
        } snapshots[kParticleDofSnapshots];
        uint64_t uses = 0, copies = 0, reused = 0, copy_bytes = 0, range_passes = 0;
        uint64_t batches = 0, active = 0, focused = 0, invalid = 0, early = 0, rejected = 0;
        uint64_t additive = 0, subtractive = 0;
        uint64_t coverage_batches = 0, coverage_unknown = 0, coverage_varying_depth = 0;
        std::array<uint64_t, size_t(ParticleDofGeometryRejection::Count)> geometry_rejections{};
        std::unordered_set<uint64_t> rejected_states;
        std::unordered_set<uint64_t> rejected_programs;
        uint32_t draw_active = 0;
        uint32_t reported = 0;
        bool active_reported = false;
        bool previous_world = false;
    } particle_dof_;
    FidelityFxDof ffx_dof_;
    ComPtr<ID3D12Resource> ffx_dof_targets_[3];  // prepared color, encoded CoC depth, official output
    D3D12_RESOURCE_STATES ffx_dof_states_[3] = {};
    uint32_t ffx_dof_failed_ = 0;  // one failure latch per Standard/Cinematic profile
    bool ffx_dof_reported_ = false;
    struct WeaponDof {
        ComPtr<ID3DBlob> coverage_ps;
        uint64_t coverage_hash = 0;
        bool conservative_supported = false;
        WeaponDofRegion region;
        WeaponDofDepthEncoding encoding;
        ComPtr<ID3D12Resource> tiles;
        D3D12_RESOURCE_STATES tiles_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        ComPtr<ID3D12Resource> before;
        D3D12_RESOURCE_STATES before_state = D3D12_RESOURCE_STATE_COPY_DEST;
        ComPtr<ID3D12Resource> packed[kDofTargets];
        D3D12_RESOURCE_STATES packed_state[kDofTargets] = {};
        ID3D12Resource* rt = nullptr;
        ID3D12Resource* ds = nullptr;
        Viewport viewport{};
        float parameters[4 * kWeaponDofRects] = {};
        uint32_t serial = 0;
        uint64_t frame = 0;
        bool pending = false;
        uint32_t skipped = 0;
        bool reported = false;
    } weapon_dof_;
    // surfaces locked without a wait: copies in flight, the newest finished one goes to guest memory
    struct Late {
        ComPtr<ID3D12Resource> rb[kFrames];
        ComPtr<ID3D12Resource> down;
        D3D12_RESOURCE_STATES down_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        uint64_t fence[kFrames] = {};
        uint64_t frame[kFrames] = {};
        uint64_t delivered = 0;
        uint32_t gw = 0;
        uint32_t gh = 0;
        uint32_t row_pitch = 0;
        uint32_t next = 0;
    };
    std::unordered_map<uint32_t, Late> late_;
    std::mutex late_lock_;                      // DeliverLate runs on the title's thread
    std::atomic<uint64_t> presents_seen_{0};    // presents_ for DeliverLate
    std::atomic<uint64_t> present_fence_[kFrames + 1] = {};  // by presents_, for WaitGpuFrame
    Assembled asm_;  // kept between draws for its buffers
    bool tearing_ = false;
    // HDR10 output: the window's display, the output asked and in use, the frame and panel targets it draws first
    HMONITOR hdr_mon_ = nullptr;
    bool hdr_display_ = false;
    bool hdr_want_ = false;
    bool hdr_out_ = false;
    float paper_nits_ = kDefaultPaperNits;
    float peak_nits_ = kDefaultPeakNits;
    ComPtr<ID3D12Resource> out_[kOutTargets];
    D3D12_RESOURCE_STATES out_state_[kOutTargets] = {};
    uint32_t out_w_ = 0;
    uint32_t out_h_ = 0;
    // SMAA: edges, blend weights and the blended frame, at the frame's size; its area and search lookups
    ComPtr<ID3D12Resource> smaa_[kSmaaTargets];
    ComPtr<ID3D12Resource> smaa_lut_[2];
    D3D12_RESOURCE_STATES smaa_state_[kSmaaTargets] = {};
    uint32_t smaa_w_ = 0;
    uint32_t smaa_h_ = 0;
    DXGI_FORMAT smaa_fmt_ = DXGI_FORMAT_UNKNOWN;
    uint64_t presents_ = 0;
    double fps_ = 0;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t fps_n_ = 0;
};

bool D3D12Backend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    HWND hwnd = static_cast<HWND>(app::Window());
    if (!CreateDevice()) return false;
    if (!CreateSwapChain(hwnd)) return false;
    CreateHeaps();
    CreateBackBufferViews();
    if (!CreateRootSignature()) return false;
    CreateFrames();
    CreateGammaLut();
    ImGui_ImplDX12_Init(dev_.Get(), kFrames, DXGI_FORMAT_B8G8R8A8_UNORM, ui_heap_.Get(),
                        ui_heap_->GetCPUDescriptorHandleForHeapStart(), ui_heap_->GetGPUDescriptorHandleForHeapStart());
    LoadSettings();
    Begin();
    try { dof_pipelines_ = std::make_unique<DofPipelineCache>(dev_.Get()); }
    catch (...) { LOGR("Particle DOF pipeline worker unavailable; native particles retained"); }
    D3D12_FEATURE_DATA_D3D12_OPTIONS conservative{};
    weapon_dof_.conservative_supported = SUCCEEDED(dev_->CheckFeatureSupport(
        D3D12_FEATURE_D3D12_OPTIONS, &conservative, sizeof(conservative))) &&
        conservative.ConservativeRasterizationTier >= D3D12_CONSERVATIVE_RASTERIZATION_TIER_2;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT particle_bounds{DXGI_FORMAT_R32G32_FLOAT};
    const auto particle_bounds_required = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_RENDER_TARGET |
        D3D12_FORMAT_SUPPORT1_SHADER_LOAD | D3D12_FORMAT_SUPPORT1_SHADER_GATHER;
    particle_float_bounds_ = SUCCEEDED(dev_->CheckFeatureSupport(
        D3D12_FEATURE_FORMAT_SUPPORT, &particle_bounds, sizeof(particle_bounds))) &&
        (particle_bounds.Support1 & particle_bounds_required) == particle_bounds_required;
    LOGR("Particle DOF 2x depth bounds: %s", particle_float_bounds_ ? "FLOAT Gather" : "UINT Load fallback");
    D3D12_FEATURE_DATA_FORMAT_SUPPORT particle_visibility{DXGI_FORMAT_R16_FLOAT};
    particle_visibility_cache_ = particle_float_bounds_ && SUCCEEDED(dev_->CheckFeatureSupport(
        D3D12_FEATURE_FORMAT_SUPPORT, &particle_visibility, sizeof(particle_visibility))) &&
        (particle_visibility.Support1 & particle_bounds_required) == particle_bounds_required;
    BeginDofTiming();
    LOGI("d3d12: %ux%u guest, %ux%u window, render scale %.3f", width, height, bb_w_, bb_h_, scale_);
    return true;
}

bool D3D12Backend::CreateDevice() {
    if (GetEnvironmentVariableW(L"XBE_D3D12_DEBUG", nullptr, 0)) {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
    }
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_)))) return false;
    // dedicated GPU first on hybrid laptops
    ComPtr<IDXGIFactory6> f6;
    ComPtr<IDXGIAdapter1> adapter;
    if (SUCCEEDED(factory_.As(&f6))) {
        f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    }
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev_))) &&
        FAILED(D3D12CreateDevice((adapter = nullptr).Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev_)))) {
        LOGE("d3d12: no device");
        return false;
    }
    DXGI_ADAPTER_DESC1 ad{};
    if (adapter) adapter->GetDesc1(&ad);
    LOGI("d3d12: adapter %ls", adapter ? ad.Description : L"default");
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    dev_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_));
    ComPtr<IDXGIFactory5> f5;
    BOOL tear = FALSE;
    if (SUCCEEDED(factory_.As(&f5))) f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tear, sizeof(tear));
    tearing_ = tear;
    return true;
}

bool D3D12Backend::CreateSwapChain(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    bb_w_ = std::max<LONG>(rc.right - rc.left, 1);
    bb_h_ = std::max<LONG>(rc.bottom - rc.top, 1);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = bb_w_;
    sd.Height = bb_h_;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kFrames;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd, &sd, nullptr, nullptr, &sc1))) {
        LOGE("d3d12: no swap chain");
        return false;
    }
    factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    sc1.As(&swap_);
    return true;
}

uint32_t D3D12Backend::CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE t, uint32_t n, bool visible,
                                  ComPtr<ID3D12DescriptorHeap>& out) {
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = t;
    hd.NumDescriptors = n;
    hd.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    dev_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&out));
    return dev_->GetDescriptorHandleIncrementSize(t);
}

// RTVs: the targets' pool, then the back buffers, occlusion, output, SMAA and the DOF filter mips
void D3D12Backend::CreateHeaps() {
    constexpr uint32_t kRtvTotal = kRtvCount + kFrames + kAoTargets + kOutTargets + kSmaaTargets +
        kDofLevels * kDofTargets + kDofLayerViews + kDofTargets + 2 + kParticleDofSnapshots + 6;
    srv_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvCount, true, srv_heap_);
    smp_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerCount, true, smp_heap_);
    rtv_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvTotal, false, rtv_heap_);
    dsv_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvCount, false, dsv_heap_);
    CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true, ui_heap_);
    for (uint32_t i = kRtvCount; i-- > 0;) free_rtv_.push_back(i);
    for (uint32_t i = kDsvCount; i-- > 0;) free_dsv_.push_back(i);
}

void D3D12Backend::CreateBackBufferViews() {
    for (int i = 0; i < kFrames; i++) {
        swap_->GetBuffer(i, IID_PPV_ARGS(&bb_[i]));
        dev_->CreateRenderTargetView(bb_[i].Get(), nullptr, Cpu(rtv_heap_.Get(), kRtvCount + i, rtv_inc_));
    }
}

bool D3D12Backend::CreateRootSignature() {
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 4;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    ranges[1].NumDescriptors = 4;
    D3D12_ROOT_PARAMETER params[4] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    for (int i = 0; i < 2; i++) {
        params[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[2 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[2 + i].ShaderVisibility = i == 0 ? D3D12_SHADER_VISIBILITY_ALL : D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 4;
    rsd.pParameters = params;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> rs;
    ComPtr<ID3DBlob> err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs, &err))) return false;
    dev_->CreateRootSignature(0, rs->GetBufferPointer(), rs->GetBufferSize(), IID_PPV_ARGS(&root_));
    return true;
}

void D3D12Backend::CreateFrames() {
    for (auto& f : frames_) {
        dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc));
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC bd = BufferDesc(kUploadSize);
        dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                      IID_PPV_ARGS(&f.upload));
        f.upload->Map(0, nullptr, reinterpret_cast<void**>(&f.map));
    }
    dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[0].alloc.Get(), nullptr, IID_PPV_ARGS(&cl_));
}

// gamma ramp as a 256 x 1 lookup
void D3D12Backend::CreateGammaLut() {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, 256, 1);
    dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                  IID_PPV_ARGS(&lut_));
}

void D3D12Backend::LoadSettings() {
    scale_ = app::RenderScale(guest_h_);
    msaa_set_ = game::Opt().msaa;
    msaa_ = Samples(msaa_set_);
    ext_ = g_extension.get();
    if (ext_) {
        ext_->Settings();
        ext_->Reset(*this);
    }
}

void D3D12Backend::Begin() {
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get(), smp_heap_.Get()};
    cl_->SetDescriptorHeaps(2, heaps);
    cl_->SetGraphicsRootSignature(root_.Get());
    bound_rt_ = bound_ds_ = nullptr;
}

void D3D12Backend::Submit(bool wait, bool final_native_present) {
    dof_timing_.Resolve(cl_.Get());
    cl_->Close();
    ID3D12CommandList* lists[] = {cl_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    queue_->Signal(fence_.Get(), ++fence_value_);
    if (dof_warm_recorded_) {
        dof_warm_fence_ = fence_value_;
        dof_warm_recorded_ = false;
    }
    dof_timing_.Submitted(fence_value_, presents_ + 1, final_native_present);
    Frame& f = CurrentFrame();
    f.fence = fence_value_;
    if (wait) {
        WaitFence(fence_value_);
        f.alloc->Reset();
        f.garbage.clear();
        f.used = 0;
        cl_->Reset(f.alloc.Get(), nullptr);
        Begin();
    }
}

void D3D12Backend::BeginDofTiming() {
    const bool enabled = dof_prepared_ && DofGpuProfilingEnabled();
    if (enabled && !dof_timing_.Available()) dof_timing_.Init(dev_.Get(), queue_.Get());
    dof_timing_.BeginFrame(presents_ + 1, enabled, fence_->GetCompletedValue());
    if (enabled && presents_ && presents_ % 120 == 0) {
        const auto report = dof_timing_.TakeReport();
        PublishDofGpuReport(report);
        ExportDofTiming(report);
        LOGR("DOF GPU timing: %llu complete frames, %llu invalid, %llu overflow, %llu busy; no GPU waits",
             (unsigned long long)report.complete_frames, (unsigned long long)report.invalid_frames,
             (unsigned long long)report.overflow_frames, (unsigned long long)report.busy_frames);
        LOGR("DOF GPU settings at report: particle coverage reuse %s",
             DofParticleCoverageReuseEnabled() ? "on" : "off");
        // Keep the existing bounded summary format consumed by capture.ps1; raw rows
        // belong only in the asynchronous CSV, never one log line per GPU interval.
        for (uint32_t i = 0; i < DofGpuTiming::kStageCount; ++i) {
            const auto& stat = report.stage[i];
            if (!stat.intervals) continue;
            LOGR("DOF GPU %s: median %.3f ms, p95 %.3f ms, %u sampled frames, %llu intervals",
                 DofGpuTiming::StageName(DofGpuTiming::Stage(i)), stat.median_frame_ms, stat.p95_frame_ms,
                 stat.percentile_frames, (unsigned long long)stat.intervals);
        }
    }
}

void D3D12Backend::ExportDofTiming(const DofGpuTiming::Report& report) {
    if (!report.frame_sample_count || !report.frequency) return;
    if (dof_csv_write_.valid()) {
        if (dof_csv_write_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            dof_csv_dropped_ += report.frame_sample_count;
            return;
        }
        bool written = false;
        try { written = dof_csv_write_.get(); } catch (...) {}
        if (!written) dof_csv_dropped_ += dof_csv_pending_frames_;
        if (!written && !dof_csv_failed_) {
            dof_csv_failed_ = true;
            LOGR("DOF GPU frame export failed; profiling remains available in the settings report");
        }
    }
    const auto path = app::ExeDir() / "logs" / "dof-gpu-frames.csv";
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const bool first = !dof_csv_started_;
    const uint64_t dropped = dof_csv_dropped_;
    try {
        dof_csv_write_ = std::async(std::launch::async, [report, path, timestamp, first, dropped] {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            if (ec) return false;
            const bool header = first || !std::filesystem::exists(path, ec) || std::filesystem::file_size(path, ec) == 0;
            std::ofstream out(path, first ? std::ios::trunc : std::ios::app);
            if (!out) return false;
            if (header) {
                out << "report_unix_ms,frame_id,fence,frequency,valid,invalid_frames,busy_frames,overflow_frames,"
                       "dropped_intervals,auto_closed_scopes,unresolved_frames,readback_errors,pending_frames,"
                       "frame_sample_evicted,export_dropped_frames";
                for (uint32_t stage = 0; stage < DofGpuTiming::kStageCount; ++stage) {
                    std::string name = DofGpuTiming::StageName(DofGpuTiming::Stage(stage));
                    std::replace(name.begin(), name.end(), ' ', '_');
                    out << ",s" << stage << '_' << name << "_ticks,s" << stage << '_' << name << "_intervals";
                }
                out << '\n';
            }
            for (uint32_t i = 0; i < report.frame_sample_count; ++i) {
                const auto& frame = report.frame_samples[i];
                out << timestamp << ',' << frame.frame_id << ',' << frame.fence << ',' << report.frequency << ",1,"
                    << report.invalid_frames << ',' << report.busy_frames << ',' << report.overflow_frames << ','
                    << report.dropped_intervals << ',' << report.auto_closed_scopes << ',' << report.unresolved_frames << ','
                    << report.readback_errors << ',' << report.pending_frames << ',' << report.frame_sample_evicted << ',' << dropped;
                for (uint32_t stage = 0; stage < DofGpuTiming::kStageCount; ++stage)
                    out << ',' << frame.ticks[stage] << ',' << frame.intervals[stage];
                out << '\n';
            }
            out.flush();
            return bool(out);
        });
        dof_csv_started_ = true;
        dof_csv_pending_frames_ = report.frame_sample_count;
    } catch (...) { dof_csv_dropped_ += report.frame_sample_count; }
}

void D3D12Backend::Flush() { Submit(true); }

void D3D12Backend::WaitFence(uint64_t value) {
    fence_->SetEventOnCompletion(value, event_);
    WaitForSingleObject(event_, INFINITE);
}

void D3D12Backend::SetFullViewport(uint32_t w, uint32_t h) {
    D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    D3D12_RECT scissor{0, 0, LONG(w), LONG(h)};
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &scissor);
}

// one w x h colour target for a Blit, the draw's bound targets forgotten
void D3D12Backend::SetBlitTarget(D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w, uint32_t h) {
    cl_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    SetFullViewport(w, h);
}

D3D12Backend::Alloc D3D12Backend::Upload(uint64_t size, uint64_t align) {
    Frame& f = CurrentFrame();
    g_perf.upload += size;
    uint64_t o = (f.used + align - 1) & ~(align - 1);
    if (o + size <= kUploadSize) {
        f.used = o + size;
        return {f.upload.Get(), o, f.map + o, f.upload->GetGPUVirtualAddress() + o};
    }
    // too big for the ring: a buffer of its own, released with the frame
    g_perf.overflow++;
    ComPtr<ID3D12Resource> r;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
    constexpr uint64_t kOwnAlign = 0x10000;  // a committed buffer's size granularity
    D3D12_RESOURCE_DESC bd = BufferDesc((size + kOwnAlign - 1) & ~(kOwnAlign - 1));
    dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                  IID_PPV_ARGS(&r));
    uint8_t* p = nullptr;
    r->Map(0, nullptr, reinterpret_cast<void**>(&p));
    f.garbage.push_back(r);
    return {r.Get(), 0, p, r->GetGPUVirtualAddress()};
}

uint32_t D3D12Backend::AllocSrvSlots(uint32_t n) {
    Frame& f = CurrentFrame();
    const uint32_t per = kSrvCount / kFrames;
    if (f.srv + n > per) {
        LOG_FIRST(4, "d3d12: descriptor ring wrapped within a frame");
        f.srv = 0;
    }
    uint32_t i = uint32_t(frame_ % kFrames) * per + f.srv;
    f.srv += n;
    return i;
}

void D3D12Backend::Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES& cur, D3D12_RESOURCE_STATES want) {
    if (weapon_dof_.pending && r == weapon_dof_.ds &&
        (want & (D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_RESOLVE_DEST |
                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS))) weapon_dof_.region.Invalidate();
    if (cur == want) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = cur;
    b.Transition.StateAfter = want;
    cl_->ResourceBarrier(1, &b);
    cur = want;
}

// targets_ key of a surface's colour or depth target
uint64_t TargetMapKey(uint32_t data, bool depth) { return uint64_t(data) << 1 | (depth ? 1 : 0); }

// a resized target gives back its view and its resources
void D3D12Backend::ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it) {
    if (it->second.depth) InvalidateParticleDepth(it->second.res.Get());
    if (bound_rt_ == &it->second || bound_ds_ == &it->second) bound_rt_ = bound_ds_ = nullptr;
    (it->second.depth ? free_dsv_ : free_rtv_).push_back(it->second.view);
    CurrentFrame().garbage.push_back(it->second.res);
    if (it->second.fb) CurrentFrame().garbage.push_back(it->second.fb);
    targets_.erase(it);
}

// the host texture of a new target, at render scale or the extension's; the scene and its depth multisampled
bool D3D12Backend::CreateTargetResource(Target& t, const Surface& s) {
    float sc = scale_;
    if (ext_) ext_->MakeTarget(s, t.depth, t.gw, t.gh, sc, t.fmt);
    t.w = std::max(1u, uint32_t(std::lround(t.gw * sc)));
    t.h = std::max(1u, uint32_t(std::lround(t.gh * sc)));
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_UNKNOWN, t.w, t.h);
    // the game's small maps and blur chain stay as they are
    td.SampleDesc.Count = t.gw == guest_w_ && t.gh == guest_h_ ? msaa_ : 1;
    D3D12_CLEAR_VALUE cv{};
    if (t.depth) {
        td.Format = DXGI_FORMAT_R24G8_TYPELESS;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        cv.DepthStencil.Depth = 1;
        t.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    } else {
        td.Format = t.fmt;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        cv.Format = td.Format;
        t.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, t.state, &cv, IID_PPV_ARGS(&t.res)))) {
        LOGE("d3d12: no %ux%u target with %u samples", t.w, t.h, td.SampleDesc.Count);
        return false;
    }
    // Reserve the scene snapshot with its target, before a reload can need it. This allocates only;
    // Feedback still performs the first real copy/resolve when the effect consumes the scene.
    if (!t.depth && t.gw == guest_w_ && t.gh == guest_h_ &&
        game::Opt().reload_blur && game::Opt().modern_reload_dof) EnsureFeedbackResource(t);
    return true;
}

// a view from the free pool, cleared; false when the pool is empty
bool D3D12Backend::CreateTargetView(Target& t) {
    auto& pool = t.depth ? free_dsv_ : free_rtv_;
    if (pool.empty()) return false;
    t.view = pool.back();
    pool.pop_back();
    if (t.depth) {
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        bool ms = Ms(t) > 1;
        dv.ViewDimension = ms ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
        dev_->CreateDepthStencilView(t.res.Get(), &dv, Cpu(dsv_heap_.Get(), t.view, dsv_inc_));
        float one = 1;
        cl_->ClearDepthStencilView(Cpu(dsv_heap_.Get(), t.view, dsv_inc_),
                                   D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, one, 0, 0, nullptr);
        InvalidateParticleDepth(t.res.Get());
    } else {
        dev_->CreateRenderTargetView(t.res.Get(), nullptr, Cpu(rtv_heap_.Get(), t.view, rtv_inc_));
        float black[4] = {0, 0, 0, 0};
        cl_->ClearRenderTargetView(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), black, 0, nullptr);
    }
    return true;
}

// alt: the extension's bits of the key
D3D12Backend::TargetSlot D3D12Backend::FindTargetSlot(const Surface& s, bool depth, uint64_t alt) {
    TargetSlot slot;
    slot.data = mem::VirtualFromPhysical(s.data);
    slot.key = TargetMapKey(slot.data, depth) | alt;
    SurfaceSize(s, slot.gw, slot.gh);
    slot.it = targets_.find(slot.key);
    slot.sized = slot.it != targets_.end() && slot.it->second.gw == slot.gw && slot.it->second.gh == slot.gh;
    return slot;
}

// the surface's target at its size, null when there is none
Target* D3D12Backend::FindTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint64_t alt = 0;
    if (ext_ && !ext_->TargetKey(s, depth, alt)) return nullptr;
    TargetSlot slot = FindTargetSlot(s, depth, alt);
    if (!slot.sized) return nullptr;
    return &slot.it->second;
}

// the surface's target, made again at its size when that changed
Target* D3D12Backend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint64_t alt = 0;
    if (ext_ && !ext_->TargetKey(s, depth, alt)) return nullptr;
    TargetSlot slot = FindTargetSlot(s, depth, alt);
    if (slot.sized) return &slot.it->second;
    if (slot.it != targets_.end()) ReleaseTarget(slot.it);
    Target t;
    t.depth = depth;
    t.gw = slot.gw;
    t.gh = slot.gh;
    if (!CreateTargetResource(t, s)) return nullptr;
    g_perf.targets++;
    // an empty pool drops the new resource with t
    if (!CreateTargetView(t)) return nullptr;
    t.data = slot.data;
    t.bytes = GuestPitch(slot.gw) * slot.gh;
    if (depth) t.bytes = slot.gw * slot.gh * 4;
    t.epoch = DirtyEpoch(slot.data);
    t.sig = MemSig(slot.data, t.bytes);
    Target& ref = targets_[slot.key];
    ref = std::move(t);
    return &ref;
}

// the sample count the adapter has for the scene's formats, the one asked for or the next under it
uint32_t D3D12Backend::Samples(uint32_t want) {
    uint32_t n = std::clamp(want, 1u, 8u);
    auto has = [&](DXGI_FORMAT f) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{f, n};
        HRESULT hr = dev_->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof(q));
        return SUCCEEDED(hr) && q.NumQualityLevels;
    };
    while (n > 1 && !(has(kBgra) && has(DXGI_FORMAT_D24_UNORM_S8_UINT))) n--;
    LOGI("d3d12: MSAA %ux (asked %u)", n, want);
    return n;
}

bool D3D12Backend::BindTargets(const State& s) {
    Target* rt = EnsureTarget(s.rt, false);
    Target* ds = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? EnsureTarget(s.zs, true) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h || Ms(*rt) != Ms(*ds))) ds = nullptr;
    if (!rt && !ds) return false;
    if (rt) {
        if (LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
        Transition(rt->res.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        rt->drawn = true;
    }
    if (ds) {
        Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        ds->drawn = true;
    }
    if (rt != bound_rt_ || ds != bound_ds_) {
        D3D12_CPU_DESCRIPTOR_HANDLE rh = rt ? Cpu(rtv_heap_.Get(), rt->view, rtv_inc_) : D3D12_CPU_DESCRIPTOR_HANDLE{};
        D3D12_CPU_DESCRIPTOR_HANDLE dh = ds ? Cpu(dsv_heap_.Get(), ds->view, dsv_inc_) : D3D12_CPU_DESCRIPTOR_HANDLE{};
        cl_->OMSetRenderTargets(rt ? 1 : 0, rt ? &rh : nullptr, FALSE, ds ? &dh : nullptr);
        bound_rt_ = rt;
        bound_ds_ = ds;
    }
    return true;
}

// a depth buffer drawn as colour into another depth buffer's memory: copied as depth
bool D3D12Backend::DepthCopy(const State& s) {
    TextureDesc d;
    if (!s.rt.header || !s.textures[0] || !Describe(s.textures[0], d) || Format(d.format).depth || d.cube ||
        d.depth != 1) {
        return false;
    }
    auto di = targets_.find(TargetMapKey(mem::VirtualFromPhysical(s.rt.data), true));
    auto si = targets_.find(TargetMapKey(d.data, true));
    if (di == targets_.end() || si == targets_.end() || di == si) return false;
    Target& dst = di->second;
    Target& src = si->second;
    D3D12_RESOURCE_STATES* st;
    ID3D12Resource* from = Flat(src, st);
    if (!from) return false;
    if (cap_) fprintf(cap_, "%04u depth copy %08X to %08X\n", cap_i_++, src.data, dst.data);
    Transition(from, *st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(dst.res.Get(), dst.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const auto projection = ParticleDepthProjection(src.res.Get());
    if (!DepthPass(from, 1, Cpu(dsv_heap_.Get(), dst.view, dsv_inc_), dst.w, dst.h, Ms(dst))) return false;
    InvalidateParticleDepth(dst.res.Get());
    // DepthPass resamples raw depth without changing its camera/viewport encoding.
    SetParticleDepthProjection(dst.res.Get(), projection);
    dst.drawn = true;
    return true;
}

// DepthPass's pipeline, kept in blit_psos_: writes depth only, always passing; null when it can't be made
ID3D12PipelineState* D3D12Backend::EnsureDepthCopyPipeline(bool ms, uint32_t samples) {
    auto& pso = blit_psos_[~(uint64_t(ms) | uint64_t(samples) << 1)];
    if (pso) return pso.Get();
    std::string hlsl = std::string(ms ? "#define MS\n" : "") + kDepthHlsl;
    uint64_t hash;
    ID3DBlob* vs = Compile(hlsl, "vs", "vs_5_0", hash);
    ID3DBlob* ps = Compile(hlsl, "psmain", "ps_5_0", hash);
    if (!vs || !ps) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = root_.Get();
    p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    p.SampleMask = UINT_MAX;
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    p.RasterizerState.DepthClipEnable = TRUE;
    p.DepthStencilState.DepthEnable = TRUE;
    p.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    p.SampleDesc.Count = samples;
    if (FAILED(dev_->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso.Get();
}

// src's depth over the w x h depth target at dh, a multisampled src's first sample
bool D3D12Backend::DepthPass(ID3D12Resource* src, uint32_t src_samples, D3D12_CPU_DESCRIPTOR_HANDLE dh, uint32_t w,
                             uint32_t h, uint32_t samples) {
    bool ms = src_samples > 1;
    ID3D12PipelineState* pso = EnsureDepthCopyPipeline(ms, samples);
    if (!pso) return false;
    cl_->OMSetRenderTargets(0, nullptr, FALSE, &dh);
    SetFullViewport(w, h);
    uint32_t slot = AllocSrvSlots(1);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    sv.ViewDimension = ms ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (!ms) sv.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(src, &sv, Cpu(srv_heap_.Get(), slot, srv_inc_));
    Alloc cb = Upload(256, 256);
    DrawFullScreen(pso, cb.gpu, slot);
    bound_rt_ = bound_ds_ = nullptr;
    return true;
}

// multiplies occlusion from the scene's depth into its colour, p as d3d::Occlusion sends it
void D3D12Backend::Occlusion(const State& s, const float p[4 * kOcclusionRects]) {
    Target* rt = FindTarget(s.rt, false);
    Target* ds = FindTarget(s.zs, true);
    if (!OcclusionFits(rt, ds)) return;
    const Viewport& v = s.viewport;
    D3D12_RECT rect = ViewRect(v, *rt);
    if (rect.right <= rect.left || rect.bottom <= rect.top || v.max_z <= v.min_z) return;
    if (!EnsureOcclusionTargets(rt->w, rt->h)) return;
    ao_frame_ = presents_ + 1;
    if (cap_) {
        CapStep(s.rt);
        fprintf(cap_, "%04u occlusion rt %08X ds %08X\n", cap_i_++, s.rt.data, s.zs.data);
    }
    uint32_t samples = Ms(*ds);
    LOG_FIRST(1, "d3d12: ambient occlusion %s %ux%u, %u samples, projection %.4f %.4f, view z %.6f / (depth - %.8f)",
              p[6] ? "GTAO" : "SSAO", rt->w, rt->h, samples, 1 / p[0], 1 / p[1], p[4], p[5]);
    float c[kAoConstants];
    OcclusionConstants(v, rect, p, c);
    const char* entry = p[6] ? "gtao" : "occlusion";
    AoPass raw{.entry = entry, .rtv = OcclusionView(0), .depth = ds->res.Get(), .ms_depth = samples > 1};
    AoPass across{.entry = "occlusion_blur", .rtv = OcclusionView(1), .src = ao_[0].Get(), .step_x = 1};
    D3D12_CPU_DESCRIPTOR_HANDLE scene = Cpu(rtv_heap_.Get(), rt->view, rtv_inc_);
    AoPass down{.entry = "occlusion_apply", .out = rt->fmt, .samples = Ms(*rt), .blend = true, .rtv = scene,
                .src = ao_[1].Get(), .step_y = 1};
    bound_rt_ = bound_ds_ = nullptr;
    Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(ao_[0].Get(), ao_state_[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
    bool ok = OcclusionPass(raw, c, rect, rt->w, rt->h);
    // a depth target left readable would pass for one whose single sample copy is current
    Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if (!ok) return;
    Transition(ao_[0].Get(), ao_state_[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(ao_[1].Get(), ao_state_[1], D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (!OcclusionPass(across, c, rect, rt->w, rt->h)) return;
    Transition(ao_[1].Get(), ao_state_[1], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(rt->res.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (OcclusionPass(down, c, rect, rt->w, rt->h)) rt->drawn = true;
}

// the frame's own scene only: views drawn to smaller targets are left
bool D3D12Backend::OcclusionFits(const Target* rt, const Target* ds) const {
    if (!rt || !ds || rt->gw != guest_w_ || rt->gh != guest_h_) return false;
    return ds->w == rt->w && ds->h == rt->h && Ms(*ds) == Ms(*rt);
}

bool D3D12Backend::EnsureOcclusionTargets(uint32_t w, uint32_t h) {
    if (ao_[kAoTargets - 1] && ao_w_ == w && ao_h_ == h) return true;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(kAoFormat, w, h);
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    for (int i = 0; i < kAoTargets; i++) {
        if (ao_[i]) Release(std::move(ao_[i]));
        ao_state_[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        HRESULT hr = dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, ao_state_[i], nullptr,
                                                   IID_PPV_ARGS(&ao_[i]));
        if (FAILED(hr)) {
            LOGE("d3d12: no %ux%u ambient occlusion target", w, h);
            return false;
        }
        dev_->CreateRenderTargetView(ao_[i].Get(), nullptr, OcclusionView(i));
    }
    ao_w_ = w;
    ao_h_ = h;
    return true;
}

ID3D12PipelineState* D3D12Backend::EnsureOcclusionPipeline(const AoPass& a) {
    uint64_t bits = uint64_t(a.out) | uint64_t(a.ms_depth) << 32 | uint64_t(a.samples) << 40;
    auto& pso = blit_psos_[Hash(a.entry, std::strlen(a.entry), bits)];
    if (pso) return pso.Get();
    std::string text = std::string(a.ms_depth ? "#define MS 1\n" : "") + kAoHlsl;
    uint64_t h;
    ID3DBlob* vs = Compile(text, "vs", "vs_5_0", h);
    ID3DBlob* ps = Compile(text, a.entry, "ps_5_0", h);
    if (!vs || !ps) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = FullScreenPipelineDesc(vs, ps, a.out);
    if (a.blend) OcclusionBlend(d.BlendState.RenderTarget[0]);
    d.SampleDesc.Count = a.samples;
    if (FAILED(dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso.Get();
}

// t0 the scene's depth, t1 the pass before, t2 the scene's stencil, t3 unused
void D3D12Backend::OcclusionViews(uint32_t slot, const AoPass& a) {
    ID3D12Resource* res[4] = {a.depth, a.src, a.depth, nullptr};
    const DXGI_FORMAT fmt[4] = {DXGI_FORMAT_R24_UNORM_X8_TYPELESS, kAoFormat, DXGI_FORMAT_X24_TYPELESS_G8_UINT,
                                kBgra};
    for (int i = 0; i < 4; i++) {
        bool ms = res[i] && res[i] == a.depth && a.ms_depth;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = res[i] ? fmt[i] : kBgra;
        sv.ViewDimension = ms ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (!ms) sv.Texture2D.MipLevels = 1;
        if (!ms && res[i] && i == 2) sv.Texture2D.PlaneSlice = 1;  // the stencil plane
        dev_->CreateShaderResourceView(res[i], &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
}

// kAoHlsl's opt.zw: the pass's blur step
constexpr int kAoStepX = 14;
constexpr int kAoStepY = 15;

bool D3D12Backend::OcclusionPass(const AoPass& a, float c[kAoConstants], const D3D12_RECT& rect, uint32_t w,
                                 uint32_t h) {
    ID3D12PipelineState* pso = EnsureOcclusionPipeline(a);
    if (!pso) return false;
    uint32_t slot = AllocSrvSlots(4);
    OcclusionViews(slot, a);
    c[kAoStepX] = a.step_x;
    c[kAoStepY] = a.step_y;
    Alloc cb = Upload(256, 256);
    std::memcpy(cb.cpu, c, kAoConstants * sizeof(float));
    D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    cl_->OMSetRenderTargets(1, &a.rtv, FALSE, nullptr);
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &rect);
    DrawFullScreen(pso, cb.gpu, slot);
    return true;
}

ID3D12PipelineState* D3D12Backend::EnsureDepthOfFieldPipeline(const char* entry, DXGI_FORMAT out, uint32_t samples,
                                                           uint32_t input_samples, bool tiled) {
    uint64_t bits = uint64_t(out) | uint64_t(input_samples) << 32 | uint64_t(samples) << 40 | uint64_t(tiled) << 48;
    const uint64_t key = Hash(entry, std::strlen(entry), bits);
    auto found = blit_psos_.find(key);
    if (found != blit_psos_.end()) return found->second.Get();
    const auto text = DofStaticPassSource(entry, input_samples);
    uint64_t hash;
    ID3DBlob* vs = CompileDof(text, tiled ? "viewmodel_dof_tiles_vs" : "vs", "vs_5_0", hash);
    ID3DBlob* ps = CompileDof(text, entry, "ps_5_0", hash);
    if (!vs || !ps) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = FullScreenPipelineDesc(vs, ps, out);
    // Filter passes write color/coverage and depth. Only the final scene pass preserves destination alpha.
    if (!std::strcmp(entry, "depth_of_field") || !std::strcmp(entry, "viewmodel_dof") ||
        !std::strcmp(entry, "ffx_dof_present")) {
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED |
            D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
    } else if (!std::strcmp(entry, "depth_of_field_gather")) {
        d.NumRenderTargets = kDofLayerViews;
        d.RTVFormats[1] = kDofFormats[0];
        d.RTVFormats[2] = kDofFormats[1];
        d.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        d.BlendState.RenderTarget[2].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    } else {
        d.NumRenderTargets = kDofTargets;
        d.RTVFormats[1] = !std::strcmp(entry, "ffx_dof_prepare") ? DXGI_FORMAT_R32_FLOAT : kDofFormats[1];
        d.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    d.SampleDesc.Count = samples;
    auto* ready = dof_pipelines_ ? dof_pipelines_->Request(key ^ 0x444F465354415449ull, d, vs, ps) : nullptr;
    if (ready) blit_psos_[key] = ready;
    return ready; // Pending is not a permanent null cache entry. A later frame can use the ready PSO.
}

bool D3D12Backend::EnsureDepthOfFieldFilter(uint32_t w, uint32_t h) {
    if (dof_filter_[0] && dof_filter_[0]->GetDesc().Width == w && dof_filter_[0]->GetDesc().Height == h) return true;
    uint32_t levels = 1;
    for (uint32_t extent = std::max(w, h); extent > 1 && levels < kDofLevels; extent >>= 1) levels++;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> filter[kDofTargets];
    for (uint32_t target = 0; target < kDofTargets; target++) {
        D3D12_RESOURCE_DESC td = Texture2DDesc(kDofFormats[target], w, h);
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        td.MipLevels = UINT16(levels);
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 nullptr, IID_PPV_ARGS(&filter[target])))) return false;
    }
    for (uint32_t target = 0; target < kDofTargets; target++) {
        if (dof_filter_[target]) Release(std::move(dof_filter_[target]));
        dof_filter_[target] = std::move(filter[target]);
        for (uint32_t mip = 0; mip < levels; mip++) {
            dof_states_[target][mip] = D3D12_RESOURCE_STATE_RENDER_TARGET;
            D3D12_RENDER_TARGET_VIEW_DESC rv{};
            rv.Format = kDofFormats[target];
            rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            rv.Texture2D.MipSlice = mip;
            dev_->CreateRenderTargetView(dof_filter_[target].Get(), &rv, DepthOfFieldView(mip, target));
        }
    }
    dof_levels_ = levels;
    return true;
}

bool D3D12Backend::EnsureDepthOfFieldLayers(uint32_t w, uint32_t h) {
    if (dof_layers_[0] && dof_layers_[0]->GetDesc().Width == w && dof_layers_[0]->GetDesc().Height == h) return true;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> layers[kDofTargets];
    for (uint32_t target = 0; target < kDofTargets; target++) {
        D3D12_RESOURCE_DESC td = Texture2DDesc(kDofFormats[target], w, h);
        td.DepthOrArraySize = target == 0 ? 2 : 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 nullptr, IID_PPV_ARGS(&layers[target])))) return false;
    }
    for (uint32_t target = 0; target < kDofTargets; target++) {
        if (dof_layers_[target]) Release(std::move(dof_layers_[target]));
        dof_layers_[target] = std::move(layers[target]);
        dof_layer_states_[target] = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    for (uint32_t view = 0; view < kDofLayerViews; view++) {
        D3D12_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format = kDofFormats[view < 2 ? 0 : 1];
        if (view < 2) {
            rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            rv.Texture2DArray.FirstArraySlice = view;
            rv.Texture2DArray.ArraySize = 1;
        } else {
            rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        }
        dev_->CreateRenderTargetView(dof_layers_[view < 2 ? 0 : 1].Get(), &rv, DepthOfFieldLayerView(view));
    }
    return true;
}

void D3D12Backend::DepthOfFieldComposeViews(uint32_t slot, ID3D12Resource* scene, DXGI_FORMAT format) {
    ID3D12Resource* resources[4] = {dof_filter_[1].Get(), scene, dof_layers_[0].Get(), dof_layers_[1].Get()};
    DXGI_FORMAT formats[4] = {kDofFormats[1], format, kDofFormats[0], kDofFormats[1]};
    for (uint32_t i = 0; i < 4; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = formats[i];
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (i == 2) {
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            sv.Texture2DArray.MipLevels = 1;
            sv.Texture2DArray.ArraySize = 2;
        } else {
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = 1;
        }
        dev_->CreateShaderResourceView(resources[i], &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
}

void D3D12Backend::SetDepthOfFieldTarget(uint32_t mip, uint32_t w, uint32_t h) {
    D3D12_CPU_DESCRIPTOR_HANDLE views[kDofTargets] = {DepthOfFieldView(mip, 0), DepthOfFieldView(mip, 1)};
    cl_->OMSetRenderTargets(kDofTargets, views, FALSE, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    SetFullViewport(w, h);
}

void D3D12Backend::DepthOfFieldViews(uint32_t slot, ID3D12Resource* depth, bool ms, ID3D12Resource* color,
                                    DXGI_FORMAT color_format, uint32_t first_mip, uint32_t mip_count) {
    ID3D12Resource* res[4] = {depth, color, mip_count ? dof_filter_[0].Get() : nullptr,
        mip_count ? dof_filter_[1].Get() : nullptr};
    DXGI_FORMAT fmt[4] = {depth ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : kBgra, color_format, kDofFormats[0], kDofFormats[1]};
    for (int i = 0; i < 4; i++) {
        bool multisampled = i < 2 && ms;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = fmt[i];
        sv.ViewDimension = multisampled ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (!multisampled) {
            sv.Texture2D.MipLevels = i >= 2 && mip_count ? mip_count : 1;
            sv.Texture2D.MostDetailedMip = i >= 2 ? first_mip : 0;
        }
        dev_->CreateShaderResourceView(res[i], &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
}

void D3D12Backend::DepthOfFieldMipState(uint32_t mip, D3D12_RESOURCE_STATES state) {
    D3D12_RESOURCE_BARRIER barriers[kDofTargets] = {};
    uint32_t count = 0;
    for (uint32_t target = 0; target < kDofTargets; target++) {
        if (dof_states_[target][mip] == state) continue;
        auto& barrier = barriers[count++];
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = dof_filter_[target].Get();
        barrier.Transition.Subresource = mip;
        barrier.Transition.StateBefore = dof_states_[target][mip];
        barrier.Transition.StateAfter = state;
        dof_states_[target][mip] = state;
    }
    if (count) cl_->ResourceBarrier(count, barriers);
}

D3D12Backend::~D3D12Backend() {
    // A CPU worker owns its device/context. Joining is allowed at shutdown, never in a native frame.
    if (dof_cpu_preparation_.valid()) {
        try { (void)dof_cpu_preparation_.get(); } catch (...) {}
    }
    if (dof_pipelines_) dof_pipelines_->Wait();
    // The SDK owns resources referenced by recorded/in-flight commands, including a failed dispatch.
    bool particle_resources = false;
    for (const auto& snapshot : particle_dof_.snapshots) particle_resources |= bool(snapshot.depth);
    if (ffx_dof_.Ready() || particle_resources || particle_dof_.atlas[0] || particle_dof_.shared[0] ||
        particle_dof_.shared[1] || dof_timing_.Available()) Flush();
    if (ffx_dof_.Ready()) ffx_dof_.Reset();
}

bool D3D12Backend::EnsureFidelityFxTargets(uint32_t w, uint32_t h) {
    if (ffx_dof_targets_[0] && ffx_dof_targets_[0]->GetDesc().Width == w &&
        ffx_dof_targets_[0]->GetDesc().Height == h) return true;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> targets[3];
    for (uint32_t i = 0; i < 3; i++) {
        auto td = Texture2DDesc(i == 1 ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT, w, h);
        td.Flags = i == 2 ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        auto state = i == 2 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, state, nullptr,
                                                 IID_PPV_ARGS(&targets[i])))) return false;
    }
    for (uint32_t i = 0; i < 3; i++) {
        if (ffx_dof_targets_[i]) Release(std::move(ffx_dof_targets_[i]));
        ffx_dof_targets_[i] = std::move(targets[i]);
        ffx_dof_states_[i] = i == 2 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (i < 2) dev_->CreateRenderTargetView(ffx_dof_targets_[i].Get(), nullptr, FidelityFxView(i));
    }
    return true;
}

void D3D12Backend::FidelityFxViews(uint32_t slot, ID3D12Resource* first, DXGI_FORMAT first_format, bool first_ms,
                                  ID3D12Resource* second, DXGI_FORMAT second_format) {
    ID3D12Resource* resources[4] = {first, second, nullptr, nullptr};
    DXGI_FORMAT formats[4] = {first_format, second_format, kBgra, kBgra};
    for (uint32_t i = 0; i < 4; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = formats[i];
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        bool ms = i == 0 && first_ms;
        sv.ViewDimension = ms ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
        if (!ms) sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(resources[i], &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
}

bool D3D12Backend::FidelityFxDepthOfField(Target& rt, Target& ds, const float constants[12], const D3D12_RECT& rect,
                                        bool cinematic) {
    float bound = constants[3] * std::max(constants[8], constants[9]);
    if (bound <= 0.25f) return true;
    const uint32_t failure_bit = cinematic ? 2u : 1u;
    if (ffx_dof_failed_ & failure_bit) return false;
    auto fail = [&](const char* reason) {
        ffx_dof_failed_ |= failure_bit;
        LOGR("FidelityFX DOF %s unavailable; using Balanced: %s", cinematic ? "Cinematic" : "Standard", reason);
        return false;
    };
    const uint32_t scale = cinematic ? 2 : 1;
    const uint32_t rings = cinematic ? 8 : 4;
    const auto processing = FidelityFxDofProcessingSize(rt.w, rt.h, scale);
    if (!processing.width || !processing.height) return fail("unsupported processing extent");
    if (!ffx_dof_.Matches(processing.width, processing.height, rings, cinematic)) {
        // Rebuild cooperatively after the next native Present, never inside a scene draw.
        dof_prime_signature_ = 0;
        dof_prepared_ = false;
        return false;
    }
    uint32_t samples = Ms(rt);
    auto* prepare = EnsureDepthOfFieldPipeline("ffx_dof_prepare", DXGI_FORMAT_R16G16B16A16_FLOAT, 1, samples);
    auto* present = EnsureDepthOfFieldPipeline("ffx_dof_present", rt.fmt, samples);
    // Unusual format/sample combinations are prepared asynchronously. Do not permanently disable
    // this profile while a pipeline is pending; the Balanced/native path can cover this frame.
    if (!prepare || !present) return false;
    if (!EnsureFidelityFxTargets(processing.width, processing.height)) return fail("adapter target allocation failed");
    auto prepare_timing = dof_timing_.Measure(DofGpuTiming::Stage::ScenePrepare, cl_.Get());
    ID3D12Resource* source = Feedback(rt);
    if (!source) return fail("scene snapshot allocation failed");
    float c[12];
    std::memcpy(c, constants, sizeof(c));
    // The SDK operates in processing pixels. Scale CoC and its bound together; UI/view distances and native
    // viewport coordinates are unchanged, and the final 2x2 resolve returns the same visible blur radius.
    c[3] *= float(scale);
    c[10] = bound * float(scale);
    c[11] = float(scale);
    Alloc cb = Upload(256, 256);
    std::memcpy(cb.cpu, c, sizeof(c));
    bound_rt_ = bound_ds_ = nullptr;
    Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    for (uint32_t i = 0; i < 2; i++)
        Transition(ffx_dof_targets_[i].Get(), ffx_dof_states_[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
    uint32_t prepare_slot = AllocSrvSlots(4);
    FidelityFxViews(prepare_slot, ds.res.Get(), DXGI_FORMAT_R24_UNORM_X8_TYPELESS, samples > 1, source, rt.fmt);
    D3D12_CPU_DESCRIPTOR_HANDLE views[2] = {FidelityFxView(0), FidelityFxView(1)};
    cl_->OMSetRenderTargets(2, views, FALSE, nullptr);
    // The shader clamps to the logical viewport in c[4..7], extending its edge through all padding.
    SetFullViewport(processing.width, processing.height);
    DrawFullScreen(prepare, cb.gpu, prepare_slot);
    Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    for (uint32_t i = 0; i < 2; i++)
        Transition(ffx_dof_targets_[i].Get(), ffx_dof_states_[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(ffx_dof_targets_[2].Get(), ffx_dof_states_[2], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    prepare_timing.End();
    auto sdk_timing = dof_timing_.Measure(DofGpuTiming::Stage::Sdk, cl_.Get());
    struct SdkTiming {
        DofGpuTiming* timer;
        DofGpuTiming::Scope pass;
    } sdk_pass{&dof_timing_, {}};
    const FidelityFxDofTiming sdk_marks{&sdk_pass,
        [](void* user, ID3D12GraphicsCommandList* list, FidelityFxDofPass pass, bool begin) noexcept {
            auto& current = *static_cast<SdkTiming*>(user);
            current.pass.End();
            if (!begin) return;
            DofGpuTiming::Stage stage;
            switch (pass) {
            case FidelityFxDofPass::DownsampleDepth: stage = DofGpuTiming::Stage::SdkDownsampleDepth; break;
            case FidelityFxDofPass::DownsampleColor: stage = DofGpuTiming::Stage::SdkDownsampleColor; break;
            case FidelityFxDofPass::Dilate: stage = DofGpuTiming::Stage::SdkDilate; break;
            case FidelityFxDofPass::Blur: stage = DofGpuTiming::Stage::SdkBlur; break;
            case FidelityFxDofPass::Composite: stage = DofGpuTiming::Stage::SdkComposite; break;
            default: return;
            }
            current.pass = current.timer->Measure(stage, list);
        }};
    bool dispatched = ffx_dof_.Dispatch(cl_.Get(), ffx_dof_targets_[0].Get(), ffx_dof_targets_[1].Get(),
        ffx_dof_targets_[2].Get(), c[10], dof_timing_.Recording() ? &sdk_marks : nullptr);
    sdk_pass.pass.End();
    sdk_timing.End();
    // The SDK switches descriptor heaps and compute root signatures. Its external textures return to their
    // entry states. Restore the engine's heaps/root and invalidate cached bindings even if dispatch failed.
    Begin();
    if (!dispatched) return fail(ffx_dof_.Failure());
    auto compose_timing = dof_timing_.Measure(DofGpuTiming::Stage::SceneCompose, cl_.Get());
    Transition(ffx_dof_targets_[2].Get(), ffx_dof_states_[2], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(ffx_dof_targets_[0].Get(), ffx_dof_states_[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    uint32_t present_slot = AllocSrvSlots(4);
    FidelityFxViews(present_slot, ffx_dof_targets_[2].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, false,
                    ffx_dof_targets_[0].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    Transition(rt.res.Get(), rt.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    SetBlitTarget(Cpu(rtv_heap_.Get(), rt.view, rtv_inc_), rt.w, rt.h);
    cl_->RSSetScissorRects(1, &rect);
    DrawFullScreen(present, cb.gpu, present_slot);
    rt.drawn = true;
    if (!ffx_dof_reported_) {
        ffx_dof_reported_ = true;
        LOGR("Official FidelityFX SDK 1.1.4 DOF %s active: %ux%u, processing %ux%u (%ux), %ux MSAA, "
             "focus %.2f world units, native radius %.3fpx, near %.2f far %.2f, rings %u, ring merge %s",
             cinematic ? "Cinematic" : "Standard", rt.w, rt.h, processing.width, processing.height, scale,
             samples, c[2], constants[3], c[8], c[9], rings, cinematic ? "off" : "on");
    }
    return true;
}

void D3D12Backend::DepthOfFieldSkipped(uint32_t reason, const char* message) {
    if (dof_skip_reported_ & (1u << reason)) return;
    dof_skip_reported_ |= 1u << reason;
    LOGR("Reload DOF skipped: %s", message);
}

// Queued after world rendering during reloads, when the game defers the weapon until after its effects.
// p: projection m14/m10, eased strength, focus, near/far scales, radius at 1080p, FidelityFX selector.
void D3D12Backend::DepthOfField(const State& s, const float p[4 * kDepthOfFieldRects]) {
    particle_dof_.world_valid = false;
    for (auto& tag : particle_dof_.depth_projection) tag = {};
    if (!dof_prepared_) return;
    for (int i = 0; i < 4 * kDepthOfFieldRects; i++) if (!std::isfinite(p[i])) {
        DepthOfFieldSkipped(0, "non-finite projection, strength or focus");
        return;
    }
    if (p[2] <= 0) return;
    float near_scale = std::clamp(p[4], 0.0f, 1.5f);
    float far_scale = std::clamp(p[5], 0.0f, 1.5f);
    if (near_scale == 0 && far_scale == 0) return;
    if (p[0] >= 0 || p[3] <= 0 || p[6] <= 0) {
        DepthOfFieldSkipped(0, "unsupported perspective projection or focus");
        return;
    }
    Target* rt = FindTarget(s.rt, false);
    Target* ds = FindTarget(s.zs, true);
    if (!OcclusionFits(rt, ds) || !rt->drawn || !ds->drawn) {
        DepthOfFieldSkipped(1, "scene color/depth missing, undrawn, resized or sample counts differ");
        return;
    }
    const Viewport& v = s.viewport;
    if (!std::isfinite(v.min_z) || !std::isfinite(v.max_z) || v.max_z <= v.min_z) {
        DepthOfFieldSkipped(2, "invalid viewport depth range");
        return;
    }
    D3D12_RECT rect = ViewRect(v, *rt);
    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        DepthOfFieldSkipped(2, "empty scene viewport");
        return;
    }
    float radius = std::clamp(p[6], 4.0f, 30.0f) * float(rect.bottom - rect.top) / 1080.0f * std::clamp(p[2], 0.0f, 1.0f);
    float zr = v.max_z - v.min_z;
    const float c[12] = {p[0] * zr, v.min_z + p[1] * zr, std::clamp(p[3], 1.0f, 30.0f), radius,
        float(rect.left), float(rect.top), float(rect.right - rect.left), float(rect.bottom - rect.top),
        near_scale, far_scale, 0, 0};
    for (float value : c) if (!std::isfinite(value)) {
        DepthOfFieldSkipped(0, "non-finite derived shader constants");
        return;
    }
    if (p[7] >= 0.5f && FidelityFxDepthOfField(*rt, *ds, c, rect, p[7] >= 1.5f)) {
        ParticleDofWorld(*rt, *ds, v, c);
        return;
    }
    if (!dof_prepared_) return; // A mismatched SDK context has queued a new preparation episode.
    uint32_t samples = Ms(*rt);
    ID3D12PipelineState* pso = EnsureDepthOfFieldPipeline("depth_of_field", rt->fmt, samples);
    ID3D12PipelineState* gather_pso = EnsureDepthOfFieldPipeline("depth_of_field_gather", kDofFormats[0], 1);
    ID3D12PipelineState* pack_pso = EnsureDepthOfFieldPipeline("depth_of_field_pack", kDofFormats[0], 1, samples);
    ID3D12PipelineState* downsample_pso = EnsureDepthOfFieldPipeline("depth_of_field_downsample", kDofFormats[0], 1);
    if (!pso || !gather_pso || !pack_pso || !downsample_pso) {
        DepthOfFieldSkipped(3, "shader compilation or pipeline creation failed");
        return;
    }
    uint32_t half_w = (rt->w + 1) / 2, half_h = (rt->h + 1) / 2;
    if (!EnsureDepthOfFieldFilter(rt->w, rt->h) || !EnsureDepthOfFieldLayers(half_w, half_h)) {
        DepthOfFieldSkipped(4, "color/depth filter target allocation failed");
        return;
    }
    // Feedback takes a copy even at 1x, so the scene is never read while it is being written. At MSAA it resolves.
    auto prepare_timing = dof_timing_.Measure(DofGpuTiming::Stage::ScenePrepare, cl_.Get());
    ID3D12Resource* source = Feedback(*rt);
    if (!source) {
        DepthOfFieldSkipped(4, "scene color snapshot allocation failed");
        return;
    }
    Alloc cb = Upload(256, 256);
    std::memcpy(cb.cpu, c, sizeof(c));
    bound_rt_ = bound_ds_ = nullptr;
    Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (samples > 1) Transition(rt->res.Get(), rt->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    DepthOfFieldMipState(0, D3D12_RESOURCE_STATE_RENDER_TARGET);
    uint32_t pack_slot = AllocSrvSlots(4);
    DepthOfFieldViews(pack_slot, ds->res.Get(), samples > 1, samples > 1 ? rt->res.Get() : source, rt->fmt);
    // Every scratch pixel is initialized with the viewport edge extended through the filter footprint.
    SetDepthOfFieldTarget(0, rt->w, rt->h);
    DrawFullScreen(pack_pso, cb.gpu, pack_slot);
    DepthOfFieldMipState(0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    // Restore write state, invalidating Feedback's cached depth resolve just as the AO pass does.
    Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    for (uint32_t mip = 1; mip < dof_levels_; mip++) {
        DepthOfFieldMipState(mip, D3D12_RESOURCE_STATE_RENDER_TARGET);
        uint32_t mip_slot = AllocSrvSlots(4);
        // The SRV exposes only the completed source mip; the destination is never simultaneously readable.
        DepthOfFieldViews(mip_slot, nullptr, false, nullptr, kBgra, mip - 1, 1);
        SetDepthOfFieldTarget(mip, std::max(1u, rt->w >> mip), std::max(1u, rt->h >> mip));
        DrawFullScreen(downsample_pso, cb.gpu, mip_slot);
        DepthOfFieldMipState(mip, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    uint32_t gather_slot = AllocSrvSlots(4);
    DepthOfFieldViews(gather_slot, nullptr, false, source, rt->fmt, 0, dof_levels_);
    for (uint32_t target = 0; target < kDofTargets; target++)
        Transition(dof_layers_[target].Get(), dof_layer_states_[target], D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE layer_views[kDofLayerViews] = {
        DepthOfFieldLayerView(0), DepthOfFieldLayerView(1), DepthOfFieldLayerView(2)};
    cl_->OMSetRenderTargets(kDofLayerViews, layer_views, FALSE, nullptr);
    SetFullViewport(half_w, half_h);
    DrawFullScreen(gather_pso, cb.gpu, gather_slot);
    for (uint32_t target = 0; target < kDofTargets; target++)
        Transition(dof_layers_[target].Get(), dof_layer_states_[target], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    prepare_timing.End();
    auto compose_timing = dof_timing_.Measure(DofGpuTiming::Stage::SceneCompose, cl_.Get());
    uint32_t slot = AllocSrvSlots(4);
    DepthOfFieldComposeViews(slot, source, rt->fmt);
    Transition(rt->res.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    SetBlitTarget(Cpu(rtv_heap_.Get(), rt->view, rtv_inc_), rt->w, rt->h);
    cl_->RSSetScissorRects(1, &rect);
    DrawFullScreen(pso, cb.gpu, slot);
    rt->drawn = true;
    if (!dof_reported_) {
        dof_reported_ = true;
        LOGR("Reload DOF active: %ux%u, %ux MSAA, focus %.2f world units, radius %.3fpx, strength %.4f, "
             "z %.6f/(depth-%.6f), near %.2f far %.2f, %ux%u gather 64+32 taps, %u filter mips",
             rt->w, rt->h, samples, c[2], radius, p[2], c[0], c[1], near_scale, far_scale, half_w, half_h, dof_levels_);
    }
    if (cap_) fprintf(cap_, "%04u depth of field rt %08X ds %08X strength %.3f\n", cap_i_++, s.rt.data, s.zs.data, p[2]);
    ParticleDofWorld(*rt, *ds, v, c);
}

void D3D12Backend::ParticleDofWorld(const Target& rt, const Target& ds, const Viewport& viewport, const float c[12]) {
    auto& p = particle_dof_;
    p.world_valid = true;
    p.world_frame = frame_;
    p.world_w = rt.w;
    p.world_h = rt.h;
    p.optics[0] = c[2]; p.optics[1] = c[3]; p.optics[2] = c[8]; p.optics[3] = c[9];
    SetParticleDepthProjection(ds.res.Get(), {c[0], c[1], viewport.min_z, viewport.max_z});
}

void D3D12Backend::InvalidateParticleDepth(ID3D12Resource* resource) {
    ++particle_depth_serial_;
    auto& p = particle_dof_;
    if (!p.world_valid || p.world_frame != frame_) return;
    for (auto& tag : p.depth_projection)
        if (!resource || tag.resource == resource) tag = {};
}

std::array<float, 4> D3D12Backend::ParticleDepthProjection(ID3D12Resource* resource) const {
    const auto& p = particle_dof_;
    if (!p.world_valid || p.world_frame != frame_) return {};
    for (const auto& tag : p.depth_projection)
        if (tag.resource == resource && tag.frame == frame_) return tag.value;
    return {};
}

void D3D12Backend::SetParticleDepthProjection(ID3D12Resource* resource, const std::array<float, 4>& projection) {
    if (!resource || projection[0] >= 0 || projection[3] <= projection[2]) return;
    for (float value : projection) if (!std::isfinite(value)) return;
    auto& p = particle_dof_;
    if (!p.world_valid || p.world_frame != frame_) return;
    // Bounded, frame-local provenance. Overflow keeps the original hardware-depth behavior.
    for (auto& tag : p.depth_projection) if (tag.resource == resource || !tag.resource) {
        tag = {resource, frame_, projection};
        return;
    }
}

void D3D12Backend::ParticleDofSkipped(uint32_t reason, const char* message) {
    auto& p = particle_dof_;
    ++p.rejected;
    if (p.reported & (1u << reason)) return;
    p.reported |= 1u << reason;
    LOGR("Particle DOF native fallback: %s", message);
}

bool D3D12Backend::EnsureParticleDofDepthRange(uint32_t index, uint32_t w, uint32_t h, uint32_t samples) {
    auto& snapshot = particle_dof_.snapshots[index];
    if (!w || !h) return false;
    const DXGI_FORMAT format = ParticleDofRangeFormat(samples);
    if (snapshot.range) {
        const auto desc = snapshot.range->GetDesc();
        if (desc.Width == w && desc.Height == h && desc.MipLevels == 1 && desc.Format == format) return true;
    }
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    auto desc = Texture2DDesc(format, w, h);
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> range;
    if (FAILED(dev_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&range)))) return false;
    // Loading preparation already polled the retirement fence. Unexpected target changes
    // retain the existing safe descriptor replacement rule, never overwrite in-flight RTVs.
    if (snapshot.range_view_used && !dof_resources_quiescent_) Flush();
    if (snapshot.range) Release(std::move(snapshot.range));
    snapshot.range = std::move(range);
    snapshot.range_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    snapshot.source = nullptr;
    dev_->CreateRenderTargetView(snapshot.range.Get(), nullptr, ParticleDofRangeView(index));
    snapshot.range_view_used = true;
    return true;
}

bool D3D12Backend::ParticleDofDepthRange(ID3D12Resource* depth, ID3D12Resource* range,
    D3D12_RESOURCE_STATES& range_state, uint32_t snapshot_index, uint32_t samples, uint32_t w, uint32_t h) {
    const auto source = DofParticleRangeSource(samples, particle_float_bounds_);
    auto* pipeline = EnsurePassPipeline(source.c_str(), "vs", "range_ps", ParticleDofRangeFormat(samples), -1, true);
    if (!pipeline) return false;
    auto range_timing = dof_timing_.Measure(DofGpuTiming::Stage::ParticleRange, cl_.Get());
    const uint32_t slot = AllocSrvSlots(4);
    for (uint32_t i = 0; i < 4; ++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = i == 3 ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : kBgra;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.ViewDimension = i == 3 ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
        if (i != 3) view.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(i == 3 ? depth : nullptr, &view, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    Transition(range, range_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    SetBlitTarget(ParticleDofRangeView(snapshot_index), w, h);
    Alloc constants = Upload(256, 256);
    std::memset(constants.cpu, 0, 256);
    DrawFullScreen(pipeline, constants.gpu, slot);
    Transition(range, range_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ++particle_dof_.range_passes;
    return true;
}

// Begin is only provenance. Deferred D3D8 state is latched by DrawUp after that marker, so validate
// the ACTUAL assembled draw and targets here. Early bucket 3 cannot pass the successful-world epoch.
bool D3D12Backend::PrepareParticleDof(const State& s, Assembled& a, const PixelKey& pk,
                                     DrawShaders& sh, DrawTextures& stages) {
    auto& p = particle_dof_;
    p.visibility_active = false;
    if (!p.marker) return false;
    p.marker = false;  // a marker authorizes exactly one draw
    ++p.batches;
    if (!p.world_valid || p.world_frame != frame_) { ++p.early; return false; }
    if (g_shader_hooks || sh.near_depth || !sh.prog || !s.vs || s.vs->program.size() != kParticleDofNativeProgram.size() ||
        !std::equal(s.vs->program.begin(), s.vs->program.end(), kParticleDofNativeProgram.begin()) ||
        s.vs->decl.size() != kParticleDofNativeDeclaration.size() ||
        !std::equal(s.vs->decl.begin(), s.vs->decl.end(), kParticleDofNativeDeclaration.begin()) ||
        !IsParticleDofNativePixel(pk) || !stages.res[0]) {
        const uint64_t vertex_hash = s.vs ? Hash(s.vs->program.data(), s.vs->program.size() * sizeof(s.vs->program[0])) : 0;
        const uint64_t pixel_hash = Hash(&pk, sizeof(pk));
        const uint64_t signature[] = {vertex_hash, pixel_hash, uint64_t(sh.near_depth), uint64_t(g_shader_hooks != nullptr)};
        if (p.rejected_programs.size() < 8 && p.rejected_programs.insert(Hash(signature, sizeof(signature))).second)
            LOGR("Particle DOF shader fallback: VS %016llX (%zu words), PS %016llX, flat %u, alpha-kill %X, near-depth %u, hooks %u, texture %u",
                 (unsigned long long)vertex_hash, s.vs ? s.vs->program.size() : 0,
                 (unsigned long long)pixel_hash, unsigned(pk.flat), unsigned(pk.alphakill),
                 unsigned(sh.near_depth), unsigned(g_shader_hooks != nullptr), unsigned(stages.res[0] != nullptr));
        ParticleDofSkipped(0, "unrecognized native VS/material/texture"); return false;
    }
    if (a.regs != 0x209 || a.decode || !a.vertices.empty() || a.buffer_count != 1 ||
        a.base_vertex || a.strip || a.topology != Topology::Triangles || a.vertex_count == 0 ||
        a.vertex_count > 4096 || a.vertex_count % 4 || a.buffers[0].stride != sizeof(NativeParticleVertex) ||
        !a.buffers[0].src || a.buffers[0].bytes < a.vertex_count * sizeof(NativeParticleVertex)) {
        ParticleDofSkipped(1, "unsupported native vertex assembly"); return false;
    }
    if (a.index16 || a.index_count != (a.vertex_count / 4) * 6 || a.indices.size() != a.index_count) {
        ParticleDofSkipped(1, "unsupported native quad indices"); return false;
    }
    constexpr uint32_t quad_indices[6] = {0, 1, 2, 0, 2, 3};
    for (uint32_t i = 0; i < a.index_count; ++i) if (a.indices[i] != (i / 6) * 4 + quad_indices[i % 6]) {
        ParticleDofSkipped(1, "unsupported native quad order"); return false;
    }
    for (int r : {0, 3, 9}) {
        const auto& e = a.elements[r];
        const uint32_t offset = r == 0 ? 0 : r == 3 ? 16 : 20;
        const DXGI_FORMAT format = r == 0 ? DXGI_FORMAT_R32G32B32A32_FLOAT :
            r == 3 ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R32G32_FLOAT;
        if (e.buffer || e.offset != offset || e.format != format) {
            ParticleDofSkipped(1, "unsupported native input layout"); return false;
        }
    }
    Target* rt = bound_rt_;
    Target* ds = bound_ds_;
    PsoKey k;
    BuildPipelineKey(s, a, 0, 0, k);
    if (!rt || !ds || !ds->drawn || rt->w != ds->w || rt->h != ds->h || Ms(*rt) != Ms(*ds) ||
        ds->res->GetDesc().Format != DXGI_FORMAT_R24G8_TYPELESS || !k.zen || k.zwrite || k.sten ||
        (Cmp(k.zfunc) != D3D12_COMPARISON_FUNC_LESS && Cmp(k.zfunc) != D3D12_COMPARISON_FUNC_LESS_EQUAL) ||
        k.zbias || k.zslope != 0 || !k.blend || k.src != 0x302 || (k.dst != 0x303 && k.dst != 1) ||
        (BlendOp(k.op) != D3D12_BLEND_OP_ADD &&
         !(k.dst == 1 && BlendOp(k.op) == D3D12_BLEND_OP_REV_SUBTRACT))) {
        const uint32_t values[] = {k.zen, k.zwrite, k.zfunc, k.sten, k.blend, k.src, k.dst, k.op};
        if (p.rejected_states.size() < 8 && p.rejected_states.insert(Hash(values, sizeof(values))).second)
            LOGR("Particle DOF state fallback: depth %u/write %u/func %X stencil %u, blend %u src %X dst %X op %X",
                 k.zen, k.zwrite, k.zfunc, k.sten, k.blend, k.src, k.dst, k.op);
        ParticleDofSkipped(2, "unsupported depth/alpha-blend state or unmatched depth surface"); return false;
    }
    const float sx = float(rt->w) / p.world_w, sy = float(rt->h) / p.world_h;
    // Native small targets may round an eighth-resolution width by one pixel; reject real anisotropy.
    if (std::abs(sx - sy) * std::max(p.world_w, p.world_h) > 1.0f) {
        ParticleDofSkipped(3, "anisotropic particle target"); return false;
    }
    GuestExtent guest{};
    SurfaceSize(s.rt, guest.w, guest.h);
    if (!guest.w || !guest.h || !std::isfinite(s.zscale) || s.zscale <= 0) {
        ParticleDofSkipped(3, "invalid particle viewport/guest size"); return false;
    }
    D3D12_RECT rect = ViewRect(s.viewport, *rt);
    const float sc = float(rt->w) / guest.w;
    if (s.scissor_count) {
        rect.left = std::max(rect.left, LONG(s.scissors[0] * sc));
        rect.top = std::max(rect.top, LONG(s.scissors[1] * sc));
        rect.right = std::min(rect.right, LONG(s.scissors[2] * sc));
        rect.bottom = std::min(rect.bottom, LONG(s.scissors[3] * sc));
    }
    if (rect.right <= rect.left || rect.bottom <= rect.top) return false;
    ParticleDofGeometryParams params{};
    std::copy(std::begin(p.optics), std::end(p.optics), params.globals.optics);
    params.globals.optics[1] *= sy;
    params.globals.viewport[0] = float(rect.left); params.globals.viewport[1] = float(rect.top);
    params.globals.viewport[2] = float(rect.right - rect.left); params.globals.viewport[3] = float(rect.bottom - rect.top);
    params.globals.target[0] = float(rt->w); params.globals.target[1] = float(rt->h);
    params.globals.target[2] = float(a.vertex_count / 4);
    params.globals.target[3] = Cmp(k.zfunc) == D3D12_COMPARISON_FUNC_LESS_EQUAL ? 1.0f : 0.0f;
    const auto vertices = std::span(reinterpret_cast<const NativeParticleVertex*>(a.buffers[0].src), a.vertex_count);
    // Unsupported/focused quads share this PSO. Pixel-center depth reproduces their native MSAA
    // rejection only when each whole quad has constant native Z/W; otherwise keep hardware depth.
    bool constant_depth = true;
    for (size_t q = 0; q < vertices.size(); q += 4) {
        const auto& first = vertices[q];
        const double projected_w = double(first.position[3]) * s.vsc[98][3] + s.vsc[97][3];
        if (!(first.position[2] > 0) || !std::isfinite(projected_w) || projected_w <= 0 ||
            projected_w > std::numeric_limits<float>::max()) constant_depth = false;
        for (size_t i = 0; i < 4; ++i) {
            const auto& vertex = vertices[q + i];
            for (float value : vertex.position) if (!std::isfinite(value)) constant_depth = false;
            if (vertex.position[2] != first.position[2] || vertex.position[3] != first.position[3])
                constant_depth = false;
        }
    }
    const auto projection = ParticleDepthProjection(ds->res.Get());
    if (constant_depth && projection[0] < 0 && std::isfinite(s.viewport.min_z) && std::isfinite(s.viewport.max_z) &&
        std::abs(projection[2] - s.viewport.min_z) <= 1e-6f &&
        std::abs(projection[3] - s.viewport.max_z) <= 1e-6f) {
        params.globals.projection[0] = projection[0];
        params.globals.projection[1] = projection[1];
        params.globals.projection[2] = 1;
    }
    std::memcpy(params.c97, s.vsc[97].data(), sizeof(params.c97));
    std::memcpy(params.c98, s.vsc[98].data(), sizeof(params.c98));
    params.guest_size[0] = float(guest.w); params.guest_size[1] = float(guest.h);
    ParticleDofGeometryStats stats{};
    ParticleDofGeometryRejections rejections{};
    bool active = BuildParticleDofGeometry(vertices, params, p.vertices, p.quads, stats, &rejections);
    for (size_t i = 0; i < rejections.size(); ++i) p.geometry_rejections[i] += rejections[i];
    p.focused += stats.focused; p.invalid += stats.invalid;
    if (stats.invalid && !(p.reported & (1u << 4))) {
        p.reported |= 1u << 4;
        LOGR("Particle DOF: unsupported/non-finite quads stay native; counts are reported periodically");
    }
    if (!active) return false;
    const auto desc = ds->res->GetDesc();
    const uint32_t samples = Ms(*ds);
    const auto compatible = [&](const ParticleDof::Snapshot& candidate) {
        if (!candidate.depth) return false;
        const auto old = candidate.depth->GetDesc();
        return old.Width == desc.Width && old.Height == desc.Height && old.Format == desc.Format &&
            old.SampleDesc.Count == desc.SampleDesc.Count && old.SampleDesc.Quality == desc.SampleDesc.Quality;
    };
    ParticleDof::Snapshot* snapshot = nullptr;
    for (auto& candidate : p.snapshots) {
        if (candidate.source == ds->res.Get() && compatible(candidate)) {
            snapshot = &candidate; break;
        }
    }
    if (!snapshot) {
        // Startup reserves one main-size slot and two small particle-target slots. Prefer an unused
        // compatible allocation so the first bucket order cannot immediately evict that reservation.
        for (auto& candidate : p.snapshots) if (!candidate.source && compatible(candidate)) {
            snapshot = &candidate; break;
        }
    }
    if (!snapshot) {
        snapshot = &*std::min_element(std::begin(p.snapshots), std::end(p.snapshots),
            [](const auto& a, const auto& b) { return a.used < b.used; });
    }
    if (!compatible(*snapshot)) {
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        ComPtr<ID3D12Resource> depth;
        if (FAILED(dev_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&depth)))) {
            ParticleDofSkipped(5, "opaque depth snapshot allocation failed"); return false;
        }
        if (snapshot->depth) Release(std::move(snapshot->depth));
        snapshot->depth = std::move(depth);
        snapshot->state = D3D12_RESOURCE_STATE_COPY_DEST;
        snapshot->source = nullptr;
    }
    const uint32_t snapshot_index = uint32_t(snapshot - p.snapshots);
    if (samples > 1 && !EnsureParticleDofDepthRange(snapshot_index, rt->w, rt->h, samples)) {
        ParticleDofSkipped(7, "MSAA depth range allocation failed"); return false;
    }
    snapshot->used = ++p.uses;
    if (snapshot->source != ds->res.Get() || snapshot->serial != particle_depth_serial_ || snapshot->frame != frame_) {
        // Actual depth mutations invalidate the cache; merely binding the same read-only particle DS does not.
        snapshot->source = nullptr;
        auto copy_timing = dof_timing_.Measure(DofGpuTiming::Stage::ParticleDepthCopy, cl_.Get());
        Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(snapshot->depth.Get(), snapshot->state, D3D12_RESOURCE_STATE_COPY_DEST);
        cl_->CopyResource(snapshot->depth.Get(), ds->res.Get());
        ++p.copies;
        p.copy_bytes += uint64_t(rt->w) * rt->h * samples * 4;
        Transition(snapshot->depth.Get(), snapshot->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        copy_timing.End();
        if (samples > 1) {
            const bool range_ready = ParticleDofDepthRange(snapshot->depth.Get(), snapshot->range.Get(),
                snapshot->range_state, snapshot_index, samples, rt->w, rt->h);
            // The prepass replaced graphics bindings; restore the native targets even on a PSO failure.
            D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), rt->view, rtv_inc_);
            D3D12_CPU_DESCRIPTOR_HANDLE dh = Cpu(dsv_heap_.Get(), ds->view, dsv_inc_);
            cl_->OMSetRenderTargets(1, &rh, FALSE, &dh);
            bound_rt_ = rt; bound_ds_ = ds;
            if (!range_ready) { ParticleDofSkipped(7, "MSAA depth range pipeline failed"); return false; }
        }
        snapshot->source = ds->res.Get();
        snapshot->serial = particle_depth_serial_;
        snapshot->frame = frame_;
    } else ++p.reused;
    if (samples > 1) {
        stages.res[2] = snapshot->range.Get();
        stages.sv[2] = {};
        stages.sv[2].Format = ParticleDofRangeFormat(samples);
        stages.sv[2].Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        stages.sv[2].ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        stages.sv[2].Texture2D.MipLevels = snapshot->range->GetDesc().MipLevels;
        params.globals.projection[3] = float(stages.sv[2].Texture2D.MipLevels);
    }
    stages.res[3] = snapshot->depth.Get();
    stages.sv[3] = {};
    stages.sv[3].Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    stages.sv[3].Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    stages.sv[3].ViewDimension = Ms(*ds) > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
    if (Ms(*ds) == 1) stages.sv[3].Texture2D.MipLevels = 1;
    p.globals = params.globals;
    p.params = params;
    a.buffers[0].src = reinterpret_cast<const uint8_t*>(p.vertices.data());
    sh.particle_samples = samples;
    sh.particle_blend = k.dst == 1 ? 1 : 0;
    sh.particle_alpha_function = EffectiveAlphaFunction(s);
    sh.particle_coverage = params.globals.projection[2] > 0;
    if (sh.particle_coverage) ++p.coverage_batches;
    else if (!constant_depth) ++p.coverage_varying_depth;
    else ++p.coverage_unknown;
    const uint32_t coverage_report = sh.particle_coverage ? 10 : 11;
    if (!(p.reported & (1u << coverage_report))) {
        p.reported |= 1u << coverage_report;
        LOGR("Particle DOF destination coverage: %s, target %ux%u, %ux MSAA",
             sh.particle_coverage ? "blur-aware with verified world depth" : "native depth (unverified encoding)",
             rt->w, rt->h, samples);
    }
    p.draw_active = stats.active;
    return true;
}

bool D3D12Backend::EnsureWeaponDofTargets(uint32_t w, uint32_t h) {
    auto& weapon = weapon_dof_;
    if (weapon.packed[0] && weapon.packed[0]->GetDesc().Width == w && weapon.packed[0]->GetDesc().Height == h) return true;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> targets[kDofTargets];
    for (uint32_t i = 0; i < kDofTargets; i++) {
        D3D12_RESOURCE_DESC td = Texture2DDesc(kDofFormats[i], w, h);
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 nullptr, IID_PPV_ARGS(&targets[i])))) return false;
    }
    if (weapon.packed[0] && !dof_resources_quiescent_) Flush(); // fixed RTVs can still be in flight outside preparation
    for (uint32_t i = 0; i < kDofTargets; i++) {
        if (weapon.packed[i]) Release(std::move(weapon.packed[i]));
        weapon.packed[i] = std::move(targets[i]);
        weapon.packed_state[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        dev_->CreateRenderTargetView(weapon.packed[i].Get(), nullptr, WeaponDofView(i));
    }
    return true;
}

bool D3D12Backend::EnsureWeaponDofTiles(uint32_t w, uint32_t h) {
    auto& weapon = weapon_dof_;
    w = (w + kViewmodelDofTileSize - 1) / kViewmodelDofTileSize;
    h = (h + kViewmodelDofTileSize - 1) / kViewmodelDofTileSize;
    if (weapon.tiles && weapon.tiles->GetDesc().Width == w && weapon.tiles->GetDesc().Height == h) return true;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    auto desc = Texture2DDesc(DXGI_FORMAT_R8_UNORM, w, h);
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> tiles;
    if (FAILED(dev_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&tiles)))) return false;
    if (weapon.tiles && !dof_resources_quiescent_) Flush();
    if (weapon.tiles) Release(std::move(weapon.tiles));
    weapon.tiles = std::move(tiles);
    weapon.tiles_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    dev_->CreateRenderTargetView(weapon.tiles.Get(), nullptr, WeaponDofTileView());
    return true;
}

void D3D12Backend::WeaponDofViews(uint32_t slot, ID3D12Resource* a, ID3D12Resource* b, ID3D12Resource* color,
                                DXGI_FORMAT format, bool pack, bool ms, ID3D12Resource* tiles) {
    ID3D12Resource* resources[4] = {a, b, color, tiles};
    DXGI_FORMAT formats[4] = {pack ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : kDofFormats[0],
        pack ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : kDofFormats[1], format, DXGI_FORMAT_R8_UNORM};
    for (uint32_t i = 0; i < 4; i++) {
        bool multisampled = pack && ms && i < 3;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = formats[i];
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.ViewDimension = multisampled ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
        if (!multisampled) sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(resources[i], &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
}

void D3D12Backend::WeaponDofSkipped(uint32_t reason, const char* message) {
    if (weapon_dof_.skipped & (1u << reason)) return;
    weapon_dof_.skipped |= 1u << reason;
    LOGR("Weapon DOF skipped: %s", message);
}

void D3D12Backend::WeaponDepthOfField(const State& s, const float p[4 * kWeaponDofRects], bool begin) {
    auto& weapon = weapon_dof_;
    bool pending = weapon.pending;
    weapon.pending = false;  // An End consumes a snapshot exactly once, including mismatches and failures.
    if (!dof_prepared_) return;
    if (!begin && !pending) return;
    // The serial is an opaque bit pattern; some valid serials encode NaN/Inf as a float.
    uint32_t serial = std::bit_cast<uint32_t>(p[3]);
    if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]) || p[0] >= 0) {
        WeaponDofSkipped(0, "invalid projection or amount");
        return;
    }
    if (!std::isfinite(p[4]) || !std::isfinite(p[5]) || !std::isfinite(p[6]) || !std::isfinite(p[7]) ||
        p[4] <= 0 || p[5] <= 0 || p[5] > p[4]) {
        WeaponDofSkipped(4, "invalid weapon focus interval");
        return;
    }
    if (p[2] <= 0) return;
    Target* rt = FindTarget(s.rt, false);
    Target* ds = FindTarget(s.zs, true);
    if (!OcclusionFits(rt, ds) || !rt->drawn || !ds->drawn) {
        WeaponDofSkipped(1, "scene targets unavailable or incompatible");
        return;
    }
    const Viewport& v = s.viewport;
    if (!std::isfinite(v.min_z) || !std::isfinite(v.max_z) || v.max_z <= v.min_z) return;
    D3D12_RECT rect = ViewRect(v, *rt);
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    if (begin) {
        const auto desc = ds->res->GetDesc();
        if (!weapon.before || weapon.before->GetDesc().Width != desc.Width || weapon.before->GetDesc().Height != desc.Height ||
            weapon.before->GetDesc().Format != desc.Format || weapon.before->GetDesc().SampleDesc.Count != desc.SampleDesc.Count ||
            weapon.before->GetDesc().SampleDesc.Quality != desc.SampleDesc.Quality) {
            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
            ComPtr<ID3D12Resource> before;
            if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                     nullptr, IID_PPV_ARGS(&before)))) {
                WeaponDofSkipped(2, "depth snapshot allocation failed");
                return;
            }
            if (weapon.before) Release(std::move(weapon.before));
            weapon.before = std::move(before);
            weapon.before_state = D3D12_RESOURCE_STATE_COPY_DEST;
        }
        auto copy_timing = dof_timing_.Measure(DofGpuTiming::Stage::WeaponDepthCopy, cl_.Get());
        Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(weapon.before.Get(), weapon.before_state, D3D12_RESOURCE_STATE_COPY_DEST);
        cl_->CopyResource(weapon.before.Get(), ds->res.Get());
        Transition(weapon.before.Get(), weapon.before_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        copy_timing.End();
        weapon.rt = rt->res.Get();
        weapon.ds = ds->res.Get();
        weapon.viewport = v;
        std::memcpy(weapon.parameters, p, sizeof(weapon.parameters));
        weapon.serial = serial;
        weapon.frame = frame_;
        // Preparation owns allocations/fixed RTV replacement. A surprise size
        // change during a weapon scope must fall back, never allocate or Flush.
        const auto tile_desc = weapon.tiles ? weapon.tiles->GetDesc() : D3D12_RESOURCE_DESC{};
        weapon.encoding.Begin(particle_depth_serial_);
        weapon.region.Begin(particle_depth_serial_, weapon.conservative_supported && weapon.coverage_ps &&
            WeaponDofViewportSupported(0, 0, float(rt->w), float(rt->h)) &&
            weapon.tiles && tile_desc.Width == (rt->w + kViewmodelDofTileSize - 1) / kViewmodelDofTileSize &&
            tile_desc.Height == (rt->h + kViewmodelDofTileSize - 1) / kViewmodelDofTileSize);
        if (weapon.region.valid) {
            auto mask_timing = dof_timing_.Measure(DofGpuTiming::Stage::WeaponClassify, cl_.Get());
            Transition(weapon.tiles.Get(), weapon.tiles_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
            const float zero[4]{};
            cl_->ClearRenderTargetView(WeaponDofTileView(), zero, 0, nullptr);
        }
        weapon.pending = true;
        return;
    }
    const Viewport& saved = weapon.viewport;
    if (weapon.serial != serial || weapon.frame != frame_ || weapon.rt != rt->res.Get() || weapon.ds != ds->res.Get() ||
        saved.x != v.x || saved.y != v.y || saved.w != v.w || saved.h != v.h || saved.min_z != v.min_z || saved.max_z != v.max_z ||
        std::memcmp(weapon.parameters, p, sizeof(weapon.parameters))) {
        WeaponDofSkipped(3, "unmatched snapshot serial, frame, viewport or targets");
        return;
    }
    uint32_t samples = Ms(*rt);
    if (!weapon.encoding.Complete(particle_depth_serial_)) {
        WeaponDofSkipped(5, "missing, mixed or unmatched weapon depth encoding");
        return;
    }
    auto* pack = EnsureDepthOfFieldPipeline("viewmodel_dof_pack", kDofFormats[0], 1, samples);
    auto* compose = EnsureDepthOfFieldPipeline("viewmodel_dof", rt->fmt, samples);
    if (!pack || !compose || !EnsureWeaponDofTargets(rt->w, rt->h)) {
        WeaponDofSkipped(2, "pipeline or packed target creation failed");
        return;
    }
    auto* sparse_pack = EnsureDepthOfFieldPipeline("viewmodel_dof_pack", kDofFormats[0], 1, samples, true);
    auto* sparse_compose = samples == 1 ? EnsureDepthOfFieldPipeline("viewmodel_dof", rt->fmt, 1, 1, true) : nullptr;
    const bool sparse = weapon.region.Complete(particle_depth_serial_) &&
        WeaponDofViewportSupported(0, 0, float(rt->w), float(rt->h)) &&
        weapon.tiles && sparse_pack && (samples > 1 || sparse_compose);
    auto scene_timing = dof_timing_.Measure(DofGpuTiming::Stage::WeaponSceneCopy, cl_.Get());
    ID3D12Resource* scene = Feedback(*rt);
    scene_timing.End();
    if (!scene) {
        WeaponDofSkipped(2, "scene snapshot allocation failed");
        return;
    }
    float zr = v.max_z - v.min_z;
    const float c[16] = {p[0] * zr, v.min_z + p[1] * zr, std::clamp(p[2], 0.0f, 1.0f),
        float(rect.bottom - rect.top) / 1080.0f, float(rect.left), float(rect.top),
        float(rect.right - rect.left), float(rect.bottom - rect.top), p[4], p[5], float(rt->w), float(rt->h),
        weapon.encoding.plane, 0, 0, 0};
    for (float value : c) if (!std::isfinite(value)) return;
    Alloc cb = Upload(256, 256);
    std::memcpy(cb.cpu, c, sizeof(c));
    bound_rt_ = bound_ds_ = nullptr;
    Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(rt->res.Get(), rt->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (sparse) {
        Transition(weapon.tiles.Get(), weapon.tiles_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    for (uint32_t i = 0; i < kDofTargets; i++)
        Transition(weapon.packed[i].Get(), weapon.packed_state[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
    uint32_t pack_slot = AllocSrvSlots(4);
    WeaponDofViews(pack_slot, ds->res.Get(), weapon.before.Get(), rt->res.Get(), rt->fmt, true, samples > 1,
                   sparse ? weapon.tiles.Get() : nullptr);
    D3D12_CPU_DESCRIPTOR_HANDLE views[kDofTargets] = {WeaponDofView(0), WeaponDofView(1)};
    cl_->OMSetRenderTargets(kDofTargets, views, FALSE, nullptr);
    SetFullViewport(rt->w, rt->h);
    const auto draw_tiles = [&](ID3D12PipelineState* pipeline, uint32_t slot) {
        cl_->SetPipelineState(pipeline);
        cl_->SetGraphicsRootConstantBufferView(0, cb.gpu);
        cl_->SetGraphicsRootConstantBufferView(1, cb.gpu);
        cl_->SetGraphicsRootDescriptorTable(2, Gpu(srv_heap_.Get(), slot, srv_inc_));
        cl_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const auto desc = weapon.tiles->GetDesc();
        cl_->DrawInstanced(6, uint32_t(desc.Width) * desc.Height, 0, 0);
    };
    auto pack_timing = dof_timing_.Measure(DofGpuTiming::Stage::WeaponPack, cl_.Get());
    if (sparse) {
        const float clear[4]{};
        for (auto view : views) cl_->ClearRenderTargetView(view, clear, 0, nullptr);
        draw_tiles(sparse_pack, pack_slot);
    } else DrawFullScreen(pack, cb.gpu, pack_slot);
    for (uint32_t i = 0; i < kDofTargets; i++)
        Transition(weapon.packed[i].Get(), weapon.packed_state[i], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    pack_timing.End();
    auto compose_timing = dof_timing_.Measure(DofGpuTiming::Stage::WeaponCompose, cl_.Get());
    Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    Transition(rt->res.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    uint32_t compose_slot = AllocSrvSlots(4);
    WeaponDofViews(compose_slot, weapon.packed[0].Get(), weapon.packed[1].Get(), scene, rt->fmt, false, false,
                   sparse ? weapon.tiles.Get() : nullptr);
    SetBlitTarget(Cpu(rtv_heap_.Get(), rt->view, rtv_inc_), rt->w, rt->h);
    cl_->RSSetScissorRects(1, &rect);
    // Keep the original full-screen MSAA writeback: later blending can observe individual samples.
    if (sparse && samples == 1) draw_tiles(sparse_compose, compose_slot);
    else DrawFullScreen(compose, cb.gpu, compose_slot);
    rt->drawn = true;
    if (!weapon.reported) {
        weapon.reported = true;
        LOGR("Weapon DOF active: %ux%u, %ux MSAA, amount %.3f, sharp depth %.2f, transition %.2f, paired depth snapshot",
             rt->w, rt->h, samples, c[2], c[8], c[9]);
    }
}

// the pixels the CPU changed since the last Sync, opaque, the rest 0; t.read takes them
std::vector<uint8_t> LatchCpuChanges(Target& t) {
    std::vector<uint8_t> px(t.read.size(), 0);
    uint32_t guest_pitch = GuestPitch(t.gw);
    const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        for (uint32_t x = 0; x < t.gw; x++) {
            const uint8_t* c = guest + size_t(y) * guest_pitch + x * 4;
            size_t i = (size_t(y) * t.gw + x) * 4;
            if (!std::memcmp(c, &t.read[i], 4)) continue;
            std::memcpy(&t.read[i], c, 4);
            std::memcpy(&px[i], c, 3);
            px[i + 3] = 255;
        }
    }
    return px;
}

// BGRA pixels into a new w x h texture, left as a shader resource
ComPtr<ID3D12Resource> D3D12Backend::CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w,
                                                        uint32_t h) {
    ComPtr<ID3D12Resource> tex;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, w, h);
    dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                  IID_PPV_ARGS(&tex));
    uint32_t row = UploadPitch(w * 4);
    Alloc a = Upload(uint64_t(row) * h, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    for (uint32_t y = 0; y < h; y++) std::memcpy(a.cpu + size_t(y) * row, &px[size_t(y) * pitch], w * 4);
    D3D12_TEXTURE_COPY_LOCATION dst{tex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION src{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint.Offset = a.off;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, w, h, 1, row};
    cl_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COPY_DEST;
    Transition(tex.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return tex;
}

// CPU pixels of a color surface into its host target, scaled
void D3D12Backend::UploadTarget(Target& t, const Surface& s) {
    if (t.depth) InvalidateParticleDepth(t.res.Get());
    g_perf.rtups++;
    TextureDesc d;
    if (!Describe(s.header, d) || Format(d.format).depth) return;
    std::vector<uint8_t> px;
    uint32_t pitch;
    // after a Sync only the pixels the CPU changed go over the picture, which stays at render scale
    bool over = !t.drawn && d.width == t.gw && d.height == t.gh && t.read.size() == size_t(t.gw) * t.gh * 4;
    if (over) {
        LOG_FIRST(4, "d3d12: CPU pixels drawn over target %08X", t.data);
        pitch = t.gw * 4;
        px = LatchCpuChanges(t);
    } else {
        DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    }
    ComPtr<ID3D12Resource> tex = CreatePixelTexture(px, pitch, d.width, d.height);
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    SetBlitTarget(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), t.w, t.h);
    float uv[4] = {0, 0, 1, 1};
    float mul = 1 / t.range;
    Blit(tex.Get(), kBgra, uv, {.alpha = true, .mul = mul, .rtv = t.fmt, .over = over, .samples = Ms(t)});
    CurrentFrame().garbage.push_back(tex);
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
}

// samplers_ key of the full screen passes' samplers, apart from the stage samplers' hashed keys
constexpr uint64_t kBlitSamplerKey = 0xB117;

// four clamp samplers: linear at slot 0, point at slots 1 to 3
uint32_t D3D12Backend::EnsureBlitSamplers() {
    uint64_t key = kBlitSamplerKey;
    auto it = samplers_.find(key);
    uint32_t si;
    if (it == samplers_.end()) {
        if (smp_used_ + 4 > kSamplerCount) {
            samplers_.clear();
            smp_used_ = 0;
        }
        si = smp_used_;
        smp_used_ += 4;
        for (int i = 0; i < 4; i++) {
            D3D12_SAMPLER_DESC sd{};
            sd.Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sd.MaxLOD = D3D12_FLOAT32_MAX;
            dev_->CreateSampler(&sd, Cpu(smp_heap_.Get(), si + i, smp_inc_));
        }
        samplers_[key] = si;
    } else {
        si = it->second;
    }
    return si;
}

// the full screen triangle: cb at b0 and b1, views at srv_slot; without a sampler slot EnsureBlitSamplers gives it
void D3D12Backend::DrawFullScreen(ID3D12PipelineState* pso, D3D12_GPU_VIRTUAL_ADDRESS cb, uint32_t srv_slot,
                                  std::optional<uint32_t> sampler_slot) {
    cl_->SetPipelineState(pso);
    cl_->SetGraphicsRootConstantBufferView(0, cb);
    cl_->SetGraphicsRootConstantBufferView(1, cb);
    cl_->SetGraphicsRootDescriptorTable(2, Gpu(srv_heap_.Get(), srv_slot, srv_inc_));
    // without a slot the passes take the blit samplers, looked up at this point as they always were
    uint32_t si = sampler_slot ? *sampler_slot : EnsureBlitSamplers();
    cl_->SetGraphicsRootDescriptorTable(3, Gpu(smp_heap_.Get(), si, smp_inc_));
    cl_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl_->DrawInstanced(3, 1, 0, 0);
}

void D3D12Backend::Blit(ID3D12Resource* src, DXGI_FORMAT fmt, const float uvrect[4], const BlitOptions& o) {
    uint32_t slot = AllocSrvSlots(2);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = fmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(src, &sv, Cpu(srv_heap_.Get(), slot, srv_inc_));
    sv.Format = kBgra;
    dev_->CreateShaderResourceView(lut_.Get(), &sv, Cpu(srv_heap_.Get(), slot + 1, srv_inc_));
    uint32_t si = EnsureBlitSamplers();
    Alloc cb = Upload(256, 256);
    float c[8] = {uvrect[0], uvrect[1], uvrect[2], uvrect[3],
                  o.gamma ? 1.0f : 0.0f, o.alpha ? 1.0f : 0.0f, float(o.taps), o.mul};
    std::memcpy(cb.cpu, c, sizeof(c));
    DrawFullScreen(EnsureBlitPipeline(o.rtv, o.over, o.samples), cb.gpu, slot, si);
}

ID3DBlob* D3D12Backend::Compile(const std::string& src, const char* entry, const char* profile, uint64_t& hash) {
    // one source can hold several entry points
    constexpr UINT flags1 = D3DCOMPILE_OPTIMIZATION_LEVEL1, flags2 = 0;
    constexpr uint64_t compiler_flags = uint64_t(flags1) | (uint64_t(flags2) << 32);
    uint64_t profile_seed = Hash(profile, std::strlen(profile), Hash(&compiler_flags, sizeof(compiler_flags)));
    uint64_t entry_seed = Hash(entry, std::strlen(entry), profile_seed);
    hash = Hash(src.data(), src.size(), entry_seed);
    auto it = blobs_.find(hash);
    if (it != blobs_.end()) return it->second.Get();
    // Cache initialization is optional too: path/identity allocation must not prevent compilation.
    static const auto disk_cache = []() -> std::optional<ShaderCache> {
        try { return std::optional<ShaderCache>{std::in_place}; }
        catch (...) { return {}; }
    }();
    static uint64_t disk_hits = 0, disk_misses = 0;
    ComPtr<ID3DBlob> code = disk_cache ? disk_cache->Load(src, entry, profile, compiler_flags) : nullptr;
    if (code) {
        if (++disk_hits == 1)
            LOGR("d3d12: shader bytecode cache hit (%llu hits, %llu misses this process)",
                 (unsigned long long)disk_hits, (unsigned long long)disk_misses);
        blobs_[hash] = code;
        return code.Get();
    }
    if (++disk_misses == 1)
        LOGR("d3d12: shader bytecode cache miss (%llu hits, %llu misses this process)",
             (unsigned long long)disk_hits, (unsigned long long)disk_misses);
    ComPtr<ID3DBlob> err;
    PerfScope perf_compile{g_perf.compile};
    g_perf.shaders++;
    HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, entry, profile,
                            flags1, flags2, &code, &err);
    const double compile_ms = PerfMs(perf_compile.t);
    if (compile_ms >= 250.0)
        LOGR("d3d12: cold shader compile %s %s %016llX took %.1f ms", entry, profile,
             (unsigned long long)hash, compile_ms);
    if (FAILED(hr)) {
        LOGE("d3d12: %s compile failed: %s", profile, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        LOGE("%s", src.c_str());
        blobs_[hash] = nullptr;
        return nullptr;
    }
    if (disk_cache) disk_cache->Store(src, entry, profile, compiler_flags, code.Get());
    blobs_[hash] = code;
    return code.Get();
}

ID3DBlob* D3D12Backend::CompileDof(const std::string& src, const char* entry, const char* profile, uint64_t& hash) {
    const uint64_t flags = DofShaderCompilerFlags(entry, profile) | (DofShaderCompilerFlags2 << 32);
    const uint64_t profile_seed = Hash(profile, std::strlen(profile), Hash(&flags, sizeof(flags)));
    hash = Hash(src.data(), src.size(), Hash(entry, std::strlen(entry), profile_seed));
    auto found = blobs_.find(hash);
    if (found != blobs_.end()) return found->second.Get();
    auto code = LoadDofShaderBytecode(src, entry, profile, flags);
    if (!code) {
        ++dof_embedded_missing_;
        LOGR("d3d12: embedded DOF shader missing: %s %s %016llX; effect variant disabled", entry, profile,
             (unsigned long long)hash);
    } else ++dof_embedded_loads_;
    // A missing pack entry is a build/configuration error. Never compile or touch the disk cache here.
    blobs_[hash] = code;
    return code.Get();
}

// a full screen pass's pipeline: solid, no culling, one target, writing all channels; callers set the blend
D3D12_GRAPHICS_PIPELINE_STATE_DESC D3D12Backend::FullScreenPipelineDesc(ID3DBlob* vs, ID3DBlob* ps,
                                                                        DXGI_FORMAT rtv) const {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = root_.Get();
    d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = rtv;
    d.SampleDesc.Count = 1;
    return d;
}

ID3D12PipelineState* D3D12Backend::EnsureBlitPipeline(DXGI_FORMAT rtv, bool over, uint32_t samples) {
    auto& pso = blit_psos_[uint64_t(rtv) | uint64_t(over) << 32 | uint64_t(samples) << 40];
    if (pso) return pso.Get();
    uint64_t h;
    ID3DBlob* vs = Compile(kBlitHlsl, "vs", "vs_5_0", h);
    ID3DBlob* ps = Compile(kBlitHlsl, "psmain", "ps_5_0", h);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = FullScreenPipelineDesc(vs, ps, rtv);
    if (over) {
        auto& blend = d.BlendState.RenderTarget[0];
        // premultiplied colour, the target keeps its alpha
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
        blend.DestBlendAlpha = D3D12_BLEND_ONE;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    d.SampleDesc.Count = samples;
    dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
    return pso.Get();
}

// Pass's pipeline for the shader, kept in blit_psos_; null when it can't be made
ID3D12PipelineState* D3D12Backend::EnsurePassPipeline(const char* hlsl, const char* vs_entry, const char* entry,
                                                      DXGI_FORMAT rtv, float blend, bool dof) {
    uint64_t seed = uint64_t(rtv) << 1 | (blend >= 0 ? 1 : 0);
    uint64_t source_seed = Hash(hlsl, std::strlen(hlsl), seed);
    uint64_t entry_seed = Hash(entry, std::strlen(entry), source_seed);
    const uint64_t key = Hash(vs_entry, std::strlen(vs_entry), entry_seed);
    auto found = blit_psos_.find(key);
    if (found != blit_psos_.end() && (found->second || dof)) return found->second.Get();
    uint64_t hv;
    uint64_t hp;
    ID3DBlob* vs = dof ? CompileDof(hlsl, vs_entry, "vs_5_0", hv) : Compile(hlsl, vs_entry, "vs_5_0", hv);
    ID3DBlob* ps = dof ? CompileDof(hlsl, entry, "ps_5_0", hp) : Compile(hlsl, entry, "ps_5_0", hp);
    if (!vs || !ps) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = FullScreenPipelineDesc(vs, ps, rtv);
    if (blend >= 0) {
        auto& b = d.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = b.SrcBlendAlpha = D3D12_BLEND_BLEND_FACTOR;
        b.DestBlend = b.DestBlendAlpha = D3D12_BLEND_INV_BLEND_FACTOR;
        b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    if (dof) {
        auto* ready = dof_pipelines_ ? dof_pipelines_->Request(key ^ 0x444F465041535300ull, d, vs, ps) : nullptr;
        if (ready) blit_psos_[key] = ready;
        return ready;
    }
    auto& pso = blit_psos_[key];
    if (FAILED(dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso.Get();
}

bool D3D12Backend::Pass(const char* hlsl, const char* vs_entry, const char* entry, DXGI_FORMAT rtv,
                        D3D12_CPU_DESCRIPTOR_HANDLE rh, uint32_t w, uint32_t h, ID3D12Resource* const src[3],
                        const DXGI_FORMAT fmt[3], const float p[8], float blend) {
    ID3D12PipelineState* pso = EnsurePassPipeline(hlsl, vs_entry, entry, rtv, blend);
    if (!pso) return false;
    uint32_t slot = AllocSrvSlots(4);
    for (int i = 0; i < 4; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = i < 3 && src[i] ? fmt[i] : kBgra;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(i < 3 ? src[i] : nullptr, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    Alloc cb = Upload(256, 256);
    std::memcpy(cb.cpu, p, 8 * sizeof(float));
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    SetFullViewport(w, h);
    if (blend >= 0) {
        float f[4] = {blend, blend, blend, blend};
        cl_->OMSetBlendFactor(f);
    }
    DrawFullScreen(pso, cb.gpu, slot);
    bound_rt_ = bound_ds_ = nullptr;
    return true;
}

// a single sample copy of the target to read: while drawing to it, or of a multisampled one
bool D3D12Backend::EnsureFeedbackResource(Target& t) {
    if (t.fb) return true;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = t.res->GetDesc();
    td.SampleDesc.Count = 1;
    td.Alignment = 0;
    if (!t.depth) td.Flags = D3D12_RESOURCE_FLAG_NONE;
    t.fb_state = t.depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_COPY_DEST;
    return SUCCEEDED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, t.fb_state, nullptr,
                                                   IID_PPV_ARGS(&t.fb)));
}

ID3D12Resource* D3D12Backend::Feedback(Target& t) {
    uint32_t samples = Ms(t);
    // every write takes a multisampled target out of this state: the last copy still holds
    D3D12_RESOURCE_STATES read = t.depth ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                         : D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    if (t.fb && samples > 1 && t.state == read && t.fb_state == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        Transition(t.fb.Get(), t.fb_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        return t.fb.Get();
    }
    if (!EnsureFeedbackResource(t)) return nullptr;
    if (t.depth) {
        if (free_dsv_.empty()) return nullptr;
        Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(t.fb.Get(), t.fb_state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        // borrows the next free view without taking it, only for this recording
        D3D12_CPU_DESCRIPTOR_HANDLE dh = Cpu(dsv_heap_.Get(), free_dsv_.back(), dsv_inc_);
        dev_->CreateDepthStencilView(t.fb.Get(), &dv, dh);
        if (!DepthPass(t.res.Get(), samples, dh, t.w, t.h, 1)) {
            Release(std::move(t.fb));
            return nullptr;
        }
        Transition(t.fb.Get(), t.fb_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        return t.fb.Get();
    }
    bool ms = samples > 1;
    Transition(t.res.Get(), t.state, ms ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(t.fb.Get(), t.fb_state, ms ? D3D12_RESOURCE_STATE_RESOLVE_DEST : D3D12_RESOURCE_STATE_COPY_DEST);
    if (ms) {
        cl_->ResolveSubresource(t.fb.Get(), 0, t.res.Get(), 0, t.fmt);
    } else {
        cl_->CopyResource(t.fb.Get(), t.res.Get());
    }
    Transition(t.fb.Get(), t.fb_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return t.fb.Get();
}

// what a read of the target takes, in state st: the target itself, or a multisampled one's single sample copy
ID3D12Resource* D3D12Backend::Flat(Target& t, D3D12_RESOURCE_STATES*& st) {
    bool ms = Ms(t) > 1;
    st = ms ? &t.fb_state : &t.state;
    return ms ? Feedback(t) : t.res.Get();
}

// the key's vertex attributes, ATTR0 to ATTR15 in register order; returns how many
uint32_t FillInputLayout(const PsoKey& k, D3D12_INPUT_ELEMENT_DESC el[16]) {
    uint32_t n = 0;
    for (uint32_t r = 0; r < 16; r++) {
        if (!((k.regs >> r) & 1)) continue;
        uint32_t e = k.elements[r];
        el[n++] = {"ATTR", r, DXGI_FORMAT(e & 0xFF), (e >> 8) & 0xFF, e >> 16,
                   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    }
    return n;
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE TopologyType(uint32_t topo) {
    switch (topo) {
        case 0: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        case 1: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        default: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

constexpr uint32_t kFillWireframe = 0x1B01;  // D3DFILL_WIREFRAME
constexpr uint32_t kCullCw = 0x900;          // D3DCULL_CW

void FillDepthStencilDesc(const PsoKey& k, D3D12_DEPTH_STENCIL_DESC& ds) {
    ds.DepthEnable = k.zen;
    ds.DepthWriteMask = k.zwrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = Cmp(k.zfunc);
    ds.StencilEnable = k.sten;
    ds.StencilReadMask = UINT8(k.smask);
    ds.StencilWriteMask = UINT8(k.swmask);
    D3D12_DEPTH_STENCILOP_DESC so{StencilOp(k.sfail), StencilOp(k.szfail), StencilOp(k.spass), Cmp(k.sfunc)};
    ds.FrontFace = ds.BackFace = so;
}

// everything of the pipeline but its input layout
void D3D12Backend::FillPipelineDesc(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps,
                                    D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) const {
    d.pRootSignature = root_.Get();
    d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    auto& b = d.BlendState.RenderTarget[0];
    b.BlendEnable = k.blend;
    b.SrcBlend = Blend(k.src, false);
    b.DestBlend = Blend(k.dst, false);
    b.BlendOp = BlendOp(k.op);
    b.SrcBlendAlpha = Blend(k.src, true);
    b.DestBlendAlpha = Blend(k.dst, true);
    b.BlendOpAlpha = BlendOp(k.op);
    b.RenderTargetWriteMask = UINT8(k.wmask);
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = k.fill == kFillWireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = k.cull ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    d.RasterizerState.FrontCounterClockwise = k.ccw;
    d.RasterizerState.DepthBias = k.zbias;
    d.RasterizerState.SlopeScaledDepthBias = k.zslope;
    d.RasterizerState.DepthClipEnable = !k.unclipped;
    d.RasterizerState.MultisampleEnable = k.msaa != 0;
    FillDepthStencilDesc(k, d.DepthStencilState);
    d.PrimitiveTopologyType = TopologyType(k.topo);
    d.NumRenderTargets = k.has_rt ? 1 : 0;
    d.RTVFormats[0] = k.has_rt ? DXGI_FORMAT(k.rtfmt) : DXGI_FORMAT_UNKNOWN;
    d.DSVFormat = k.has_ds ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = std::max(k.msaa, 1u);
}

ID3D12PipelineState* D3D12Backend::EnsurePipeline(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps) {
    uint64_t h = Hash(&k, sizeof(k));
    auto it = psos_.find(h);
    if (it != psos_.end()) return it->second.Get();
    D3D12_INPUT_ELEMENT_DESC el[16];
    uint32_t n = FillInputLayout(k, el);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.InputLayout = {el, n};
    FillPipelineDesc(k, vs, ps, d);
    ComPtr<ID3D12PipelineState> pso;
    g_perf.psos++;
    if (FAILED(dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) LOGE("d3d12: pipeline creation failed");
    psos_[h] = pso;
    return pso.Get();
}

ID3D12PipelineState* D3D12Backend::EnsureParticlePipeline(PsoKey k, ID3DBlob* vs, ID3DBlob* ps) {
    if (!dof_pipelines_ || !vs || !ps) return nullptr;
    // Normalize descriptor-equivalent inherited state, especially the source pass with no DS/blending.
    if (!k.blend) k.src = k.dst = k.op = 0;
    if (!k.zen) k.zwrite = k.zfunc = 0;
    if (!k.sten) k.sfunc = k.smask = k.swmask = k.sfail = k.szfail = k.spass = 0;
    if (!k.cull) k.ccw = 0;
    if (k.fill != kFillWireframe) k.fill = 0;
    D3D12_INPUT_ELEMENT_DESC elements[16];
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.InputLayout = {elements, FillInputLayout(k, elements)};
    FillPipelineDesc(k, vs, ps, desc);
    return dof_pipelines_->Request(Hash(&k, sizeof(k)), desc, vs, ps);
}

void D3D12Backend::QueueDofPipelines(uint32_t native_samples) {
    bool ready = true;
    weapon_dof_.coverage_ps = CompileDof(DofStaticPassSource("viewmodel_dof_coverage"),
        "viewmodel_dof_coverage", "ps_5_0", weapon_dof_.coverage_hash);
    const uint32_t sample_counts[] = {1, native_samples};
    for (uint32_t i = 0; i < 2; ++i) {
        if (i && native_samples == 1) continue;
        const uint32_t samples = sample_counts[i];
        ready &= EnsureDepthOfFieldPipeline("depth_of_field_pack", kDofFormats[0], 1, samples) != nullptr;
        ready &= EnsureDepthOfFieldPipeline("depth_of_field", kBgra, samples) != nullptr;
        ready &= EnsureDepthOfFieldPipeline("viewmodel_dof_pack", kDofFormats[0], 1, samples) != nullptr;
        ready &= EnsureDepthOfFieldPipeline("viewmodel_dof", kBgra, samples) != nullptr;
        ready &= EnsureDepthOfFieldPipeline("viewmodel_dof_pack", kDofFormats[0], 1, samples, true) != nullptr;
        if (samples == 1) ready &= EnsureDepthOfFieldPipeline("viewmodel_dof", kBgra, 1, 1, true) != nullptr;
        ready &= EnsureDepthOfFieldPipeline("ffx_dof_prepare", kDofFormats[0], 1, samples) != nullptr;
        ready &= EnsureDepthOfFieldPipeline("ffx_dof_present", kBgra, samples) != nullptr;
        if (samples > 1) {
            const auto source = DofParticleRangeSource(samples, particle_float_bounds_);
            ready &= EnsurePassPipeline(source.c_str(), "vs", "range_ps", ParticleDofRangeFormat(samples), -1, true) != nullptr;
        }
        // Native 15BB00 uses GREATER; disabled alpha testing maps to ALWAYS. Other alpha/raster/mask
        // states remain supported by the same embedded library and a single asynchronous PSO worker.
        for (uint32_t pass = 0; pass < 8; ++pass) {
            const bool shared = pass >= 3;
            if (shared && samples != 2) continue;
            if (pass >= 6 && !particle_visibility_cache_) continue;
            uint64_t vertex_hash;
            auto* vs = CompileDof(DofParticleVertexSource(pass), "main", "vs_5_0", vertex_hash);
            if (!vs) { ready = false; continue; }
            for (int alpha : {4, 7}) for (bool kill : {false, true}) for (uint32_t blend = 0; blend < 2; ++blend) {
                if (shared && (alpha != 4 || kill || (pass != 4 && pass != 7 && blend))) continue;
                uint64_t pixel_hash;
                auto* ps = CompileDof(DofParticlePixelSource(samples, blend, shared ? 0 : alpha, kill, pass, particle_float_bounds_),
                                      "main", "ps_5_0", pixel_hash);
                if (!ps) { ready = false; continue; }
                for (uint32_t cull = 0; cull < 2; ++cull)
                    for (uint32_t mask : {7u, 15u}) for (uint32_t depth : {0x201u, 0x203u}) {
                        PsoKey state{};
                        state.vs = vertex_hash; state.ps = pixel_hash;
                        state.regs = 0x209; state.topo = uint32_t(Topology::Triangles);
                        state.elements[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
                        state.elements[3] = DXGI_FORMAT_B8G8R8A8_UNORM | (16u << 16);
                        state.elements[9] = DXGI_FORMAT_R32G32_FLOAT | (20u << 16);
                        state.has_rt = 1; state.rtfmt = kBgra;
                        state.has_ds = state.zen = state.blend = 1;
                        state.zfunc = depth; state.src = 0x302; state.dst = blend ? 1 : 0x303; state.op = 0x8006;
                        state.wmask = mask; state.cull = state.ccw = cull;
                        state.msaa = samples > 1 ? samples : 0;
                        if (pass == 1 || pass == 3 || pass == 5 || pass == 6) {
                            state.has_ds = state.zen = state.blend = state.msaa = 0;
                            state.rtfmt = kDofFormats[0]; state.wmask = D3D12_COLOR_WRITE_ENABLE_ALL;
                            if (pass == 3) {
                                state.rtfmt = DXGI_FORMAT_R32G32_FLOAT;
                                state.cull = state.ccw = 0;
                            } else if (pass == 5) {
                                state.rtfmt = DXGI_FORMAT_R8_UNORM; state.msaa = 2;
                            } else if (pass == 6) {
                                state.rtfmt = DXGI_FORMAT_R16_FLOAT;
                                state.cull = state.ccw = 0;
                            }
                        }
                        EnsureParticlePipeline(state, vs, ps);
                        if (pass != 1 && pass != 3 && pass != 5 && pass != 6) {
                            if (blend) {
                                state.op = 0x800B;  // native SRC_ALPHA/ONE reverse-subtract uses the same P/Q shader
                                EnsureParticlePipeline(state, vs, ps);
                                state.op = 0x8006;
                            }
                            // Verified particle depth uses the shader's per-sample coverage output.
                            // Keep the original hardware-depth family ready for unknown encodings.
                            state.zen = 0;
                            EnsureParticlePipeline(state, vs, ps);
                            if (blend) {
                                state.op = 0x800B;
                                EnsureParticlePipeline(state, vs, ps);
                            }
                        }
                    }
            }
        }
    }
    ready &= EnsureDepthOfFieldPipeline("depth_of_field_gather", kDofFormats[0], 1) != nullptr;
    ready &= EnsureDepthOfFieldPipeline("depth_of_field_downsample", kDofFormats[0], 1) != nullptr;
    ready &= EnsurePassPipeline(kParticleDofAtlasFilterHlsl.c_str(), "vs", "filter_ps",
                                kDofFormats[0], -1, true) != nullptr;
    (void)ready; // A null request means pending or unavailable; neither blocks this native frame.
}

bool D3D12Backend::PrepareDofResources(uint32_t width, uint32_t height, bool balanced) {
    // Prime reaches this only after polling the old DOF submission fence. DOF draws remain disabled.
    dof_resources_quiescent_ = true;
    struct Restore { bool& flag; ~Restore() { flag = false; } } restore{dof_resources_quiescent_};
    bool ready = true;
    if (balanced) {
        ready &= EnsureDepthOfFieldFilter(width, height);
        ready &= EnsureDepthOfFieldLayers((width + 1) / 2, (height + 1) / 2);
    }
    ready &= EnsureWeaponDofTargets(width, height);
    EnsureWeaponDofTiles(width, height); // optional sparse path, full-screen fallback remains ready
    for (auto& [id, target] : targets_)
        if (!target.depth && target.gw == guest_w_ && target.gh == guest_h_) ready &= EnsureFeedbackResource(target);
    // At high native resolutions a small-CoC full-size quad may need a 4096 cell. Reserve that page
    // before gameplay instead of allocating it and flushing the current frame during the first reload.
    ready &= EnsureParticleDofAtlas(std::max(width, height) > 2044 ? 4096 : 2048);
    if (msaa_ == 2 && particle_visibility_cache_)
        EnsureParticleDofVisibility(std::max(width, height) > 2044 ? 4096 : 2048);
    // Optional optimization: allocation failure must not disable the established DOF path.
    if (msaa_ == 2) PrepareParticleSharedCoverage(width, height);
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    const auto prepare_depth = [&](ComPtr<ID3D12Resource>& resource, D3D12_RESOURCE_STATES& state,
                                   uint32_t w, uint32_t h, uint32_t samples) {
        if (resource) {
            const auto old = resource->GetDesc();
            if (old.Width == w && old.Height == h && old.SampleDesc.Count == samples) return true;
        }
        auto desc = Texture2DDesc(DXGI_FORMAT_R24G8_TYPELESS, w, h);
        desc.SampleDesc.Count = samples;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        ComPtr<ID3D12Resource> next;
        if (FAILED(dev_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&next)))) return false;
        if (resource) Release(std::move(resource));
        resource = std::move(next); state = D3D12_RESOURCE_STATE_COPY_DEST;
        return true;
    };
    ready &= prepare_depth(weapon_dof_.before, weapon_dof_.before_state, width, height, msaa_);
    for (uint32_t i = 0; i < kParticleDofSnapshots; ++i) {
        auto& snapshot = particle_dof_.snapshots[i];
        const uint32_t w = i ? std::max(1u, uint32_t(std::lround((guest_w_ / 8) * scale_))) : width;
        const uint32_t h = i ? std::max(1u, uint32_t(std::lround((guest_h_ / 8) * scale_))) : height;
        const uint32_t samples = i ? 1 : msaa_;
        ready &= prepare_depth(snapshot.depth, snapshot.state, w, h, samples);
        snapshot.source = nullptr;
        if (samples > 1) ready &= EnsureParticleDofDepthRange(i, w, h, samples);
    }
    return ready;
}

void D3D12Backend::PublishDofPreparation() {
    if (dof_preparation_ != DofPreparation::Complete || presents_ < dof_loading_after_present_ ||
        !dof_loading_generation_ ||
        g_dof_loading_requested_generation.load(std::memory_order_acquire) != dof_loading_generation_) return;
    g_dof_loading_ready_generation.store(dof_loading_generation_, std::memory_order_release);
}

void D3D12Backend::FinishDofPreparation(bool usable) {
    dof_prepared_ = usable;
    dof_preparation_ = DofPreparation::Complete;
    const auto pipelines = dof_pipelines_ ? dof_pipelines_->GetStats() : DofPipelineCache::Stats{};
    LOGR("DOF native-loading preparation complete: %llu embedded shaders/%llu missing, "
         "%llu PSOs ready/%llu pending/%llu failed, effects %s, %.1f ms elapsed; native frames continued",
         (unsigned long long)dof_embedded_loads_, (unsigned long long)dof_embedded_missing_,
         (unsigned long long)pipelines.ready, (unsigned long long)pipelines.pending, (unsigned long long)pipelines.failed,
         usable ? "ready" : "native fallback", PerfMs(dof_preparation_start_));
    // A terminal resource/worker failure must release the native loading gate too.
    PublishDofPreparation();
}

void D3D12Backend::PrimeDepthOfField() {
    // Called once after an ordinary native Present. Boot/menu frames never start preparation on their own.
    if (!dof_preparation_started_ || presents_ < dof_loading_after_present_) return;
    const auto& options = game::Opt();
    const bool enabled = options.reload_blur && options.modern_reload_dof;
    const uint32_t profile = options.fidelityfx_dof ? (options.dof_cinematic_quality ? 2u : 1u) : 0u;
    const uint32_t width = std::max(1u, uint32_t(std::lround(guest_w_ * scale_)));
    const uint32_t height = std::max(1u, uint32_t(std::lround(guest_h_ * scale_)));
    const uint32_t key[] = {width, height, msaa_, profile, uint32_t(enabled)};
    const uint64_t signature = Hash(key, sizeof(key));
    if (dof_prime_signature_ != signature) {
        dof_prime_signature_ = signature;
        dof_prepared_ = false;
        dof_preparation_ = DofPreparation::Drain;
        dof_drain_fence_ = fence_value_;
        dof_warm_fence_ = 0;
        dof_preparation_start_ = PerfClock::now();
        g_dof_loading_ready_generation.store(0, std::memory_order_release);
        particle_dof_.world_valid = weapon_dof_.pending = false;
        for (auto& tag : particle_dof_.depth_projection) tag = {};
    }
    try {
        switch (dof_preparation_) {
        case DofPreparation::Dormant:
            return;
        case DofPreparation::Complete:
            PublishDofPreparation();
            return;
        case DofPreparation::Drain:
            // Turning the feature off needs no new resources. Retain old GPU/CPU owners for safe
            // later retirement, but do not make a disabled feature hold the mission loading gate.
            if (!enabled) { FinishDofPreparation(false); return; }
            if (fence_->GetCompletedValue() < dof_drain_fence_) return;
            // std::future destruction can wait. Keep an obsolete CPU job until it has really finished.
            if (dof_cpu_preparation_.valid()) {
                if (dof_cpu_preparation_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
                try { (void)dof_cpu_preparation_.get(); } catch (...) {}
            }
            ffx_dof_.Reset(); // All previous SDK dispatches are complete; no GPU wait is issued here.
            ffx_dof_failed_ = 0;
            ffx_dof_reported_ = false;
            dof_preparation_ = DofPreparation::Queue;
            return;
        case DofPreparation::Queue: {
            if (!dof_pipelines_) { FinishDofPreparation(false); return; }
            QueueDofPipelines(msaa_);
            if (!profile) { dof_preparation_ = DofPreparation::Resources; return; }
            auto device = dev_;
            const bool cinematic = profile == 2;
            const auto extent = FidelityFxDofProcessingSize(width, height, cinematic ? 2 : 1);
            dof_cpu_signature_ = signature;
            dof_cpu_preparation_ = std::async(std::launch::async, [device, extent, profile, cinematic] {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                DofPreparedCpu result;
                if (profile && extent.width && extent.height) {
                    result.context = std::make_unique<FidelityFxDof>();
                    result.context->Create(device.Get(), extent.width, extent.height, cinematic ? 8 : 4, cinematic);
                }
                return result;
            });
            dof_preparation_ = DofPreparation::Cpu;
            return;
        }
        case DofPreparation::Cpu: {
            if (dof_cpu_preparation_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            auto result = dof_cpu_preparation_.get();
            if (dof_cpu_signature_ != signature) { dof_preparation_ = DofPreparation::Drain; return; }
            if (result.context && result.context->Ready()) ffx_dof_.Swap(*result.context);
            if (profile && !ffx_dof_.Ready()) {
                ffx_dof_failed_ |= profile == 2 ? 2u : 1u;
                LOGR("DOF native loading: FidelityFX unavailable; preparing Balanced fallback");
            }
            dof_preparation_ = DofPreparation::Resources;
            return;
        }
        case DofPreparation::Resources: {
            bool world_ready = profile && ffx_dof_.Ready();
            if (world_ready) {
                const auto extent = FidelityFxDofProcessingSize(width, height, profile == 2 ? 2 : 1);
                world_ready = EnsureFidelityFxTargets(extent.width, extent.height);
                if (!world_ready) ffx_dof_failed_ |= profile == 2 ? 2u : 1u;
            }
            if (!PrepareDofResources(width, height, !world_ready)) { FinishDofPreparation(false); return; }
            dof_preparation_ = world_ready ? DofPreparation::Warm : DofPreparation::Gpu;
            dof_warm_fence_ = world_ready ? 0 : dof_drain_fence_;
            return;
        }
        case DofPreparation::Warm: {
            // Record on the renderer's next normal frame. Its ordinary Submit supplies the fence;
            // preparation never presents, submits, or waits on a command list of its own.
            dof_preparation_ = DofPreparation::Gpu;
            dof_warm_recorded_ = true;
            for (uint32_t i = 0; i < 2; ++i)
                Transition(ffx_dof_targets_[i].Get(), ffx_dof_states_[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
            const float color[4] = {};
            const float depth[4] = {.5f, .5f, .5f, .5f};
            cl_->ClearRenderTargetView(FidelityFxView(0), color, 0, nullptr);
            cl_->ClearRenderTargetView(FidelityFxView(1), depth, 0, nullptr);
            for (uint32_t i = 0; i < 2; ++i)
                Transition(ffx_dof_targets_[i].Get(), ffx_dof_states_[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(ffx_dof_targets_[2].Get(), ffx_dof_states_[2], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            const bool warmed = ffx_dof_.Dispatch(cl_.Get(), ffx_dof_targets_[0].Get(), ffx_dof_targets_[1].Get(),
                                                 ffx_dof_targets_[2].Get(), 1.0f);
            Begin(); // SDK work changes heaps/root bindings, including on a failed dispatch.
            if (!warmed) {
                ffx_dof_failed_ |= profile == 2 ? 2u : 1u;
                if (!PrepareDofResources(width, height, true)) { FinishDofPreparation(false); return; }
            }
            return;
        }
        case DofPreparation::Gpu:
            if (!dof_warm_fence_ || fence_->GetCompletedValue() < dof_warm_fence_) return;
            if (dof_pipelines_ && dof_pipelines_->GetStats().pending) return;
            FinishDofPreparation(bool(dof_pipelines_));
            return;
        }
    } catch (...) {
        // Keep any still-running future owned until a later configuration or shutdown can reap it.
        // Queue/allocation/SDK failures never strand the mission at its loading gate.
        Begin();
        FinishDofPreparation(false);
    }
}


TexKind TextureKind(const TextureDesc& d) {
    if (d.cube) return kTexCube;
    if (d.depth > 1) return kTex3D;
    return kTex2D;
}

constexpr float kUpscaledMin = 1.01f;  // host texels per console texel past rounding: the target is upscaled

ID3D12Resource* D3D12Backend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box,
                                                DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    D3D12_SHADER_RESOURCE_VIEW_DESC& sv = stages.sv[stage];
    if (&t == draw_ds_) return nullptr;
    bool feedback = &t == draw_rt_;
    stages.rt[stage] = true;
    info.flags |= kTexTarget;
    if (!t.depth && LatchCpuWrite(t)) {
        Surface su;
        su.header = s.textures[size_t(stage)];
        UploadTarget(t, su);
    }
    info.kind = kTex2D;
    // a pass shrinking it averages each tap over a console texel, else upscaled taps skip texels and ghost
    Target* rt = FindTarget(s.rt, false);
    float k = float(t.w) / (t.gw * stages.k[stage]);
    // a blur between the small targets reads texel for texel, whatever their sizes
    bool blur = rt && t.gw < guest_w_ && rt->gw < guest_w_;
    if (!t.depth && box && k > kUpscaledMin && rt && rt != &t &&
        (blur || t.gw > rt->gw || t.gh > rt->gh)) {
        stages.wide[stage] = k;
        stages.blur[stage] = blur;
        info.flags |= kTexWide;
    }
    if (ext_) ext_->Sampled(d, &t, 0, info);
    sv.Format = t.depth ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : t.fmt;
    // the console reads the pixels it is about to overwrite: give the draw a copy
    if (feedback) return Feedback(t);
    if (t.depth) info.shadow = ShadowDepthKind(d.format);
    info.kind = kTex2D;
    D3D12_RESOURCE_STATES* st;
    ID3D12Resource* r = Flat(t, st);
    if (r) Transition(r, *st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return r;
}

bool SameTexture(const Tex& tx, const TextureDesc& d) {
    return tx.res && tx.d.format == d.format && tx.d.width == d.width && tx.d.height == d.height &&
           tx.d.depth == d.depth && tx.d.levels == d.levels && tx.d.cube == d.cube && tx.d.pitch == d.pitch;
}

// Keep the previous resource if allocation fails, so an HD upload can fall back.
bool D3D12Backend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key) {
    g_perf.newtex++;
    Tex previous = tx;
    tx = Tex{};
    tx.d = d;
    tx.fmt = HostFormat(fi.host);
    bool bc = fi.compressed;
    uint32_t w = d.width;
    uint32_t h = d.height;
    tx.levels = d.levels;
    if (bc && ((w & 3) || (h & 3))) {
        w = (w + 3) & ~3u;
        h = (h + 3) & ~3u;
        tx.levels = 1;
    }
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td{};
    td.Dimension = d.depth > 1 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = UINT16(d.cube ? 6 : d.depth);
    td.MipLevels = UINT16(tx.levels);
    td.Format = tx.fmt;
    td.SampleDesc.Count = 1;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&tx.res)))) {
        LOGR("textures: GPU allocation failed for resource %llx (%ux%u)", static_cast<unsigned long long>(key), w, h);
        tx = std::move(previous);
        return false;
    }
    if (previous.res) CurrentFrame().garbage.push_back(std::move(previous.res));
    return true;
}

void CopyLevelRows(uint8_t* out, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp, UINT rows, UINT64 row_size,
                   const std::vector<uint8_t>& px, uint32_t pitch) {
    uint32_t slices = fp.Footprint.Depth;
    size_t src_rows = pitch ? px.size() / pitch / std::max(slices, 1u) : 0;
    for (uint32_t z = 0; z < slices; z++) {
        for (uint32_t y = 0; y < rows; y++) {
            uint8_t* dst = out + (size_t(z) * rows + y) * fp.Footprint.RowPitch;
            if (y < src_rows) {
                std::memcpy(dst, &px[(size_t(z) * src_rows + y) * pitch], std::min<size_t>(pitch, row_size));
            } else {
                std::memset(dst, 0, size_t(row_size));
            }
        }
    }
}

void D3D12Backend::UploadTextureLevel(Tex& tx, uint32_t f, uint32_t l, const std::vector<uint8_t>& px,
                                      uint32_t pitch) {
    D3D12_RESOURCE_DESC rd = tx.res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    uint32_t sub = f * tx.levels + l;
    dev_->GetCopyableFootprints(&rd, sub, 1, 0, &fp, &rows, &row_size, &bytes);
    Alloc a = Upload(bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    CopyLevelRows(a.cpu, fp, rows, row_size, px, pitch);
    fp.Offset = a.off;
    D3D12_TEXTURE_COPY_LOCATION dl{tx.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dl.SubresourceIndex = sub;
    D3D12_TEXTURE_COPY_LOCATION sl{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    sl.PlacedFootprint = fp;
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
}

void D3D12Backend::UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p,
                                 const uint32_t* colours, bool same) {
    D3D12_RESOURCE_STATES st = same ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_DEST;
    Transition(tx.res.Get(), st, D3D12_RESOURCE_STATE_COPY_DEST);
    uint32_t faces = d.cube ? 6 : 1;
    std::vector<uint8_t> px;
    for (uint32_t f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch;
            DecodeLevel(d, l, f, p, colours, px, pitch);
            if (ext_) ext_->Decoded(d, fi, f, l, px, pitch, tx.flags);
            UploadTextureLevel(tx, f, l, px, pitch);
        }
    }
    Transition(tx.res.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void FillTextureView(const Tex& tx, const TextureDesc& d, D3D12_SHADER_RESOURCE_VIEW_DESC& sv) {
    sv.Format = tx.fmt;
    if (d.cube) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube.MipLevels = tx.levels;
    } else if (d.depth > 1) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        sv.Texture3D.MipLevels = tx.levels;
    } else {
        sv.Texture2D.MipLevels = tx.levels;
    }
}

// a P8 texture's palette: its colours, their address (part of the texture's key) and their hash, all 0 without one
struct StagePalette {
    uint32_t colours[256] = {};
    uint32_t va = 0;
    uint64_t hash = 0;
};

// a P8 texture is decoded with the stage's palette: 256 colours, fewer by the size code in the top two bits
void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& palette) {
    constexpr uint32_t kFmtP8 = 0x0B;          // D3DFMT_P8
    constexpr uint32_t kPaletteCommon = 0;     // D3DPalette: Common, its top two bits the size code
    constexpr uint32_t kPaletteData = 4;       // D3DPalette: Data, physical address of the colours
    constexpr uint32_t kPaletteSizeShift = 30;
    if (d.format != kFmtP8 || !s.palettes[size_t(stage)]) return;
    uint32_t header = s.palettes[size_t(stage)];
    palette.va = mem::VirtualFromPhysical(kernel::R32(header + kPaletteData));
    uint32_t size_code = kernel::R32(header + kPaletteCommon) >> kPaletteSizeShift;
    size_t bytes = (std::size(palette.colours) >> size_code) * sizeof(uint32_t);
    std::memcpy(palette.colours, kernel::HostPtr(palette.va), bytes);
    palette.hash = Hash(palette.colours, sizeof(palette.colours));
}

// a texture in guest memory: made again when its layout changed, uploaded again when its bytes did
ID3D12Resource* D3D12Backend::BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                               DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    StagePalette palette;
    ReadStagePalette(s, stage, d, palette);
    const uint64_t key = uint64_t(palette.va) << 32 | d.data;
    Tex& tx = textures_[key];
    uint32_t total = TotalSize(d);
    bool same = SameTexture(tx, d);
    if (!same && !CreateTexture(tx, d, fi, key)) return nullptr;
    // cheap change check once per frame: lock epochs plus a sample of the bytes
    if (tx.frame != presents_ + 1 || !same || tx.palette != palette.hash) {
        tx.frame = presents_ + 1;
        tx.palette = palette.hash;
        const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
        auto perf_t = PerfClock::now();
        uint64_t sig = MemSig(d.data, total) ^ DirtyEpoch(d.data) ^ palette.hash;
        g_perf.sig += PerfMs(perf_t);
        if (sig != tx.sig || !same) {
            PerfScope perf_up{g_perf.texup};
            g_perf.texups++;
            TextureDesc hd;
            std::vector<uint8_t> blocks;
            bool replaced = texture_pack::Load(d, p, palette.va ? palette.colours : nullptr, hd, blocks);
            TextureDesc upload = replaced ? hd : d;
            const FormatInfo* format = replaced ? &Format(hd.format) : &fi;
            auto resource = tx.res->GetDesc();
            if (tx.replacement != replaced || resource.Width != upload.width || resource.Height != upload.height ||
                tx.levels != upload.levels) {
                if (CreateTexture(tx, upload, *format, key)) {
                    same = false;
                } else {
                    if (!replaced) return nullptr;
                    replaced = false;
                    upload = d;
                    format = &fi;
                    if (tx.replacement || !SameTexture(tx, d)) {
                        if (!CreateTexture(tx, d, fi, key)) return nullptr;
                        same = false;
                    }
                }
            }
            tx.d = d;
            tx.replacement = replaced;
            tx.sig = sig;
            tx.frame = presents_ + 1;
            tx.palette = palette.hash;
            g_perf.texup_bytes += TotalSize(upload);
            UploadTexture(tx, upload, *format, replaced ? blocks.data() : p,
                          replaced ? nullptr : (palette.va ? palette.colours : nullptr), same);
        }
    }
    FillTextureView(tx, d, stages.sv[stage]);
    if (fi.depth) info.shadow = ShadowDepthKind(d.format);
    if (ext_) ext_->Sampled(d, nullptr, tx.flags, info);
    return tx.res.Get();
}

ID3D12Resource* D3D12Backend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    D3D12_SHADER_RESOURCE_VIEW_DESC& sv = stages.sv[stage];
    info = {};
    sv = {};
    stages.rt[stage] = false;
    stages.k[stage] = 1;
    stages.wide[stage] = 0;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sv.Texture2D.MipLevels = 1;
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return nullptr;
    const FormatInfo& fi = Format(d.format);
    info.kind = TextureKind(d);
    info.rect = d.linear;

    // rendered by the GPU: sample the target itself; DXT data loaded over a target's memory is not it
    uint64_t alt = 0;
    bool box = true;
    if (ext_) ext_->Sample(d.data, fi.depth, stage, alt, stages.k[stage], box);
    auto tit = targets_.find(TargetMapKey(d.data, fi.depth) | alt);
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed) {
        return BindTargetTexture(s, stage, d, tit->second, box, stages);
    }
    return BindGuestTexture(s, stage, d, fi, stages);
}

// D3DTEXF: 1 point, 2 linear, 3 anisotropic
constexpr uint32_t kTexfLinear = 2;
constexpr uint32_t kTexfAnisotropic = 3;

D3D12_SAMPLER_DESC StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp, uint32_t aniso) {
    D3D12_SAMPLER_DESC sd = {};
    const auto& t = s.tss[size_t(i)];
    uint32_t mag = t[kTssMagFilter];
    uint32_t min = t[kTssMinFilter];
    uint32_t mip = t[kTssMipFilter];
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    D3D12_FILTER_TYPE fmin = lin(min) ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE fmag = lin(mag) ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE fmip = mip == kTexfLinear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    sd.Filter = D3D12_FILTER(fmin << 4 | fmag << 2 | fmip);
    bool anisotropic = min == kTexfAnisotropic || mag == kTexfAnisotropic;
    uint32_t an = anisotropic ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
    // forced only where the title filters across mip levels and the texture has them
    bool mipped = mip && stages.sv[i].Texture2D.MipLevels > 1;
    if (aniso && lin(min) && mipped && !stages.info[i].shadow) an = aniso;
    if (an > 1) {
        sd.Filter = D3D12_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = std::min<uint32_t>(an, 16);
    }
    // clamp: the draw's coordinates all lie in 0..1 (UnitTexcoords), so wrap is read as clamp
    uint32_t u = t[kTssAddressU];
    uint32_t v = t[kTssAddressV];
    sd.AddressU = Address(clamp && u == kTaddressWrap ? kTaddressClamp : u);
    sd.AddressV = Address(clamp && v == kTaddressWrap ? kTaddressClamp : v);
    sd.AddressW = Address(t[kTssAddressW]);
    sd.MipLODBias = Bits(t[kTssMipLodBias]);
    sd.MinLOD = float(t[kTssMaxMipLevel]);
    sd.MaxLOD = mip ? D3D12_FLOAT32_MAX : sd.MinLOD;
    Color(t[kTssBorderColor], sd.BorderColor);
    if (stages.info[i].shadow) {
        // the console tests texel OP r, D3D12 tests r OP texel
        static const uint8_t swap[8] = {0, 4, 2, 6, 1, 5, 3, 7};
        sd.Filter = lin(min) || lin(mag) ? D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT
                                         : D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
        sd.ComparisonFunc = D3D12_COMPARISON_FUNC(swap[Cmp(s.rs[kRsShadowFunc]) - 1] + 1);
        sd.MaxAnisotropy = 0;
    }
    return sd;
}

uint32_t D3D12Backend::EnsureStageSamplers(const State& s, const DrawTextures& stages, bool clamp) {
    D3D12_SAMPLER_DESC sd[4];
    uint32_t aniso = app::Get().aniso;
    for (int i = 0; i < 4; i++) sd[i] = StageSampler(s, i, stages, clamp, aniso);
    uint64_t key = Hash(sd, sizeof(sd));
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    if (smp_used_ + 4 > kSamplerCount) {
        samplers_.clear();
        smp_used_ = 0;
    }
    uint32_t base = smp_used_;
    smp_used_ += 4;
    for (int i = 0; i < 4; i++) dev_->CreateSampler(&sd[i], Cpu(smp_heap_.Get(), base + i, smp_inc_));
    samplers_[key] = base;
    return base;
}

uint32_t SimpleState(const State& s, uint32_t method) {
    return s.simple[method >> 2];
}

constexpr uint32_t kQuadVertices = 6;  // a quad drawn as two triangles

DrawShaders ClassifyShaders(const State& s, const Assembled& a) {
    DrawShaders sh;
    // a shader made from a declaration alone (no program) is the fixed function pipeline on those inputs
    sh.prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    sh.fixed_transform = !sh.prog && !a.pretransformed;
    if (!sh.prog) sh.fk = MakeFixedKey(s);
    return sh;
}

// the program's hash is cached in the guest shader object
void EnsureProgramHash(const State& s) {
    if (!s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
}

// vertex shader cache key salts
constexpr uint64_t kDeclarationSalt = 0x10000;    // fixed function seed, above the 16 bit register mask
constexpr uint64_t kPretransformedSalt = 0x5A5A;  // screen space fixed function vertices, apart from untransformed
constexpr uint64_t kNearDepthSalt = 0xC2B2AE3D27D4EB4Full;

uint64_t VertexKey(const State& s, const Assembled& a, const DrawShaders& sh) {
    uint64_t seed = sh.prog ? s.vs->hash ^ a.regs : Hash(&s.vs_handle, 4, a.regs | kDeclarationSalt);
    uint64_t vkey = Hash(&a.decode, 8, seed);
    if (!sh.prog && a.pretransformed) vkey ^= kPretransformedSalt;
    if (!sh.prog) vkey = Hash(&sh.fk, sizeof(sh.fk), vkey);
    return vkey;
}

void DrawShaderKeys(const State& s, const Assembled& a, const PixelKey& pk, DrawShaders& sh) {
    sh.vkey = VertexKey(s, a, sh);
    sh.pkey = Hash(&pk, sizeof(pk)) ^ (sh.near_depth ? kNearDepthSalt : 0);
    if (sh.particle_samples) {
        sh.vkey ^= 0x50444F4656530001ull ^ (uint64_t(sh.particle_pass) << 24);
        sh.pkey ^= 0x50444F4650530002ull ^ (uint64_t(sh.particle_samples) << 48) ^
            (uint64_t(sh.particle_blend) << 56) ^ (uint64_t(sh.particle_alpha_function + 1) << 32) ^
            (uint64_t(sh.particle_pass) << 24);
    }
}

std::string VertexHlsl(const State& s, const Assembled& a, const DrawShaders& sh) {
    if (sh.particle_samples) return DofParticleVertexSource(sh.particle_pass);
    if (sh.prog) {
        return VertexProgram(*s.vs, a.regs, a.decode);
    }
    return FixedVertexHlsl(a.regs, a.pretransformed, a.decode, sh.fk);
}

// sets sh.vh and sh.vs_code, compiling the shader on a cache miss
void D3D12Backend::EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh) {
    auto vit = shader_src_.find(sh.vkey);
    if (vit != shader_src_.end()) {
        sh.vh = vit->second;
        sh.vs_code = blobs_[sh.vh].Get();
        return;
    }
    const auto source = VertexHlsl(s, a, sh);
    sh.vs_code = sh.particle_samples ? CompileDof(source, "main", "vs_5_0", sh.vh) :
                                      Compile(source, "main", "vs_5_0", sh.vh);
    shader_src_[sh.vkey] = sh.vh;
}

// textures may have recorded copies that unbound the targets or moved them out of their target states
bool D3D12Backend::BindStageTexturesAndTargets(const State& s, DrawTextures& stages) {
    auto perf_t = PerfClock::now();
    draw_rt_ = bound_rt_;
    draw_ds_ = bound_ds_;
    for (int i = 0; i < 4; i++) stages.res[i] = BindTexture(s, i, stages);
    g_perf.bind += PerfMs(perf_t);
    if (bound_rt_ != draw_rt_ || bound_ds_ != draw_ds_) return BindTargets(s);
    if (bound_rt_) Transition(bound_rt_->res.Get(), bound_rt_->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (bound_ds_) Transition(bound_ds_->res.Get(), bound_ds_->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    return true;
}

constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;
constexpr uint8_t kTexGlowSmooth = 16;  // local shader variant, separate from SDK target/extension flags

// Smooth only the final glow layer. Shared reflection and reload blur shaders stay intact.
std::string GlowPixelHlsl(const PixelKey& pk) {
    std::string source = PixelHlsl(pk);
    if (!(pk.tex[0].flags & kTexGlowSmooth)) return source;
    constexpr const char* call = "float4 t0 = spreadtap(";
    size_t at = source.find(call);
    if (at == std::string::npos || source.find(call, at + 1) != std::string::npos) {
        LOGE("d3d12: final glow sample not found in translated shader");
        return {};
    }
    source.replace(at, std::strlen(call), "float4 t0 = glowtap(");
    return R"(
float4 glowtap(Texture2D t, SamplerState s, float2 uv, float4 w, float m) {
    float2 dx = ddx(uv), dy = ddy(uv);
    const float weight[5] = {1, 4, 6, 4, 1};
    float4 sum = 0;
    [unroll] for (int y = -2; y <= 2; ++y)
        [unroll] for (int x = -2; x <= 2; ++x) {
            float4 v = t.SampleGrad(s, uv + w.xy * float2(x, y), dx, dy);
            sum += float4(saturate(v.rgb * m), v.a) * weight[x + 2] * weight[y + 2];
        }
    return sum / 256;
}
)" + source;
}

// c-38, the viewport scale; its w the plane d3d8.cpp NearDepth set, 0 without
constexpr uint32_t kViewportScaleRow = 58;

bool NearDepth(const State& s) { return s.vsc[kViewportScaleRow][3] > 0; }

// The pixel shader writes the depth. Past the near plane it is the rasterizer's, squeezed behind a small front share
// of the range; nearer, the parts the plane would clip fill that share linearly in view depth (pos.w), in order
std::string NearDepthHlsl(std::string src) {
    const std::string head = "float4 main(VOut i, bool front : SV_IsFrontFace) : SV_Target {\n";
    const std::string with_depth =
        "float4 main(VOut i, bool front : SV_IsFrontFace, out float depth : SV_Depth) : SV_Target {\n"
        "    const float front_share = 1.0 / 4096;\n"
        "    float plane = c[" + std::to_string(kViewportScaleRow) + "].w;\n"
        "    float past = front_share + (1 - front_share) * i.pos.z;\n"
        "    depth = i.pos.w < plane ? front_share * i.pos.w / plane : past;\n";
    size_t at = src.find(head);
    if (at == std::string::npos || src.find(head, at + 1) != std::string::npos) {
        LOGE("d3d12: near-depth entry point not found in translated shader");
        return {};
    }
    src.replace(at, head.size(), with_depth);
    return src;
}

// Compilation and diagnostic captures use the same composed local shader variants.
std::string DrawPixelHlsl(const PixelKey& pk, bool near_depth) {
    std::string src = GlowPixelHlsl(pk);
    return near_depth ? NearDepthHlsl(std::move(src)) : src;
}

// sets sh.ph and sh.ps_code, compiling the shader on a cache miss
void D3D12Backend::EnsurePixelShader(const PixelKey& pk, DrawShaders& sh) {
    // pixel shaders share the map with vertex shaders, the salt keeps their keys apart
    auto pit = shader_src_.find(sh.pkey ^ kPixelKeySalt);
    if (pit != shader_src_.end()) {
        sh.ph = pit->second;
        sh.ps_code = blobs_[sh.ph].Get();
        return;
    }
    const auto source = sh.particle_samples ? DofParticlePixelSource(sh.particle_samples, sh.particle_blend,
        sh.particle_alpha_function, pk.alphakill != 0, sh.particle_pass, particle_float_bounds_) : DrawPixelHlsl(pk, sh.near_depth);
    sh.ps_code = sh.particle_samples ? CompileDof(source, "main", "ps_5_0", sh.ph) :
                                      Compile(source, "main", "ps_5_0", sh.ph);
    shader_src_[sh.pkey ^ kPixelKeySalt] = sh.ph;
}

void KeyBlend(const State& s, PsoKey& k) {
    k.blend = SimpleState(s, kMBlendEnable) != 0;
    if (k.blend) {
        k.src = SimpleState(s, kMSrcBlend);
        k.dst = SimpleState(s, kMDestBlend);
        k.op = SimpleState(s, kMBlendOp);
    }
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    // console ARGB bytes to D3D12 RGBA bits
    k.wmask = ((cw >> 16) & 1) | ((cw >> 8) & 1) << 1 | (cw & 1) << 2 | ((cw >> 24) & 1) << 3;
}

constexpr float kDepth24Max = 16777215.0f;  // 2^24 minus 1: a 24 bit depth buffer's units per 1.0

// stencil bit 7 marks a blended or alpha tested surface for the occlusion pass; the game never tests stencil
constexpr uint32_t kAoStencilBit = 0x80;
constexpr uint32_t kStencilAlways = 0x207;    // D3DCMP_ALWAYS
constexpr uint32_t kStencilKeep = 0x1E00;     // D3DSTENCILOP_KEEP
constexpr uint32_t kStencilReplace = 0x1E01;  // D3DSTENCILOP_REPLACE
constexpr uint32_t kAlphaKillEnable = 4;      // D3DTALPHAKILL_ENABLE

bool D3D12Backend::MarksOcclusion(const PsoKey& k) const {
    if (ao_frame_ == 0 || ao_frame_ != presents_ || k.sten || !k.zen || !k.zwrite) return false;
    return bound_ds_ && bound_ds_->gw == guest_w_ && bound_ds_->gh == guest_h_;
}

void KeyOcclusionMark(PsoKey& k) {
    k.ao_mark = 1;
    k.sten = 1;
    k.sfunc = kStencilAlways;
    k.swmask = kAoStencilBit;
    k.sfail = kStencilKeep;
    k.szfail = kStencilKeep;
    k.spass = kStencilReplace;
}

bool Translucent(const State& s, const PsoKey& k) {
    if (k.blend || SimpleState(s, kMAlphaTestEnable)) return true;
    for (int i = 0; i < 4; i++) {
        if (s.textures[i] && s.tss[size_t(i)][kTssAlphaKill] == kAlphaKillEnable) return true;
    }
    return false;
}

void D3D12Backend::KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k) {
    k.zen = s.rs[kRsZEnable] != 0;
    if (k.zen) {
        k.zwrite = SimpleState(s, kMZWriteEnable) != 0;
        k.zfunc = SimpleState(s, kMZFunc);
    }
    k.sten = s.rs[kRsStencilEnable] != 0;
    if (k.sten) {
        k.sfunc = SimpleState(s, kMStencilFunc);
        k.smask = SimpleState(s, kMStencilMask) & 0xFF;
        k.swmask = SimpleState(s, kMStencilWriteMask) & 0xFF;
        k.sfail = s.rs[kRsStencilFail];
        k.szfail = SimpleState(s, kMStencilZFail);
        k.spass = SimpleState(s, kMStencilPass);
    }
    if (MarksOcclusion(k)) KeyOcclusionMark(k);
    // polygon offset: bias in depth buffer units, the target is always 24 bit; the slope is per console pixel,
    // and an upscaled target's pixels are smaller
    if (a.topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable)) {
        k.zbias = int32_t(std::lround(Bits(SimpleState(s, kMDepthOffset)) * kDepth24Max / s.zscale));
        k.zslope = Bits(SimpleState(s, kMSlopeScale)) * (bound_ds_ ? float(bound_ds_->w) / bound_ds_->gw : 1.0f);
    }
}

void KeyRasterizer(const State& s, const Assembled& a, PsoKey& k) {
    uint32_t cull = s.rs[kRsCullMode];
    k.cull = cull != 0 && a.topology == Topology::Triangles;
    k.ccw = cull == kCullCw;  // culling clockwise leaves counter clockwise as the front
    k.fill = s.rs[kRsFillMode];
    k.unclipped = NearDepth(s);
}

void D3D12Backend::BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k) {
    std::memset(&k, 0, sizeof(k));
    k.vs = vh;
    k.ps = ph;
    k.regs = a.regs;
    for (int r = 0; r < 16; r++) {
        if ((a.regs >> r) & 1) {
            const VertexElement& e = a.elements[size_t(r)];
            k.elements[r] = e.format | uint32_t(e.buffer) << 8 | uint32_t(e.offset) << 16;
        }
    }
    k.topo = uint32_t(a.topology);
    k.has_rt = bound_rt_ != nullptr;
    k.has_ds = bound_ds_ != nullptr;
    k.rtfmt = bound_rt_ ? bound_rt_->fmt : 0;
    uint32_t samples = Ms(bound_rt_ ? *bound_rt_ : *bound_ds_);
    k.msaa = samples > 1 ? samples : 0;
    KeyBlend(s, k);
    if (k.has_ds) KeyDepthStencil(s, a, k);
    KeyRasterizer(s, a, k);
}

void FillFogConstants(const State& s, DrawConstants& c) {
    if (!s.rs[kRsFogEnable]) return;
    c.fog[0] = Bits(s.rs[kRsFogStart]);
    c.fog[1] = Bits(s.rs[kRsFogEnd]);
    c.fog[2] = Bits(s.rs[kRsFogDensity]);
    c.fog[3] = float(s.rs[kRsFogTableMode]);
}

void FillSurfaceConstants(const State& s, GuestExtent size, DrawConstants& c) {
    c.surf[0] = float(size.w);
    c.surf[1] = float(size.h);
    c.surf[2] = 1.0f / s.zscale;
    Color(s.rs[kRsFogColor], c.fog_color);
    c.misc[0] = float(SimpleState(s, kMAlphaRef) & 0xFF);
    c.misc[1] = float(EffectiveAlphaFunction(s));
    c.misc[2] = float(Cmp(s.rs[kRsShadowFunc]) - 1);
    c.misc[3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;
}

void FillCombinerConstants(const State& s, DrawConstants& c) {
    for (int i = 0; i < 8; i++) {
        Color(SimpleState(s, kMPsConstant0 + i * 4), c.pc[i * 2]);
        Color(SimpleState(s, kMPsConstant1 + i * 4), c.pc[i * 2 + 1]);
    }
    Color(SimpleState(s, kMPsFinalConstant0), c.pc[16]);
    Color(SimpleState(s, kMPsFinalConstant1), c.pc[17]);
    Color(s.rs[kRsTextureFactor], c.tfactor);
}

constexpr float kGlowBoxTexels = 3.0f;  // console texels across the box each wide tap averages
constexpr float kBlurTapTexels = 7.0f / 6;  // console texels between a blur's taps

void D3D12Backend::FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c) {
    c.texsize[i][0] = float(d.width);
    c.texsize[i][1] = float(d.height);
    if (!stages.rt[i]) return;
    float k = stages.k[i];
    if (stages.info[i].flags & kTexGlowSmooth) {
        constexpr float kExtraGlowBlur = 2.27f;
        c.spread[i][0] = kGlowBoxTexels * kExtraGlowBlur / (4 * float(d.width) * k);
        c.spread[i][1] = kGlowBoxTexels * kExtraGlowBlur / (4 * float(d.height) * k);
        return;
    }
    // a 3 console texel box per tap keeps the console chain's glow width and brightness; a blur's taps
    // are 7/6 texel apart, so each covers that much, a sample per host texel
    if (stages.wide[i] > 0) {
        bool blur = stages.blur[i];
        float fw = blur ? kBlurTapTexels : kGlowBoxTexels;
        float per_texel = blur ? 1.0f : 0.5f;
        float n = std::ceil(fw * stages.wide[i] * per_texel);
        c.spread[i][0] = fw / (n * float(d.width) * k);
        c.spread[i][1] = fw / (n * float(d.height) * k);
        c.spread[i][2] = n;
    }
}

void D3D12Backend::FillTextureConstants(const State& s, const DrawShaders& sh, const DrawTextures& stages,
                                        DrawConstants& c) {
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (s.textures[size_t(i)] && Describe(s.textures[size_t(i)], d)) FillTexelConstants(i, d, stages, c);
        if (stages.rt[i]) c.rtshift[i] = RenderTargetShift(s, sh);
        const auto& t = s.tss[size_t(i)];
        c.bump[i][0] = Bits(t[kTssBumpEnvMat00]);
        c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
        c.bump[i][2] = Bits(t[kTssBumpEnvMat10]);
        c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
        c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]);
        c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
    }
}

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) {
            o[r * 4 + col] = x[r * 4] * y[col] + x[r * 4 + 1] * y[4 + col] + x[r * 4 + 2] * y[8 + col] +
                             x[r * 4 + 3] * y[12 + col];
        }
    }
}

// HLSL mul(row vector, matrix) with column major packing wants the transpose
void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) o[col * 4 + r] = m[r * 4 + col];
    }
}

void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0 ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; r++) {
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3;
            int r2 = (r + 2) % 3;
            int c1 = (col + 1) % 3;
            int c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
    }
}

// D3DTRANSFORMSTATETYPE indices into s.transforms
constexpr size_t kTsView = 2;
constexpr size_t kTsProjection = 3;
constexpr size_t kTsTexture0 = 16;
constexpr size_t kTsWorld = 256;

void FillTransformConstants(const State& s, GuestExtent size, DrawConstants& c) {
    uint32_t gw = size.w;
    uint32_t gh = size.h;
    // world, view, projection, then the viewport inside the target
    float wv[16];
    float wvp[16];
    MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
    MatMul(wv, s.transforms[kTsProjection].data(), wvp);
    const auto& v = s.viewport;
    float sx = float(v.w) / gw;
    float sy = float(v.h) / gh;
    float ox = (2.0f * v.x + v.w) / gw - 1;
    float oy = 1 - (2.0f * v.y + v.h) / gh;
    float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
    float full[16];
    MatMul(wvp, vpm, full);
    Transpose(full, c.wvp);
    Transpose(wv, c.wv);
    // normals take the inverse transpose of the upper 3x3
    float n[16] = {};
    InverseTranspose3x3(wv, n);
    Transpose(n, c.wvn);
    for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), c.tm[t]);
}

// D3DLIGHT8 as floats: Type, Diffuse[4], Specular[4], Ambient[4], Position[3], Direction[3], Range, Falloff,
// Attenuation0 to 2, Theta, Phi
constexpr int kLightFloats = 26;
constexpr int kLightDiffuse = 1;
constexpr int kLightSpecular = 5;
constexpr int kLightAmbient = 9;
constexpr int kLightPosition = 13;
constexpr int kLightDirection = 16;
constexpr int kLightRange = 19;
constexpr int kLightFalloff = 20;
constexpr int kLightAttenuation0 = 21;
constexpr int kLightAttenuation1 = 22;
constexpr int kLightAttenuation2 = 23;
constexpr int kLightTheta = 24;
constexpr int kLightPhi = 25;

void LightToView(const std::array<uint32_t, kLightFloats>& l, const float* vm, float (&o)[7][4]) {
    float f[kLightFloats];
    std::memcpy(f, l.data(), sizeof(f));
    for (int j = 0; j < 4; j++) {
        o[0][j] = f[kLightDiffuse + j];
        o[1][j] = f[kLightSpecular + j];
        o[2][j] = f[kLightAmbient + j];
    }
    const float* pos = f + kLightPosition;
    const float* dir = f + kLightDirection;
    for (int j = 0; j < 3; j++) {
        o[3][j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
        o[4][j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
    }
    o[3][3] = f[kLightRange];
    o[4][3] = f[kLightFalloff];
    o[5][0] = f[kLightAttenuation0];
    o[5][1] = f[kLightAttenuation1];
    o[5][2] = f[kLightAttenuation2];
    o[6][0] = std::cos(f[kLightTheta] * 0.5f);
    o[6][1] = std::cos(f[kLightPhi] * 0.5f);
}

void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c) {
    std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
    c.mat[4][0] = s.material[16];
    Color(s.rs[kRsAmbient], c.amb);
    // lights to view space
    const float* vm = s.transforms[kTsView].data();
    for (int i = 0; i < 8; i++) {
        if (!((fk.types >> (i * 2)) & 3)) continue;
        LightToView(s.lights[size_t(i)], vm, c.lt[i]);
    }
}

float D3D12Backend::TargetScale() const {
    const Target* t = bound_rt_ ? bound_rt_ : bound_ds_;
    if (!t || !t->gw) return 1;
    return float(t->w) / float(t->gw);
}

// the XDK's screen space offset puts the console's positions half a pixel right and down of the host's, unless a
// program writes them without c-37; render target reads move back that far (TexUv)
float D3D12Backend::RenderTargetShift(const State& s, const DrawShaders& sh) const {
    bool offset = !sh.prog || ReadsViewportOffset(s.vs->program);
    return offset ? -0.5f * TargetScale() : 0.0f;
}

std::array<D3D12_GPU_VIRTUAL_ADDRESS, 2> D3D12Backend::UploadConstants(const State& s, const DrawShaders& sh,
    const PsoKey& k, const DrawTextures& stages, GuestExtent size, DrawConstants& c) {
    const bool visibility = sh.particle_samples && particle_dof_.visibility_active;
    static_assert(sizeof(s.vsc) == 192 * 16);
    const size_t visibility_bytes = visibility ? 16 + kParticleDofMaxQuads * sizeof(ParticleDofVisibilityMap) : 0;
    Alloc vc = Upload(sizeof(s.vsc) + visibility_bytes, 256);
    std::memcpy(vc.cpu, s.vsc.data(), sizeof(s.vsc));
    static_assert(sizeof(DrawConstants) == 2096);
    static_assert(sizeof(DrawConstants) + sizeof(ParticleDofGlobals) +
        kParticleDofMaxQuads * sizeof(ParticleDofQuad) <= 65536);
    const size_t tail = sh.particle_samples ? sizeof(ParticleDofGlobals) + particle_dof_.quads.size() * sizeof(ParticleDofQuad) : 0;
    Alloc dc = Upload(sizeof(DrawConstants) + tail, 256);
    FillSurfaceConstants(s, size, c);
    FillFogConstants(s, c);
    FillCombinerConstants(s, c);
    if (stages.final_glow) {
        // RGB only: preserve the original alpha and the scene underneath the glow.
        for (int i = 0; i < 3; i++) c.pc[0][i] *= 0.65f;
    }
    if (ext_) ext_->Constants(reinterpret_cast<float(*)[4]>(vc.cpu), c.ext, bound_rt_, k.blend ? k.src : 1);
    if (visibility) {
        auto* tail_ptr = static_cast<uint8_t*>(vc.cpu) + sizeof(s.vsc);
        std::memcpy(tail_ptr, particle_dof_.visibility_target, 16);
        std::memset(tail_ptr + 16, 0xff, visibility_bytes - 16);
        std::memcpy(tail_ptr + 16, particle_dof_.visibility_maps.data(),
            particle_dof_.visibility_maps.size() * sizeof(ParticleDofVisibilityMap));
    }
    FillTextureConstants(s, sh, stages, c);
    if (sh.fixed_transform) {
        FillTransformConstants(s, size, c);
        FillLightConstants(s, sh.fk, c);
    }
    std::memcpy(dc.cpu, &c, sizeof(c));
    if (sh.particle_samples) {
        auto* tail_ptr = static_cast<uint8_t*>(dc.cpu) + sizeof(c);
        std::memcpy(tail_ptr, &particle_dof_.globals, sizeof(ParticleDofGlobals));
        std::memcpy(tail_ptr + sizeof(ParticleDofGlobals), particle_dof_.quads.data(),
                    particle_dof_.quads.size() * sizeof(ParticleDofQuad));
    }
    cl_->SetGraphicsRootConstantBufferView(0, vc.gpu);
    cl_->SetGraphicsRootConstantBufferView(1, dc.gpu);
    return {vc.gpu, dc.gpu};
}

void D3D12Backend::CaptureTextures(const State& s, const DrawTextures& stages) {
    for (int i = 0; i < 4; i++) {
        ID3D12Resource* res = stages.res[i];
        TextureDesc d;
        if (!s.textures[size_t(i)] || !Describe(s.textures[size_t(i)], d)) continue;
        bool tg = targets_.count(TargetMapKey(d.data, Format(d.format).depth));
        fprintf(cap_, " | t%d %08X fmt %X %ux%u%s%s", i, d.data, d.format, d.width, d.height, tg ? " target" : "",
                res ? "" : " unbound");
        if (res && !tg && stages.info[i].kind == kTex2D && cap_seen_.insert(d.data).second) {
            cap_tex_.push_back({res, stages.sv[i].Format, d.data, d.width, d.height});
        }
    }
}

// s.vsc holds the 192 vertex shader constants in hardware order: D3D register + 96
constexpr int kVsConstants = 192;
constexpr int kVsConstantBias = 96;

void D3D12Backend::CaptureInputs(const State& s, const Assembled& a, const DrawConstants& c) {
    fprintf(cap_, "\n     verts");
    uint32_t n = 0;
    for (int r = 0; r < 16; r++) n += (a.regs >> r) & 1;
    for (size_t v = 0; v < std::min<size_t>(4, n ? a.vertices.size() / (n * 4) : 0); v++) {
        fprintf(cap_, " [");
        for (uint32_t k = 0; k < n * 4; k++) fprintf(cap_, " %g", a.vertices[v * n * 4 + k]);
        fprintf(cap_, " ]");
    }
    for (int r = 0; r < kVsConstants; r++) {
        const auto& v = s.vsc[r];
        if (v[0] || v[1] || v[2] || v[3]) {
            fprintf(cap_, "\n     c%d %g %g %g %g", r - kVsConstantBias, v[0], v[1], v[2], v[3]);
        }
    }
    fprintf(cap_, "\n     pc");
    for (size_t r = 0; r < std::size(c.pc); r++) {
        fprintf(cap_, " (%.3f %.3f %.3f %.3f)", c.pc[r][0], c.pc[r][1], c.pc[r][2], c.pc[r][3]);
    }
}

// the draw's line: its state, then its textures
void D3D12Backend::CaptureDraw(const State& s, uint32_t prim, uint32_t count, const PsoKey& k, const DrawConstants& c,
                               const DrawTextures& stages) {
    fprintf(cap_,
            "%04u draw rt %08X ds %08X prim %u count %u vs %08X ps %08X blend %u %X %X zen %u %X zwrite %u zbias %d %.2f cull %u cw %X c0 %.3f %.3f %.3f %.3f",
            cap_i_++, s.rt.data, bound_ds_ ? s.zs.data : 0, prim, count, s.vs_handle, s.ps ? s.ps->handle : 0, k.blend,
            k.src, k.dst, k.zen, k.zfunc, k.zwrite, k.zbias, k.zslope, k.cull, k.wmask, c.pc[0][0], c.pc[0][1],
            c.pc[0][2], c.pc[0][3]);
    CaptureTextures(s, stages);
}

// small draws: their inputs and constants; then ends the draw's line
void D3D12Backend::EndDrawCapture(const State& s, const Assembled& a, uint32_t count, const DrawConstants& c) {
    if (count <= kQuadVertices) CaptureInputs(s, a, c);
    fputc('\n', cap_);
}

// each shader once, as translated
void D3D12Backend::CaptureShader(const char* kind, uint64_t key, const std::string& src) {
    if (!cap_sh_.insert(key).second) return;
    char name[48];
    snprintf(name, sizeof(name), "%s_%016llX.hlsl", kind, (unsigned long long)key);
    if (FILE* f = _wfopen((cap_dir_ / name).c_str(), L"w")) {
        fwrite(src.data(), 1, src.size(), f);
        fclose(f);
    }
    fprintf(cap_, "     %s\n", name);
}

void D3D12Backend::CaptureShaders(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk) {
    CaptureShader("vs", sh.vkey, VertexHlsl(s, a, sh));
    const auto source = sh.particle_samples ? DofParticlePixelSource(sh.particle_samples, sh.particle_blend,
        sh.particle_alpha_function, pk.alphakill != 0, sh.particle_pass, particle_float_bounds_) : DrawPixelHlsl(pk, sh.near_depth);
    CaptureShader("ps", sh.pkey, source);
}

D3D12_SRV_DIMENSION NullViewDimension(uint8_t kind) {
    if (kind == kTexCube) return D3D12_SRV_DIMENSION_TEXTURECUBE;
    if (kind == kTex3D) return D3D12_SRV_DIMENSION_TEXTURE3D;
    return D3D12_SRV_DIMENSION_TEXTURE2D;
}

// a stage without a texture gets a null view of its kind; the samplers read the views after
void D3D12Backend::SetTextureViewsAndSamplers(const State& s, DrawTextures& stages, bool clamp) {
    uint32_t slot = AllocSrvSlots(4);
    for (int i = 0; i < 4; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC& sv = stages.sv[i];
        if (!stages.res[i]) {
            sv.ViewDimension = NullViewDimension(stages.info[i].kind);
            sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        }
        dev_->CreateShaderResourceView(stages.res[i], &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    cl_->SetGraphicsRootDescriptorTable(2, Gpu(srv_heap_.Get(), slot, srv_inc_));
    uint32_t samplers = EnsureStageSamplers(s, stages, clamp);
    cl_->SetGraphicsRootDescriptorTable(3, Gpu(smp_heap_.Get(), samplers, smp_inc_));
}

// viewport covers the target, the guest viewport is in the shader; scissor clips to it
void D3D12Backend::SetViewportScissor(const State& s, uint32_t gw, bool weapon_tiles) {
    float W = float(bound_rt_ ? bound_rt_->w : bound_ds_->w);
    float H = float(bound_rt_ ? bound_rt_->h : bound_ds_->h);
    D3D12_VIEWPORT vp{0, 0, W, H, 0, 1};
    // Exact native viewport / 8, not the rounded-up tile resource extent.
    if (weapon_tiles) { vp.Width /= kViewmodelDofTileSize; vp.Height /= kViewmodelDofTileSize; }
    cl_->RSSetViewports(1, &vp);
    float sc = W / gw;
    LONG x0 = LONG(s.viewport.x * sc);
    LONG y0 = LONG(s.viewport.y * sc);
    LONG x1 = LONG((s.viewport.x + s.viewport.w) * sc);
    LONG y1 = LONG((s.viewport.y + s.viewport.h) * sc);
    if (s.scissor_count) {
        x0 = std::max(x0, LONG(s.scissors[0] * sc));
        y0 = std::max(y0, LONG(s.scissors[1] * sc));
        x1 = std::min(x1, LONG(s.scissors[2] * sc));
        y1 = std::min(y1, LONG(s.scissors[3] * sc));
    }
    D3D12_RECT rect{std::clamp<LONG>(x0, 0, LONG(W)), std::clamp<LONG>(y0, 0, LONG(H)),
                    std::clamp<LONG>(x1, 0, LONG(W)), std::clamp<LONG>(y1, 0, LONG(H))};
    if (weapon_tiles) {
        constexpr LONG tile = kViewmodelDofTileSize;
        rect = {rect.left / tile, rect.top / tile, (rect.right + tile - 1) / tile,
                (rect.bottom + tile - 1) / tile};
    }
    cl_->RSSetScissorRects(1, &rect);
}

void D3D12Backend::SetBlendFactorStencilRef(const State& s, const PsoKey& k) {
    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    cl_->OMSetBlendFactor(bf);
    uint32_t ref = SimpleState(s, kMStencilRef) & 0xFF;
    if (k.ao_mark) ref = Translucent(s, k) ? kAoStencilBit : 0;
    cl_->OMSetStencilRef(ref);
}

D3D_PRIMITIVE_TOPOLOGY PrimitiveTopology(const Assembled& a) {
    if (a.topology == Topology::Points) return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    if (a.topology == Topology::Lines) {
        return a.strip ? D3D_PRIMITIVE_TOPOLOGY_LINESTRIP : D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    }
    return a.strip ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

void D3D12Backend::UploadVertexBuffers(const Assembled& a) {
    D3D12_VERTEX_BUFFER_VIEW vbv[16];
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        Alloc vb = Upload(b.bytes, 16);
        std::memcpy(vb.cpu, b.src, b.bytes);
        vbv[i] = {vb.gpu, b.bytes, b.stride};
    }
    cl_->IASetVertexBuffers(0, a.buffer_count, vbv);
}

void D3D12Backend::DrawGeometry(const Assembled& a) {
    UploadVertexBuffers(a);
    cl_->IASetPrimitiveTopology(PrimitiveTopology(a));
    if (a.index_count) {
        uint32_t ibytes = a.index_count * (a.index16 ? 2 : 4);
        Alloc ib = Upload(ibytes, 4);
        std::memcpy(ib.cpu, a.index_src, ibytes);
        D3D12_INDEX_BUFFER_VIEW ibv{ib.gpu, ibytes, a.index16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
        cl_->IASetIndexBuffer(&ibv);
        cl_->DrawIndexedInstanced(a.index_count, 1, 0, a.base_vertex, 0);
    } else {
        cl_->DrawInstanced(a.draw_count, 1, 0, 0);
    }
}

void D3D12Backend::RecordWeaponDofCoverage(const State& s, const Assembled& a, const DrawShaders& sh,
                                          const PsoKey& k, ID3D12PipelineState* native, uint32_t guest_width) {
    auto& weapon = weapon_dof_;
    if (!weapon.pending || !k.zen || !k.zwrite) return;
    weapon.encoding.Observe(particle_depth_serial_, bound_rt_ && bound_ds_ &&
        bound_rt_->res.Get() == weapon.rt && bound_ds_->res.Get() == weapon.ds && weapon.frame == frame_,
        sh.near_depth, s.vsc[kViewportScaleRow][3]);
    if (!weapon.region.valid) return;
    const bool supported = weapon.region.Complete(particle_depth_serial_) && bound_rt_ && bound_ds_ &&
        // SetViewportScissor uses this full-target host viewport; the guest
        // viewport affects VS coordinates/scissor, not the host viewport.
        WeaponDofViewportSupported(0, 0, float(bound_rt_->w), float(bound_rt_->h)) &&
        bound_rt_->res.Get() == weapon.rt && bound_ds_->res.Get() == weapon.ds &&
        weapon.frame == frame_ && a.topology == Topology::Triangles && k.fill != kFillWireframe &&
        !sh.particle_samples && sh.vs_code && weapon.coverage_ps && dof_pipelines_;
    if (!supported) { weapon.region.Invalidate(); return; }

    // Only VS/input-layout affect this PSO; all native color/depth/raster state
    // is intentionally replaced by conservative overcoverage with no culling.
    PsoKey key{};
    key.vs = k.vs; key.ps = weapon.coverage_hash; key.regs = k.regs;
    std::memcpy(key.elements, k.elements, sizeof(key.elements));
    D3D12_INPUT_ELEMENT_DESC elements[16];
    auto desc = FullScreenPipelineDesc(sh.vs_code, weapon.coverage_ps.Get(), DXGI_FORMAT_R8_UNORM);
    desc.InputLayout = {elements, FillInputLayout(key, elements)};
    desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON;
    desc.RasterizerState.DepthClipEnable = FALSE;
    auto* coverage = dof_pipelines_->Request(Hash(&key, sizeof(key), 0x574541504F4E524Full),
                                            desc, sh.vs_code, weapon.coverage_ps.Get());
    if (!weapon.region.AcceptDraw(particle_depth_serial_, coverage != nullptr)) return;

    auto timing = dof_timing_.Measure(DofGpuTiming::Stage::WeaponClassify, cl_.Get());
    // DrawGeometry just bound the exact native IA buffers and constants. Reuse
    // them without another upload; no textures or root parameters change.
    const auto mask_rtv = WeaponDofTileView();
    cl_->OMSetRenderTargets(1, &mask_rtv, FALSE, nullptr);
    cl_->SetPipelineState(coverage);
    SetViewportScissor(s, guest_width, true);
    if (a.index_count) cl_->DrawIndexedInstanced(a.index_count, 1, 0, a.base_vertex, 0);
    else cl_->DrawInstanced(a.draw_count, 1, 0, 0);

    const auto native_rtv = Cpu(rtv_heap_.Get(), bound_rt_->view, rtv_inc_);
    const auto native_dsv = Cpu(dsv_heap_.Get(), bound_ds_->view, dsv_inc_);
    cl_->OMSetRenderTargets(1, &native_rtv, FALSE, &native_dsv);
    cl_->SetPipelineState(native);
    SetViewportScissor(s, guest_width);
}

bool D3D12Backend::PrepareParticleSharedCoverage(uint32_t width, uint32_t height) {
    auto& p = particle_dof_;
    if (p.shared[0] && p.shared[1] && p.shared_width == width && p.shared_height == height) return true;
    // Only normal loading preparation may allocate these optional targets or replace
    // their persistent RTVs. A later mismatched target uses the existing per-quad path.
    if (!dof_resources_quiescent_ || !width || !height) return false;
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R32G32_FLOAT};
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> resources[2];
    for (uint32_t i = 0; i < 2; ++i) {
        auto desc = Texture2DDesc(formats[i], width, height);
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        desc.SampleDesc.Count = i == 0 ? 2 : 1;
        if (FAILED(dev_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&resources[i])))) return false;
    }
    for (uint32_t i = 0; i < 2; ++i) {
        if (p.shared[i]) Release(std::move(p.shared[i]));
        p.shared[i] = std::move(resources[i]);
        p.shared_states[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        dev_->CreateRenderTargetView(p.shared[i].Get(), nullptr, ParticleSharedView(i));
    }
    p.shared_width = width; p.shared_height = height;
    LOGR("Particle DOF shared coverage prepared: %ux%u support 2x R8, transmission RG32F", width, height);
    return true;
}

bool D3D12Backend::EnsureParticleDofVisibility(uint32_t extent) {
    auto& p = particle_dof_;
    if (p.visibility && p.visibility_extent >= extent) return true;
    const uint32_t failure = extent > 2048 ? 2u : 1u;
    if (!particle_visibility_cache_ || (p.visibility_failed & failure)) return false;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    auto desc = Texture2DDesc(DXGI_FORMAT_R16_FLOAT, extent, extent);
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> next;
    if (FAILED(dev_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&next)))) {
        p.visibility_failed |= failure;
        return false;
    }
    if (p.visibility && !dof_resources_quiescent_) Flush();
    if (p.visibility) Release(std::move(p.visibility));
    p.visibility = std::move(next);
    p.visibility_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    p.visibility_extent = extent;
    dev_->CreateRenderTargetView(p.visibility.Get(), nullptr, ParticleVisibilityView());
    const float zero[4] = {};
    cl_->ClearRenderTargetView(ParticleVisibilityView(), zero, 0, nullptr);
    return true;
}

bool D3D12Backend::EnsureParticleDofAtlas(uint32_t extent) {
    auto& p = particle_dof_;
    if (p.atlas[0] && p.atlas_extent == extent) return true;
    const uint32_t failure = extent == 2048 ? 1 : 2;
    if (p.atlas_failed & failure) return false;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    auto desc = Texture2DDesc(DXGI_FORMAT_R16G16B16A16_FLOAT, extent, extent);
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> atlas[2];
    for (auto& target : atlas) {
        if (FAILED(dev_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)))) {
            p.atlas_failed |= failure;
            LOGR("Particle DOF: %ux%u atlas allocation failed; retaining direct aperture filtering", extent, extent);
            return false;
        }
    }
    // Replacing an RTV must wait for previous draws that reference its old descriptor/resource.
    // The fixed pages otherwise survive target resizes and are reused without a CPU wait.
    if (p.atlas[0] && !dof_resources_quiescent_) Flush();
    for (uint32_t i = 0; i < 2; ++i) {
        if (p.atlas[i]) Release(std::move(p.atlas[i]));
        p.atlas[i] = std::move(atlas[i]);
        p.atlas_states[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        dev_->CreateRenderTargetView(p.atlas[i].Get(), nullptr, ParticleDofAtlasView(i));
    }
    p.atlas_extent = extent;
    return true;
}

bool D3D12Backend::DrawParticleDofAtlas(const State& s, const Assembled& a, const PixelKey& pk,
    const DrawShaders& sh, DrawTextures& stages, const PsoKey& native_key, bool clamp) {
    auto& p = particle_dof_;
    const bool reuse_coverage = DofParticleCoverageReuseEnabled();
    p.visibility_active = false;
    Target* rt = bound_rt_;
    Target* ds = bound_ds_;
    uint32_t extent = p.atlas_extent ? p.atlas_extent : 2048;
    std::vector<ParticleDofAtlasPage> pages;
    std::vector<ParticleSharedCoverage> shared_pages;
    uint32_t mapped = 0;
    try {
        pages = BuildParticleDofAtlasPages(p.vertices, p.params, p.quads, extent, extent);
        const auto mapped_count = [&]() {
            return uint32_t(std::count_if(p.quads.begin(), p.quads.end(),
                [](const ParticleDofQuad& q) { return q.atlas_scale > 0; }));
        };
        mapped = mapped_count();
        // Only grow if the larger page actually recovers a quad that does not fit. Offscreen and
        // unsupported quads also have zero metadata; those alone must not allocate larger pages.
        if (extent == 2048 && mapped < p.draw_active && (rt->w > 2044 || rt->h > 2044) && !(p.atlas_failed & 2)) {
            auto small_metadata = p.quads;
            auto larger_pages = BuildParticleDofAtlasPages(p.vertices, p.params, p.quads, 4096, 4096);
            const uint32_t larger_count = mapped_count();
            if (larger_count > mapped) {
                extent = 4096; mapped = larger_count; pages = std::move(larger_pages);
            } else p.quads = std::move(small_metadata);
        }
        // No vector growth is allowed once an earlier chunk may already have been composited.
        p.atlas_rects.reserve(p.quads.size());
        // Preserve page-index alignment when disabled, without running admission
        // work or counting deliberately disabled pages as rejected candidates.
        shared_pages.resize(pages.size());
        if (reuse_coverage) for (size_t i = 0; i < pages.size(); ++i) {
            shared_pages[i] = BuildParticleSharedCoverage(p.vertices, p.quads, p.params, pages[i],
                                                         sh.particle_samples, sh.particle_coverage);
            ++p.shared_admission[size_t(shared_pages[i].rejection)];
        }
    } catch (const std::bad_alloc&) {
        if (!(p.reported & (1u << 9))) {
            p.reported |= 1u << 9;
            LOGR("Particle DOF: atlas metadata allocation failed; retaining direct aperture filtering");
        }
        return false;
    }
    if (!mapped || pages.empty()) return false;

    DrawShaders source = sh, composite = sh;
    source.particle_pass = 1;
    composite.particle_pass = 2;
    auto pipeline = [&](DrawShaders& variant, bool prepare) -> ID3D12PipelineState* {
        PixelKey pixel = pk;
        if (variant.particle_pass >= 3) {
            pixel.alphakill = 0;
            variant.particle_alpha_function = 0;
            if (variant.particle_pass != 4 && variant.particle_pass != 7) variant.particle_blend = 0;
        }
        DrawShaderKeys(s, a, pixel, variant);
        EnsureVertexShader(s, a, variant);
        EnsurePixelShader(pixel, variant);
        if (!variant.vs_code || !variant.ps_code) return nullptr;
        PsoKey key = native_key;
        key.vs = variant.vh; key.ps = variant.ph;
        if (prepare || variant.particle_pass == 3 || variant.particle_pass == 5 || variant.particle_pass == 6) {
            key.has_ds = key.zen = key.zwrite = key.sten = key.blend = key.msaa = 0;
            key.has_rt = 1;
            key.rtfmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
            key.wmask = D3D12_COLOR_WRITE_ENABLE_ALL;
            if (variant.particle_pass == 3) {
                key.rtfmt = DXGI_FORMAT_R32G32_FLOAT;
                key.cull = key.ccw = 0;
            } else if (variant.particle_pass == 5) {
                key.rtfmt = DXGI_FORMAT_R8_UNORM; key.msaa = 2;
            } else if (variant.particle_pass == 6) {
                key.rtfmt = DXGI_FORMAT_R16_FLOAT; key.cull = key.ccw = 0;
            }
        } else if (variant.particle_coverage) {
            key.zen = 0;  // the shader preserves native/focused sample rejection and softens defocused coverage
        }
        return EnsureParticlePipeline(key, variant.vs_code, variant.ps_code);
    };
    // Every fallible resource/pipeline operation precedes the first native-target composite.
    // A failure can therefore fall back to the complete original draw without double blending.
    auto* source_pso = pipeline(source, true);
    auto* composite_pso = pipeline(composite, false);
    auto* filter_pso = EnsurePassPipeline(kParticleDofAtlasFilterHlsl.c_str(), "vs", "filter_ps",
                                         DXGI_FORMAT_R16G16B16A16_FLOAT, -1, true);
    if (!source_pso || !composite_pso || !filter_pso) {
        if (!(p.reported & (1u << 8))) {
            p.reported |= 1u << 8;
            LOGR("Particle DOF: atlas pipeline pending/unavailable; using the ready direct or native path");
        }
        return false;
    }
    if (!EnsureParticleDofAtlas(extent)) return false;
    DrawShaders visibility_source = sh, visibility_composite = sh;
    visibility_source.particle_pass = 6; visibility_composite.particle_pass = 7;
    ID3D12PipelineState* visibility_source_pso = nullptr;
    ID3D12PipelineState* visibility_composite_pso = nullptr;
    std::vector<ParticleDofVisibilityPage> visibility_pages;
    const float largest_radius = p.globals.optics[1] * std::max(p.globals.optics[2], p.globals.optics[3]);
    if (reuse_coverage && particle_visibility_cache_ && sh.particle_samples == 2 && sh.particle_coverage &&
        native_key.fill != kFillWireframe && !native_key.zbias && !native_key.zslope &&
        std::isfinite(largest_radius) && largest_radius > 0 && largest_radius < 4096) {
        const uint32_t halo = uint32_t(std::ceil(largest_radius)) + 2;
        try {
            visibility_pages = BuildParticleDofVisibilityPages(pages, p.quads, p.globals, extent, halo, p.visibility_maps);
        } catch (const std::bad_alloc&) { visibility_pages.clear(); }
        if (std::any_of(visibility_pages.begin(), visibility_pages.end(),
            [](const auto& page) { return page.cached; }) && EnsureParticleDofVisibility(extent)) {
            visibility_source_pso = pipeline(visibility_source, true);
            visibility_composite_pso = pipeline(visibility_composite, false);
            if (visibility_source_pso && visibility_composite_pso) {
                try {
                    std::vector<ParticleDofAtlasPage> split_pages;
                    split_pages.reserve(visibility_pages.size());
                    std::vector<ParticleSharedCoverage> no_shared(visibility_pages.size());
                    for (const auto& page : visibility_pages)
                        split_pages.push_back({page.first_quad, page.quad_count});
                    pages = std::move(split_pages); shared_pages = std::move(no_shared);
                    p.visibility_active = true;
                    p.visibility_target[0] = p.visibility_target[1] = float(p.visibility_extent);
                    p.visibility_target[2] = float(halo); p.visibility_target[3] = 0;
                } catch (const std::bad_alloc&) { /* Keep the original ordered pages. */ }
            }
        }
    }
    DrawShaders support = sh, field = sh, shared_composite = sh;
    support.particle_pass = 5; field.particle_pass = 3; shared_composite.particle_pass = 4;
    ID3D12PipelineState* support_pso = nullptr;
    ID3D12PipelineState* field_pso = nullptr;
    ID3D12PipelineState* shared_pso = nullptr;
    const bool shared_candidate = reuse_coverage && std::any_of(shared_pages.begin(), shared_pages.end(),
        [](const ParticleSharedCoverage& plan) { return bool(plan); });
    if (shared_candidate && p.shared[0] && p.shared[1] && p.shared_width >= rt->w && p.shared_height >= rt->h) {
        support_pso = pipeline(support, false);
        field_pso = pipeline(field, false);
        shared_pso = pipeline(shared_composite, false);
    }
    const bool shared_ready = reuse_coverage && support_pso && field_pso && shared_pso;
    if (shared_candidate && !shared_ready) ++p.shared_unavailable;
    const auto restore_target = [&]() {
        const auto color = Cpu(rtv_heap_.Get(), rt->view, rtv_inc_);
        const auto depth = Cpu(dsv_heap_.Get(), ds->view, dsv_inc_);
        cl_->OMSetRenderTargets(1, &color, FALSE, &depth);
        bound_rt_ = rt; bound_ds_ = ds;
    };
    restore_target();  // Atlas growth can Flush and reset the bound-target cache.
    if (cap_) {
        CaptureShaders(s, a, source, pk);
        CaptureShaders(s, a, composite, pk);
        CaptureShader("particle_filter", Hash(kParticleDofAtlasFilterHlsl.data(), kParticleDofAtlasFilterHlsl.size()),
                      kParticleDofAtlasFilterHlsl);
    }

    float radii[kParticleDofMaxQuads]{};
    for (size_t q = 0; q < p.quads.size(); ++q) if (p.quads[q].atlas_scale > 0)
        radii[q] = ParticleDofRadius(1.0f / p.vertices[q * 4].position[2], p.params.globals) * p.quads[q].atlas_scale;
    Alloc radius_constants = Upload(sizeof(radii), 256);
    std::memcpy(radius_constants.cpu, radii, sizeof(radii));
    Alloc native_vertices = Upload(a.buffers[0].bytes, 16);
    std::memcpy(native_vertices.cpu, a.buffers[0].src, a.buffers[0].bytes);
    const D3D12_VERTEX_BUFFER_VIEW native_view{native_vertices.gpu, a.buffers[0].bytes, a.buffers[0].stride};
    cl_->IASetVertexBuffers(0, 1, &native_view);
    Alloc indices = Upload(uint64_t(a.index_count) * sizeof(uint32_t), 4);
    std::memcpy(indices.cpu, a.index_src, size_t(a.index_count) * sizeof(uint32_t));
    D3D12_INDEX_BUFFER_VIEW index_view{indices.gpu, a.index_count * uint32_t(sizeof(uint32_t)), DXGI_FORMAT_R32_UINT};
    cl_->IASetIndexBuffer(&index_view);

    DrawTextures filtered = stages;
    filtered.res[1] = p.atlas[1].Get();
    filtered.info[1] = TexInfo{kTex2D};
    filtered.sv[1] = {};
    filtered.sv[1].Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    filtered.sv[1].Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    filtered.sv[1].ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    filtered.sv[1].Texture2D.MipLevels = 1;
    filtered.rt[1] = false;
    filtered.wide[1] = 0; filtered.blur[1] = false; filtered.k[1] = 1;
    DrawTextures field_inputs = stages, shared_filtered = filtered;
    field_inputs.res[1] = p.shared[0].Get();
    field_inputs.info[1] = TexInfo{kTex2D};
    field_inputs.sv[1] = {};
    field_inputs.sv[1].Format = DXGI_FORMAT_R8_UNORM;
    field_inputs.sv[1].ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    field_inputs.sv[1].Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    field_inputs.rt[1] = false; field_inputs.wide[1] = 0; field_inputs.blur[1] = false; field_inputs.k[1] = 1;
    shared_filtered.res[2] = p.shared[1].Get();
    shared_filtered.info[2] = TexInfo{kTex2D};
    shared_filtered.sv[2] = filtered.sv[1];
    shared_filtered.sv[2].Format = DXGI_FORMAT_R32G32_FLOAT;
    shared_filtered.rt[2] = false; shared_filtered.wide[2] = 0; shared_filtered.blur[2] = false; shared_filtered.k[2] = 1;
    DrawTextures visibility_filtered = filtered;
    visibility_filtered.res[0] = stages.res[2];
    visibility_filtered.info[0] = TexInfo{kTex2D};
    visibility_filtered.sv[0] = stages.sv[2];
    visibility_filtered.rt[0] = false; visibility_filtered.wide[0] = 0;
    visibility_filtered.blur[0] = false; visibility_filtered.k[0] = 1;
    visibility_filtered.res[2] = p.visibility.Get();
    visibility_filtered.sv[2] = filtered.sv[1];
    visibility_filtered.sv[2].Format = DXGI_FORMAT_R16_FLOAT;
    visibility_filtered.rt[2] = false; visibility_filtered.wide[2] = 0;
    visibility_filtered.blur[2] = false; visibility_filtered.k[2] = 1;
    DrawTextures unfiltered = stages;
    unfiltered.res[0] = p.atlas[0].Get();
    unfiltered.info[0] = TexInfo{kTex2D};
    unfiltered.sv[0] = filtered.sv[1];
    GuestExtent size{};
    SurfaceSize(s.rt, size.w, size.h);
    p.globals.atlas_target[0] = p.globals.atlas_target[1] = float(extent);
    for (size_t page_index = 0; page_index < pages.size(); ++page_index) {
        const auto& page = pages[page_index];
        const auto& shared = shared_pages[page_index];
        const bool use_shared = shared_ready && bool(shared);
        const bool use_visibility = p.visibility_active && visibility_pages[page_index].cached;
        p.globals.atlas_target[2] = float(page.first_quad);
        p.globals.atlas_target[3] = float(page.quad_count);
        DrawConstants constants{};
        const auto cb = UploadConstants(s, source, native_key, stages, size, constants);
        if (cap_ && page.first_quad == 0) {
            CaptureDraw(s, 8, a.vertex_count, native_key, constants, stages);
            EndDrawCapture(s, a, a.vertex_count, constants);
        }
        p.atlas_rects.clear();
        for (uint32_t q = page.first_quad; q < page.first_quad + page.quad_count; ++q) {
            const auto& map = p.quads[q];
            if (map.atlas_scale <= 0) continue;
            const LONG x = LONG(map.atlas_xy & 0xffff), y = LONG(map.atlas_xy >> 16);
            p.atlas_rects.push_back({x, y, x + LONG(map.atlas_size & 0xffff), y + LONG(map.atlas_size >> 16)});
        }
        if (!p.atlas_rects.empty()) {
            auto source_timing = dof_timing_.Measure(DofGpuTiming::Stage::ParticleSource, cl_.Get());
            Transition(p.atlas[0].Get(), p.atlas_states[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
            SetBlitTarget(ParticleDofAtlasView(0), extent, extent);
            ClearParticleDofAtlasCells(cl_.Get(), ParticleDofAtlasView(0), p.atlas_rects);
            cl_->SetPipelineState(source_pso);
            SetTextureViewsAndSamplers(s, stages, clamp);
            cl_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cl_->DrawIndexedInstanced(page.quad_count * 6, 1, page.first_quad * 6, 0, 0);
            Transition(p.atlas[0].Get(), p.atlas_states[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            source_timing.End();
            auto filter_timing = dof_timing_.Measure(DofGpuTiming::Stage::ParticleFilter, cl_.Get());

            Transition(p.atlas[1].Get(), p.atlas_states[1], D3D12_RESOURCE_STATE_RENDER_TARGET);
            SetBlitTarget(ParticleDofAtlasView(1), extent, extent);
            cl_->SetPipelineState(filter_pso);
            cl_->SetGraphicsRootConstantBufferView(0, radius_constants.gpu);
            SetTextureViewsAndSamplers(s, unfiltered, clamp);
            cl_->SetGraphicsRootDescriptorTable(3, Gpu(smp_heap_.Get(), EnsureBlitSamplers(), smp_inc_));
            cl_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            // The procedural pass writes every texel of each allocated cell, including its zero guard.
            cl_->DrawInstanced(6, page.quad_count, 0, 0);
            Transition(p.atlas[1].Get(), p.atlas_states[1], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        if (use_visibility) {
            auto visibility_timing = dof_timing_.Measure(DofGpuTiming::Stage::ParticleTransmission, cl_.Get());
            Transition(p.visibility.Get(), p.visibility_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
            SetBlitTarget(ParticleVisibilityView(), p.visibility_extent, p.visibility_extent);
            cl_->SetPipelineState(visibility_source_pso);
            cl_->SetGraphicsRootConstantBufferView(0, cb[0]);
            cl_->SetGraphicsRootConstantBufferView(1, cb[1]);
            cl_->IASetVertexBuffers(0, 1, &native_view);
            cl_->IASetIndexBuffer(&index_view);
            cl_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            SetTextureViewsAndSamplers(s, stages, clamp);
            cl_->DrawIndexedInstanced(page.quad_count * 6, 1, page.first_quad * 6, 0, 0);
            Transition(p.visibility.Get(), p.visibility_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            ++p.visibility_pages; p.visibility_quads += page.quad_count;
            if (p.visibility_pages == 1)
                LOGR("Particle DOF native visibility cache active: %ux%u R16F, unchanged aperture samples",
                     p.visibility_extent, p.visibility_extent);
        }
        if (use_shared) {
            const D3D12_RECT box{shared.bounds[0], shared.bounds[1], shared.bounds[2], shared.bounds[3]};
            Alloc field_vertices = Upload(sizeof(shared.vertices), 16);
            std::memcpy(field_vertices.cpu, shared.vertices.data(), sizeof(shared.vertices));
            const D3D12_VERTEX_BUFFER_VIEW field_vb{field_vertices.gpu, UINT(sizeof(shared.vertices)), sizeof(NativeParticleVertex)};
            // A synthetic rectangle uses SV_VertexID/4==0. Only its representative
            // metadata is needed; native page IDs/metadata remain unchanged for final blends.
            auto globals = p.globals; globals.target[2] = 1;
            Alloc field_constants = Upload(sizeof(constants) + sizeof(globals) + sizeof(ParticleDofQuad), 256);
            auto* bytes = static_cast<uint8_t*>(field_constants.cpu);
            std::memcpy(bytes, &constants, sizeof(constants));
            std::memcpy(bytes + sizeof(constants), &globals, sizeof(globals));
            std::memcpy(bytes + sizeof(constants) + sizeof(globals), &p.quads[page.first_quad], sizeof(ParticleDofQuad));
            // Keep the original target's pixel-coordinate viewport even though scratch
            // resources were reserved at the largest native extent during loading.
            restore_target();
            SetViewportScissor(s, size.w);
            cl_->RSSetScissorRects(1, &box);
            auto support_timing = dof_timing_.Measure(DofGpuTiming::Stage::ParticleSupport, cl_.Get());
            Transition(p.shared[0].Get(), p.shared_states[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
            const auto mask_view = ParticleSharedView(0);
            cl_->OMSetRenderTargets(1, &mask_view, FALSE, nullptr);
            const float zero[4] = {};
            cl_->ClearRenderTargetView(mask_view, zero, 1, &box);
            cl_->SetPipelineState(support_pso);
            cl_->SetGraphicsRootConstantBufferView(0, cb[0]);
            cl_->SetGraphicsRootConstantBufferView(1, cb[1]);
            cl_->IASetVertexBuffers(0, 1, &native_view);
            cl_->IASetIndexBuffer(&index_view);
            cl_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            SetTextureViewsAndSamplers(s, filtered, clamp);
            cl_->DrawIndexedInstanced(page.quad_count * 6, 1, page.first_quad * 6, 0, 0);
            Transition(p.shared[0].Get(), p.shared_states[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            support_timing.End();

            auto field_timing = dof_timing_.Measure(DofGpuTiming::Stage::ParticleTransmission, cl_.Get());
            Transition(p.shared[1].Get(), p.shared_states[1], D3D12_RESOURCE_STATE_RENDER_TARGET);
            const auto field_view = ParticleSharedView(1);
            cl_->OMSetRenderTargets(1, &field_view, FALSE, nullptr);
            cl_->IASetVertexBuffers(0, 1, &field_vb);
            cl_->SetGraphicsRootConstantBufferView(1, field_constants.gpu);
            cl_->SetPipelineState(field_pso);
            SetTextureViewsAndSamplers(s, field_inputs, clamp);
            cl_->DrawIndexedInstanced(6, 1, 0, 0, 0);
            Transition(p.shared[1].Get(), p.shared_states[1], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cl_->IASetVertexBuffers(0, 1, &native_view);
            ++p.shared_pages;
            p.shared_sum_area += shared.sum_area; p.shared_union_area += shared.union_area;
        }
        auto composite_timing = dof_timing_.Measure(DofGpuTiming::Stage::ParticleComposite, cl_.Get());
        restore_target();
        cl_->SetPipelineState(use_visibility ? visibility_composite_pso : use_shared ? shared_pso : composite_pso);
        cl_->SetGraphicsRootConstantBufferView(0, cb[0]);
        cl_->SetGraphicsRootConstantBufferView(1, cb[1]);
        SetTextureViewsAndSamplers(s, use_visibility ? visibility_filtered : use_shared ? shared_filtered : filtered, clamp);
        SetViewportScissor(s, size.w);
        SetBlendFactorStencilRef(s, native_key);
        cl_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl_->DrawIndexedInstanced(page.quad_count * 6, 1, page.first_quad * 6, 0, 0);
    }
    ++p.atlas_batches;
    p.atlas_pages += pages.size();
    p.atlas_quads += mapped;
    p.atlas_fallback += p.draw_active - mapped;
    if (p.atlas_batches == 1)
        LOGR("Particle DOF atlas active: %ux%u pages, source visibility/material prefilter, "
             "64 prepared taps, ordered native composite", extent, extent);
    return true;
}

// calls the extension's EndDraw on every return from Draw once made
struct DrawEnd {
    Extension* e;
    ~DrawEnd() {
        if (e) e->EndDraw();
    }
};

// Assemble, timed and counted
bool D3D12Backend::AssembleDraw(const State& s, uint32_t prim, uint32_t start, uint32_t count,
                                const uint8_t* indices, Assembled& a) {
    auto perf_t = PerfClock::now();
    bool assembled = Assemble(s, prim, start, count, indices, a);
    g_perf.assemble += PerfMs(perf_t);
    g_perf.verts += a.vertex_count;
    g_perf.raw += a.vertices.empty();
    return assembled;
}

void D3D12Backend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    PerfScope perf_draw{g_perf.draw};
    g_perf.draws++;
    if (particle_dof_.marker && (prim != 8 || start || indices || count == 0 || count > 4096 || count % 4)) {
        particle_dof_.marker = false;
        ParticleDofSkipped(1, "marker did not precede a native quad draw");
    }
    if (cap_) CapStep(s.rt);
    if (DepthCopy(s)) return;
    DrawEnd draw_end{ext_};
    if (ext_) ext_->BeginDraw(s);
    Assembled& a = asm_;
    if (!AssembleDraw(s, prim, start, count, indices, a)) return;
    if (!BindTargets(s)) return;
    if (ext_ && ext_->Bound(*this, s, bound_rt_, bound_ds_, a.pretransformed) && !BindTargets(s)) return;

    // vertex shader
    DrawShaders sh = ClassifyShaders(s, a);
    if (sh.prog) EnsureProgramHash(s);
    // stage textures, then the pixel shader made for them
    DrawTextures stages;
    if (!BindStageTexturesAndTargets(s, stages)) return;
    PixelKey pk = MakePixelKey(s, stages.info);
    // The final glow is a quarter-size target added to the scene with RGB = t0 * pc0.
    TextureDesc glow;
    stages.final_glow = prim == 5 && count == 3 && bound_rt_ && bound_rt_->gw == guest_w_ &&
        bound_rt_->gh == guest_h_ && !s.rs[kRsZEnable] && pk.programmable && (pk.count & 0xFF) == 1 &&
        pk.tex_modes == 1 && !pk.alphakill && pk.rgb_in[0] == 0xC8C10000 && pk.rgb_out[0] == 0xC0 &&
        pk.final_abcd == 0x00000C00 && pk.final_efg == 0x00001180 &&
        SimpleState(s, kMBlendEnable) && SimpleState(s, kMSrcBlend) == 1 &&
        SimpleState(s, kMDestBlend) == 0x302 && BlendOp(SimpleState(s, kMBlendOp)) == D3D12_BLEND_OP_ADD &&
        stages.rt[0] && (SimpleState(s, kMPsFinalConstant0) >> 24) == 255 &&
        !s.textures[1] && !s.textures[2] && !s.textures[3] && Describe(s.textures[0], glow) &&
        glow.format == 0x12 && glow.width * 4 == guest_w_ && glow.height * 4 == guest_h_;
    if (stages.final_glow) {
        stages.info[0].flags |= kTexWide | kTexGlowSmooth;
        pk = MakePixelKey(s, stages.info);
    }
    sh.near_depth = NearDepth(s);
    const PixelKey native_pk = pk;
    // Preserve the native sampler heuristic: expanded UV support intentionally extends outside 0..1.
    const bool native_clamp = AnyStageWraps(s) && UnitTexcoords(a);
    const uint8_t* native_vertices = a.buffers[0].src;
    ID3D12Resource* native_t2 = stages.res[2];
    const auto native_t2_view = stages.sv[2];
    ID3D12Resource* native_t3 = stages.res[3];
    const auto native_t3_view = stages.sv[3];
    PrepareParticleDof(s, a, pk, sh, stages);
    if (sh.particle_samples) pk = CanonicalParticleDofPixelKey(pk);
    PsoKey k;
    if (sh.particle_samples) BuildPipelineKey(s, a, 0, 0, k);
    // The atlas composite already handles individual unpacked quads. Compile the separate
    // direct-aperture pipeline only if the complete batch cannot use the atlas.
    const bool atlas = sh.particle_samples && DrawParticleDofAtlas(s, a, pk, sh, stages, k, native_clamp);
    if (!atlas) {
        auto pipeline = [&]() -> ID3D12PipelineState* {
            DrawShaderKeys(s, a, pk, sh);
            EnsureVertexShader(s, a, sh);
            EnsurePixelShader(pk, sh);
            if (!sh.vs_code || !sh.ps_code) return nullptr;
            BuildPipelineKey(s, a, sh.vh, sh.ph, k);
            if (sh.particle_samples && sh.particle_coverage) k.zen = 0;
            return sh.particle_samples ? EnsureParticlePipeline(k, sh.vs_code, sh.ps_code) :
                                         EnsurePipeline(k, sh.vs_code, sh.ps_code);
        };
        ID3D12PipelineState* pso = pipeline();
        if (!pso && sh.particle_samples) {
            ParticleDofSkipped(6, "particle pipeline pending/unavailable");
            a.buffers[0].src = native_vertices;
            stages.res[2] = native_t2;
            stages.sv[2] = native_t2_view;
            stages.res[3] = native_t3;
            stages.sv[3] = native_t3_view;
            pk = native_pk;
            sh.particle_samples = 0;
            sh.particle_blend = 0;
            sh.particle_alpha_function = -1;
            sh.particle_coverage = false;
            pso = pipeline();
        }
        if (!pso) return;
        cl_->SetPipelineState(pso);

        GuestExtent size;
        SurfaceSize(bound_rt_ ? s.rt : s.zs, size.w, size.h);
        DrawConstants c{};
        UploadConstants(s, sh, k, stages, size, c);
        if (cap_) {
            CaptureDraw(s, prim, count, k, c, stages);
            EndDrawCapture(s, a, count, c);
            CaptureShaders(s, a, sh, pk);
        }
        // Ordinary draws retain native depth; verified particle variants implement it in SV_Coverage.
        SetTextureViewsAndSamplers(s, stages, native_clamp);
        SetViewportScissor(s, size.w);
        SetBlendFactorStencilRef(s, k);
        auto direct_timing = sh.particle_samples ? dof_timing_.Measure(DofGpuTiming::Stage::ParticleDirect, cl_.Get()) :
                                                   DofGpuTiming::Scope{};
        DrawGeometry(a);
        RecordWeaponDofCoverage(s, a, sh, k, pso, size.w);
    }
    if (bound_ds_ && k.zen && k.zwrite) InvalidateParticleDepth(bound_ds_->res.Get());
    if (sh.particle_samples) {
        particle_dof_.active += particle_dof_.draw_active;
        if (sh.particle_blend) {
            if (BlendOp(k.op) == D3D12_BLEND_OP_REV_SUBTRACT) particle_dof_.subtractive += particle_dof_.draw_active;
            else particle_dof_.additive += particle_dof_.draw_active;
        }
        if (!particle_dof_.active_reported) {
            particle_dof_.active_reported = true;
            LOGR("Particle DOF active: native D660/D6A0 late alpha quads, %ux%u target, %ux MSAA, "
                 "%s; early particles keep the world pass", bound_rt_->w, bound_rt_->h,
                 sh.particle_samples, atlas ? "prepared atlas aperture" : "direct source-visible aperture");
        }
    }
}

// a guest rect scaled to the target and clamped to it
D3D12_RECT ScaledRect(const Target& t, int x0, int y0, int x1, int y1) {
    float sc = float(t.w) / t.gw;
    return {std::clamp<LONG>(LONG(x0 * sc), 0, t.w), std::clamp<LONG>(LONG(y0 * sc), 0, t.h),
            std::clamp<LONG>(LONG(x1 * sc), 0, t.w), std::clamp<LONG>(LONG(y1 * sc), 0, t.h)};
}

// rs: the rects buffer, reused across targets
void D3D12Backend::ClearTarget(Target& t, const State& s, const ClearArgs& c, std::vector<D3D12_RECT>& rs) {
    rs.clear();
    if (c.count && c.rects) {
        for (uint32_t i = 0; i < c.count; i++) {
            int32_t r[4];
            std::memcpy(r, c.rects + size_t(i) * sizeof(r), sizeof(r));
            rs.push_back(ScaledRect(t, r[0], r[1], r[2], r[3]));
        }
    } else {
        const auto& v = s.viewport;
        rs.push_back(ScaledRect(t, int(v.x), int(v.y), int(v.x + v.w), int(v.y + v.h)));
    }
    if (t.depth) {
        Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        D3D12_CLEAR_FLAGS cf = D3D12_CLEAR_FLAGS(((c.flags & kClearZ) ? D3D12_CLEAR_FLAG_DEPTH : 0) |
                                                 ((c.flags & kClearStencil) ? D3D12_CLEAR_FLAG_STENCIL : 0));
        cl_->ClearDepthStencilView(Cpu(dsv_heap_.Get(), t.view, dsv_inc_), cf, std::clamp(c.z, 0.0f, 1.0f),
                                   UINT8(c.stencil), UINT(rs.size()), rs.data());
        if (c.flags & kClearZ) InvalidateParticleDepth(t.res.Get());
        else ++particle_depth_serial_;  // stencil-only changes do not alter the verified depth encoding
    } else {
        // pixels the CPU wrote before this clear go in first, a later upload would paint over the clear
        if (LatchCpuWrite(t)) UploadTarget(t, s.rt);
        Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        float col[4];
        Color(c.color, col);
        for (int i = 0; t.range != 1 && i < 3; i++) col[i] /= t.range;
        cl_->ClearRenderTargetView(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), col, UINT(rs.size()), rs.data());
    }
    t.drawn = true;
}

void D3D12Backend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                         uint32_t stencil) {
    if (flags & kClearDofLoading) {
        if (flags != kClearDofLoading || count != 1 || !rects) return;
        DofLoadingMarker marker{};
        std::memcpy(&marker, rects, sizeof(marker));
        if (!marker.generation || marker.reserved || marker.begin > 1) return;
        if (marker.begin) {
            // The atomic cancellation check also rejects a Begin whose mission ended while it was queued.
            if (g_dof_loading_requested_generation.load(std::memory_order_acquire) != marker.generation) return;
            dof_loading_generation_ = marker.generation;
            dof_loading_after_present_ = presents_ + 1;
            dof_preparation_started_ = true;
        } else if (dof_loading_generation_ == marker.generation) {
            dof_loading_generation_ = 0;
        }
        return;
    }
    if (flags & (kClearParticleDofBegin | kClearParticleDofEnd)) {
        particle_dof_.marker = flags == kClearParticleDofBegin && count == 0 && !rects;
        return;
    }
    if (flags & (kClearWeaponDofBegin | kClearWeaponDofEnd)) {
        if (count != kWeaponDofRects || !rects) { weapon_dof_.pending = false; return; }
        float p[4 * kWeaponDofRects];
        std::memcpy(p, rects, sizeof(p));
        WeaponDepthOfField(s, p, (flags & kClearWeaponDofBegin) != 0);
        return;
    }
    if (flags & kClearDepthOfField) {
        if (count != kDepthOfFieldRects || !rects) return;
        float p[4 * kDepthOfFieldRects];
        std::memcpy(p, rects, sizeof(p));
        DepthOfField(s, p);
        return;
    }
    if (flags & kClearOcclusion) {
        if (count != kOcclusionRects || !rects) return;
        float p[4 * kOcclusionRects];
        std::memcpy(p, rects, sizeof(p));
        Occlusion(s, p);
        return;
    }
    PerfScope perf_clear{g_perf.clear};
    if (cap_) {
        CapStep(s.rt);
        fprintf(cap_, "%04u clear rt %08X ds %08X flags %X color %08X z %.3f\n", cap_i_++, s.rt.data, s.zs.data, flags,
                color, z);
    }
    Target* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    Target* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    uint32_t gw;
    uint32_t gh;
    SurfaceSize(rt ? s.rt : s.zs, gw, gh);
    std::vector<D3D12_RECT> rs;
    ClearArgs c{.count = count, .rects = rects, .flags = flags, .color = color, .z = z, .stencil = stencil};
    for (Target* t : {rt, ds}) {
        if (t) ClearTarget(*t, s, c, rs);
    }
}

// CopyRects' guest lists: src_rects RECTs, dst_points POINTs, all int32
constexpr uint32_t kGuestRectBytes = 16;
constexpr uint32_t kRectLeft = 0;
constexpr uint32_t kRectTop = 4;
constexpr uint32_t kRectRight = 8;
constexpr uint32_t kRectBottom = 12;
constexpr uint32_t kGuestPointBytes = 8;
constexpr uint32_t kPointX = 0;
constexpr uint32_t kPointY = 4;

struct GuestRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

struct GuestPoint {
    int32_t x;
    int32_t y;
};

GuestRect ReadGuestRect(uint32_t rects, uint32_t i) {
    uint32_t r = rects + i * kGuestRectBytes;
    GuestRect g;
    g.left = int32_t(kernel::R32(r + kRectLeft));
    g.top = int32_t(kernel::R32(r + kRectTop));
    g.right = int32_t(kernel::R32(r + kRectRight));
    g.bottom = int32_t(kernel::R32(r + kRectBottom));
    return g;
}

GuestPoint ReadGuestPoint(uint32_t points, uint32_t i) {
    uint32_t p = points + i * kGuestPointBytes;
    GuestPoint g;
    g.x = int32_t(kernel::R32(p + kPointX));
    g.y = int32_t(kernel::R32(p + kPointY));
    return g;
}

// a drawn copy's rect in guest pixels: the source corners and where it lands
struct DrawnRect {
    float x0 = 0;
    float y0 = 0;
    float x1 = 0;
    float y1 = 0;
    float dx = 0;
    float dy = 0;
};

// without dst_points the rect lands where it is
DrawnRect ReadDrawnRect(uint32_t src_rects, uint32_t dst_points, uint32_t i) {
    GuestRect g = ReadGuestRect(src_rects, i);
    DrawnRect r;
    r.x0 = float(g.left);
    r.y0 = float(g.top);
    r.x1 = float(g.right);
    r.y1 = float(g.bottom);
    r.dx = r.x0;
    r.dy = r.y0;
    if (dst_points) {
        GuestPoint p = ReadGuestPoint(dst_points, i);
        r.dx = float(p.x);
        r.dy = float(p.y);
    }
    return r;
}

// a guest coordinate read as unsigned, times the host scale
UINT ScaleUnsigned(int32_t v, float sc) { return UINT(uint32_t(v) * sc); }

// a drawn copy's taps round down a ratio a hair over a whole number
constexpr float kTapsSlack = 1e-3f;

// one Blit per rect: for formats that differ or a multisampled target
void D3D12Backend::CopyByDraw(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state,
                              Target& dst_t, const CopyList& l) {
    if (dst_t.depth) InvalidateParticleDepth(dst_t.res.Get());
    Transition(src_res, src_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(dst_t.res.Get(), dst_t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), dst_t.view, rtv_inc_);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    float src_scale = float(src_t.w) / src_t.gw;
    float dst_scale = float(dst_t.w) / dst_t.gw;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    for (uint32_t i = 0; i < n; i++) {
        float whole_w = float(std::min(src_t.gw, dst_t.gw));
        float whole_h = float(std::min(src_t.gh, dst_t.gh));
        DrawnRect r{.x0 = 0, .y0 = 0, .x1 = whole_w, .y1 = whole_h};
        if (listed) r = ReadDrawnRect(l.src_rects, l.dst_points, i);
        if (r.x1 <= r.x0 || r.y1 <= r.y0) continue;
        D3D12_VIEWPORT vp{r.dx * dst_scale, r.dy * dst_scale, (r.x1 - r.x0) * dst_scale, (r.y1 - r.y0) * dst_scale,
                          0, 1};
        LONG right = std::min(LONG(vp.TopLeftX + vp.Width), LONG(dst_t.w));
        LONG bottom = std::min(LONG(vp.TopLeftY + vp.Height), LONG(dst_t.h));
        D3D12_RECT scissor{LONG(vp.TopLeftX), LONG(vp.TopLeftY), right, bottom};
        if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) continue;
        cl_->RSSetViewports(1, &vp);
        cl_->RSSetScissorRects(1, &scissor);
        float uv[4] = {r.x0 / src_t.gw, r.y0 / src_t.gh, (r.x1 - r.x0) / src_t.gw, (r.y1 - r.y0) / src_t.gh};
        uint32_t taps = uint32_t(std::ceil(std::max(src_scale / dst_scale, 1.0f) - kTapsSlack));
        float mul = 1 / dst_t.range;
        BlitOptions o{.alpha = true, .taps = taps, .mul = mul, .rtv = dst_t.fmt, .samples = Ms(dst_t)};
        Blit(src_res, src_t.fmt, uv, o);
    }
    dst_t.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

// one CopyTextureRegion per rect, clamped to both targets
void D3D12Backend::CopyByRegion(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state,
                                Target& dst_t, const CopyList& l) {
    if (dst_t.depth) InvalidateParticleDepth(dst_t.res.Get());
    Transition(src_res, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(dst_t.res.Get(), dst_t.state, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dl{dst_t.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION sl{src_res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    float dst_scale = float(dst_t.w) / dst_t.gw;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    for (uint32_t i = 0; i < n; i++) {
        D3D12_BOX b{0, 0, 0, std::min(src_t.w, dst_t.w), std::min(src_t.h, dst_t.h), 1};
        uint32_t dx = 0;
        uint32_t dy = 0;
        if (listed) {
            GuestRect r = ReadGuestRect(l.src_rects, i);
            b.left = ScaleUnsigned(r.left, dst_scale);
            b.top = ScaleUnsigned(r.top, dst_scale);
            b.right = std::min(ScaleUnsigned(r.right, dst_scale), src_t.w);
            b.bottom = std::min(ScaleUnsigned(r.bottom, dst_scale), src_t.h);
            dx = b.left;
            dy = b.top;
            if (l.dst_points) {
                GuestPoint p = ReadGuestPoint(l.dst_points, i);
                dx = ScaleUnsigned(p.x, dst_scale);
                dy = ScaleUnsigned(p.y, dst_scale);
            }
        }
        if (b.right <= b.left || b.bottom <= b.top || dx >= dst_t.w || dy >= dst_t.h) continue;
        b.right = std::min<UINT>(b.right, b.left + dst_t.w - dx);
        b.bottom = std::min<UINT>(b.bottom, b.top + dst_t.h - dy);
        cl_->CopyTextureRegion(&dl, dx, dy, 0, &sl, &b);
    }
    dst_t.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

void D3D12Backend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects,
                        uint32_t dst_points) {
    PerfScope perf_copy{g_perf.copy};
    Target* src_t = FindTarget(src, false);
    if (cap_) {
        const char* from = "memory";
        if (src_t) from = src_t->drawn ? "target" : "idle target";
        fprintf(cap_, "%04u copy %08X %s to %08X\n", cap_i_++, src.data, from, dst.data);
    }
    // pixels the CPU made: the copy happens in memory, textures and targets pick it up from there
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "d3d12: CopyRects %08X to %08X skipped", src.data, dst.data);
        } else if (Target* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }
    Resolve(*src_t);
    Target* dst_t = EnsureTarget(dst, false);
    if (!dst_t || src_t == dst_t) return;
    D3D12_RESOURCE_STATES* st;
    ID3D12Resource* from = Flat(*src_t, st);
    if (!from) return;
    CopyList l{.count = count, .src_rects = src_rects, .dst_points = dst_points};
    if (src_t->fmt != dst_t->fmt || Ms(*dst_t) > 1) {
        // formats differ, or a multisampled target takes no copy: drawn across
        CopyByDraw(*src_t, from, *st, *dst_t, l);
        return;
    }
    CopyByRegion(*src_t, from, *st, *dst_t, l);
    if (cap_) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_copy_%08X", cap_i_, dst_t->data);
        CapSave(*dst_t, name, dst_t->range);
    }
}

// the output the window's display has: a factory made before Windows HDR was switched no longer tells
void D3D12Backend::UpdateHdrDisplay() {
    HMONITOR mon = MonitorFromWindow(static_cast<HWND>(app::Window()), MONITOR_DEFAULTTONEAREST);
    if (mon == hdr_mon_ && factory_->IsCurrent()) return;
    hdr_mon_ = mon;
    ComPtr<IDXGIFactory4> fresh;
    if (!factory_->IsCurrent() && SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&fresh)))) factory_ = fresh;
    DXGI_OUTPUT_DESC1 desc{};
    bool found = FindOutput(factory_.Get(), mon, desc);
    hdr_display_ = found && desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    bool known_peak = found && desc.MaxLuminance > 0 && desc.MaxLuminance <= kMaxPeakNits;
    peak_nits_ = known_peak ? desc.MaxLuminance : kDefaultPeakNits;
    paper_nits_ = SdrWhiteNits(mon);
    game::g_hdr_display = hdr_display_;
    LOGR("Display HDR: %s, peak %.0f nits, SDR white %.0f nits", hdr_display_ ? "on" : "off", peak_nits_,
         paper_nits_);
}

void D3D12Backend::ResizeSwap() {
    HWND hwnd = static_cast<HWND>(app::Window());
    RECT rc;
    GetClientRect(hwnd, &rc);
    uint32_t w = std::max<LONG>(rc.right - rc.left, 1);
    uint32_t h = std::max<LONG>(rc.bottom - rc.top, 1);
    bool hdr = hdr_display_ && game::Opt().hdr_output;
    if (w == bb_w_ && h == bb_h_ && hdr == hdr_want_) return;
    particle_dof_.marker = particle_dof_.world_valid = false;
    InvalidateParticleDepth();
    Flush();
    for (auto& f : frames_) {
        if (f.fence) WaitFence(f.fence);
    }
    for (auto& b : bb_) b.Reset();
    hdr_want_ = hdr;
    hdr_out_ = hdr && ResizeBackBuffers(w, h, kHdr10Format, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
    if (!hdr_out_) ResizeBackBuffers(w, h, kBgra, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    if (hdr && !hdr_out_) LOGR("Display HDR: the swap chain can't show HDR10, SDR it is");
    CreateBackBufferViews();
    bb_w_ = w;
    bb_h_ = h;
}

// false when the swap chain can't present in that colour space
bool D3D12Backend::ResizeBackBuffers(uint32_t w, uint32_t h, DXGI_FORMAT fmt, DXGI_COLOR_SPACE_TYPE space) {
    UINT flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (FAILED(swap_->ResizeBuffers(kFrames, w, h, fmt, flags))) return false;
    UINT support = 0;
    swap_->CheckColorSpaceSupport(space, &support);
    if (!(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) return false;
    return SUCCEEDED(swap_->SetColorSpace1(space));
}

void D3D12Backend::DrawFrame(Target* t, uint32_t bi) {
    D3D12_RESOURCE_STATES bs = D3D12_RESOURCE_STATE_PRESENT;
    ID3D12Resource* r = PrepareShownFrame(t);
    Transition(bb_[bi].Get(), bs, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), kRtvCount + bi, rtv_inc_);
    // without its targets an HDR10 back buffer stays black: the SDR pipelines don't match its format
    if (!hdr_out_) {
        DrawSdrFrame(t, r, rh);
    } else if (EnsureOutTargets()) {
        DrawHdrFrame(t, r, rh);
    } else {
        float black[4] = {0, 0, 0, 1};
        cl_->ClearRenderTargetView(rh, black, 0, nullptr);
    }
    Transition(bb_[bi].Get(), bs, D3D12_RESOURCE_STATE_PRESENT);
}

// what the frame shows: the target made readable, SMAA's blend of it when that is on
ID3D12Resource* D3D12Backend::PrepareShownFrame(Target* t) {
    if (!t) return nullptr;
    D3D12_RESOURCE_STATES* st;
    ID3D12Resource* r = Flat(*t, st);
    if (!r) return nullptr;
    Transition(r, *st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (game::Opt().smaa) {
        if (ID3D12Resource* aa = Smaa(r, t->fmt, t->w, t->h)) r = aa;
    }
    return r;
}

// src in the pixel shader resource state: SMAA's blend of it, null when it can't run
ID3D12Resource* D3D12Backend::Smaa(ID3D12Resource* src, DXGI_FORMAT fmt, uint32_t w, uint32_t h) {
    static const std::string hlsl = std::string(kSmaaConfig) + kSmaaHlsl + kSmaaPasses;
    if (!EnsureSmaaLookups()) return nullptr;
    if (!EnsureSmaaTargets(fmt, w, h)) return nullptr;
    if (!DrawSmaaPasses(src, fmt, w, h, hlsl.c_str())) return nullptr;
    return smaa_[2].Get();
}

bool D3D12Backend::EnsureSmaaLookups() {
    // smaa_textures.h: area 160 x 560 in two channels, search 64 x 16 in one
    constexpr SmaaLookup area{DXGI_FORMAT_R8G8_UNORM, 160, 560, 2, kSmaaArea};
    constexpr SmaaLookup search{DXGI_FORMAT_R8_UNORM, 64, 16, 1, kSmaaSearch};
    static_assert(sizeof(kSmaaArea) == area.w * area.h * area.bpp);
    static_assert(sizeof(kSmaaSearch) == search.w * search.h * search.bpp);
    if (smaa_lut_[1]) return true;
    if (!CreateLookupTexture(smaa_lut_[0], area) || !CreateLookupTexture(smaa_lut_[1], search)) {
        smaa_lut_[0].Reset();
        smaa_lut_[1].Reset();
        return false;
    }
    return true;
}

// a lookup texture filled from l's data, left in the pixel shader resource state
bool D3D12Backend::CreateLookupTexture(ComPtr<ID3D12Resource>& r, const SmaaLookup& l) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(l.fmt, l.w, l.h);
    D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COPY_DEST;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, st, nullptr, IID_PPV_ARGS(&r)))) {
        return false;
    }
    uint32_t row = l.w * l.bpp;
    uint32_t pitch = UploadPitch(row);
    Alloc a = Upload(uint64_t(pitch) * l.h, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    for (uint32_t y = 0; y < l.h; y++) std::memcpy(a.cpu + size_t(y) * pitch, l.data + size_t(y) * row, row);
    D3D12_TEXTURE_COPY_LOCATION dl{r.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION sl{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    sl.PlacedFootprint.Offset = a.off;
    sl.PlacedFootprint.Footprint = {l.fmt, l.w, l.h, 1, pitch};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    Transition(r.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}

// SMAA's own targets: edges, then blend weights, then the blended frame in the frame's format
constexpr DXGI_FORMAT kSmaaEdgeFormat = DXGI_FORMAT_R8G8_UNORM;
constexpr DXGI_FORMAT kSmaaWeightFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

bool D3D12Backend::EnsureSmaaTargets(DXGI_FORMAT fmt, uint32_t w, uint32_t h) {
    if (smaa_[2] && smaa_w_ == w && smaa_h_ == h && smaa_fmt_ == fmt) return true;
    smaa_w_ = smaa_h_ = 0;
    const DXGI_FORMAT f[kSmaaTargets] = {kSmaaEdgeFormat, kSmaaWeightFormat, fmt};
    for (int i = 0; i < kSmaaTargets; i++) {
        if (!CreateSmaaTarget(i, f[i], w, h)) {
            for (auto& r : smaa_) {
                if (r) Release(std::move(r));
            }
            return false;
        }
    }
    smaa_w_ = w;
    smaa_h_ = h;
    smaa_fmt_ = fmt;
    LOGI("d3d12: SMAA %ux%u", w, h);
    return true;
}

bool D3D12Backend::CreateSmaaTarget(int i, DXGI_FORMAT fmt, uint32_t w, uint32_t h) {
    if (smaa_[i]) Release(std::move(smaa_[i]));
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(fmt, w, h);
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    smaa_state_[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
    HRESULT hr = dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, smaa_state_[i], nullptr,
                                               IID_PPV_ARGS(&smaa_[i]));
    if (FAILED(hr)) return false;
    dev_->CreateRenderTargetView(smaa_[i].Get(), nullptr, SmaaView(i));
    return true;
}

// edges, blend weights, then the blend of src into smaa_[2], left readable
bool D3D12Backend::DrawSmaaPasses(ID3D12Resource* src, DXGI_FORMAT fmt, uint32_t w, uint32_t h, const char* hlsl) {
    // edge detection discards where there is no edge, so the edges start clear each frame
    const float clear[4] = {};
    float p[8] = {1.0f / w, 1.0f / h, float(w), float(h)};
    Transition(smaa_[0].Get(), smaa_state_[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
    cl_->ClearRenderTargetView(SmaaView(0), clear, 0, nullptr);
    ID3D12Resource* const edges[3] = {src, nullptr, nullptr};
    const DXGI_FORMAT edges_fmt[3] = {fmt, fmt, fmt};
    if (!Pass(hlsl, "vs_edges", "smaa_edges", kSmaaEdgeFormat, SmaaView(0), w, h, edges, edges_fmt, p, -1)) {
        return false;
    }
    Transition(smaa_[0].Get(), smaa_state_[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(smaa_[1].Get(), smaa_state_[1], D3D12_RESOURCE_STATE_RENDER_TARGET);
    cl_->ClearRenderTargetView(SmaaView(1), clear, 0, nullptr);
    ID3D12Resource* const weights[3] = {smaa_[0].Get(), smaa_lut_[0].Get(), smaa_lut_[1].Get()};
    const DXGI_FORMAT weights_fmt[3] = {kSmaaEdgeFormat, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8_UNORM};
    if (!Pass(hlsl, "vs_weights", "smaa_weights", kSmaaWeightFormat, SmaaView(1), w, h, weights, weights_fmt, p, -1)) {
        return false;
    }
    Transition(smaa_[1].Get(), smaa_state_[1], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(smaa_[2].Get(), smaa_state_[2], D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12Resource* const blend[3] = {src, smaa_[1].Get(), nullptr};
    const DXGI_FORMAT blend_fmt[3] = {fmt, kSmaaWeightFormat, fmt};
    if (!Pass(hlsl, "vs_blend", "smaa_blend", fmt, SmaaView(2), w, h, blend, blend_fmt, p, -1)) return false;
    Transition(smaa_[2].Get(), smaa_state_[2], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}

void D3D12Backend::DrawSdrFrame(Target* t, ID3D12Resource* r, D3D12_CPU_DESCRIPTOR_HANDLE bb) {
    float black[4] = {0, 0, 0, 1};
    cl_->ClearRenderTargetView(bb, black, 0, nullptr);
    cl_->OMSetRenderTargets(1, &bb, FALSE, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    if (t) BlitFrame(*t, r, kBgra);
    RenderUi();
}

bool D3D12Backend::EnsureOutTargets() {
    if (out_[kOutFrame] && out_[kOutPanel] && out_w_ == bb_w_ && out_h_ == bb_h_) return true;
    out_w_ = bb_w_;
    out_h_ = bb_h_;
    return CreateOutTarget(kOutFrame, kOutFrameFormat) && CreateOutTarget(kOutPanel, kBgra);
}

bool D3D12Backend::CreateOutTarget(int i, DXGI_FORMAT fmt) {
    if (out_[i]) Release(std::move(out_[i]));
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(fmt, bb_w_, bb_h_);
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    out_state_[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
    HRESULT hr = dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, out_state_[i], nullptr,
                                               IID_PPV_ARGS(&out_[i]));
    if (FAILED(hr)) {
        LOGE("d3d12: no %ux%u HDR10 output target", bb_w_, bb_h_);
        return false;
    }
    dev_->CreateRenderTargetView(out_[i].Get(), nullptr, OutView(i));
    return true;
}

// frame and panel into their own targets, then both into the back buffer as HDR10, panel at SDR white
void D3D12Backend::DrawHdrFrame(Target* t, ID3D12Resource* r, D3D12_CPU_DESCRIPTOR_HANDLE bb) {
    D3D12_CPU_DESCRIPTOR_HANDLE frame = OutView(kOutFrame);
    D3D12_CPU_DESCRIPTOR_HANDLE panel = OutView(kOutPanel);
    float clear[4] = {0, 0, 0, 0};
    for (int i = 0; i < kOutTargets; i++) {
        Transition(out_[i].Get(), out_state_[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    cl_->ClearRenderTargetView(frame, clear, 0, nullptr);
    cl_->ClearRenderTargetView(panel, clear, 0, nullptr);
    cl_->OMSetRenderTargets(1, &frame, FALSE, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    if (t) BlitFrame(*t, r, kOutFrameFormat);
    cl_->OMSetRenderTargets(1, &panel, FALSE, nullptr);
    RenderUi();
    Begin();  // RenderUi left ImGui's heap and root signature bound
    for (int i = 0; i < kOutTargets; i++) {
        Transition(out_[i].Get(), out_state_[i], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    ID3D12Resource* const src[3] = {out_[kOutFrame].Get(), out_[kOutPanel].Get(), nullptr};
    const DXGI_FORMAT fmt[3] = {kOutFrameFormat, kBgra, kBgra};
    float p[8] = {paper_nits_, peak_nits_, 0, 0};
    Pass(kHdrHlsl, "hdr10", kHdr10Format, bb, bb_w_, bb_h_, src, fmt, p, -1);
}

// the frame letterboxed to the game's aspect into the bound target
void D3D12Backend::BlitFrame(Target& t, ID3D12Resource* r, DXGI_FORMAT rtv) {
    float aspect = game::Aspect();
    float w = float(bb_w_);
    float h = float(bb_h_);
    float vw = std::min(w, h * aspect);
    float vh = vw / aspect;
    float uv[4] = {0, 0, 1, 1};
    D3D12_VIEWPORT vp{(w - vw) / 2, (h - vh) / 2, vw, vh, 0, 1};
    D3D12_RECT scissor{0, 0, LONG(bb_w_), LONG(bb_h_)};
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &scissor);
    if (r) Blit(r, t.fmt, uv, {.gamma = gamma_, .rtv = rtv});
}

void D3D12Backend::RenderUi() {
    std::lock_guard lock(app::UiLock());
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (app::kGame.ui) app::kGame.ui(fps_);
    ImGui::Render();
    ID3D12DescriptorHeap* uh[] = {ui_heap_.Get()};
    cl_->SetDescriptorHeaps(1, uh);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl_.Get());
}

void D3D12Backend::FinishCapture(const Surface& front) {
    CapStep(Surface{});
    if (Target* f = FindTarget(front, false)) CapSave(*f, "final", f->range);
    for (auto& t : cap_tex_) {
        char name[32];
        snprintf(name, sizeof(name), "tex_%08X", t.data);
        D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        CapSave({.res = t.res.Get(), .state = &st, .fmt = t.fmt, .w = t.w, .h = t.h}, name);
    }
    fclose(cap_);
    cap_ = nullptr;
    cap_tex_.clear();
    LOGI("d3d12: frame captured to %s", cap_dir_.string().c_str());
}

void D3D12Backend::StartCapture() {
    SYSTEMTIME tm;
    GetLocalTime(&tm);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%02u%02u%02u", tm.wHour, tm.wMinute, tm.wSecond);
    std::error_code ec;
    cap_dir_ = std::filesystem::weakly_canonical(app::ExeDir() / diag::CaptureDir() / stamp, ec);
    std::filesystem::create_directories(cap_dir_, ec);
    cap_ = _wfopen((cap_dir_ / "frame.txt").c_str(), L"w");
    cap_i_ = cap_rt_ = 0;
    cap_seen_.clear();
    cap_sh_.clear();
}

// ramp_ holds the red, green and blue ramps, 256 entries each
constexpr int kRampRed = 0;
constexpr int kRampGreen = 256;
constexpr int kRampBlue = 512;

void D3D12Backend::UploadGammaRamp() {
    constexpr uint32_t kRampBytes = 256 * 4;  // one BGRA texel per ramp entry
    ramp_dirty_ = false;
    Alloc a = Upload(kRampBytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    bool identity = true;
    for (int i = 0; i < 256; i++) {
        uint8_t* p = a.cpu + i * 4;
        p[0] = ramp_[kRampBlue + i];
        p[1] = ramp_[kRampGreen + i];
        p[2] = ramp_[kRampRed + i];
        p[3] = 255;
        identity &= ramp_[kRampRed + i] == i && ramp_[kRampGreen + i] == i && ramp_[kRampBlue + i] == i;
    }
    gamma_ = !identity;
    Transition(lut_.Get(), lut_state_, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dl{lut_.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION sl{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    sl.PlacedFootprint.Offset = a.off;
    sl.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, 256, 1, 1, kRampBytes};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
}

void D3D12Backend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;    // presents between sweeps
    constexpr size_t kTexturesKept = 2048;  // no sweep below this many
    constexpr uint64_t kIdleFrames = 600;
    // textures unused for ten seconds go, levels stream theirs in and out
    if (presents_ % kSweepEvery != 0 || (!texture_pack::Enabled() && textures_.size() <= kTexturesKept)) return;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.frame + kIdleFrames < presents_) {
            CurrentFrame().garbage.push_back(it->second.res);
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }
}

void D3D12Backend::ResetFrame() {
    Frame& f = CurrentFrame();
    if (f.fence && fence_->GetCompletedValue() < f.fence) {
        auto perf_wait = PerfClock::now();
        WaitFence(f.fence);
        g_perf.wait += PerfMs(perf_wait);
    }
    f.alloc->Reset();
    f.garbage.clear();
    f.used = 0;
    f.srv = 0;
    cl_->Reset(f.alloc.Get(), nullptr);
    Begin();
}

void D3D12Backend::CountFps() {
    fps_n_++;
    auto now = std::chrono::steady_clock::now();
    double el = std::chrono::duration<double>(now - fps_t_).count();
    if (el >= 0.5) {
        fps_ = fps_n_ / el;
        fps_n_ = 0;
        fps_t_ = now;
    }
}

void LogPerf() {
    static PerfClock::time_point perf_start = PerfClock::now();
    static uint64_t perf_frames = 0;
    perf_frames++;
    double el = PerfMs(perf_start);
    if (el >= 1000.0) {
        Perf& p = g_perf;
        double n = double(perf_frames);
        double host = p.draw + p.clear + p.copy + p.present + p.sync;
        LOGI("perf: %.1f fps %.2f ms/frame | host %.2f (draw %.2f: assemble %.2f bind %.2f [check %.2f, upload %.2f] compile %.2f; "
             "clear %.2f copy %.2f sync %.2f present %.2f [swap %.2f wait %.2f]) other %.2f",
             n * 1000.0 / el, el / n, host / n, p.draw / n, p.assemble / n, p.bind / n, p.sig / n, p.texup / n,
             p.compile / n, p.clear / n, p.copy / n, p.sync / n, p.present / n, p.swap / n, p.wait / n,
             (el - host) / n);
        LOGI("perf: title thread per frame: recording %.2f ms, waiting %.2f ms (%.1f waits: read-back %.2f resource %.2f fence %.2f "
             "flush %.2f copy %.2f, next frame %.2f) | read-backs waited for: %.1f lock, %.1f NOOVERWRITE",
             g_perf_capture_ns.exchange(0) / n / 1e6, g_perf_wait_ns.exchange(0) / n / 1e6,
             g_perf_waits.exchange(0) / n, g_perf_wait_kind_ns[0].exchange(0) / n / 1e6,
             g_perf_wait_kind_ns[1].exchange(0) / n / 1e6, g_perf_wait_kind_ns[2].exchange(0) / n / 1e6,
             g_perf_wait_kind_ns[3].exchange(0) / n / 1e6, g_perf_wait_kind_ns[4].exchange(0) / n / 1e6,
             g_perf_wait_kind_ns[5].exchange(0) / n / 1e6, g_perf_sync_block.exchange(0) / n,
             g_perf_sync_late.exchange(0) / n);
        LOGI("perf: per frame %.0f draws (%.0f raw) %.0fk verts %.1f MB upload %.1f overflow | %.0f checks %.2f MB hashed | %.1f tex uploads "
             "%.2f MB, %.1f new tex, %.1f new targets, %.1f target uploads, %.1f shrinks, %.1f read-backs %.1f late | %llu shaders %llu psos this second",
             p.draws / n, p.raw / n, p.verts / n / 1000.0, p.upload / n / 1048576.0, p.overflow / n, p.sigs / n,
             p.sig_bytes / n / 1048576.0, p.texups / n, p.texup_bytes / n / 1048576.0, p.newtex / n,
             p.targets / n, p.rtups / n, p.shrinks / n, p.syncs / n, p.lates / n,
             (unsigned long long)p.shaders, (unsigned long long)p.psos);
        g_perf = Perf{};
        perf_start = PerfClock::now();
        perf_frames = 0;
    }
}

void D3D12Backend::Present(const Surface& front) {
    auto& particle = particle_dof_;
    if (particle.batches && ((particle.world_valid && frame_ % 120 == 0) ||
        (particle.previous_world && !particle.world_valid))) {
        LOGR("Particle DOF totals: %llu batches, %llu filtered quads (%llu additive, %llu reverse-subtractive), %llu focused, %llu invalid quads, "
             "%llu early/no-world batches, %llu rejected batches; %llu depth copies/%.1f MiB, %llu reused, %llu range passes; "
             "%llu atlas batches/%llu pages/%llu quads, %llu direct-filter quads",
             (unsigned long long)particle.batches, (unsigned long long)particle.active,
             (unsigned long long)particle.additive,
             (unsigned long long)particle.subtractive,
             (unsigned long long)particle.focused, (unsigned long long)particle.invalid,
             (unsigned long long)particle.early, (unsigned long long)particle.rejected,
             (unsigned long long)particle.copies, double(particle.copy_bytes) / 1048576.0,
             (unsigned long long)particle.reused, (unsigned long long)particle.range_passes,
             (unsigned long long)particle.atlas_batches, (unsigned long long)particle.atlas_pages,
             (unsigned long long)particle.atlas_quads, (unsigned long long)particle.atlas_fallback);
        const auto& reasons = particle.geometry_rejections;
        LOGR("Particle DOF geometry fallbacks: %llu invalid inputs, %llu depth, %llu UV, %llu degenerate, %llu non-affine",
             (unsigned long long)reasons[size_t(ParticleDofGeometryRejection::InvalidInput)],
             (unsigned long long)reasons[size_t(ParticleDofGeometryRejection::Depth)],
             (unsigned long long)reasons[size_t(ParticleDofGeometryRejection::Uv)],
             (unsigned long long)reasons[size_t(ParticleDofGeometryRejection::Degenerate)],
             (unsigned long long)reasons[size_t(ParticleDofGeometryRejection::NonAffine)]);
        LOGR("Particle DOF destination coverage totals: %llu blur-aware batches, %llu unknown depth, %llu varying native depth",
             (unsigned long long)particle.coverage_batches, (unsigned long long)particle.coverage_unknown,
             (unsigned long long)particle.coverage_varying_depth);
        const auto& shared = particle.shared_admission;
        LOGR("Particle DOF shared coverage: %llu admitted/%llu used pages, %llu unavailable batches, %.0f summed/%.0f union pixels; "
             "reject samples %llu, projection %llu, count %llu, mapping %llu, depth %llu, bounds %llu, overlap %llu",
             (unsigned long long)shared[size_t(ParticleSharedReject::None)], (unsigned long long)particle.shared_pages,
             (unsigned long long)particle.shared_unavailable, particle.shared_sum_area, particle.shared_union_area,
             (unsigned long long)shared[size_t(ParticleSharedReject::Samples)],
             (unsigned long long)shared[size_t(ParticleSharedReject::Projection)],
             (unsigned long long)shared[size_t(ParticleSharedReject::Count)],
             (unsigned long long)shared[size_t(ParticleSharedReject::Mapping)],
             (unsigned long long)shared[size_t(ParticleSharedReject::Depth)],
             (unsigned long long)shared[size_t(ParticleSharedReject::Bounds)],
             (unsigned long long)shared[size_t(ParticleSharedReject::Overlap)]);
        LOGR("Particle DOF native visibility cache: %llu pages, %llu quads",
             (unsigned long long)particle.visibility_pages, (unsigned long long)particle.visibility_quads);
    }
    particle.previous_world = particle.world_valid;
    particle.marker = particle.world_valid = false;
    for (auto& tag : particle.depth_projection) tag = {};
    weapon_dof_.pending = false;
    PerfScope perf_present{g_perf.present};
    if (cap_) {
        FinishCapture(front);
    } else if (g_capture.exchange(false) && CaptureOn()) {
        StartCapture();
    }
    UpdateHdrDisplay();
    ResizeSwap();
    Target* t = EnsureTarget(front, false);
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    if (t) Resolve(*t);
    if (ramp_dirty_) UploadGammaRamp();
    Transition(lut_.Get(), lut_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    uint32_t bi = swap_->GetCurrentBackBufferIndex();
    DrawFrame(t, bi);
    Submit(false, true);
    present_fence_[presents_ % (kFrames + 1)].store(fence_value_);
    bool vsync = app::Get().vsync;
    auto perf_swap = PerfClock::now();
    swap_->Present(vsync ? 1 : 0, !vsync && tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
    g_perf.swap += PerfMs(perf_swap);

    presents_++;
    presents_seen_.store(presents_);
    frame_++;
    ReleaseOldTextures();
    ResetFrame();
    CountFps();
    if (diag::Perf()) LogPerf();
    ApplySettings();
}

void D3D12Backend::ApplySettings() {
    float want = app::RenderScale(guest_h_);
    bool remake = ext_ && ext_->Settings();
    uint32_t msaa = game::Opt().msaa;
    if (std::fabs(want - scale_) > 1e-4f || remake || msaa != msaa_set_) {
        particle_dof_.marker = particle_dof_.world_valid = false;
        InvalidateParticleDepth();
        Flush();
        for (auto& [key, tg] : targets_) (tg.depth ? free_dsv_ : free_rtv_).push_back(tg.view);
        targets_.clear();
        scale_ = want;
        if (msaa != msaa_set_) {
            msaa_set_ = msaa;
            msaa_ = Samples(msaa_set_);
        }
        if (ext_) ext_->Reset(*this);
        bound_rt_ = bound_ds_ = nullptr;
    }
    PrimeDepthOfField();
    BeginDofTiming();
}

// the target resolved, then shrunk with o when it is not guest sized BGRA; res null on failure
D3D12Backend::CopySource D3D12Backend::PrepareCopySource(Target& t, const ShrinkOptions& o) {
    Resolve(t);
    D3D12_RESOURCE_STATES* st;
    CopySource out{Flat(t, st), nullptr};
    if (!out.res) return out;
    if (t.w != t.gw || t.h != t.gh || t.fmt != kBgra) {
        out.down = Shrink(out.res, *st, t.fmt, t.gw, t.gh, o);
        out.res = out.down.Get();
    } else {
        Transition(out.res, *st, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    return out;
}

// the CPU reads a surface the GPU drew: bring the pixels back
void D3D12Backend::Sync(uint32_t data, uint32_t) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || !it->second.drawn) return;
    PerfScope perf_sync{g_perf.sync};
    g_perf.syncs++;
    if (diag::Perf()) {
        LOG_FIRST(16, "perf: read-back of %08X (%ux%u guest, %ux%u host)", data, it->second.gw, it->second.gh,
                  it->second.w, it->second.h);
    }
    Target& t = it->second;
    // src.down, when there is one, stays alive until ReadBack is done
    CopySource src = PrepareCopySource(t, {});
    if (!src.res) return;
    std::vector<uint8_t> px;
    if (!ReadBack(src.res, t.gw, t.gh, px)) return;
    // color targets are 32 bit linear in guest memory, the pitch rounds to 64 bytes
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    for (uint32_t y = 0; y < t.gh; y++) std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.gw * 4], t.gw * 4);
    t.read = std::move(px);
    t.drawn = false;
    t.sig = MemSig(t.data, t.bytes);
}

// A NOOVERWRITE lock: the console reads whatever the GPU has finished, so a surface locked every frame gets the newest
// finished copy (a frame or two old) and the CPU never waits on the GPU; one locked now and then still waits in Sync
void D3D12Backend::SyncLate(uint32_t data) {
    if (!DeliverLate(data)) Sync(data, 0);
    RecordLate(data);
}

bool D3D12Backend::DeliverLate(uint32_t data) {
    std::lock_guard lock(late_lock_);
    auto it = late_.find(data);
    if (it == late_.end()) return false;
    Late& l = it->second;
    uint64_t done = fence_->GetCompletedValue();
    uint64_t now = presents_seen_.load();
    int best = -1;
    for (int i = 0; i < kFrames; i++) {
        bool finished = l.fence[i] && l.fence[i] <= done;
        bool recent = now - l.frame[i] <= uint64_t(kFrames) + 1;
        bool newest = best < 0 || l.fence[i] > l.fence[best];
        if (finished && recent && newest) best = i;
    }
    if (best < 0) {
        if (diag::Perf()) {
            LOG_FIRST(16, "perf: late %08X nothing finished: done %llu now %llu, slots %llu/%llu %llu/%llu %llu/%llu",
                      data, (unsigned long long)done, (unsigned long long)now, (unsigned long long)l.fence[0],
                      (unsigned long long)l.frame[0], (unsigned long long)l.fence[1], (unsigned long long)l.frame[1],
                      (unsigned long long)l.fence[2], (unsigned long long)l.frame[2]);
        }
        return false;
    }
    if (l.fence[best] > l.delivered) {
        uint8_t* p = nullptr;
        // the last row is not padded to the pitch: a range to the end of the pitch fails to map
        D3D12_RANGE rr{0, size_t(l.row_pitch) * (l.gh - 1) + l.gw * 4};
        if (FAILED(l.rb[best]->Map(0, &rr, reinterpret_cast<void**>(&p)))) return false;
        uint32_t pitch = GuestPitch(l.gw);
        uint8_t* dst = static_cast<uint8_t*>(kernel::HostPtr(data));
        for (uint32_t y = 0; y < l.gh; y++) std::memcpy(dst + size_t(y) * pitch, p + size_t(y) * l.row_pitch, l.gw * 4);
        D3D12_RANGE none{0, 0};
        l.rb[best]->Unmap(0, &none);
        l.delivered = l.fence[best];
    }
    return true;
}

// a slot set for another size: its buffers go with the frame
void D3D12Backend::ResetLate(Late& l, const Target& t) {
    auto& garbage = CurrentFrame().garbage;
    for (auto& r : l.rb) {
        if (r) garbage.push_back(r);
    }
    if (l.down) garbage.push_back(l.down);
    l = Late{};
    l.gw = t.gw;
    l.gh = t.gh;
}

// the target as a guest sized BGRA copy source, shrunk into l.down when it is not one already
ID3D12Resource* D3D12Backend::PrepareLateSource(Target& t, Late& l) {
    ShrinkOptions reuse{.into = l.down.Get(), .into_state = l.down_state};
    CopySource src = PrepareCopySource(t, reuse);
    if (src.down) {
        l.down = src.down;
        l.down_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    return src.res;
}

// src into the readback buffer of the slot, made on first use
void D3D12Backend::RecordLateCopy(Late& l, uint32_t slot, ID3D12Resource* src) {
    D3D12_RESOURCE_DESC rd = src->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    dev_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_size, &bytes);
    if (!l.rb[slot]) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd = BufferDesc(bytes);
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&l.rb[slot])))) {
            return;
        }
    }
    D3D12_TEXTURE_COPY_LOCATION dl{l.rb[slot].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dl.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION sl{src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    l.row_pitch = fp.Footprint.RowPitch;
    l.fence[slot] = fence_value_ + 1;  // the next Submit carries it
    l.frame[slot] = presents_;
    l.next = (slot + 1) % kFrames;
}

void D3D12Backend::RecordLate(uint32_t data) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end()) return;
    Target& t = it->second;
    PerfScope perf_sync{g_perf.sync};
    g_perf.lates++;
    // what DeliverLate or Sync left in guest memory is no CPU write to upload back
    t.sig = MemSig(t.data, t.bytes);
    t.read.clear();
    std::lock_guard lock(late_lock_);
    Late& l = late_[data];
    if (l.gw != t.gw || l.gh != t.gh) {
        if (diag::Perf()) LOG_FIRST(16, "perf: late %08X ring reset, %ux%u -> %ux%u", data, l.gw, l.gh, t.gw, t.gh);
        ResetLate(l, t);
    }
    // a copy for a later lock, unless the GPU is a whole ring behind
    uint32_t slot = l.next;
    if (l.fence[slot] > fence_->GetCompletedValue()) return;
    ID3D12Resource* src = PrepareLateSource(t, l);
    if (!src) return;
    RecordLateCopy(l, slot, src);
}

// filters src into a new w x h target, left as a copy source
ComPtr<ID3D12Resource> D3D12Backend::Shrink(ID3D12Resource* src, D3D12_RESOURCE_STATES& st, DXGI_FORMAT fmt,
                                            uint32_t w, uint32_t h, const ShrinkOptions& o) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, w, h);
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = td.Format;
    ComPtr<ID3D12Resource> down = o.into;
    D3D12_RESOURCE_STATES down_state = o.into ? o.into_state : D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_perf.shrinks++;
    if (free_rtv_.empty()) return nullptr;
    if (down) {
        Transition(down.Get(), down_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    } else if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                                    D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&down)))) {
        return nullptr;
    }
    // borrows the next free view without taking it, only for this recording
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), free_rtv_.back(), rtv_inc_);
    dev_->CreateRenderTargetView(down.Get(), nullptr, rh);
    Transition(src, st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    SetBlitTarget(rh, w, h);
    float uv[4] = {0, 0, 1, 1};
    Blit(src, fmt, uv, {.alpha = true, .taps = o.taps, .mul = o.mul});
    Transition(down.Get(), down_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    return down;
}

// a BGRA copy source to the CPU, rows packed; waits for the GPU
bool D3D12Backend::ReadBack(ID3D12Resource* res, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    D3D12_RESOURCE_DESC rd = res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    dev_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_size, &bytes);
    ComPtr<ID3D12Resource> rb;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = BufferDesc(bytes);
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&rb)))) {
        return false;
    }
    D3D12_TEXTURE_COPY_LOCATION dl{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dl.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION sl{res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    Flush();
    uint8_t* p = nullptr;
    rb->Map(0, nullptr, reinterpret_cast<void**>(&p));
    out.resize(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; y++) std::memcpy(&out[size_t(y) * w * 4], p + size_t(y) * fp.Footprint.RowPitch, w * 4);
    rb->Unmap(0, nullptr);
    return true;
}

// BMP file and info header: byte offsets of the fields CapSave writes
constexpr int kBmpHeaderBytes = 54;
constexpr int kBmpFileSize = 2;
constexpr int kBmpPixelOffset = 10;
constexpr int kBmpInfoSize = 14;
constexpr int kBmpWidth = 18;
constexpr int kBmpHeight = 22;  // negative: rows top down
constexpr int kBmpPlanes = 26;
constexpr int kBmpBitCount = 28;
constexpr int kBmpImageSize = 34;
constexpr uint32_t kBmpInfoBytes = 40;

void D3D12Backend::CapSave(const CapImage& img, const char* name) {
    ComPtr<ID3D12Resource> down = Shrink(img.res, *img.state, img.fmt, img.w, img.h, {.mul = img.mul});
    std::vector<uint8_t> px;
    if (!down || !ReadBack(down.Get(), img.w, img.h, px)) return;
    FILE* f = _wfopen((cap_dir_ / (std::string(name) + ".bmp")).c_str(), L"wb");
    if (!f) return;
    uint8_t hd[kBmpHeaderBytes] = {'B', 'M'};  // BITMAPFILEHEADER and BITMAPINFOHEADER, top down 32 bit
    auto put = [&](int o, uint32_t v) { std::memcpy(hd + o, &v, 4); };
    put(kBmpFileSize, kBmpHeaderBytes + uint32_t(px.size()));
    put(kBmpPixelOffset, kBmpHeaderBytes);
    put(kBmpInfoSize, kBmpInfoBytes);
    put(kBmpWidth, img.w);
    put(kBmpHeight, uint32_t(-int32_t(img.h)));
    hd[kBmpPlanes] = 1;
    hd[kBmpBitCount] = 32;
    put(kBmpImageSize, uint32_t(px.size()));
    fwrite(hd, 1, kBmpHeaderBytes, f);
    fwrite(px.data(), 1, px.size(), f);
    fclose(f);
}

void D3D12Backend::CapSave(Target& t, const char* name, float mul) {
    D3D12_RESOURCE_STATES* st;
    ID3D12Resource* r = Flat(t, st);
    if (!r) return;
    DXGI_FORMAT fmt = t.depth ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : t.fmt;
    CapSave({.res = r, .state = st, .fmt = fmt, .w = t.gw, .h = t.gh, .mul = mul}, name);
}

// the frame moves to another target: save the one it leaves
void D3D12Backend::CapStep(const Surface& rt) {
    uint32_t d = rt.header ? mem::VirtualFromPhysical(rt.data) : 0;
    if (d == cap_rt_) return;
    auto it = targets_.find(TargetMapKey(cap_rt_, false));
    if (cap_rt_ && it != targets_.end()) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_%08X", cap_i_, cap_rt_);
        CapSave(it->second, name, it->second.range);
    }
    auto z = targets_.find(TargetMapKey(cap_rt_, true));
    if (cap_rt_ && z != targets_.end()) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_%08X_z", cap_i_, cap_rt_);
        CapSave(z->second, name, 1);
    }
    cap_rt_ = d;
}

void D3D12Backend::Capture(Target& t, const char* tag, float mul) {
    if (!cap_) return;
    char name[32];
    snprintf(name, sizeof(name), "%04u_%s_%08X", cap_i_, tag, t.data);
    CapSave(t, name, mul);
}

void D3D12Backend::SetGamma(const uint8_t* ramp) {
    std::memcpy(ramp_, ramp, sizeof(ramp_));
    ramp_dirty_ = true;
}

// the slot is not reused before the caller's wait: the title runs at most one present ahead
void D3D12Backend::WaitGpuFrame(uint64_t n) {
    uint64_t fence = present_fence_[n % (kFrames + 1)].load();
    if (fence > fence_->GetCompletedValue()) fence_->SetEventOnCompletion(fence, nullptr);
}

D3D12Backend* g_d3d12 = nullptr;

}  // namespace

std::unique_ptr<Backend> CreateBackend() {
    auto backend = std::make_unique<D3D12Backend>();
    g_d3d12 = backend.get();
    return backend;
}

void WaitGpuFrame(uint64_t n) {
    if (g_d3d12) g_d3d12->WaitGpuFrame(n);
}

}  // namespace gpu
