//! The game's per-user data, per profile.
//!
//! The agent gives each profile its own `%LOCALAPPDATA%` (`<profile>/state/
//! AppData/Local`), so one server's store cache, notes and local database
//! never show up in another's sessions. The player's graphics settings and key
//! bindings ([`SHARED_STATE_FILES`]) should still follow them everywhere, so
//! they are synced with the game's normal folder before and after each
//! session, newest copy wins.

use sc_profile::{Profile, SHARED_STATE_FILES, STATE_DIR};
use std::path::{Path, PathBuf};

/// The game's normal `%LOCALAPPDATA%`: inside the Proton prefix on Linux.
pub fn shared_local_appdata(setup: &crate::launch::Setup) -> Option<PathBuf> {
    #[cfg(unix)]
    return Some(setup.proton.compat_data.join("pfx/drive_c/users/steamuser/AppData/Local"));
    #[cfg(windows)]
    {
        let _ = setup;
        std::env::var_os("LOCALAPPDATA").map(PathBuf::from)
    }
}

/// The profile's own `%LOCALAPPDATA%`.
pub fn profile_local_appdata(profile_dir: &Path) -> PathBuf {
    profile_dir.join(STATE_DIR).join("AppData").join("Local")
}

/// Copies whichever of `a` and `b` is newer over the other, if they differ.
fn sync_file(a: &Path, b: &Path) -> Result<(), String> {
    let modified = |p: &Path| std::fs::metadata(p).and_then(|m| m.modified()).ok();
    let (from, to) = match (modified(a), modified(b)) {
        (None, None) => return Ok(()),
        (Some(_), None) => (a, b),
        (None, Some(_)) => (b, a),
        (Some(ta), Some(tb)) => {
            if std::fs::read(a).ok() == std::fs::read(b).ok() {
                return Ok(());
            }
            if ta >= tb { (a, b) } else { (b, a) }
        }
    };
    if let Some(dir) = to.parent() {
        std::fs::create_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    }
    std::fs::copy(from, to).map(|_| ()).map_err(|e| format!("{} -> {}: {e}", from.display(), to.display()))
}

/// Brings the profile's and the shared copies of the settings files up to
/// date with each other. Call before the game starts and after it exits.
pub fn sync_shared(profile_dir: &Path, profile: &Profile, shared: Option<&Path>) -> Result<(), String> {
    let Some(shared) = shared.filter(|_| profile.own_game_data) else { return Ok(()) };
    let own = profile_local_appdata(profile_dir);
    for rel in SHARED_STATE_FILES {
        sync_file(&shared.join(rel), &own.join(rel))?;
    }
    Ok(())
}

/// [`sync_shared`] for every profile that keeps its own game data. Run once
/// when the launcher starts, to catch settings changed in a session whose
/// launcher had already quit (so no profile's changes are lost).
pub fn sync_all_shared(profiles_dir: &Path, shared: Option<&Path>) -> Result<(), String> {
    for entry in crate::profiles::list(profiles_dir) {
        if let Ok(profile) = &entry.profile {
            sync_shared(&entry.dir, profile, shared)?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn newest_settings_win() {
        let base = std::env::temp_dir().join(format!("sf-state-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&base);
        let (shared, profile_dir) = (base.join("shared"), base.join("profile"));
        let rel = SHARED_STATE_FILES[0];
        std::fs::create_dir_all(shared.join(rel).parent().unwrap()).unwrap();
        std::fs::write(shared.join(rel), "shared").unwrap();
        let profile = Profile { own_game_data: true, ..Profile::default() };

        sync_shared(&profile_dir, &profile, Some(&shared)).unwrap();
        let own = profile_local_appdata(&profile_dir).join(rel);
        assert_eq!(std::fs::read_to_string(&own).unwrap(), "shared");

        // Changed in the session: goes back to the shared copy.
        std::thread::sleep(std::time::Duration::from_millis(20));
        std::fs::write(&own, "changed").unwrap();
        sync_shared(&profile_dir, &profile, Some(&shared)).unwrap();
        assert_eq!(std::fs::read_to_string(shared.join(rel)).unwrap(), "changed");

        // Profiles that share the normal folder are left alone.
        std::fs::write(shared.join(rel), "again").unwrap();
        sync_shared(&profile_dir, &Profile::default(), Some(&shared)).unwrap();
        assert_eq!(std::fs::read_to_string(&own).unwrap(), "changed");
        std::fs::remove_dir_all(&base).unwrap();
    }
}
