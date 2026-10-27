//! First-run setup: where the game is, where profiles live, and (for an
//! AppImage) putting the launcher in the applications menu. It can be run
//! again from Settings.

use crate::prefs;
use sc_launch::{launch, profiles, steam};
use serde::Serialize;
use std::path::{Path, PathBuf};
use tauri::AppHandle;

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AppImageInfo {
    /// Where the running AppImage is.
    path: String,
    /// Suggested folder to keep it in.
    suggested_dir: String,
    /// It's already in that folder.
    in_place: bool,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SetupInfo {
    /// The setup still has to run.
    needed: bool,
    /// The game folder set now, or the one Steam knows; empty if neither.
    game_dir: String,
    /// `game_dir` came from Steam rather than the player.
    game_detected: bool,
    profiles_dir: String,
    standard_profiles_dir: String,
    /// Settings, trust keys and logs; fixed.
    data_dir: String,
    /// Profiles in `profiles_dir` (offered to move when it changes).
    profile_count: usize,
    appimage: Option<AppImageInfo>,
}

fn text(path: &Path) -> String {
    path.to_string_lossy().into_owned()
}

fn detected_game() -> Option<PathBuf> {
    launch::find_game(&launch::Options { game_dir: None, tools_dir: None, proton: None }).ok()
}

fn appimage() -> Option<AppImageInfo> {
    let path = PathBuf::from(std::env::var_os("APPIMAGE")?);
    let home = PathBuf::from(std::env::var_os("HOME")?);
    let suggested = home.join("Applications");
    Some(AppImageInfo { in_place: path.parent() == Some(suggested.as_path()), path: text(&path), suggested_dir: text(&suggested) })
}

#[tauri::command]
pub fn get_setup_info(app: AppHandle) -> Result<SetupInfo, String> {
    let p = prefs::get(&app);
    let chosen = p.game_directory.trim();
    let (game_dir, game_detected) = if chosen.is_empty() {
        (detected_game().map(|d| text(&d)).unwrap_or_default(), true)
    } else {
        (chosen.to_string(), false)
    };
    let profiles_dir = crate::profiles::profiles_dir()?;
    Ok(SetupInfo {
        needed: !p.setup_done,
        game_detected: game_detected && !game_dir.is_empty(),
        game_dir,
        profile_count: profiles::list(&profiles_dir).len(),
        profiles_dir: text(&profiles_dir),
        standard_profiles_dir: profiles::standard_profiles_dir().map(|d| text(&d)).unwrap_or_default(),
        data_dir: profiles::data_dir().map(|d| text(&d)).unwrap_or_default(),
        appimage: appimage(),
    })
}

/// Why `path` isn't a Star Conflict folder, if it isn't.
#[tauri::command]
pub fn check_game_folder(path: String) -> Result<(), String> {
    if steam::is_game_dir(Path::new(path.trim())) {
        Ok(())
    } else {
        Err("That folder doesn't hold Star Conflict (no win64\\game.exe). Pick the folder Steam installed it in.".into())
    }
}

#[tauri::command]
pub async fn pick_folder(app: AppHandle, title: String, start: Option<String>) -> Option<String> {
    use tauri_plugin_dialog::DialogExt;
    let mut dialog = app.dialog().file().set_title(title);
    if let Some(start) = start.filter(|s| Path::new(s).is_dir()) {
        dialog = dialog.set_directory(start);
    }
    let dir = dialog.blocking_pick_folder()?;
    dir.into_path().ok().map(|p| text(&p))
}

/// Saves the setup. An empty `game_dir`, or Steam's own folder, keeps finding
/// the game through Steam. With `move_profiles`, existing profiles move to a
/// new profiles folder.
#[tauri::command]
pub async fn finish_setup(app: AppHandle, game_dir: String, profiles_dir: String, move_profiles: bool) -> Result<(), String> {
    if crate::game::busy(&app) {
        return Err("Close the game before changing the setup.".into());
    }
    let game_dir = game_dir.trim().to_string();
    if !game_dir.is_empty() {
        check_game_folder(game_dir.clone())?;
    }
    let keep_detected = game_dir.is_empty() || detected_game().is_some_and(|d| d == Path::new(&game_dir));

    let profiles_dir = profiles_dir.trim();
    if profiles_dir.is_empty() || Path::new(profiles_dir).is_relative() {
        return Err("Pick a full path for the profiles folder.".into());
    }
    let new_dir = PathBuf::from(profiles_dir);
    let old_dir = crate::profiles::profiles_dir()?;
    if new_dir != old_dir {
        std::fs::create_dir_all(&new_dir).map_err(|e| format!("{}: {e}", new_dir.display()))?;
        if move_profiles {
            let (from, to) = (old_dir.clone(), new_dir.clone());
            tauri::async_runtime::spawn_blocking(move || profiles::move_profiles(&from, &to))
                .await
                .map_err(|e| e.to_string())??;
        }
        let standard = profiles::standard_profiles_dir();
        profiles::set_profiles_location((standard.as_ref() != Some(&new_dir)).then_some(new_dir.as_path()))?;
    }

    prefs::modify(&app, |p| {
        p.game_directory = if keep_detected { String::new() } else { game_dir };
        p.setup_done = true;
    })?;
    crate::game::refresh(&app);
    Ok(())
}

/// Moves the running AppImage into `dir`, adds it to the applications menu,
/// and restarts it from there. Linux AppImage only.
#[tauri::command]
pub fn install_appimage(app: AppHandle, dir: String) -> Result<(), String> {
    let current = PathBuf::from(std::env::var_os("APPIMAGE").ok_or("This launcher isn't running as an AppImage.")?);
    let dir = PathBuf::from(dir.trim());
    if dir.is_relative() {
        return Err("Pick a full path for the AppImage folder.".into());
    }
    std::fs::create_dir_all(&dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    let target = dir.join(current.file_name().ok_or("the AppImage has no file name")?);
    if target != current && std::fs::rename(&current, &target).is_err() {
        std::fs::copy(&current, &target).map_err(|e| format!("{}: {e}", target.display()))?;
        std::fs::remove_file(&current).map_err(|e| format!("{}: {e}", current.display()))?;
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(&target, std::fs::Permissions::from_mode(0o755))
            .map_err(|e| format!("{}: {e}", target.display()))?;
    }
    desktop_entry(&target)?;
    if target != current {
        // Start the moved copy once this one has gone (a second instance
        // would only hand over to this one and quit).
        std::process::Command::new("sh")
            .arg("-c")
            .arg("sleep 1; exec \"$0\"")
            .arg(&target)
            .spawn()
            .map_err(|e| format!("could not restart the launcher: {e}"))?;
        app.exit(0);
    }
    Ok(())
}

/// The applications-menu entry and icons, as scripts/install-desktop-entry.sh
/// writes them for development builds.
fn desktop_entry(exe: &Path) -> Result<(), String> {
    let data = std::env::var_os("XDG_DATA_HOME")
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".local/share")))
        .ok_or("cannot work out the applications folder")?;
    let write = |path: PathBuf, bytes: &[u8]| -> Result<(), String> {
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent).map_err(|e| format!("{}: {e}", parent.display()))?;
        }
        std::fs::write(&path, bytes).map_err(|e| format!("{}: {e}", path.display()))
    };
    let icons: [(&str, &[u8]); 4] = [
        ("32x32", include_bytes!("../icons/32x32.png")),
        ("128x128", include_bytes!("../icons/128x128.png")),
        ("256x256", include_bytes!("../icons/128x128@2x.png")),
        ("512x512", include_bytes!("../icons/icon.png")),
    ];
    for (size, bytes) in icons {
        write(data.join(format!("icons/hicolor/{size}/apps/star-forge.png")), bytes)?;
    }
    let icon = data.join("icons/hicolor/256x256/apps/star-forge.png");
    // Icon is a full path: by name, KDE can pick a theme's "star" icon first.
    let entry = format!(
        "[Desktop Entry]\nType=Application\nName=STAR FORGE Launcher\nComment=Star Conflict profile launcher\n\
         Exec=\"{}\" %u\nIcon={}\nStartupWMClass=star-forge\nCategories=Game;\nMimeType=x-scheme-handler/starforge;\n",
        exe.display(),
        icon.display()
    );
    write(data.join("applications/star-forge.desktop"), entry.as_bytes())?;
    for cmd in [
        vec!["update-desktop-database".to_string(), text(&data.join("applications"))],
        vec!["xdg-mime".into(), "default".into(), "star-forge.desktop".into(), "x-scheme-handler/starforge".into()],
    ] {
        let _ = std::process::Command::new(&cmd[0]).args(&cmd[1..]).status();
    }
    Ok(())
}
