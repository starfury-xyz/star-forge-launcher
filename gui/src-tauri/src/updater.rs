//! Launcher self-update through tauri-plugin-updater. The endpoint and public
//! key are in tauri.conf.json (`plugins.updater`); release builds publish a
//! signed `latest.json` next to the packages (see .github/workflows/release.yml).

use crate::{notify, NotificationLevel};
use serde::Serialize;
use std::sync::Mutex;
use tauri::{AppHandle, Emitter, Manager};
use tauri_plugin_updater::{Update, UpdaterExt};

/// The update found by the last check, kept for `install_launcher_update`.
#[derive(Default)]
pub struct UpdaterState(Mutex<Option<Update>>);

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct LauncherUpdate {
    version: String,
    current: String,
    /// Release page for the new version, when latest.json names one.
    page: Option<String>,
}

fn page_of(update: &Update) -> Option<String> {
    let page = update.raw_json.get("page")?.as_str()?;
    page.starts_with("https://").then(|| page.to_owned())
}

/// Development builds have no bundle type, so there is nothing to update.
fn supported() -> bool {
    !cfg!(debug_assertions)
}

#[tauri::command]
pub async fn check_launcher_update(app: AppHandle) -> Result<Option<LauncherUpdate>, String> {
    if !supported() {
        return Err("Development builds do not update themselves.".into());
    }
    let update = app
        .updater()
        .map_err(|e| e.to_string())?
        .check()
        .await
        .map_err(|e| format!("could not check for launcher updates: {e}"))?;
    let info = update.as_ref().map(|u| LauncherUpdate {
        version: u.version.clone(),
        current: u.current_version.clone(),
        page: page_of(u),
    });
    *app.state::<UpdaterState>().0.lock().unwrap() = update;
    Ok(info)
}

/// Downloads and installs the update found by the last check, then restarts.
/// Progress goes to the interface as `launcher-update-progress` events
/// `(downloaded, total)`, then `launcher-update-downloaded` before the
/// package's signature is checked and it installs.
#[tauri::command]
pub async fn install_launcher_update(app: AppHandle) -> Result<(), String> {
    if crate::game::busy(&app) {
        return Err("Close the game before updating the launcher.".into());
    }
    let update = app
        .state::<UpdaterState>()
        .0
        .lock()
        .unwrap()
        .take()
        .ok_or("Check for updates first.")?;
    let mut downloaded = 0u64;
    let progress = app.clone();
    let result = update
        .download_and_install(
            |chunk, total| {
                downloaded += chunk as u64;
                let _ = progress.emit_to("main", "launcher-update-progress", (downloaded, total));
            },
            || {
                let _ = progress.emit_to("main", "launcher-update-downloaded", ());
            },
        )
        .await;
    if let Err(e) = result {
        // Keep it so the player can retry without checking again.
        *app.state::<UpdaterState>().0.lock().unwrap() = Some(update);
        return Err(format!("could not install the launcher update: {e}"));
    }
    // The Windows installer closes and restarts the launcher itself; the
    // AppImage and Linux packages are replaced in place.
    notify(&app, format!("Updated to {}, restarting…", update.version), NotificationLevel::Info);
    app.restart();
}

#[tauri::command]
pub fn open_launcher_release(app: AppHandle) -> Result<(), String> {
    let page = app
        .state::<UpdaterState>()
        .0
        .lock()
        .unwrap()
        .as_ref()
        .and_then(page_of)
        .ok_or("No release page for this update.")?;
    crate::open_path(&app, page.as_ref())
}
