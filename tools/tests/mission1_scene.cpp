// Compiled only into the explicit hidden Mission 1 benchmark executable.
#include "common/mission_benchmark.h"

#if defined(BLACK_MISSION_BENCHMARK)
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>

#include "core/log.h"
#include "game/layout.h"
#include "xdk/hle.h"

namespace game::bench {
namespace {
using namespace kernel;

constexpr uint32_t kMissionState = 0x20F78;
constexpr uint32_t kMissionVtable = 0x295338;
constexpr uint32_t kPendingState = 0x21074;
constexpr uint32_t kTransitionPhase = 0x21084;
constexpr uint32_t kFrame = 0x2F4368;
constexpr uint32_t kProfileSet = 0x2D19B4;
constexpr uint32_t kProfileManager = 0x2D19BC;
constexpr uint32_t kInputManager = 0x2D45E8;
constexpr uint32_t kActiveInputMap = 0x2F3BD4;
constexpr uint32_t kCamera = 0x2D12B8;

std::atomic<bool> g_ready{false};
std::atomic<uint32_t> g_frame{0};
std::atomic<uint64_t> g_camera_signature{0};
uint32_t g_requested_game = 0;
uint32_t g_profile_requested_game = 0;
uint64_t g_last_camera = 0;
uint32_t g_camera_changes = 0;

struct State {
    uint32_t game = 0, current = 0, pending = 0, phase = 0;
    uint32_t frontend = 0, ui = 0, page = 0, asked = 0, leaving = 0;
    uint32_t profiles = 0, active_profile = 0, profile_manager = 0, profile_stage = 0;
    uint32_t profile_command = 0, profile_load = 0, profile_done = 0, profile_loaded = 0, profile_slot = 0;
    uint32_t input_manager = 0, active_pad = 0, input_map = 0;
    uint32_t world = 0, world_stage = 0, world_running = 0, mission_stage = 0, mission = 0, block = 0;
    bool operator==(const State&) const = default;
};

State ReadState(uint32_t game) {
    State s;
    s.game = game;
    if (!game) return s;
    s.current = R32(game + layout::kGameState);
    s.pending = R32(game + kPendingState);
    s.phase = R32(game + kTransitionPhase);
    s.frontend = R32(game + layout::kFrontEnd + 4);
    s.mission_stage = R32(game + kMissionState + 4);
    s.mission = G<uint8_t>(game + 0x2020C);
    s.block = G<uint8_t>(game + 0x2020D);
    s.ui = R32(layout::kUi);
    if (s.ui) {
        s.page = R32(s.ui + layout::kPageReady);
        s.asked = R32(s.ui + layout::kPageAsked);
        s.leaving = R32(s.ui + layout::kPageLeaving);
    }
    s.profiles = R32(kProfileSet);
    s.active_profile = s.profiles ? R32(s.profiles) : 0;
    s.profile_manager = R32(kProfileManager);
    s.profile_stage = s.profile_manager ? R32(s.profile_manager + 4) : 0;
    if (s.profile_manager) {
        s.profile_command = R32(s.profile_manager + 0x0C);
        s.profile_load = R32(s.profile_manager + 0x10);
        s.profile_done = G<uint8_t>(s.profile_manager + 0x515);
        s.profile_loaded = G<uint8_t>(s.profile_manager + 0x510);
        s.profile_slot = R32(s.profile_manager + 0x524);
    }
    s.input_manager = R32(kInputManager);
    s.active_pad = R32(game + 0x21060);
    s.input_map = R32(kActiveInputMap);
    s.world = R32(layout::kWorld);
    s.world_stage = s.world ? R32(s.world + 0x5AE0) : 0;
    s.world_running = s.world ? G<uint8_t>(s.world + 0x28) : 0;
    return s;
}

void ReportState(const State& state, const char* reason) {
    static State previous;
    static const char* previous_reason = nullptr;
    static uint32_t reports = 0;
    if (state == previous && reason == previous_reason) return;
    previous = state;
    previous_reason = reason;
    if (reports++ >= 192) return;
    LOGR("mission benchmark scene: %s game=%08X current=%08X pending=%08X phase=%u "
         "frontend=%X ui=%08X page=%u asked=%u leaving=%u profiles=%08X active=%08X "
         "profile_manager=%08X profile_stage=%X world=%08X world_stage=%X world_running=%u "
         "mission_stage=%X mission=%u block=%u profile_command=%u load=%u done=%u loaded=%u slot=%u "
         "input=%08X pad=%08X map=%08X",
         reason, state.game, state.current, state.pending, state.phase, state.frontend,
         state.ui, state.page, state.asked, state.leaving, state.profiles, state.active_profile,
         state.profile_manager, state.profile_stage, state.world, state.world_stage, state.world_running,
         state.mission_stage, state.mission, state.block, state.profile_command, state.profile_load,
         state.profile_done, state.profile_loaded, state.profile_slot,
         state.input_manager, state.active_pad, state.input_map);
}

struct RestoreContext {
    X86Context& target;
    X86Context saved;
    ~RestoreContext() { target = saved; }
};

bool BindPortZero(X86Context& ctx, const State& state) {
    if (!state.input_manager) return false;
    uint32_t pad = state.input_manager + 0x518;
    if (!R32(pad + 0x0C)) return false;  // native device record initialized by 204E30
    if (state.active_pad == pad && state.input_map == state.game + 0x20174) return true;
    if (state.active_pad && state.active_pad != pad) {
        Stop("native controller selection was not port zero");
        return false;
    }
    // 11A750 checks port 0's +AC/+D8/+A8 before any other port. Give only that
    // synchronous native selector a nonzero +A8 word, restoring it before the
    // game/input/UI tick resumes. This produces no OS or host pad event, and no
    // menu/navigation/aim input survives the call. The native handler itself sets
    // game+21060, pad+8, UI+450 and global2F3BD4 (11A7B6..11A7D8).
    struct RestoreWord {
        uint32_t address;
        uint16_t saved;
        ~RestoreWord() { G<uint16_t>(address) = saved; }
    } restore_word{pad + 0xA8, G<uint16_t>(pad + 0xA8)};
    RestoreContext restore_context{ctx, ctx};
    G<uint16_t>(pad + 0xA8) = 1;
    uint32_t result = CallGuest(ctx, 0x11A750, {});
    bool bound = (result & 0xFF) == 1 && R32(state.game + 0x21060) == pad &&
                 R32(kActiveInputMap) == state.game + 0x20174;
    if (!bound) {
        Stop("native port-zero controller selection failed");
        return false;
    }
    LOGR("mission benchmark scene: native controller bound port=0 pad=%08X map=%08X selector=0011A750",
         pad, R32(kActiveInputMap));
    return true;
}

bool LoadedProfile(const State& state) {
    return state.profile_done == 1 && state.profile_loaded == 1 && state.profile_slot < 3 &&
           state.active_profile && state.input_manager &&
           state.active_pad == state.input_manager + 0x518 &&
           state.input_map == state.game + 0x20174;
}

bool PrepareNativeProfile(X86Context& ctx, const State& state) {
    if (!BindPortZero(ctx, state)) return false;
    if (LoadedProfile(state)) return true;
    if (g_profile_requested_game != state.game) {
        // Command 1 runs the native asynchronous autoload state machine 1FEFB0
        // through ordinary 204C90 ticks. Success at 1FF184..1FF1BC copies the
        // validated saved profile, calls 1FB300 (including its control-map copy),
        // then publishes loaded=1 and the selected slot. No fabricated defaults.
        if (state.profile_command == 1 && !state.profile_done) {
            g_profile_requested_game = state.game;
            LOGR("mission benchmark scene: observing native profile autoload already in progress");
        } else if (!state.profile_command || state.profile_done) {
            RestoreContext restore{ctx, ctx};
            ctx.eax = 1;
            CallGuest(ctx, 0x1F7460, {});
            g_profile_requested_game = state.game;
            LOGR("mission benchmark scene: native profile autoload requested command=1 manager=%08X",
                 state.profile_manager);
        }
    } else if (state.profile_done) {
        Stop("native profile autoload completed without a valid saved profile");
    }
    return false;
}

// Mirrors the read-only descriptor lookup at 1F4830. Mission 1 is native index 0:
// 1132E0 writes {0,1,1}; 1EDD5E..1EDD8A transfer it to the world and resolve this table.
uint32_t MissionDescriptor(uint32_t game) {
    uint32_t table = R32(game + 0x2106C);
    if (!table) return 0;
    uint32_t count = G<uint8_t>(table);
    uint32_t records = R32(table + 4);
    if (!records || !count || count > 32) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t record = records + i * 0x24;
        if (G<uint8_t>(record + 0x10) == 0 && G<int8_t>(record + 0x11) >= 1) return record;
    }
    return 0;
}

void StopNonBackgroundFilm(uint32_t game) {
    // The same native stop request and state bounds used by SkipVideo. Do not
    // tear down a movie or alter its clock; the frontend consumes this byte.
    uint32_t film = R32(game + layout::kCurrentFilm);
    uint32_t player = game + layout::kFrontEnd + layout::kFilmPlayer;
    uint32_t state = R32(player + 0x1A8);
    if (film >= layout::kFilms || state < 2 || state > 0x1C ||
        G<uint8_t>(layout::kFilmTable + film * layout::kFilmEntry + layout::kFilmBehindMenu)) return;
    if (!G<uint8_t>(player + 0x1B9)) {
        LOGR("mission benchmark scene: request native film stop film=%u state=%X", film, state);
        G<uint8_t>(player + 0x1B9) = 1;
    }
}

// 203518..203548 store the final composed camera transform at camera+7E0,
// then 20356E/20357F/203596 copy its position/basis into the render scene.
// Hash meaningful xyz lanes plus FOV, excluding SIMD padding. No camera writes.
uint64_t ReadCamera(uint32_t camera, std::array<float, 13>& values) {
    if (!camera) return 0;
    for (uint32_t row = 0; row < 4; ++row) {
        float length_squared = 0;
        for (uint32_t axis = 0; axis < 3; ++axis) {
            float value = G<float>(camera + 0x7E0 + row * 16 + axis * 4);
            if (!std::isfinite(value) || std::abs(value) > 1.0e7f) return 0;
            values[row * 3 + axis] = value;
            length_squared += value * value;
        }
        if (row < 3 && (length_squared < 0.5f || length_squared > 1.5f)) return 0;
    }
    values[12] = G<float>(camera + 0x750);
    if (!std::isfinite(values[12]) || values[12] <= 1 || values[12] >= 179) return 0;
    uint64_t hash = 14695981039346656037ull;
    for (float value : values) {
        hash ^= std::bit_cast<uint32_t>(value);
        hash *= 1099511628211ull;
    }
    return hash ? hash : 1;
}

}  // namespace

bool SceneReady() { return g_ready.load(std::memory_order_acquire); }
uint32_t SceneFrame() { return g_frame.load(std::memory_order_acquire); }
uint64_t CameraSignature() { return g_camera_signature.load(std::memory_order_acquire); }

void Tick(X86Context& ctx) {
    if (!Enabled()) return;
    uint32_t game = R32(layout::kGame);
    State state = ReadState(game);
    if (!game || ctx.esi != game) {
        g_ready.store(false, std::memory_order_release);
        ReportState(state, "waiting for game tick");
        return;
    }
    StopNonBackgroundFilm(game);
    if (!g_requested_game) {
        // Startup 20CAC0 initializes profile storage through 1F6F10/1FB770.
        // Those defaults alone do not select a controller or apply saved control
        // settings: complete both native paths before requesting the mission.
        if (state.current != game + layout::kFrontEnd || state.pending || state.phase ||
            state.frontend != 0x1C) {
            ReportState(state, "waiting for initialized frontend");
            return;
        }
        if (!state.active_profile || state.profile_stage != 0x1C) {
            ReportState(state, "waiting for native profile initialization");
            return;
        }
        if (!state.ui || state.page != layout::kPageRunning || state.asked || state.leaving ||
            !G<uint8_t>(state.ui + layout::kUiReady)) {
            ReportState(state, "waiting for settled frontend page");
            return;
        }
        if (!PrepareNativeProfile(ctx, state)) {
            ReportState(state, "waiting for native controller binding and saved profile autoload");
            return;
        }
        if (g_profile_requested_game) {
            LOGR("mission benchmark scene: native profile autoload complete profile=%08X id=%08X "
                 "slot=%u done=%u loaded=%u map=%08X pause=%u cancel=%u",
                 state.active_profile, R32(state.active_profile), state.profile_slot, state.profile_done,
                 state.profile_loaded, state.input_map, R32(state.input_map + 0x64), R32(state.input_map + 0x90));
            g_profile_requested_game = 0;
        }
        uint32_t descriptor = MissionDescriptor(game);
        if (!descriptor || R32(game + kMissionState) != kMissionVtable) {
            ReportState(state, "waiting for Mission 1 descriptor");
            return;
        }
        // No input synthesis: this is the native video-free Mission 1 selector.
        // Its deferred request is consumed by 124330 -> 1244B9 -> 20DCA0, which
        // destroys the current page and enters the ordinary mission loading path.
        RestoreContext restore{ctx, ctx};
        CallGuest(ctx, 0x1132E0, {});
        g_requested_game = game;
        LOGR("mission benchmark scene: native Mission 1 request selector=001132E0 descriptor=%08X "
             "profile=%08X mission=%u block=%u initial=%u", descriptor, state.active_profile,
             G<uint8_t>(game + 0x2020C), G<uint8_t>(game + 0x2020D), G<uint8_t>(game + 0x2020E));
        return;
    }
    if (game != g_requested_game || state.current != game + kMissionState ||
        R32(game + kMissionState) != kMissionVtable || state.pending || state.phase ||
        state.mission_stage != 0x1C || !LoadedProfile(state) ||
        !state.world || state.world_stage != 0x1C || !state.world_running ||
        state.mission != 0 || state.block != 1 || G<uint8_t>(state.world + 0x5AEC) != 0 ||
        R32(state.world + 0x5AF0) != 1) {
        g_ready.store(false, std::memory_order_release);
        ReportState(state, "waiting for active Mission 1 opening block");
        return;
    }
    uint32_t player = state.world + layout::kWorldPlayer;
    uint32_t camera = R32(kCamera);
    std::array<float, 13> view{};
    uint64_t signature = ReadCamera(camera, view);
    // The player is embedded at world+30, not a pointer stored there. The native
    // animation controller owns that exact player; it must exist before sampling.
    uint32_t controller = R32(player + 0x328);
    if (!R32(player) || !controller || R32(controller) != player || !signature) {
        g_ready.store(false, std::memory_order_release);
        ReportState(state, "waiting for player and finite camera");
        return;
    }
    uint32_t frame = R32(kFrame);
    g_camera_signature.store(signature, std::memory_order_release);
    g_frame.store(frame, std::memory_order_release);
    if (!g_ready.exchange(true, std::memory_order_acq_rel)) {
        ReportState(state, "Mission 1 scene ready");
        LOGR("mission benchmark scene: ready frame=%u world=%08X player=%08X controller=%08X "
             "camera=%08X signature=%016llX position=(%.6f,%.6f,%.6f) fov=%.6f",
             frame, state.world, player, controller, camera, static_cast<unsigned long long>(signature),
             view[9], view[10], view[11], view[12]);
    }
    if (g_last_camera && signature != g_last_camera) {
        ++g_camera_changes;
        if (g_camera_changes <= 8 || (g_camera_changes & (g_camera_changes - 1)) == 0)
            LOGR("mission benchmark scene: camera changed count=%u frame=%u signature=%016llX "
                 "position=(%.6f,%.6f,%.6f) fov=%.6f", g_camera_changes, frame,
                 static_cast<unsigned long long>(signature), view[9], view[10], view[11], view[12]);
    }
    g_last_camera = signature;
}

}  // namespace game::bench
#endif
