//! Starts game.exe suspended, injects sc_agent.dll, then resumes it.
//!
//! Normally started by sc-launch. Usage (Windows, or inside Proton/Wine):
//!   sc-inject.exe --game <path\to\win64\game.exe> --profile <profile dir> [--dll <sc_agent.dll>] [-- game args]

// A windowed program, not a console one: started from the launcher, a
// console program gets its own terminal window (Proton opens one, and the
// game's taskbar entry groups under it). Everything it says goes to the
// profile's inject.log; a parent that passes its own stdout still gets it.
#![cfg_attr(windows, windows_subsystem = "windows")]

#[cfg(not(windows))]
fn main() {
    eprintln!("sc-inject is a Windows program; start it with sc-launch");
    std::process::exit(2);
}

#[cfg(windows)]
fn main() {
    match win::run() {
        Ok(code) => std::process::exit(code as i32),
        Err(e) => {
            win::say(&format!("error: {e}"));
            std::process::exit(1);
        }
    }
}

#[cfg(windows)]
mod win {
    use sc_profile::{normalize, windows_path_from_arg, Profile, FILES_DIR, INJECT_LOG, PROFILE_ENV};
    use std::io::Write;
    use std::sync::{Mutex, OnceLock};

    static LOG: OnceLock<Mutex<std::fs::File>> = OnceLock::new();

    /// Prints a line and appends it to the profile's inject.log (Proton
    /// swallows this program's console output).
    pub fn say(msg: &str) {
        println!("sc-inject: {msg}");
        if let Some(Ok(mut f)) = LOG.get().map(Mutex::lock) {
            let _ = writeln!(f, "{msg}");
        }
    }
    use std::ffi::OsStr;
    use std::os::windows::ffi::OsStrExt;
    use std::path::{Path, PathBuf};
    use std::ptr::{null, null_mut};
    use windows_sys::Win32::Foundation::{CloseHandle, GetLastError, HANDLE, WAIT_OBJECT_0};
    use windows_sys::Win32::System::Diagnostics::Debug::WriteProcessMemory;
    use windows_sys::Win32::System::JobObjects::{
        AssignProcessToJobObject, CreateJobObjectW, JobObjectBasicAccountingInformation, QueryInformationJobObject,
        TerminateJobObject, JOBOBJECT_BASIC_ACCOUNTING_INFORMATION,
    };
    use windows_sys::Win32::System::LibraryLoader::{GetModuleHandleW, GetProcAddress};
    use windows_sys::Win32::System::Memory::{
        VirtualAllocEx, VirtualFreeEx, MEM_COMMIT, MEM_RELEASE, MEM_RESERVE, PAGE_READWRITE,
    };
    use windows_sys::Win32::System::Threading::{
        CreateProcessW, CreateRemoteThread, GetExitCodeProcess, GetExitCodeThread, ResumeThread,
        TerminateProcess, WaitForSingleObject, CREATE_SUSPENDED, INFINITE, PROCESS_INFORMATION,
        STARTUPINFOW,
    };

    struct Args {
        game: String,
        profile: String,
        dll: Option<String>,
        game_args: Option<Vec<String>>,
    }

    fn parse_args() -> Result<Args, String> {
        let mut game = None;
        let mut profile = None;
        let mut dll = None;
        let mut game_args = None;
        let mut it = std::env::args().skip(1);
        while let Some(a) = it.next() {
            match a.as_str() {
                "--game" => game = it.next(),
                "--profile" => profile = it.next(),
                "--dll" => dll = it.next(),
                "--" => {
                    game_args = Some(it.by_ref().collect());
                }
                _ => return Err(format!("unknown argument {a:?}")),
            }
        }
        Ok(Args {
            game: windows_path_from_arg(&game.ok_or("--game is required")?),
            profile: windows_path_from_arg(&profile.ok_or("--profile is required")?),
            dll: dll.map(|d| windows_path_from_arg(&d)),
            game_args,
        })
    }

    fn wide(s: &str) -> Vec<u16> {
        OsStr::new(s).encode_wide().chain(Some(0)).collect()
    }

    /// Quotes one argument the way CommandLineToArgvW expects.
    fn quote(arg: &str) -> String {
        if !arg.is_empty() && !arg.contains([' ', '\t', '"']) {
            return arg.to_string();
        }
        let mut out = String::from('"');
        let mut slashes = 0;
        for c in arg.chars() {
            match c {
                '\\' => slashes += 1,
                '"' => {
                    out.extend(std::iter::repeat('\\').take(slashes * 2 + 1));
                    out.push('"');
                    slashes = 0;
                    continue;
                }
                _ => {}
            }
            if c != '\\' {
                out.extend(std::iter::repeat('\\').take(slashes));
                slashes = 0;
                out.push(c);
            }
        }
        out.extend(std::iter::repeat('\\').take(slashes * 2));
        out.push('"');
        out
    }

    fn last_error(what: &str) -> String {
        format!("{what} failed (error {})", unsafe { GetLastError() })
    }

    pub fn run() -> Result<u32, String> {
        let args = parse_args()?;
        let profile_dir = normalize(&args.profile);
        if let Ok(f) = std::fs::File::create(Path::new(&profile_dir).join(INJECT_LOG)) {
            let _ = LOG.set(Mutex::new(f));
        }
        let profile = Profile::load(Path::new(&profile_dir)).map_err(|e| e.to_string())?;
        // Fail before launching, not in-game.
        for file in sc_profile::patchset_files(Path::new(&profile_dir)) {
            let text = std::fs::read_to_string(&file).map_err(|e| format!("{}: {e}", file.display()))?;
            sc_patch::Patchset::parse(&text, &file.to_string_lossy()).map_err(|e| e.to_string())?;
        }

        if let Some(cfg) = profile.address_cfg().map_err(|e| e.to_string())? {
            let dir = Path::new(&profile_dir).join(FILES_DIR).join("data");
            std::fs::create_dir_all(&dir).map_err(|e| format!("{}: {e}", dir.display()))?;
            std::fs::write(dir.join("address.cfg"), cfg).map_err(|e| format!("address.cfg: {e}"))?;
        }

        let dll = match args.dll {
            Some(d) => PathBuf::from(d),
            None => std::env::current_exe()
                .map_err(|e| e.to_string())?
                .with_file_name("sc_agent.dll"),
        };
        if !dll.is_file() {
            return Err(format!("agent not found at {}", dll.display()));
        }
        let dll = normalize(&dll.to_string_lossy());

        let game = normalize(&args.game);
        let game_dir = Path::new(&game)
            .parent()
            .ok_or("bad --game path")?
            .to_string_lossy()
            .into_owned();
        let game_args = args.game_args.unwrap_or_else(|| profile.args());
        let mut cmdline = quote(&game);
        for a in &game_args {
            cmdline.push(' ');
            cmdline.push_str(&quote(a));
        }

        // The child inherits our environment; the agent reads the profile from it.
        std::env::set_var(PROFILE_ENV, &profile_dir);

        say(&format!("profile {profile_dir}"));
        say(&format!("starting {cmdline}"));

        let game_w = wide(&game);
        let dir_w = wide(&game_dir);
        let mut cmd_w = wide(&cmdline);
        let mut si: STARTUPINFOW = unsafe { std::mem::zeroed() };
        si.cb = std::mem::size_of::<STARTUPINFOW>() as u32;
        let mut pi: PROCESS_INFORMATION = unsafe { std::mem::zeroed() };
        let ok = unsafe {
            CreateProcessW(
                game_w.as_ptr(),
                cmd_w.as_mut_ptr(),
                null(),
                null(),
                0,
                CREATE_SUSPENDED,
                null(),
                dir_w.as_ptr(),
                &si,
                &mut pi,
            )
        };
        if ok == 0 {
            return Err(last_error("CreateProcessW"));
        }

        // Everything the game starts (the bundled local server, for one) joins
        // this job, so it can be stopped with the game; otherwise leftovers
        // keep the Proton session, and the launcher, waiting.
        let job = unsafe { CreateJobObjectW(null(), null()) };
        if job.is_null() || unsafe { AssignProcessToJobObject(job, pi.hProcess) } == 0 {
            say(&format!("{}; processes the game starts will outlive it", last_error("job object setup")));
        }

        if let Err(e) = inject(pi.hProcess, &dll) {
            unsafe {
                TerminateProcess(pi.hProcess, 1);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
            return Err(e);
        }
        say("agent injected, resuming game");

        unsafe {
            ResumeThread(pi.hThread);
            CloseHandle(pi.hThread);
            // Stay alive until the game exits so Proton keeps the session open.
            WaitForSingleObject(pi.hProcess, INFINITE);
            let mut code = 0u32;
            GetExitCodeProcess(pi.hProcess, &mut code);
            CloseHandle(pi.hProcess);
            say(&format!("game exited with {code}"));
            if !job.is_null() {
                let mut info: JOBOBJECT_BASIC_ACCOUNTING_INFORMATION = std::mem::zeroed();
                let ok = QueryInformationJobObject(
                    job,
                    JobObjectBasicAccountingInformation,
                    (&mut info as *mut JOBOBJECT_BASIC_ACCOUNTING_INFORMATION).cast(),
                    std::mem::size_of_val(&info) as u32,
                    null_mut(),
                );
                if ok != 0 && info.ActiveProcesses > 0 {
                    say(&format!("stopping {} process(es) the game left running", info.ActiveProcesses));
                }
                TerminateJobObject(job, 0);
                CloseHandle(job);
            }
            Ok(code)
        }
    }

    /// Classic LoadLibraryW injection into the suspended process.
    fn inject(process: HANDLE, dll: &str) -> Result<(), String> {
        let path = wide(dll);
        let bytes = path.len() * 2;
        unsafe {
            let k32 = GetModuleHandleW(wide("kernel32.dll").as_ptr());
            let load = GetProcAddress(k32, c"LoadLibraryW".as_ptr().cast())
                .ok_or_else(|| last_error("GetProcAddress(LoadLibraryW)"))?;

            let mem = VirtualAllocEx(process, null(), bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if mem.is_null() {
                return Err(last_error("VirtualAllocEx"));
            }
            let result = (|| {
                if WriteProcessMemory(process, mem, path.as_ptr().cast(), bytes, null_mut()) == 0 {
                    return Err(last_error("WriteProcessMemory"));
                }
                let thread = CreateRemoteThread(
                    process,
                    null(),
                    0,
                    Some(std::mem::transmute::<
                        unsafe extern "system" fn() -> isize,
                        unsafe extern "system" fn(*mut core::ffi::c_void) -> u32,
                    >(load)),
                    mem,
                    0,
                    null_mut(),
                );
                if thread.is_null() {
                    return Err(last_error("CreateRemoteThread"));
                }
                let waited = WaitForSingleObject(thread, 30_000);
                let mut module = 0u32;
                GetExitCodeThread(thread, &mut module);
                CloseHandle(thread);
                if waited != WAIT_OBJECT_0 {
                    return Err("timed out waiting for LoadLibraryW in the game".into());
                }
                // The exit code is the low 32 bits of the module handle; 0 means it failed.
                if module == 0 {
                    return Err(format!(
                        "the agent ({dll}) did not load; if the pack has patches, they may not match this game build (see agent.log in the profile folder)"
                    ));
                }
                Ok(())
            })();
            VirtualFreeEx(process, mem, 0, MEM_RELEASE);
            result
        }
    }

    #[cfg(test)]
    mod tests {
        use super::quote;

        #[test]
        fn quoting() {
            assert_eq!(quote("-launchLocalServer"), "-launchLocalServer");
            assert_eq!(quote(r"Z:\star conflict\win64\game.exe"), r#""Z:\star conflict\win64\game.exe""#);
            assert_eq!(quote(r"a\"), r"a\");
            assert_eq!(quote(r"a b\"), r#""a b\\""#);
            assert_eq!(quote(r#"say "hi""#), r#""say \"hi\"""#);
        }
    }
}
