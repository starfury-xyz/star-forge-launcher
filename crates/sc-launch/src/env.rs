//! Machine-specific overrides, read from `STAR_FORGE_*` environment variables.
//! Explicit settings (command-line flags, launcher preferences) win over these,
//! and these win over auto-detection. See `.env.example` at the repo root.

use std::ffi::OsString;
use std::path::PathBuf;
use std::process::Command;

/// Star Conflict install folder (holding win64\ and data\).
pub const GAME_DIR: &str = "STAR_FORGE_GAME_DIR";
/// Folder holding sc-inject.exe and sc_agent.dll.
pub const TOOLS_DIR: &str = "STAR_FORGE_TOOLS_DIR";
/// Proton build folder (Linux).
pub const PROTON: &str = "STAR_FORGE_PROTON";
/// Launcher data folder (preferences, profiles, launch log).
pub const DATA_DIR: &str = "STAR_FORGE_DATA_DIR";
/// Profiles folder, if not the one chosen in the launcher's setup.
pub const PROFILES_DIR: &str = "STAR_FORGE_PROFILES_DIR";

/// A path variable, ignoring unset and empty values.
pub fn path(name: &str) -> Option<PathBuf> {
    std::env::var_os(name).filter(|v| !v.is_empty()).map(PathBuf::from)
}

/// Development builds read `<repo>/.env` so overrides apply however the binary
/// is started (terminal, desktop entry). Variables already set are kept.
/// Release builds never read a .env file. Call before starting any threads.
pub fn load_dotenv() {
    if cfg!(debug_assertions) {
        let file = concat!(env!("CARGO_MANIFEST_DIR"), "/../../.env");
        if let Ok(text) = std::fs::read_to_string(file) {
            for (key, value) in parse_dotenv(&text) {
                if std::env::var_os(&key).is_none() {
                    std::env::set_var(key, value);
                }
            }
        }
    }
}

/// `KEY=value` lines; blank lines and `#` comments are skipped, an optional
/// `export ` prefix and matching surrounding quotes are dropped.
fn parse_dotenv(text: &str) -> Vec<(String, String)> {
    text.lines()
        .map(str::trim)
        .filter(|l| !l.is_empty() && !l.starts_with('#'))
        .filter_map(|l| {
            let (key, value) = l.strip_prefix("export ").unwrap_or(l).split_once('=')?;
            let value = value.trim();
            let value = [('"', '"'), ('\'', '\'')]
                .iter()
                .find_map(|&(a, b)| value.strip_prefix(a)?.strip_suffix(b))
                .unwrap_or(value);
            Some((key.trim().to_string(), value.to_string()))
        })
        .filter(|(k, _)| !k.is_empty())
        .collect()
}

/// Gives a program we start the host's environment rather than the bundle's.
///
/// An AppImage's AppRun points library, Python, Perl and GTK search paths into
/// the image (`LD_LIBRARY_PATH`, `PYTHONHOME`, `GTK_PATH`, ...). Inherited by
/// Proton, its Python script cannot find its standard library and the game
/// never starts; inherited by `xdg-open`, the file manager fails. Outside an
/// AppImage this does nothing.
pub fn use_host_env(cmd: &mut Command) {
    if std::env::var_os("APPIMAGE").is_none() {
        return;
    }
    let Some(appdir) = std::env::var_os("APPDIR") else { return };
    for (key, value) in host_env_changes(std::env::vars_os(), &appdir) {
        match value {
            Some(v) => cmd.env(key, v),
            None => cmd.env_remove(key),
        };
    }
}

/// Variables to change so nothing points into `appdir`: path lists lose their
/// bundle entries (`None` when nothing is left), single values are removed.
/// The theme AppRun forces on GTK goes too.
fn host_env_changes(vars: impl Iterator<Item = (OsString, OsString)>, appdir: &std::ffi::OsStr) -> Vec<(OsString, Option<String>)> {
    let appdir = appdir.to_string_lossy();
    let appdir = appdir.trim_end_matches('/');
    let mut out = Vec::new();
    if appdir.is_empty() {
        return out;
    }
    for (key, value) in vars {
        let value = value.to_string_lossy();
        if key == "GTK_THEME" || key == "PYTHONDONTWRITEBYTECODE" {
            out.push((key, None));
        } else if value.contains(appdir) {
            let kept: Vec<&str> = value.split(':').filter(|p| !p.is_empty() && !p.contains(appdir)).collect();
            out.push((key, (!kept.is_empty()).then(|| kept.join(":"))));
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn strips_appimage_paths() {
        let vars = [
            ("LD_LIBRARY_PATH", "/tmp/.mount_x/usr/lib/:/tmp/.mount_x/lib/:/opt/host/lib"),
            ("PYTHONHOME", "/tmp/.mount_x/usr/"),
            ("XDG_DATA_DIRS", "/tmp/.mount_x/usr/share:/usr/share:/usr/local/share"),
            ("GTK_THEME", "Adwaita:dark"),
            ("HOME", "/home/user"),
        ]
        .map(|(k, v)| (OsString::from(k), OsString::from(v)));
        let changes = host_env_changes(vars.into_iter(), std::ffi::OsStr::new("/tmp/.mount_x/"));
        assert_eq!(
            changes,
            vec![
                ("LD_LIBRARY_PATH".into(), Some("/opt/host/lib".into())),
                ("PYTHONHOME".into(), None),
                ("XDG_DATA_DIRS".into(), Some("/usr/share:/usr/local/share".into())),
                ("GTK_THEME".into(), None),
            ]
        );
    }

    #[test]
    fn parses_dotenv_lines() {
        let text = "# comment\n\nSTAR_FORGE_GAME_DIR=\"/games/star conflict\"\nexport STAR_FORGE_PROTON='/p'\nEMPTY=\nnot a line\n";
        assert_eq!(
            parse_dotenv(text),
            vec![
                ("STAR_FORGE_GAME_DIR".into(), "/games/star conflict".into()),
                ("STAR_FORGE_PROTON".into(), "/p".into()),
                ("EMPTY".into(), String::new()),
            ]
        );
    }
}
