// Prepare graphics while the native mission loading screen continues ticking and presenting.
#include "d3d/dof_loading.h"
#include "game/layout.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// 0x20236 constructs the mission state with this vtable; its load method is 0x20CE00.
constexpr uint32_t kMissionState = 0x20F78;
constexpr uint32_t kMissionVtable = 0x295338;
constexpr uint32_t kPendingState = 0x21074;
constexpr uint32_t kTransitionPhase = 0x21084;
constexpr uint32_t kLoadStage = 4;

uint32_t g_loading_game = 0;
uint64_t g_loading_generation = 0;

bool MissionLoading(uint32_t game) {
    if (!game || game != R32(layout::kGame)) return false;
    const uint32_t phase = R32(game + kTransitionPhase);
    return (phase == 1 || phase == 2) &&
        R32(game + kPendingState) == game + kMissionState &&
        R32(game + kMissionState) == kMissionVtable;
}

void CancelLoading() {
    d3d::EndMissionDofPreparation(g_loading_generation);
    g_loading_generation = 0;
    g_loading_game = 0;
}

}  // namespace

// Before 0x20F6E5 dispatches the transition phase, ESI is the game object.
// Both new missions (0x20DCA0) and same-state restarts (0x20DBC0) reach this path.
void ObserveMissionDofLoading(X86Context& ctx, uint8_t*) {
    if (!MissionLoading(ctx.esi)) {
        CancelLoading();
        return;
    }
    if (g_loading_game == ctx.esi) return;
    CancelLoading();
    g_loading_game = ctx.esi;
    g_loading_generation = d3d::BeginMissionDofPreparation();
}

// 0x20CE78 is after assets and UI are ready, before the native brightness fade.
// False takes 0x20CEB2's balanced AL=0 return, leaving stage 4 and the artwork alive.
// Its next poll repeats only terminal UI readiness (0x115A40), not world loading.
bool MissionDofLoadingReady(X86Context& ctx, uint8_t*) {
    if (!g_loading_game || !MissionLoading(g_loading_game) ||
        ctx.ebp != g_loading_game + kMissionState ||
        R32(g_loading_game + kTransitionPhase) != 1 ||
        R32(ctx.ebp + 4) != kLoadStage) {
        CancelLoading();
        return true;
    }
    return d3d::MissionDofPreparationReady(g_loading_generation);
}
