#pragma once

#include "d3d/dof_gpu_timing.h"

#include <atomic>
#include <mutex>

namespace gpu {

// Session-only switch and last completed report. The render thread owns the
// timer; the menu only reads this small copied snapshot.
inline std::atomic<bool> g_dof_gpu_profiling_enabled{false};
inline std::mutex g_dof_gpu_report_mutex;
inline DofGpuTiming::Report g_dof_gpu_report{};

inline bool DofGpuProfilingEnabled() {
    return g_dof_gpu_profiling_enabled.load(std::memory_order_relaxed);
}

inline void SetDofGpuProfilingEnabled(bool enabled) {
    g_dof_gpu_profiling_enabled.store(enabled, std::memory_order_relaxed);
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
