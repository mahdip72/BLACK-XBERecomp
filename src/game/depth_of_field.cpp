// Replace only reload-triggered screen blur. Keep the original timer and its late weapon draw order.
#include <array>
#include <cmath>

#include "common/options.h"
#include "d3d/depth_of_field.h"
#include "game/layout.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {
constexpr uint32_t kEffects = 0x2D1990;
constexpr uint32_t kEffect = 0x830A0;
constexpr uint32_t kStrength = 0x10;
constexpr uint32_t kPeak = 0x18;
constexpr uint32_t kRamp = 0x1C;
constexpr uint32_t kHold = 0x20;
constexpr uint32_t kFade = 0x24;
constexpr uint32_t kType = 0x34;
constexpr uint32_t kProjection = 0x297720;
constexpr uint32_t kFrameCount = 0x2F4368;

uint32_t g_reload = 0;
std::array<uint32_t, 4> g_timing{};
uint32_t g_replaced_frame = 0;
bool g_replaced = false;

std::array<uint32_t, 4> Timing(uint32_t effect) {
    return {R32(effect + kPeak), R32(effect + kRamp), R32(effect + kHold), R32(effect + kFade)};
}
}  // namespace

// Both normal and multipart reloads enter here; tag only a screen effect this call actually started.
X86_EXTERN_FUNC(__imp__sub_0017B130);
void ReloadAnimationDof(X86Context& ctx, uint8_t* base) {
    uint32_t manager = R32(kEffects);
    bool idle = manager && R32(manager + kEffect + kType) == 0;
    __imp__sub_0017B130(ctx, base);
    if (idle && R32(kEffects) == manager && R32(manager + kEffect + kType) == 1) {
        g_reload = manager + kEffect;
        g_timing = Timing(g_reload);
    }
}

// After ambient occlusion, before alpha, bloom and HUD. Type 1 keeps the weapon in the late draw list.
void ReloadDepthOfField(X86Context&, uint8_t*) {
    g_replaced = false;
    uint32_t manager = R32(kEffects);
    uint32_t game = R32(layout::kGame);
    if (!g_reload || !manager || manager + kEffect != g_reload || R32(g_reload + kType) != 1 ||
        Timing(g_reload) != g_timing || !game || !R32(layout::kWorld) ||
        R32(game + layout::kCurrentFilm) < layout::kFilms) {
        g_reload = 0;
        return;
    }
    if (!game::Opt().reload_blur || !game::Opt().modern_reload_dof) return;
    float strength = G<float>(g_reload + kStrength);
    if (!std::isfinite(strength)) return;
    g_replaced = d3d::DepthOfField(&G<float>(kProjection), strength);
    g_replaced_frame = R32(kFrameCount);
}

X86_EXTERN_FUNC(__imp__sub_001772E0);
X86_FUNC(sub_001772E0) {
    uint32_t effect = R32(ctx.esp + 4);
    if (g_replaced && g_replaced_frame == R32(kFrameCount) && effect == g_reload &&
        R32(effect + kType) == 1) {
        ctx.esp += 8;  // the original returns with ret 4
        return;
    }
    __imp__sub_001772E0(ctx, base);
}
