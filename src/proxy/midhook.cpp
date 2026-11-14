#include "proxy/midhook.hpp"

#include <windows.h>

#include <cstring>
#include <vector>

namespace sfp {

static MidhookReg* g_hooks = nullptr;

MidhookReg::MidhookReg(const char* n, MidhookFn f) : name(n), fn(f), next(g_hooks) { g_hooks = this; }

MidhookFn find_midhook(const char* name) {
    for (auto h = g_hooks; h; h = h->next)
        if (std::strcmp(h->name, name) == 0) return h->fn;
    return nullptr;
}

// Bump allocator for thunks in one RWX region per process.
static uint8_t* alloc_thunk(size_t n) {
    static uint8_t* pool = nullptr;
    static size_t used = 0, cap = 0;
    if (!pool || used + n > cap) {
        cap = 0x10000;
        pool = static_cast<uint8_t*>(VirtualAlloc(nullptr, cap, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        used = 0;
        if (!pool) return nullptr;
    }
    uint8_t* p = pool + used;
    used += (n + 15) & ~size_t(15);
    return p;
}

static void put32(std::vector<uint8_t>& c, uint32_t v) {
    for (int i = 0; i < 4; i++) c.push_back(uint8_t(v >> (8 * i)));
}

std::string install_midhook(uint32_t site, const uint8_t* stolen, size_t n, MidhookFn fn) {
    if (n < 5) return "a midhook site needs at least 5 bytes";
    std::vector<uint8_t> c = {
        0x6a, 0x00,                   // push 0              ; resume slot
        0x9c,                         // pushfd
        0x60,                         // pushad
        0x54,                         // push esp            ; Regs*
        0xfc,                         // cld                 ; C++ expects DF=0 (flags restored by popfd)
        0xb8,                         // mov eax, fn
    };
    put32(c, uint32_t(uintptr_t(fn)));
    c.insert(c.end(), {
        0xff, 0xd0,                   // call eax
        0x83, 0xc4, 0x04,             // add esp, 4
        0x89, 0x44, 0x24, 0x24,       // mov [esp+0x24], eax ; resume slot
        0x61,                         // popad
        0x83, 0x7c, 0x24, 0x04, 0x00, // cmp dword [esp+4], 0
        0x74, 0x02,                   // je cont
        0x9d,                         // popfd
        0xc3,                         // ret                 ; pops slot -> jumps to returned address
        0x9d,                         // cont: popfd
        0x8d, 0x64, 0x24, 0x04,       // lea esp, [esp+4]    ; drop slot without touching flags
    });
    c.insert(c.end(), stolen, stolen + n);
    uint8_t* t = alloc_thunk(c.size() + 5);
    if (!t) return "VirtualAlloc for the thunk failed";
    c.push_back(0xe9);                // jmp site+n
    put32(c, uint32_t(site + n) - (uint32_t(uintptr_t(t)) + uint32_t(c.size()) + 4));
    std::memcpy(t, c.data(), c.size());
    FlushInstructionCache(GetCurrentProcess(), t, c.size());

    std::vector<uint8_t> j(n, 0x90);
    j[0] = 0xe9;
    uint32_t rel = uint32_t(uintptr_t(t)) - (site + 5);
    std::memcpy(&j[1], &rel, 4);
    void* p = reinterpret_cast<void*>(uintptr_t(site));
    DWORD old;
    if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old)) return "VirtualProtect failed at the site";
    std::memcpy(p, j.data(), n);
    VirtualProtect(p, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    return "";
}

}  // namespace sfp
