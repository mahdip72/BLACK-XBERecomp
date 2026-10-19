#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace gpu {

// GPU timestamps are opt-in. One slot covers every command-list submission of a
// native present, so stage percentiles describe complete frames rather than batches.
// All methods run on the render thread. The queue must be drained before destruction.
class DofGpuTiming {
public:
    enum class Stage : uint8_t {
        DofTotal,
        ScenePrepare,
        Sdk,
        SceneCompose,
        WeaponSceneCopy,
        WeaponDepthCopy,
        WeaponClassify,
        WeaponPack,
        WeaponCompose,
        ParticleDepthCopy,
        ParticleRange,
        ParticleSource,
        ParticleFilter,
        ParticleComposite,
        ParticleDirect,
        ParticleHierarchy,
        SdkDownsampleDepth,
        SdkDownsampleColor,
        SdkDilate,
        SdkBlur,
        SdkComposite,
        ParticleSupport,
        ParticleTransmission,
        Count
    };

    static constexpr uint32_t kStageCount = uint32_t(Stage::Count);
    static constexpr uint32_t kSlots = 8;
    static constexpr uint32_t kIntervalsPerFrame = 512;
    static constexpr uint32_t kSamplesPerReport = 240;
    static constexpr uint32_t kFrameSamplesPerReport = 240;
    static_assert(kIntervalsPerFrame <= 0xffff);

    // SDK children are nested inside Sdk. Only top-level stages contribute to
    // DofTotal; adding both the parent and its children would double-count.
    static constexpr bool CountsTowardDofTotal(Stage stage) {
        return (stage > Stage::DofTotal && stage < Stage::SdkDownsampleDepth) ||
               stage == Stage::ParticleSupport || stage == Stage::ParticleTransmission;
    }

    static constexpr bool IsSdkSubpass(Stage stage) {
        return stage >= Stage::SdkDownsampleDepth && stage <= Stage::SdkComposite;
    }

    // Valid, final-fence-completed native frame. Ticks retain queue-frequency
    // precision; inactive stages have zero ticks and intervals. Frame IDs permit
    // alignment across live benchmark streams. Missing IDs must be treated as
    // losses, not as zero-cost frames.
    struct FrameSample {
        uint64_t frame_id = 0;
        uint64_t fence = 0;
        std::array<uint64_t, kStageCount> ticks{};
        std::array<uint16_t, kStageCount> intervals{};
    };

    struct StageReport {
        uint64_t intervals = 0;
        uint64_t active_frames = 0;
        uint64_t percentile_evicted_frames = 0;
        uint32_t percentile_frames = 0;
        double total_ms = 0;
        double median_frame_ms = 0;
        double p95_frame_ms = 0;
    };

    struct Report {
        bool available = false;
        uint64_t frequency = 0;
        uint64_t complete_frames = 0;
        uint64_t invalid_frames = 0;
        uint64_t busy_frames = 0;
        uint64_t overflow_frames = 0;
        uint64_t dropped_intervals = 0;
        uint64_t auto_closed_scopes = 0;
        uint64_t unresolved_frames = 0;
        uint64_t readback_errors = 0;
        uint64_t frame_sample_evicted = 0;
        uint32_t pending_frames = 0;
        uint32_t frame_sample_count = 0;
        std::array<StageReport, kStageCount> stage{};
        std::array<FrameSample, kFrameSamplesPerReport> frame_samples{};
    };

    class Scope {
    public:
        Scope() = default;
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        Scope(Scope&& other) noexcept { Move(other); }
        Scope& operator=(Scope&& other) noexcept {
            if (this != &other) { End(); Move(other); }
            return *this;
        }
        ~Scope() { End(); }

        void End() {
            if (owner_) owner_->EndInterval(slot_, interval_, generation_, list_);
            owner_ = nullptr;
        }

    private:
        friend class DofGpuTiming;
        Scope(DofGpuTiming* owner, ID3D12GraphicsCommandList* list, uint32_t slot,
              uint32_t interval, uint64_t generation)
            : owner_(owner), list_(list), slot_(slot), interval_(interval), generation_(generation) {}
        void Move(Scope& other) {
            owner_ = other.owner_; list_ = other.list_; slot_ = other.slot_;
            interval_ = other.interval_; generation_ = other.generation_;
            other.owner_ = nullptr;
        }
        DofGpuTiming* owner_ = nullptr;
        ID3D12GraphicsCommandList* list_ = nullptr;
        uint32_t slot_ = 0, interval_ = 0;
        uint64_t generation_ = 0;
    };

    bool Init(ID3D12Device* device, ID3D12CommandQueue* queue) {
        if (available_) return true;
        queries_.Reset();
        readback_.Reset();
        if (!device || !queue || FAILED(queue->GetTimestampFrequency(&frequency_)) || !frequency_)
            return false;
        D3D12_QUERY_HEAP_DESC query{};
        query.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        query.Count = kSlots * kIntervalsPerFrame * 2;
        if (FAILED(device->CreateQueryHeap(&query, IID_PPV_ARGS(queries_.GetAddressOf())))) return false;
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = uint64_t(query.Count) * sizeof(uint64_t);
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(readback_.GetAddressOf())))) return false;
        available_ = true;
        return true;
    }

    bool Available() const { return available_; }
    bool Recording() const { return active_slot_ >= 0; }

    // Call once per native present before any timed draw. A frame with no
    // markers is still a valid zero-duration sample when timing is enabled.
    void BeginFrame(uint64_t frame_id, bool enabled, uint64_t completed_fence) {
        Collect(completed_fence);
        if (frame_id == frame_id_) return;
        if (active_slot_ >= 0) { // Integration error: previous frame was not finalized.
            Slot& old = slots_[active_slot_];
            old.invalid = true;
            if (old.fence) old.state = SlotState::Pending;
            else { old.state = SlotState::Free; ++period_.invalid_frames; }
            active_slot_ = -1;
            ++period_.unresolved_frames;
        }
        frame_id_ = frame_id;
        skipped_frame_ = false;
        if (!enabled || !available_) return;
        for (uint32_t i = 0; i < kSlots; ++i) {
            Slot& slot = slots_[i];
            if (slot.state != SlotState::Free) continue;
            slot.state = SlotState::Recording;
            slot.frame_id = frame_id;
            slot.fence = 0;
            slot.used = slot.resolved = 0;
            slot.invalid = slot.overflow = false;
            slot.awaiting_submit = false;
            ++slot.generation;
            active_slot_ = int(i);
            return;
        }
        skipped_frame_ = true;
        ++period_.busy_frames;
    }

    Scope Measure(Stage stage, ID3D12GraphicsCommandList* list) {
        // DofTotal is derived from nonoverlapping leaf stages at readback.
        if (!list || active_slot_ < 0 || stage == Stage::DofTotal || stage >= Stage::Count) return {};
        Slot& slot = slots_[active_slot_];
        if (slot.awaiting_submit) { slot.invalid = true; return {}; }
        if (slot.used == kIntervalsPerFrame) {
            slot.invalid = slot.overflow = true;
            ++period_.dropped_intervals;
            return {};
        }
        const uint32_t interval = slot.used++;
        slot.intervals[interval] = {stage, true};
        list->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, QueryIndex(active_slot_, interval));
        return Scope(this, list, uint32_t(active_slot_), interval, slot.generation);
    }

    // Call before Close/Execute for every submission, including intermediate
    // Flush calls. It resolves only markers recorded since the last Submit.
    void Resolve(ID3D12GraphicsCommandList* list) {
        if (!list || active_slot_ < 0) return;
        Slot& slot = slots_[active_slot_];
        if (slot.awaiting_submit) { slot.invalid = true; return; }
        for (uint32_t i = slot.resolved; i < slot.used; ++i) {
            if (slot.intervals[i].open) {
                list->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, QueryIndex(active_slot_, i) + 1);
                slot.intervals[i].open = false;
                slot.invalid = true;
                ++period_.auto_closed_scopes;
            }
        }
        if (slot.used != slot.resolved) {
            const uint32_t first = QueryIndex(active_slot_, slot.resolved);
            const uint32_t count = (slot.used - slot.resolved) * 2;
            list->ResolveQueryData(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, count,
                                   readback_.Get(), uint64_t(first) * sizeof(uint64_t));
            slot.resolved = slot.used;
        }
        slot.awaiting_submit = true;
    }

    // Call after the queue Signal in Submit. final_native_present is true only
    // for the submission associated with the title's Present, never a Flush.
    void Submitted(uint64_t fence_value, uint64_t frame_id, bool final_native_present) {
        if (frame_id != frame_id_) return;
        if (skipped_frame_) {
            if (final_native_present) { ++period_.invalid_frames; skipped_frame_ = false; }
            return;
        }
        if (active_slot_ < 0) return;
        Slot& slot = slots_[active_slot_];
        if (slot.frame_id != frame_id || fence_value <= slot.fence ||
            slot.resolved != slot.used || !slot.awaiting_submit) {
            slot.invalid = true;
            ++period_.unresolved_frames;
        }
        slot.fence = fence_value;
        slot.awaiting_submit = false;
        if (final_native_present) {
            slot.state = SlotState::Pending;
            active_slot_ = -1;
        }
    }

    // A completed fence proves every Resolve in that native frame finished.
    // Map is read-only and never waits for GPU progress.
    void Collect(uint64_t completed_fence) {
        if (!available_) return;
        std::array<uint32_t, kSlots> ready{};
        uint32_t ready_count = 0;
        for (uint32_t i = 0; i < kSlots; ++i) {
            const Slot& slot = slots_[i];
            if (slot.state == SlotState::Pending && slot.fence && slot.fence <= completed_fence)
                ready[ready_count++] = i;
        }
        std::sort(ready.begin(), ready.begin() + ready_count, [&](uint32_t a, uint32_t b) {
            return slots_[a].frame_id < slots_[b].frame_id;
        });
        for (uint32_t r = 0; r < ready_count; ++r) {
            const uint32_t i = ready[r];
            Slot& slot = slots_[i];
            if (slot.used != slot.resolved) slot.invalid = true;
            std::array<uint64_t, kStageCount> ticks{};
            std::array<uint32_t, kStageCount> counts{};
            if (!slot.invalid && slot.used) {
                const uint64_t base = uint64_t(i) * kIntervalsPerFrame * 2 * sizeof(uint64_t);
                D3D12_RANGE range{SIZE_T(base), SIZE_T(base + uint64_t(slot.used) * 2 * sizeof(uint64_t))};
                void* mapped = nullptr;
                if (FAILED(readback_->Map(0, &range, &mapped)) || !mapped) {
                    slot.invalid = true;
                    ++period_.readback_errors;
                } else {
                    const auto* values = reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(mapped) + base);
                    for (uint32_t j = 0; j < slot.used; ++j) {
                        const uint64_t start = values[j * 2], end = values[j * 2 + 1];
                        if (end < start) { slot.invalid = true; break; }
                        const auto stage = uint32_t(slot.intervals[j].stage);
                        ticks[stage] += end - start;
                        ++counts[stage];
                    }
                    for (uint32_t j = 1; j < kStageCount; ++j)
                        if (CountsTowardDofTotal(Stage(j))) {
                            ticks[0] += ticks[j];
                            counts[0] += counts[j];
                        }
                    D3D12_RANGE written{0, 0};
                    readback_->Unmap(0, &written);
                }
            }
            if (slot.invalid) {
                ++period_.invalid_frames;
                period_.overflow_frames += slot.overflow;
            } else {
                ++period_.complete_frames;
                FrameSample& sample = period_.frame_samples[period_.frame_sample_next];
                sample.frame_id = slot.frame_id;
                sample.fence = slot.fence;
                sample.ticks = ticks;
                for (uint32_t j = 0; j < kStageCount; ++j)
                    sample.intervals[j] = uint16_t(counts[j]);
                period_.frame_sample_next = (period_.frame_sample_next + 1) % kFrameSamplesPerReport;
                period_.frame_sample_evicted += period_.frame_sample_count == kFrameSamplesPerReport;
                period_.frame_sample_count = std::min(period_.frame_sample_count + 1,
                                                      kFrameSamplesPerReport);
                for (uint32_t j = 0; j < kStageCount; ++j) {
                    auto& stat = period_.stage[j];
                    const double ms = double(ticks[j]) * 1000.0 / double(frequency_);
                    stat.total_ms += ms;
                    stat.intervals += counts[j];
                    stat.active_frames += counts[j] != 0;
                    stat.samples[stat.next] = ms;
                    stat.next = (stat.next + 1) % kSamplesPerReport;
                    stat.evicted += stat.sample_count == kSamplesPerReport;
                    stat.sample_count = std::min(stat.sample_count + 1, kSamplesPerReport);
                }
            }
            slot.state = SlotState::Free;
        }
    }

    Report TakeReport() {
        Report out;
        out.available = available_;
        out.frequency = frequency_;
        out.complete_frames = period_.complete_frames;
        out.invalid_frames = period_.invalid_frames;
        out.busy_frames = period_.busy_frames;
        out.overflow_frames = period_.overflow_frames;
        out.dropped_intervals = period_.dropped_intervals;
        out.auto_closed_scopes = period_.auto_closed_scopes;
        out.unresolved_frames = period_.unresolved_frames;
        out.readback_errors = period_.readback_errors;
        out.frame_sample_evicted = period_.frame_sample_evicted;
        out.frame_sample_count = period_.frame_sample_count;
        const uint32_t first = period_.frame_sample_count == kFrameSamplesPerReport
            ? period_.frame_sample_next : 0;
        for (uint32_t i = 0; i < out.frame_sample_count; ++i)
            out.frame_samples[i] = period_.frame_samples[(first + i) % kFrameSamplesPerReport];
        for (const Slot& slot : slots_) out.pending_frames += slot.state == SlotState::Pending;
        for (uint32_t i = 0; i < kStageCount; ++i) {
            const auto& src = period_.stage[i];
            auto& dst = out.stage[i];
            dst.intervals = src.intervals;
            dst.active_frames = src.active_frames;
            dst.percentile_frames = src.sample_count;
            dst.percentile_evicted_frames = src.evicted;
            dst.total_ms = src.total_ms;
            if (!src.sample_count) continue;
            std::array<double, kSamplesPerReport> sorted = src.samples;
            std::sort(sorted.begin(), sorted.begin() + src.sample_count);
            const uint32_t n = src.sample_count;
            dst.median_frame_ms = n & 1 ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) * .5;
            dst.p95_frame_ms = sorted[uint32_t(std::ceil(.95 * n)) - 1];
        }
        period_ = {};
        return out;
    }

    static const char* StageName(Stage stage) {
        constexpr const char* names[] = {"DOF total", "scene prepare", "SDK whole", "scene compose",
            "weapon scene copy", "weapon depth copy", "weapon coverage", "weapon pack", "weapon compose",
            "particle depth copy", "particle range", "particle source", "particle filter",
            "particle composite", "particle direct", "particle hierarchy",
            "SDK depth downsample", "SDK color downsample", "SDK dilate", "SDK blur", "SDK composite",
            "particle support", "particle visibility"};
        static_assert(sizeof(names) / sizeof(names[0]) == kStageCount);
        const uint32_t index = uint32_t(stage);
        return index < kStageCount ? names[index] : "unknown";
    }

private:
    enum class SlotState : uint8_t { Free, Recording, Pending };
    struct Interval { Stage stage = Stage::DofTotal; bool open = false; };
    struct Slot {
        SlotState state = SlotState::Free;
        uint64_t frame_id = 0, fence = 0, generation = 0;
        uint32_t used = 0, resolved = 0;
        bool invalid = false, overflow = false, awaiting_submit = false;
        std::array<Interval, kIntervalsPerFrame> intervals{};
    };
    struct PeriodStage {
        uint64_t intervals = 0, active_frames = 0, evicted = 0;
        double total_ms = 0;
        std::array<double, kSamplesPerReport> samples{};
        uint32_t next = 0, sample_count = 0;
    };
    struct Period {
        uint64_t complete_frames = 0, invalid_frames = 0, busy_frames = 0;
        uint64_t overflow_frames = 0, dropped_intervals = 0, auto_closed_scopes = 0;
        uint64_t unresolved_frames = 0, readback_errors = 0;
        uint64_t frame_sample_evicted = 0;
        uint32_t frame_sample_next = 0, frame_sample_count = 0;
        std::array<PeriodStage, kStageCount> stage{};
        std::array<FrameSample, kFrameSamplesPerReport> frame_samples{};
    };

    uint32_t QueryIndex(uint32_t slot, uint32_t interval) const {
        return (slot * kIntervalsPerFrame + interval) * 2;
    }
    void EndInterval(uint32_t index, uint32_t interval, uint64_t generation, ID3D12GraphicsCommandList* list) {
        Slot& slot = slots_[index];
        if (slot.state != SlotState::Recording || slot.generation != generation ||
            interval >= slot.used || !slot.intervals[interval].open) return;
        list->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, QueryIndex(index, interval) + 1);
        slot.intervals[interval].open = false;
    }

    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries_;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
    std::array<Slot, kSlots> slots_{};
    Period period_{};
    uint64_t frequency_ = 0, frame_id_ = 0;
    int active_slot_ = -1;
    bool available_ = false, skipped_frame_ = false;
};

} // namespace gpu
