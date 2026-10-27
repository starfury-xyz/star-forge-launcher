//! Installing and updating server packs.
//!
//! A pack arrives as a zip (a file, or a download), a folder, or a published
//! `pack.json` whose `download` names the zip and its SHA-256. A published
//! `pack.json` without `download` is a whole pack by itself, for packs that
//! only set a server and take their game files from a list (`files_from`). It is unpacked
//! into a staging folder first so the player can review it, then moved into
//! the profiles folder. Updating replaces the pack's own files and keeps the
//! player's `local.json`.
//!
//! A signed pack (see [`crate::signing`]) is checked file by file against its
//! signature, and its key must match the one pinned to the pack id (see
//! [`crate::trust`]); the pinned key is checked again before every launch.

use crate::profiles;
use crate::signing::{self, Depth};
use crate::trust::{self, Trust};
use sc_profile::{Pack, FILES_DIR, MANIFEST_FILE, PACK_FILE, PATCHES_DIR, RESOURCES_DIR, SIGNATURE_FILE};
use sha2::{Digest, Sha256};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU32, Ordering};
use std::time::Duration;

/// Largest pack we unpack or download. Packs may carry whole `.pak` archives.
const MAX_PACK_BYTES: u64 = 8 << 30;
/// Largest published `pack.json` we read.
const MAX_MANIFEST_BYTES: u64 = 1 << 20;
const MAX_ENTRIES: usize = 100_000;
/// URL scheme registered for one-click installs: `starforge://install?url=...`.
pub const SCHEME: &str = "starforge";

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Source {
    /// A zip file or a pack folder.
    Path(PathBuf),
    /// A published `pack.json` or a zip.
    Url(String),
}

impl Source {
    /// A path, an `http(s)://` URL, or a `starforge://install?url=` link.
    pub fn parse(text: &str) -> Result<Source, String> {
        let text = text.trim();
        if let Some(rest) = text.strip_prefix(&format!("{SCHEME}://")) {
            let query = rest.strip_prefix("install").and_then(|r| r.trim_start_matches('/').strip_prefix('?'));
            let url = query
                .into_iter()
                .flat_map(|q| q.split('&'))
                .find_map(|kv| kv.strip_prefix("url="))
                .ok_or_else(|| format!("{SCHEME} link without an install url: {text}"))?;
            let url = percent_decode(url)?;
            check_url(&url)?;
            return Ok(Source::Url(url));
        }
        if text.starts_with("http://") || text.starts_with("https://") {
            check_url(text)?;
            return Ok(Source::Url(text.to_string()));
        }
        if text.contains("://") {
            return Err(format!("unsupported link: {text}"));
        }
        Ok(Source::Path(PathBuf::from(text)))
    }
}

fn percent_decode(s: &str) -> Result<String, String> {
    let bytes = s.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        match bytes[i] {
            b'%' => {
                let hex = s.get(i + 1..i + 3).ok_or("bad %-escape in link")?;
                out.push(u8::from_str_radix(hex, 16).map_err(|_| "bad %-escape in link")?);
                i += 3;
            }
            b'+' => {
                out.push(b' ');
                i += 1;
            }
            b => {
                out.push(b);
                i += 1;
            }
        }
    }
    String::from_utf8(out).map_err(|_| "link is not valid UTF-8".into())
}

/// Packs come over HTTPS; plain HTTP only from this machine (for testing).
pub(crate) fn check_url(url: &str) -> Result<(), String> {
    if url.starts_with("https://") {
        return Ok(());
    }
    if let Some(rest) = url.strip_prefix("http://") {
        let host = rest.split(['/', '?', '#']).next().unwrap_or("");
        let host = host.rsplit_once(':').map_or(host, |(h, port)| if port.chars().all(|c| c.is_ascii_digit()) { h } else { host });
        if matches!(host, "localhost" | "127.0.0.1" | "[::1]") {
            return Ok(());
        }
        return Err(format!("{url}: packs must be downloaded over https"));
    }
    Err(format!("{url}: not an http(s) link"))
}

/// A pack unpacked for review, not yet installed.
#[derive(Debug)]
pub struct Staged {
    /// Staging folder; removed by [`discard`] or [`install`].
    pub dir: PathBuf,
    /// Folder inside `dir` holding `pack.json`.
    pub root: PathBuf,
    pub pack: Pack,
    /// Where it came from, for display.
    pub origin: String,
    /// Whether the download matched the SHA-256 in a published `pack.json`.
    pub verified: bool,
    /// Who signed it, if anyone, and whether that key is already trusted.
    pub trust: Trust,
    /// What the pack changes, for the review.
    pub reach: Reach,
    /// The pack's `files_from` list, fetched and checked; its files are
    /// downloaded on install.
    pub remote: Option<crate::remote::FileList>,
    /// Paths under `files/`, with `/` separators, sorted.
    pub files: Vec<String>,
    /// Things worth telling the player that don't stop the install.
    pub warnings: Vec<String>,
}

/// What a pack can change, from least to most powerful.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Reach {
    /// Files the game reads (data, scripts, configs) under `files/`.
    pub files: usize,
    /// The pack's own patchsets, which change the game's code in memory.
    pub patchsets: usize,
    /// Core patches it turns on; their code ships with the launcher.
    pub core: usize,
    /// Native DLLs loaded into the game.
    pub plugins: usize,
}

impl Reach {
    pub fn of(root: &Path, pack: &Pack) -> Reach {
        Reach {
            files: file_list(root).len(),
            patchsets: sc_profile::patchset_files(root).len(),
            core: pack.patches.core.len(),
            plugins: pack.patches.plugins.len(),
        }
    }

    /// Whether the pack brings its own code for the game, beyond its data
    /// and the launcher's core patches.
    pub fn runs_code(&self) -> bool {
        self.patchsets > 0 || self.plugins > 0
    }
}

/// Folder for packs being reviewed, under the launcher data folder.
pub fn staging_root() -> Option<PathBuf> {
    profiles::data_dir().map(|d| d.join("staging"))
}

/// Removes leftovers from reviews that never finished.
pub fn clear_staging() {
    if let Some(dir) = staging_root() {
        let _ = std::fs::remove_dir_all(dir);
    }
}

pub(crate) fn new_staging_dir() -> Result<PathBuf, String> {
    static COUNTER: AtomicU32 = AtomicU32::new(0);
    let root = staging_root().ok_or("cannot work out the launcher data folder")?;
    let stamp = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_millis());
    let dir = root.join(format!("{}-{stamp}-{}", std::process::id(), COUNTER.fetch_add(1, Ordering::Relaxed)));
    std::fs::create_dir_all(&dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    Ok(dir)
}

/// Unpacks a pack for review. On error nothing is left behind.
pub fn stage(source: &Source) -> Result<Staged, String> {
    let dir = new_staging_dir()?;
    stage_into(source, &dir).inspect_err(|_| {
        let _ = std::fs::remove_dir_all(&dir);
    })
}

fn stage_into(source: &Source, dir: &Path) -> Result<Staged, String> {
    let content = dir.join("content");
    let (origin, verified, published) = match source {
        Source::Path(path) if path.is_dir() => {
            copy_pack_folder(path, &content)?;
            (path.display().to_string(), false, None)
        }
        Source::Path(path) => {
            unzip(path, &content)?;
            (path.display().to_string(), false, None)
        }
        Source::Url(url) => {
            let download = dir.join("download");
            fetch(url, &download, MAX_PACK_BYTES, None)?;
            if looks_like_json(&download) {
                let manifest = read_published(&download, url)?;
                let Some(zip) = manifest.download.clone() else {
                    std::fs::create_dir_all(&content).map_err(|e| format!("{}: {e}", content.display()))?;
                    std::fs::rename(&download, content.join(PACK_FILE)).map_err(|e| e.to_string())?;
                    return finish_stage(source, dir, &content, url.clone(), false, Some(manifest));
                };
                check_url(&zip.url)?;
                let mut hasher = Sha256::new();
                fetch(&zip.url, &download, MAX_PACK_BYTES, Some(&mut hasher))?;
                let got = hex(&hasher.finalize());
                if !got.eq_ignore_ascii_case(&zip.sha256) {
                    return Err(format!("{}: download does not match the SHA-256 in {url}; not installing it", zip.url));
                }
                unzip(&download, &content)?;
                (url.clone(), true, Some(manifest))
            } else {
                unzip(&download, &content)?;
                (url.clone(), false, None)
            }
        }
    };
    let _ = std::fs::remove_file(dir.join("download"));
    finish_stage(source, dir, &content, origin, verified, published)
}

fn finish_stage(
    source: &Source,
    dir: &Path,
    content: &Path,
    origin: String,
    verified: bool,
    published: Option<Pack>,
) -> Result<Staged, String> {
    let root = find_root(content)?;
    let mut pack = profiles::read_pack(&root)?;
    pack.validate().map_err(|e| e.to_string())?;
    let signature = signing::verify(&root, Depth::Full)?;
    let trust = trust::check(&pack.id, signature)?;
    let remote = match &pack.files_from {
        Some(from) => {
            if root.join(FILES_DIR).exists() {
                return Err(format!("the pack has both a {FILES_DIR}/ folder and files_from; use one or the other"));
            }
            Some(crate::remote::fetch_list(from, trust.key().as_ref()).map_err(|e| e.to_string())?)
        }
        None => None,
    };
    let mut warnings = Vec::new();
    if let Some(published) = &published {
        if published.id != pack.id || published.version != pack.version {
            return Err(format!(
                "the published pack.json describes {} {} but the download contains {} {}",
                published.id, published.version, pack.id, pack.version
            ));
        }
        // Installed from a published pack.json: check that same address for
        // updates. A signed pack.json can't be edited, so it must name it itself.
        if pack.update.is_none() && trust == Trust::Unsigned {
            if let Source::Url(url) = source {
                pack.update = Some(sc_profile::Update { url: url.clone(), min_version: None });
            }
        }
    }
    // Signed packs stay byte for byte as signed.
    if trust == Trust::Unsigned {
        pack.download = None;
        profiles::write_pack(&root, &pack)?;
    }

    if let Some(min) = &pack.requires.launcher {
        let need = sc_profile::parse_version(min).ok_or_else(|| format!("requires.launcher {min:?}: expected x.y.z"))?;
        let have = sc_profile::parse_version(env!("CARGO_PKG_VERSION")).unwrap_or_default();
        if have < need {
            return Err(format!(
                "{} needs STAR FORGE Launcher {min} or newer (this is {}); update the launcher first",
                pack.name,
                env!("CARGO_PKG_VERSION")
            ));
        }
    }
    let b = &pack.branding;
    for (key, paths) in [("logo", b.logo.as_slice()), ("icon", b.icon.as_slice()), ("background", b.background.as_slice())] {
        for p in paths {
            if !root.join(p).is_file() {
                warnings.push(format!("branding.{key} points at {p}, which is not in the pack"));
            }
        }
    }
    sc_patch::core::sets(&pack.patches.core)?;
    let sets = crate::patching::load_sets(&root);
    for e in sets.iter().filter_map(|s| s.as_ref().err()) {
        warnings.push(format!("Broken patch file: {e}"));
    }
    let mut reach = Reach::of(&root, &pack);
    if let Some(list) = &remote {
        reach.files = list.entries.len();
        if !list.signed {
            warnings.push(format!("Its game files come from {} without a signature; they are checked against the list's hashes only.", list.host()));
        }
    }
    if reach.runs_code() && trust == Trust::Unsigned {
        warnings.push(
            "This pack changes the game's code but isn't signed, so there is no telling who made it. Only install it from a source you trust.".into(),
        );
    }
    for p in &pack.patches.plugins {
        if !sc_profile::patches_rel(p).is_some_and(|rel| root.join(PATCHES_DIR).join(rel).is_file()) {
            warnings.push(format!("patches.plugins points at {p}, which is not in the pack"));
        }
    }
    if !pack.patches.plugins.is_empty() {
        warnings.push(
            "This pack runs its own native code inside the game (plugins). Only install it if you trust whoever made it completely.".into(),
        );
    }
    if pack.update.is_none() {
        warnings.push("This pack has no update address, so the launcher can't check for new versions.".into());
    }
    let files = match &remote {
        Some(list) => {
            let mut paths: Vec<String> = list.entries.iter().map(|e| e.path.clone()).collect();
            paths.sort();
            paths
        }
        None => file_list(&root),
    };
    Ok(Staged {
        dir: dir.to_path_buf(),
        root,
        pack,
        origin,
        verified,
        trust,
        reach,
        remote,
        files,
        warnings,
    })
}

/// Paths under a profile's `files/`, with `/` separators, sorted.
pub fn file_list(profile_dir: &Path) -> Vec<String> {
    fn walk(dir: &Path, prefix: &str, out: &mut Vec<String>) {
        let Ok(rd) = std::fs::read_dir(dir) else { return };
        for e in rd.flatten() {
            let name = e.file_name().to_string_lossy().into_owned();
            if name.starts_with('.') {
                continue;
            }
            let rel = if prefix.is_empty() { name } else { format!("{prefix}/{name}") };
            match e.file_type() {
                Ok(t) if t.is_dir() => walk(&e.path(), &rel, out),
                Ok(t) if t.is_file() => out.push(rel),
                _ => {}
            }
        }
    }
    let mut out = Vec::new();
    walk(&profile_dir.join(FILES_DIR), "", &mut out);
    out.sort();
    out
}

pub fn discard(staged: Staged) {
    let _ = std::fs::remove_dir_all(staged.dir);
}

/// An installed pack.
#[derive(Debug)]
pub struct Installed {
    /// The profile's folder name.
    pub name: String,
    /// Set when the pack installed but its listed game files didn't all
    /// download; they are fetched again before the next launch.
    pub warning: Option<String>,
}

/// Installs a reviewed pack: updates the profile that already has this pack id,
/// or creates a new one, then downloads any listed game files, telling
/// `progress` how that goes (see [`crate::remote::sync`]).
pub fn install(
    staged: Staged,
    profiles_dir: &Path,
    progress: &mut dyn FnMut(crate::remote::SyncEvent<'_>) -> Result<(), String>,
) -> Result<Installed, String> {
    let result = install_inner(&staged, profiles_dir);
    let remote = staged.remote.clone();
    let pack_name = staged.pack.name.clone();
    discard(staged);
    let name = result?;
    let warning = remote.and_then(|list| {
        crate::remote::sync(&profiles_dir.join(&name), &list, progress)
            .err()
            .map(|e| format!("{pack_name} is installed, but its game files didn't all download ({e}). They'll be fetched again before you play."))
    });
    Ok(Installed { name, warning })
}

fn install_inner(staged: &Staged, profiles_dir: &Path) -> Result<String, String> {
    std::fs::create_dir_all(profiles_dir).map_err(|e| format!("{}: {e}", profiles_dir.display()))?;
    // Checked again: another install may have pinned a key since the review.
    let trust = trust::check(&staged.pack.id, staged.trust.key().map_or(signing::Signature::Unsigned, signing::Signature::Signed))?;
    let name = if let Some(existing) = profiles::find_pack(profiles_dir, &staged.pack.id) {
        replace_pack(&staged.root, &existing)?;
        existing.file_name().unwrap().to_string_lossy().into_owned()
    } else {
        let name = profiles::free_name(profiles_dir, &staged.pack.name);
        let target = profiles_dir.join(&name);
        std::fs::rename(&staged.root, &target).map_err(|e| format!("{}: {e}", target.display()))?;
        name
    };
    if let Trust::NewKey(key) = trust {
        trust::pin(&staged.pack.id, &key)?;
    }
    Ok(name)
}

/// Checks an installed pack before it starts: its files against its
/// signature (code fully, `files/` by size) and its key against the pinned
/// one. A signed pack installed without the launcher gets its key pinned here.
pub fn verify_installed(profile_dir: &Path) -> Result<Trust, String> {
    let pack = profiles::read_pack(profile_dir)?;
    let signature = signing::verify(profile_dir, Depth::Launch)
        .map_err(|e| format!("{}: {e}. Reinstall the pack to repair it.", pack.name))?;
    let trust = trust::check(&pack.id, signature)?;
    if let Trust::NewKey(key) = trust {
        trust::pin(&pack.id, &key)?;
        return Ok(Trust::Pinned(key));
    }
    Ok(trust)
}

/// The parts of a profile folder that belong to the pack.
const PACK_PARTS: [&str; 6] = [PACK_FILE, MANIFEST_FILE, SIGNATURE_FILE, RESOURCES_DIR, FILES_DIR, PATCHES_DIR];

fn replace_pack(new_root: &Path, profile: &Path) -> Result<(), String> {
    // A pack that downloads its files from a list keeps the ones it has, so
    // an update only fetches what changed. Otherwise files/ is the pack's.
    let new_listed = profiles::read_pack(new_root).is_ok_and(|p| p.files_from.is_some());
    let old_listed = profiles::read_pack(profile).is_ok_and(|p| p.files_from.is_some());
    let keep_files = new_listed && old_listed;
    if !new_listed {
        crate::remote::forget(profile);
    }
    let old = profile.join(".update-old");
    let _ = std::fs::remove_dir_all(&old);
    std::fs::create_dir_all(&old).map_err(|e| format!("{}: {e}", old.display()))?;
    let mut moved_out = Vec::new();
    let mut moved_in = Vec::new();
    let result = (|| {
        for part in PACK_PARTS.into_iter().filter(|p| !(keep_files && *p == FILES_DIR)) {
            if profile.join(part).exists() {
                std::fs::rename(profile.join(part), old.join(part)).map_err(|e| format!("{}: {e}", profile.join(part).display()))?;
                moved_out.push(part);
            }
        }
        for part in PACK_PARTS.into_iter().filter(|p| !(keep_files && *p == FILES_DIR)) {
            if new_root.join(part).exists() {
                std::fs::rename(new_root.join(part), profile.join(part)).map_err(|e| format!("{}: {e}", profile.join(part).display()))?;
                moved_in.push(part);
            }
        }
        Ok(())
    })();
    if result.is_err() {
        // Put the old pack back.
        for part in moved_in {
            let _ = std::fs::remove_dir_all(profile.join(part)).or_else(|_| std::fs::remove_file(profile.join(part)));
        }
        for part in moved_out {
            let _ = std::fs::rename(old.join(part), profile.join(part));
        }
    }
    let _ = std::fs::remove_dir_all(&old);
    result
}

/// A newer published version of an installed pack.
#[derive(Debug, Clone)]
pub struct Available {
    pub version: String,
    pub url: String,
    /// The server no longer accepts the installed version.
    pub required: bool,
}

/// Asks the pack's update address whether there is a newer version.
pub fn check_update(pack: &Pack) -> Result<Option<Available>, String> {
    let Some(update) = &pack.update else { return Ok(None) };
    check_url(&update.url)?;
    let dir = new_staging_dir()?;
    let path = dir.join("pack.json");
    let result = fetch(&update.url, &path, MAX_MANIFEST_BYTES, None).and_then(|()| read_published(&path, &update.url));
    let _ = std::fs::remove_dir_all(&dir);
    let published = result?;
    if published.id != pack.id {
        return Err(format!("{}: publishes pack {} instead of {}", update.url, published.id, pack.id));
    }
    let have = sc_profile::parse_version(&pack.version);
    let newer = sc_profile::parse_version(&published.version) > have;
    let required = published
        .update
        .as_ref()
        .and_then(|u| u.min_version.as_deref())
        .and_then(sc_profile::parse_version)
        .is_some_and(|min| have < Some(min));
    Ok(newer.then(|| Available { version: published.version, url: update.url.clone(), required }))
}

/// Before a launch: asks the pack's server whether the installed version is
/// still accepted. `Err` means it isn't (update first); a warning means it
/// couldn't tell (the player should know they may be missing an update), or a
/// newer optional version exists.
pub fn launch_check(pack: &Pack) -> Result<Option<String>, String> {
    if pack.update.is_none() {
        return Ok(None);
    }
    match check_update(pack) {
        Ok(Some(a)) if a.required => Err(format!(
            "{}'s server needs pack version {} or newer (you have {}). Update the pack first.",
            pack.name, a.version, pack.version
        )),
        Ok(Some(a)) => Ok(Some(format!("{} {} is available.", pack.name, a.version))),
        Ok(None) => Ok(None),
        Err(e) => Ok(Some(format!("Could not check {} for updates: {e}", pack.name))),
    }
}

fn read_published(path: &Path, url: &str) -> Result<Pack, String> {
    let text = std::fs::read_to_string(path).map_err(|e| format!("{url}: {e}"))?;
    let pack = Pack::from_json(&text).map_err(|e| format!("{url}: {e}"))?;
    pack.validate().map_err(|e| format!("{url}: {e}"))?;
    Ok(pack)
}

fn looks_like_json(path: &Path) -> bool {
    let mut buf = [0u8; 64];
    let n = std::fs::File::open(path).and_then(|mut f| f.read(&mut buf)).unwrap_or(0);
    buf[..n].iter().find(|b| !b.is_ascii_whitespace()) == Some(&b'{')
}

/// Downloads `url` to `dest`, at most `limit` bytes, optionally hashing it.
pub(crate) fn fetch(url: &str, dest: &Path, limit: u64, hasher: Option<&mut Sha256>) -> Result<(), String> {
    fetch_with(url, dest, limit, hasher, &mut |_, _| Ok(()))
}

/// [`fetch`], calling `on_bytes` after each chunk with the bytes received so
/// far and the size the server announced, if it did. An error from
/// `on_bytes` stops the download with that error.
pub(crate) fn fetch_with(
    url: &str,
    dest: &Path,
    limit: u64,
    mut hasher: Option<&mut Sha256>,
    on_bytes: &mut dyn FnMut(u64, Option<u64>) -> Result<(), String>,
) -> Result<(), String> {
    let agent = ureq::AgentBuilder::new()
        .timeout_connect(Duration::from_secs(15))
        .timeout_read(Duration::from_secs(60))
        .user_agent(concat!("star-forge-launcher/", env!("CARGO_PKG_VERSION")))
        .https_only(!url.starts_with("http://"))
        .build();
    let response = agent.get(url).call().map_err(|e| format!("{url}: {e}"))?;
    let announced = response.header("Content-Length").and_then(|v| v.trim().parse().ok());
    let mut reader = response.into_reader().take(limit + 1);
    let mut file = std::fs::File::create(dest).map_err(|e| format!("{}: {e}", dest.display()))?;
    let mut buf = vec![0u8; 64 * 1024];
    let mut total = 0u64;
    loop {
        let n = reader.read(&mut buf).map_err(|e| format!("{url}: {e}"))?;
        if n == 0 {
            break;
        }
        total += n as u64;
        if total > limit {
            return Err(format!("{url}: larger than the {} MB limit", limit >> 20));
        }
        if let Some(h) = hasher.as_deref_mut() {
            h.update(&buf[..n]);
        }
        file.write_all(&buf[..n]).map_err(|e| format!("{}: {e}", dest.display()))?;
        on_bytes(total, announced)?;
    }
    Ok(())
}

pub(crate) fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

/// SHA-256 of a file, hex.
pub fn sha256_file(path: &Path) -> std::io::Result<String> {
    let mut f = std::fs::File::open(path)?;
    let mut h = Sha256::new();
    std::io::copy(&mut f, &mut h)?;
    Ok(hex(&h.finalize()))
}

/// A warning when the pack lists the game builds it was made for and the
/// installed `game.exe` is none of them.
pub fn game_build_warning(pack: &Pack, game_exe: &Path) -> Option<String> {
    if pack.requires.game.is_empty() {
        return None;
    }
    let hash = sha256_file(game_exe).ok()?;
    let known = pack.requires.game.iter().any(|h| h.eq_ignore_ascii_case(&hash));
    (!known).then(|| {
        format!(
            "{} was made for a different game build; its files and patches may not work with this one.",
            pack.name
        )
    })
}

/// What a pack may contain at its top level; anything else is ignored.
fn allowed(rel: &str) -> bool {
    let top = rel.split('/').next().unwrap_or("");
    let file = !rel.contains('/');
    if rel.split('/').any(|part| part.starts_with('.')) {
        return false; // .gitkeep, .DS_Store, ...
    }
    rel == PACK_FILE
        || rel == MANIFEST_FILE
        || rel == SIGNATURE_FILE
        || ((top == RESOURCES_DIR || top == FILES_DIR || top == PATCHES_DIR) && !file)
        || (file && ["readme", "license", "changelog"].iter().any(|p| top.to_ascii_lowercase().starts_with(p)))
}

/// Relative path with `/` separators if every part is a plain name.
fn clean_rel(path: &str) -> Option<String> {
    let parts: Vec<&str> = path.split(['/', '\\']).filter(|p| !p.is_empty()).collect();
    let ok = !parts.is_empty() && parts.iter().all(|p| *p != "." && *p != ".." && !p.contains(':'));
    ok.then(|| parts.join("/"))
}

fn unzip(zip_path: &Path, dest: &Path) -> Result<(), String> {
    let file = std::fs::File::open(zip_path).map_err(|e| format!("{}: {e}", zip_path.display()))?;
    let mut archive = zip::ZipArchive::new(file).map_err(|e| format!("{}: not a pack zip ({e})", zip_path.display()))?;
    if archive.len() > MAX_ENTRIES {
        return Err(format!("{}: too many files", zip_path.display()));
    }
    let names: Vec<String> = (0..archive.len())
        .filter_map(|i| archive.by_index_raw(i).ok().and_then(|f| clean_rel(f.name())))
        .collect();
    // pack.json at the top, or everything inside a single folder.
    let prefix = if names.iter().any(|n| n == PACK_FILE) {
        String::new()
    } else {
        let tops: std::collections::BTreeSet<&str> = names.iter().map(|n| n.split('/').next().unwrap_or("")).collect();
        match tops.into_iter().collect::<Vec<_>>().as_slice() {
            [top] if names.iter().any(|n| *n == format!("{top}/{PACK_FILE}")) => format!("{top}/"),
            _ => return Err(format!("{}: no {PACK_FILE} at the top of the zip", zip_path.display())),
        }
    };
    let mut total = 0u64;
    for i in 0..archive.len() {
        let mut entry = archive.by_index(i).map_err(|e| format!("{}: {e}", zip_path.display()))?;
        let Some(rel) = clean_rel(entry.name()) else { continue };
        let Some(rel) = rel.strip_prefix(&prefix) else { continue };
        if entry.is_dir() || !allowed(rel) {
            continue;
        }
        if entry.unix_mode().is_some_and(|m| m & 0o170000 == 0o120000) {
            continue; // symlink
        }
        let out = dest.join(rel);
        std::fs::create_dir_all(out.parent().unwrap()).map_err(|e| format!("{}: {e}", out.display()))?;
        let mut f = std::fs::File::create(&out).map_err(|e| format!("{}: {e}", out.display()))?;
        let remaining = MAX_PACK_BYTES - total;
        let n = std::io::copy(&mut (&mut entry).take(remaining + 1), &mut f).map_err(|e| format!("{rel}: {e}"))?;
        total += n;
        if total > MAX_PACK_BYTES {
            return Err(format!("{}: unpacks to more than {} GB", zip_path.display(), MAX_PACK_BYTES >> 30));
        }
    }
    Ok(())
}

fn copy_pack_folder(src: &Path, dest: &Path) -> Result<(), String> {
    fn walk(src: &Path, rel: &str, dest: &Path, count: &mut usize) -> Result<(), String> {
        let dir = if rel.is_empty() { src.to_path_buf() } else { src.join(rel) };
        for e in std::fs::read_dir(&dir).map_err(|e| format!("{}: {e}", dir.display()))?.flatten() {
            let name = e.file_name().to_string_lossy().into_owned();
            let child = if rel.is_empty() { name } else { format!("{rel}/{name}") };
            let Ok(t) = e.file_type() else { continue };
            if t.is_dir() && (child.contains('/') || child == RESOURCES_DIR || child == FILES_DIR || child == PATCHES_DIR) {
                walk(src, &child, dest, count)?;
            } else if t.is_file() && allowed(&child) {
                *count += 1;
                if *count > MAX_ENTRIES {
                    return Err(format!("{}: too many files", src.display()));
                }
                let out = dest.join(&child);
                std::fs::create_dir_all(out.parent().unwrap()).map_err(|e| format!("{}: {e}", out.display()))?;
                std::fs::copy(e.path(), &out).map_err(|e| format!("{}: {e}", out.display()))?;
            }
        }
        Ok(())
    }
    if !src.join(PACK_FILE).is_file() {
        return Err(format!("{}: no {PACK_FILE} in this folder", src.display()));
    }
    walk(src, "", dest, &mut 0)
}

fn find_root(content: &Path) -> Result<PathBuf, String> {
    if content.join(PACK_FILE).is_file() {
        Ok(content.to_path_buf())
    } else {
        Err(format!("no {PACK_FILE} found in the pack"))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::remote::quiet;

    #[test]
    fn parses_sources() {
        assert_eq!(Source::parse("https://a.org/p.json").unwrap(), Source::Url("https://a.org/p.json".into()));
        assert_eq!(
            Source::parse("starforge://install?url=https%3A%2F%2Fa.org%2Fpack.json").unwrap(),
            Source::Url("https://a.org/pack.json".into())
        );
        assert_eq!(
            Source::parse("starforge://install/?x=1&url=https://a.org/p.zip").unwrap(),
            Source::Url("https://a.org/p.zip".into())
        );
        assert!(Source::parse("starforge://install?url=http%3A%2F%2Fa.org%2Fp.zip").is_err());
        assert!(Source::parse("starforge://other").is_err());
        assert!(Source::parse("ftp://a.org/p.zip").is_err());
        assert!(Source::parse("http://localhost:8000/p.zip").is_ok());
        assert!(Source::parse("http://evil.org/p.zip").is_err());
        assert_eq!(Source::parse("/home/user/pack.zip").unwrap(), Source::Path("/home/user/pack.zip".into()));
    }

    #[test]
    fn filters_pack_entries() {
        assert!(allowed("pack.json"));
        assert!(allowed("files/data/address.cfg"));
        assert!(allowed("resources/logo.png"));
        assert!(allowed("patches/game.exe.patchset.toml"));
        assert!(allowed("README.md"));
        assert!(!allowed("files"));
        assert!(!allowed("local.json"));
        assert!(!allowed("agent.log"));
        assert!(!allowed("__MACOSX/x"));
        assert!(!allowed("files/data/.gitkeep"));
        assert_eq!(clean_rel("a/../b"), None);
        assert_eq!(clean_rel("C:/x"), None);
        assert_eq!(clean_rel("/a//b\\c").as_deref(), Some("a/b/c"));
    }

    fn write_zip(path: &Path, entries: &[(&str, &str)]) {
        let mut zip = zip::ZipWriter::new(std::fs::File::create(path).unwrap());
        for (name, body) in entries {
            zip.start_file(*name, zip::write::SimpleFileOptions::default()).unwrap();
            zip.write_all(body.as_bytes()).unwrap();
        }
        zip.finish().unwrap();
    }

    #[test]
    fn installs_and_updates_a_pack() {
        let base = std::env::temp_dir().join(format!("sf-packs-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&base);
        std::fs::create_dir_all(&base).unwrap();
        crate::test_data_dir();
        let profiles_dir = base.join("profiles");
        let manifest = |v: &str| {
            format!(r#"{{"format":1,"id":"org.example.test","name":"Test Server","version":"{v}","server":{{"address":"a.org:1"}}}}"#)
        };

        let v1 = base.join("v1.zip");
        write_zip(&v1, &[
            ("test/pack.json", &manifest("1.0.0")),
            ("test/files/data/a.cfg", "one"),
            ("test/files/data/old.cfg", "old"),
            ("test/local.json", r#"{"name":"sneaky"}"#),
            ("test/../escape.txt", "x"),
        ]);
        let staged = stage(&Source::Path(v1)).unwrap();
        assert_eq!(staged.files, vec!["data/a.cfg", "data/old.cfg"]);
        assert!(!staged.verified);
        let name = install(staged, &profiles_dir, &mut quiet).unwrap().name;
        assert_eq!(name, "test-server");
        let dir = profiles_dir.join(&name);
        assert!(!dir.join("local.json").exists(), "packs can't ship player settings");
        assert!(!base.join("escape.txt").exists());

        profiles::write_local(&dir, &sc_profile::Local { name: Some("Mine".into()), ..Default::default() }).unwrap();
        let v2 = base.join("v2.zip");
        write_zip(&v2, &[("pack.json", &manifest("1.1.0")), ("files/data/a.cfg", "two")]);
        let name2 = install(stage(&Source::Path(v2)).unwrap(), &profiles_dir, &mut quiet).unwrap().name;
        assert_eq!(name2, name, "same pack id updates in place");
        assert_eq!(std::fs::read_to_string(dir.join("files/data/a.cfg")).unwrap(), "two");
        assert!(!dir.join("files/data/old.cfg").exists());
        let p = profiles::load(&dir).unwrap();
        assert_eq!(p.name, "Mine", "local settings survive updates");
        assert_eq!(profiles::read_pack(&dir).unwrap().version, "1.1.0");

        let bad = base.join("bad.zip");
        write_zip(&bad, &[("readme.txt", "no manifest")]);
        assert!(stage(&Source::Path(bad)).is_err());
        std::fs::remove_dir_all(&base).unwrap();
    }

    #[test]
    fn signed_packs_keep_their_key() {
        crate::test_data_dir();
        let base = std::env::temp_dir().join(format!("sf-signed-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&base);
        let profiles_dir = base.join("profiles");
        let make = |name: &str, version: &str, key: Option<&signing::SecretKey>| {
            let dir = base.join(name);
            std::fs::create_dir_all(dir.join("patches")).unwrap();
            std::fs::write(
                dir.join(PACK_FILE),
                format!(r#"{{"format":1,"id":"org.example.signed","name":"Signed","version":"{version}"}}"#),
            )
            .unwrap();
            std::fs::write(dir.join("patches/game.exe.patchset.toml"), "x").unwrap();
            if let Some(key) = key {
                signing::sign(&dir, key).unwrap();
            }
            Source::Path(dir)
        };
        let key = signing::SecretKey::generate().unwrap();
        let other = signing::SecretKey::generate().unwrap();

        let staged = stage(&make("v1", "1.0.0", Some(&key))).unwrap();
        assert_eq!(staged.trust, Trust::NewKey(key.public()));
        assert!(staged.reach.runs_code());
        let name = install(staged, &profiles_dir, &mut quiet).unwrap().name;
        let dir = profiles_dir.join(&name);
        assert_eq!(verify_installed(&dir).unwrap(), Trust::Pinned(key.public()));

        assert!(stage(&make("v2-other", "1.1.0", Some(&other))).is_err(), "another key can't update it");
        assert!(stage(&make("v2-unsigned", "1.1.0", None)).is_err(), "nor an unsigned copy");
        let v2 = stage(&make("v2", "1.1.0", Some(&key))).unwrap();
        assert_eq!(v2.trust, Trust::Pinned(key.public()));
        install(v2, &profiles_dir, &mut quiet).unwrap();

        std::fs::write(dir.join("patches/game.exe.patchset.toml"), "tampered").unwrap();
        assert!(verify_installed(&dir).is_err());
        trust::unpin("org.example.signed").unwrap();
        std::fs::remove_dir_all(&base).unwrap();
    }
}
