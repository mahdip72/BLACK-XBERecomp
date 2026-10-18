// Reload DOF follows the native timer; its session preview routes only rendering through the late weapon draw.
#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>

#include "common/options.h"
#include "core/log.h"
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
bool g_weapon_pending = false;
uint32_t g_weapon_frame = 0;
uint32_t g_weapon_serial = 0;
float g_weapon_strength = 0;
std::array<float, 16> g_weapon_projection{};
struct PreviewDraw {
    uint32_t manager = 0;
    uint32_t display = 0;
    uint32_t frame = 0;
    uint32_t serial = 0;
    float strength = 0;
};
PreviewDraw g_preview;
uint32_t g_preview_serial = 0;
float g_preview_phase = 0;
uint32_t g_preview_tick_frame = 0;
bool g_preview_clock_started = false;
std::chrono::steady_clock::time_point g_preview_clock;

std::array<uint32_t, 4> Timing(uint32_t effect) {
    return {R32(effect + kPeak), R32(effect + kRamp), R32(effect + kHold), R32(effect + kFade)};
}

bool WeaponDofEligible() {
    const auto& options = game::Opt();
    uint32_t manager = R32(kEffects);
    return options.reload_blur && options.modern_reload_dof && !options.dof_preview && g_preview_phase == 0 &&
           !g_preview.serial && options.dof_weapon_blur &&
           manager && R32(manager + kEffect + kType) == 0 && R32(layout::kGame) && R32(layout::kWorld);
}

bool PreviewAllowed() {
    const auto& options = game::Opt();
    uint32_t manager = R32(kEffects);
    return options.reload_blur && options.modern_reload_dof && manager &&
           R32(manager + kEffect + kType) == 0 && R32(layout::kGame) && R32(layout::kWorld) && R32(layout::kDisplay);
}

bool PreviewDrawPending() {
    return g_preview.serial && g_preview.frame == R32(kFrameCount) &&
           g_preview.manager == R32(kEffects) && g_preview.display == R32(layout::kDisplay) &&
           R32(g_preview.manager + kEffect + kType) == 0;
}
}  // namespace

float CinematicDofStrength(float strength) {
    if (!std::isfinite(strength)) return 0;
    float s = std::clamp(strength, 0.0f, 1.0f);
    return s * s * s * (s * (s * 6 - 15) + 10);
}

float StepDofPreview(float phase, float seconds, bool enabled) {
    float value = std::isfinite(phase) ? std::clamp(phase, 0.0f, 1.0f) : 0.0f;
    float dt = std::isfinite(seconds) ? std::clamp(seconds, 0.0f, 0.1f) : 0.0f;
    float next = std::clamp(value + (enabled ? dt / 0.8f : -dt / 0.9f), 0.0f, 1.0f);
    // Settle the endpoint exactly after accumulated floating-point steps.
    if (enabled && next > 1.0f - 1e-6f) return 1;
    if (!enabled && next < 1e-6f) return 0;
    return next;
}

// The render hook supplies bounded host time. This step also provides a deterministic phase update for tests.
void TickDofPreview(float seconds) {
    if (PreviewAllowed()) {
        g_preview_phase = StepDofPreview(g_preview_phase, seconds, game::Opt().dof_preview);
    } else {
        g_preview_phase = 0;
        g_preview = {};
    }
    g_preview_tick_frame = R32(kFrameCount);
    g_preview_clock = std::chrono::steady_clock::now();
    g_preview_clock_started = true;
}

// These bool hooks bypass only the two render branches. Preview never changes the native effect or its clock.
bool DeferPreviewWeapon(X86Context&, uint8_t*) {
    g_preview = {};
    if (!PreviewAllowed() || !g_preview_clock_started || g_preview_tick_frame != R32(kFrameCount)) {
        auto now = std::chrono::steady_clock::now();
        float seconds = g_preview_clock_started ? std::chrono::duration<float>(now - g_preview_clock).count() : 0;
        TickDofPreview(seconds);
    }
    if (!PreviewAllowed() || (!game::Opt().dof_preview && g_preview_phase == 0)) return false;
    if (++g_preview_serial == 0) ++g_preview_serial;
    g_preview = {R32(kEffects), R32(layout::kDisplay), R32(kFrameCount), g_preview_serial,
                 CinematicDofStrength(g_preview_phase)};
    return true;
}

bool DrawPreviewWeapon(X86Context&, uint8_t*) {
    bool deferred = PreviewDrawPending();
    g_preview = {};
    // The weapon must still draw if preview was disabled or its world queue failed after the early branch.
    return deferred;
}

// Both normal and multipart reloads enter here; tag only a screen effect this call actually started.
X86_EXTERN_FUNC(__imp__sub_0017B130);
void ReloadAnimationDof(X86Context& ctx, uint8_t* base) {
    uint32_t manager = R32(kEffects);
    bool idle = manager && R32(manager + kEffect + kType) == 0;
    __imp__sub_0017B130(ctx, base);
    if (idle && R32(kEffects) == manager && R32(manager + kEffect + kType) == 1) {
        g_reload = manager + kEffect;
        // The stock updater and cancellation code keep owning the clock and late weapon draw lifetime.
        // Lengthen only modern reload transitions, then ease their normalized strength before rendering.
        if (game::Opt().reload_blur && game::Opt().modern_reload_dof) {
            float& ramp = G<float>(g_reload + kRamp);
            float& fade = G<float>(g_reload + kFade);
            if (std::isfinite(ramp) && std::isfinite(fade) && ramp >= 0 && fade >= 0) {
                ramp = std::max(ramp, 0.8f);
                fade = std::max(fade, 0.9f);
            }
        }
        g_timing = Timing(g_reload);
        static bool reported = false;
        if (!reported) {
            LOGR("dof: reload captured, ramp %.2f, hold %.2f, fade %.2f",
                 G<float>(g_reload + kRamp), G<float>(g_reload + kHold), G<float>(g_reload + kFade));
            reported = true;
        }
    }
}

// After world meshes and particles, before the original post-effect capture, bloom and HUD.
// Type 1 keeps the weapon in the late draw list until all screen effects have finished.
void ReloadDepthOfField(X86Context&, uint8_t*) {
    g_replaced = false;
    uint32_t manager = R32(kEffects);
    uint32_t game = R32(layout::kGame);
    if (!g_reload || !manager || manager + kEffect != g_reload || R32(g_reload + kType) != 1 ||
        Timing(g_reload) != g_timing || !game || !R32(layout::kWorld)) {
        g_reload = 0;
        if (PreviewDrawPending() && PreviewAllowed() && g_preview.strength > 0)
            d3d::DepthOfField(&G<float>(kProjection), g_preview.strength);
        return;
    }
    if (!game::Opt().reload_blur || !game::Opt().modern_reload_dof) return;
    float strength = G<float>(g_reload + kStrength);
    if (!std::isfinite(strength)) return;
    g_replaced = d3d::DepthOfField(&G<float>(kProjection), CinematicDofStrength(strength));
    g_replaced_frame = R32(kFrameCount);
    static bool reported = false;
    if (g_replaced && !reported) {
        LOGR("dof: modern reload queued at frame %u", g_replaced_frame);
        reported = true;
    }
}

// The normal weapon draw follows world list 0. Capture its existing depth so only newly written weapon samples
// can receive the light blur. The original type-1 branch skips both hooks and draws the reload weapon later.
void BeginWeaponDepthOfField(X86Context& ctx, uint8_t*) {
    g_weapon_pending = false;
    uint32_t display = R32(layout::kDisplay);
    uint32_t list = display + layout::kDisplayDrawLists + layout::kGunDrawList * layout::kDrawListSize;
    if (!WeaponDofEligible() || !display || ctx.esi != list || !R32(list + 8)) return;
    g_weapon_frame = R32(kFrameCount);
    ++g_weapon_serial;
    g_weapon_strength = float(std::min(game::Opt().dof_weapon_blur, 100u)) / 100.0f;
    std::copy_n(&G<float>(kProjection), g_weapon_projection.size(), g_weapon_projection.begin());
    g_weapon_pending = d3d::BeginWeaponDepthOfField(g_weapon_projection.data(), g_weapon_strength, g_weapon_serial);
}

void EndWeaponDepthOfField(X86Context&, uint8_t*) {
    bool pending = g_weapon_pending;
    g_weapon_pending = false;
    if (!pending || g_weapon_frame != R32(kFrameCount) || !WeaponDofEligible()) return;
    d3d::EndWeaponDepthOfField(g_weapon_projection.data(), g_weapon_strength, g_weapon_serial);
}

X86_EXTERN_FUNC(__imp__sub_001772E0);
X86_FUNC(sub_001772E0) {
    uint32_t effect = R32(ctx.esp + 4);
    if (g_replaced && g_replaced_frame == R32(kFrameCount) && effect == g_reload &&
        R32(effect + kType) == 1) {
        static bool reported = false;
        if (!reported) {
            LOGR("dof: original reload blur suppressed");
            reported = true;
        }
        ctx.esp += 8;  // the original returns with ret 4
        return;
    }
    __imp__sub_001772E0(ctx, base);
}
