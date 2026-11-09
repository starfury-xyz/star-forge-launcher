import { invoke, listen } from './bridge.js';
import { object, string } from './contracts.js';
import { control, required, run, notice, errorMessage } from './ui.js';

/** @param {unknown} input */
function parseUpdate(input) {
  if (input === null) return null;
  const value = object(input);
  return {
    version: string(value.version),
    current: string(value.current),
    page: typeof value.page === 'string' ? value.page : null,
  };
}

/** Launcher self-update: checks at startup (when allowed) and from Settings.
 * @param {import('./preferences.js').PreferenceStore} preferences
 * @param {() => boolean} gameBusy
 * @param {import('./downloads.js').Downloads} downloads */
export async function bindUpdater(preferences, gameBusy, downloads) {
  const bar = required('launcher-update');
  const text = required('launcher-update-text');
  const notes = control('launcher-update-notes', HTMLButtonElement);
  const install = control('launcher-update-install', HTMLButtonElement);
  const PACKAGE = 'update package';
  let installing = false;
  /** @type {{ version: string, current: string } | null} */
  let found = null;
  let received = 0;

  /** @param {boolean} manual */
  const check = async (manual) => {
    if (installing) return;
    const update = parseUpdate(await invoke('check_launcher_update'));
    found = update;
    bar.hidden = !update;
    if (!update) {
      if (manual) notice('STAR FORGE Launcher is up to date.', 'success');
      return;
    }
    text.textContent = `STAR FORGE Launcher ${update.version} is available (you have ${update.current}).`;
    notes.hidden = !update.page;
    install.disabled = false;
    install.textContent = 'Update and restart';
  };

  await listen('launcher-update-progress', (payload) => {
    if (!Array.isArray(payload) || typeof payload[0] !== 'number') return;
    const total = typeof payload[1] === 'number' && payload[1] > 0 ? payload[1] : 0;
    install.textContent = total
      ? `Downloading… ${Math.min(100, Math.round((payload[0] / total) * 100))}%`
      : `Downloading… ${(payload[0] / 1048576).toFixed(1)} MB`;
    received = payload[0];
    if (downloads.current() === 'launcher') downloads.bytes(payload[0], total || null);
  });
  // The plugin checks the package's signature once it has it all, then installs.
  await listen('launcher-update-downloaded', () => {
    if (downloads.current() !== 'launcher') return;
    downloads.verified(PACKAGE, null, received);
    downloads.line('sig', 'checking the package signature, then installing', 'minisign');
  });
  notes.addEventListener('click', () => run(() => invoke('open_launcher_release')));
  install.addEventListener('click', () => {
    if (gameBusy()) {
      notice('Close the game before updating the launcher.', 'error');
      return;
    }
    installing = true;
    install.disabled = true;
    install.textContent = 'Downloading…';
    received = 0;
    downloads.begin(
      {
        id: 'launcher',
        action: 'launcher',
        title: `STAR FORGE Launcher ${found?.version || ''}`.trim(),
        files: 1,
        bytes: null,
        cancel: null,
      },
      found ? [['plan', `${found.current} → ${found.version}`, 'signed release']] : [],
    );
    downloads.file(PACKAGE, null);
    invoke('install_launcher_update').catch((error) => {
      installing = false;
      install.disabled = false;
      install.textContent = 'Update and restart';
      if (downloads.current() === 'launcher') downloads.finish(errorMessage(error));
      notice(errorMessage(error), 'error');
    });
  });
  document.addEventListener('click', (event) => {
    if (event.target instanceof Element && event.target.closest('#check-launcher-update'))
      run(() => check(true));
  });
  // A failed automatic check (offline, no release yet) stays quiet.
  if (preferences.get().checkUpdates) check(false).catch((error) => console.warn(error));
}
