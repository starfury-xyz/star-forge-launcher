// The `inject` patch method: applies the core patches the pack asks for, then the pack's own
// patchsets, to the game's memory and loads its native plugins, all before the game's own code
// runs. Port of crates/sc-agent/src/patches.rs.
//
// A patchset for game.exe is applied at once (the process is still suspended). One for a DLL
// that isn't loaded yet is applied the moment the loader maps it, before its DllMain runs.

#include "agent/patches.hpp"

#include <windows.h>

#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

#include "agent/agent.hpp"
#include "agent/plugin_api.h"
#include "common/file.hpp"
#include "common/text.hpp"
#include "patch/core.hpp"
#include "patch/patchset.hpp"
#include "patch/sha1.hpp"

namespace sf::agent::patches {
namespace {

using patch::Patch;
using patch::Patchset;
using profile::OnError;
using profile::Profile;

// A loaded module, addressed the way its patchset is: patchset address - image_base = offset
// into the module, wherever Windows actually loaded it.
class Memory : public patch::Target {
public:
    Memory(uint8_t* base, uint64_t image_base, uint64_t size) : base_(base), image_base_(image_base), size_(size) {}

    bool read(uint64_t va, uint8_t* out, size_t n) override {
        uint8_t* p = at(va, n);
        if (!p) return false;
        std::memcpy(out, p, n);
        return true;
    }

    bool write(uint64_t va, const uint8_t* data, size_t n) override {
        uint8_t* p = at(va, n);
        if (!p) return false;
        // Code pages are read-only: unlock, write, restore, and make the CPU drop any cached
        // copy of the old instructions.
        DWORD old = 0;
        if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old)) return false;
        std::memcpy(p, data, n);
        VirtualProtect(p, n, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, n);
        return true;
    }

private:
    uint8_t* at(uint64_t va, size_t n) const {
        if (va < image_base_) return nullptr;
        const uint64_t rva = va - image_base_;
        if (rva + n > size_) return nullptr;  // outside the module
        return base_ + rva;
    }
    uint8_t* base_;
    uint64_t image_base_, size_;
};

// Size of a mapped image, from its PE optional header (SizeOfImage).
uint64_t image_size(const uint8_t* base) {
    uint32_t nt_offset;
    std::memcpy(&nt_offset, base + 0x3c, 4);
    uint32_t size;
    std::memcpy(&size, base + nt_offset + 0x50, 4);
    return size;
}

std::optional<std::wstring> module_path(HMODULE module) {
    std::wstring buf(1024, L'\0');
    DWORD n = GetModuleFileNameW(module, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return std::nullopt;
    buf.resize(n);
    return buf;
}

bool eq_ignore_ascii_case(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

// The player's on/off choices for one module (module names compared ignoring case).
const patch::Choices* choices(const Profile& profile, const std::string& exe) {
    for (const auto& entry : profile.patch_choices)
        if (eq_ignore_ascii_case(entry.first, exe)) return &entry.second;
    return nullptr;
}

// Applies one patchset to a loaded module. Every enabled patch is verified before anything is
// written, so a set never half-applies. Returns "" or a failure for the pack's on_error to decide.
std::string apply_set(const Patchset& set, HMODULE module, const Profile& profile) {
    auto path = module_path(module);
    if (!path) return "cannot find the module file";
    std::string file;
    if (unsigned long err = read_file(*path, file)) return utf8(*path) + ": " + win_error(err);
    const std::string sha = patch::sha1_hex(file.data(), file.size());
    if (!set.accepts(sha))
        return set.exe + " is a different build (sha1 " + sha + ", the patches need " + set.expected_sha1() + ")";

    auto* base = reinterpret_cast<uint8_t*>(module);
    Memory mem(base, set.image_base, image_size(base));
    patch::Enabled en = set.enabled(choices(profile, set.exe));
    if (auto o = patch::overlaps(en.on); !o.empty()) return "enabled patches overlap: " + o.front();

    std::vector<const Patch*> go;
    std::string failed;
    for (const Patch* p : en.on) {
        patch::State s = patch::check(mem, *p);
        std::string why;
        if (s.kind == patch::State::Applied) {
            log("%s: %s already applied", set.exe.c_str(), p->id.c_str());
            continue;
        } else if (s.kind == patch::State::Mismatch) {
            why = s.why;
        } else {
            // x64 midhook thunks don't exist yet; hooks go in plugins for now.
            for (const auto& w : p->writes)
                if (!w.midhook.empty()) {
                    why = "midhook \"" + w.midhook + "\" is not supported for the client yet";
                    break;
                }
        }
        if (why.empty()) go.push_back(p);
        else if (p->optional) log("%s: skipping optional patch %s: %s", set.exe.c_str(), p->id.c_str(), why.c_str());
        else failed += (failed.empty() ? "" : "; ") + p->id + ": " + why;
    }
    if (!failed.empty()) return set.exe + ": " + failed;

    for (const Patch* p : go) {
        if (std::string err = patch::apply(mem, *p); !err.empty()) return set.exe + ": " + p->id + ": " + err;
        const std::string title = p->title.empty() ? "" : " (" + p->title + ")";
        log("%s: applied %s%s, %zu write(s)", set.exe.c_str(), p->id.c_str(), title.c_str(), p->writes.size());
    }
    log("%s: %zu patch(es) applied", set.exe.c_str(), go.size());
    return "";
}

// Logs a failure and returns whether the game may still run (the pack's patches.on_error).
bool may_continue(OnError on_error, const std::string& msg) {
    if (on_error == OnError::Exit) {
        log("ERROR: %s", msg.c_str());
        log("not starting the game (patches.on_error is \"exit\" in pack.json)");
        return false;
    }
    log("ERROR: %s (continuing: patches.on_error is \"continue\")", msg.c_str());
    return true;
}

// --- patchsets for DLLs that load later ------------------------------------------------------------

// Patchsets waiting for their DLL, and the profile apply_set needs then. Only touched under the
// lock: the loader calls on_dll_load from whichever thread loads a DLL.
std::mutex g_pending_lock;
std::vector<Patchset> g_pending;
Profile g_pending_profile;

// What the loader passes to a DLL notification (LDR_DLL_NOTIFICATION_DATA, undocumented-ish
// but stable since Vista; the Rust agent uses the same layout).
struct UnicodeString {
    USHORT length;  // in bytes
    USHORT maximum_length;
    const wchar_t* buffer;
};
struct DllLoaded {
    ULONG flags;
    const UnicodeString* full_name;
    const UnicodeString* base_name;
    void* base;
    ULONG size;
};
constexpr ULONG kDllLoaded = 1;  // LDR_DLL_NOTIFICATION_REASON_LOADED

using NotifyFn = void(NTAPI*)(ULONG reason, const DllLoaded* data, void* context);
using RegisterFn = LONG(NTAPI*)(ULONG flags, NotifyFn callback, void* context, void** cookie);

// Called by the loader after it maps a DLL and before the DLL's DllMain.
void NTAPI on_dll_load(ULONG reason, const DllLoaded* data, void*) {
    if (reason != kDllLoaded || !data || !data->base_name || !data->base_name->buffer) return;
    const std::string name = utf8(std::wstring(data->base_name->buffer, data->base_name->length / sizeof(wchar_t)));
    try {
        std::lock_guard<std::mutex> lock(g_pending_lock);
        for (size_t i = 0; i < g_pending.size(); ++i) {
            if (!g_pending[i].targets(name)) continue;
            Patchset set = std::move(g_pending[i]);
            g_pending.erase(g_pending.begin() + i);
            std::string err = apply_set(set, static_cast<HMODULE>(data->base), g_pending_profile);
            if (!err.empty() && !may_continue(g_pending_profile.patches.on_error, err))
                TerminateProcess(GetCurrentProcess(), 1);  // too late to refuse cleanly: the game is running
            return;
        }
    } catch (...) {
        // Never let an exception escape into the loader.
    }
}

bool watch_dll_loads() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto reg = reinterpret_cast<RegisterFn>(GetProcAddress(ntdll, "LdrRegisterDllNotification"));
    if (!reg) return false;
    void* cookie = nullptr;
    return reg(0, on_dll_load, nullptr, &cookie) == 0;
}

// --- plugins ------------------------------------------------------------------------------------------

extern "C" void plugin_log(const uint8_t* text, size_t len) {
    if (!text) return;
    log("plugin: %s", std::string(reinterpret_cast<const char*>(text), len).c_str());
}

// Loads the pack's native plugins (see plugin_api.h).
bool load_plugins(const std::wstring& profile_dir, const Profile& profile) {
    const OnError on_error = profile.patches.on_error;
    for (const std::string& rel : profile.patches.plugins) {
        auto inner = profile::patches_rel(rel);
        if (!inner) continue;  // the launcher validated pack.json; skip anything unsafe
        std::wstring path = profile_dir + L"\\" + profile::kPatchesDir + L"\\" + wide(*inner);
        for (auto& c : path)
            if (c == L'/') c = L'\\';
        HMODULE module = LoadLibraryW(path.c_str());
        if (!module) {
            if (!may_continue(on_error, "plugin " + rel + ": could not be loaded")) return false;
            continue;
        }
        int code = 0;
        if (auto init = reinterpret_cast<StarForgePluginInit>(GetProcAddress(module, "starforge_plugin_init"))) {
            StarForgePluginApi api{1, profile_dir.c_str(), plugin_log};
            code = init(&api);
        }
        if (code != 0) {
            if (!may_continue(on_error, "plugin " + rel + ": init returned " + std::to_string(code))) return false;
            continue;
        }
        log("plugin %s: loaded", rel.c_str());
    }
    return true;
}

}  // namespace

bool apply_all(const std::wstring& profile_dir, const Profile& profile) {
    const OnError on_error = profile.patches.on_error;

    // Every set to apply, in order: core patches, then the pack's own; or the error reading one.
    struct Entry {
        std::optional<Patchset> set;
        std::string error;
    };
    std::vector<Entry> entries;
    try {
        for (Patchset& s : patch::core::sets(patch::core::requested(profile.patches.core, profile.patch_choices)))
            entries.push_back({std::move(s), ""});
    } catch (const std::exception& e) {
        entries.push_back({std::nullopt, e.what()});
    }
    for (const std::wstring& file : profile::patchset_files(profile_dir)) {
        const std::string name = utf8(file.substr(file.rfind(L'\\') + 1));
        std::string text;
        if (unsigned long err = read_file(file, text)) {
            entries.push_back({std::nullopt, name + ": " + win_error(err)});
            continue;
        }
        try {
            entries.push_back({Patchset::parse(text, name), ""});
        } catch (const std::exception& e) {
            entries.push_back({std::nullopt, e.what()});
        }
    }

    // Switches in local.json naming patches nobody has (checked per module across every set).
    std::vector<const Patchset*> loaded;
    for (const Entry& e : entries)
        if (e.set) loaded.push_back(&*e.set);
    for (const auto& w : patch::core::unknown_choices(loaded, profile.patch_choices)) log("warning: %s", w.c_str());

    std::string own_exe;  // this process's exe file name, e.g. game.exe
    if (auto p = module_path(nullptr)) own_exe = utf8(p->substr(p->rfind(L'\\') + 1));

    std::vector<Patchset> later;
    for (Entry& e : entries) {
        if (!e.set) {
            if (!may_continue(on_error, e.error)) return false;
            continue;
        }
        Patchset& set = *e.set;
        HMODULE module = set.targets(own_exe) ? GetModuleHandleW(nullptr) : GetModuleHandleW(wide(set.exe).c_str());
        if (!module) {
            log("%s: not loaded yet; patching when it loads", set.exe.c_str());
            later.push_back(std::move(set));
            continue;
        }
        if (std::string err = apply_set(set, module, profile); !err.empty() && !may_continue(on_error, err)) return false;
    }
    if (!later.empty()) {
        {
            std::lock_guard<std::mutex> lock(g_pending_lock);
            g_pending = std::move(later);
            g_pending_profile = profile;
        }
        if (!watch_dll_loads() && !may_continue(on_error, "cannot watch for DLL loads; DLL patches will not apply"))
            return false;
    }
    return load_plugins(profile_dir, profile);
}

}  // namespace sf::agent::patches
