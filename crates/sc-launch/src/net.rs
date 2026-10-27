//! Downloads: one shared HTTP agent, retries for passing network trouble, and
//! errors a player can read (the technical detail goes to stderr and the log).

use sha2::{Digest, Sha256};
use std::io::{Read, Write};
use std::path::Path;
use std::sync::OnceLock;
use std::time::Duration;

/// Tries per download: the first plus two retries.
const ATTEMPTS: u32 = 3;
/// No data for this long counts as a stall (and is retried).
const STALL: Duration = Duration::from_secs(20);

/// Why a download failed, as far as a player cares.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Kind {
    /// No connection: offline, DNS, refused, proxy.
    Offline,
    /// Connected, then nothing arrived for a while.
    Timeout,
    /// The server says there's no such file (404, 410).
    NotFound,
    /// Another HTTP error status.
    Status(u16),
    /// The secure connection couldn't be set up (certificate, TLS).
    Tls,
    /// Bigger than allowed or expected.
    TooLarge,
    /// Arrived, but doesn't match its checksum or size.
    Corrupt,
    /// Saving it on this computer failed.
    Disk,
    /// Stopped by the caller (say, the player cancelled).
    Stopped,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FetchError {
    pub kind: Kind,
    /// What a player sees.
    pub message: String,
}

impl FetchError {
    fn new(kind: Kind, url: &str, detail: impl std::fmt::Display) -> FetchError {
        let host = host(url);
        let message = match kind {
            Kind::Offline => format!("Can't reach {host}. Check your internet connection."),
            Kind::Timeout => format!("{host} stopped responding."),
            Kind::NotFound => format!("{host} doesn't have {}.", file_name(url)),
            Kind::Status(code) if code >= 500 => format!("{host} had a problem (error {code}). Try again later."),
            Kind::Status(code) => format!("{host} refused the download (error {code})."),
            Kind::Tls => format!("Couldn't make a secure connection to {host}."),
            Kind::TooLarge => format!("{} from {host} is larger than allowed.", file_name(url)),
            Kind::Corrupt => format!("{} from {host} arrived damaged.", file_name(url)),
            Kind::Disk => format!("Couldn't save {}: {detail}", file_name(url)),
            Kind::Stopped => detail.to_string(),
        };
        if kind != Kind::Stopped {
            eprintln!("download {url}: {detail}");
        }
        FetchError { kind, message }
    }

    /// Worth trying again.
    fn passing(&self) -> bool {
        matches!(self.kind, Kind::Offline | Kind::Timeout) || matches!(self.kind, Kind::Status(c) if c >= 500 || c == 429)
    }
}

impl std::fmt::Display for FetchError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.message)
    }
}

impl From<FetchError> for String {
    fn from(e: FetchError) -> String {
        e.message
    }
}

/// The host of `url`, for messages.
pub fn host(url: &str) -> &str {
    let rest = url.split_once("://").map_or(url, |(_, r)| r);
    rest.split(['/', '?', '#']).next().unwrap_or(rest)
}

fn file_name(url: &str) -> &str {
    let path = url.split(['?', '#']).next().unwrap_or(url);
    path.rsplit('/').find(|s| !s.is_empty()).unwrap_or(path)
}

fn agent(https_only: bool) -> &'static ureq::Agent {
    static AGENTS: [OnceLock<ureq::Agent>; 2] = [OnceLock::new(), OnceLock::new()];
    AGENTS[usize::from(https_only)].get_or_init(|| {
        ureq::AgentBuilder::new()
            .timeout_connect(Duration::from_secs(15))
            .timeout_read(STALL)
            .user_agent(concat!("star-forge-launcher/", env!("CARGO_PKG_VERSION")))
            .https_only(https_only)
            .build()
    })
}

fn classify(url: &str, e: ureq::Error) -> FetchError {
    match e {
        ureq::Error::Status(code, _) if code == 404 || code == 410 => FetchError::new(Kind::NotFound, url, code),
        ureq::Error::Status(code, _) => FetchError::new(Kind::Status(code), url, code),
        ureq::Error::Transport(t) => {
            let text = t.to_string();
            let lower = text.to_ascii_lowercase();
            let kind = if lower.contains("tls") || lower.contains("certificate") || lower.contains("handshake") {
                Kind::Tls
            } else if lower.contains("timed out") || lower.contains("timeout") {
                Kind::Timeout
            } else {
                Kind::Offline
            };
            FetchError::new(kind, url, text)
        }
    }
}

fn read_error(url: &str, e: std::io::Error) -> FetchError {
    let kind = if matches!(e.kind(), std::io::ErrorKind::TimedOut | std::io::ErrorKind::WouldBlock) {
        Kind::Timeout
    } else {
        Kind::Offline
    };
    FetchError::new(kind, url, e)
}

/// Downloads `url` to `dest`, at most `limit` bytes, optionally hashing it.
pub fn fetch(url: &str, dest: &Path, limit: u64, hasher: Option<&mut Sha256>) -> Result<(), FetchError> {
    fetch_with(url, dest, limit, hasher, &mut |_, _| Ok(()))
}

/// [`fetch`], calling `on_bytes` after each chunk with the bytes received so
/// far and the size the server announced, if it did. An error from
/// `on_bytes` stops the download with that error (kind [`Kind::Stopped`]).
/// Passing network trouble is retried twice, from the start; `dest` is
/// flushed to disk before this returns.
pub fn fetch_with(
    url: &str,
    dest: &Path,
    limit: u64,
    mut hasher: Option<&mut Sha256>,
    on_bytes: &mut dyn FnMut(u64, Option<u64>) -> Result<(), String>,
) -> Result<(), FetchError> {
    let mut attempt = 1;
    loop {
        if let Some(h) = hasher.as_deref_mut() {
            *h = Sha256::new();
        }
        match fetch_once(url, dest, limit, hasher.as_deref_mut(), on_bytes) {
            Err(e) if e.passing() && attempt < ATTEMPTS => {
                std::thread::sleep(Duration::from_secs(u64::from(2 * attempt - 1)));
                attempt += 1;
            }
            other => return other,
        }
    }
}

fn fetch_once(
    url: &str,
    dest: &Path,
    limit: u64,
    mut hasher: Option<&mut Sha256>,
    on_bytes: &mut dyn FnMut(u64, Option<u64>) -> Result<(), String>,
) -> Result<(), FetchError> {
    let response = agent(!url.starts_with("http://")).get(url).call().map_err(|e| classify(url, e))?;
    let announced: Option<u64> = response.header("Content-Length").and_then(|v| v.trim().parse().ok());
    if announced.is_some_and(|n| n > limit) {
        return Err(FetchError::new(Kind::TooLarge, url, format!("{} bytes announced", announced.unwrap_or(0))));
    }
    let disk = |e: std::io::Error| FetchError::new(Kind::Disk, url, format!("{}: {e}", dest.display()));
    let mut reader = response.into_reader().take(limit + 1);
    let mut file = std::fs::File::create(dest).map_err(disk)?;
    let mut buf = vec![0u8; 64 * 1024];
    let mut total = 0u64;
    loop {
        let n = reader.read(&mut buf).map_err(|e| read_error(url, e))?;
        if n == 0 {
            break;
        }
        total += n as u64;
        if total > limit {
            return Err(FetchError::new(Kind::TooLarge, url, format!("over {limit} bytes")));
        }
        if let Some(h) = hasher.as_deref_mut() {
            h.update(&buf[..n]);
        }
        file.write_all(&buf[..n]).map_err(disk)?;
        on_bytes(total, announced).map_err(|e| FetchError::new(Kind::Stopped, url, e))?;
    }
    if announced.is_some_and(|n| n != total) {
        return Err(FetchError::new(Kind::Offline, url, format!("connection closed after {total} of {} bytes", announced.unwrap_or(0))));
    }
    file.sync_all().map_err(disk)?;
    Ok(())
}

/// A damaged-download error for `url` (checksum or size mismatch).
pub fn corrupt(url: &str, detail: impl std::fmt::Display) -> FetchError {
    FetchError::new(Kind::Corrupt, url, detail)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names_hosts_and_files() {
        assert_eq!(host("https://a.example/x/y.lua?v=1"), "a.example");
        assert_eq!(file_name("https://a.example/x/y.lua?v=1"), "y.lua");
        assert_eq!(file_name("https://a.example/x/"), "x");
    }

    #[test]
    fn stopping_is_not_retried() {
        let e = FetchError::new(Kind::Stopped, "https://a/b", "download cancelled");
        assert_eq!(e.message, "download cancelled");
        assert!(!e.passing());
        assert!(FetchError::new(Kind::Status(503), "https://a/b", 503).passing());
        assert!(!FetchError::new(Kind::NotFound, "https://a/b", 404).passing());
    }
}
