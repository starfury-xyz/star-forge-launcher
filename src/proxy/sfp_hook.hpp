// Public API for writing C++ patches ("midhooks").
//
// A midhook replaces >= 5 bytes at an address with a jmp to a generated thunk that saves all
// registers and flags, calls your function with a Regs*, restores (possibly modified)
// registers, then either runs the replaced ("stolen") instructions and continues after them,
// or jumps to the address your function returns.
//
//   SFP_MIDHOOK(my_hook) {                 // r is sfp::Regs*
//       if (r->ecx == 0) return 0x401234;  // jump there (stolen bytes are NOT executed)
//       r->stack()[1] = 42;                // edit the first stack arg as seen at the hook site
//       return 0;                          // continue: run stolen bytes, resume after them
//   }
//
// Then reference it from a patch with  midhook = "my_hook"  (orig = the >= 5 stolen bytes). Hook
// files are compiled into the proxy DLL: cmake -DSF_PROXY_HOOKS=<folder of .cpp files>.
#pragma once
#include <cstdint>

namespace sfp {

struct Regs {
    uint32_t edi, esi, ebp, esp_pushad, ebx, edx, ecx, eax; // pushad order; esp_pushad is ignored on restore
    uint32_t eflags;
    uint32_t resume_; // internal: return value slot

    // The stack as it was at the hook site: stack()[0] is [esp], stack()[1] is [esp+4], ...
    uint32_t* stack() { return reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(this) + sizeof(Regs)); }
    template <class T> T* ptr(uint32_t addr) { return reinterpret_cast<T*>(uintptr_t(addr)); }
};
static_assert(sizeof(Regs) == 40, "Regs must match the thunk's push layout");

// Return 0 to continue normally, or an address to jump to instead.
using MidhookFn = uint32_t(__cdecl*)(Regs* r);

struct MidhookReg {
    const char* name;
    MidhookFn fn;
    MidhookReg* next;
    MidhookReg(const char* n, MidhookFn f);
};

MidhookFn find_midhook(const char* name);

void log(const char* fmt, ...);

} // namespace sfp

#define SFP_MIDHOOK(name)                                                     \
    static uint32_t __cdecl sfp_midhook_##name(::sfp::Regs* r);               \
    static ::sfp::MidhookReg sfp_midhook_reg_##name(#name, sfp_midhook_##name); \
    static uint32_t __cdecl sfp_midhook_##name([[maybe_unused]] ::sfp::Regs* r)
