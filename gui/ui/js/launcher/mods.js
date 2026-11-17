import { invoke } from './bridge.js';
import { array, boolean, object, string } from './contracts.js';
import { brand, control, el, errorMessage, notice, required, run } from './ui.js';
import { parseServer } from './servers.js';

/** @param {unknown} item */
function parseToggle(item) {
  const value = object(item);
  const strings = (/** @type {unknown} */ v) => array(v, string);
  return {
    module: string(value.module),
    id: string(value.id),
    title: string(value.title),
    description: string(value.description),
    source: string(value.source),
    build: string(value.build),
    on: boolean(value.on),
    defaultOn: boolean(value.defaultOn),
    chosen: value.chosen === null ? null : boolean(value.chosen),
    optional: boolean(value.optional),
    conflicts: strings(value.conflicts),
    clashing: strings(value.clashing),
  };
}
/** @typedef {ReturnType<typeof parseToggle>} Toggle */

/** @param {unknown} value */
function parseList(value) {
  const list = object(value);
  return {
    toggles: array(list.toggles, parseToggle),
    errors: array(list.errors, string),
    warnings: array(list.warnings, string),
  };
}

/** @param {unknown} item */
function parseUserMod(item) {
  const value = object(item);
  return {
    id: string(value.id),
    file: string(value.file),
    name: string(value.name),
    version: string(value.version),
    description: string(value.description),
    authors: array(value.authors, string),
    on: boolean(value.on),
    status: string(value.status),
    reason: string(value.reason),
    notes: array(value.notes, string),
  };
}
/** @typedef {ReturnType<typeof parseUserMod>} UserMod */

/** A profile's mods/ folder. @param {unknown} value */
function parseUserMods(value) {
  const list = object(value);
  return {
    folder: string(list.folder),
    mods: array(list.mods, parseUserMod),
    conflicts: array(list.conflicts, (c) => {
      const v = object(c);
      return { what: string(v.what), mods: array(v.mods, string) };
    }),
    checked: boolean(list.checked),
    fingerprint: string(list.fingerprint),
  };
}

/** Where a patch comes from, for a group heading. @param {Toggle} t */
const sourceLabel = (t) =>
  t.source === 'core'
    ? 'Core patches (ship with STAR FORGE)'
    : t.source.startsWith('starforge/')
      ? `Server patchset (${t.source})`
      : `From the pack (${t.source})`;

/**
 * The Mods page: the chosen profile's installed mods (its mods/ folder, in load order) and every
 * patch of the profile or server install, each with an on/off switch.
 * @param {ReturnType<typeof import('./profiles.js').bindProfiles>} profiles
 * @param {() => boolean} locked the game is starting or running
 */
export function bindMods(profiles, locked) {
  const target = control('mods-target', HTMLSelectElement);
  const listNode = required('mods-list');
  const problems = required('mods-problems');
  const note = required('mods-note');
  const userSection = required('user-mods');
  const userNote = required('user-mods-note');
  const userConflicts = required('user-mods-conflicts');
  const userList = required('user-mods-list');
  const openFolder = control('mods-open-folder', HTMLButtonElement);
  const checkButton = control('mods-check', HTMLButtonElement);

  /** The list on screen, redrawn when the game starts or stops. @type {ReturnType<typeof parseList> | null} */
  let shown = null;
  let wasLocked = false;
  let server = false;  // the list on screen is a server install's
  /** A target to pick when the page is next shown, e.g. "server:testbed". @type {string | null} */
  let pending = null;

  /** The installed mods on screen. @type {ReturnType<typeof parseUserMods> | null} */
  let userShown = null;
  let userLocked = false;

  /** @param {ReturnType<typeof parseUserMods>} list */
  const renderUser = (list) => {
    userShown = list;
    const busy = locked();
    userLocked = busy;
    checkButton.disabled = busy;
    const on = list.mods.filter((m) => m.on).length;
    userNote.textContent = list.mods.length
      ? `${on} of ${list.mods.length} mod${list.mods.length === 1 ? '' : 's'} on, loaded top to bottom: when two change the same thing, the lower one wins. ` +
        (busy ? 'The game is running: changes apply the next time you press Play.' : 'Changes apply the next time you press Play.')
      : 'No mods yet. Put a mod (a .zip, or a folder with a mod.json) in the mods folder, then come back here.';
    userConflicts.replaceChildren();
    if (list.conflicts.length) {
      userConflicts.append(el('p', 'mods-warning', `Conflicts at the last start${list.fingerprint ? ` (mod set ${list.fingerprint})` : ''}:`));
      for (const c of list.conflicts) {
        const node = el('div', 'mod-conflict');
        node.append(el('code', '', c.what), ` — ${c.mods.join(', ')}; ${c.mods[c.mods.length - 1]} wins.`);
        userConflicts.append(node);
      }
    }
    userList.replaceChildren(...list.mods.map((m, i) => userRow(m, i, list.mods.length)));
  };

  /** @param {UserMod} m @param {number} index @param {number} count */
  const userRow = (m, index, count) => {
    const node = el('div', 'mod-row');
    node.dataset.on = String(m.on);
    node.dataset.status = m.status;
    const box = document.createElement('input');
    box.type = 'checkbox';
    box.checked = m.on;
    box.disabled = m.status === 'broken';
    box.setAttribute('aria-label', `${m.name}: ${m.on ? 'on' : 'off'}`);
    box.addEventListener('change', () => changeUser('set_profile_mod', { mod: m.id, on: box.checked }));

    const text = el('div', 'mod-text');
    const title = el('div', 'mod-title');
    title.append(el('strong', '', m.name), el('code', '', m.id));
    if (m.version) title.append(el('span', 'mod-badge', `v${m.version}`));
    if (m.status === 'loaded') title.append(el('span', 'mod-badge mod-badge-ok', 'loaded'));
    else if (m.status === 'failed') title.append(el('span', 'mod-badge mod-badge-bad', 'not loaded'));
    else if (m.status === 'broken') title.append(el('span', 'mod-badge mod-badge-bad', 'broken'));
    else if (m.status === 'new') title.append(el('span', 'mod-badge', 'new'));
    text.append(title);
    if (m.description) text.append(el('p', 'mod-description', m.description));
    if (m.reason) text.append(el('p', 'mods-problem', m.reason));
    for (const n of m.notes) text.append(el('p', 'mods-warning', n));
    const meta = [m.authors.length ? `By ${m.authors.join(', ')}` : '', m.file].filter(Boolean).join(' · ');
    text.append(el('div', 'mod-meta', meta));

    const order = el('div', 'mod-order');
    /** @param {string} label @param {string} name @param {number} by @param {boolean} disabled */
    const move = (label, name, by, disabled) => {
      const button = el('button', 'button', label);
      button.type = 'button';
      button.title = name;
      button.setAttribute('aria-label', `${name}: ${m.name}`);
      button.disabled = disabled;
      button.addEventListener('click', () => changeUser('move_profile_mod', { mod: m.id, by }));
      return button;
    };
    order.append(move('▲', 'Load earlier', -1, index === 0), move('▼', 'Load later', 1, index === count - 1));
    node.append(box, text, order);
    return node;
  };

  /** The selected profile's id, or null for a server install. */
  const profileId = () => {
    const t = selected();
    return t?.kind === 'profile' ? t.id : null;
  };

  /** @param {string} command @param {Record<string, unknown>} args */
  const changeUser = (command, args) =>
    run(async () => {
      const id = profileId();
      if (!id) return;
      userList.inert = true;
      try {
        renderUser(parseUserMods(await invoke(command, { id, ...args })));
      } catch (error) {
        notice(brand(errorMessage(error)), 'error');
        await loadUser();
      } finally {
        userList.inert = false;
      }
    });

  const loadUser = async () => {
    const id = profileId();
    userSection.hidden = !id;
    userShown = null;
    if (!id) return;
    try {
      renderUser(parseUserMods(await invoke('list_profile_mods', { id })));
    } catch (error) {
      userList.replaceChildren();
      userConflicts.replaceChildren();
      userNote.textContent = '';
      userConflicts.append(el('p', 'mods-problem', brand(errorMessage(error))));
    }
  };

  openFolder.addEventListener('click', () =>
    run(async () => {
      const id = profileId();
      if (id) await invoke('open_mods_folder', { id });
    }),
  );
  checkButton.addEventListener('click', () =>
    run(async () => {
      const id = profileId();
      if (!id) return;
      checkButton.disabled = true;
      try {
        const list = parseUserMods(await invoke('check_profile_mods', { id }));
        renderUser(list);
        const loaded = list.mods.filter((m) => m.status === 'loaded').length;
        const trouble = list.mods.filter((m) => m.status === 'failed' || m.status === 'broken' || m.notes.length).length;
        notice(
          `${loaded} mod${loaded === 1 ? '' : 's'} would load` +
            (list.conflicts.length ? `, ${list.conflicts.length} conflict${list.conflicts.length === 1 ? '' : 's'}` : '') +
            (trouble ? `; ${trouble} with problems (see the list)` : '') +
            '.',
          trouble ? 'error' : 'info',
        );
      } catch (error) {
        notice(brand(errorMessage(error)), 'error');
      } finally {
        checkButton.disabled = locked();
      }
    }),
  );

  /** @param {ReturnType<typeof parseList>} list */
  const render = (list) => {
    shown = list;
    server = selected()?.kind === 'server';
    wasLocked = !server && locked();
    problems.replaceChildren();
    for (const e of list.errors) problems.append(el('p', 'mods-problem', brand(e)));
    for (const w of list.warnings) problems.append(el('p', 'mods-warning', brand(w)));
    const on = list.toggles.filter((t) => t.on).length;
    note.textContent = list.toggles.length
      ? `${on} of ${list.toggles.length} patch${list.toggles.length === 1 ? '' : 'es'} on. ` +
        (server ? 'Changes apply when the server restarts.' : 'Changes apply the next time you press Play.')
      : server
        ? 'This server install has no patchsets (starforge/patchsets is empty).'
        : 'This profile has no patches.';
    if (!on && list.toggles.length) note.textContent += ` No patches will be applied to the ${server ? 'server' : 'game'}.`;
    if (wasLocked && !server) note.textContent = 'The game is running: switches can change again once it exits.';

    listNode.replaceChildren();
    let group = '';
    for (const t of list.toggles) {
      const key = `${t.module}\n${t.source}`;
      if (key !== group) {
        group = key;
        const heading = el('h2', 'mods-group');
        heading.append(el('span', 'mods-module', t.module), el('span', '', sourceLabel(t)));
        if (t.build) heading.append(el('small', '', `for build ${t.build}`));
        listNode.append(heading);
      }
      listNode.append(row(t));
    }
  };

  /** @param {Toggle} t */
  const row = (t) => {
    const node = el('div', 'mod-row');
    node.dataset.on = String(t.on);
    const box = document.createElement('input');
    box.type = 'checkbox';
    box.checked = t.on;
    box.disabled = wasLocked;
    box.setAttribute('aria-label', `${t.title || t.id}: ${t.on ? 'on' : 'off'}`);
    box.addEventListener('change', () => change(t, box.checked));
    const text = el('div', 'mod-text');
    const title = el('div', 'mod-title');
    title.append(el('strong', '', t.title || t.id), el('code', '', t.id));
    if (t.optional) title.append(el('span', 'mod-badge', 'optional'));
    if (t.chosen !== null) title.append(el('span', 'mod-badge mod-badge-chosen', 'your choice'));
    text.append(title);
    if (t.description) {
      // Descriptions are hard-wrapped TOML text: unwrap each paragraph, then show the first
      // sentence and keep the rest (often RE notes) behind a click.
      const prose = t.description
        .trim()
        .split(/\n\s*\n/)
        .map((p) => p.replace(/\s*\n\s*/g, ' '))
        .join('\n\n');
      const end = prose.search(/[.!?](\s|$)/);
      const first = end < 0 ? prose : prose.slice(0, end + 1);
      const rest = prose.slice(first.length).trim();
      if (rest) {
        const details = document.createElement('details');
        const summary = document.createElement('summary');
        summary.textContent = first;
        details.append(summary, el('p', '', rest));
        text.append(details);
      } else {
        text.append(el('p', 'mod-description', first));
      }
    }
    const meta = el('div', 'mod-meta', `Default: ${t.defaultOn ? 'on' : 'off'}`);
    if (t.chosen !== null) {
      const reset = el('button', 'link-button', 'Back to default');
      reset.type = 'button';
      reset.disabled = wasLocked;
      reset.addEventListener('click', () => change(t, null));
      meta.append(' · ', reset);
    }
    text.append(meta);
    if (t.clashing.length)
      text.append(
        el('p', 'mods-problem', `Clashes with ${t.clashing.join(', ')}, which is also on. Turn one off, or the patches won't apply.`),
      );
    else if (t.conflicts.length)
      text.append(el('p', 'mod-meta', `Can't be on together with ${t.conflicts.join(', ')}.`));
    node.append(box, text);
    return node;
  };

  const fill = async () => {
    const all = profiles.list();
    const keep = pending ?? (target.value || `profile:${profiles.current()?.id ?? ''}`);
    pending = null;
    /** @type {import('./servers.js').ServerInfo[]} */
    let hosted = [];
    try {
      hosted = array(await invoke('list_servers'), parseServer);
    } catch (error) {
      notice(brand(errorMessage(error)), 'error');
    }
    target.replaceChildren();
    const group = document.createElement('optgroup');
    group.label = 'Profiles';
    for (const p of all) group.append(new Option(brand(p.name), `profile:${p.id}`));
    target.append(group);
    if (hosted.length) {
      const servers = document.createElement('optgroup');
      servers.label = 'Server installs';
      for (const h of hosted) servers.append(new Option(h.name, `server:${h.id}`));
      target.append(servers);
    }
    if ([...target.options].some((o) => o.value === keep)) target.value = keep;
    target.disabled = !target.options.length;
  };

  /** The selected target as the backend's kind and id. */
  const selected = () => {
    const [kind, ...rest] = target.value.split(':');
    return kind && rest.length ? { kind, id: rest.join(':') } : null;
  };

  const load = async () => {
    const t = selected();
    await loadUser();
    if (!t) {
      listNode.replaceChildren();
      problems.replaceChildren();
      note.textContent = 'Add a server profile or a server install first.';
      return;
    }
    try {
      render(parseList(await invoke('list_mods', t)));
    } catch (error) {
      listNode.replaceChildren();
      note.textContent = '';
      problems.replaceChildren(el('p', 'mods-problem', brand(errorMessage(error))));
    }
  };

  /** @param {Toggle} t @param {boolean | null} on */
  const change = (t, on) =>
    run(async () => {
      const where = selected();
      if (!where) return;
      listNode.inert = true;  // one change at a time
      try {
        render(parseList(await invoke('set_mod', { ...where, module: t.module, patch: t.id, on })));
        if (where.kind === 'profile') await profiles.refresh(profiles.current()?.id);
      } catch (error) {
        notice(brand(errorMessage(error)), 'error');
        await load();
      } finally {
        listNode.inert = false;
      }
    });

  target.addEventListener('change', () => run(load));

  return {
    /** The game started or stopped: grey the switches out, or back in. */
    update: () => {
      const now = locked();
      if (shown && now !== wasLocked) render(shown);
      // A start writes a new report (what loaded, conflicts): read it again when the game starts
      // or stops.
      if (userShown && now !== userLocked) {
        userLocked = now;
        run(loadUser);
      }
    },
    /** Called when the page is shown: follows the profile picked on the play page. */
    show: () =>
      run(async () => {
        target.value = '';
        await fill();
        await load();
      }),
    /** Opens the page on a server install's patches next time it's shown. @param {string} id */
    pickServer: (id) => (pending = `server:${id}`),
  };
}
