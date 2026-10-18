#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gpu {

// Draw-time requests never create a PSO on the calling thread. A missing,
// pending, or failed entry returns nullptr so the caller can render natively.
// Keys must identify the complete descriptor and shader pair. Ready pointers
// remain valid until this cache is destroyed; entries are never evicted.
class DofPipelineCache {
public:
    struct Stats {
        uint64_t ready = 0;
        uint64_t pending = 0;
        uint64_t failed = 0;
    };

    explicit DofPipelineCache(ID3D12Device* device) : device_(device) {
        if (!device_) return;
        try {
            worker_ = std::thread([this] { Work(); });
            available_ = true;
        } catch (...) {
            // Thread creation failure leaves a usable native-fallback cache.
        }
    }

    DofPipelineCache(const DofPipelineCache&) = delete;
    DofPipelineCache& operator=(const DofPipelineCache&) = delete;

    ~DofPipelineCache() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_one();
        // Finish owned CPU jobs before releasing their device or shader blobs.
        // No GPU fence or command queue is involved.
        if (worker_.joinable()) worker_.join();
    }

    ID3D12PipelineState* Request(uint64_t key,
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
        ID3DBlob* vs, ID3DBlob* ps) {
        std::lock_guard lock(mutex_);
        const auto found = entries_.find(key);
        if (found != entries_.end()) return found->second.pipeline.Get();

        try {
            // Failed is the initial state so preparation/allocation failures
            // are cached too, and never retried on every draw.
            auto [entry, inserted] = entries_.try_emplace(key);
            (void)inserted;
            ++stats_.failed;
            if (!available_ || stopping_) return nullptr;

            auto job = std::make_unique<Job>();
            if (!job->Capture(key, device_.Get(), desc, vs, ps)) return nullptr;
            jobs_.push_back(std::move(job));
            entry->second.state = State::Pending;
            --stats_.failed;
            ++stats_.pending;
            wake_.notify_one();
        } catch (...) {
            // If even the map allocation fails, no entry can be recorded.
            // Rendering still falls back without a driver call on this thread.
        }
        return nullptr;
    }

    ID3D12PipelineState* Request(uint64_t key,
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
        const Microsoft::WRL::ComPtr<ID3DBlob>& vs,
        const Microsoft::WRL::ComPtr<ID3DBlob>& ps) {
        return Request(key, desc, vs.Get(), ps.Get());
    }

    // Startup/prewarm only: draw-time code should use Request and native fallback.
    void Wait() {
        std::unique_lock lock(mutex_);
        drained_.wait(lock, [this] { return stats_.pending == 0; });
    }

    // A bounded startup wait lets the caller keep its loading display responsive.
    bool WaitFor(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return drained_.wait_for(lock, timeout,
            [this] { return stats_.pending == 0; });
    }

    Stats GetStats() const {
        std::lock_guard lock(mutex_);
        return stats_;
    }

private:
    enum class State { Failed, Pending, Ready };
    struct Entry {
        State state = State::Failed;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    };

    struct Job {
        uint64_t key = 0;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
        Microsoft::WRL::ComPtr<ID3DBlob> vs, ps;
        std::vector<D3D12_INPUT_ELEMENT_DESC> inputs;
        std::vector<std::string> semantics;
        std::vector<uint8_t> cached;

        bool Capture(uint64_t requested_key, ID3D12Device* requested_device,
            const D3D12_GRAPHICS_PIPELINE_STATE_DESC& source,
            ID3DBlob* vertex, ID3DBlob* pixel) {
            // This helper owns only the VS/PS graphics path. Reject unsupported
            // borrowed-pointer fields rather than retaining caller storage.
            if (!vertex || !pixel || !vertex->GetBufferSize() ||
                !pixel->GetBufferSize() ||
                source.GS.pShaderBytecode || source.GS.BytecodeLength ||
                source.HS.pShaderBytecode || source.HS.BytecodeLength ||
                source.DS.pShaderBytecode || source.DS.BytecodeLength ||
                source.StreamOutput.pSODeclaration || source.StreamOutput.NumEntries ||
                source.StreamOutput.pBufferStrides || source.StreamOutput.NumStrides ||
                source.InputLayout.NumElements > D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT ||
                (source.InputLayout.NumElements && !source.InputLayout.pInputElementDescs) ||
                (source.CachedPSO.CachedBlobSizeInBytes && !source.CachedPSO.pCachedBlob))
                return false;

            key = requested_key;
            device = requested_device;
            root = source.pRootSignature;
            vs = vertex;
            ps = pixel;
            desc = source; // Preserve all fixed fields, including stencil state.
            desc.pRootSignature = root.Get();
            desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
            desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};

            const UINT count = source.InputLayout.NumElements;
            inputs.resize(count);
            semantics.resize(count);
            for (UINT i = 0; i < count; ++i) {
                inputs[i] = source.InputLayout.pInputElementDescs[i];
                if (!inputs[i].SemanticName) return false;
                semantics[i] = inputs[i].SemanticName;
            }
            // Set pointers after all strings are populated. Job is heap-owned
            // and never moved, including while waiting in the work queue.
            for (UINT i = 0; i < count; ++i)
                inputs[i].SemanticName = semantics[i].c_str();
            desc.InputLayout.pInputElementDescs = count ? inputs.data() : nullptr;
            cached.resize(source.CachedPSO.CachedBlobSizeInBytes);
            if (!cached.empty())
                std::memcpy(cached.data(), source.CachedPSO.pCachedBlob, cached.size());
            desc.CachedPSO.pCachedBlob = cached.empty() ? nullptr : cached.data();
            return true;
        }
    };

    void Work() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;) {
            std::unique_ptr<Job> job;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
                if (jobs_.empty()) return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
            try {
                if (FAILED(job->device->CreateGraphicsPipelineState(
                        &job->desc, IID_PPV_ARGS(pipeline.GetAddressOf()))))
                    pipeline.Reset();
            } catch (...) {
                pipeline.Reset();
            }
            {
                std::lock_guard lock(mutex_);
                auto& entry = entries_.find(job->key)->second;
                entry.pipeline = std::move(pipeline);
                entry.state = entry.pipeline ? State::Ready : State::Failed;
                --stats_.pending;
                if (entry.pipeline) ++stats_.ready;
                else ++stats_.failed;
            }
            drained_.notify_all();
        }
    }

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    mutable std::mutex mutex_;
    std::condition_variable wake_, drained_;
    std::unordered_map<uint64_t, Entry> entries_;
    std::deque<std::unique_ptr<Job>> jobs_;
    Stats stats_;
    bool available_ = false;
    bool stopping_ = false;
    std::thread worker_;
};

} // namespace gpu
