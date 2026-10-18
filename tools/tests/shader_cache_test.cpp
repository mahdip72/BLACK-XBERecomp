// Focused disk-cache regression. No D3D device or game process is created.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "d3d/shader_cache.h"

using Microsoft::WRL::ComPtr;
namespace {
void Check(bool okay,const char* what) {
    if (!okay) throw std::runtime_error(what);
}
bool SameBlob(ID3DBlob* a,ID3DBlob* b) {
    return a && b && a->GetBufferSize()==b->GetBufferSize() &&
           std::memcmp(a->GetBufferPointer(),b->GetBufferPointer(),a->GetBufferSize())==0;
}
ComPtr<ID3DBlob> Compile(std::string_view source,const char* entry,const char* profile,
                         UINT flags) {
    ComPtr<ID3DBlob> code,errors;
    const HRESULT hr=D3DCompile(source.data(),source.size(),nullptr,nullptr,nullptr,
                                entry,profile,flags,0,&code,&errors);
    if (FAILED(hr)) {
        if (errors) std::fprintf(stderr,"FXC: %s\n",
                                 static_cast<const char*>(errors->GetBufferPointer()));
        throw std::runtime_error("fixture FXC compile failed");
    }
    return code;
}
} // namespace

int main() {
    try {
        const std::string source="float4 main():SV_Target { return float4(.25,.5,.75,1); }";
        const std::string changed="float4 main():SV_Target { return float4(.5,.25,.75,1); }";
        constexpr UINT flags=D3DCOMPILE_OPTIMIZATION_LEVEL1;
        const auto root=std::filesystem::current_path()/"out"/"tests"/"shader-cache"/
                        std::to_string(GetCurrentProcessId());
        gpu::ShaderCache cache(root);
        const auto path=cache.FilePath(source,"main","ps_5_0",flags);
        Check(!path.empty(),"loaded FXC identity is unavailable");
        Check(!cache.Load(source,"main","ps_5_0",flags),"cold cache unexpectedly hit");
        const auto code=Compile(source,"main","ps_5_0",flags);
        cache.Store(source,"main","ps_5_0",flags,code.Get());
        Check(std::filesystem::is_regular_file(path),"cache Store did not publish a file");
        Check(SameBlob(code.Get(),cache.Load(source,"main","ps_5_0",flags).Get()),
              "warm cache bytecode differs from actual FXC output");
        Check(!cache.Load(changed,"main","ps_5_0",flags),"changed source reused stale bytecode");
        Check(!cache.Load(source,"other","ps_5_0",flags),"entry mismatch reused bytecode");
        Check(!cache.Load(source,"main","vs_5_0",flags),"profile mismatch reused bytecode");
        Check(!cache.Load(source,"main","ps_5_0",flags^1u),
              "compiler flag mismatch reused bytecode");
        Check(!cache.Load(source,"main","ps_5_0",uint64_t(flags)|(1ull<<32)),
              "secondary compiler flag mismatch reused bytecode");
        gpu::ShaderCache different_compiler(root,"different compiler identity");
        Check(!different_compiler.Load(source,"main","ps_5_0",flags),
              "compiler identity mismatch reused bytecode");
        Check(cache.FilePath(source,"main","../ps_5_0",flags).empty(),
              "profile path accepted directory traversal");

        // Simulate a filename-hash collision or misplaced cache file. The wrong
        // lookup path exists and has a valid checksum, so exact metadata must reject it.
        auto misfile=[&](const std::filesystem::path& destination) {
            Check(!destination.empty() && destination!=path,"invalid collision fixture path");
            std::filesystem::create_directories(destination.parent_path());
            std::filesystem::copy_file(path,destination,
                                       std::filesystem::copy_options::overwrite_existing);
        };
        misfile(cache.FilePath(changed,"main","ps_5_0",flags));
        Check(!cache.Load(changed,"main","ps_5_0",flags),
              "misfiled source passed exact-source validation");
        misfile(cache.FilePath(source,"other","ps_5_0",flags));
        Check(!cache.Load(source,"other","ps_5_0",flags),
              "misfiled entry passed exact-entry validation");
        misfile(cache.FilePath(source,"main","vs_5_0",flags));
        Check(!cache.Load(source,"main","vs_5_0",flags),
              "misfiled profile passed exact-profile validation");
        misfile(cache.FilePath(source,"main","ps_5_0",flags^1u));
        Check(!cache.Load(source,"main","ps_5_0",flags^1u),
              "misfiled flags passed exact-flags validation");
        misfile(different_compiler.FilePath(source,"main","ps_5_0",flags));
        Check(!different_compiler.Load(source,"main","ps_5_0",flags),
              "misfiled compiler identity passed exact-identity validation");

        { // Same path, invalid contents: exact metadata and checksum must both fail closed.
            std::fstream file(path,std::ios::binary|std::ios::in|std::ios::out);
            Check(bool(file),"cannot open stored bytecode for corruption fixture");
            file.seekg(-1,std::ios::end);
            const auto last=file.get();
            Check(last!=EOF,"empty cache record");
            file.seekp(-1,std::ios::end);
            file.put(char(last^0x40));
            Check(bool(file),"cannot corrupt stored bytecode fixture");
        }
        Check(!cache.Load(source,"main","ps_5_0",flags),
              "corrupt bytecode passed checksum validation");
        cache.Store(source,"main","ps_5_0",flags,code.Get());
        Check(SameBlob(code.Get(),cache.Load(source,"main","ps_5_0",flags).Get()),
              "atomic replacement did not restore cache entry");
        std::filesystem::resize_file(path,11);
        Check(!cache.Load(source,"main","ps_5_0",flags),
              "truncated cache entry was accepted");

        std::filesystem::create_directories(root);
        const auto blocker=root/"not-a-directory";
        {std::ofstream file(blocker,std::ios::binary);file.put('x');}
        gpu::ShaderCache blocked(blocker,"test compiler identity");
        blocked.Store(source,"main","ps_5_0",flags,code.Get());
        Check(!blocked.Load(source,"main","ps_5_0",flags),
              "unwritable cache location should fall through");
        std::puts("FXC shader cache miss/store/hit, misfiled exact inputs, corruption, truncation, and write failure PASS");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr,"FXC shader cache test FAILED: %s\n",e.what());
        return 1;
    }
}
