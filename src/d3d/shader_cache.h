#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

namespace gpu {

// Optional FXC bytecode cache. A filename hash is only a lookup hint: every hit
// verifies the exact inputs, compiler identity, record length, and blob checksum.
// Any unavailable or invalid entry simply falls through to the caller's compile.
class ShaderCache {
public:
    explicit ShaderCache(std::filesystem::path root = {},
                         std::string compiler_identity = {})
        : root_(root.empty() ? ExecutableCacheRoot() : std::move(root)),
          identity_(compiler_identity.empty() ? CompilerIdentity() : std::move(compiler_identity)) {}

    Microsoft::WRL::ComPtr<ID3DBlob> Load(std::string_view source,
        std::string_view entry,std::string_view profile,uint64_t flags) const {
        try {
        Microsoft::WRL::ComPtr<ID3DBlob> blob;
        const auto path=FilePath(source,entry,profile,flags);
        if (path.empty()) return blob;
        std::ifstream file(path,std::ios::binary|std::ios::ate);
        if (!file) return blob;
        const std::streamoff length=file.tellg();
        if (length<0 || uint64_t(length)>kMaxRecord) return blob;
        std::string bytes(size_t(length),'\0');
        file.seekg(0);
        if (!file.read(bytes.data(),static_cast<std::streamsize>(length))) return blob;
        if (bytes.size()<kHeaderBytes ||
            std::memcmp(bytes.data(),"FXCCACHE",8)!=0) return blob;
        size_t pos=8;
        uint32_t version=0,src_length=0,entry_length=0,
                 profile_length=0,identity_length=0,blob_length=0;
        uint64_t saved_flags=0,checksum=0;
        if (!Read32(bytes,pos,version) || !Read64(bytes,pos,saved_flags) ||
            !Read32(bytes,pos,src_length) || !Read32(bytes,pos,entry_length) ||
            !Read32(bytes,pos,profile_length) || !Read32(bytes,pos,identity_length) ||
            !Read32(bytes,pos,blob_length) || !Read64(bytes,pos,checksum) ||
            version!=1 || saved_flags!=flags ||
            src_length!=source.size() || entry_length!=entry.size() ||
            profile_length!=profile.size() || identity_length!=identity_.size() ||
            blob_length==0 || blob_length>kMaxBlob ||
            uint64_t(pos)+src_length+entry_length+profile_length+
                identity_length+blob_length!=bytes.size()) return blob;
        const uint64_t actual=Hash(bytes.data(),kHeaderBytes-8,
                                   Hash(bytes.data()+kHeaderBytes,
                                        bytes.size()-kHeaderBytes));
        if (actual!=checksum) return blob;
        auto equal=[&](std::string_view expected) {
            if (bytes.compare(pos,expected.size(),expected)!=0) return false;
            pos+=expected.size();
            return true;
        };
        if (!equal(source) || !equal(entry) || !equal(profile) ||
            !equal(identity_)) return blob;
        if (FAILED(D3DCreateBlob(blob_length,&blob))) return {};
        std::memcpy(blob->GetBufferPointer(),bytes.data()+pos,blob_length);
        return blob;
        } catch (const std::exception&) { return {}; }
    }

    void Store(std::string_view source,std::string_view entry,
               std::string_view profile,uint64_t flags,ID3DBlob* blob) const {
        try {
        const auto path=FilePath(source,entry,profile,flags);
        if (path.empty() || !blob || blob->GetBufferSize()==0 ||
            blob->GetBufferSize()>kMaxBlob) return;
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(),ec);
        if (ec) return;
        std::string record;
        record.reserve(kHeaderBytes+source.size()+entry.size()+profile.size()+
                       identity_.size()+blob->GetBufferSize());
        record.append("FXCCACHE",8);
        Append32(record,1);
        Append64(record,flags);
        Append32(record,uint32_t(source.size()));
        Append32(record,uint32_t(entry.size()));
        Append32(record,uint32_t(profile.size()));
        Append32(record,uint32_t(identity_.size()));
        Append32(record,uint32_t(blob->GetBufferSize()));
        const size_t checksum_at=record.size();
        Append64(record,0);
        record.append(source);
        record.append(entry);
        record.append(profile);
        record.append(identity_);
        record.append(static_cast<const char*>(blob->GetBufferPointer()),blob->GetBufferSize());
        const uint64_t checksum=Hash(record.data(),kHeaderBytes-8,
                                      Hash(record.data()+kHeaderBytes,
                                           record.size()-kHeaderBytes));
        for (unsigned i=0;i<8;++i) record[checksum_at+i]=char(checksum>>(i*8));
        static std::atomic<uint64_t> sequence{0};
        auto temporary=path;
        temporary+=L".tmp-"+std::to_wstring(GetCurrentProcessId())+L"-"+
                   std::to_wstring(++sequence);
        HANDLE handle=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL,nullptr);
        if (handle==INVALID_HANDLE_VALUE) return;
        DWORD written=0;
        const bool okay=WriteFile(handle,record.data(),DWORD(record.size()),&written,nullptr) &&
                        written==record.size();
        CloseHandle(handle);
        if (!okay || !MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING))
            DeleteFileW(temporary.c_str());
        } catch (const std::exception&) { return; }
    }

    std::filesystem::path FilePath(std::string_view source,std::string_view entry,
        std::string_view profile,uint64_t flags) const {
        if (root_.empty() || identity_.empty() || source.empty() ||
            source.size()>kMaxSource || entry.empty() || entry.size()>128 ||
            profile.empty() || profile.size()>128 || identity_.size()>4096) return {};
        for (char c:profile)
            if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||
                  (c>='0'&&c<='9')||c=='_'||c=='-')) return {};
        std::string key;
        Append64(key,flags);
        Append32(key,uint32_t(source.size()));key.append(source);
        Append32(key,uint32_t(entry.size()));key.append(entry);
        Append32(key,uint32_t(profile.size()));key.append(profile);
        Append32(key,uint32_t(identity_.size()));key.append(identity_);
        const uint64_t hash=Hash(key.data(),key.size());
        wchar_t filename[32];
        std::swprintf(filename,32,L"%016llx.fxc",static_cast<unsigned long long>(hash));
        return root_/std::filesystem::path(std::string(profile))/filename;
    }

private:
    static constexpr uint32_t kMaxSource=16u*1024*1024;
    static constexpr uint32_t kMaxBlob=64u*1024*1024;
    static constexpr uint64_t kMaxRecord=uint64_t(kMaxSource)+kMaxBlob+8192;
    static constexpr size_t kHeaderBytes=8+6*4+2*8;
    static uint64_t Hash(const void* data,size_t bytes,
                         uint64_t seed=14695981039346656037ull) {
        const auto* p=static_cast<const uint8_t*>(data);
        for (size_t i=0;i<bytes;++i) {seed^=p[i];seed*=1099511628211ull;}
        return seed;
    }
    static void Append32(std::string& out,uint32_t value) {
        for (unsigned i=0;i<4;++i) out.push_back(char(value>>(i*8)));
    }
    static void Append64(std::string& out,uint64_t value) {
        for (unsigned i=0;i<8;++i) out.push_back(char(value>>(i*8)));
    }
    static bool Read32(const std::string& bytes,size_t& pos,uint32_t& value) {
        if (pos+4>bytes.size()) return false;
        value=0;
        for (unsigned i=0;i<4;++i) value|=uint32_t(uint8_t(bytes[pos++]))<<(i*8);
        return true;
    }
    static bool Read64(const std::string& bytes,size_t& pos,uint64_t& value) {
        if (pos+8>bytes.size()) return false;
        value=0;
        for (unsigned i=0;i<8;++i) value|=uint64_t(uint8_t(bytes[pos++]))<<(i*8);
        return true;
    }
    static std::filesystem::path ExecutableCacheRoot() {
        std::wstring path(32768,L'\0');
        const DWORD n=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));
        if (!n || n>=path.size()) return {};
        path.resize(n);
        return std::filesystem::path(path).parent_path()/L"cache"/L"shaders"/L"v1";
    }
    static std::string CompilerIdentity() {
        const HMODULE compiler=GetModuleHandleW(L"d3dcompiler_47.dll");
        if (!compiler) return {};
        std::wstring path(32768,L'\0');
        const DWORD n=GetModuleFileNameW(compiler,path.data(),DWORD(path.size()));
        if (!n || n>=path.size()) return {};
        path.resize(n);
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&info)) return {};
        std::string id(reinterpret_cast<const char*>(path.data()),path.size()*sizeof(wchar_t));
        Append32(id,info.nFileSizeLow);Append32(id,info.nFileSizeHigh);
        Append32(id,info.ftLastWriteTime.dwLowDateTime);
        Append32(id,info.ftLastWriteTime.dwHighDateTime);
        return id;
    }
    std::filesystem::path root_;
    std::string identity_;
};

} // namespace gpu
