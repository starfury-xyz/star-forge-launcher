//! Launcher preferences, kept in `<data dir>/launcher.json`.

use crate::Locked;
use serde::{Deserialize, Serialize};
use std::path::PathBuf;
use std::sync::Mutex;
use tauri::{AppHandle, Manager};

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum LaunchBehavior {
    #[default]
    Tray,
    KeepOpen,
    Exit,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum CloseBehavior {
    #[default]
    Tray,
    Exit,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum Faction {
    #[default]
    Starforge,
    Starfury,
    Empire,
    Federation,
    Jericho,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct Preferences {
    /// Folder name of the profile the play button uses.
    pub selected_profile: String,
    pub launch_behavior: LaunchBehavior,
    pub close_behavior: CloseBehavior,
    pub faction_theme: Faction,
    /// Show the selected server pack's logo, icon, background and colour.
    pub pack_branding: bool,
    /// Folder of the player's own wallpapers; empty = Steam's banner.
    pub background_folder: String,
    /// Game install folder; empty = find through Steam.
    pub game_directory: String,
    /// Proton build folder (Linux); empty = the one that last ran the game.
    pub proton_path: String,
    /// Look for a new launcher version at startup.
    pub check_updates: bool,
}

impl Default for Preferences {
    fn default() -> Self {
        Self {
            selected_profile: String::new(),
            launch_behavior: LaunchBehavior::default(),
            close_behavior: CloseBehavior::default(),
            faction_theme: Faction::default(),
            pack_branding: true,
            background_folder: String::new(),
            game_directory: String::new(),
            proton_path: String::new(),
            check_updates: true,
        }
    }
}

#[derive(Default)]
pub struct PrefsState(Mutex<Preferences>);

fn file() -> Option<PathBuf> {
    sc_launch::profiles::data_dir().map(|d| d.join("launcher.json"))
}

pub fn load(app: &AppHandle) {
    let prefs = file()
        .and_then(|f| std::fs::read_to_string(f).ok())
        .and_then(|t| serde_json::from_str(&t).ok())
        .unwrap_or_default();
    *app.state::<PrefsState>().0.locked() = prefs;
}

pub fn get(app: &AppHandle) -> Preferences {
    app.state::<PrefsState>().0.locked().clone()
}

fn save(prefs: &Preferences) -> Result<(), String> {
    let path = file().ok_or("cannot work out the launcher data folder")?;
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    }
    let text = serde_json::to_string_pretty(prefs).map_err(|e| e.to_string())?;
    let tmp = path.with_extension("json.tmp");
    std::fs::write(&tmp, text).map_err(|e| format!("{}: {e}", tmp.display()))?;
    std::fs::rename(&tmp, &path).map_err(|e| format!("{}: {e}", path.display()))
}

/// Applies `f` to the preferences and saves them.
pub fn modify(app: &AppHandle, f: impl FnOnce(&mut Preferences)) -> Result<Preferences, String> {
    let state = app.state::<PrefsState>();
    let mut guard = state.0.locked();
    let mut next = guard.clone();
    f(&mut next);
    save(&next)?;
    *guard = next.clone();
    Ok(next)
}

#[derive(Deserialize)]
pub struct PreferenceUpdate {
    key: String,
    value: serde_json::Value,
}

#[tauri::command]
pub fn get_launcher_preferences(app: AppHandle) -> Preferences {
    get(&app)
}

#[tauri::command]
pub fn update_launcher_preferences(app: AppHandle, update: PreferenceUpdate) -> Result<Preferences, String> {
    // Round-trip through JSON so the value is checked against the field's type.
    let mut value = serde_json::to_value(get(&app)).map_err(|e| e.to_string())?;
    let fields = value.as_object_mut().ok_or("preferences are not an object")?;
    if !fields.contains_key(&update.key) {
        return Err(format!("unknown preference {}", update.key));
    }
    fields.insert(update.key.clone(), update.value);
    let parsed: Preferences =
        serde_json::from_value(value).map_err(|e| format!("invalid value for {}: {e}", update.key))?;
    let next = modify(&app, |p| *p = parsed)?;
    if update.key == "gameDirectory" || update.key == "protonPath" {
        crate::game::refresh(&app);
    }
    Ok(next)
}
