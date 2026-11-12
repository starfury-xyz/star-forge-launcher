import { invoke } from './bridge.js';
import { array, boolean, object, string } from './contracts.js';
import { brand, control, el, errorMessage, notice, required, run } from './ui.js';

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

/** Where a patch comes from, for a group heading. @param {Toggle} t */
const sourceLabel = (t) =>
  t.source === 'core' ? 'Core patches (ship with STAR FORGE)' : `From the pack (${t.source})`;

/**
 * The Mods page: every patch of the chosen profile with an on/off switch.
 * @param {ReturnType<typeof import('./profiles.js').bindProfiles>} profiles
 * @param {() => boolean} locked the game is starting or running
 */
export function bindMods(profiles, locked) {
  const target = control('mods-target', HTMLSelectElement);
  const listNode = required('mods-list');
  const problems = required('mods-problems');
  const note = required('mods-note');

  /** The list on screen, redrawn when the game starts or stops. @type {ReturnType<typeof parseList> | null} */
  let shown = null;
  let wasLocked = false;

  /** @param {ReturnType<typeof parseList>} list */
  const render = (list) => {
    shown = list;
    wasLocked = locked();
    problems.replaceChildren();
    for (const e of list.errors) problems.append(el('p', 'mods-problem', brand(e)));
    for (const w of list.warnings) problems.append(el('p', 'mods-warning', brand(w)));
    const on = list.toggles.filter((t) => t.on).length;
    note.textContent = list.toggles.length
      ? `${on} of ${list.toggles.length} patch${list.toggles.length === 1 ? '' : 'es'} on. Changes apply the next time you press Play.`
      : 'This profile has no patches.';
    if (!on && list.toggles.length) note.textContent += ' No patches will be applied to the game.';
    if (wasLocked) note.textContent = 'The game is running: switches can change again once it exits.';

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
    box.disabled = locked();
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
      reset.disabled = locked();
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

  const fill = () => {
    const all = profiles.list();
    const keep = target.value || profiles.current()?.id || '';
    target.replaceChildren();
    const group = document.createElement('optgroup');
    group.label = 'Profiles';
    for (const p of all) group.append(new Option(brand(p.name), `profile:${p.id}`));
    target.append(group);
    const wanted = keep.includes(':') ? keep : `profile:${keep}`;
    if ([...target.options].some((o) => o.value === wanted)) target.value = wanted;
    target.disabled = !all.length;
  };

  /** The selected target as the backend's kind and id. */
  const selected = () => {
    const [kind, ...rest] = target.value.split(':');
    return kind && rest.length ? { kind, id: rest.join(':') } : null;
  };

  const load = async () => {
    const t = selected();
    if (!t) {
      listNode.replaceChildren();
      problems.replaceChildren();
      note.textContent = 'Add a server profile first.';
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
      if (shown && locked() !== wasLocked) render(shown);
    },
    /** Called when the page is shown: follows the profile picked on the play page. */
    show: () => {
      target.value = '';
      fill();
      run(load);
    },
  };
}
