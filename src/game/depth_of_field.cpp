// Reload DOF follows the native timer. Preview and authored idle inspections share a render-only focus fade.
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
bool g_weapon_preview = false;
uint32_t g_weapon_frame = 0;
uint32_t g_weapon_serial = 0;
uint32_t g_weapon_manager = 0;
uint32_t g_weapon_display = 0;
float g_weapon_strength = 0;
float g_weapon_sharp_depth = 0;
float g_weapon_transition = 0;
std::array<float, 16> g_weapon_projection{};
struct PreviewDraw {
    uint32_t manager = 0;
    uint32_t display = 0;
    uint32_t frame = 0;
    uint32_t serial = 0;
    float strength = 0;
    float weapon_strength = 0;
};
PreviewDraw g_preview;
PreviewDraw g_preview_weapon;
uint32_t g_preview_serial = 0;
float g_preview_phase = 0;
uint32_t g_preview_tick_frame = 0;
bool g_preview_clock_started = false;
std::chrono::steady_clock::time_point g_preview_clock;

struct IdleSample {
    uint32_t player = 0, weapon = 0, controller = 0, instance = 0;
    uint32_t root = 0, cache = 0, generation = 0, key = 0, frame = 0;
    bool inspection = false;
};
IdleSample g_idle;

// 92B60/9CD30 resolve the controller name to this lowercase SDBM key. 96FD0 stores
// it in the active instance, so a name copied by a failed 17B130 lookup cannot tag an old clip.
bool IdleNameMatches(uint32_t controller, uint32_t key) {
    std::array<char, 26> name{};
    size_t length = 0;
    uint32_t hash = 0;
    for (; length < name.size(); ++length) {
        char c = G<char>(controller + 0x5C + uint32_t(length));
        if (!c) break;
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
        name[length] = c;
        hash = hash * 0x1003Fu + uint8_t(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    }
    constexpr std::array<char, 6> suffix{'_', 'S', '_', '0', '0', '1'};
    return length >= 10 && length < name.size() && name[0] == 'F' && name[1] == 'P' && name[2] == '_' &&
           std::equal(suffix.begin(), suffix.end(), name.begin() + length - suffix.size()) && hash == key;
}

bool IdleIdentity(IdleSample& sample) {
    uint32_t world = R32(layout::kWorld);
    if (!world) return false;
    sample.player = world + layout::kWorldPlayer;
    sample.weapon = R32(sample.player + 0x29C);
    sample.controller = R32(sample.player + 0x328);
    if (!sample.weapon || R32(sample.weapon + 0xF0) != sample.player || !sample.controller ||
        R32(sample.controller) != sample.player) return false;
    // 18029A also sets controller+B4 for every looping descriptor, including S001.
    // Only the animation instance's native completion state can terminate this idle graph.
    sample.instance = R32(sample.controller + 0x54);
    if (!sample.instance || (R32(sample.instance) & 0x02000001u)) return false;
    float duration = G<float>(sample.instance + 0x1C);
    if (!std::isfinite(duration) || duration <= 1) return false;
    sample.root = R32(sample.instance + 0x0C);
    sample.cache = R32(sample.instance + 0x98C);
    sample.generation = R32(sample.instance + 0x970);
    sample.key = R32(sample.instance + 0x10);
    return sample.root && sample.cache && IdleNameMatches(sample.controller, sample.key);
}

bool SameIdleIdentity(const IdleSample& a, const IdleSample& b) {
    return a.player == b.player && a.weapon == b.weapon && a.controller == b.controller &&
           a.instance == b.instance && a.root == b.root && a.cache == b.cache &&
           a.generation == b.generation && a.key == b.key;
}

bool IdleFocusActive() {
    IdleSample live;
    // Allow one frame-counter boundary between pose sampling and rendering. Never reuse an
    // older sample or one belonging to an interrupted/replaced playback instance.
    return g_idle.inspection && uint32_t(R32(kFrameCount) - g_idle.frame) <= 1 &&
           IdleIdentity(live) && SameIdleIdentity(live, g_idle);
}

bool NodeDuration(uint32_t node, uint32_t cache, float& duration) {
    int id = G<int16_t>(node + 2);
    uint32_t count = R32(cache + 0x118);
    if (id < 0 || count > 70 || (id < 70 && uint32_t(id) >= count)) return false;
    // Mirror 98CB0's reads, without invoking 9B3B0 (which can initialize guest cache entries).
    duration = G<float>(id < 70 ? cache + uint32_t(id) * 4 : node + 4);
    return std::isfinite(duration) && duration > 0;
}

uint32_t IdleLeaf(uint32_t node) {
    // Only the duration wrappers and terminal animation leaves present in the authored FP idle
    // graphs are accepted. The depth limit also terminates cyclic/malformed graphs.
    for (unsigned depth = 0; node && depth < 8; ++depth) {
        unsigned type = G<uint8_t>(node) & 0x3F;
        int count = G<int8_t>(node + 1);
        if (type == 0x16 && count == 0) return R32(node + 0x10);
        if (type != 8 || count != 1) return 0;
        uint32_t params = R32(node + 8);
        if (!params || !std::isfinite(G<float>(params)) || G<float>(params) <= 0) return 0;
        node = R32(node + 0x10);
    }
    return 0;
}

bool InspectionPhase(uint32_t root, uint32_t cache, float time, bool& inspection) {
    if (!std::isfinite(time) || time < 0) return false;
    uint32_t sequence = root;
    unsigned depth = 0;
    for (; sequence && depth < 8; ++depth) {
        if ((G<uint8_t>(sequence) & 0x3F) != 2) return false;
        int count = G<int8_t>(sequence + 1);
        if (count != 1) break;
        sequence = R32(sequence + 0x10);
    }
    if (!sequence || depth == 8) return false;
    int count = G<int8_t>(sequence + 1);
    if (count < 2 || count > 16) return false;
    uint32_t ordinary = 0;
    float total = 0, tail_start = 0;
    for (int i = 0; i < count; ++i) {
        uint32_t child = R32(sequence + 0x10 + uint32_t(i) * 4);
        if (!child) return false;
        uint32_t leaf = IdleLeaf(child);
        float duration = 0;
        if (!leaf || !NodeDuration(child, cache, duration)) return false;
        if (!i) ordinary = leaf;
        if (i == count - 1) {
            if (leaf == ordinary) return false;
            tail_start = total;
        } else if (leaf != ordinary) return false;
        total += duration;
        if (!std::isfinite(total)) return false;
    }
    if (time >= total) return false;
    // 98CB0 selects the first cumulative duration strictly greater than current time.
    inspection = time >= tail_start;
    return true;
}

std::array<uint32_t, 4> Timing(uint32_t effect) {
    return {R32(effect + kPeak), R32(effect + kRamp), R32(effect + kHold), R32(effect + kFade)};
}

bool WeaponDofAllowed() {
    const auto& options = game::Opt();
    uint32_t manager = R32(kEffects);
    return options.reload_blur && options.modern_reload_dof && manager &&
           R32(manager + kEffect + kType) == 0 && R32(layout::kGame) && R32(layout::kWorld) && R32(layout::kDisplay);
}

bool WeaponDofEligible() {
    const auto& options = game::Opt();
    return WeaponDofAllowed() && !options.dof_preview && !IdleFocusActive() && g_preview_phase == 0 &&
           !g_preview.serial && options.dof_weapon_blur;
}

bool PreviewAllowed() { return WeaponDofAllowed(); }

bool PreviewDrawMatches(const PreviewDraw& draw) {
    return draw.serial && draw.frame == R32(kFrameCount) &&
           draw.manager == R32(kEffects) && draw.display == R32(layout::kDisplay) &&
           R32(draw.manager + kEffect + kType) == 0;
}

bool PreviewDrawPending() { return PreviewDrawMatches(g_preview); }

void BeginWeaponDof(X86Context& ctx, float strength, bool preview) {
    g_weapon_pending = false;
    uint32_t display = R32(layout::kDisplay);
    uint32_t list = display + layout::kDisplayDrawLists + layout::kGunDrawList * layout::kDrawListSize;
    if (!WeaponDofAllowed() || ctx.esi != list || !R32(list + 8) || strength <= 0) return;
    g_weapon_frame = R32(kFrameCount);
    if (++g_weapon_serial == 0) ++g_weapon_serial;
    g_weapon_manager = R32(kEffects);
    g_weapon_display = display;
    g_weapon_preview = preview;
    g_weapon_strength = strength;
    g_weapon_sharp_depth = float(game::Opt().dof_weapon_sharp_depth) / 100.0f;
    g_weapon_transition = float(game::Opt().dof_weapon_transition) / 100.0f;
    std::copy_n(&G<float>(kProjection), g_weapon_projection.size(), g_weapon_projection.begin());
    g_weapon_pending = d3d::BeginWeaponDepthOfField(g_weapon_projection.data(), g_weapon_strength, g_weapon_serial,
                                                  g_weapon_sharp_depth, g_weapon_transition);
}

void EndWeaponDof(bool preview) {
    bool pending = g_weapon_pending;
    g_weapon_pending = false;
    if (!pending || g_weapon_preview != preview || g_weapon_frame != R32(kFrameCount) ||
        g_weapon_manager != R32(kEffects) || g_weapon_display != R32(layout::kDisplay) || !WeaponDofAllowed()) return;
    d3d::EndWeaponDepthOfField(g_weapon_projection.data(), g_weapon_strength, g_weapon_serial,
                             g_weapon_sharp_depth, g_weapon_transition);
}
}  // namespace

// Entry to 98450 sees the native root time after fade offsets and 98040 loop normalization.
// Reading instance+14 directly would misidentify the inspection phase during a transition.
void ObserveIdleDof(X86Context& ctx, uint8_t*) {
    if (!PreviewAllowed()) return;
    uint32_t player = R32(layout::kWorld) + layout::kWorldPlayer;
    uint32_t controller = R32(player + 0x328);
    if (!controller || R32(ctx.esp + 4) != R32(controller + 0x54)) return;
    IdleSample sample;
    if (!IdleIdentity(sample) || R32(ctx.esp + 4) != sample.instance ||
        R32(ctx.esp + 8) != sample.root || R32(ctx.esp + 0xC) != sample.cache ||
        (R32(ctx.esp + 0x18) & 8) || sample.generation != R32(sample.instance + 0x980)) return;
    sample.frame = R32(kFrameCount);
    if (!InspectionPhase(sample.root, sample.cache, G<float>(ctx.esp + 0x10), sample.inspection)) {
        g_idle = {};
        return;
    }
    g_idle = sample;
}

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
        g_preview_phase = StepDofPreview(g_preview_phase, seconds, game::Opt().dof_preview || IdleFocusActive());
    } else {
        g_idle = {};
        g_preview_phase = 0;
        g_preview = {};
        g_preview_weapon = {};
    }
    g_preview_tick_frame = R32(kFrameCount);
    g_preview_clock = std::chrono::steady_clock::now();
    g_preview_clock_started = true;
}

// Keep Modern mode's weapon after the scene effects even at zero focus strength. Switching its draw
// stage at preview/idle endpoints abruptly changes post-effect lighting independently of the blur fade.
// These bool hooks change only the render branches; Off/Original and native effect clocks stay native.
bool DeferPreviewWeapon(X86Context&, uint8_t*) {
    g_preview = {};
    g_preview_weapon = {};
    g_weapon_pending = false;
    if (!PreviewAllowed() || !g_preview_clock_started || g_preview_tick_frame != R32(kFrameCount)) {
        auto now = std::chrono::steady_clock::now();
        float seconds = g_preview_clock_started ? std::chrono::duration<float>(now - g_preview_clock).count() : 0;
        TickDofPreview(seconds);
    }
    if (!PreviewAllowed()) return false;
    if (++g_preview_serial == 0) ++g_preview_serial;
    g_preview = {R32(kEffects), R32(layout::kDisplay), R32(kFrameCount), g_preview_serial,
                 CinematicDofStrength(g_preview_phase)};
    g_preview.weapon_strength = float(std::min(game::Opt().dof_weapon_blur, 100u)) / 100.0f *
                                (1.0f - g_preview.strength);
    return true;
}

bool DrawPreviewWeapon(X86Context&, uint8_t*) {
    bool deferred = PreviewDrawPending();
    g_preview_weapon = deferred ? g_preview : PreviewDraw{};
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

// After opaque world rendering and the early particles, before capture, later particle layers, bloom and HUD.
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

// Keep the native early hooks installed. Eligible Modern frames defer this draw; Off/Original and
// other native effect types fail blur eligibility. Native type-1 reloads also skip this early path.
void BeginWeaponDepthOfField(X86Context& ctx, uint8_t*) {
    g_weapon_pending = false;
    if (!WeaponDofEligible()) return;
    BeginWeaponDof(ctx, float(std::min(game::Opt().dof_weapon_blur, 100u)) / 100.0f, false);
}

void EndWeaponDepthOfField(X86Context&, uint8_t*) {
    EndWeaponDof(false);
}

// Modern mode uses the same isolated list-5 depth mask after world effects, including at zero focus.
// Its normal blur fades inversely to the latched world strength; native type-1 reloads remain sharp.
void BeginPreviewWeaponDepthOfField(X86Context& ctx, uint8_t*) {
    const PreviewDraw draw = g_preview_weapon;
    g_preview_weapon = {};
    g_weapon_pending = false;
    if (!PreviewDrawMatches(draw)) return;
    BeginWeaponDof(ctx, draw.weapon_strength, true);
}

void EndPreviewWeaponDepthOfField(X86Context&, uint8_t*) {
    EndWeaponDof(true);
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
