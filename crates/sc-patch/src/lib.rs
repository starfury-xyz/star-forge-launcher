//! Patchsets: byte patches for one executable or DLL, in the format the STAR
//! FORGE Patcher (`patcher/`, see its docs/FORMAT.md) builds for the server
//! binaries. Packs ship them in `patches/` for the game client.
//!
//! This crate only knows the format and how to check and apply a patchset
//! against an image. *How* that image is reached is up to the patch method:
//! the agent DLL patches the running game in memory ([`Target`] over process
//! memory), the launcher checks the file on disk before starting the game
//! ([`FileImage`]), and a method that writes a patched copy could use
//! [`FileImage`] too.

pub mod core;

use serde::Deserialize;
use std::collections::HashMap;
use std::fmt;

/// The resolved patchset format this crate reads (`format = 1`).
pub const FORMAT: i64 = 1;
/// File name suffix of a patchset: `<exe>.patchset.toml`.
pub const SUFFIX: &str = ".patchset.toml";

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Patchset {
    /// Module the patchset targets, e.g. `game.exe`.
    pub exe: String,
    /// SHA-1s (lowercase hex) of the pristine builds the addresses hold for.
    /// `sha1` in the file is one string or a list of them.
    pub sha1: Vec<String>,
    pub build: String,
    /// Image base the virtual addresses assume. Images that load elsewhere
    /// are patched at the same offset from their actual base.
    pub image_base: u64,
    pub patches: Vec<Patch>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Patch {
    pub id: String,
    pub title: String,
    pub description: String,
    pub default: bool,
    /// A verification failure skips this patch instead of failing the set.
    pub optional: bool,
    pub writes: Vec<Write>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Write {
    pub va: u64,
    /// Bytes that must be there before patching.
    pub orig: Vec<u8>,
    pub kind: WriteKind,
    pub comment: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum WriteKind {
    /// Replacement bytes, same length as `orig`.
    Bytes(Vec<u8>),
    /// A native mid-function hook by name; `orig` are the stolen bytes.
    Midhook(String),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error(pub String);

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Error {}

fn err<T>(msg: impl Into<String>) -> Result<T, Error> {
    Err(Error(msg.into()))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RawSet {
    format: i64,
    exe: String,
    sha1: RawSha1,
    #[serde(default)]
    build: String,
    #[serde(default)]
    image_base: i64,
    #[serde(default)]
    patch: Vec<RawPatch>,
}

/// `sha1 = "..."` or `sha1 = ["...", "..."]`.
#[derive(Deserialize)]
#[serde(untagged)]
enum RawSha1 {
    One(String),
    Many(Vec<String>),
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RawPatch {
    id: String,
    #[serde(default)]
    title: String,
    #[serde(default)]
    description: String,
    #[serde(default = "yes")]
    default: bool,
    #[serde(default)]
    optional: bool,
    #[serde(default)]
    write: Vec<RawWrite>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RawWrite {
    va: i64,
    orig: String,
    bytes: Option<String>,
    midhook: Option<String>,
    #[serde(default)]
    comment: String,
}

fn yes() -> bool {
    true
}

/// Hex bytes; spaces and `_` are allowed between digits.
pub fn parse_hex(text: &str) -> Option<Vec<u8>> {
    let digits: Vec<u8> = text.bytes().filter(|b| !b.is_ascii_whitespace() && *b != b'_').collect();
    if !digits.len().is_multiple_of(2) {
        return None;
    }
    digits
        .chunks(2)
        .map(|pair| u8::from_str_radix(std::str::from_utf8(pair).ok()?, 16).ok())
        .collect()
}

pub fn to_hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

/// A module name a patchset may target: a plain file name.
pub fn valid_module(name: &str) -> bool {
    !name.is_empty() && !name.contains(['/', '\\', ':']) && name != "." && name != ".."
}

impl Patchset {
    /// Parses a resolved `<exe>.patchset.toml`; `name` is used in errors.
    pub fn parse(text: &str, name: &str) -> Result<Self, Error> {
        let raw: RawSet = toml::from_str(text).map_err(|e| Error(format!("{name}: {e}")))?;
        if raw.format != FORMAT {
            return err(format!("{name}: patchset format {} is not supported (this launcher reads {FORMAT})", raw.format));
        }
        if !valid_module(&raw.exe) {
            return err(format!("{name}: exe must be a file name, got {:?}", raw.exe));
        }
        let sha1: Vec<String> = match raw.sha1 {
            RawSha1::One(s) => vec![s],
            RawSha1::Many(v) => v,
        };
        if sha1.is_empty() {
            return err(format!("{name}: sha1 needs at least one hash"));
        }
        if sha1.iter().any(|s| s.len() != 40 || !s.bytes().all(|b| b.is_ascii_hexdigit())) {
            return err(format!("{name}: sha1 must be 40 hex digits"));
        }
        let sha1 = sha1.iter().map(|s| s.to_ascii_lowercase()).collect();
        let mut patches = Vec::new();
        for p in raw.patch {
            let at = format!("{name}: patch {:?}", p.id);
            if p.id.is_empty() {
                return err(format!("{name}: patch without id"));
            }
            if patches.iter().any(|q: &Patch| q.id == p.id) {
                return err(format!("{at}: duplicate id"));
            }
            if p.write.is_empty() {
                return err(format!("{at}: no writes"));
            }
            let mut writes = Vec::new();
            for w in p.write {
                let here = format!("{at} at {:#x}", w.va);
                let orig = parse_hex(&w.orig).ok_or_else(|| Error(format!("{here}: orig is not valid hex")))?;
                if w.va <= 0 || orig.is_empty() {
                    return err(format!("{here}: a write needs va and orig"));
                }
                let kind = match (w.bytes, w.midhook) {
                    (Some(b), None) => {
                        let bytes = parse_hex(&b).ok_or_else(|| Error(format!("{here}: bytes is not valid hex")))?;
                        if bytes.len() != orig.len() {
                            return err(format!("{here}: orig and bytes differ in length"));
                        }
                        WriteKind::Bytes(bytes)
                    }
                    (None, Some(m)) if !m.is_empty() && orig.len() >= 5 => WriteKind::Midhook(m),
                    (None, Some(_)) => return err(format!("{here}: a midhook needs a name and at least 5 orig bytes")),
                    _ => return err(format!("{here}: exactly one of bytes / midhook is required")),
                };
                writes.push(Write { va: w.va as u64, orig, kind, comment: w.comment });
            }
            patches.push(Patch {
                id: p.id,
                title: p.title,
                description: p.description,
                default: p.default,
                optional: p.optional,
                writes,
            });
        }
        Ok(Patchset {
            exe: raw.exe,
            sha1,
            build: raw.build,
            image_base: raw.image_base as u64,
            patches,
        })
    }

    /// Whether `sha` (lowercase hex, as [`sha1_hex`] gives it) is a build this set was made for.
    pub fn accepts(&self, sha: &str) -> bool {
        self.sha1.iter().any(|s| s == sha)
    }

    /// The accepted hashes, for messages.
    pub fn expected_sha1(&self) -> String {
        self.sha1.join(" or ")
    }

    /// Whether this set targets the module `name` (case-insensitive, like Windows).
    pub fn targets(&self, name: &str) -> bool {
        self.exe.eq_ignore_ascii_case(name)
    }

    /// Patches that are on, given the player's `id -> on/off` choices.
    /// Unknown ids in `overrides` are returned as warnings.
    pub fn enabled<'a>(&'a self, overrides: Option<&HashMap<String, bool>>) -> (Vec<&'a Patch>, Vec<String>) {
        let mut warnings = Vec::new();
        if let Some(o) = overrides {
            for id in o.keys() {
                if !self.patches.iter().any(|p| &p.id == id) {
                    warnings.push(format!("{}: no patch called {id:?}", self.exe));
                }
            }
        }
        let on = self
            .patches
            .iter()
            .filter(|p| overrides.and_then(|o| o.get(&p.id)).copied().unwrap_or(p.default))
            .collect();
        (on, warnings)
    }
}

/// One message per pair of patches that write the same bytes.
pub fn overlaps(patches: &[&Patch]) -> Vec<String> {
    let mut ranges: Vec<(u64, u64, &str)> = patches
        .iter()
        .flat_map(|p| p.writes.iter().map(|w| (w.va, w.va + w.orig.len() as u64, p.id.as_str())))
        .collect();
    ranges.sort();
    let mut out = Vec::new();
    for (i, a) in ranges.iter().enumerate() {
        for b in ranges[i + 1..].iter().take_while(|b| b.0 < a.1) {
            out.push(format!("{:?} and {:?} both write {:#x}..{:#x}", a.2, b.2, b.0, a.1.min(b.1)));
        }
    }
    out
}

/// An image patches are checked against and written to.
pub trait Target {
    /// Reads `out.len()` bytes at `va` (in the patchset's address space).
    fn read(&mut self, va: u64, out: &mut [u8]) -> bool;
    fn write(&mut self, va: u64, data: &[u8]) -> bool;
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum State {
    /// Every write's original bytes are there.
    Pristine,
    /// Every write is already in place.
    Applied,
    /// Neither; the reason says where.
    Mismatch(String),
}

pub fn check(t: &mut dyn Target, p: &Patch) -> State {
    let (mut all_orig, mut all_new) = (true, true);
    for w in &p.writes {
        let mut cur = vec![0; w.orig.len()];
        if !t.read(w.va, &mut cur) {
            return State::Mismatch(format!("cannot read {:#x} (+{})", w.va, cur.len()));
        }
        let is_orig = cur == w.orig;
        let is_new = match &w.kind {
            WriteKind::Bytes(b) => &cur == b,
            WriteKind::Midhook(_) => cur[0] == 0xe9,
        };
        if !is_orig && !is_new {
            return State::Mismatch(format!(
                "bytes at {:#x} are {}, expected {}",
                w.va,
                to_hex(&cur),
                to_hex(&w.orig)
            ));
        }
        all_orig &= is_orig;
        all_new &= is_new;
    }
    match (all_orig, all_new) {
        (true, _) => State::Pristine,
        (_, true) => State::Applied,
        _ => State::Mismatch("partly applied".into()),
    }
}

/// Installs a midhook write; methods that can't run native code pass `None`.
pub type MidhookInstaller<'a> = Option<&'a mut dyn FnMut(&Write, &str) -> Result<(), String>>;

/// Applies a patch whose state is [`State::Pristine`], re-reading every write.
pub fn apply(t: &mut dyn Target, p: &Patch, mut midhook: MidhookInstaller) -> Result<(), String> {
    for w in &p.writes {
        match &w.kind {
            WriteKind::Bytes(bytes) => {
                if !t.write(w.va, bytes) {
                    return Err(format!("write failed at {:#x}", w.va));
                }
                let mut back = vec![0; bytes.len()];
                if !t.read(w.va, &mut back) || &back != bytes {
                    return Err(format!("readback mismatch at {:#x}", w.va));
                }
            }
            WriteKind::Midhook(name) => match midhook.as_mut() {
                Some(install) => install(w, name)?,
                None => return Err(format!("midhook {name:?} is not supported by this patch method")),
            },
        }
    }
    Ok(())
}

/// Lower-case hex SHA-1, as patchsets record it.
pub fn sha1_hex(data: &[u8]) -> String {
    use sha1::{Digest, Sha1};
    to_hex(&Sha1::digest(data))
}

/// A PE file on disk, addressed by virtual address. Good for checking a
/// patchset before launch, or for writing a patched copy.
pub struct FileImage {
    pub data: Vec<u8>,
    pub image_base: u64,
    /// (rva, virtual size, file offset, raw size)
    sections: Vec<(u64, u64, u64, u64)>,
}

impl FileImage {
    pub fn parse(data: Vec<u8>) -> Result<Self, Error> {
        let u16_at = |o: usize| data.get(o..o + 2).map(|b| u16::from_le_bytes([b[0], b[1]]) as u64);
        let u32_at = |o: usize| data.get(o..o + 4).map(|b| u32::from_le_bytes(b.try_into().unwrap()) as u64);
        let u64_at = |o: usize| data.get(o..o + 8).map(|b| u64::from_le_bytes(b.try_into().unwrap()));
        let bad = || Error("not a valid PE file".into());
        if data.get(..2) != Some(b"MZ") {
            return Err(bad());
        }
        let nt = u32_at(0x3c).ok_or_else(bad)? as usize;
        if data.get(nt..nt + 4) != Some(b"PE\0\0") {
            return Err(bad());
        }
        let sections_n = u16_at(nt + 6).ok_or_else(bad)? as usize;
        let opt_size = u16_at(nt + 20).ok_or_else(bad)? as usize;
        let opt = nt + 24;
        let image_base = match u16_at(opt).ok_or_else(bad)? {
            0x10b => u32_at(opt + 28).ok_or_else(bad)?,
            0x20b => u64_at(opt + 24).ok_or_else(bad)?,
            _ => return Err(bad()),
        };
        let table = opt + opt_size;
        let mut sections = Vec::new();
        for i in 0..sections_n {
            let s = table + i * 40;
            let vsize = u32_at(s + 8).ok_or_else(bad)?;
            let rva = u32_at(s + 12).ok_or_else(bad)?;
            let raw_size = u32_at(s + 16).ok_or_else(bad)?;
            let raw = u32_at(s + 20).ok_or_else(bad)?;
            sections.push((rva, vsize.max(raw_size), raw, raw_size));
        }
        Ok(FileImage { data, image_base, sections })
    }

    /// File offset of `n` bytes at `va`, if they are all backed by the file.
    fn offset(&self, va: u64, n: usize) -> Option<usize> {
        let rva = va.checked_sub(self.image_base)?;
        let (s_rva, _, raw, raw_size) = self.sections.iter().find(|(r, v, ..)| rva >= *r && rva < r + v)?;
        let into = rva - s_rva;
        (into + n as u64 <= *raw_size).then(|| (raw + into) as usize).filter(|o| o + n <= self.data.len())
    }
}

impl Target for FileImage {
    fn read(&mut self, va: u64, out: &mut [u8]) -> bool {
        match self.offset(va, out.len()) {
            Some(o) => {
                out.copy_from_slice(&self.data[o..o + out.len()]);
                true
            }
            None => false,
        }
    }

    fn write(&mut self, va: u64, data: &[u8]) -> bool {
        match self.offset(va, data.len()) {
            Some(o) => {
                self.data[o..o + data.len()].copy_from_slice(data);
                true
            }
            None => false,
        }
    }
}

/// What checking a patchset against a file found.
#[derive(Debug, Default)]
pub struct Report {
    /// Patches that would apply.
    pub ok: Vec<String>,
    /// Optional patches that would be skipped, with the reason.
    pub skipped: Vec<String>,
    /// Problems that stop the set from applying.
    pub errors: Vec<String>,
    pub warnings: Vec<String>,
}

/// Checks the enabled patches of `set` against the file `data`, the way a
/// loader would before applying them.
pub fn verify_file(set: &Patchset, data: Vec<u8>, overrides: Option<&HashMap<String, bool>>) -> Report {
    let mut r = Report::default();
    let sha = sha1_hex(&data);
    if !set.accepts(&sha) {
        r.errors.push(format!(
            "{} is a different build than the patches were made for (sha1 {sha}, expected {})",
            set.exe,
            set.expected_sha1()
        ));
        return r;
    }
    let mut image = match FileImage::parse(data) {
        Ok(i) => i,
        Err(e) => {
            r.errors.push(format!("{}: {e}", set.exe));
            return r;
        }
    };
    // Addresses are relative to the set's image base; the file's own base is the same build.
    image.image_base = set.image_base;
    let (on, warnings) = set.enabled(overrides);
    r.warnings = warnings;
    r.errors.extend(overlaps(&on));
    for p in on {
        match check(&mut image, p) {
            State::Pristine => r.ok.push(p.id.clone()),
            State::Applied => r.errors.push(format!("{}: {} is already patched on disk", set.exe, p.id)),
            State::Mismatch(why) if p.optional => r.skipped.push(format!("{}: {why}", p.id)),
            State::Mismatch(why) => r.errors.push(format!("{}: {}: {why}", set.exe, p.id)),
        }
    }
    r
}

#[cfg(test)]
mod tests {
    use super::*;

    const SET: &str = r#"
format = 1
exe = "game.exe"
sha1 = "0000000000000000000000000000000000000000"
image_base = 0x140000000

[[patch]]
id = "one"
title = "Nop a check"
[[patch.write]]
va = 0x140001002
orig = "74 05"
bytes = "90 90"

[[patch]]
id = "two"
default = false
optional = true
[[patch.write]]
va = 0x140001003
orig = "05aa"
bytes = "0000"
"#;

    /// A minimal PE32+ with one section: rva 0x1000, file offset 0x200, 0x10 bytes.
    fn pe() -> Vec<u8> {
        let mut d = vec![0u8; 0x210];
        d[..2].copy_from_slice(b"MZ");
        d[0x3c] = 0x40;
        d[0x40..0x44].copy_from_slice(b"PE\0\0");
        d[0x46] = 1; // sections
        d[0x54] = 0xf0; // optional header size
        d[0x58..0x5a].copy_from_slice(&0x20bu16.to_le_bytes());
        d[0x70..0x78].copy_from_slice(&0x140000000u64.to_le_bytes());
        let s = 0x58 + 0xf0;
        d[s + 8..s + 12].copy_from_slice(&0x10u32.to_le_bytes());
        d[s + 12..s + 16].copy_from_slice(&0x1000u32.to_le_bytes());
        d[s + 16..s + 20].copy_from_slice(&0x10u32.to_le_bytes());
        d[s + 20..s + 24].copy_from_slice(&0x200u32.to_le_bytes());
        d[0x202..0x205].copy_from_slice(&[0x74, 0x05, 0xaa]);
        d
    }

    #[test]
    fn parses_and_selects() {
        let set = Patchset::parse(SET, "t").unwrap();
        assert_eq!(set.image_base, 0x140000000);
        assert!(set.targets("GAME.EXE"));
        let (on, _) = set.enabled(None);
        assert_eq!(on.len(), 1);
        let choice = HashMap::from([("two".to_string(), true), ("nope".to_string(), true)]);
        let (on, warnings) = set.enabled(Some(&choice));
        assert_eq!(on.len(), 2);
        assert_eq!(warnings.len(), 1);
        assert_eq!(overlaps(&on).len(), 1);
    }

    #[test]
    fn rejects_bad_sets() {
        let bad = |from: &str, to: &str| Patchset::parse(&SET.replace(from, to), "t").is_err();
        assert!(bad("format = 1", "format = 2"));
        assert!(bad("\"game.exe\"", "\"..\\\\game.exe\""));
        assert!(bad("bytes = \"90 90\"", "bytes = \"90\""));
        assert!(bad("bytes = \"90 90\"", "midhook = \"x\""));
        assert!(bad("id = \"two\"", "id = \"one\""));
    }

    #[test]
    fn checks_and_applies_a_file() {
        let data = pe();
        let mut set = Patchset::parse(SET, "t").unwrap();
        set.sha1 = vec![sha1_hex(&data)];
        let r = verify_file(&set, data.clone(), None);
        assert_eq!(r.ok, ["one"]);
        assert!(r.errors.is_empty());

        let mut image = FileImage::parse(data.clone()).unwrap();
        let p = &set.patches[0];
        assert_eq!(check(&mut image, p), State::Pristine);
        apply(&mut image, p, None).unwrap();
        assert_eq!(check(&mut image, p), State::Applied);
        assert_eq!(&image.data[0x202..0x204], &[0x90, 0x90]);
        assert!(!image.read(0x140001010, &mut [0; 1]), "past the section");

        // Optional patch that doesn't match is skipped; a wrong build is an error.
        let mut tweaked = data.clone();
        tweaked[0x204] = 0x11;
        set.sha1 = vec![sha1_hex(&tweaked)];
        let r = verify_file(&set, tweaked, Some(&HashMap::from([("two".into(), true)])));
        assert_eq!(r.skipped.len(), 1);
        assert!(verify_file(&set, data.clone(), None).errors[0].contains("different build"));

        // A set can list several builds; any of them is accepted.
        set.sha1 = vec!["0".repeat(40), sha1_hex(&data)];
        assert!(verify_file(&set, data, None).errors.is_empty());
    }

    #[test]
    fn sha1_is_one_or_a_list() {
        let one = Patchset::parse(SET, "t").unwrap();
        assert_eq!(one.sha1, ["0".repeat(40)]);
        let list = SET.replace(
            "sha1 = \"0000000000000000000000000000000000000000\"",
            "sha1 = [\"0000000000000000000000000000000000000000\", \"ABCDEF0123456789abcdef0123456789abcdef01\"]",
        );
        let many = Patchset::parse(&list, "t").unwrap();
        assert!(many.accepts("abcdef0123456789abcdef0123456789abcdef01"));
        assert!(many.accepts(&"0".repeat(40)));
        assert!(!many.accepts(&"1".repeat(40)));
        let bad = |to: &str| Patchset::parse(&SET.replace("sha1 = \"0000000000000000000000000000000000000000\"", to), "t").is_err();
        assert!(bad("sha1 = []"));
        assert!(bad("sha1 = [\"0000\"]"));
    }
}
