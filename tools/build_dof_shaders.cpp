#include <d3dcompiler.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "d3d/dof_shader_library.h"
#include "d3d/dof_shader_source.h"
#include "d3d/shader_cache.h"
#include "gpu/shaders.h"

namespace {

constexpr uint32_t kSamples[] = {1, 2, 4, 8};
constexpr size_t kExpectedUniqueKeys = 536;
const gpu::ShaderCache* g_bytecode_cache = nullptr;

struct Key {
    uint64_t hash0 = 0;
    uint64_t hash1 = 0;
    size_t source_size = 0;
    std::string entry;
    std::string profile;
    uint64_t flags = 0;

    auto Tie() const { return std::tie(hash0, hash1, source_size, entry, profile, flags); }
    bool operator<(const Key& other) const { return Tie() < other.Tie(); }
};

struct Record {
    Key key;
    std::string source;
    std::vector<uint8_t> bytecode;
};

std::string Quote(std::string_view value) {
    std::string result = "\"";
    for (unsigned char c : value) {
        if (c == '\\' || c == '"') {
            result += '\\';
            result += static_cast<char>(c);
        } else if (c >= 32 && c < 127) {
            result += static_cast<char>(c);
        } else {
            static constexpr char hex[] = "0123456789ABCDEF";
            result += "\\x";
            result += hex[c >> 4];
            result += hex[c & 15];
            result += "\"\""; // terminate the hex escape before the next literal byte
        }
    }
    result += '"';
    return result;
}

bool Add(std::map<Key, Record>& records, std::string source, std::string_view entry,
         std::string_view profile) {
    if (source.empty()) {
        std::cerr << "shader source builder returned empty source for " << entry << '\n';
        return false;
    }
    const uint64_t flags = gpu::DofShaderCompilerFlags(entry, profile);
    const auto hash = gpu::HashDofShaderSource(source);
    Key key{hash.first, hash.second, source.size(), std::string(entry), std::string(profile), flags};
    auto found = records.find(key);
    if (found != records.end()) {
        if (found->second.source != source) {
            std::cerr << "fatal DOF shader key collision for " << entry << ' ' << profile << '\n';
            return false;
        }
        return true;
    }

    Microsoft::WRL::ComPtr<ID3DBlob> code;
    if (g_bytecode_cache)
        code = g_bytecode_cache->Load(source, entry, profile, flags);
    if (!code) {
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        const HRESULT hr = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr,
                                      std::string(entry).c_str(), std::string(profile).c_str(),
                                      static_cast<UINT>(flags & 0xffffffffu),
                                      static_cast<UINT>(flags >> 32), &code, &errors);
        if (FAILED(hr) || !code || !code->GetBufferSize()) {
            std::cerr << "D3DCompile failed for " << entry << ' ' << profile << " hr=0x"
                      << std::hex << static_cast<uint32_t>(hr) << std::dec << '\n';
            if (errors && errors->GetBufferSize())
                std::cerr << static_cast<const char*>(errors->GetBufferPointer()) << '\n';
            return false;
        }
        if (g_bytecode_cache)
            g_bytecode_cache->Store(source, entry, profile, flags, code.Get());
    }

    Record record;
    record.key = key;
    record.source = std::move(source);
    const auto* begin = static_cast<const uint8_t*>(code->GetBufferPointer());
    record.bytecode.assign(begin, begin + code->GetBufferSize());
    records.emplace(std::move(key), std::move(record));
    return true;
}

bool AddPair(std::map<Key, Record>& records, std::string source, std::string_view vs_entry,
             std::string_view ps_entry) {
    return Add(records, source, vs_entry, "vs_5_0") &&
           Add(records, std::move(source), ps_entry, "ps_5_0");
}

bool Build(std::map<Key, Record>& records) {
    struct StaticPass { std::string_view entry; uint32_t samples; };
    constexpr StaticPass static_passes[] = {
        {"depth_of_field", 1},
        {"depth_of_field_gather", 1},
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
    for (const auto& pass : static_passes) {
        auto source = gpu::DofStaticPassSource(pass.entry, pass.samples);
        if (!AddPair(records, std::move(source), "vs", pass.entry)) return false;
    }

    if (!Add(records, gpu::DofStaticPassSource("viewmodel_dof_coverage", 1),
             "viewmodel_dof_coverage", "ps_5_0")) return false;
    for (uint32_t samples : kSamples) {
        if (!Add(records, gpu::DofStaticPassSource("viewmodel_dof_pack", samples),
                 "viewmodel_dof_tiles_vs", "vs_5_0")) return false;
    }
    if (!Add(records, gpu::DofStaticPassSource("viewmodel_dof"),
             "viewmodel_dof_tiles_vs", "vs_5_0")) return false;

    for (uint32_t samples : kSamples) {
        for (bool float_bounds : {false, true}) {
            if (float_bounds && samples != 2) continue;
            auto source = gpu::DofParticleRangeSource(samples, float_bounds);
            if (!AddPair(records, std::move(source), "vs", "range_ps")) return false;
        }
    }
    if (!AddPair(records, gpu::kParticleDofAtlasFilterHlsl, "vs", "filter_ps")) return false;

    for (uint32_t pass = 0; pass <= 5; ++pass) {
        if (!Add(records, gpu::DofParticleVertexSource(pass), "main", "vs_5_0")) return false;
    }
    constexpr int alpha_functions[] = {0, 1, 2, 3, 4, 5, 6, 7};
    for (uint32_t samples : kSamples) {
        for (bool float_bounds : {false, true}) {
            if (float_bounds && samples != 2) continue;
            for (uint32_t blend = 0; blend <= 1; ++blend) {
                for (int alpha_function : alpha_functions) {
                    for (bool alpha_kill : {false, true}) {
                        for (uint32_t pass = 0; pass <= 2; ++pass) {
                            auto source = gpu::DofParticlePixelSource(
                                samples, blend, alpha_function, alpha_kill, pass, float_bounds);
                            if (!Add(records, std::move(source), "main", "ps_5_0")) return false;
                        }
                    }
                }
            }
        }
    }
    for (uint32_t pass = 3; pass <= 5; ++pass) {
        for (uint32_t blend = 0; blend < (pass == 4 ? 2u : 1u); ++blend) {
            for (bool float_bounds : {false, true})
                if (!Add(records, gpu::DofParticlePixelSource(2, blend, 0, false, pass, float_bounds), "main", "ps_5_0")) return false;
        }
    }
    return true;
}

bool Write(const std::filesystem::path& output, const std::map<Key, Record>& records) {
    std::filesystem::create_directories(output.parent_path());
    auto temporary = output;
    temporary += ".tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "cannot open generated shader source: " << temporary.string() << '\n';
        return false;
    }

    out << "// Generated by tools/build_dof_shaders.cpp; do not edit.\n"
           "#include \"d3d/dof_shader_library.h\"\n"
           "#include <array>\n#include <cstring>\n\n"
           "namespace gpu { namespace {\n"
           "struct Record { uint64_t hash0, hash1; size_t source_size; const char* entry; "
           "const char* profile; uint64_t flags; const uint8_t* data; size_t bytecode_size; };\n";

    size_t index = 0;
    for (const auto& [key, record] : records) {
        out << "alignas(16) constexpr uint8_t kShader" << index << "[] = {\n    ";
        for (size_t i = 0; i < record.bytecode.size(); ++i) {
            if (i && i % 16 == 0) out << "\n    ";
            out << "0x" << std::hex << static_cast<unsigned>(record.bytecode[i]) << std::dec;
            if (i + 1 != record.bytecode.size()) out << ',';
        }
        out << "\n};\n";
        ++index;
    }

    out << "const Record kRecords[] = {\n";
    index = 0;
    for (const auto& [key, record] : records) {
        out << "    {0x" << std::hex << key.hash0 << "ull, 0x" << key.hash1 << "ull, "
            << std::dec << key.source_size << ", " << Quote(key.entry) << ", " << Quote(key.profile)
            << ", 0x" << std::hex << key.flags << "ull, kShader" << std::dec << index << ", "
            << record.bytecode.size() << "},\n";
        ++index;
    }
    out << "};\n} // namespace\n\n"
           "DofShaderBytecode FindDofShaderBytecode(std::string_view source, std::string_view entry, "
           "std::string_view profile, uint64_t compiler_flags) {\n"
           "    const auto hash = HashDofShaderSource(source);\n"
           "    for (const auto& r : kRecords) {\n"
           "        if (r.hash0 == hash.first && r.hash1 == hash.second && r.source_size == source.size() &&\n"
           "            entry == r.entry && profile == r.profile && compiler_flags == r.flags)\n"
           "            return {r.data, r.bytecode_size};\n"
           "    }\n"
           "    return {};\n"
           "}\n\n} // namespace gpu\n";
    out.close();
    if (!out) {
        std::cerr << "failed writing generated shader source: " << temporary.string() << '\n';
        return false;
    }
    std::error_code error;
    std::filesystem::remove(output, error);
    error.clear();
    std::filesystem::rename(temporary, output, error);
    if (error) {
        std::cerr << "cannot publish generated shader source: " << error.message() << '\n';
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: build_dof_shaders <generated-cpp-path>\n";
        return 2;
    }
    const std::filesystem::path output(argv[1]);
    const gpu::ShaderCache bytecode_cache(output.parent_path() / "dof-bytecode-cache");
    g_bytecode_cache = &bytecode_cache;

    std::map<Key, Record> records;
    if (!Build(records)) return 1;
    if (records.size() != kExpectedUniqueKeys) {
        std::cerr << "DOF shader manifest key count changed: expected " << kExpectedUniqueKeys
                  << ", produced " << records.size() << " unique keys\n";
        return 1;
    }
    if (!Write(output, records)) return 1;
    std::cout << "Embedded " << records.size() << " unique DOF shader bytecode keys\n";
    return 0;
}
