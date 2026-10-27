//! Native (Linux/Windows) side of the launcher: finds the game and Proton,
//! manages profiles, and starts sc-inject.exe. Kept as a library so a GUI can
//! wrap it later.

#[cfg(unix)]
pub mod compat;
pub mod env;
pub mod launch;
pub mod packs;
pub mod patching;
pub mod profiles;
pub mod remote;
pub mod signing;
pub mod state;
pub mod steam;
pub mod trust;

/// True if `part` is one path component that means the same file on Linux and
/// Windows: no `.`/`..`, no characters Windows forbids, no trailing dot or
/// space (Windows drops them, so `a.cfg.` would write `a.cfg`), and no device
/// name such as `con` or `nul.lua`.
pub(crate) fn plain_part(part: &str) -> bool {
    const DEVICES: [&str; 4] = ["con", "prn", "aux", "nul"];
    let stem = part.split('.').next().unwrap_or("").trim_end().to_ascii_lowercase();
    let device = DEVICES.contains(&stem.as_str())
        || (stem.len() == 4
            && (stem.starts_with("com") || stem.starts_with("lpt"))
            && stem.as_bytes()[3].is_ascii_digit());
    !part.is_empty()
        && part != "."
        && part != ".."
        && !part.ends_with(['.', ' '])
        && !part.chars().any(|c| c.is_control() || "<>:\"|?*".contains(c))
        && !device
}

/// One launcher data folder for every test in the process (tests run in
/// parallel threads and `STAR_FORGE_DATA_DIR` is process-wide).
#[cfg(test)]
pub(crate) fn test_data_dir() -> &'static std::path::Path {
    static DIR: std::sync::OnceLock<std::path::PathBuf> = std::sync::OnceLock::new();
    DIR.get_or_init(|| {
        let dir = std::env::temp_dir().join(format!("sf-test-data-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::env::set_var(env::DATA_DIR, &dir);
        dir
    })
}

#[cfg(test)]
mod tests {
    use super::plain_part;

    #[test]
    fn plain_parts() {
        for ok in ["data", "a.lua", "x.y.z", ".gitkeep", "console.lua", "com.lua", "comx", "lpt10"] {
            assert!(plain_part(ok), "{ok}");
        }
        for bad in ["", ".", "..", "a.", "a ", "C:", "a*b", "a\0", "con", "CON.lua", "nul .txt", "com1", "LPT9.cfg", "aux"] {
            assert!(!plain_part(bad), "{bad:?}");
        }
    }
}
