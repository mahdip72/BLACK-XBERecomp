#pragma once

#include <cstdint>

namespace kernel {
struct X86Context {
    uint32_t esp = 0;
    uint32_t eax = 0;
    uint32_t esi = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    uint32_t ebx = 0;
    uint32_t edi = 0;
    uint32_t ebp = 0;
    uint32_t eflags = 0;
};

uint8_t* TestGuestAddress(uint32_t address);
inline uint32_t& R32(uint32_t address) {
    return *reinterpret_cast<uint32_t*>(TestGuestAddress(address));
}
template <class T> T& G(uint32_t address) {
    return *reinterpret_cast<T*>(TestGuestAddress(address));
}
} // namespace kernel

#define X86_EXTERN_FUNC(name) void name(kernel::X86Context& ctx, uint8_t* base)
#define X86_FUNC(name) void name(kernel::X86Context& ctx, uint8_t* base)
