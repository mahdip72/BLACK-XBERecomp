// Offline FXC O1/O3 comparison for every statically supported DoF shader key.
// This tool is not linked into Black and never creates a graphics device.
#include <d3dcompiler.h>
#include <d3d11shader.h>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "d3d/dof_shader_library.h"
#include "d3d/dof_shader_source.h"
#include "gpu/shaders.h"

namespace {

constexpr size_t kExpectedUniqueKeys = 432;

struct Key {
    uint64_t hash0;
    uint64_t hash1;
    size_t source_size;
    std::string entry;
    std::string profile;
    auto Tie() const { return std::tie(hash0, hash1, source_size, entry, profile); }
    bool operator<(const Key& other) const { return Tie() < other.Tie(); }
};

struct Variant {
    Key key;
    std::string source;
};

std::map<Key, Variant> BuildManifest() {
    std::map<Key, Variant> variants;
    auto add = [&](std::string source, std::string_view entry, std::string_view profile) {
        const auto h = gpu::HashDofShaderSource(source);
        Key key{h.first, h.second, source.size(), std::string(entry), std::string(profile)};
        auto found = variants.find(key);
        if (found != variants.end()) {
            if (found->second.source != source)
                throw std::runtime_error("DoF shader source hash collision in comparison manifest");
            return;
        }
        Variant variant{key, std::move(source)};
        variants.emplace(std::move(key), std::move(variant));
    };
    auto pair = [&](const std::string& source, std::string_view ps) {
        add(source, "vs", "vs_5_0");
        add(source, ps, "ps_5_0");
    };

    struct StaticPass { std::string_view entry; uint32_t samples; };
    constexpr StaticPass passes[] = {
        {"depth_of_field", 1}, {"depth_of_field_gather", 1},
        {"depth_of_field_pack", 1}, {"depth_of_field_pack", 2},
        {"depth_of_field_pack", 4}, {"depth_of_field_pack", 8},
        {"depth_of_field_downsample", 1},
        {"viewmodel_dof_pack", 1}, {"viewmodel_dof_pack", 2},
        {"viewmodel_dof_pack", 4}, {"viewmodel_dof_pack", 8},
        {"viewmodel_dof", 1},
        {"ffx_dof_prepare", 1}, {"ffx_dof_prepare", 2},
        {"ffx_dof_prepare", 4}, {"ffx_dof_prepare", 8},
        {"ffx_dof_present", 1},
    };
    for (const auto& p : passes) pair(gpu::DofStaticPassSource(p.entry, p.samples), p.entry);
    add(gpu::DofStaticPassSource("viewmodel_dof_coverage", 1),
        "viewmodel_dof_coverage", "ps_5_0");
    for (uint32_t samples : {1u, 2u, 4u, 8u})
        add(gpu::DofStaticPassSource("viewmodel_dof_pack", samples),
            "viewmodel_dof_tiles_vs", "vs_5_0");
    add(gpu::DofStaticPassSource("viewmodel_dof"), "viewmodel_dof_tiles_vs", "vs_5_0");
    for (uint32_t samples : {1u, 2u, 4u, 8u})
        pair(gpu::DofParticleRangeSource(samples), "range_ps");
    pair(gpu::kParticleDofAtlasFilterHlsl, "filter_ps");
    for (uint32_t pass : {0u, 1u, 2u})
        add(gpu::DofParticleVertexSource(pass), "main", "vs_5_0");
    for (uint32_t pass : {0u, 1u, 2u})
        for (uint32_t samples : {1u, 2u, 4u, 8u})
            for (uint32_t blend : {0u, 1u})
                for (int alpha = 0; alpha < 8; ++alpha)
                    for (bool kill : {false, true})
                        add(gpu::DofParticlePixelSource(samples, blend, alpha, kill, pass),
                            "main", "ps_5_0");
    return variants;
}

struct Metrics {
    size_t bytes = 0;
    uint64_t byte_hash0 = 0;
    uint64_t byte_hash1 = 0;
    UINT instructions = 0;
    UINT temp_registers = 0;
    UINT float_instructions = 0;
    UINT int_instructions = 0;
    UINT texture_instructions = 0;
    UINT static_flow = 0;
    UINT dynamic_flow = 0;
};

Metrics CompileAndInspect(const Variant& variant, gpu::DofShaderOptimization optimization) {
    const uint64_t flags = gpu::DofShaderCompilerFlags(optimization);
    Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(variant.source.data(), variant.source.size(), nullptr, nullptr, nullptr,
        variant.key.entry.c_str(), variant.key.profile.c_str(),
        static_cast<UINT>(flags & 0xffffffffu),
        static_cast<UINT>(gpu::DofShaderCompilerFlags2), &code, &errors);
    if (FAILED(hr) || !code) {
        std::cerr << "D3DCompile failed for " << variant.key.entry << ' ' << variant.key.profile
                  << " optimization=" << (optimization == gpu::DofShaderOptimization::O1 ? 1 : 3)
                  << " hr=0x" << std::hex << static_cast<uint32_t>(hr) << std::dec << '\n';
        if (errors && errors->GetBufferSize())
            std::cerr << static_cast<const char*>(errors->GetBufferPointer()) << '\n';
        throw std::runtime_error("offline comparison compile failed");
    }

    Microsoft::WRL::ComPtr<ID3D11ShaderReflection> reflection;
    const HRESULT reflect_hr = D3DReflect(code->GetBufferPointer(), code->GetBufferSize(),
        IID_ID3D11ShaderReflection, reinterpret_cast<void**>(reflection.GetAddressOf()));
    if (FAILED(reflect_hr) || !reflection)
        throw std::runtime_error("D3DReflect failed on valid FXC output");
    D3D11_SHADER_DESC desc{};
    if (FAILED(reflection->GetDesc(&desc))) throw std::runtime_error("shader reflection GetDesc failed");

    const auto* data = static_cast<const char*>(code->GetBufferPointer());
    const auto byte_hash = gpu::HashDofShaderSource(std::string_view(data, code->GetBufferSize()));
    return {code->GetBufferSize(), byte_hash.first, byte_hash.second,
        desc.InstructionCount, desc.TempRegisterCount, desc.FloatInstructionCount,
        desc.IntInstructionCount, desc.TextureNormalInstructions + desc.TextureLoadInstructions +
        desc.TextureCompInstructions, desc.StaticFlowControlCount, desc.DynamicFlowControlCount};
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: dof_shader_optimization_compare <output.csv>\n";
        return 2;
    }
    try {
        const auto variants = BuildManifest();
        if (variants.size() != kExpectedUniqueKeys)
            throw std::runtime_error("comparison manifest key count changed: expected " +
                                     std::to_string(kExpectedUniqueKeys) + ", got " +
                                     std::to_string(variants.size()));
        std::ofstream out(argv[1], std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot open requested CSV output");
        out << "entry,profile,source_bytes,source_hash0,source_hash1,"
               "o1_bytes,o1_byte_hash0,o1_byte_hash1,o1_instructions,o1_temp_registers,"
               "o1_float_instructions,o1_int_instructions,o1_texture_instructions,o1_static_flow,o1_dynamic_flow,"
               "o3_bytes,o3_byte_hash0,o3_byte_hash1,o3_instructions,o3_temp_registers,"
               "o3_float_instructions,o3_int_instructions,o3_texture_instructions,o3_static_flow,o3_dynamic_flow,"
               "bytecode_identical\n";
        for (const auto& [key, variant] : variants) {
            const Metrics o1 = CompileAndInspect(variant, gpu::DofShaderOptimization::O1);
            const Metrics o3 = CompileAndInspect(variant, gpu::DofShaderOptimization::O3);
            out << key.entry << ',' << key.profile << ',' << key.source_size << ",0x"
                << std::hex << key.hash0 << ",0x" << key.hash1 << std::dec << ','
                << o1.bytes << ",0x" << std::hex << o1.byte_hash0 << ",0x" << o1.byte_hash1 << std::dec << ','
                << o1.instructions << ',' << o1.temp_registers << ',' << o1.float_instructions << ','
                << o1.int_instructions << ',' << o1.texture_instructions << ',' << o1.static_flow << ','
                << o1.dynamic_flow << ',' << o3.bytes << ",0x" << std::hex << o3.byte_hash0 << ",0x"
                << o3.byte_hash1 << std::dec << ',' << o3.instructions << ',' << o3.temp_registers << ','
                << o3.float_instructions << ',' << o3.int_instructions << ',' << o3.texture_instructions << ','
                << o3.static_flow << ',' << o3.dynamic_flow << ','
                << (o1.bytes == o3.bytes && o1.byte_hash0 == o3.byte_hash0 &&
                    o1.byte_hash1 == o3.byte_hash1 ? "true" : "false") << '\n';
        }
        if (!out) throw std::runtime_error("failed writing comparison CSV");
        std::cout << "Compared O1 and O3 for " << variants.size()
                  << " unique static/native-particle DoF shader keys; no GPU timing performed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "DoF optimization comparison failed: " << error.what() << '\n';
        return 1;
    }
}
