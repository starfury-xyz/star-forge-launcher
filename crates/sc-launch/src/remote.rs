//! Game files from a published file list (`files_from` in `pack.json`).
//!
//! Instead of shipping its game files in `files/`, a pack can point at a list
//! of them on the server's site. Two list formats are read, so servers can
//! keep the one they already publish:
//!
//! - **STAR FORGE file list** (JSON): `{"files": {"data/x.lua": {"sha256":
//!   "…", "size": 123}}}`, with each file at the list's folder plus its path
//!   (or `base_url` plus its path, or its own `url`). Extra fields such as
//!   `version` are ignored, so per-version manifests from other tools work.
//! - **SCMOD1**: a text file whose first line is `SCMOD1`, then one
//!   `path<TAB>sha256<TAB>https-url` per file (`#` lines are comments).
//!
//! Paths are relative to the game folder, like `files/`. Files are downloaded
//! into the profile's `files/` (never the game folder), each checked against
//! its SHA-256, and only new or changed files are fetched. For a signed pack
//! the list must carry a detached signature by the same key at `<list>.sig`,
//! which covers every file through the hashes.
//!
//! Both formats are kept working while the community settles on one.

use crate::packs::{check_url, fetch, fetch_with, hex};
use crate::signing::{verify_bytes, PublicKey};
use sc_profile::{FilesFrom, FILES_DIR, GENERATED_FILES, REMOTE_RECORD};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::path::Path;

/// Largest file list we read.
const MAX_LIST_BYTES: u64 = 8 << 20;
/// Largest single file we download.
const MAX_FILE_BYTES: u64 = 4 << 30;
const MAX_ENTRIES: usize = 100_000;

/// One file in a list.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Entry {
    /// Relative to the game folder, `/` separators.
    pub path: String,
    /// Lower-case hex.
    pub sha256: String,
    pub size: Option<u64>,
    pub url: String,
}

/// A fetched and checked file list.
#[derive(Debug, Clone)]
pub struct FileList {
    pub source: String,
    pub entries: Vec<Entry>,
    /// Whether it carried a valid signature by the pack's key.
    pub signed: bool,
}

impl FileList {
    /// Total size, when the list gives every file's size.
    pub fn bytes(&self) -> Option<u64> {
        self.entries.iter().map(|e| e.size).sum()
    }

    /// The host the files come from, for display.
    pub fn host(&self) -> &str {
        let rest = self.source.split_once("://").map_or(self.source.as_str(), |(_, r)| r);
        rest.split(['/', '?', '#']).next().unwrap_or(rest)
    }
}

/// Why a list couldn't be used: the network (worth retrying, and old files
/// may do) or the list itself (never use it).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ListError {
    Network(String),
    Invalid(String),
}

impl std::fmt::Display for ListError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ListError::Network(e) | ListError::Invalid(e) => f.write_str(e),
        }
    }
}

#[derive(Deserialize)]
struct JsonList {
    #[serde(default)]
    base_url: Option<String>,
    files: BTreeMap<String, JsonEntry>,
}

#[derive(Deserialize)]
struct JsonEntry {
    sha256: String,
    #[serde(default)]
    size: Option<u64>,
    #[serde(default)]
    url: Option<String>,
}

/// A game-relative path with `/` separators, or why it isn't one.
fn clean_path(raw: &str) -> Result<String, String> {
    let parts: Vec<&str> = raw.trim().split(['/', '\\']).collect();
    let ok = !parts.is_empty()
        && parts.iter().all(|p| !p.is_empty() && *p != "." && *p != ".." && !p.contains(':') && !p.contains('\0'));
    if !ok {
        return Err(format!("{raw:?} is not a plain path inside the game folder"));
    }
    let path = parts.join("/");
    if GENERATED_FILES.iter().any(|g| g.eq_ignore_ascii_case(&path)) {
        return Err(format!("{path} is written by the launcher from server.address; set that instead"));
    }
    Ok(path)
}

fn clean_hash(raw: &str, path: &str) -> Result<String, String> {
    let h = raw.trim().to_ascii_lowercase();
    if h.len() == 64 && h.chars().all(|c| c.is_ascii_hexdigit()) {
        Ok(h)
    } else {
        Err(format!("{path}: not a SHA-256"))
    }
}

/// `https://a.org/x/list.json` -> `https://a.org/x/`.
fn folder_of(url: &str) -> &str {
    let end = url.find(['?', '#']).unwrap_or(url.len());
    url[..end].rfind('/').map_or(url, |i| &url[..=i])
}

/// Appends a path to a base URL, percent-encoding each segment.
fn join_url(base: &str, path: &str) -> String {
    let mut out = base.trim_end_matches('/').replace(' ', "%20");
    for seg in path.split('/') {
        out.push('/');
        for b in seg.bytes() {
            if b.is_ascii_alphanumeric() || b"-._~".contains(&b) {
                out.push(b as char);
            } else {
                out.push_str(&format!("%{b:02X}"));
            }
        }
    }
    out
}

pub fn parse_json(bytes: &[u8], list_url: &str) -> Result<Vec<Entry>, String> {
    let list: JsonList = serde_json::from_slice(bytes).map_err(|e| format!("file list: {e}"))?;
    let base = list.base_url.as_deref().unwrap_or_else(|| folder_of(list_url));
    list.files
        .into_iter()
        .map(|(raw, e)| {
            let path = clean_path(&raw)?;
            let sha256 = clean_hash(&e.sha256, &path)?;
            let url = e.url.unwrap_or_else(|| join_url(base, &path));
            Ok(Entry { path, sha256, size: e.size, url })
        })
        .collect()
}

pub fn parse_scmod1(text: &str) -> Result<Vec<Entry>, String> {
    let mut lines = text.lines();
    if lines.next().map(|l| l.trim_start_matches('\u{feff}').trim()) != Some("SCMOD1") {
        return Err("not an SCMOD1 list (its first line must be SCMOD1)".into());
    }
    let mut out = Vec::new();
    for (i, line) in lines.enumerate() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let fields: Vec<&str> = line.split('\t').collect();
        let [path, hash, url] = fields.as_slice() else {
            return Err(format!("SCMOD1 line {}: expected path, SHA-256 and URL separated by tabs", i + 2));
        };
        let path = clean_path(path)?;
        let sha256 = clean_hash(hash, &path)?;
        out.push(Entry { path, sha256, size: None, url: url.trim().to_string() });
    }
    Ok(out)
}

/// Downloads a pack's file list and checks it. `key` is the pack's signing
/// key: a signed pack's list must be signed by it too.
pub fn fetch_list(from: &FilesFrom, key: Option<&PublicKey>) -> Result<FileList, ListError> {
    let url = from.url();
    check_url(url).map_err(ListError::Invalid)?;
    let dir = crate::packs::new_staging_dir().map_err(ListError::Invalid)?;
    let result = (|| {
        let path = dir.join("list");
        fetch(url, &path, MAX_LIST_BYTES, None).map_err(ListError::Network)?;
        let bytes = std::fs::read(&path).map_err(|e| ListError::Network(e.to_string()))?;
        let signed = match key {
            Some(key) => {
                let sig_path = dir.join("list.sig");
                fetch(&format!("{url}.sig"), &sig_path, 4096, None)
                    .map_err(|e| ListError::Invalid(format!("the pack is signed but its file list has no signature ({e})")))?;
                let sig = std::fs::read_to_string(&sig_path).unwrap_or_default();
                verify_bytes(key, &bytes, &sig)
                    .map_err(|e| ListError::Invalid(format!("{url}: {e}; it was not signed with the pack's key")))?;
                true
            }
            None => false,
        };
        let entries = if from.scmod1.is_some() {
            parse_scmod1(&String::from_utf8_lossy(&bytes))
        } else {
            parse_json(&bytes, url)
        }
        .map_err(|e| ListError::Invalid(format!("{url}: {e}")))?;
        if entries.len() > MAX_ENTRIES {
            return Err(ListError::Invalid(format!("{url}: too many files")));
        }
        let mut seen = std::collections::HashSet::new();
        for e in &entries {
            check_url(&e.url).map_err(ListError::Invalid)?;
            if !seen.insert(e.path.to_ascii_lowercase()) {
                return Err(ListError::Invalid(format!("{url}: {} is listed twice", e.path)));
            }
        }
        Ok(FileList { source: url.to_string(), entries, signed })
    })();
    let _ = std::fs::remove_dir_all(&dir);
    result
}

/// Writes a file list for every file under `folder` (laid out like the game
/// folder). SCMOD1 needs `base_url`, since it names each file's address; a
/// STAR FORGE list without one expects the files next to it.
pub fn build_list(folder: &Path, base_url: Option<&str>, scmod1: bool) -> Result<String, String> {
    fn walk(dir: &Path, rel: &str, out: &mut Vec<String>) -> Result<(), String> {
        for e in std::fs::read_dir(dir).map_err(|e| format!("{}: {e}", dir.display()))?.flatten() {
            let name = e.file_name().to_string_lossy().into_owned();
            if name.starts_with('.') {
                continue;
            }
            let child = if rel.is_empty() { name } else { format!("{rel}/{name}") };
            match e.file_type() {
                Ok(t) if t.is_dir() => walk(&e.path(), &child, out)?,
                Ok(t) if t.is_file() => out.push(child),
                _ => {}
            }
        }
        Ok(())
    }
    let mut paths = Vec::new();
    walk(folder, "", &mut paths)?;
    paths.sort();
    let mut files = BTreeMap::new();
    for rel in &paths {
        clean_path(rel)?;
        let path = folder.join(rel);
        let size = std::fs::metadata(&path).map_err(|e| format!("{rel}: {e}"))?.len();
        files.insert(rel.clone(), (crate::packs::sha256_file(&path).map_err(|e| format!("{rel}: {e}"))?, size));
    }
    if scmod1 {
        let base = base_url.ok_or("an SCMOD1 list needs --base-url (where the files will be hosted)")?;
        let mut out = String::from("SCMOD1\n");
        for (rel, (sha, _)) in &files {
            out.push_str(&format!("{rel}\t{sha}\t{}\n", join_url(base, rel)));
        }
        return Ok(out);
    }
    let mut list = serde_json::json!({ "format": 1, "files": {} });
    if let Some(base) = base_url {
        list["base_url"] = base.into();
    }
    for (rel, (sha, size)) in files {
        list["files"][rel] = serde_json::json!({ "sha256": sha, "size": size });
    }
    Ok(serde_json::to_string_pretty(&list).map_err(|e| e.to_string())? + "\n")
}

/// What a profile downloaded last time (`remote-files.json`).
#[derive(Debug, Default, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Record {
    source: String,
    files: BTreeMap<String, Recorded>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Recorded {
    sha256: String,
    size: u64,
}

fn load_record(profile_dir: &Path) -> Option<Record> {
    serde_json::from_str(&std::fs::read_to_string(profile_dir.join(REMOTE_RECORD)).ok()?).ok()
}

fn save_record(profile_dir: &Path, record: &Record) -> Result<(), String> {
    let text = serde_json::to_string_pretty(record).map_err(|e| e.to_string())? + "\n";
    crate::profiles::write_atomic(&profile_dir.join(REMOTE_RECORD), &text)
}

/// What a sync did.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct Synced {
    pub downloaded: usize,
    pub removed: usize,
}

fn target(profile_dir: &Path, rel: &str) -> std::path::PathBuf {
    profile_dir.join(FILES_DIR).join(rel)
}

/// What a [`sync`] is doing, for a progress display.
#[derive(Debug, Clone, Copy)]
pub enum SyncEvent<'a> {
    /// Before anything downloads: `files` of the list's files are new or
    /// changed (`bytes` in all, when the list gives every size), `kept` are
    /// already up to date, and `remove` were dropped from the list.
    Plan { list: &'a FileList, files: usize, bytes: Option<u64>, kept: usize, remove: usize },
    /// `path` starts downloading; `size` is from the list, if it says.
    File { path: &'a str, size: Option<u64> },
    /// `done` bytes of that file have arrived, of `total` (the list's size,
    /// else the one the server announced).
    Bytes { done: u64, total: Option<u64> },
    /// It arrived whole and matches its SHA-256.
    Verified { path: &'a str, sha256: &'a str, size: u64 },
    /// `path` was dropped from the list, and deleted.
    Removed { path: &'a str },
}

/// A [`sync`] progress callback that ignores everything.
pub fn quiet(_: SyncEvent<'_>) -> Result<(), String> {
    Ok(())
}

/// Brings a profile's `files/` in line with the list: downloads what's new or
/// changed, deletes what the list dropped. Files the list never named (say,
/// the launcher's own `address.cfg`) are left alone. `progress` hears what
/// happens; an error from it stops the sync with that error, keeping the
/// files that finished downloading.
pub fn sync(
    profile_dir: &Path,
    list: &FileList,
    progress: &mut dyn FnMut(SyncEvent<'_>) -> Result<(), String>,
) -> Result<Synced, String> {
    let mut record = load_record(profile_dir).unwrap_or_default();
    record.source = list.source.clone();
    let mut done = Synced::default();
    let result = (|| {
        let mut wanted = Vec::new();
        for e in &list.entries {
            let path = target(profile_dir, &e.path);
            let size_now = std::fs::metadata(&path).map(|m| m.len()).ok();
            let unchanged = record.files.get(&e.path).is_some_and(|r| r.sha256 == e.sha256 && Some(r.size) == size_now);
            if unchanged {
                continue;
            }
            if let Some(size) = size_now.filter(|_| crate::packs::sha256_file(&path).is_ok_and(|h| h == e.sha256)) {
                record.files.insert(e.path.clone(), Recorded { sha256: e.sha256.clone(), size });
                continue;
            }
            wanted.push(e);
        }
        let listed: std::collections::HashSet<&str> = list.entries.iter().map(|e| e.path.as_str()).collect();
        let dropped: Vec<String> = record.files.keys().filter(|p| !listed.contains(p.as_str())).cloned().collect();
        progress(SyncEvent::Plan {
            list,
            files: wanted.len(),
            bytes: wanted.iter().map(|e| e.size).sum(),
            kept: list.entries.len() - wanted.len(),
            remove: dropped.len(),
        })?;
        for e in wanted {
            progress(SyncEvent::File { path: &e.path, size: e.size })?;
            let path = target(profile_dir, &e.path);
            let dir = path.parent().unwrap();
            std::fs::create_dir_all(dir).map_err(|err| format!("{}: {err}", dir.display()))?;
            let tmp = path.with_file_name(format!(".{}.download", path.file_name().unwrap().to_string_lossy()));
            let mut hasher = Sha256::new();
            let fetched = fetch_with(&e.url, &tmp, e.size.unwrap_or(MAX_FILE_BYTES), Some(&mut hasher), &mut |got, announced| {
                progress(SyncEvent::Bytes { done: got, total: e.size.or(announced) })
            });
            let size = std::fs::metadata(&tmp).map(|m| m.len()).unwrap_or(0);
            let ok = fetched.and_then(|()| {
                if hex(&hasher.finalize()) != e.sha256 || e.size.is_some_and(|s| s != size) {
                    Err(format!("{}: the download does not match the file list", e.path))
                } else {
                    Ok(())
                }
            });
            if let Err(err) = ok {
                let _ = std::fs::remove_file(&tmp);
                return Err(err);
            }
            std::fs::rename(&tmp, &path).map_err(|err| format!("{}: {err}", path.display()))?;
            record.files.insert(e.path.clone(), Recorded { sha256: e.sha256.clone(), size });
            done.downloaded += 1;
            progress(SyncEvent::Verified { path: &e.path, sha256: &e.sha256, size })?;
        }
        for rel in dropped {
            let removed = match std::fs::remove_file(target(profile_dir, &rel)) {
                Ok(()) => true,
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => false,
                Err(e) => return Err(format!("{rel}: {e}")),
            };
            record.files.remove(&rel);
            if removed {
                done.removed += 1;
                progress(SyncEvent::Removed { path: &rel })?;
            }
        }
        Ok(())
    })();
    // Keep what did download, even when a later file failed.
    save_record(profile_dir, &record)?;
    result.map(|()| done)
}

/// Whether every file from the last sync is still there, at its size.
fn intact(profile_dir: &Path) -> bool {
    load_record(profile_dir).is_some_and(|r| {
        !r.files.is_empty()
            && r.files
                .iter()
                .all(|(rel, f)| std::fs::metadata(target(profile_dir, rel)).is_ok_and(|m| m.len() == f.size))
    })
}

/// Before a launch: refreshes a `files_from` pack's files. If the list can't
/// be reached but the last download is intact, the game starts with those
/// (and the returned note says so); a list that fails its checks stops it.
/// `progress` is as for [`sync`].
pub fn launch_sync(
    profile_dir: &Path,
    pack: &sc_profile::Pack,
    key: Option<&PublicKey>,
    progress: &mut dyn FnMut(SyncEvent<'_>) -> Result<(), String>,
) -> Result<Option<String>, String> {
    let Some(from) = &pack.files_from else { return Ok(None) };
    match fetch_list(from, key) {
        Ok(list) => {
            let done = sync(profile_dir, &list, progress)?;
            Ok((done.downloaded + done.removed > 0).then(|| {
                format!("{}: updated {} game file(s) from {}", pack.name, done.downloaded + done.removed, list.host())
            }))
        }
        Err(ListError::Network(e)) if intact(profile_dir) => {
            Ok(Some(format!("Could not check {}'s game files ({e}); using the ones from last time.", pack.name)))
        }
        Err(e) => Err(format!("{}'s game files: {e}", pack.name)),
    }
}

/// Forgets which files a profile downloaded (when a pack stops using a list).
pub fn forget(profile_dir: &Path) {
    let _ = std::fs::remove_file(profile_dir.join(REMOTE_RECORD));
}

#[cfg(test)]
mod tests {
    use super::*;

    const HASH: &str = "2c26b46b68ffc68ff99b453c1d30413413422d706483bfa0f98a5e886266e7ae";

    #[test]
    fn reads_scmod1() {
        let text = format!(
            "\u{feff}SCMOD1\r\n# comment\r\ndata\\ui\\x.lua\t{}\thttps://a.org/x.lua\r\n\r\n",
            HASH.to_uppercase()
        );
        let entries = parse_scmod1(&text).unwrap();
        assert_eq!(entries, vec![Entry { path: "data/ui/x.lua".into(), sha256: HASH.into(), size: None, url: "https://a.org/x.lua".into() }]);
        assert!(parse_scmod1("SCMOD2\n").is_err());
        assert!(parse_scmod1(&format!("SCMOD1\n../x\t{HASH}\thttps://a.org/x")).is_err());
        assert!(parse_scmod1(&format!("SCMOD1\ndata/x {HASH} https://a.org/x")).is_err());
        assert!(parse_scmod1(&format!("SCMOD1\ndata/address.cfg\t{HASH}\thttps://a.org/x")).is_err());
    }

    #[test]
    fn reads_json_lists() {
        let json = format!(
            r#"{{"version":"v2","files":{{"data/gamedata/a b.lua":{{"sha256":"{HASH}","size":3}}}}}}"#
        );
        let entries = parse_json(json.as_bytes(), "https://cdn.org/starfury/v2.json?x=1").unwrap();
        assert_eq!(entries[0].url, "https://cdn.org/starfury/data/gamedata/a%20b.lua");
        assert_eq!(entries[0].size, Some(3));
        let based = format!(r#"{{"base_url":"https://b.org/files/","files":{{"x":{{"sha256":"{HASH}"}}}}}}"#);
        assert_eq!(parse_json(based.as_bytes(), "https://a.org/l.json").unwrap()[0].url, "https://b.org/files/x");
        assert!(parse_json(br#"{"files":{"x":{"sha256":"nope"}}}"#, "https://a.org/l.json").is_err());
    }

    type Files = std::sync::Arc<std::sync::Mutex<Vec<(String, Vec<u8>)>>>;

    /// Serves files over plain HTTP on localhost for one test.
    fn serve() -> (String, Files) {
        let files: Files = Default::default();
        let served = files.clone();
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let base = format!("http://{}", listener.local_addr().unwrap());
        std::thread::spawn(move || {
            use std::io::{BufRead, Write};
            for stream in listener.incoming().flatten() {
                let mut reader = std::io::BufReader::new(&stream);
                let mut line = String::new();
                if reader.read_line(&mut line).is_err() {
                    continue;
                }
                let path = line.split(' ').nth(1).unwrap_or("").to_string();
                loop {
                    let mut h = String::new();
                    if reader.read_line(&mut h).map_or(true, |n| n == 0) || h == "\r\n" {
                        break;
                    }
                }
                let body = served.lock().unwrap().iter().find(|(p, _)| *p == path).map(|(_, b)| b.clone());
                let mut out = &stream;
                let _ = match body {
                    Some(b) => write!(out, "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", b.len())
                        .and_then(|()| out.write_all(&b)),
                    None => write!(out, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"),
                };
            }
        });
        (base, files)
    }

    #[test]
    fn syncs_only_changes_and_checks_signatures() {
        crate::test_data_dir();
        let key = crate::signing::SecretKey::generate().unwrap();
        let (base, files) = serve();
        let sha = |b: &[u8]| hex(&Sha256::digest(b));
        let put = |path: &str, body: &[u8]| files.lock().unwrap().push((path.to_string(), body.to_vec()));
        let list = |name: &str, body: String, signed: bool| {
            if signed {
                put(&format!("/{name}.sig"), crate::signing::sign_bytes(&key, body.as_bytes()).as_bytes());
            }
            put(&format!("/{name}"), body.as_bytes());
        };
        for (path, body) in [("/a1", "one"), ("/a2", "two"), ("/b", "bee")] {
            put(path, body.as_bytes());
        }
        let v1 = format!("SCMOD1\ndata/a.lua\t{}\t{base}/a1\ndata/b.lua\t{}\t{base}/b\n", sha(b"one"), sha(b"bee"));
        list("v1.txt", v1.clone(), true);
        list("unsigned.txt", v1, false);
        list("v2.txt", format!("SCMOD1\ndata/a.lua\t{}\t{base}/a2\n", sha(b"two")), false);
        list("bad.txt", format!("SCMOD1\ndata/a.lua\t{}\t{base}/a1\n", sha(b"other")), true);
        let from = |name: &str| FilesFrom { scmod1: Some(format!("{base}/{name}")), manifest: None };
        let profile = std::env::temp_dir().join(format!("sf-remote-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&profile);
        std::fs::create_dir_all(&profile).unwrap();

        // Unsigned packs take any list; signed ones need their key's signature.
        assert!(fetch_list(&from("unsigned.txt"), None).is_ok());
        assert!(matches!(fetch_list(&from("unsigned.txt"), Some(&key.public())), Err(ListError::Invalid(_))));
        let other = crate::signing::SecretKey::generate().unwrap();
        assert!(matches!(fetch_list(&from("v1.txt"), Some(&other.public())), Err(ListError::Invalid(_))));
        assert!(matches!(fetch_list(&from("missing.txt"), None), Err(ListError::Network(_))));

        // Syncs, and what it told the progress callback.
        let synced = |list: &FileList| {
            let mut seen: Vec<String> = Vec::new();
            let result = sync(&profile, list, &mut |e| {
                let line = match e {
                    SyncEvent::Plan { files, bytes, kept, remove, .. } => format!("plan {files} {bytes:?} {kept} {remove}"),
                    SyncEvent::File { path, size } => format!("get {path} {size:?}"),
                    SyncEvent::Bytes { done, total } => format!("bytes {done} {total:?}"),
                    SyncEvent::Verified { path, size, .. } => format!("ok {path} {size}"),
                    SyncEvent::Removed { path } => format!("del {path}"),
                };
                // A file may arrive in several chunks; keep the last count.
                if line.starts_with("bytes") && seen.last().is_some_and(|l| l.starts_with("bytes")) {
                    seen.pop();
                }
                seen.push(line);
                Ok(())
            });
            (result, seen)
        };

        let list = fetch_list(&from("v1.txt"), Some(&key.public())).unwrap();
        assert!(list.signed);
        // SCMOD1 gives no sizes, so the total comes from the server.
        assert_eq!(
            synced(&list),
            (
                Ok(Synced { downloaded: 2, removed: 0 }),
                ["plan 2 None 0 0", "get data/a.lua None", "bytes 3 Some(3)", "ok data/a.lua 3", "get data/b.lua None", "bytes 3 Some(3)", "ok data/b.lua 3"]
                    .map(String::from)
                    .to_vec()
            )
        );
        assert_eq!(std::fs::read(profile.join("files/data/a.lua")).unwrap(), b"one");
        assert_eq!(synced(&list), (Ok(Synced::default()), vec!["plan 0 Some(0) 2 0".into()]), "nothing changed, nothing fetched");
        assert!(intact(&profile));

        let list = fetch_list(&from("v2.txt"), None).unwrap();
        assert_eq!(
            synced(&list),
            (
                Ok(Synced { downloaded: 1, removed: 1 }),
                ["plan 1 None 0 1", "get data/a.lua None", "bytes 3 Some(3)", "ok data/a.lua 3", "del data/b.lua"].map(String::from).to_vec()
            )
        );
        assert_eq!(std::fs::read(profile.join("files/data/a.lua")).unwrap(), b"two");
        assert!(!profile.join("files/data/b.lua").exists());

        // The progress callback can stop a sync mid-file (the window's Cancel).
        let list = fetch_list(&from("v1.txt"), Some(&key.public())).unwrap();
        let stopped = sync(&profile, &list, &mut |e| match e {
            SyncEvent::Bytes { .. } => Err("stopped".into()),
            _ => Ok(()),
        });
        assert_eq!(stopped, Err("stopped".into()));
        assert_eq!(std::fs::read(profile.join("files/data/a.lua")).unwrap(), b"two", "the old file stays");
        assert!(!profile.join("files/data/.a.lua.download").exists());

        let list = fetch_list(&from("bad.txt"), Some(&key.public())).unwrap();
        assert!(sync(&profile, &list, &mut quiet).is_err(), "a file that doesn't match its hash is refused");
        assert_eq!(std::fs::read(profile.join("files/data/a.lua")).unwrap(), b"two", "and the old one is kept");
        std::fs::remove_dir_all(&profile).unwrap();
    }
}
