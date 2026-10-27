//! Server profiles: list, add, edit and delete from the launcher, and install
//! or update server packs.

use sc_launch::{packs, profiles};
use sc_profile::{Local, Pack, FILES_DIR};
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Mutex;
use tauri::{AppHandle, Emitter, Manager};

pub fn profiles_dir() -> Result<PathBuf, String> {
    profiles::default_profiles_dir().ok_or_else(|| "cannot work out the profiles folder".into())
}

fn profile_dir(id: &str) -> Result<PathBuf, String> {
    let dir = profiles_dir()?.join(id);
    if !profiles::valid_name(id) || !profiles::is_profile(&dir) {
        return Err(format!("profile {id:?} does not exist"));
    }
    Ok(dir)
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Link {
    key: &'static str,
    label: &'static str,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ProfileInfo {
    id: String,
    name: String,
    server: Option<String>,
    local_server: bool,
    /// The game's per-user data stays in the profile (see sc_launch::state).
    own_game_data: bool,
    patches: usize,
    error: Option<String>,
    pack_id: Option<String>,
    version: Option<String>,
    description: String,
    /// Published by a server operator and updated from them.
    managed: bool,
    links: Vec<Link>,
}

fn link_label(key: &str) -> &'static str {
    match key {
        "website" => "Website",
        "discord" => "Discord",
        "news" => "News",
        _ => "Source",
    }
}

#[tauri::command]
pub fn list_profiles() -> Result<Vec<ProfileInfo>, String> {
    Ok(profiles::list(&profiles_dir()?)
        .into_iter()
        .map(|e| {
            let pack = e.pack.as_ref();
            let base = |name: String| ProfileInfo {
                name,
                id: e.name.clone(),
                server: None,
                local_server: false,
                own_game_data: true,
                patches: 0,
                error: None,
                pack_id: pack.map(|p| p.id.clone()),
                version: pack.map(|p| p.version.clone()),
                description: pack.map(|p| p.description.clone()).unwrap_or_default(),
                managed: pack.is_some_and(|p| p.update.is_some()),
                links: pack
                    .map(|p| p.links.entries().map(|(key, _)| Link { key, label: link_label(key) }).collect())
                    .unwrap_or_default(),
            };
            match &e.profile {
                Ok(p) => {
                    let patching = sc_launch::patching::summary(&e.dir, p);
                    ProfileInfo {
                        server: p.server.clone(),
                        local_server: p.local_server,
                        own_game_data: p.own_game_data,
                        patches: patching.patches,
                        error: patching.errors.first().cloned(),
                        ..base(if p.name.trim().is_empty() { e.name.clone() } else { p.name.clone() })
                    }
                }
                Err(err) => ProfileInfo { error: Some(err.clone()), ..base(e.name.clone()) },
            }
        })
        .collect())
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ProfileInput {
    id: Option<String>,
    name: String,
    server: String,
    local_server: bool,
    own_game_data: bool,
}

/// Removes an `address.cfg` this launcher generated, so clearing the server
/// field really clears it. A hand-written file is left alone.
fn remove_generated_address(dir: &std::path::Path) {
    let cfg = dir.join(FILES_DIR).join("data").join("address.cfg");
    if let Ok(text) = std::fs::read_to_string(&cfg) {
        let lines: Vec<&str> = text.lines().filter(|l| !l.trim().is_empty()).collect();
        if lines.len() == 1 && lines[0].starts_with("set net_masterServerAddress ") {
            let _ = std::fs::remove_file(cfg);
        }
    }
}

const ADDRESS_HINT: &str =
    "The server address can only use letters, digits, '.', '-', '_' and ':' (for example STARFURY.XYZ:3801).";

#[tauri::command]
pub fn save_profile(app: AppHandle, profile: ProfileInput) -> Result<String, String> {
    let root = profiles_dir()?;
    let name = profile.name.trim();
    if name.is_empty() {
        return Err("Give the profile a name.".into());
    }
    if name.chars().count() > 64 {
        return Err("The profile name is too long (64 characters at most).".into());
    }
    let server = profile.server.trim();

    let id = match &profile.id {
        Some(id) => {
            profile_dir(id)?;
            id.clone()
        }
        None => profiles::free_name(&root, name),
    };
    let dir = root.join(&id);

    match profile.id {
        // A pack from a server operator: keep the player's changes in
        // local.json so updates don't undo them.
        Some(_) if profiles::read_pack(&dir)?.update.is_some() => {
            let pack = profiles::read_pack(&dir)?;
            let mut local = profiles::read_local(&dir)?;
            local.name = (name != pack.name).then(|| name.to_string());
            let pack_server = pack.server.address.as_deref().unwrap_or("");
            local.server = (server != pack_server).then(|| server.to_string());
            local.local_server = (profile.local_server != pack.server.local).then_some(profile.local_server);
            local.shared_game_data = !profile.own_game_data;
            sc_profile::Profile::new(&pack, &local).address_cfg().map_err(|_| ADDRESS_HINT.to_string())?;
            profiles::write_local(&dir, &local)?;
        }
        // The player's own pack: edit it directly.
        Some(_) => {
            let mut pack = profiles::read_pack(&dir)?;
            pack.name = name.to_string();
            pack.server.address = (!server.is_empty()).then(|| server.to_string());
            pack.server.local = profile.local_server;
            write_own_pack(&dir, &pack)?;
            set_shared_game_data(&dir, !profile.own_game_data)?;
        }
        None => {
            let mut pack = Pack {
                schema: Some(SCHEMA_URL.into()),
                format: sc_profile::PACK_FORMAT,
                id: format!("local.{}", profiles::slug(&id)),
                name: name.to_string(),
                version: "1.0.0".into(),
                ..Pack::default()
            };
            pack.server.address = (!server.is_empty()).then(|| server.to_string());
            pack.server.local = profile.local_server;
            std::fs::create_dir_all(&dir).map_err(|e| format!("{}: {e}", dir.display()))?;
            write_own_pack(&dir, &pack)
                .and_then(|()| set_shared_game_data(&dir, !profile.own_game_data))
                .inspect_err(|_| {
                    let _ = std::fs::remove_dir_all(&dir);
                })?;
        }
    }
    if server.is_empty() {
        remove_generated_address(&dir);
    }
    crate::prefs::modify(&app, |p| {
        if p.selected_profile.is_empty() {
            p.selected_profile = id.clone();
        }
    })?;
    Ok(id)
}

const SCHEMA_URL: &str = "https://raw.githubusercontent.com/starfury-xyz/star-forge-launcher/main/schemas/pack.schema.json";

/// The game data choice lives in local.json for every kind of profile.
fn set_shared_game_data(dir: &std::path::Path, shared: bool) -> Result<(), String> {
    let mut local = profiles::read_local(dir)?;
    if local.shared_game_data != shared {
        local.shared_game_data = shared;
        profiles::write_local(dir, &local)?;
    }
    Ok(())
}

fn write_own_pack(dir: &std::path::Path, pack: &Pack) -> Result<(), String> {
    sc_profile::Profile::new(pack, &Local::default()).address_cfg().map_err(|_| ADDRESS_HINT.to_string())?;
    profiles::write_pack(dir, pack)
}

#[tauri::command]
pub fn delete_profile(app: AppHandle, id: String) -> Result<(), String> {
    let dir = profile_dir(&id)?;
    let pack_id = profiles::read_pack(&dir).map(|p| p.id);
    std::fs::remove_dir_all(&dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    // A reinstall starts over, so a server that changed its signing key can be installed again.
    if let Ok(pack_id) = pack_id {
        sc_launch::trust::unpin(&pack_id)?;
    }
    crate::prefs::modify(&app, |p| {
        if p.selected_profile == id {
            p.selected_profile.clear();
        }
    })?;
    Ok(())
}

#[tauri::command]
pub fn open_profile_folder(app: AppHandle, id: Option<String>) -> Result<(), String> {
    let dir = match id {
        Some(id) => profile_dir(&id)?,
        None => {
            let root = profiles_dir()?;
            std::fs::create_dir_all(&root).map_err(|e| format!("{}: {e}", root.display()))?;
            root
        }
    };
    crate::open_path(&app, dir.as_os_str())
}

/// Opens one of a pack's links. The URL comes from the pack itself, never from
/// the interface, so only http(s) links the pack declares can be opened.
#[tauri::command]
pub fn open_pack_link(app: AppHandle, id: String, key: String) -> Result<(), String> {
    let pack = profiles::read_pack(&profile_dir(&id)?)?;
    let url = pack
        .links
        .entries()
        .find(|(k, _)| *k == key)
        .map(|(_, url)| url.to_string())
        .ok_or("This pack has no such link.")?;
    if !url.starts_with("https://") && !url.starts_with("http://") {
        return Err(format!("Not opening {url}: only web links are allowed."));
    }
    crate::open_path(&app, std::ffi::OsStr::new(&url))
}

#[derive(Serialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct BrandingInfo {
    logo: Option<String>,
    icon: Option<String>,
    background: Option<String>,
    accent: Option<String>,
}

/// A pack image as a data URL. The file must really sit inside the pack's
/// `resources/` (symlinks out are refused).
fn resource_image(dir: &std::path::Path, path: Option<&str>) -> Option<String> {
    let rel = sc_profile::resource_rel(path?)?;
    let root = dir.join(sc_profile::RESOURCES_DIR).canonicalize().ok()?;
    let file = root.join(rel).canonicalize().ok().filter(|f| f.starts_with(&root))?;
    crate::game::data_url(&file)
}

/// The selected pack's artwork and colour; missing entries use the defaults.
#[tauri::command]
pub async fn get_profile_branding(id: String) -> Result<BrandingInfo, String> {
    let dir = profile_dir(&id)?;
    let Ok(pack) = profiles::read_pack(&dir) else { return Ok(BrandingInfo::default()) };
    let b = &pack.branding;
    Ok(BrandingInfo {
        logo: resource_image(&dir, b.logo.as_deref()),
        icon: resource_image(&dir, b.icon.as_deref()),
        background: match b.background.len() {
            0 => None,
            n => resource_image(&dir, Some(&b.background[crate::game::random_below(n)])),
        },
        accent: b.accent.clone(),
    })
}

/// Packs unpacked for review, by token, until installed or discarded.
#[derive(Default)]
pub struct PackReviews {
    staged: Mutex<HashMap<u64, packs::Staged>>,
    next: Mutex<u64>,
    /// Whether the interface listens for links yet, and a link that arrived
    /// before it did (e.g. the one the launcher was started with).
    links: Mutex<(bool, Option<String>)>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PackUpdate {
    profile: String,
    name: String,
    version: String,
    server: Option<String>,
    patches: usize,
    files_added: usize,
    files_removed: usize,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PackReview {
    token: u64,
    id: String,
    name: String,
    version: String,
    description: String,
    authors: Vec<String>,
    origin: String,
    verified: bool,
    /// Key fingerprint, when signed.
    signer: Option<String>,
    /// Publisher name, when the key belongs to a publisher the launcher knows.
    publisher: Option<String>,
    /// Signed with a key not seen before for this pack (it gets remembered).
    new_key: bool,
    /// Patches or plugins: the pack changes the game's code.
    runs_code: bool,
    managed: bool,
    server: Option<String>,
    local_server: bool,
    args: Vec<String>,
    patches: usize,
    plugins: usize,
    file_count: usize,
    files: Vec<String>,
    /// Host the game files are downloaded from, for packs with a file list.
    files_host: Option<String>,
    /// Their total size, when the list says.
    download_bytes: Option<u64>,
    warnings: Vec<String>,
    /// Set when this pack is already installed and would be updated.
    update: Option<PackUpdate>,
}

const FILES_SHOWN: usize = 50;

fn review_of(token: u64, staged: &packs::Staged, game_exe: Option<PathBuf>) -> Result<PackReview, String> {
    let pack = &staged.pack;
    let mut warnings = staged.warnings.clone();
    let plain = sc_profile::Profile::new(pack, &Local::default());
    if let Some(exe) = game_exe {
        warnings.extend(packs::game_build_warning(pack, &exe));
        // Tells the player now, not at Play, when the patches can't apply to their game.
        match sc_launch::patching::preflight(&staged.root, &plain, &exe) {
            Ok(w) => warnings.extend(w),
            Err(e) => warnings.push(e),
        }
    }
    let patching = sc_launch::patching::summary(&staged.root, &plain);
    let update = match profiles::find_pack(&profiles_dir()?, &pack.id) {
        Some(dir) => {
            let old = profiles::read_pack(&dir)?;
            let old_files = packs::file_list(&dir);
            let added = staged.files.iter().filter(|f| old_files.binary_search(f).is_err()).count();
            let removed = old_files.iter().filter(|f| staged.files.binary_search(f).is_err()).count();
            Some(PackUpdate {
                profile: dir.file_name().unwrap().to_string_lossy().into_owned(),
                name: old.name,
                version: old.version,
                server: old.server.address,
                patches: sc_launch::patching::summary(&dir, &profiles::load(&dir).unwrap_or_default()).patches,
                files_added: added,
                files_removed: removed,
            })
        }
        None => None,
    };
    Ok(PackReview {
        token,
        id: pack.id.clone(),
        name: pack.name.clone(),
        version: pack.version.clone(),
        description: pack.description.clone(),
        authors: pack.authors.clone(),
        origin: match packs::staging_root().map(|r| r.join("samples")) {
            Some(samples) if std::path::Path::new(&staged.origin).starts_with(&samples) => "Included with STAR FORGE".into(),
            _ => staged.origin.clone(),
        },
        verified: staged.verified,
        signer: staged.trust.key().map(|k| k.fingerprint()),
        publisher: match &staged.trust {
            sc_launch::trust::Trust::Known { name, .. } => Some(name.clone()),
            _ => None,
        },
        new_key: matches!(staged.trust, sc_launch::trust::Trust::NewKey(_)),
        runs_code: staged.reach.runs_code(),
        managed: pack.update.is_some(),
        server: pack.server.address.clone(),
        local_server: pack.server.local,
        args: pack.game.args.clone(),
        patches: patching.patches,
        plugins: patching.plugins,
        file_count: staged.files.len(),
        files: staged.files.iter().take(FILES_SHOWN).cloned().collect(),
        files_host: staged.remote.as_ref().map(|l| l.host().to_string()),
        download_bytes: staged.remote.as_ref().and_then(|l| l.bytes()),
        warnings,
        update,
    })
}

/// Downloads or unpacks a pack and describes what installing it would do.
#[tauri::command]
pub async fn inspect_pack(app: AppHandle, source: String) -> Result<PackReview, String> {
    let source = packs::Source::parse(&source)?;
    let game_exe = crate::game::game_exe(&app);
    let staged = tauri::async_runtime::spawn_blocking(move || packs::stage(&source))
        .await
        .map_err(|e| e.to_string())??;
    let reviews = app.state::<PackReviews>();
    let token = {
        let mut next = reviews.next.lock().unwrap();
        *next += 1;
        *next
    };
    let review = match review_of(token, &staged, game_exe) {
        Ok(r) => r,
        Err(e) => {
            packs::discard(staged);
            return Err(e);
        }
    };
    reviews.staged.lock().unwrap().insert(token, staged);
    Ok(review)
}

#[tauri::command]
pub async fn install_pack(app: AppHandle, token: u64) -> Result<String, String> {
    let staged = app
        .state::<PackReviews>()
        .staged
        .lock()
        .unwrap()
        .remove(&token)
        .ok_or("This pack review has expired; open the pack again.")?;
    let root = profiles_dir()?;
    let handle = app.clone();
    let installed = tauri::async_runtime::spawn_blocking(move || {
        let mut download = crate::downloads::Session::new(&handle, "install", &staged.pack.name);
        let installed = packs::install(staged, &root, &mut |e| download.report(e));
        download.finish(installed.as_ref().map_or_else(|e| Some(e.as_str()), |i| i.warning.as_deref()));
        installed
    })
    .await
    .map_err(|e| e.to_string())??;
    if let Some(w) = installed.warning {
        crate::notify(&app, w, crate::NotificationLevel::Error);
    }
    let id = installed.name;
    crate::prefs::modify(&app, |p| p.selected_profile = id.clone())?;
    Ok(id)
}

#[tauri::command]
pub fn discard_pack(app: AppHandle, token: u64) {
    if let Some(staged) = app.state::<PackReviews>().staged.lock().unwrap().remove(&token) {
        packs::discard(staged);
    }
}

/// Community server packs that ship with the launcher (see samples/ in the repo).
const SAMPLES: [(&str, &str); 2] = [
    ("starfury", include_str!("../../../samples/starfury/pack.json")),
    ("sc-classic-europe", include_str!("../../../samples/sc-classic-europe/pack.json")),
];

#[derive(Serialize)]
pub struct SamplePack {
    key: &'static str,
    name: String,
}

#[tauri::command]
pub fn list_sample_packs() -> Vec<SamplePack> {
    SAMPLES
        .iter()
        .filter_map(|(key, json)| Some(SamplePack { key, name: Pack::from_json(json).ok()?.name }))
        .collect()
}

/// Writes a sample pack to a folder the review can open, and returns it.
#[tauri::command]
pub fn sample_pack_source(key: String) -> Result<String, String> {
    let (_, json) = SAMPLES.iter().find(|(k, _)| *k == key).ok_or("unknown sample pack")?;
    let dir = packs::staging_root().ok_or("cannot work out the launcher data folder")?.join("samples").join(&key);
    std::fs::create_dir_all(&dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    profiles::write_atomic(&dir.join(sc_profile::PACK_FILE), json)?;
    Ok(dir.to_string_lossy().into_owned())
}

#[tauri::command]
pub async fn pick_pack_file(app: AppHandle) -> Option<String> {
    use tauri_plugin_dialog::DialogExt;
    let file = app
        .dialog()
        .file()
        .set_title("Import server pack")
        .add_filter("Server pack", &["zip"])
        .blocking_pick_file()?;
    file.into_path().ok().map(|p| p.to_string_lossy().into_owned())
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct UpdateAvailable {
    id: String,
    version: String,
    url: String,
}

/// Asks every pack with an update address for a newer version. Packs that
/// can't be reached are skipped quietly; this runs in the background.
#[tauri::command]
pub async fn check_pack_updates() -> Result<Vec<UpdateAvailable>, String> {
    let entries = profiles::list(&profiles_dir()?);
    tauri::async_runtime::spawn_blocking(move || {
        entries
            .into_iter()
            .filter_map(|e| {
                let pack = e.pack?;
                match packs::check_update(&pack) {
                    Ok(Some(a)) => Some(UpdateAvailable { id: e.name, version: a.version, url: a.url }),
                    Ok(None) => None,
                    Err(err) => {
                        eprintln!("update check for {}: {err}", e.name);
                        None
                    }
                }
            })
            .collect()
    })
    .await
    .map_err(|e| e.to_string())
}

/// Hands a `starforge://` link to the interface, or keeps it until it listens.
pub fn receive_link(app: &AppHandle, url: String) {
    let reviews = app.state::<PackReviews>();
    let mut links = reviews.links.lock().unwrap();
    if links.0 {
        crate::lifecycle::restore(app);
        if let Err(e) = app.emit_to("main", "pack-link", url) {
            eprintln!("could not pass on the pack link: {e}");
        }
    } else {
        links.1 = Some(url);
    }
}

/// Called once the interface listens for `pack-link`; returns a link that came earlier.
#[tauri::command]
pub fn take_pending_link(app: AppHandle) -> Option<String> {
    let reviews = app.state::<PackReviews>();
    let mut links = reviews.links.lock().unwrap();
    links.0 = true;
    links.1.take()
}
