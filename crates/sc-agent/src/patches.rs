//! The `inject` patch method: applies the core patches the pack asks for, then
//! the pack's own patchsets, to the game's memory and loads its native
//! plugins, all before the game's own code runs.
//!
//! A patchset for game.exe is applied at once (the process is still
//! suspended). One for a DLL that isn't loaded yet is applied the moment the
//! loader maps it, before its DllMain runs.

use crate::agent::log;
use sc_patch::{check, Patch, Patchset, State, Target, WriteKind};
use sc_profile::{OnError, Profile, PATCHES_DIR};
use std::collections::HashMap;
use std::ffi::{c_void, OsStr};
use std::os::windows::ffi::OsStrExt;
use std::path::Path;
use std::sync::Mutex;
use windows_sys::Win32::Foundation::HMODULE;
use windows_sys::Win32::System::Diagnostics::Debug::FlushInstructionCache;
use windows_sys::Win32::System::LibraryLoader::{GetModuleFileNameW, GetModuleHandleW, GetProcAddress, LoadLibraryW};
use windows_sys::Win32::System::Memory::{VirtualProtect, PAGE_EXECUTE_READWRITE};
use windows_sys::Win32::System::Threading::{GetCurrentProcess, TerminateProcess};

fn wide(s: &str) -> Vec<u16> {
    OsStr::new(s).encode_wide().chain(Some(0)).collect()
}

/// A loaded module, addressed the way its patchset is.
struct Memory {
    base: *mut u8,
    image_base: u64,
    size: u64,
}

impl Memory {
    fn at(&self, va: u64, n: usize) -> Option<*mut u8> {
        let rva = va.checked_sub(self.image_base)?;
        (rva + n as u64 <= self.size).then(|| unsafe { self.base.add(rva as usize) })
    }
}

impl Target for Memory {
    fn read(&mut self, va: u64, out: &mut [u8]) -> bool {
        let Some(p) = self.at(va, out.len()) else { return false };
        unsafe { std::ptr::copy_nonoverlapping(p, out.as_mut_ptr(), out.len()) };
        true
    }

    fn write(&mut self, va: u64, data: &[u8]) -> bool {
        let Some(p) = self.at(va, data.len()) else { return false };
        unsafe {
            let mut old = 0u32;
            if VirtualProtect(p.cast(), data.len(), PAGE_EXECUTE_READWRITE, &mut old) == 0 {
                return false;
            }
            std::ptr::copy_nonoverlapping(data.as_ptr(), p, data.len());
            VirtualProtect(p.cast(), data.len(), old, &mut old);
            FlushInstructionCache(GetCurrentProcess(), p.cast(), data.len());
        }
        true
    }
}

/// Size of a mapped image, from its PE optional header.
unsafe fn image_size(base: *const u8) -> u64 {
    let nt = base.add(*(base.add(0x3c) as *const u32) as usize);
    *(nt.add(0x50) as *const u32) as u64
}

fn module_path(module: HMODULE) -> Option<String> {
    let mut buf = vec![0u16; 1024];
    let n = unsafe { GetModuleFileNameW(module, buf.as_mut_ptr(), buf.len() as u32) } as usize;
    (n > 0 && n < buf.len()).then(|| String::from_utf16_lossy(&buf[..n]))
}

/// The player's on/off choices for one module (keys compared case-insensitively).
fn choices<'a>(profile: &'a Profile, exe: &str) -> Option<&'a HashMap<String, bool>> {
    profile.patch_choices.iter().find(|(k, _)| k.eq_ignore_ascii_case(exe)).map(|(_, v)| v)
}

/// Applies one patchset to a loaded module. Every enabled patch is verified
/// before anything is written, so a set never half-applies. `Err` is a
/// failure the pack's `on_error` decides about.
fn apply_set(set: &Patchset, module: HMODULE, profile: &Profile) -> Result<(), String> {
    let path = module_path(module).ok_or("cannot find the module file")?;
    let file = std::fs::read(&path).map_err(|e| format!("{path}: {e}"))?;
    let sha = sc_patch::sha1_hex(&file);
    if !set.accepts(&sha) {
        return Err(format!("{} is a different build (sha1 {sha}, the patches need {})", set.exe, set.expected_sha1()));
    }
    let base = module as *mut u8;
    let mut mem = Memory { base, image_base: set.image_base, size: unsafe { image_size(base) } };
    let (on, warnings) = set.enabled(choices(profile, &set.exe));
    for w in warnings {
        log(&format!("warning: {w}"));
    }
    if let Some(o) = sc_patch::overlaps(&on).first() {
        return Err(format!("enabled patches overlap: {o}"));
    }
    let mut go: Vec<&Patch> = Vec::new();
    let mut failed = Vec::new();
    for p in on {
        let why = match check(&mut mem, p) {
            State::Pristine => p
                .writes
                .iter()
                .find_map(|w| match &w.kind {
                    // x64 midhook thunks don't exist yet; hooks go in plugins for now.
                    WriteKind::Midhook(name) => Some(format!("midhook {name:?} is not supported for the client yet")),
                    WriteKind::Bytes(_) => None,
                }),
            State::Applied => {
                log(&format!("{}: {} already applied", set.exe, p.id));
                continue;
            }
            State::Mismatch(why) => Some(why),
        };
        match why {
            None => go.push(p),
            Some(why) if p.optional => log(&format!("{}: skipping optional patch {}: {why}", set.exe, p.id)),
            Some(why) => failed.push(format!("{}: {why}", p.id)),
        }
    }
    if !failed.is_empty() {
        return Err(format!("{}: {}", set.exe, failed.join("; ")));
    }
    for p in &go {
        sc_patch::apply(&mut mem, p, None).map_err(|e| format!("{}: {}: {e}", set.exe, p.id))?;
        let title = if p.title.is_empty() { String::new() } else { format!(" ({})", p.title) };
        log(&format!("{}: applied {}{title}, {} write(s)", set.exe, p.id, p.writes.len()));
    }
    log(&format!("{}: {} patch(es) applied", set.exe, go.len()));
    Ok(())
}

/// Patchsets waiting for their DLL, with what the deferred callback needs.
struct Pending {
    sets: Vec<Patchset>,
    profile: Profile,
}

// SAFETY: only touched under the mutex, from the loader's notification thread.
unsafe impl Send for Pending {}

static PENDING: Mutex<Option<Pending>> = Mutex::new(None);

/// Logs a failure and, unless the pack says to continue, reports that the
/// game must not run.
fn failed(on_error: OnError, msg: &str) -> bool {
    match on_error {
        OnError::Exit => {
            log(&format!("ERROR: {msg}"));
            log("not starting the game (patches.on_error is \"exit\" in pack.json)");
            false
        }
        OnError::Continue => {
            log(&format!("ERROR: {msg} (continuing: patches.on_error is \"continue\")"));
            true
        }
    }
}

/// Applies everything the profile asks for. Returns false when the game
/// must not start.
pub fn apply_all(profile_dir: &str, profile: &Profile) -> bool {
    let on_error = profile.patches.on_error;
    let mut later = Vec::new();
    let core: Vec<Result<Patchset, String>> = match sc_patch::core::sets(&profile.patches.core) {
        Ok(sets) => sets.into_iter().map(Ok).collect(),
        Err(e) => vec![Err(e)],
    };
    let pack_sets = sc_profile::patchset_files(Path::new(profile_dir)).into_iter().map(|file| {
        let name = file.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
        std::fs::read_to_string(&file)
            .map_err(|e| e.to_string())
            .and_then(|t| Patchset::parse(&t, &name).map_err(|e| e.to_string()))
    });
    for set in core.into_iter().chain(pack_sets) {
        let set = match set {
            Ok(set) => set,
            Err(e) => {
                if !failed(on_error, &e) {
                    return false;
                }
                continue;
            }
        };
        let own = module_path(std::ptr::null_mut())
            .and_then(|p| p.rsplit(['\\', '/']).next().map(str::to_string))
            .is_some_and(|exe| set.targets(&exe));
        let module = unsafe {
            if own {
                GetModuleHandleW(std::ptr::null())
            } else {
                GetModuleHandleW(wide(&set.exe).as_ptr())
            }
        };
        if module.is_null() {
            log(&format!("{}: not loaded yet; patching when it loads", set.exe));
            later.push(set);
            continue;
        }
        if let Err(e) = apply_set(&set, module, profile) {
            if !failed(on_error, &e) {
                return false;
            }
        }
    }
    if !later.is_empty() {
        *PENDING.lock().unwrap() = Some(Pending { sets: later, profile: profile.clone() });
        if !watch_dll_loads() && !failed(on_error, "cannot watch for DLL loads; DLL patches will not apply") {
            return false;
        }
    }
    load_plugins(profile_dir, profile)
}

#[repr(C)]
struct UnicodeString {
    length: u16,
    maximum_length: u16,
    buffer: *const u16,
}

#[repr(C)]
struct DllLoaded {
    flags: u32,
    full_name: *const UnicodeString,
    base_name: *const UnicodeString,
    base: *mut c_void,
    size: u32,
}

const LDR_DLL_NOTIFICATION_REASON_LOADED: u32 = 1;

type Notify = unsafe extern "system" fn(u32, *const DllLoaded, *mut c_void);
type Register = unsafe extern "system" fn(u32, Notify, *mut c_void, *mut *mut c_void) -> i32;

/// Called by the loader after it maps a DLL and before the DLL's DllMain.
unsafe extern "system" fn on_dll_load(reason: u32, data: *const DllLoaded, _: *mut c_void) {
    if reason != LDR_DLL_NOTIFICATION_REASON_LOADED || data.is_null() || (*data).base_name.is_null() {
        return;
    }
    let name = &*(*data).base_name;
    let name = String::from_utf16_lossy(std::slice::from_raw_parts(name.buffer, name.length as usize / 2));
    let _ = std::panic::catch_unwind(|| {
        let mut guard = PENDING.lock().unwrap();
        let Some(pending) = guard.as_mut() else { return };
        let Some(i) = pending.sets.iter().position(|s| s.targets(&name)) else { return };
        let set = pending.sets.remove(i);
        if let Err(e) = apply_set(&set, (*data).base as HMODULE, &pending.profile) {
            if !failed(pending.profile.patches.on_error, &e) {
                // Too late to refuse cleanly: the game is already running.
                TerminateProcess(GetCurrentProcess(), 1);
            }
        }
    });
}

fn watch_dll_loads() -> bool {
    unsafe {
        let ntdll = GetModuleHandleW(wide("ntdll.dll").as_ptr());
        let Some(register) = GetProcAddress(ntdll, c"LdrRegisterDllNotification".as_ptr().cast()) else {
            return false;
        };
        let register: Register = std::mem::transmute(register);
        let mut cookie = std::ptr::null_mut();
        register(0, on_dll_load, std::ptr::null_mut(), &mut cookie) == 0
    }
}

/// What a plugin's `starforge_plugin_init` receives. Append-only; `version`
/// goes up when fields are added.
#[repr(C)]
pub struct PluginApi {
    pub version: u32,
    /// Profile folder, NUL-terminated UTF-16; valid only during the call.
    pub profile_dir: *const u16,
    /// Writes a UTF-8 line to agent.log.
    pub log: extern "C" fn(*const u8, usize),
}

extern "C" fn plugin_log(text: *const u8, len: usize) {
    if text.is_null() {
        return;
    }
    let bytes = unsafe { std::slice::from_raw_parts(text, len) };
    log(&format!("plugin: {}", String::from_utf8_lossy(bytes)));
}

type PluginInit = unsafe extern "C" fn(*const PluginApi) -> i32;

/// Loads the pack's native plugins. A plugin may export
/// `int starforge_plugin_init(const PluginApi*)`; non-zero means it failed.
fn load_plugins(profile_dir: &str, profile: &Profile) -> bool {
    let on_error = profile.patches.on_error;
    let dir = wide(profile_dir);
    for rel in &profile.patches.plugins {
        let Some(inner) = sc_profile::patches_rel(rel) else { continue };
        let path = format!("{profile_dir}\\{PATCHES_DIR}\\{}", inner.replace('/', "\\"));
        let module = unsafe { LoadLibraryW(wide(&path).as_ptr()) };
        if module.is_null() {
            if !failed(on_error, &format!("plugin {rel}: could not be loaded")) {
                return false;
            }
            continue;
        }
        let init = unsafe { GetProcAddress(module, c"starforge_plugin_init".as_ptr().cast()) };
        let code = match init {
            Some(f) => {
                let api = PluginApi { version: 1, profile_dir: dir.as_ptr(), log: plugin_log };
                let f: PluginInit = unsafe { std::mem::transmute(f) };
                unsafe { f(&api) }
            }
            None => 0,
        };
        if code != 0 {
            if !failed(on_error, &format!("plugin {rel}: init returned {code}")) {
                return false;
            }
            continue;
        }
        log(&format!("plugin {rel}: loaded"));
    }
    true
}
