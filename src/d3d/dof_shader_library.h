#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <d3dcompiler.h>
#include <wrl/client.h>

namespace gpu {

struct DofShaderBytecode {
    const uint8_t* data = nullptr;
    size_t size = 0;
    explicit operator bool() const { return data != nullptr && size != 0; }
};

struct DofShaderKeyHash {
    uint64_t first;
    uint64_t second;
};

enum class DofShaderOptimization : uint8_t { O1, O3 };

inline constexpr uint64_t DofShaderCompilerFlags(DofShaderOptimization optimization) {
    return optimization == DofShaderOptimization::O3
        ? uint64_t(D3DCOMPILE_OPTIMIZATION_LEVEL3)
        : uint64_t(D3DCOMPILE_OPTIMIZATION_LEVEL1);
}

// Single policy point for shipped DoF bytecode. Keep the entry/profile parameters so a
// reviewed per-entry policy can be introduced from measured evidence; all entries ship O1.
inline constexpr uint64_t DofShaderCompilerFlags(std::string_view entry, std::string_view profile) {
    (void)entry;
    (void)profile;
    return DofShaderCompilerFlags(DofShaderOptimization::O1);
}

inline constexpr uint64_t DofShaderCompilerFlags2 = 0;

// Two independent byte-wise hashes keep the generated table compact. The generator rejects
// any collision on the complete lookup tuple before writing its output.
inline DofShaderKeyHash HashDofShaderSource(std::string_view source) {
    uint64_t a = 14695981039346656037ull;
    uint64_t b = 7809847782465536322ull;
    for (unsigned char c : source) {
        a = (a ^ c) * 1099511628211ull;
        b = (b ^ static_cast<unsigned char>(c + 0x9d)) * 14029467366897019727ull;
    }
    a ^= uint64_t(source.size()) + 0x9e3779b97f4a7c15ull;
    b ^= (uint64_t(source.size()) << 1) + 0xd6e8feb86659fd93ull;
    return {a, b};
}

DofShaderBytecode FindDofShaderBytecode(std::string_view source, std::string_view entry,
                                        std::string_view profile, uint64_t compiler_flags);

inline Microsoft::WRL::ComPtr<ID3DBlob> LoadDofShaderBytecode(
    std::string_view source, std::string_view entry, std::string_view profile, uint64_t compiler_flags) {
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    const DofShaderBytecode found = FindDofShaderBytecode(source, entry, profile, compiler_flags);
    if (!found || FAILED(D3DCreateBlob(found.size, &blob))) return {};
    std::memcpy(blob->GetBufferPointer(), found.data, found.size);
    return blob;
}

} // namespace gpu
