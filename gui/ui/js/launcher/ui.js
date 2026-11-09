/** @template {Element} T @param {string} id @param {new (...args: never[]) => T} kind @returns {T} */
export function control(id, kind) {
  const element = document.getElementById(id);
  if (!(element instanceof kind)) throw new Error(`Missing ${id} control`);
  return element;
}
/** Displayed text always spells brand names in capitals (STAR FORGE, STARFURY, STARFURY.XYZ); stored values stay as typed. @param {string} text */
export const brand = (text) => text
    .replace(/star\s?forge/gi, 'STAR FORGE')
    .replace(/star\s?fury(\.xyz)?/gi, (name) => name.replace(/\s/, '').toUpperCase());
/** @param {string} id */
export const required = (id) => control(id, HTMLElement);

/** Builds an element with text content only; pack contents are never parsed as HTML. */
export function el(/** @type {string} */ tag, /** @type {string} */ className, /** @type {string} */ content = '') {
  const node = document.createElement(tag);
  if (className) node.className = className;
  node.textContent = content;
  return node;
}

const paths = {
  home: '<path d="m3 10 9-7 9 7v10H3Z"/><path d="M9 20v-7h6v7"/>',
  minus: '<path d="M5 12h14"/>',
  square: '<rect x="5" y="5" width="14" height="14" rx="1"/>',
  close: '<path d="m6 6 12 12M6 18 18 6"/>',
  arrow: '<path d="M6 18 18 6M6 6h12v12"/>',
  play: '<path d="m7 4 13 8-13 8Z"/>',
  plus: '<path d="M12 5v14M5 12h14"/>',
  edit: '<path d="M4 20h4L19 9l-4-4L4 16Z"/><path d="m13 7 4 4"/>',
  settings:
    '<circle cx="12" cy="12" r="3"/><path d="M12 2v3m0 14v3M2 12h3m14 0h3M4.9 4.9l2.1 2.1m10 10 2.1 2.1M4.9 19.1 7 17m10-10 2.1-2.1"/>',
  folder: '<path d="M3 6a1 1 0 0 1 1-1h5l2 2h9a1 1 0 0 1 1 1v10a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1Z"/>',
  download: '<path d="M12 4v11m-5-5 5 5 5-5"/><path d="M5 20h14"/>',
  bell: '<path d="M6 16V11a6 6 0 0 1 12 0v5l2 2H4Z"/><path d="M10 20a2 2 0 0 0 4 0"/>',
};
/** @param {string} name */
export function icon(name) {
  const path = Object.entries(paths).find(([key]) => key === name)?.[1];
  if (!path) throw new Error(`Unknown icon: ${name}`);
  return `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${path}</svg>`;
}
export function renderIcons() {
  document.querySelectorAll('[data-icon]').forEach((node) => {
    node.innerHTML = icon(node.getAttribute('data-icon') || 'user');
  });
}

/** @param {unknown} error */
export function errorMessage(error) {
  if (
    typeof error === 'object' &&
    error !== null &&
    'message' in error &&
    typeof error.message === 'string'
  )
    return error.message;
  return error instanceof Error ? error.message : String(error);
}

/** Panels (settings, profiles, packs, downloads) open over the main area only, so the rail and window controls stay usable. @type {HTMLDialogElement[]} */
const panels = [];

/** Closes any open panel. */
export function closePanels() {
  for (const panel of [...panels]) panel.close();
}

/**
 * Opens `dialog` as a panel over the main area, replacing any other panel.
 * @param {HTMLDialogElement} dialog
 */
export function openPanel(dialog) {
  for (const other of [...panels]) if (other !== dialog) other.close();
  if (dialog.open) return;
  if (!dialog.dataset.panel) {
    dialog.dataset.panel = 'true';
    dialog.addEventListener('close', () => {
      const at = panels.indexOf(dialog);
      if (at >= 0) panels.splice(at, 1);
    });
  }
  panels.push(dialog);
  dialog.show();
}

// Esc closes the newest panel, as it would a modal dialog (modal ones handle it themselves).
document.addEventListener('keydown', (event) => {
  if (event.key !== 'Escape' || event.defaultPrevented || document.querySelector('dialog:modal')) return;
  const top = panels.at(-1);
  if (top) {
    event.preventDefault();
    top.close();
  }
});

/** At most this many notices on screen; older ones stay in the history. */
const MAX_VISIBLE = 3;
const HISTORY_LIMIT = 50;
/** How long a notice stays, unless the pointer is on it (ms). */
const LIFETIME = { info: 5000, success: 5000, error: 10000 };

/** @type {{ message: string, level: string, time: Date }[]} */
const history = [];
let unread = 0;

/**
 * Keeps the notices in the top layer, above any open dialog: modal dialogs
 * render there too, so whichever was shown last wins. Re-showing the notices
 * after each dialog opens puts them back on top.
 */
function noticeLayer() {
  const area = required('notices');
  if (area.dataset.bound) return area;
  area.dataset.bound = 'true';
  if (typeof area.showPopover !== 'function') return area; // no popover support: plain z-index
  area.popover = 'manual';
  const raise = () => {
    area.hidePopover();
    area.showPopover();
  };
  raise();
  new MutationObserver((changes) => {
    if (changes.some((c) => c.target instanceof HTMLDialogElement && c.target.open)) raise();
  }).observe(document.body, { subtree: true, attributes: true, attributeFilter: ['open'] });
  return area;
}

function renderHistory() {
  const bell = document.getElementById('open-notices');
  const badge = document.getElementById('notice-badge');
  const list = document.getElementById('notice-history-list');
  const empty = document.getElementById('notice-history-empty');
  if (!bell || !badge || !list || !empty) return;
  empty.hidden = history.length > 0;
  badge.hidden = unread === 0;
  badge.textContent = unread > 9 ? '9+' : String(unread);
  list.replaceChildren(
    ...history.map((entry) => {
      const row = document.createElement('li');
      row.dataset.level = entry.level;
      const time = document.createElement('time');
      time.textContent = entry.time.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
      const text = document.createElement('p');
      text.textContent = entry.message;
      row.append(time, text);
      return row;
    }),
  );
}

/** The rail's notification bell and its history panel. */
export function bindNoticeHistory() {
  const panel = control('notice-history', HTMLDialogElement);
  required('open-notices').addEventListener('click', () => {
    if (panel.open) return panel.close();
    unread = 0;
    renderHistory();
    openPanel(panel);
  });
  panel.addEventListener('close', renderHistory);
  required('notice-history-clear').addEventListener('click', () => {
    history.length = 0;
    unread = 0;
    renderHistory();
  });
  renderHistory();
}

/**
 * Shows a notice in the bottom right, above dialogs, and keeps it in the
 * history. Info and success go after 5 s, errors after 10 s (the history
 * keeps them); the pointer on a notice holds it. A repeat of a notice on
 * screen counts up instead of stacking.
 * @param {string} message
 * @param {'info' | 'error' | 'success'} [level]
 * @param {{ label: string, action: () => void }} [button] runs once and dismisses the notice
 */
export function notice(message, level = 'info', button) {
  const area = noticeLayer();
  const messageText = brand(message.replace(/<br\s*\/?\s*>/gi, '\n'));
  history.unshift({ message: messageText, level, time: new Date() });
  history.length = Math.min(history.length, HISTORY_LIMIT);
  if (level === 'error') unread += 1;
  renderHistory();

  const same = Array.from(area.children).find(
    (node) => node instanceof HTMLElement && node.dataset.message === messageText,
  );
  if (same instanceof HTMLElement) {
    const count = Number(same.dataset.count || '1') + 1;
    same.dataset.count = String(count);
    const badge = same.querySelector('.notice-count');
    if (badge) badge.textContent = `×${count}`;
    same.dispatchEvent(new Event('notice-restart'));
    return;
  }

  const item = document.createElement('div');
  item.className = 'notice';
  item.dataset.level = level;
  item.dataset.message = messageText;
  item.setAttribute('role', level === 'error' ? 'alert' : 'status');
  const text = document.createElement('p');
  text.textContent = messageText;
  text.title = messageText;
  const count = document.createElement('span');
  count.className = 'notice-count';
  const close = document.createElement('button');
  close.textContent = '×';
  close.setAttribute('aria-label', 'Dismiss notification');
  close.addEventListener('click', () => item.remove());
  item.append(text, count);
  if (button) {
    const act = document.createElement('button');
    act.className = 'notice-action';
    act.textContent = button.label;
    act.addEventListener('click', () => {
      item.remove();
      button.action();
    });
    item.append(act);
  }
  item.append(close);

  let remaining = LIFETIME[level];
  let started = 0;
  /** @type {ReturnType<typeof setTimeout> | undefined} */
  let timer;
  const resume = () => {
    clearTimeout(timer);
    started = Date.now();
    timer = setTimeout(() => item.remove(), remaining);
  };
  item.addEventListener('pointerenter', () => {
    clearTimeout(timer);
    remaining = Math.max(remaining - (Date.now() - started), 2000);
  });
  item.addEventListener('pointerleave', resume);
  item.addEventListener('notice-restart', () => {
    remaining = LIFETIME[level];
    if (!item.matches(':hover')) resume();
  });

  area.append(item);
  resume();
  while (area.childElementCount > MAX_VISIBLE) area.firstElementChild?.remove();
}

/** @param {() => Promise<unknown>} action */
export const run = (action) => {
  action().catch((error) => notice(errorMessage(error), 'error'));
};
