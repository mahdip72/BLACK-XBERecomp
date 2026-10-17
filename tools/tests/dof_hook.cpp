// CPU-only contract checks that compile the actual reload hook source with a
// guest-memory shim. The original game functions and render queue are stubs.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

#include "common/options.h"
#include "game/layout.h"
#include "xdk/hle.h"

using namespace kernel;

void ReloadAnimationDof(X86Context& ctx, uint8_t* base);
void ReloadDepthOfField(X86Context& ctx, uint8_t* base);
void sub_001772E0(X86Context& ctx, uint8_t* base);
float CinematicDofStrength(float strength);

namespace {
constexpr uint32_t kEffects = 0x2D1990;
constexpr uint32_t kManager = 0x300000;
constexpr uint32_t kEffect = kManager + 0x830A0;
constexpr uint32_t kGameObject = 0x400000;
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

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

X86Context Context() { return {kStack, 0x12345678}; }

void Reset(uint32_t initial_type = 0) {
    R32(kEffects) = 0;
    auto ctx = Context();
    ReloadDepthOfField(ctx, nullptr); // clear the actual hook's previous tag
    std::fill(memory.begin(), memory.end(), uint8_t{0});
    R32(kEffects) = kManager;
    R32(layout::kGame) = kGameObject;
    R32(layout::kWorld) = 0x500000;
    R32(kGameObject + layout::kCurrentFilm) = 0;
    R32(kEffect + 0x34) = initial_type;
    R32(kEffect + 0x18) = 20;
    G<float>(kEffect + 0x1C) = 0.3f;
    G<float>(kEffect + 0x20) = 0.4f;
    G<float>(kEffect + 0x24) = 0.2f;
    G<float>(kEffect + 0x10) = 0.45f;
    G<float>(kProjection) = 1.0f;
    R32(kFrameCount) = 12;
    R32(kStack + 4) = kEffect;
    options.reload_blur = true;
    options.modern_reload_dof = true;
    original_new_type = 1;
    animation_calls = original_effect_calls = queue_calls = 0;
    queue_accepts = true;
    queued_strength = -1;
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
        std::puts("PASS production reload DOF hook checks");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL production reload DOF hook checks: %s\n", error.what());
        return 1;
    }
}
