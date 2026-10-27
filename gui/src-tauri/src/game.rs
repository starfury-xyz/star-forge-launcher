//! Game detection, launch state and the play button.

use crate::prefs::{self, LaunchBehavior};
use crate::{lifecycle, notify, NotificationLevel};
use base64::Engine;
use sc_launch::{launch, profiles, steam};
use serde::Serialize;
use std::path::{Path, PathBuf};
use std::process::Stdio;
use std::sync::Mutex;
use tauri::{AppHandle, Emitter, Manager};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub enum Phase {
    Checking,
    Ready,
    Launching,
    Running,
    Failed,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Snapshot {
    revision: u64,
    phase: Phase,
    primary_action: Option<&'static str>,
    busy: bool,
    blocking_reason: Option<String>,
    /// Label of a button that fixes `blocking_reason` in Steam.
    fix_label: Option<&'static str>,
    game_directory: Option<String>,
    proton: Option<String>,
}

#[derive(Default)]
struct Inner {
    revision: u64,
    phase: Option<Phase>,
    reason: Option<String>,
    fix: Option<launch::SteamFix>,
    /// Steam was opened for `fix`; the play button is Retry again.
    fix_opened: bool,
    setup: Option<launch::Setup>,
}

#[derive(Default)]
pub struct GameState(Mutex<Inner>);

fn snapshot_of(inner: &Inner) -> Snapshot {
    let phase = inner.phase.unwrap_or(Phase::Checking);
    #[cfg(unix)]
    let proton = inner.setup.as_ref().map(|s| {
        let runtime = s.proton.runtime.parent().and_then(|d| d.file_name()).map(|n| n.to_string_lossy().into_owned());
        format!("{} inside {}", s.proton.dir.display(), runtime.unwrap_or_else(|| "the Steam Linux Runtime".into()))
    });
    #[cfg(not(unix))]
    let proton = None;
    Snapshot {
        revision: inner.revision,
        phase,
        primary_action: match phase {
            Phase::Ready => Some("launch"),
            Phase::Failed if inner.fix.is_some() && !inner.fix_opened => Some("fix"),
            Phase::Failed => Some("retry"),
            _ => None,
        },
        busy: matches!(phase, Phase::Checking | Phase::Launching),
        blocking_reason: inner.reason.clone(),
        fix_label: inner.fix.map(launch::SteamFix::label),
        game_directory: inner.setup.as_ref().map(|s| s.game_dir.display().to_string()),
        proton,
    }
}

fn update(app: &AppHandle, f: impl FnOnce(&mut Inner)) {
    let snapshot = {
        let state = app.state::<GameState>();
        let mut inner = state.0.lock().unwrap();
        f(&mut inner);
        inner.revision += 1;
        snapshot_of(&inner)
    };
    if let Err(e) = app.emit_to("main", "launcher-state-changed", snapshot) {
        eprintln!("could not publish launcher state: {e}");
    }
}

fn phase(app: &AppHandle) -> Phase {
    app.state::<GameState>().0.lock().unwrap().phase.unwrap_or(Phase::Checking)
}

/// The game is starting or running.
pub fn busy(app: &AppHandle) -> bool {
    matches!(phase(app), Phase::Launching | Phase::Running)
}

/// Where sc-inject.exe and sc_agent.dll are: `STAR_FORGE_TOOLS_DIR`, bundled
/// resources, next to this program, or (development builds only) the repo's
/// dist folder.
fn tools_dir(app: &AppHandle) -> Option<PathBuf> {
    if let Some(dir) = sc_launch::env::path(sc_launch::env::TOOLS_DIR) {
        return Some(dir);
    }
    let has_tools = |d: &Path| d.join(launch::INJECT_EXE).is_file() && d.join(launch::AGENT_DLL).is_file();
    let mut candidates = Vec::new();
    if let Ok(dir) = app.path().resource_dir() {
        candidates.push(dir.join("tools"));
    }
    if let Some(dir) = std::env::current_exe().ok().and_then(|e| e.parent().map(Path::to_path_buf)) {
        candidates.push(dir.clone());
        // target/<profile>/ -> <repo>/dist/<platform>/. Never in release builds:
        // the search climbs to the drive root, and a folder planted there (any
        // user can create C:\dist on Windows) would get its DLL into the game.
        if cfg!(debug_assertions) {
            let platform = if cfg!(windows) { "windows" } else { "linux" };
            if let Some(repo) = dir.ancestors().find(|a| a.join("dist").join(platform).is_dir()) {
                candidates.push(repo.join("dist").join(platform));
            }
        }
    }
    candidates.into_iter().find(|d| has_tools(d))
}

fn options(app: &AppHandle) -> launch::Options {
    let p = prefs::get(app);
    let path = |s: &str| (!s.trim().is_empty()).then(|| PathBuf::from(s.trim()));
    launch::Options {
        game_dir: path(&p.game_directory),
        tools_dir: tools_dir(app),
        proton: path(&p.proton_path),
    }
}

/// Re-detects the game and Proton in the background.
pub fn refresh(app: &AppHandle) {
    if busy(app) {
        return;
    }
    update(app, |s| {
        s.phase = Some(Phase::Checking);
        s.reason = None;
        s.fix = None;
        s.fix_opened = false;
    });
    let app = app.clone();
    std::thread::spawn(move || {
        let result = launch::resolve(&options(&app));
        update(&app, |s| match result {
            Ok(setup) => {
                s.setup = Some(setup);
                s.phase = Some(Phase::Ready);
            }
            Err(e) => {
                s.setup = None;
                s.phase = Some(Phase::Failed);
                s.reason = Some(e.message);
                s.fix = e.fix;
            }
        });
    });
}

fn launch_log() -> Option<PathBuf> {
    profiles::data_dir().map(|d| d.join("game-launch.log"))
}

fn start_game(app: &AppHandle) -> Result<(), String> {
    let setup = app
        .state::<GameState>()
        .0
        .lock()
        .unwrap()
        .setup
        .clone()
        .ok_or("the game setup has not been detected yet")?;
    let selected = prefs::get(app).selected_profile;
    if selected.is_empty() || !profiles::valid_name(&selected) {
        return Err("Choose a server profile first.".into());
    }
    let dir = crate::profiles::profiles_dir()?.join(&selected);
    let profile = profiles::load(&dir)?;
    if let Some(warning) = profiles::read_pack(&dir).ok().and_then(|p| sc_launch::packs::game_build_warning(&p, &setup.game_exe())) {
        notify(app, warning, NotificationLevel::Info);
    }
    let trust = sc_launch::packs::verify_installed(&dir)?;
    let pack = profiles::read_pack(&dir)?;
    if let Some(warning) = sc_launch::packs::launch_check(&pack)? {
        notify(app, warning, NotificationLevel::Info);
    }
    let mut download = crate::downloads::Session::new(app, "update", &pack.name);
    let synced = sc_launch::remote::launch_sync(&dir, &pack, trust.key().as_ref(), &mut |e| download.report(e));
    download.finish(synced.as_ref().err().map(String::as_str));
    if let Some(note) = synced? {
        notify(app, note, NotificationLevel::Info);
    }
    for warning in sc_launch::patching::preflight(&dir, &profile, &setup.game_exe())? {
        notify(app, warning, NotificationLevel::Info);
    }
    let shared = sc_launch::state::shared_local_appdata(&setup);
    sc_launch::state::sync_shared(&dir, &profile, shared.as_deref())?;
    let mut cmd = launch::command(&setup, &dir, &[])?;

    // Proton and the injector are chatty; keep their output for troubleshooting.
    let (out, err) = match launch_log().and_then(|p| {
        p.parent().map(std::fs::create_dir_all);
        std::fs::File::create(p).ok()
    }) {
        Some(f) => (
            f.try_clone().map(Stdio::from).unwrap_or_else(|_| Stdio::null()),
            Stdio::from(f),
        ),
        None => (Stdio::null(), Stdio::null()),
    };
    cmd.stdin(Stdio::null()).stdout(out).stderr(err);
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        const CREATE_NO_WINDOW: u32 = 0x0800_0000;
        cmd.creation_flags(CREATE_NO_WINDOW);
    }
    let mut child = cmd.spawn().map_err(|e| format!("could not start the game: {e}"))?;
    update(app, |s| s.phase = Some(Phase::Running));

    match prefs::get(app).launch_behavior {
        LaunchBehavior::Tray => {
            if let Err(e) = lifecycle::hide_to_tray(app) {
                eprintln!("could not hide the launcher: {e}");
            }
        }
        LaunchBehavior::KeepOpen => {}
        LaunchBehavior::Exit => {
            // The game keeps running on its own.
            app.exit(0);
            return Ok(());
        }
    }

    let app = app.clone();
    std::thread::spawn(move || {
        let status = child.wait();
        if let Err(e) = sc_launch::state::sync_shared(&dir, &profile, shared.as_deref()) {
            notify(&app, format!("Could not save your game settings for other profiles: {e}"), NotificationLevel::Error);
        }
        update(&app, |s| s.phase = Some(Phase::Ready));
        match status {
            Ok(s) if s.success() => {}
            Ok(s) => {
                eprintln!("game exited with {s}");
                if let Some(e) = launch::inject_error(&dir) {
                    notify(&app, format!("The game did not start: {e}"), NotificationLevel::Error);
                }
            }
            Err(e) => eprintln!("could not wait for the game: {e}"),
        }
        if prefs::get(&app).launch_behavior == LaunchBehavior::Tray {
            lifecycle::restore(&app);
        }
    });
    Ok(())
}

/// The detected game.exe, once the setup has been found.
pub fn game_exe(app: &AppHandle) -> Option<std::path::PathBuf> {
    app.state::<GameState>().0.lock().unwrap().setup.as_ref().map(|s| s.game_exe())
}

#[tauri::command]
pub fn get_launcher_snapshot(app: AppHandle) -> Snapshot {
    snapshot_of(&app.state::<GameState>().0.lock().unwrap())
}

#[tauri::command]
pub fn initialize_launcher(app: AppHandle) {
    refresh(&app);
}

#[tauri::command]
pub fn perform_launcher_action(app: AppHandle, action: String) -> Result<(), String> {
    match (action.as_str(), phase(&app)) {
        ("fix", Phase::Failed) => {
            let fix = app.state::<GameState>().0.lock().unwrap().fix.ok_or("Nothing to fix in Steam.")?;
            crate::open_path(&app, std::ffi::OsStr::new(&fix.url()))?;
            update(&app, |s| s.fix_opened = true);
            Ok(())
        }
        ("retry", Phase::Failed | Phase::Ready) => {
            refresh(&app);
            Ok(())
        }
        ("launch", Phase::Ready) => {
            update(&app, |s| s.phase = Some(Phase::Launching));
            // Off the main thread: checks hash the pack and may ask its server.
            std::thread::spawn(move || {
                if let Err(e) = start_game(&app) {
                    update(&app, |s| s.phase = Some(Phase::Ready));
                    if e == crate::downloads::CANCELLED {
                        notify(&app, "Download cancelled; the game was not started.".into(), NotificationLevel::Info);
                    } else {
                        notify(&app, e, NotificationLevel::Error);
                    }
                }
            });
            Ok(())
        }
        _ => Err("This action is not available right now.".into()),
    }
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CompatTools {
    supported: bool,
    proton: Vec<String>,
}

#[tauri::command]
pub fn detect_compat_tools() -> CompatTools {
    #[cfg(unix)]
    {
        let roots = steam::steam_roots();
        let libraries = steam::libraries(&roots);
        CompatTools {
            supported: true,
            proton: sc_launch::compat::proton_candidates(&roots, &libraries)
                .into_iter()
                .map(|p| p.display().to_string())
                .collect(),
        }
    }
    #[cfg(not(unix))]
    CompatTools { supported: false, proton: Vec::new() }
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Artwork {
    hero: Option<String>,
    logo: Option<String>,
}

/// A file from Steam's library artwork cache for the game, in either the
/// current (`<appid>/<hash>/<name>`) or older (`<appid>_<name>`) layout.
fn steam_art(name: &str) -> Option<PathBuf> {
    for root in steam::steam_roots() {
        let cache = root.join("appcache").join("librarycache");
        let flat = cache.join(format!("{}_{name}", steam::GAME_APPID));
        if flat.is_file() {
            return Some(flat);
        }
        let Ok(entries) = std::fs::read_dir(cache.join(steam::GAME_APPID)) else { continue };
        for e in entries.flatten() {
            let p = e.path();
            if p.join(name).is_file() {
                return Some(p.join(name));
            }
            if p.file_name().is_some_and(|f| f == name) && p.is_file() {
                return Some(p);
            }
        }
    }
    None
}

/// An image as a data URL: PNG, JPEG or WebP by its contents, up to 8 MB.
pub fn data_url(path: &Path) -> Option<String> {
    if std::fs::metadata(path).ok()?.len() > 8 << 20 {
        eprintln!("{}: too large for artwork (8 MB at most)", path.display());
        return None;
    }
    let bytes = std::fs::read(path).ok()?;
    let mime = match bytes.as_slice() {
        [0x89, b'P', b'N', b'G', ..] => "image/png",
        [0xff, 0xd8, 0xff, ..] => "image/jpeg",
        [b'R', b'I', b'F', b'F', _, _, _, _, b'W', b'E', b'B', b'P', ..] => "image/webp",
        _ => {
            eprintln!("{}: not a PNG, JPEG or WebP image", path.display());
            return None;
        }
    };
    Some(format!(
        "data:{mime};base64,{}",
        base64::engine::general_purpose::STANDARD.encode(bytes)
    ))
}

/// A number below `n` that differs from run to run (n > 0).
pub fn random_below(n: usize) -> usize {
    use std::hash::{BuildHasher, Hasher};
    let seed = std::collections::hash_map::RandomState::new().build_hasher().finish();
    (seed % n as u64) as usize
}

/// A random image from the player's background folder (not its subfolders).
fn folder_background(dir: &str) -> Option<String> {
    if dir.trim().is_empty() {
        return None;
    }
    let mut images: Vec<PathBuf> = std::fs::read_dir(dir.trim())
        .ok()?
        .flatten()
        .map(|e| e.path())
        .filter(|p| {
            p.is_file()
                && p.extension()
                    .and_then(|e| e.to_str())
                    .is_some_and(|e| matches!(e.to_ascii_lowercase().as_str(), "jpg" | "jpeg" | "png" | "webp"))
        })
        .collect();
    images.sort();
    if images.is_empty() {
        return None;
    }
    // Start at a random image and take the first one that loads.
    let start = random_below(images.len());
    (0..images.len()).find_map(|i| data_url(&images[(start + i) % images.len()]))
}

/// The default play screen art: a random image from the player's background
/// folder, else Star Conflict's banner from the local Steam install (nothing
/// is bundled).
#[tauri::command]
pub async fn get_artwork(app: AppHandle) -> Artwork {
    let folder = prefs::get(&app).background_folder;
    Artwork {
        hero: folder_background(&folder).or_else(|| steam_art("library_hero.jpg").and_then(|p| data_url(&p))),
        logo: steam_art("logo.png").and_then(|p| data_url(&p)),
    }
}

/// Lets the player choose their background folder.
#[tauri::command]
pub async fn pick_background_folder(app: AppHandle) -> Option<String> {
    use tauri_plugin_dialog::DialogExt;
    let dir = app.dialog().file().set_title("Background folder").blocking_pick_folder()?;
    dir.into_path().ok().map(|p| p.to_string_lossy().into_owned())
}

#[tauri::command]
pub fn view_game_install(app: AppHandle) -> Result<(), String> {
    let dir = app
        .state::<GameState>()
        .0
        .lock()
        .unwrap()
        .setup
        .as_ref()
        .map(|s| s.game_dir.clone())
        .ok_or("The game folder has not been found yet.")?;
    crate::open_path(&app, dir.as_os_str())
}

#[tauri::command]
pub fn view_launch_log(app: AppHandle) -> Result<(), String> {
    let log = launch_log().filter(|p| p.is_file()).ok_or("The game has not been started from the launcher yet.")?;
    crate::open_path(&app, log.as_os_str())
}
