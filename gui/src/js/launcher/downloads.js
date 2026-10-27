import { invoke, listen } from './bridge.js';
import { boolean, choice, number, object, string } from './contracts.js';
import { brand, control, el, errorMessage, notice, required } from './ui.js';

/** Console lines kept; older ones scroll away. */
const MAX_LINES = 400;
/** The throughput graph gets one sample per tick and shows the last 30 s. */
const TICK_MS = 500;
const GRAPH_POINTS = 60;
/** Speed is averaged over this many milliseconds. */
const SPEED_WINDOW = 3000;
/** A finished download's window closes by itself after this, unless the player is reading it. */
const AUTO_CLOSE_MS = 3000;

const eyebrows = {
  install: 'Installing game files',
  update: 'Updating game files',
  launcher: 'Updating the launcher',
};
const states = { running: 'Live', done: 'Complete', failed: 'Failed', cancelled: 'Cancelled' };
const railLabels = { running: '', done: 'Done', failed: 'Failed', cancelled: 'Stopped' };

/** @param {number} bytes */
export function formatBytes(bytes) {
  if (bytes < 1024) return `${Math.round(bytes)} B`;
  const units = ['KB', 'MB', 'GB', 'TB'];
  let value = bytes / 1024;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit += 1;
  }
  return `${value.toFixed(value < 10 ? 2 : 1)} ${units[unit]}`;
}

/** @param {number} seconds */
function clock(seconds) {
  const s = Math.max(0, Math.round(seconds));
  const pad = (/** @type {number} */ n) => String(n).padStart(2, '0');
  const h = Math.floor(s / 3600);
  const m = Math.floor(s / 60) % 60;
  return h ? `${h}:${pad(m)}:${pad(s % 60)}` : `${m}:${pad(s % 60)}`;
}

const count = (/** @type {unknown} */ value) => number(value, 0, Number.MAX_SAFE_INTEGER);
const size = (/** @type {unknown} */ value) => (value === null ? null : count(value));

/**
 * @typedef {{
 *   id: string,
 *   action: keyof typeof eyebrows,
 *   title: string,
 *   files: number,
 *   bytes: number | null,
 *   cancel: (() => Promise<unknown>) | null,
 * }} DownloadInfo
 * @typedef {{ path: string, done: number, total: number | null, started: number, row: HTMLElement }} CurrentFile
 * @typedef {{
 *   info: DownloadInfo,
 *   state: keyof typeof states,
 *   started: number,
 *   ended: number,
 *   filesDone: number,
 *   bytesDone: number,
 *   current: CurrentFile | null,
 *   history: [number, number][],
 *   graph: number[],
 * }} Session
 */

/**
 * The download window: a percentage and bar, speed, ETA, a throughput graph
 * and a console listing every file as it arrives, plus a ring in the rail
 * while it runs. Game file syncs drive it from `download-progress` events;
 * the launcher's own update through the returned functions.
 */
export async function bindDownloads() {
  const dialog = control('download-dialog', HTMLDialogElement);
  const log = required('download-console');
  const bar = required('download-bar');
  const percent = required('download-percent');
  const state = required('download-state');
  const amount = required('download-amount');
  const fileLabel = required('download-file');
  const countLabel = required('download-count');
  const speedLabel = required('download-speed');
  const etaLabel = required('download-eta');
  const elapsedLabel = required('download-elapsed');
  const graphLine = control('download-graph-line', SVGPolylineElement);
  const graphFill = control('download-graph-fill', SVGPolygonElement);
  const hide = control('download-hide', HTMLButtonElement);
  const cancel = control('download-cancel', HTMLButtonElement);
  const rail = control('open-downloads', HTMLButtonElement);
  const ring = control('download-ring', SVGCircleElement);
  const railLabel = required('download-rail-label');
  /** @type {Session | null} */
  let active = null;
  /** @type {ReturnType<typeof setInterval> | undefined} */
  let ticker;
  /** @type {ReturnType<typeof setTimeout> | undefined} */
  let autoClose;
  /** @type {ReturnType<typeof setTimeout> | undefined} */
  let railTimer;

  /** Adds a console line, following the end unless the player scrolled up. @param {string} tag @param {string} text @param {string} [meta] */
  const line = (tag, text, meta = '') => {
    const seconds = active ? (performance.now() - active.started) / 1000 : 0;
    const row = el('div', 'console-line');
    row.dataset.tag = tag;
    row.append(
      el('span', 'console-time', `[${seconds.toFixed(3).padStart(8)}]`),
      el('span', 'console-tag', tag),
      el('span', 'console-text', text),
      el('span', 'console-meta', meta),
    );
    const following = log.scrollHeight - log.scrollTop - log.clientHeight < 24;
    log.append(row);
    while (log.childElementCount > MAX_LINES) log.firstElementChild?.remove();
    if (following) log.scrollTop = log.scrollHeight;
    return row;
  };
  /** @param {HTMLElement} row @param {string} tag @param {string} meta */
  const relabel = (row, tag, meta) => {
    row.dataset.tag = tag;
    const [, tagNode, , metaNode] = row.children;
    tagNode.textContent = tag;
    metaNode.textContent = meta;
  };

  /** @param {Session} s */
  const received = (s) => s.bytesDone + (s.current?.done || 0);
  /** Fraction done, 0–1: by bytes when the sizes are known, else by files. @param {Session} s */
  const fraction = (s) => {
    if (s.state === 'done') return 1;
    if (s.info.bytes) return Math.min(1, received(s) / s.info.bytes);
    if (!s.info.files) return 0;
    const part = s.current?.total ? Math.min(1, s.current.done / s.current.total) : 0;
    return Math.min(1, (s.filesDone + part) / s.info.files);
  };
  /** Bytes per second over the last few seconds. @param {Session} s @param {number} now */
  const speed = (s, now) => {
    s.history.push([now, received(s)]);
    while (s.history.length > 2 && now - s.history[1][0] > SPEED_WINDOW) s.history.shift();
    const [then, before] = s.history[0];
    return now - then > 250 ? ((received(s) - before) * 1000) / (now - then) : 0;
  };

  const drawGraph = (/** @type {number[]} */ values) => {
    const max = Math.max(...values, 1);
    const points = values.map((v, i) => {
      const x = ((GRAPH_POINTS - values.length + i) * 60) / (GRAPH_POINTS - 1);
      return `${x.toFixed(2)},${(19.5 - (v / max) * 18).toFixed(2)}`;
    });
    graphLine.setAttribute('points', points.join(' '));
    graphFill.setAttribute('points', points.length ? `${points[0].split(',')[0]},20 ${points.join(' ')} 60,20` : '');
  };

  /** Shows the active download; returns its speed. */
  const paint = () => {
    const s = active;
    if (!s) return 0;
    const now = s.ended || performance.now();
    const elapsed = (now - s.started) / 1000;
    const p = fraction(s);
    const rate = s.state === 'running' ? speed(s, now) : elapsed > 0 ? s.bytesDone / elapsed : 0;
    percent.textContent = p >= 1 ? '100%' : `${(Math.floor(p * 1000) / 10).toFixed(1)}%`;
    bar.style.setProperty('--progress', String(p));
    bar.setAttribute('aria-valuenow', String(Math.floor(p * 100)));
    amount.textContent = s.info.bytes
      ? `${formatBytes(received(s))} / ${formatBytes(s.info.bytes)}`
      : `${formatBytes(received(s))} received`;
    countLabel.textContent = `file ${Math.min(s.filesDone + (s.current ? 1 : 0), s.info.files)} / ${s.info.files}`;
    speedLabel.textContent = rate > 0 ? `${formatBytes(rate)}/s` : '–';
    // Nothing arriving, no estimate. Without sizes, extrapolate from the fraction done.
    let eta = null;
    if (s.state === 'running' && rate > 0) {
      if (s.info.bytes) eta = (s.info.bytes - received(s)) / rate;
      else if (p > 0.02) eta = (elapsed * (1 - p)) / p;
    }
    etaLabel.textContent = eta === null ? '–' : clock(eta);
    elapsedLabel.textContent = clock(elapsed);
    ring.setAttribute('stroke-dasharray', `${(p * 100).toFixed(1)} 100`);
    railLabel.textContent = s.state === 'running' ? `${Math.floor(p * 100)}%` : railLabels[s.state];
    dialog.dataset.state = s.state;
    rail.dataset.state = s.state;
    state.textContent = states[s.state];
    return rate;
  };
  const tick = () => {
    const s = active;
    if (!s || s.state !== 'running') return;
    s.graph.push(paint());
    if (s.graph.length > GRAPH_POINTS) s.graph.shift();
    drawGraph(s.graph);
  };

  const api = {
    /** The download shown, if any. */
    current: () => active?.info.id ?? null,
    /** Opens the window for a new download. @param {DownloadInfo} info @param {[string, string, string?][]} [header] console lines to start with */
    begin(info, header = []) {
      clearTimeout(autoClose);
      clearTimeout(railTimer);
      clearInterval(ticker);
      active = {
        info: { ...info },
        state: 'running',
        started: performance.now(),
        ended: 0,
        filesDone: 0,
        bytesDone: 0,
        current: null,
        history: [],
        graph: [],
      };
      log.replaceChildren();
      required('download-eyebrow').textContent = eyebrows[info.action];
      required('download-title').textContent = brand(info.title);
      fileLabel.textContent = 'Connecting…';
      cancel.hidden = !info.cancel;
      cancel.disabled = false;
      cancel.textContent = 'Cancel download';
      hide.textContent = 'Hide';
      for (const [tag, text, meta] of header) line(tag, text, meta);
      drawGraph([]);
      paint();
      rail.hidden = false;
      ticker = setInterval(tick, TICK_MS);
      if (!dialog.open) dialog.showModal();
    },
    /** A file starts downloading. @param {string} path @param {number | null} total */
    file(path, total) {
      const s = active;
      if (!s || s.state !== 'running') return;
      const row = line('get', path, total === null ? '' : formatBytes(total));
      s.current = { path, done: 0, total, started: performance.now(), row };
      fileLabel.textContent = path;
      paint();
    },
    /** Bytes of the current file so far. @param {number} done @param {number | null} total */
    bytes(done, total) {
      const s = active;
      if (!s?.current || s.state !== 'running') return;
      s.current.done = done;
      s.current.total = total;
      // A single file's size is the whole download's.
      if (s.info.files === 1 && s.info.bytes === null && total) s.info.bytes = total;
      const meta = total
        ? `${Math.floor((done / total) * 100)}%  ${formatBytes(done)} / ${formatBytes(total)}`
        : formatBytes(done);
      relabel(s.current.row, 'get', meta);
      paint();
    },
    /** The current file arrived (and matched its hash, if given). @param {string} path @param {string | null} sha256 @param {number} bytes */
    verified(path, sha256, bytes) {
      const s = active;
      if (!s || s.state !== 'running') return;
      const current = s.current;
      const seconds = current ? (performance.now() - current.started) / 1000 : 0;
      const meta = [formatBytes(bytes), `${seconds.toFixed(2)} s`, ...(sha256 ? [`sha256:${sha256.slice(0, 12)}`] : [])].join(' · ');
      if (current?.path === path) relabel(current.row, 'ok', meta);
      else line('ok', path, meta);
      s.filesDone += 1;
      s.bytesDone += bytes;
      s.current = null;
      fileLabel.textContent = s.filesDone < s.info.files ? 'Next file…' : 'Finishing…';
      paint();
    },
    line,
    /** Ends the download: done, failed with `error`, or cancelled by the player. @param {string | null} error @param {boolean} [cancelled] */
    finish(error, cancelled = false) {
      const s = active;
      if (!s || s.state !== 'running') return;
      s.ended = performance.now();
      clearInterval(ticker);
      if (s.current) relabel(s.current.row, cancelled ? '^c' : 'err', cancelled ? 'stopped' : 'failed');
      s.current = null;
      s.state = cancelled ? 'cancelled' : error ? 'failed' : 'done';
      const seconds = (s.ended - s.started) / 1000;
      if (s.state === 'done') {
        line(
          'done',
          `${s.filesDone} file${s.filesDone === 1 ? '' : 's'} · ${formatBytes(s.bytesDone)} in ${seconds.toFixed(1)} s`,
          seconds > 0 ? `avg ${formatBytes(s.bytesDone / seconds)}/s` : '',
        );
        fileLabel.textContent = 'All files downloaded and verified';
      } else if (cancelled) {
        line('^c', `cancelled · ${s.filesDone} of ${s.info.files} files kept`);
        fileLabel.textContent = 'Cancelled. Files that finished are kept.';
      } else {
        line('err', brand(error || 'download failed'));
        fileLabel.textContent = 'Download failed';
      }
      cancel.hidden = true;
      hide.textContent = 'Close';
      paint();
      // Done or stopped: the window closes by itself unless the player is
      // reading it; a failure stays up. Hidden, the rail shows how it went.
      if (s.state !== 'failed') autoClose = setTimeout(() => dialog.close(), AUTO_CLOSE_MS);
      if (!dialog.open) railTimer = setTimeout(() => (rail.hidden = dialog.open), 8000);
    },
  };

  // Touching the window (a click, scrolling the log) keeps it open.
  for (const event of ['pointerdown', 'wheel']) dialog.addEventListener(event, () => clearTimeout(autoClose));
  dialog.addEventListener('close', () => {
    clearTimeout(autoClose);
    if (active?.state !== 'running') rail.hidden = true;
  });
  rail.addEventListener('click', () => {
    clearTimeout(railTimer);
    if (active && !dialog.open) dialog.showModal();
  });
  cancel.addEventListener('click', () => {
    const stop = active?.info.cancel;
    if (!stop) return;
    cancel.disabled = true;
    cancel.textContent = 'Cancelling…';
    stop().catch((error) => {
      cancel.disabled = false;
      cancel.textContent = 'Cancel download';
      notice(errorMessage(error), 'error');
    });
  });

  await listen('download-progress', (input) => {
    const value = object(input);
    const id = `sync:${count(value.id)}`;
    if (value.kind === 'start') {
      const files = count(value.files);
      const bytes = size(value.bytes);
      const kept = count(value.kept);
      const remove = count(value.remove);
      api.begin(
        {
          id,
          action: choice(value.action, ['install', 'update']),
          title: string(value.title),
          files,
          bytes,
          cancel: () => invoke('cancel_download', { id: value.id }),
        },
        [
          ['list', string(value.source), boolean(value.signed) ? 'ed25519 signature ok' : 'unsigned'],
          [
            'plan',
            `${count(value.listed)} listed · ${kept} up to date · ${files} to fetch${bytes ? ` (${formatBytes(bytes)})` : ''}${remove ? ` · ${remove} to remove` : ''}`,
          ],
        ],
      );
      return;
    }
    if (api.current() !== id) return;
    switch (value.kind) {
      case 'file':
        return api.file(string(value.path), size(value.size));
      case 'bytes':
        return api.bytes(count(value.done), size(value.total));
      case 'verified':
        return api.verified(string(value.path), string(value.sha256), count(value.size));
      case 'removed':
        line('del', string(value.path), 'dropped from the list');
        return;
      case 'finish':
        return api.finish(value.error === null ? null : string(value.error), boolean(value.cancelled));
      default:
        throw new Error('Invalid download event');
    }
  });
  return api;
}
/** @typedef {Awaited<ReturnType<typeof bindDownloads>>} Downloads */
