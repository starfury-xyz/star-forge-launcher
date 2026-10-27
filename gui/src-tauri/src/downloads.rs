//! The download window. Game files from a pack's file list report their
//! progress to the interface as `download-progress` events `{id, kind, ...}`:
//! `start` (only when there is something to download), then `file`, `bytes`
//! and `verified` for each file, `removed` for dropped ones, and `finish`.

use sc_launch::remote::SyncEvent;
use serde::Serialize;
use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use tauri::{AppHandle, Emitter, Manager};

/// What a download the player cancelled fails with.
pub const CANCELLED: &str = "download cancelled";

/// Byte counts go out at most this often; the window shows no more.
const TICK: Duration = Duration::from_millis(100);

/// Downloads in progress, by id, with their cancel flags.
#[derive(Default)]
pub struct Downloads {
    next: AtomicU64,
    running: Mutex<HashMap<u64, Arc<AtomicBool>>>,
}

#[derive(Clone, Serialize)]
#[serde(tag = "kind", rename_all = "camelCase")]
enum Event<'a> {
    Start {
        action: &'a str,
        title: &'a str,
        source: &'a str,
        signed: bool,
        listed: usize,
        files: usize,
        bytes: Option<u64>,
        kept: usize,
        remove: usize,
    },
    File { path: &'a str, size: Option<u64> },
    Bytes { done: u64, total: Option<u64> },
    Verified { path: &'a str, sha256: &'a str, size: u64 },
    Removed { path: &'a str },
    Finish { error: Option<&'a str>, cancelled: bool },
}

#[derive(Clone, Serialize)]
struct Message<'a> {
    id: u64,
    #[serde(flatten)]
    event: Event<'a>,
}

/// One sync shown in the window. Feed it the sync's progress with
/// [`Session::report`], then end it with [`Session::finish`].
pub struct Session {
    app: AppHandle,
    id: u64,
    /// `install` or `update`.
    action: &'static str,
    title: String,
    cancel: Arc<AtomicBool>,
    /// The window opened: there was something to download.
    shown: bool,
    last_tick: Option<Instant>,
}

impl Session {
    pub fn new(app: &AppHandle, action: &'static str, title: &str) -> Session {
        let downloads = app.state::<Downloads>();
        let id = downloads.next.fetch_add(1, Ordering::Relaxed) + 1;
        let cancel = Arc::new(AtomicBool::new(false));
        downloads.running.lock().unwrap().insert(id, cancel.clone());
        Session { app: app.clone(), id, action, title: title.to_string(), cancel, shown: false, last_tick: None }
    }

    fn emit(&self, event: Event) {
        if let Err(e) = self.app.emit_to("main", "download-progress", Message { id: self.id, event }) {
            eprintln!("could not publish download progress: {e}");
        }
    }

    /// The progress callback for a sync; fails once the player cancels.
    pub fn report(&mut self, event: SyncEvent<'_>) -> Result<(), String> {
        if self.cancel.load(Ordering::Relaxed) {
            return Err(CANCELLED.into());
        }
        match event {
            SyncEvent::Plan { list, files, bytes, kept, remove } => {
                self.shown = files > 0;
                if self.shown {
                    self.emit(Event::Start {
                        action: self.action,
                        title: &self.title,
                        source: &list.source,
                        signed: list.signed,
                        listed: list.entries.len(),
                        files,
                        bytes,
                        kept,
                        remove,
                    });
                }
            }
            _ if !self.shown => {}
            SyncEvent::File { path, size } => {
                self.last_tick = None;
                self.emit(Event::File { path, size });
            }
            SyncEvent::Bytes { done, total } => {
                let now = Instant::now();
                if total == Some(done) || self.last_tick.is_none_or(|t| now - t >= TICK) {
                    self.last_tick = Some(now);
                    self.emit(Event::Bytes { done, total });
                }
            }
            SyncEvent::Verified { path, sha256, size } => self.emit(Event::Verified { path, sha256, size }),
            SyncEvent::Removed { path } => self.emit(Event::Removed { path }),
        }
        Ok(())
    }

    /// Ends the download in the window, with what went wrong, if anything.
    pub fn finish(self, error: Option<&str>) {
        if self.shown {
            self.emit(Event::Finish { error, cancelled: self.cancel.load(Ordering::Relaxed) });
        }
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        self.app.state::<Downloads>().running.lock().unwrap().remove(&self.id);
    }
}

/// Stops a download at its next chunk. Files that finished are kept.
#[tauri::command]
pub fn cancel_download(app: AppHandle, id: u64) {
    if let Some(flag) = app.state::<Downloads>().running.lock().unwrap().get(&id) {
        flag.store(true, Ordering::Relaxed);
    }
}
