#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>

#include <cmath>
#include <cstdio>
#include <vector>

#include "d3d/fidelityfx_dof.h"

#if BLACK_FIDELITYFX_DOF
#include <FidelityFX/host/ffx_dof.h>
#include <FidelityFX/host/backends/dx12/ffx_dx12.h>
#endif

namespace gpu {

struct FidelityFxDof::Impl {
    bool active = false;
    ID3D12Device* device = nullptr;
    uint32_t width = 0, height = 0;
    uint32_t rings = 4;
    bool disable_ring_merge = false;
    char failure[160] = {};
#if BLACK_FIDELITYFX_DOF
    FfxDofContext context{};
    std::vector<uint8_t> scratch;
    FfxInterface original{};
    FfxErrorCode callback_error = FFX_OK;
    bool backend_created = false;
    bool poisoned = false;
    // The SDK copies FfxInterface and owns scratchBuffer. A scoped thread-local association preserves that ABI
    // while monitoring callbacks; separate render/test threads do not share error state.
    static inline thread_local Impl* calling = nullptr;
    struct Scope {
        Impl* previous;
        explicit Scope(Impl* instance) : previous(calling) { calling = instance; }
        ~Scope() { calling = previous; }
    };
    template<class Call> FfxErrorCode Check(const char* operation, Call call) {
        FfxErrorCode result;
        try { result = call(); }
        catch (...) { result = FFX_ERROR_BACKEND_API_ERROR; }
        if (result != FFX_OK && callback_error == FFX_OK) {
            callback_error = result;
            Fail(operation, result);
        }
        return result;
    }
    static FfxErrorCode CreateBackend(FfxInterface* i, FfxEffect effect, FfxEffectBindlessConfig* config, FfxUInt32* id) {
        auto& self = *calling;
        auto result = self.Check("FidelityFX backend creation failed", [&] {
            return self.original.fpCreateBackendContext(i, effect, config, id);
        });
        self.backend_created = result == FFX_OK;
        return result;
    }
    static FfxErrorCode Register(FfxInterface* i, const FfxResource* resource, FfxUInt32 id, FfxResourceInternal* out) {
        auto& self = *calling;
        if (self.callback_error != FFX_OK) { *out = {}; return self.callback_error; }
        return self.Check("FidelityFX resource registration failed", [&] {
            return self.original.fpRegisterResource(i, resource, id, out);
        });
    }
    static FfxErrorCode Stage(FfxInterface* i, void* data, FfxUInt32 size, FfxConstantBuffer* out) {
        auto& self = *calling;
        if (self.callback_error != FFX_OK) return self.callback_error;
        return self.Check("FidelityFX constant staging failed", [&] {
            return self.original.fpStageConstantBufferDataFunc(i, data, size, out);
        });
    }
    static FfxErrorCode Schedule(FfxInterface* i, const FfxGpuJobDescription* job) {
        auto& self = *calling;
        if (self.callback_error != FFX_OK) return self.callback_error;
        return self.Check("FidelityFX job scheduling failed", [&] { return self.original.fpScheduleGpuJob(i, job); });
    }
    static FfxErrorCode Execute(FfxInterface* i, FfxCommandList list, FfxUInt32 id) {
        auto& self = *calling;
        if (self.callback_error != FFX_OK) return self.callback_error;
        // The DX12 backend throws on some allocation failures. Catch here so the component still unregisters
        // external resources and records their return barriers before the caller selects Balanced.
        return self.Check("FidelityFX GPU job execution failed", [&] { return self.original.fpExecuteGpuJobs(i, list, id); });
    }
    static FfxErrorCode Unregister(FfxInterface* i, FfxCommandList list, FfxUInt32 id) {
        auto& self = *calling;
        return self.Check("FidelityFX resource restoration failed", [&] { return self.original.fpUnregisterResources(i, list, id); });
    }
    void Monitor(FfxInterface& backend) {
        original = backend;
        backend.fpCreateBackendContext = CreateBackend;
        backend.fpRegisterResource = Register;
        backend.fpStageConstantBufferDataFunc = Stage;
        backend.fpScheduleGpuJob = Schedule;
        backend.fpExecuteGpuJobs = Execute;
        backend.fpUnregisterResources = Unregister;
    }
#endif
    bool Fail(const char* operation, int code = 0) {
        std::snprintf(failure, sizeof(failure), "%s (0x%08X)", operation, unsigned(code));
        return false;
    }
};

FidelityFxDof::FidelityFxDof() : impl_(std::make_unique<Impl>()) {}

void FidelityFxDof::Swap(FidelityFxDof& other) noexcept { impl_.swap(other.impl_); }
FidelityFxDof::~FidelityFxDof() { Reset(); }
bool FidelityFxDof::Ready() const { return impl_->active; }
bool FidelityFxDof::Matches(uint32_t width, uint32_t height, uint32_t rings, bool disable_ring_merge) const {
    return Ready() && impl_->width == width && impl_->height == height && impl_->rings == rings &&
        impl_->disable_ring_merge == disable_ring_merge;
}
const char* FidelityFxDof::Failure() const { return impl_->failure; }

void FidelityFxDof::Reset() {
#if BLACK_FIDELITYFX_DOF
    Impl::Scope scope(impl_.get());
    if (impl_->active) ffxDofContextDestroy(&impl_->context);
    impl_->context = {};
    impl_->scratch.clear();
    impl_->backend_created = impl_->poisoned = false;
    impl_->callback_error = FFX_OK;
#endif
    impl_->active = false;
    impl_->device = nullptr;
    impl_->width = impl_->height = 0;
    impl_->rings = 4;
    impl_->disable_ring_merge = false;
}

bool FidelityFxDof::Create(ID3D12Device* device, uint32_t width, uint32_t height, uint32_t rings, bool disable_ring_merge) {
    if (Matches(width, height, rings, disable_ring_merge) && impl_->device == device) return true;
    Reset();
    if (!device || !width || !height || !rings || rings > 16) return impl_->Fail("invalid device, extent or ring count");
#if BLACK_FIDELITYFX_DOF
    D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_2};
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model))) ||
        model.HighestShaderModel < D3D_SHADER_MODEL_6_2 ||
        FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options, sizeof(options))) || !options.WaveOps)
        return impl_->Fail("FidelityFX requires Shader Model 6.2 and wave operations");
    FfxDofContextDescription desc{};
    desc.resolution = {width, height};
    desc.quality = rings;
    desc.flags = disable_ring_merge ? FFX_DOF_DISABLE_RING_MERGE : 0;
    // UI limit: 30px at 1080p times 150% near/far strength. Actual radius/strength stays a dispatch input.
    desc.cocLimitFactor = 45.0f / 1080.0f;
    try { impl_->scratch.resize(ffxGetScratchMemorySizeDX12(1)); }
    catch (...) { return impl_->Fail("FidelityFX scratch allocation failed"); }
    FfxErrorCode result = ffxGetInterfaceDX12(&desc.backendInterface, ffxGetDeviceDX12(device),
        impl_->scratch.data(), impl_->scratch.size(), 1);
    if (result != FFX_OK) return impl_->Fail("FidelityFX DX12 interface creation failed", result);
    impl_->Monitor(desc.backendInterface);
    Impl::Scope scope(impl_.get());
    result = impl_->Check("FidelityFX context creation failed", [&] { return ffxDofContextCreate(&impl_->context, &desc); });
    if (result != FFX_OK || impl_->callback_error != FFX_OK) {
        // Version/interface errors occur before backend creation. Only a created backend is destroyable.
        if (impl_->backend_created) ffxDofContextDestroy(&impl_->context);
        impl_->context = {};
        if (impl_->callback_error == FFX_OK) impl_->Fail("FidelityFX DoF context creation failed", result);
        return false;
    }
    impl_->active = true;
    impl_->device = device;
    impl_->width = width;
    impl_->height = height;
    impl_->rings = rings;
    impl_->disable_ring_merge = disable_ring_merge;
    impl_->failure[0] = 0;
    return true;
#else
    return impl_->Fail("FidelityFX component was not included in this build");
#endif
}

bool FidelityFxDof::Dispatch(ID3D12GraphicsCommandList* command_list, ID3D12Resource* color,
                            ID3D12Resource* encoded_depth, ID3D12Resource* output, float coc_bound) {
    if (!Ready() || !command_list || !color || !encoded_depth || !output || output == color ||
        output == encoded_depth || !std::isfinite(coc_bound) || coc_bound <= 0)
        return impl_->Fail("invalid FidelityFX dispatch");
    ID3D12Resource* inputs[] = {color, encoded_depth, output};
    DXGI_FORMAT formats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT};
    for (uint32_t i = 0; i < 3; i++) {
        const auto desc = inputs[i]->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width != impl_->width ||
            desc.Height != impl_->height || desc.SampleDesc.Count != 1 || desc.DepthOrArraySize != 1 ||
            desc.MipLevels != 1 || desc.Format != formats[i] ||
            (i == 2 && !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)))
            return impl_->Fail("incompatible FidelityFX input/output texture");
    }
#if BLACK_FIDELITYFX_DOF
    if (impl_->poisoned) return impl_->Fail("FidelityFX context disabled after a failed dispatch");
    impl_->callback_error = FFX_OK;
    Impl::Scope scope(impl_.get());
    FfxDofDispatchDescription desc{};
    desc.commandList = ffxGetCommandListDX12(command_list);
    desc.color = ffxGetResourceDX12(color, ffxGetResourceDescriptionDX12(color), L"BLACK DoF input",
                                  FFX_RESOURCE_STATE_COMPUTE_READ);
    desc.depth = ffxGetResourceDX12(encoded_depth, ffxGetResourceDescriptionDX12(encoded_depth), L"BLACK DoF CoC depth",
                                  FFX_RESOURCE_STATE_COMPUTE_READ);
    desc.output = ffxGetResourceDX12(output, ffxGetResourceDescriptionDX12(output, FFX_RESOURCE_USAGE_UAV),
                                   L"BLACK DoF output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    // Normal depth ordering, near-positive CoC, in HALF-resolution pixels as required by the official kernels.
    desc.cocScale = -coc_bound;
    desc.cocBias = coc_bound * 0.5f;
    FfxErrorCode result = ffxDofContextDispatch(&impl_->context, &desc);
    if (impl_->callback_error != FFX_OK || result != FFX_OK) {
        // Never execute stale queued jobs after a failure; retain the context until the caller's GPU fence.
        impl_->poisoned = true;
        if (impl_->callback_error == FFX_OK) impl_->Fail("FidelityFX DoF dispatch failed", result);
        return false;
    }
    return true;
#else
    return impl_->Fail("FidelityFX component was not included in this build");
#endif
}

}  // namespace gpu
