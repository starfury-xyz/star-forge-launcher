import { invoke } from './bridge.js';
import { object, boolean, string, choice } from './contracts.js';
import { brand, required, run, errorMessage, notice } from './ui.js';
import { factions } from './preferences.js';

/** @param {string} name */
const themeName = (name) => brand(name[0].toUpperCase() + name.slice(1));

/** @param {import('./preferences.js').PreferenceStore} preferences
 * @param {ReturnType<typeof import('./state.js').createLauncherStore>} store */
export async function bindSettings(preferences, store) {
  let tab = 'launcher';
  let compat = { supported: false, proton: /** @type {string[]} */ ([]) };
  const area = required('settings-content');
  /** @param {string} title @param {string} note @param {string} id @param {string} input */
  const row = (title, note, id, input) =>
    `<div class="setting-row"><label for="${id}">${title}<small>${note}</small></label>${input}</div>`;
  const render = () => {
    area.innerHTML =
      tab === 'launcher'
        ? `
      <section class="setting-group"><h2>Window behavior</h2>${row('After starting the game', 'Choose where the launcher stays while you play.', 'launch-behavior', '<select id="launch-behavior" data-preference="launchBehavior"><option value="tray">Minimize to tray</option><option value="keepOpen">Keep launcher open</option><option value="exit">Exit launcher</option></select>')}
      ${row('When closing the launcher', 'Choose what happens when you click X.', 'close-behavior', '<select id="close-behavior" data-preference="closeBehavior"><option value="exit">Exit launcher</option><option value="tray">Minimize to tray</option></select>')}</section>
      <section class="setting-group"><h2>Appearance</h2>${row('Color theme', 'Hover a theme to preview it.', 'theme-button', `<div class="theme-picker"><button type="button" class="theme-button" id="theme-button" aria-haspopup="listbox" aria-expanded="false" aria-controls="theme-list"><span class="swatch"></span><span class="theme-name"></span><span class="caret" aria-hidden="true">▾</span></button><ul class="theme-list" id="theme-list" role="listbox" aria-label="Color theme" hidden>${factions.map((name) => `<li class="theme-option" role="option" id="theme-${name}" data-theme="${name}"><span class="swatch" data-faction="${name}"></span>${themeName(name)}</li>`).join('')}</ul></div>`)}
      ${row('Server pack look', 'Show the selected server pack’s logo, icon, background and color. Its color replaces the faction color.', 'pack-branding', '<input id="pack-branding" data-preference="packBranding" type="checkbox">')}
      ${row('Background folder', 'Your own wallpapers: one is picked at random each time the launcher starts, unless the server pack brings its own.', 'background-folder', '<span class="folder-field"><input id="background-folder" data-preference="backgroundFolder" type="text" spellcheck="false" placeholder="Steam banner"><button class="button" id="pick-background-folder">Browse…</button></span>')}</section>
      <section class="setting-group"><h2>Updates</h2>${row('Check for launcher updates', 'Look for a new STAR FORGE Launcher version each time it starts.', 'check-updates', '<input id="check-updates" data-preference="checkUpdates" type="checkbox">')}
      <div class="setting-buttons"><button class="button" id="check-launcher-update">Check now</button></div></section>`
        : `
      <section class="setting-group"><h2>Installation</h2>
      ${row('Game folder', 'Leave empty to find Star Conflict through Steam.', 'game-directory', '<input id="game-directory" data-preference="gameDirectory" type="text" spellcheck="false" placeholder="Found through Steam">')}
      <p class="settings-path" id="detected-game"></p></section>
      <section class="setting-group" ${compat.supported ? '' : 'hidden'}><h2>Compatibility</h2>
      ${row('Proton version', 'Automatic uses the Proton that last ran the game from Steam, with the same prefix and saved settings.', 'proton-path', '<select id="proton-path" data-preference="protonPath"><option value="">Automatic</option></select>')}
      <p class="settings-path" id="detected-proton"></p></section>
      <section class="setting-group"><h2>Setup</h2>
      ${row('Run setup again', 'Pick the Star Conflict and profiles folders again, as on first start.', 'run-setup', '<span class="confirm-inline"><button class="button" id="run-setup">Run setup…</button><span id="run-setup-confirm" hidden>Open the setup now? <button class="button primary" id="run-setup-yes">Run setup</button><button class="button" id="run-setup-no">Cancel</button></span></span>')}</section>
      <section class="setting-group"><h2>Folders and logs</h2><div class="setting-buttons"><button class="button" data-command="view_game_install">Open game folder</button><button class="button" data-command="open_profile_folder">Open profiles folder</button><button class="button" data-command="view_launch_log">Open last launch log</button></div></section>`;
    if (tab === 'game') {
      const select = area.querySelector('#proton-path');
      if (select instanceof HTMLSelectElement)
        for (const path of compat.proton)
          select.add(new Option(path.split(/[\\/]/).filter(Boolean).pop() || path, path));
    }
    sync();
  };
  const sync = () => {
    const values = preferences.get();
    const previewing = area.querySelector('#theme-button[aria-expanded="true"]');
    if (!previewing) document.documentElement.dataset.faction = values.factionTheme;
    area.querySelectorAll('[data-preference]').forEach((input) => {
      if (!(input instanceof HTMLInputElement || input instanceof HTMLSelectElement)) return;
      if (input === document.activeElement && input.type === 'text') return;
      const entry = Object.entries(values).find(([key]) => key === input.dataset.preference);
      if (!entry) return;
      if (input instanceof HTMLInputElement && input.type === 'radio')
        input.checked = input.value === entry[1];
      else if (input instanceof HTMLInputElement && input.type === 'checkbox')
        input.checked = entry[1] === true;
      else input.value = String(entry[1]);
    });
    const theme = area.querySelector('#theme-button');
    if (theme && !previewing) {
      theme.querySelector('.swatch')?.setAttribute('data-faction', values.factionTheme);
      const name = theme.querySelector('.theme-name');
      if (name) name.textContent = themeName(values.factionTheme);
      area.querySelectorAll('.theme-option').forEach((option) =>
        option.setAttribute('aria-selected', String(option.getAttribute('data-theme') === values.factionTheme)),
      );
    }
    const state = store.getSnapshot();
    const game = area.querySelector('#detected-game');
    if (game)
      game.textContent = state?.gameDirectory
        ? `Using ${state.gameDirectory}`
        : state?.phase === 'checking'
          ? 'Looking for the game…'
          : 'Star Conflict was not found.';
    const proton = area.querySelector('#detected-proton');
    if (proton) proton.textContent = state?.proton ? `Using ${state.proton}` : '';
  };
  // Theme picker: hovering or arrowing through themes previews them on the
  // whole launcher; closing without choosing puts the saved one back.
  const picker = () => {
    const button = area.querySelector('#theme-button');
    const list = area.querySelector('#theme-list');
    return button instanceof HTMLElement && list instanceof HTMLElement ? { button, list } : null;
  };
  const options = () => Array.from(area.querySelectorAll('.theme-option'));
  /** @param {Element | undefined} option */
  const preview = (option) => {
    const parts = picker();
    if (!parts || !option) return;
    options().forEach((item) => item.classList.toggle('active', item === option));
    parts.button.setAttribute('aria-activedescendant', option.id);
    document.documentElement.dataset.faction = option.getAttribute('data-theme') || '';
    option.scrollIntoView({ block: 'nearest' });
  };
  const openThemes = () => {
    const parts = picker();
    if (!parts) return;
    parts.list.hidden = false;
    parts.button.setAttribute('aria-expanded', 'true');
    preview(options().find((item) => item.getAttribute('aria-selected') === 'true'));
  };
  /** @param {string} [chosen] */
  const closeThemes = (chosen) => {
    const parts = picker();
    if (!parts || parts.list.hidden) return;
    parts.list.hidden = true;
    parts.button.setAttribute('aria-expanded', 'false');
    parts.button.removeAttribute('aria-activedescendant');
    // Saving re-syncs through the preferences subscription.
    if (chosen) run(() => preferences.update('factionTheme', choice(chosen, factions)));
    else sync();
  };
  area.addEventListener('click', (event) => {
    if (!(event.target instanceof Element)) return;
    const option = event.target.closest('.theme-option');
    if (option) closeThemes(option.getAttribute('data-theme') || undefined);
    else if (event.target.closest('#theme-button'))
      picker()?.list.hidden ? openThemes() : closeThemes();
  });
  area.addEventListener('mouseover', (event) => {
    if (event.target instanceof Element && !picker()?.list.hidden) {
      const option = event.target.closest('.theme-option');
      if (option) preview(option);
    }
  });
  area.addEventListener('keydown', (event) => {
    const parts = picker();
    if (!parts || !(event.target instanceof Element) || !event.target.closest('#theme-button')) return;
    const all = options();
    const current = all.findIndex((item) => item.classList.contains('active'));
    if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
      event.preventDefault();
      if (parts.list.hidden) openThemes();
      else preview(all[(current + (event.key === 'ArrowDown' ? 1 : all.length - 1)) % all.length]);
    } else if ((event.key === 'Enter' || event.key === ' ') && !parts.list.hidden) {
      event.preventDefault();
      closeThemes(all[current]?.getAttribute('data-theme') || undefined);
    } else if (event.key === 'Escape' && !parts.list.hidden) {
      // Close the list, not the settings dialog.
      event.preventDefault();
      event.stopPropagation();
      closeThemes();
    }
  });
  document.addEventListener('click', (event) => {
    if (event.target instanceof Element && !event.target.closest('.theme-picker')) closeThemes();
  });
  preferences.subscribe(sync);
  store.subscribe(sync);
  area.addEventListener('change', (event) => {
    const input = event.target;
    if (!(input instanceof HTMLInputElement || input instanceof HTMLSelectElement)) return;
    const key = input.dataset.preference;
    if (key === 'launchBehavior')
      run(() => preferences.update(key, choice(input.value, ['tray', 'keepOpen', 'exit'])));
    else if (key === 'closeBehavior')
      run(() => preferences.update(key, choice(input.value, ['tray', 'exit'])));
    else if ((key === 'packBranding' || key === 'checkUpdates') && input instanceof HTMLInputElement)
      run(() => preferences.update(key, input.checked));
    else if (key === 'gameDirectory' || key === 'protonPath' || key === 'backgroundFolder')
      run(() => preferences.update(key, input.value.trim()));
  });
  document.addEventListener('click', (event) => {
    if (!(event.target instanceof Element)) return;
    const button = event.target.closest('button');
    if (!button) return;
    if (button.dataset.settingsTab) {
      tab = button.dataset.settingsTab;
      document
        .querySelectorAll('[data-settings-tab]')
        .forEach((item) => item.removeAttribute('aria-current'));
      button.setAttribute('aria-current', 'page');
      render();
    }
    if (button.id === 'open-settings') run(() => preferences.reload());
    if (button.id === 'run-setup' || button.id === 'run-setup-no') {
      const asking = button.id === 'run-setup';
      required('run-setup').hidden = asking;
      required('run-setup-confirm').hidden = !asking;
    }
    if (button.id === 'run-setup-yes') {
      button.closest('dialog')?.close();
      document.dispatchEvent(new Event('run-setup'));
    }
    if (button.id === 'pick-background-folder')
      run(async () => {
        const dir = await invoke('pick_background_folder');
        if (typeof dir === 'string') await preferences.update('backgroundFolder', dir);
      });
    if (button.dataset.command && button.closest('#settings-content'))
      run(() => invoke(button.dataset.command || ''));
  });
  render();
  try {
    const value = object(await invoke('detect_compat_tools'));
    compat = {
      supported: boolean(value.supported),
      proton: Array.isArray(value.proton) ? value.proton.map(string) : [],
    };
    required('launcher-version').textContent = `STAR FORGE Launcher ${string(await invoke('app_version'))}`;
    render();
  } catch (error) {
    notice(`Could not load launcher settings: ${errorMessage(error)}`, 'error');
  }
}
