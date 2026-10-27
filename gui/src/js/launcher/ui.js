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
  globe:
    '<circle cx="12" cy="12" r="9"/><ellipse cx="12" cy="12" rx="4" ry="9"/><path d="M3 12h18"/>',
  plus: '<path d="M12 5v14M5 12h14"/>',
  edit: '<path d="M4 20h4L19 9l-4-4L4 16Z"/><path d="m13 7 4 4"/>',
  settings:
    '<circle cx="12" cy="12" r="3"/><path d="M12 2v3m0 14v3M2 12h3m14 0h3M4.9 4.9l2.1 2.1m10 10 2.1 2.1M4.9 19.1 7 17m10-10 2.1-2.1"/>',
  folder: '<path d="M3 6a1 1 0 0 1 1-1h5l2 2h9a1 1 0 0 1 1 1v10a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1Z"/>',
  download: '<path d="M12 4v11m-5-5 5 5 5-5"/><path d="M5 20h14"/>',
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

/** @param {string} message @param {'info' | 'error' | 'success'} [level] */
export function notice(message, level = 'info') {
  const area = required('notices');
  const messageText = message.replace(/<br\s*\/?\s*>/gi, '\n');
  if (Array.from(area.querySelectorAll('p')).some((node) => node.textContent === messageText))
    return;
  const item = document.createElement('div');
  item.className = 'notice';
  item.dataset.level = level;
  const text = document.createElement('p');
  text.textContent = brand(messageText);
  const close = document.createElement('button');
  close.textContent = '×';
  close.setAttribute('aria-label', 'Dismiss notification');
  close.addEventListener('click', () => item.remove());
  item.append(text, close);
  area.append(item);
  if (level !== 'error') setTimeout(() => item.remove(), 8000);
}

/** @param {() => Promise<unknown>} action */
export const run = (action) => {
  action().catch((error) => notice(errorMessage(error), 'error'));
};
