#pragma once
#include <d3d12.h>
#include <span>

namespace gpu {

// A bulk 38-cell clear reproduces a stack-cookie failure in the NVIDIA driver.
// Individual clears preserve the exact pixel union and avoid that driver path.
inline void ClearParticleDofAtlasCells(ID3D12GraphicsCommandList* list,
    D3D12_CPU_DESCRIPTOR_HANDLE target, std::span<const D3D12_RECT> cells) {
    const float zero[4]{};
    for (const auto& cell : cells) list->ClearRenderTargetView(target, zero, 1, &cell);
}

} // namespace gpu
