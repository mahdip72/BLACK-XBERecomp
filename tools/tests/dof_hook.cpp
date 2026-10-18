// CPU-only contract checks that compile the actual reload hook source with a
// guest-memory shim. The original game functions and render queue are stubs.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "common/options.h"
#include "game/layout.h"
#include "xdk/hle.h"

using namespace kernel;

void ReloadAnimationDof(X86Context& ctx, uint8_t* base);
void ReloadDepthOfField(X86Context& ctx, uint8_t* base);
void BeginWeaponDepthOfField(X86Context& ctx, uint8_t* base);
void EndWeaponDepthOfField(X86Context& ctx, uint8_t* base);
bool DeferPreviewWeapon(X86Context& ctx, uint8_t* base);
bool DrawPreviewWeapon(X86Context& ctx, uint8_t* base);
float StepDofPreview(float phase, float seconds, bool enabled);
void TickDofPreview(float seconds);
void sub_001772E0(X86Context& ctx, uint8_t* base);
float CinematicDofStrength(float strength);

namespace {
constexpr uint32_t kEffects = 0x2D1990;
constexpr uint32_t kManager = 0x300000;
constexpr uint32_t kEffect = kManager + 0x830A0;
constexpr uint32_t kGameObject = 0x400000;
constexpr uint32_t kDisplayObject = 0x600000;
constexpr uint32_t kGunList = kDisplayObject + layout::kDisplayDrawLists +
                              layout::kGunDrawList * layout::kDrawListSize;
constexpr uint32_t kProjection = 0x297720;
constexpr uint32_t kFrameCount = 0x2F4368;
constexpr uint32_t kStack = 0x1000;
alignas(4) std::array<uint8_t, 8 * 1024 * 1024> memory{};
game::Options options;
uint32_t original_new_type = 1;
int animation_calls = 0;
int original_effect_calls = 0;
int queue_calls = 0;
bool queue_accepts = true;
float queued_strength = -1;
int weapon_begin_calls = 0;
int weapon_end_calls = 0;
bool weapon_begin_accepts = true;
float weapon_begin_strength = -1;
float weapon_end_strength = -1;
uint32_t weapon_begin_serial = 0;
uint32_t weapon_end_serial = 0;
std::array<float, 16> weapon_begin_projection{};
std::array<float, 16> weapon_end_projection{};

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

X86Context Context() {
    return {kStack, 0x12345678, 0, 0xABCDEF01, 0x23456789,
            0x3456789A, 0x456789AB, 0x56789ABC, 0x202};
}

void Reset(uint32_t initial_type = 0) {
    R32(kEffects) = 0;
    auto ctx = Context();
    ReloadDepthOfField(ctx, nullptr); // clear the actual hook's previous tag
    EndWeaponDepthOfField(ctx, nullptr); // clear a previous weapon pair
    TickDofPreview(0); // clear the actual preview phase and deferred draw token
    std::fill(memory.begin(), memory.end(), uint8_t{0});
    R32(kEffects) = kManager;
    R32(layout::kGame) = kGameObject;
    R32(layout::kWorld) = 0x500000;
    R32(layout::kDisplay) = kDisplayObject;
    R32(kGunList + 8) = 1;
    R32(kGameObject + layout::kCurrentFilm) = 0;
    R32(kEffect + 0x34) = initial_type;
    R32(kEffect + 0x18) = 20;
    G<float>(kEffect + 0x1C) = 0.3f;
    G<float>(kEffect + 0x20) = 0.4f;
    G<float>(kEffect + 0x24) = 0.2f;
    G<float>(kEffect + 0x10) = 0.45f;
    for (unsigned i = 0; i < 16; ++i)
        G<float>(kProjection + 4 * i) = 1.0f + 0.01f * i;
    R32(kFrameCount) = 12;
    R32(kStack + 4) = kEffect;
    options.reload_blur = true;
    options.modern_reload_dof = true;
    options.dof_preview = false;
    options.dof_weapon_blur = 25;
    original_new_type = 1;
    animation_calls = original_effect_calls = queue_calls = 0;
    queue_accepts = true;
    queued_strength = -1;
    weapon_begin_calls = weapon_end_calls = 0;
    weapon_begin_accepts = true;
    weapon_begin_strength = weapon_end_strength = -1;
    weapon_begin_serial = weapon_end_serial = 0;
    weapon_begin_projection.fill(0);
    weapon_end_projection.fill(0);
}

X86Context GunContext() {
    auto ctx = Context();
    ctx.esi = kGunList;
    return ctx;
}

void BeginGun() {
    auto ctx = GunContext();
    BeginWeaponDepthOfField(ctx, nullptr);
}

void EndGun() {
    auto ctx = GunContext();
    EndWeaponDepthOfField(ctx, nullptr);
}

std::vector<uint8_t> GuestSnapshot() {
    return {memory.begin(), memory.end()};
}

void SameGuest(const std::vector<uint8_t>& before, const char* message) {
    Check(before.size() == memory.size() &&
          std::equal(before.begin(), before.end(), memory.begin()), message);
}

void TickPreview(float seconds) {
    const auto before = GuestSnapshot();
    TickDofPreview(seconds);
    SameGuest(before, "preview phase update mutated guest memory");
}

bool DeferPreview() {
    auto ctx = Context();
    const auto saved = ctx;
    const auto before = GuestSnapshot();
    const bool deferred = DeferPreviewWeapon(ctx, nullptr);
    SameGuest(before, "preview early route mutated guest memory");
    Check(std::memcmp(&ctx, &saved, sizeof(ctx)) == 0,
          "preview early route changed guest registers or flags");
    return deferred;
}

bool DrawPreview() {
    auto ctx = Context();
    const auto saved = ctx;
    const auto before = GuestSnapshot();
    const bool drawn = DrawPreviewWeapon(ctx, nullptr);
    SameGuest(before, "preview late route mutated guest memory");
    Check(std::memcmp(&ctx, &saved, sizeof(ctx)) == 0,
          "preview late route changed guest registers or flags");
    return drawn;
}

void StartReload() {
    auto ctx = Context();
    ReloadAnimationDof(ctx, nullptr);
    Check(animation_calls == 1, "animation original not called once");
}

void RenderFrame() {
    auto ctx = Context();
    ReloadDepthOfField(ctx, nullptr);
}

void PreviewRenderFrame() {
    const auto before = GuestSnapshot();
    RenderFrame();
    SameGuest(before, "preview world hook mutated guest memory");
}

void Effect(bool skipped) {
    auto ctx = Context();
    sub_001772E0(ctx, nullptr);
    Check(original_effect_calls == (skipped ? 0 : 1),
          "legacy effect forwarding did not match queued result");
    Check(ctx.esp == kStack + (skipped ? 8u : 0u), "legacy ret 4 stack adjustment wrong");
    Check(ctx.eax == 0x12345678, "hook damaged EAX");
}

void TestTagAndSkip() {
    Reset();
    StartReload();
    Check(G<float>(kEffect + 0x1C) == 0.8f &&
          G<float>(kEffect + 0x24) == 0.9f &&
          G<float>(kEffect + 0x20) == 0.4f,
          "Modern reload did not lengthen only ramp/fade");
    RenderFrame();
    Check(queue_calls == 1 &&
          std::abs(queued_strength - CinematicDofStrength(0.45f)) < 1e-6f,
          "valid reload did not queue eased strength");
    Effect(true);
    R32(kFrameCount)++;
    original_effect_calls = 0;
    Effect(false);
    std::puts("PASS idle-to-reload tag, same-frame ret 4 skip, stale-frame fallback");
}

void TestUntaggedAndInvalidated() {
    Reset(1); // a scripted type-1 effect already active before the animation call
    StartReload();
    RenderFrame();
    Check(queue_calls == 0, "untagged effect queued DOF");
    Effect(false);

    Reset();
    original_new_type = 2;
    StartReload();
    RenderFrame();
    Check(queue_calls == 0, "non-reload type queued DOF");

    Reset();
    StartReload();
    R32(kEffect + 0x18)++;
    RenderFrame();
    Check(queue_calls == 0, "changed effect timing kept reload tag");
    Effect(false);

    Reset();
    StartReload();
    R32(kEffect + 0x34) = 2;
    RenderFrame();
    Check(queue_calls == 0, "changed effect type kept reload tag");

    std::puts("PASS scripted effect isolation and type/timing invalidation");
}

void TestFilmIndicesAndProvenance() {
    // kCurrentFilm is a retained/requested film index. Gameplay can hold a
    // valid index here; it is not a flag that disables the reload effect.
    for (uint32_t film : {0u, 1u, layout::kFilms - 1}) {
        Reset();
        R32(kGameObject + layout::kCurrentFilm) = film;
        StartReload();
        RenderFrame();
        Check(queue_calls == 1, "valid retained film index disabled reload DOF");
        Effect(true);
    }
    Reset();
    StartReload();
    R32(layout::kWorld) = 0;
    RenderFrame();
    Check(queue_calls == 0, "missing world did not invalidate reload tag");
    Effect(false);

    Reset();
    StartReload();
    R32(kEffects) = kManager + 0x1000;
    RenderFrame();
    Check(queue_calls == 0, "different effect manager kept reload tag");
    Effect(false);
    std::puts("PASS valid film indices and world/manager provenance guards");
}

void TestFallbacks() {
    Reset();
    options.modern_reload_dof = false;
    StartReload();
    Check(G<float>(kEffect + 0x1C) == 0.3f &&
          G<float>(kEffect + 0x24) == 0.2f,
          "Original mode modified stock durations");
    RenderFrame();
    Check(queue_calls == 0, "Original mode queued modern DOF");
    Effect(false);

    Reset();
    options.reload_blur = false;
    StartReload();
    Check(G<float>(kEffect + 0x1C) == 0.3f &&
          G<float>(kEffect + 0x24) == 0.2f,
          "Off mode modified stock durations");
    RenderFrame();
    Check(queue_calls == 0, "Off mode queued modern DOF");
    Effect(false);

    Reset();
    StartReload();
    queue_accepts = false;
    RenderFrame();
    Check(queue_calls == 1, "rejected queue was not attempted");
    Effect(false);

    Reset();
    StartReload();
    G<float>(kEffect + 0x10) = NAN;
    RenderFrame();
    Check(queue_calls == 0, "nonfinite strength reached render queue");
    Effect(false);
    std::puts("PASS Original/Off, queue rejection, and invalid-strength legacy fallback");
}

void TestEnvelope() {
    Check(CinematicDofStrength(-1) == 0 && CinematicDofStrength(0) == 0 &&
          CinematicDofStrength(1) == 1 && CinematicDofStrength(2) == 1 &&
          CinematicDofStrength(NAN) == 0, "strength envelope did not clamp");
    Check(std::abs(CinematicDofStrength(0.25f) - 0.103515625f) < 1e-6f &&
          std::abs(CinematicDofStrength(0.5f) - 0.5f) < 1e-6f &&
          std::abs(CinematicDofStrength(0.75f) - 0.896484375f) < 1e-6f,
          "strength envelope is not smooth quintic easing");
    float previous = 0;
    for (int i = 1; i <= 100; ++i) {
        const float current = CinematicDofStrength(i / 100.0f);
        Check(current >= previous, "strength envelope decreased during ramp");
        previous = current;
    }
    std::puts("PASS production quintic strength envelope and Modern-only durations");
}

void TestPreviewEnvelope() {
    float phase = 0;
    float last = 0;
    for (int i = 0; i < 8; ++i) {
        phase = StepDofPreview(phase, 0.1f, true);
        const float eased = CinematicDofStrength(phase);
        Check(phase > 0 && eased >= last, "preview ramp reversed or failed to advance");
        last = eased;
    }
    Check(std::abs(phase - 1) < 1e-5f && last == 1,
          "preview ramp did not reach full strength in 0.8 seconds");
    for (int i = 0; i < 9; ++i) {
        const float next = StepDofPreview(phase, 0.1f, false);
        Check(next <= phase, "preview fade reversed while disabling");
        phase = next;
    }
    Check(std::abs(phase) < 1e-5f, "preview fade did not finish in 0.9 seconds");
    phase = StepDofPreview(0, 0.1f, true);
    const float half_ramp = StepDofPreview(phase, 0.1f, true);
    const float reversed = StepDofPreview(half_ramp, 0.1f, false);
    Check(reversed > 0 && reversed < half_ramp,
          "mid-ramp disable did not reverse from its current phase");
    Check(StepDofPreview(reversed, 0.1f, true) > reversed,
          "mid-fade enable did not reverse from its current phase");
    Check(StepDofPreview(0.5f, NAN, true) == 0.5f &&
          StepDofPreview(0.5f, -1, false) == 0.5f &&
          StepDofPreview(0.5f, 10, true) <= 0.62501f,
          "preview step accepted invalid or unbounded elapsed time");
    std::puts("PASS production preview ramp/fade endpoints, reversal, and bounded time step");
}

void AdvancePreview(unsigned frames, bool enabled) {
    options.dof_preview = enabled;
    for (unsigned i = 0; i < frames; ++i) {
        R32(kFrameCount)++;
        TickPreview(0.1f);
    }
}

void TestPreviewIdleAndReloadPriority() {
    Reset();
    AdvancePreview(8, true);
    PreviewRenderFrame();
    Check(queue_calls == 0, "preview queued world blur without an early gun route");
    Check(DeferPreview(), "steady preview did not defer the early gun");
    BeginGun(); EndGun();
    Check(weapon_begin_calls == 0 && weapon_end_calls == 0,
          "preview blurred the normal weapon before late draw");
    PreviewRenderFrame();
    Check(queue_calls == 1 && queued_strength == 1,
          "steady preview did not queue full eased world strength");
    Effect(false); // preview must not synthesize reload provenance or suppress stock effects
    Check(DrawPreview() && !DrawPreview(),
          "preview late gun was not drawn exactly once");
    Check(R32(kEffect + 0x34) == 0 && R32(kEffect + 0x18) == 20 &&
          G<float>(kEffect + 0x1C) == 0.3f &&
          G<float>(kEffect + 0x20) == 0.4f &&
          G<float>(kEffect + 0x24) == 0.2f &&
          G<float>(kEffect + 0x10) == 0.45f,
          "idle preview changed native effect state or timing");

    Reset();
    AdvancePreview(8, true);
    Check(DeferPreview(), "preview early route was not available before reload");
    StartReload(); // actual native type-1 transition must take priority
    PreviewRenderFrame();
    Check(queue_calls == 1 &&
          std::abs(queued_strength - CinematicDofStrength(0.45f)) < 1e-6f,
          "real reload did not take priority over preview strength");
    Effect(true);
    Check(!DrawPreview(), "preview drew a second gun after real reload began");
    std::puts("PASS idle preview strength, no guest changes, sharp gun, and real reload priority");
}

void TestPreviewGuards() {
    auto denied = [](const char* message) {
        TickPreview(0.1f);
        Check(!DeferPreview(), message);
        PreviewRenderFrame();
        Check(queue_calls == 0 && !DrawPreview(), message);
    };
    Reset(); denied("preview default-on regression or missing early-route guard");
    Reset(); options.dof_preview = true; options.reload_blur = false;
    denied("Off mode entered preview");
    Reset(); options.dof_preview = true; options.modern_reload_dof = false;
    denied("Original mode entered preview");
    Reset(); options.dof_preview = true; R32(kEffects) = 0;
    denied("missing effect manager entered preview");
    Reset(); options.dof_preview = true; R32(layout::kGame) = 0;
    denied("missing game entered preview");
    Reset(); options.dof_preview = true; R32(layout::kWorld) = 0;
    denied("missing world entered preview");
    Reset(); options.dof_preview = true; R32(layout::kDisplay) = 0;
    denied("missing display entered preview");
    Reset(1); options.dof_preview = true;
    denied("untagged type-1 effect entered preview");
    Effect(false);
    std::puts("PASS preview default-off, mode, world/display and type provenance guards");
}

void TestPreviewFadeAndCancellation() {
    Reset();
    AdvancePreview(8, true);
    AdvancePreview(1, false);
    Check(DeferPreview(), "fade-out prematurely restored early gun route");
    BeginGun(); EndGun();
    Check(weapon_begin_calls == 0 && weapon_end_calls == 0,
          "fade-out blurred the normal weapon");
    PreviewRenderFrame();
    Check(queue_calls == 1 && queued_strength > 0 && queued_strength < 1,
          "fade-out did not queue partial eased preview strength");
    Check(DrawPreview(), "fade-out lost deferred sharp gun draw");
    AdvancePreview(8, false);
    Check(!DeferPreview(), "expired preview still deferred the gun");
    BeginGun(); EndGun();
    Check(weapon_begin_calls == 1 && weapon_end_calls == 1,
          "normal weapon blur did not resume after preview faded out");

    Reset(); AdvancePreview(4, true);
    Check(DeferPreview(), "partial preview failed to defer gun");
    options.dof_preview = false;
    queue_accepts = false;
    PreviewRenderFrame();
    Check(queue_calls == 1 && queued_strength > 0 && queued_strength < 1,
          "preview token did not preserve partial strength across UI toggle");
    Check(DrawPreview(), "UI-off or queue rejection lost deferred gun draw");

    Reset(); options.dof_preview = true; TickPreview(0.1f);
    Check(DeferPreview(), "preview did not create cancellation token");
    R32(kFrameCount)++;
    Check(!DrawPreview(), "stale-frame preview token drew a weapon");
    Reset(); options.dof_preview = true; TickPreview(0.1f);
    Check(DeferPreview(), "preview did not create manager cancellation token");
    R32(kEffects) = kManager + 0x1000;
    Check(!DrawPreview(), "different manager retained preview draw token");
    Reset(); options.dof_preview = true; TickPreview(0.1f);
    Check(DeferPreview(), "preview did not create type cancellation token");
    R32(kEffect + 0x34) = 1;
    Check(!DrawPreview(), "type change retained preview draw token");
    Reset(); AdvancePreview(4, true);
    options.modern_reload_dof = false;
    TickPreview(0.1f); // a non-Modern mode clears the preview phase, not just its target
    Check(!DeferPreview(), "Original mode retained preview phase or route");
    options.modern_reload_dof = true;
    options.dof_preview = false;
    TickPreview(0.1f);
    BeginGun(); EndGun();
    Check(weapon_begin_calls == 1 && weapon_end_calls == 1,
          "normal weapon blur did not resume after non-Modern preview reset");
    Reset(); AdvancePreview(4, true);
    Check(DeferPreview(), "preview did not create world cancellation token");
    R32(layout::kWorld) = 0;
    TickPreview(0.1f);
    Check(!DrawPreview(), "missing world retained deferred preview gun");
    std::puts("PASS preview fade-out, queue/UI resilience and route cancellation");
}

void TestWeaponPair() {
    Reset();
    BeginGun();
    Check(weapon_begin_calls == 1 && weapon_end_calls == 0,
          "eligible normal weapon did not begin exactly once");
    Check(std::abs(weapon_begin_strength - 0.25f) < 1e-6f,
          "normal weapon amount was not 25 percent");
    Check(weapon_begin_serial != 0, "weapon pair serial was not assigned");
    const auto saved = weapon_begin_projection;
    G<float>(kProjection + 4) = 999;
    options.dof_weapon_blur = 50; // still eligible; End must use Begin's snapshot
    EndGun();
    Check(weapon_end_calls == 1 && weapon_end_serial == weapon_begin_serial &&
          std::abs(weapon_end_strength - 0.25f) < 1e-6f &&
          weapon_end_projection == saved,
          "weapon End did not use Begin's serial, amount, and projection snapshot");
    EndGun();
    Check(weapon_end_calls == 1, "weapon End consumed one pair more than once");

    Reset();
    options.dof_weapon_blur = 150;
    BeginGun();
    Check(weapon_begin_calls == 1 && weapon_begin_strength == 1.0f,
          "weapon amount was not capped at 100 percent");
    EndGun();
    Check(weapon_end_calls == 1, "capped weapon pair did not end");

    Reset();
    weapon_begin_accepts = false;
    BeginGun();
    EndGun();
    Check(weapon_begin_calls == 1 && weapon_end_calls == 0,
          "rejected weapon Begin still issued an End");
    std::puts("PASS normal weapon Begin/End pair, saved payload, one-shot and rejection");
}

void TestWeaponEligibility() {
    auto denied = [](const char* message) {
        BeginGun();
        EndGun();
        Check(weapon_begin_calls == 0 && weapon_end_calls == 0, message);
    };
    Reset(); options.reload_blur = false;
    denied("Off mode blurred normal weapon");
    Reset(); options.modern_reload_dof = false;
    denied("Original mode blurred normal weapon");
    Reset(); options.dof_weapon_blur = 0;
    denied("zero weapon amount still began a blur");
    Reset(); R32(kEffect + 0x34) = 1;
    denied("reload type-1 late weapon received normal-weapon blur");
    Reset(); R32(kEffects) = 0;
    denied("missing effect manager began weapon blur");
    Reset(); R32(layout::kGame) = 0;
    denied("missing game began weapon blur");
    Reset(); R32(layout::kWorld) = 0;
    denied("missing world began weapon blur");
    Reset(); R32(layout::kDisplay) = 0;
    denied("missing display began weapon blur");
    Reset(); R32(kGunList + 8) = 0;
    denied("empty gun draw list began weapon blur");
    Reset();
    auto wrong_list = Context();
    wrong_list.esi = kGunList + layout::kDrawListSize;
    BeginWeaponDepthOfField(wrong_list, nullptr);
    EndGun();
    Check(weapon_begin_calls == 0 && weapon_end_calls == 0,
          "different draw list began normal-weapon blur");

    Reset();
    BeginGun();
    R32(kFrameCount)++;
    EndGun();
    Check(weapon_begin_calls == 1 && weapon_end_calls == 0,
          "stale-frame weapon End was submitted");
    Reset();
    BeginGun();
    options.modern_reload_dof = false;
    EndGun();
    Check(weapon_begin_calls == 1 && weapon_end_calls == 0,
          "mode change did not cancel pending weapon End");
    Reset();
    BeginGun();
    auto wrong_begin = Context();
    BeginWeaponDepthOfField(wrong_begin, nullptr);
    EndGun();
    Check(weapon_begin_calls == 1 && weapon_end_calls == 0,
          "new invalid Begin did not cancel prior weapon pair");
    std::puts("PASS weapon eligibility, draw-list provenance and stale-pair cancellation");
}
} // namespace

namespace kernel {
uint8_t* TestGuestAddress(uint32_t address) {
    if (address + 4 > memory.size()) throw std::runtime_error("guest fixture address out of range");
    return memory.data() + address;
}
} // namespace kernel

namespace game {
Options& Opt() { return options; }
} // namespace game

namespace d3d {
bool DepthOfField(const float* projection, float strength) {
    Check(projection == &G<float>(kProjection), "queue did not receive guest projection");
    ++queue_calls;
    queued_strength = strength;
    return queue_accepts;
}
bool BeginWeaponDepthOfField(const float* projection, float strength, uint32_t serial) {
    ++weapon_begin_calls;
    weapon_begin_strength = strength;
    weapon_begin_serial = serial;
    std::copy_n(projection, weapon_begin_projection.size(), weapon_begin_projection.begin());
    return weapon_begin_accepts;
}
bool EndWeaponDepthOfField(const float* projection, float strength, uint32_t serial) {
    ++weapon_end_calls;
    weapon_end_strength = strength;
    weapon_end_serial = serial;
    std::copy_n(projection, weapon_end_projection.size(), weapon_end_projection.begin());
    return true;
}
} // namespace d3d

void __imp__sub_0017B130(X86Context&, uint8_t*) {
    ++animation_calls;
    R32(kEffect + 0x34) = original_new_type;
}

void __imp__sub_001772E0(X86Context&, uint8_t*) { ++original_effect_calls; }

int main() {
    try {
        TestTagAndSkip();
        TestUntaggedAndInvalidated();
        TestFilmIndicesAndProvenance();
        TestFallbacks();
        TestEnvelope();
        TestPreviewEnvelope();
        TestPreviewIdleAndReloadPriority();
        TestPreviewGuards();
        TestPreviewFadeAndCancellation();
        TestWeaponPair();
        TestWeaponEligibility();
        std::puts("PASS production reload DOF hook checks");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL production reload DOF hook checks: %s\n", error.what());
        return 1;
    }
}
