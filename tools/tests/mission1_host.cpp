#if defined(BLACK_MISSION_BENCHMARK)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include "app/app.h"
#include "audio/output.h"
#include "common/mission_benchmark.h"
#include "common/options.h"
#include "core/input.h"
#include "core/log.h"
#include "d3d/dof_profile.h"

namespace game::bench {
namespace {

using Clock = std::chrono::steady_clock;
constexpr UINT_PTR kDeadlineTimer = 0x4D31;
constexpr UINT kTimerPeriodMs = 250;
constexpr uint64_t kWarmupFrames = 120;
constexpr uint64_t kEpochFrames = 360;
constexpr uint64_t kDiscardEpochFrames = 120;
constexpr uint64_t kDrainFrames = 120;
constexpr std::array<bool, 4> kEpochReuse = {false, true, true, false};

enum class RunStage : uint8_t { WaitingForScene, Warmup, Epoch, Drain, Stopped };

struct Row {
    Frame frame{};
    bool scene_ready = false;
    uint32_t scene_frame = 0;
    uint64_t camera_signature = 0;
    bool preview = false;
    bool reuse = false;
    uint64_t batch_delta = 0, atlas_page_delta = 0, atlas_quad_delta = 0;
    uint64_t cached_page_delta = 0, cached_quad_delta = 0, direct_quad_delta = 0;
    std::string phase;
    int32_t epoch = -1;
    uint64_t phase_frame = 0;
    bool include = false;
    int window_visible = -1;
    int foreground_owned = -1;
};

std::atomic<HWND> g_hwnd{nullptr};
std::atomic<bool> g_stop_requested{false};
std::atomic<bool> g_csv_flushed{false};
std::mutex g_rows_mutex;
std::vector<Row> g_rows;
std::string g_stop_reason;
Clock::time_point g_deadline{};
Clock::time_point g_scene_started{};
RunStage g_stage = RunStage::WaitingForScene;
uint64_t g_stage_frames = 0;
uint32_t g_epoch = 0;
bool g_have_previous_counters = false;
Frame g_previous{};

uint32_t DeadlineSeconds() {
    wchar_t text[32]{};
    const DWORD length = GetEnvironmentVariableW(L"BLACK_MISSION1_BENCHMARK_SECONDS", text,
                                                  DWORD(std::size(text)));
    if (!length || length >= std::size(text)) return 180;
    wchar_t* end = nullptr;
    const unsigned long parsed = std::wcstoul(text, &end, 10);
    if (end == text || *end != L'\0') return 180;
    return uint32_t(std::clamp<unsigned long>(parsed, 1ul, 300ul));
}

uint64_t Delta(uint64_t current, uint64_t previous) {
    return current >= previous ? current - previous : current;
}

void FlushCsv() {
    if (g_csv_flushed.exchange(true, std::memory_order_acq_rel)) return;
    std::vector<Row> rows;
    {
        std::lock_guard lock(g_rows_mutex);
        rows = g_rows;
    }
    const std::filesystem::path path = app::ExeDir() / L"logs" / L"mission1-benchmark.csv";
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        LOGE("Mission 1 benchmark: could not write %s", path.string().c_str());
        return;
    }
    out << "present_id,scene_ready,scene_frame,camera_signature,phase,epoch,phase_frame,include,"
           "reuse,preview,world,batches_total,batches_delta,atlas_pages_total,atlas_pages_delta,"
           "atlas_quads_total,atlas_quads_delta,cached_pages_total,cached_pages_delta,"
           "cached_quads_total,cached_quads_delta,direct_quads_total,direct_quads_delta,"
           "width,height,samples,present_hresult,present_ms,frame_ms,window_visible,foreground_owned,proof_capture\n";
    out << std::setprecision(9);
    for (const Row& row : rows) {
        const Frame& f = row.frame;
        out << f.id << ',' << int(row.scene_ready) << ',' << row.scene_frame << ',' << row.camera_signature << ','
            << row.phase << ',' << row.epoch << ',' << row.phase_frame << ',' << int(row.include) << ','
            << int(row.reuse) << ',' << int(row.preview) << ',' << int(f.world) << ','
            << f.batches << ',' << row.batch_delta << ','
            << f.atlas_pages << ',' << row.atlas_page_delta << ','
            << f.atlas_quads << ',' << row.atlas_quad_delta << ','
            << f.cached_pages << ',' << row.cached_page_delta << ','
            << f.cached_quads << ',' << row.cached_quad_delta << ','
            << f.direct_quads << ',' << row.direct_quad_delta << ','
            << f.width << ',' << f.height << ',' << f.samples << ',' << f.present_result << ','
            << f.present_ms << ',' << f.frame_ms << ',' << row.window_visible << ','
            << row.foreground_owned << ',' << int(f.proof_capture) << '\n';
    }
    out.flush();
    if (!out) LOGE("Mission 1 benchmark: CSV write failed");
    else LOGR("Mission 1 benchmark: wrote %zu frame rows to %s", rows.size(), path.string().c_str());
}

void RequestClose() {
    const HWND hwnd = g_hwnd.load(std::memory_order_acquire);
    if (hwnd && IsWindow(hwnd)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
}

} // namespace

bool Enabled() {
    static const bool enabled = [] {
        wchar_t value[8]{};
        const DWORD length = GetEnvironmentVariableW(L"BLACK_MISSION1_BENCHMARK", value,
                                                       DWORD(std::size(value)));
        return length == 1 && value[0] == L'1';
    }();
    return enabled;
}

void Init() {
    if (!Enabled()) return;
    // Hidden fullscreen setup can still change the desktop mode before Window().
    app::Get().display_mode = app::DisplayMode::Windowed;
    Options& options = Opt();
    options.pause_on_focus_loss = false;
    options.dof_preview = true;
    options.skip_intros = true;
    options.menu_animations = false;
    audio::SetPaused(true);
    gpu::SetDofGpuProfilingEnabled(true);
    gpu::SetDofParticleCoverageReuseEnabled(false);
    g_deadline = Clock::now() + std::chrono::seconds(DeadlineSeconds());
    g_rows.clear();
    g_rows.reserve(12000);
    LOGR("Mission 1 benchmark enabled: hidden host, audio paused, deadline %u s", DeadlineSeconds());
}

void Window(void* window) {
    if (!Enabled()) return;
    const HWND hwnd = static_cast<HWND>(window);
    g_hwnd.store(hwnd, std::memory_order_release);
    const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex_style | WS_EX_NOACTIVATE);
    const bool visible = IsWindowVisible(hwnd) != FALSE;
    if (visible) ShowWindow(hwnd, SW_HIDE);
    LOGR("Mission 1 benchmark HWND ready: visible=%d foreground_owned=%d", int(visible),
         int(GetForegroundWindow() == hwnd));
    if (!SetTimer(hwnd, kDeadlineTimer, kTimerPeriodMs, nullptr)) {
        Stop("failed to install deadline timer");
        return;
    }
    if (visible || GetForegroundWindow() == hwnd) {
        Stop("startup window was visible or foreground");
    } else if (g_stop_requested.load(std::memory_order_acquire)) {
        RequestClose();
    }
}

bool Message(void* window, uint32_t message, uintptr_t wparam, intptr_t lparam, intptr_t* result) {
    if (!Enabled()) return false;
    const HWND hwnd = static_cast<HWND>(window);
    if (message == WM_GETMINMAXINFO && lparam) {
        // A hidden benchmark may have a full-desktop client area plus a caption.
        // The default window tracking limit otherwise trims its render height.
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        limits->ptMaxTrackSize.x = std::max<LONG>(limits->ptMaxTrackSize.x, LONG(app::Get().width) + 256);
        limits->ptMaxTrackSize.y = std::max<LONG>(limits->ptMaxTrackSize.y, LONG(app::Get().height) + 256);
        if (result) *result = 0;
        return true;
    }
    if (message == WM_NCCREATE) {
        const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex_style | WS_EX_NOACTIVATE);
        return false;
    }
    if (message == WM_WINDOWPOSCHANGING && lparam) {
        auto* position = reinterpret_cast<WINDOWPOS*>(lparam);
        position->flags |= SWP_NOACTIVATE;
        position->flags &= ~SWP_SHOWWINDOW;
        return false;
    }
    if (message == WM_MOUSEACTIVATE) {
        if (result) *result = MA_NOACTIVATE;
        return true;
    }
    if (message == WM_TIMER && wparam == kDeadlineTimer) {
        if (Clock::now() >= g_deadline) Stop("deadline reached");
        if (result) *result = 0;
        return true;
    }
    if (message == WM_CLOSE) {
        if (!g_stop_requested.exchange(true, std::memory_order_acq_rel)) {
            {
                std::lock_guard lock(g_rows_mutex);
                g_stop_reason = "window close requested";
            }
            FlushCsv();
            logx::Flush();
        }
        return false; // Let the SDK destroy only this benchmark window.
    }
    if (message == WM_NCDESTROY) {
        KillTimer(hwnd, kDeadlineTimer);
        g_hwnd.store(nullptr, std::memory_order_release);
    }
    return false;
}

void Pad(int port, input::Pad& pad) {
    pad = input::Pad{};
    pad.connected = port == 0;
}

void Stop(const char* reason) {
    if (!Enabled()) return;
    bool expected = false;
    if (!g_stop_requested.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
    {
        std::lock_guard lock(g_rows_mutex);
        g_stop_reason = reason ? reason : "stopped";
        g_stage = RunStage::Stopped;
    }
    LOGR("Mission 1 benchmark stopping: %s", g_stop_reason.c_str());
    FlushCsv();
    logx::Flush();
    RequestClose();
}

void Present(const Frame& frame) {
    if (!Enabled() || g_stop_requested.load(std::memory_order_acquire)) return;
    const bool ready = SceneReady();
    const uint32_t scene_frame = SceneFrame();
    const uint64_t camera = CameraSignature();
    const bool reuse = gpu::DofParticleCoverageReuseEnabled();
    const bool preview = Opt().dof_preview;
    const HWND hwnd = g_hwnd.load(std::memory_order_acquire);
    bool should_stop_visibility = false;
    bool should_stop_complete = false;
    Row row;
    row.frame = frame;
    row.scene_ready = ready;
    row.scene_frame = scene_frame;
    row.camera_signature = camera;
    row.preview = preview;
    row.reuse = reuse;
    row.window_visible = -1;
    row.foreground_owned = -1;

    {
        std::lock_guard lock(g_rows_mutex);
        if (g_stop_requested.load(std::memory_order_acquire)) return;
        if (!ready) {
            row.phase = "waiting";
        } else {
            if (g_stage == RunStage::WaitingForScene) {
                g_stage = RunStage::Warmup;
                g_scene_started = Clock::now();
                g_stage_frames = 0;
                gpu::SetDofParticleCoverageReuseEnabled(false);
            }
            if (g_stage == RunStage::Warmup) {
                row.phase = "warmup";
                row.phase_frame = ++g_stage_frames;
                // The mission begins rendering behind its scripted title/fade.
                // Do not mistake that opening presentation for settled gameplay.
                if (row.phase_frame >= kWarmupFrames && frame.world &&
                    Clock::now() - g_scene_started >= std::chrono::seconds(25)) {
                    g_stage = RunStage::Epoch;
                    g_epoch = 0;
                    g_stage_frames = 0;
                    gpu::SetDofParticleCoverageReuseEnabled(kEpochReuse[0]);
                }
            } else if (g_stage == RunStage::Epoch) {
                row.phase = "epoch";
                row.epoch = int32_t(g_epoch);
                row.phase_frame = ++g_stage_frames;
                row.include = row.phase_frame > kDiscardEpochFrames && !frame.proof_capture;
                if (row.phase_frame == kEpochFrames) {
                    if (g_epoch + 1 < kEpochReuse.size()) {
                        ++g_epoch;
                        g_stage_frames = 0;
                        gpu::SetDofParticleCoverageReuseEnabled(kEpochReuse[g_epoch]);
                    } else {
                        g_stage = RunStage::Drain;
                        g_stage_frames = 0;
                        gpu::SetDofParticleCoverageReuseEnabled(false);
                    }
                }
            } else if (g_stage == RunStage::Drain) {
                row.phase = "drain";
                row.phase_frame = ++g_stage_frames;
                if (row.phase_frame == kDrainFrames) {
                    should_stop_complete = true;
                }
            } else {
                row.phase = "stopped";
            }
            row.epoch = row.phase == "epoch" ? row.epoch : -1;
        }

        if (g_have_previous_counters) {
            row.batch_delta = Delta(frame.batches, g_previous.batches);
            row.atlas_page_delta = Delta(frame.atlas_pages, g_previous.atlas_pages);
            row.atlas_quad_delta = Delta(frame.atlas_quads, g_previous.atlas_quads);
            row.cached_page_delta = Delta(frame.cached_pages, g_previous.cached_pages);
            row.cached_quad_delta = Delta(frame.cached_quads, g_previous.cached_quads);
            row.direct_quad_delta = Delta(frame.direct_quads, g_previous.direct_quads);
        } else {
            row.batch_delta = frame.batches;
            row.atlas_page_delta = frame.atlas_pages;
            row.atlas_quad_delta = frame.atlas_quads;
            row.cached_page_delta = frame.cached_pages;
            row.cached_quad_delta = frame.cached_quads;
            row.direct_quad_delta = frame.direct_quads;
            g_have_previous_counters = true;
        }
        g_previous = frame;

        if (frame.id == 1 || frame.id % 120 == 0) {
            row.window_visible = hwnd && IsWindowVisible(hwnd) ? 1 : 0;
            row.foreground_owned = hwnd && GetForegroundWindow() == hwnd ? 1 : 0;
            if (row.window_visible || row.foreground_owned) {
                should_stop_visibility = true;
            }
        }
        g_rows.push_back(std::move(row));
    }

    if (frame.id == 1 || frame.id % 120 == 0) {
        LOGR("Mission 1 benchmark frame %llu: scene=%d/%u camera=%016llX visible=%d foreground_owned=%d",
             (unsigned long long)frame.id, int(ready), scene_frame, (unsigned long long)camera,
             hwnd && IsWindowVisible(hwnd) ? 1 : 0, hwnd && GetForegroundWindow() == hwnd ? 1 : 0);
    }
    if (should_stop_visibility) Stop("window visibility/focus invariant failed");
    else if (should_stop_complete) Stop("completed warmup, four epochs, and drain");
}

} // namespace game::bench

#endif // BLACK_MISSION_BENCHMARK
