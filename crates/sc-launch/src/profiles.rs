//! Where profiles live and how they're named.
//!
//! A profile is a folder under the profiles folder holding a server pack
//! (`pack.json`, `resources/`, `files/`) and the player's `local.json`.

use sc_profile::{Local, Pack, Profile, LOCAL_FILE, PACK_FILE};
use serde::Deserialize;
use std::path::{Path, PathBuf};

/// Folder name under the platform data folder; matches the Tauri identifier so
/// the webview's own storage lands in the same place.
const DATA_FOLDER: &str = "star-forge";
/// Name used before the STAR FORGE rename; its data is moved over on first use.
const OLD_DATA_FOLDER: &str = "sc-launcher";
/// Profile file used before server packs; converted on first load.
const LEGACY_FILE: &str = "profile.toml";

fn platform_data_root() -> Option<PathBuf> {
    #[cfg(windows)]
    return std::env::var_os("APPDATA").map(PathBuf::from);
    #[cfg(target_os = "macos")]
    return std::env::var_os("HOME").map(|h| PathBuf::from(h).join("Library/Application Support"));
    #[cfg(all(unix, not(target_os = "macos")))]
    return std::env::var_os("XDG_DATA_HOME")
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".local/share")));
}

/// Per-user data folder for the launcher (`STAR_FORGE_DATA_DIR` overrides it).
pub fn data_dir() -> Option<PathBuf> {
    if let Some(dir) = crate::env::path(crate::env::DATA_DIR) {
        return Some(dir);
    }
    let root = platform_data_root()?;
    let dir = root.join(DATA_FOLDER);
    let old = root.join(OLD_DATA_FOLDER);
    if !dir.exists() && old.is_dir() {
        // Best effort: if the move fails the launcher just starts with fresh data.
        let _ = std::fs::rename(&old, &dir);
    }
    Some(dir)
}

/// File in the data folder naming a profiles folder chosen by the player.
const PROFILES_LOCATION: &str = "profiles-location";

/// The profiles folder under the data folder, used unless the player picks another.
pub fn standard_profiles_dir() -> Option<PathBuf> {
    data_dir().map(|d| d.join("profiles"))
}

/// The profiles folder: `STAR_FORGE_PROFILES_DIR`, else the one picked in the
/// launcher's setup, else [`standard_profiles_dir`].
pub fn default_profiles_dir() -> Option<PathBuf> {
    if let Some(dir) = crate::env::path(crate::env::PROFILES_DIR) {
        return Some(dir);
    }
    let chosen = data_dir()
        .and_then(|d| std::fs::read_to_string(d.join(PROFILES_LOCATION)).ok())
        .map(|t| t.trim().to_string())
        .filter(|t| !t.is_empty())
        .map(PathBuf::from);
    chosen.or_else(standard_profiles_dir)
}

/// Remembers `dir` as the profiles folder; `None` goes back to the standard one.
pub fn set_profiles_location(dir: Option<&Path>) -> Result<(), String> {
    let data = data_dir().ok_or("cannot work out the launcher data folder")?;
    let file = data.join(PROFILES_LOCATION);
    match dir {
        Some(dir) => {
            std::fs::create_dir_all(&data).map_err(|e| format!("{}: {e}", data.display()))?;
            let text = dir.to_str().ok_or_else(|| format!("{} is not valid Unicode", dir.display()))?;
            write_atomic(&file, text)
        }
        None => match std::fs::remove_file(&file) {
            Err(e) if e.kind() != std::io::ErrorKind::NotFound => Err(format!("{}: {e}", file.display())),
            _ => Ok(()),
        },
    }
}

/// Moves every profile in `from` into `to`. Stops at the first profile that
/// can't move; ones already moved stay moved. Returns how many moved.
pub fn move_profiles(from: &Path, to: &Path) -> Result<usize, String> {
    std::fs::create_dir_all(to).map_err(|e| format!("{}: {e}", to.display()))?;
    let mut moved = 0;
    for entry in list(from) {
        let target = to.join(&entry.name);
        if target.exists() {
            return Err(format!("{} already has a profile called {}", to.display(), entry.name));
        }
        if std::fs::rename(&entry.dir, &target).is_err() {
            // Another drive: copy, then remove the original.
            copy_dir(&entry.dir, &target)?;
            std::fs::remove_dir_all(&entry.dir).map_err(|e| format!("{}: {e}", entry.dir.display()))?;
        }
        moved += 1;
    }
    Ok(moved)
}

fn copy_dir(from: &Path, to: &Path) -> Result<(), String> {
    std::fs::create_dir_all(to).map_err(|e| format!("{}: {e}", to.display()))?;
    for entry in std::fs::read_dir(from).map_err(|e| format!("{}: {e}", from.display()))? {
        let entry = entry.map_err(|e| format!("{}: {e}", from.display()))?;
        let (src, dst) = (entry.path(), to.join(entry.file_name()));
        if entry.file_type().map_err(|e| format!("{}: {e}", src.display()))?.is_dir() {
            copy_dir(&src, &dst)?;
        } else {
            std::fs::copy(&src, &dst).map_err(|e| format!("{} -> {}: {e}", src.display(), dst.display()))?;
        }
    }
    Ok(())
}

/// A profile name is a single folder name: no separators, no dot names.
pub fn valid_name(name: &str) -> bool {
    !name.is_empty()
        && name != "."
        && name != ".."
        && !name.starts_with('.')
        && !name.contains(['/', '\\', ':', '\0'])
}

/// Lower-case letters, digits and dashes, for folder names and pack ids.
pub fn slug(name: &str) -> String {
    let mut out = String::new();
    for c in name.chars() {
        if c.is_ascii_alphanumeric() {
            out.push(c.to_ascii_lowercase());
        } else if !out.ends_with('-') && !out.is_empty() {
            out.push('-');
        }
    }
    let out = out.trim_end_matches('-').chars().take(40).collect::<String>();
    if out.is_empty() { "profile".into() } else { out.trim_end_matches('-').to_string() }
}

/// A free folder name under `root` based on `name`.
pub fn free_name(root: &Path, name: &str) -> String {
    let base = slug(name);
    let mut id = base.clone();
    let mut n = 2;
    while root.join(&id).exists() {
        id = format!("{base}-{n}");
        n += 1;
    }
    id
}

/// Whether `dir` looks like a profile (possibly one still to be converted).
pub fn is_profile(dir: &Path) -> bool {
    dir.join(PACK_FILE).is_file() || dir.join(LEGACY_FILE).is_file()
}

pub struct Entry {
    pub name: String,
    pub dir: PathBuf,
    pub profile: Result<Profile, String>,
    /// The pack as written, when it parses.
    pub pack: Option<Pack>,
}

pub fn read_pack(dir: &Path) -> Result<Pack, String> {
    let path = dir.join(PACK_FILE);
    let text = std::fs::read_to_string(&path).map_err(|e| format!("{}: {e}", path.display()))?;
    Pack::from_json(&text).map_err(|e| e.to_string())
}

pub fn read_local(dir: &Path) -> Result<Local, String> {
    match std::fs::read_to_string(dir.join(LOCAL_FILE)) {
        Ok(text) => Local::from_json(&text).map_err(|e| e.to_string()),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(Local::default()),
        Err(e) => Err(format!("{}: {e}", dir.join(LOCAL_FILE).display())),
    }
}

/// Writes through a temporary file so a crash never leaves half a file.
pub fn write_atomic(path: &Path, text: &str) -> Result<(), String> {
    let name = path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    let tmp = path.with_file_name(format!(".{name}.tmp"));
    std::fs::write(&tmp, text).map_err(|e| format!("{}: {e}", tmp.display()))?;
    std::fs::rename(&tmp, path).map_err(|e| format!("{}: {e}", path.display()))
}

pub fn write_local(dir: &Path, local: &Local) -> Result<(), String> {
    write_atomic(&dir.join(LOCAL_FILE), &local.to_json())
}

pub fn write_pack(dir: &Path, pack: &Pack) -> Result<(), String> {
    pack.validate().map_err(|e| e.to_string())?;
    write_atomic(&dir.join(PACK_FILE), &pack.to_json())
}

pub fn load(dir: &Path) -> Result<Profile, String> {
    migrate_legacy(dir)?;
    Profile::load(dir).map_err(|e| e.to_string())
}

pub fn list(profiles_dir: &Path) -> Vec<Entry> {
    let mut out: Vec<Entry> = std::fs::read_dir(profiles_dir)
        .into_iter()
        .flatten()
        .flatten()
        .filter(|e| valid_name(&e.file_name().to_string_lossy()) && is_profile(&e.path()))
        .map(|e| {
            let dir = e.path();
            let profile = load(&dir);
            Entry {
                name: e.file_name().to_string_lossy().into_owned(),
                pack: read_pack(&dir).ok(),
                profile,
                dir,
            }
        })
        .collect();
    out.sort_by(|a, b| a.name.cmp(&b.name));
    out
}

/// The profile folder holding the pack with this id, if any.
pub fn find_pack(profiles_dir: &Path, id: &str) -> Option<PathBuf> {
    list(profiles_dir)
        .into_iter()
        .find(|e| e.pack.as_ref().is_some_and(|p| p.id == id))
        .map(|e| e.dir)
}

/// `profile.toml` from before server packs.
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Legacy {
    #[serde(default)]
    name: String,
    server: Option<String>,
    #[serde(default = "yes")]
    local_server: bool,
    #[serde(default)]
    args: Vec<String>,
    #[serde(default)]
    trace_files: bool,
    #[serde(default, rename = "patch")]
    patches: Vec<toml::Value>,
}

fn yes() -> bool {
    true
}

/// Converts a `profile.toml` profile to `pack.json` + `local.json`, keeping the
/// old file as `profile.toml.migrated`.
fn migrate_legacy(dir: &Path) -> Result<(), String> {
    let legacy_path = dir.join(LEGACY_FILE);
    if dir.join(PACK_FILE).exists() || !legacy_path.is_file() {
        return Ok(());
    }
    let text = std::fs::read_to_string(&legacy_path).map_err(|e| format!("{}: {e}", legacy_path.display()))?;
    let old: Legacy = toml::from_str(&text).map_err(|e| format!("{LEGACY_FILE}: {e}"))?;
    if !old.patches.is_empty() {
        return Err(format!(
            "{LEGACY_FILE} has [[patch]] entries, which this launcher no longer reads: move them to a \
             patches/game.exe.patchset.toml (see templates/pack/patches/README.md) and remove them from {LEGACY_FILE}"
        ));
    }
    let folder = dir.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    let mut pack = Pack {
        format: sc_profile::PACK_FORMAT,
        id: format!("local.{}", slug(&folder)),
        name: if old.name.trim().is_empty() { folder } else { old.name },
        version: "1.0.0".into(),
        ..Pack::default()
    };
    pack.server.address = old.server;
    pack.server.local = old.local_server;
    pack.game.args = old.args;
    write_pack(dir, &pack)?;
    if old.trace_files {
        write_local(dir, &Local { trace_files: true, ..Local::default() })?;
    }
    std::fs::rename(&legacy_path, dir.join(format!("{LEGACY_FILE}.migrated"))).map_err(|e| format!("{}: {e}", legacy_path.display()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn moves_profiles() {
        let base = std::env::temp_dir().join(format!("sf-move-{}", std::process::id()));
        let (from, to) = (base.join("from"), base.join("to"));
        std::fs::create_dir_all(from.join("one/files")).unwrap();
        std::fs::write(from.join("one").join(PACK_FILE), "{}").unwrap();
        std::fs::write(from.join("one/files/a.cfg"), "a").unwrap();
        std::fs::create_dir_all(from.join("not-a-profile")).unwrap();
        assert_eq!(move_profiles(&from, &to).unwrap(), 1);
        assert_eq!(std::fs::read_to_string(to.join("one/files/a.cfg")).unwrap(), "a");
        assert!(!from.join("one").exists());
        assert!(from.join("not-a-profile").exists(), "only profiles move");
        std::fs::create_dir_all(from.join("one")).unwrap();
        std::fs::write(from.join("one").join(PACK_FILE), "{}").unwrap();
        assert!(move_profiles(&from, &to).is_err(), "never overwrites a profile");
        std::fs::remove_dir_all(&base).unwrap();
    }

    #[test]
    fn names() {
        assert!(valid_name("starfury"));
        assert!(valid_name("local dev"));
        assert!(!valid_name(".."));
        assert!(!valid_name(".staging"));
        assert!(!valid_name("a/b"));
        assert!(!valid_name(r"a\b"));
        assert!(!valid_name("C:x"));
    }

    #[test]
    fn slugs() {
        assert_eq!(slug("Starfury community server"), "starfury-community-server");
        assert_eq!(slug("  Local (dev) "), "local-dev");
        assert_eq!(slug("!!!"), "profile");
    }

    #[test]
    fn migrates_profile_toml() {
        let dir = std::env::temp_dir().join(format!("sf-migrate-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(
            dir.join(LEGACY_FILE),
            "name = \"Old\"\nserver = \"a.b:1\"\ntrace_files = true\n[[patch]]\noffset = \"0x10\"\noriginal = \"90\"\npatched = \"91\"\n",
        )
        .unwrap();
        assert!(load(&dir).unwrap_err().contains("patchset"), "old inline patches are not dropped silently");
        std::fs::write(dir.join(LEGACY_FILE), "name = \"Old\"\nserver = \"a.b:1\"\ntrace_files = true\n").unwrap();
        let p = load(&dir).unwrap();
        assert_eq!(p.name, "Old");
        assert_eq!(p.server.as_deref(), Some("a.b:1"));
        assert!(p.local_server, "old default kept");
        assert!(p.trace_files);
        assert!(dir.join("profile.toml.migrated").is_file());
        assert!(read_pack(&dir).unwrap().id.starts_with("local.sf-migrate-"));
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
