#pragma once

#include "d3d/dof_gpu_timing.h"

#include <atomic>
#include <mutex>

namespace gpu {

// Session-only switch and last completed, bounded report. The render thread
// owns the timer; readers copy only the published, fence-completed snapshot.
inline std::atomic<bool> g_dof_gpu_profiling_enabled{false};
inline std::atomic<bool> g_particle_coverage_reuse_enabled{true};
inline std::mutex g_dof_gpu_report_mutex;
inline DofGpuTiming::Report g_dof_gpu_report{};

inline bool DofGpuProfilingEnabled() {
    return g_dof_gpu_profiling_enabled.load(std::memory_order_relaxed);
}

inline void SetDofGpuProfilingEnabled(bool enabled) {
    g_dof_gpu_profiling_enabled.store(enabled, std::memory_order_relaxed);
}

inline bool DofParticleCoverageReuseEnabled() {
    return g_particle_coverage_reuse_enabled.load(std::memory_order_relaxed);
}

inline void SetDofParticleCoverageReuseEnabled(bool enabled) {
    g_particle_coverage_reuse_enabled.store(enabled, std::memory_order_relaxed);
}

inline DofGpuTiming::Report GetDofGpuReport() {
    std::lock_guard lock(g_dof_gpu_report_mutex);
    return g_dof_gpu_report;
}

inline void PublishDofGpuReport(const DofGpuTiming::Report& report) {
    std::lock_guard lock(g_dof_gpu_report_mutex);
    g_dof_gpu_report = report;
}

} // namespace gpu
