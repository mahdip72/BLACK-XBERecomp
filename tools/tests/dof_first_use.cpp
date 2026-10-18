// Consumer-side validation of the production embedded DOF shader library.
// This test never starts the game or creates a graphics device.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "d3d/dof_shader_library.h"
#include "d3d/dof_shader_source.h"
#include "d3d/particle_dof_shader.h"

using Microsoft::WRL::ComPtr;
namespace {

constexpr uint64_t kFlags=D3DCOMPILE_OPTIMIZATION_LEVEL1; // Flags2 is zero in the renderer.
struct Counters { unsigned static_pairs=0,range_pairs=0,atlas_pairs=0,particle_vs=0,
                           particle_ps=0,lookups=0; };
void Check(bool value,const char* message) {
    if (!value) throw std::runtime_error(message);
}
void RequireEmbedded(std::string_view source,std::string_view entry,
                     std::string_view profile,Counters& count) {
    const auto bytes=gpu::FindDofShaderBytecode(source,entry,profile,kFlags);
    Check(bytes.data && bytes.size>16,"production DOF shader variant is absent from the embedded library");
    Check(std::memcmp(bytes.data,"DXBC",4)==0,
          "embedded DOF variant does not start with an FXC bytecode container");
    const auto blob=gpu::LoadDofShaderBytecode(source,entry,profile,kFlags);
    Check(blob && blob->GetBufferSize()==bytes.size &&
          std::memcmp(blob->GetBufferPointer(),bytes.data,bytes.size)==0,
          "embedded DOF loader did not return the indexed bytecode");
    ++count.lookups;
}
void RequirePair(std::string_view source,std::string_view pixel_entry,Counters& count) {
    RequireEmbedded(source,"vs","vs_5_0",count);
    RequireEmbedded(source,pixel_entry,"ps_5_0",count);
}
void CheckStaticManifest(Counters& count) {
    // Independently transcribed from D3D12Backend's actual call sites. Keeping this
    // consumer list separate from the generator catches missing build-time variants.
    const auto one=[&](const char* entry) {
        const auto source=gpu::DofStaticPassSource(entry,1);
        RequirePair(source,entry,count);
        ++count.static_pairs;
    };
    const auto ms=[&](const char* entry) {
        for (uint32_t samples:{1u,2u,4u,8u}) {
            const auto source=gpu::DofStaticPassSource(entry,samples);
            RequirePair(source,entry,count);
            ++count.static_pairs;
        }
    };
    one("depth_of_field");
    one("depth_of_field_gather");
    ms("depth_of_field_pack");
    one("depth_of_field_downsample");
    ms("viewmodel_dof_pack");
    one("viewmodel_dof");
    ms("ffx_dof_prepare");
    one("ffx_dof_present");
    Check(count.static_pairs==17,"static DOF call-site matrix changed without updating test");

    for (uint32_t samples:{1u,2u,4u,8u}) {
        const auto source=gpu::DofParticleRangeSource(samples);
        RequirePair(source,"range_ps",count);
        ++count.range_pairs;
    }
    RequirePair(gpu::kParticleDofAtlasFilterHlsl,"filter_ps",count);
    ++count.atlas_pairs;
}
void CheckParticleMatrix(Counters& count) {
    Check(gpu::DofParticleVertexSource(0)==gpu::DofParticleVertexSource(2) &&
          gpu::DofParticleVertexSource(0)!=gpu::DofParticleVertexSource(1),
          "particle VS pass deduplication changed unexpectedly");
    for (uint32_t pass:{0u,1u,2u}) {
        const auto source=gpu::DofParticleVertexSource(pass);
        Check(!source.empty(),"particle VS source generator rejected a production pass");
        RequireEmbedded(source,"main","vs_5_0",count);
        ++count.particle_vs;
    }
    for (uint32_t pass:{0u,1u,2u})
        for (uint32_t samples:{1u,2u,4u,8u})
            for (uint32_t blend:{0u,1u})
                for (int alpha=0;alpha<8;++alpha)
                    for (bool kill:{false,true}) {
                        const auto source=gpu::DofParticlePixelSource(
                            samples,blend,alpha,kill,pass);
                        Check(!source.empty(),"particle PS source generator rejected a production mode");
                        RequireEmbedded(source,"main","ps_5_0",count);
                        ++count.particle_ps;
                    }
    Check(count.particle_vs==3 && count.particle_ps==384,
          "particle DOF consumer matrix changed without updating test");
}
void CheckSelectedBytecode(std::string_view source,const char* entry,const char* profile,
                           const char* label) {
    const auto embedded=gpu::FindDofShaderBytecode(source,entry,profile,kFlags);
    Check(embedded.data && embedded.size,"selected embedded DOF shader is missing");
    ComPtr<ID3DBlob> compiled,errors;
    const HRESULT hr=D3DCompile(source.data(),source.size(),nullptr,nullptr,nullptr,
                                entry,profile,UINT(kFlags),0,&compiled,&errors);
    if (FAILED(hr) && errors)
        std::fprintf(stderr,"%s FXC: %s\n",label,
                     static_cast<const char*>(errors->GetBufferPointer()));
    Check(SUCCEEDED(hr) && compiled,"reference FXC compile failed");
    Check(compiled->GetBufferSize()==embedded.size &&
          std::memcmp(compiled->GetBufferPointer(),embedded.data,embedded.size)==0,
          "embedded DOF bytecode differs from FXC for the exact production source");
    std::printf("byte-exact FXC: %s (%zu bytes)\n",label,embedded.size);
}
void CheckMissingVariants() {
    const auto source=gpu::DofStaticPassSource("depth_of_field_pack",4);
    const auto miss=[&](std::string_view text,std::string_view entry,
                        std::string_view profile,uint64_t flags) {
        const auto bytes=gpu::FindDofShaderBytecode(text,entry,profile,flags);
        Check(!bytes.data && bytes.size==0,"unknown DOF shader variant unexpectedly resolved");
        Check(!gpu::LoadDofShaderBytecode(text,entry,profile,flags),
              "production DOF loader compiled or returned a missing variant");
    };
    const auto changed=source+"\n// intentionally unembedded variant\n";
    miss(changed,"depth_of_field_pack","ps_5_0",kFlags);
    miss(source,"unlisted_entry","ps_5_0",kFlags);
    miss(source,"depth_of_field_pack","vs_5_0",kFlags);
    miss(source,"depth_of_field_pack","ps_5_0",kFlags^1ull);
    miss(source,"depth_of_field_pack","ps_5_0",kFlags|(1ull<<32));
}
} // namespace

int main(int argc,char** argv) {
    try {
        const bool no_fxc=argc==2 && std::strcmp(argv[1],"--no-fxc")==0;
        Check(argc==1 || no_fxc,"usage: dof_first_use [--no-fxc]");
        const auto exe=std::filesystem::path([] {
            std::wstring path(32768,L'\0');
            const DWORD n=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));
            Check(n && n<path.size(),"GetModuleFileNameW failed");
            path.resize(n);
            return path;
        }());
        const auto cache_root=exe.parent_path()/"cache"/"shaders"/"v1";
        std::error_code cache_error;
        const bool initially_empty=!std::filesystem::exists(cache_root,cache_error);
        Counters count;
        CheckStaticManifest(count);
        CheckParticleMatrix(count);
        CheckMissingVariants();
        if (!no_fxc) {
            CheckSelectedBytecode(gpu::DofStaticPassSource("depth_of_field_pack",8),
                                  "depth_of_field_pack","ps_5_0","world pack MS8");
            CheckSelectedBytecode(gpu::DofParticleVertexSource(1),"main","vs_5_0",
                                  "particle atlas source VS");
            CheckSelectedBytecode(gpu::DofParticlePixelSource(8,1,4,true,2),"main","ps_5_0",
                                  "particle additive alpha-test MS8 composite");
            CheckSelectedBytecode(gpu::kParticleDofAtlasFilterHlsl,"filter_ps","ps_5_0",
                                  "particle atlas filter");
        }
        if (initially_empty) {
            cache_error.clear();
            Check(!std::filesystem::exists(cache_root,cache_error),
                  "embedded lookup wrote an executable-local FXC disk cache");
        }
        std::printf("embedded DOF first use PASS: %u static pairs, %u range pairs, %u atlas pair, "
                    "%u particle VS, %u particle PS; %u successful lookups; cache initially %s; FXC references %s\n",
                    count.static_pairs,count.range_pairs,count.atlas_pairs,
                    count.particle_vs,count.particle_ps,count.lookups,
                    initially_empty?"absent":"already present",no_fxc?"disabled":"four byte-exact");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr,"embedded DOF first use FAILED: %s\n",error.what());
        return 1;
    }
}
