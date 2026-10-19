#pragma once
#include <algorithm>
#include <array>
#include <string>
#include <string_view>

#include "gpu/shaders.h"
#include "particle_dof_shader.h"

namespace gpu {

// D660/D6A0, installed by 178A00 and selected by 15BB00. Match the translated program,
// not a runtime handle: handles change while these native instructions do not.
inline constexpr std::array<uint32_t, 16> kParticleDofNativeProgram = {
    0x00000000, 0x0020121B, 0x0836106C, 0x2070F848,
    0x00000000, 0x004C401B, 0x0836186C, 0x2F200FF8,
    0x00000000, 0x006C201B, 0x2436106C, 0x3070F800,
    0x00000000, 0x0020061B, 0x0836106C, 0x2070F819};
inline constexpr std::array<uint32_t, 5> kParticleDofNativeDeclaration = {
    0x20000000, 0x40420000, 0x40400003, 0x40220009, 0xFFFFFFFF};

inline PixelKey ParticleDofNativePixelKey(TexInfo texture) {
    PixelKey k{};
    k.programmable = 1;
    k.count = 0x00011101;
    k.a_in[0] = 0xD8D41010;
    k.a_out[0] = 0xC0;
    k.rgb_in[0] = 0xC8C40000;
    k.rgb_out[0] = 0xC0;
    k.tex_modes = 1;
    k.tex[0] = texture;
    return k;
}

inline bool IsParticleDofNativePixel(const PixelKey& k) {
    if (k.tex[0].kind != kTex2D || k.tex[0].rect || k.tex[0].shadow ||
        (k.tex[0].flags & (kTexWide | kTexTarget)) || k.flat > 1 || k.two_sided || k.quad || k.proj ||
        (k.alphakill & ~1u)) return false;
    auto expected = ParticleDofNativePixelKey(k.tex[0]);
    expected.alphakill = k.alphakill;
    // Native emitter 158B70 selects both shade modes. The current SDK stores this bit
    // in PixelKey but does not consume it in either shader translator.
    expected.flat = k.flat;
    // Bound unused textures do not affect this program; only stage zero is sampled.
    std::copy(std::begin(k.tex), std::end(k.tex), std::begin(expected.tex));
    return expected == k;
}

inline PixelKey CanonicalParticleDofPixelKey(PixelKey k) {
    // The verified program samples only t0. Stale unused texture/sampler types otherwise
    // produce different source hashes and repeat the expensive particle shader compilation.
    if (IsParticleDofNativePixel(k)) {
        k.flat = 0;
        for (int i = 1; i < 4; ++i) k.tex[i] = {};
    }
    return k;
}

namespace particle_dof_detail {
inline bool ReplaceOne(std::string& s, std::string_view from, std::string_view to) {
    auto at = s.find(from);
    if (at == std::string::npos || s.find(from, at + from.size()) != std::string::npos) return false;
    s.replace(at, from.size(), to);
    return true;
}
inline bool Common(std::string& s) {
    if (!ReplaceOne(s, "    float fogamt : TEXCOORD4;", "    float fogamt : TEXCOORD4;\n"
        "    nointerpolation uint particleQuad : TEXCOORD5;\n"
        "    nointerpolation float particleViewZ : TEXCOORD6;")) return false;
    if (!ReplaceOne(s, "    float4 lt[56];\n    float4 rtshift;\n};",
        std::string("    float4 lt[56];\n    float4 rtshift;\n") +
        kParticleDofCbufferFields + "};")) return false;
    s.insert(0, kParticleDofQuadHlsl);
    return true;
}
} // namespace particle_dof_detail

// Empty means that the translator changed its contract; the renderer must use native geometry/shaders.
inline std::string MakeParticleDofVertexShader(std::string s, uint32_t pass = 0) {
    using namespace particle_dof_detail;
    if (pass > 5 || !Common(s)) return {};
    if (!ReplaceOne(s, "VOut main(VIn i)", "VOut main(VIn i, uint vertexId : SV_VertexID)") &&
        !ReplaceOne(s, "VOut main(VRaw ri)", "VOut main(VRaw ri, uint vertexId : SV_VertexID)")) return {};
    std::string finish = "    o.particleQuad = vertexId / 4;\n"
        "    o.particleViewZ = 1.0 / i.v0.z;\n";
    if (pass == 1) finish += R"(
    ParticleDofQuad quad = particleQuads[o.particleQuad];
    if (quad.atlas_scale <= 0) o.pos = float4(2, 2, 0, 1);
    else {
        float2 screen = (o.pos.xy / o.pos.w * float2(.5, -.5) + .5) * particleTarget.xy;
        float2 origin = float2(quad.atlas_xy & 65535u, quad.atlas_xy >> 16);
        float2 screenOrigin = float2(quad.screen_xy & 65535u, quad.screen_xy >> 16);
        float2 atlas = origin + 2 + (screen - screenOrigin) * quad.atlas_scale;
        o.pos.xy = (atlas / particleAtlasTarget.xy * float2(2, -2) + float2(-1, 1)) * o.pos.w;
    }
)";
    finish += "    return o;";
    if (!ReplaceOne(s, "    return o;", finish)) return {};
    return s;
}

inline std::string MakeParticleDofPixelShader(std::string s, uint32_t samples, uint32_t blend = 0,
                                             int alphaFunction = -1, uint32_t pass = 0) {
    using namespace particle_dof_detail;
    if ((samples != 1 && samples != 2 && samples != 4 && samples != 8) || blend > 1 ||
        alphaFunction < -1 || alphaFunction > 7 || pass > 5) return {};
    // Shared fields are an explicitly eligible2x path. These passes consume
    // filtered source energy, so native source alpha variants are unnecessary.
    if (pass >= 3 && (samples != 2 || alphaFunction != 0 || (pass != 4 && blend != 0))) return {};
    if (!Common(s)) return {};
    // Unused native stages can retain stale 2D/cube/shadow bindings. Remove their declarations
    // to make room for the exact MSAA bounds at t2 and original-sample depth copy at t3.
    for (int stage = pass >= 2 ? 1 : 2; stage < 4; ++stage) {
        const auto declaration = " tex" + std::to_string(stage) + " : register(t" + std::to_string(stage) + ");";
        auto at = s.find(declaration);
        if (at == std::string::npos) return {};
        auto line = s.rfind('\n', at);
        if (line == std::string::npos) return {};
        auto line_end = s.find('\n', at);
        if (line_end == std::string::npos) return {};
        s.erase(line + 1, line_end - line);
    }
    const std::string entry = "float4 main(VOut i, bool front : SV_IsFrontFace) : SV_Target";
    auto at = s.find(entry);
    if (at == std::string::npos || s.find(entry, at + entry.size()) != std::string::npos) return {};
    std::string body = s.substr(at + entry.size());
    if (pass >= 3 && body.find("if (t0.a == 0) discard;") != std::string::npos) return {};
    // The translated native main is last. Reject added functions or unexpected texture accesses.
    if (body.find("tex1.") != std::string::npos || body.find("tex2.") != std::string::npos ||
        body.find("tex3.") != std::string::npos || body.find("spreadtap(") != std::string::npos) return {};
    std::string tap = body;
    const std::string sample = "tex0.Sample(smp0, ";
    auto fetch = tap.find(sample);
    if (fetch == std::string::npos || tap.find(sample, fetch + sample.size()) != std::string::npos) return {};
    size_t end = fetch + sample.size();
    int nested = 0;
    for (; end < tap.size(); ++end) {
        if (tap[end] == '(') ++nested;
        if (tap[end] == ')') { if (nested == 0) break; --nested; }
    }
    if (end == tap.size()) return {};
    tap.insert(end, ", uvDx, uvDy");
    tap.replace(fetch, sample.size(), "tex0.SampleGrad(smp0, ");
    // A rejected SOURCE texel contributes transparent, rather than discarding the destination aperture.
    ReplaceOne(tap, "if (t0.a == 0) discard;", "if (t0.a == 0) return float4(0, 0, 0, 0);");
    if (!ReplaceOne(tap, "alphatest(outc.a);", "if (!ParticleNativeAlphaPass(outc.a)) return float4(0, 0, 0, 0);")) return {};
    if (tap.find("discard") != std::string::npos) return {};
    s.resize(at);
    s += "float4 ParticleNativeExact(VOut i, bool front)" + body;
    if (alphaFunction < 0) s += R"(
bool ParticleNativeAlphaPass(float a) {
    int f = (int)misc.y;
    int v = (int)round(a * 255), r = (int)misc.x;
    return f == 0 ? false : f == 1 ? v < r : f == 2 ? v == r : f == 3 ? v <= r :
        f == 4 ? v > r : f == 5 ? v != r : f == 6 ? v >= r : true;
}
)";
    else {
        // Specialize only the repeated source test. The native zero-CoC callback and dynamic
        // alpha reference retain exactly the translator's round-to-byte comparison semantics.
        constexpr std::array<const char*, 8> expression = {
            "false", "v < r", "v == r", "v <= r", "v > r", "v != r", "v >= r", "true"};
        s += "\nbool ParticleNativeAlphaPass(float a) {\n";
        if (alphaFunction != 0 && alphaFunction != 7)
            s += "    int v = (int)round(a * 255), r = (int)misc.x;\n";
        s += "    return " + std::string(expression[alphaFunction]) + ";\n}\n";
    }
    s += "float4 ParticleNativeTap(VOut i, bool front, float2 uvDx, float2 uvDy)" + tap;
    s += "\n#define PARTICLE_DOF_SAMPLES " + std::to_string(samples) + "\n";
    s += "#define PARTICLE_DOF_BLEND " + std::to_string(blend) + "\n";
    s += "#define PARTICLE_DOF_PASS " + std::to_string(pass) + "\n";
    s += kParticleDofDiskHlsl;
    s += kParticleDofHlsl;
    return s;
}

} // namespace gpu
