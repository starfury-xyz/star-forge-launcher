import { invoke } from './bridge.js';
import { array, number, object, string } from './contracts.js';
import { control, el, errorMessage, notice, openPanel, required, run } from './ui.js';

/** @param {unknown} item */
export function parseServer(item) {
  const value = object(item);
  return { id: string(value.id), name: string(value.name), cloud: string(value.cloud) };
}
/** @typedef {ReturnType<typeof parseServer>} ServerInfo */

/** @param {unknown} value */
function parseStatus(value) {
  const s = object(value);
  const strings = (/** @type {unknown} */ v) => array(v, string);
  return {
    server: parseServer(s.server),
    run: (() => {
      const r = object(s.run);
      return { state: string(r.state), started: number(r.started, 0, 1e13), mongoPort: number(r.mongoPort, 0, 65535) };
    })(),
    proxy: string(s.proxy),
    proxyBuild: s.proxyBuild === null ? null : string(s.proxyBuild),
    onError: s.onError === null ? null : string(s.onError),
    exes: array(s.exes, (item) => {
      const e = object(item);
      return {
        module: string(e.module),
        build: string(e.build),
        sha1: e.sha1 === null ? null : string(e.sha1),
        ok: strings(e.ok),
        skipped: strings(e.skipped),
        warnings: strings(e.warnings),
        errors: strings(e.errors),
      };
    }),
    errors: strings(s.errors),
    patches: number(s.patches, 0, 100000),
    patchesOn: number(s.patchesOn, 0, 100000),
  };
}

/** What the proxy DLL's state means, and whether it's a problem. @type {Record<string, [string, boolean]>} */
const proxyText = {
  installed: ['Installed', false],
  stock: ['Not installed: the game’s own libcrypto-1_1_tg.dll, so no server patches apply', true],
  restored: ['Not active: Steam put the game’s libcrypto-1_1_tg.dll back (“verify files” or an update); install the patcher again', true],
  broken: ['Broken: the patcher without the game’s original libcrypto-1_1_tg.dll; verify the game files in Steam', true],
  missing: ['No libcrypto-1_1_tg.dll here: is this the game’s cloud folder?', true],
};

/** What the stack's state means, and whether it's a problem. @type {Record<string, [string, boolean]>} */
const runText = {
  running: ['Running', false],
  stopped: ['Stopped', false],
  stale: ['Not responding: its supervisor stopped without cleaning up (crashed or killed?); Stop clears it', true],
};

/**
 * The Servers page: the player's server installs and what each would do at startup.
 * @param {(serverId: string) => void} openMods shows the Mods page for a server install
 */
export function bindServers(openMods) {
  const listNode = required('servers-list');
  const detail = required('server-detail');
  const dialog = control('server-dialog', HTMLDialogElement);
  const form = control('server-form', HTMLFormElement);
  const nameInput = control('server-name-input', HTMLInputElement);
  const cloudInput = control('server-cloud-input', HTMLInputElement);
  const useGame = control('server-use-game', HTMLButtonElement);
  const formError = required('server-error');
  const confirm = control('server-confirm', HTMLDialogElement);
  /** @type {ServerInfo[]} */
  let all = [];
  /** @type {string | null} */
  let selected = null;
  /** @type {string | null} */
  let gameCloud = null;

  const renderList = () => {
    listNode.replaceChildren();
    if (!all.length) listNode.append(el('p', 'mods-note', 'No server installs yet.'));
    for (const s of all) {
      const card = el('button', 'server-card');
      card.type = 'button';
      if (s.id === selected) card.setAttribute('aria-current', 'true');
      card.append(el('strong', '', s.name), el('small', '', s.cloud));
      card.addEventListener('click', () => select(s.id));
      listNode.append(card);
    }
  };

  /** Set while a start or stop is under way: the state it should reach. @type {string | null} */
  let pending = null;
  /** @type {number | undefined} */
  let pollTimer;

  /** @param {ReturnType<typeof parseStatus>} st */
  const renderRun = (st) => {
    const runNode = required('server-run');
    const [text, bad] = runText[st.run.state] ?? [st.run.state, true];
    let line = text;
    if (st.run.state === 'running' && st.run.started)
      line += ` since ${new Date(st.run.started * 1000).toLocaleString()} (MongoDB on port ${st.run.mongoPort})`;
    if (pending) line = pending === 'running' ? 'Starting…' : 'Stopping…';
    runNode.textContent = line;
    runNode.dataset.bad = String(bad && !pending);
    const start = control('server-start', HTMLButtonElement);
    const stop = control('server-stop', HTMLButtonElement);
    start.hidden = st.run.state === 'running';
    stop.hidden = st.run.state === 'stopped';
    start.disabled = stop.disabled = pending !== null;
    // Changing the patcher or patches while the stack runs only applies on the next start.
    control('server-install', HTMLButtonElement).disabled ||= st.run.state === 'running';
    control('server-uninstall', HTMLButtonElement).disabled = st.run.state === 'running';
  };

  /** @param {ReturnType<typeof parseStatus>} st */
  const renderStatus = (st) => {
    required('server-name').textContent = st.server.name;
    required('server-cloud').textContent = st.server.cloud;
    const [proxy, proxyBad] = proxyText[st.proxy] ?? [st.proxy, true];
    const proxyNode = required('server-proxy');
    proxyNode.textContent = proxy;
    proxyNode.dataset.bad = String(proxyBad);
    const install = control('server-install', HTMLButtonElement);
    install.hidden = !['stock', 'restored', 'installed'].includes(st.proxy);
    install.textContent = st.proxy === 'installed' ? 'Update patcher' : 'Install patcher';
    install.disabled = !st.proxyBuild;
    install.title = st.proxyBuild
      ? `Copies in ${st.proxyBuild}; the game’s own DLL is kept. Restart the server afterwards.`
      : 'No patcher build found: it goes in server/ beside the launcher’s tools, or set STAR_FORGE_SERVER_PROXY';
    required('server-uninstall').hidden = !['installed', 'restored'].includes(st.proxy);
    renderRun(st);
    required('server-on-error').textContent =
      st.onError === 'continue' ? 'Start anyway, with whatever patches match' : 'Refuse to start (recommended)';
    required('server-patches').textContent = st.patches
      ? `${st.patchesOn} of ${st.patches} on`
      : 'No patchsets installed in starforge/patchsets';
    const problems = required('server-problems');
    problems.replaceChildren();
    for (const e of st.errors) problems.append(el('p', 'mods-problem', e));
    const exes = required('server-exes');
    exes.replaceChildren();
    for (const e of st.exes) {
      const row = el('div', 'server-exe');
      const head = el('div', 'mod-title');
      head.append(el('strong', 'mods-module', e.module));
      if (e.build) head.append(el('code', '', `build ${e.build}`));
      const fine = !e.errors.length;
      head.append(el('span', fine ? 'mod-badge mod-badge-chosen' : 'mod-badge mod-badge-bad', fine ? 'ready' : 'won’t start'));
      row.append(head);
      if (fine) row.append(el('p', 'mod-meta', `${e.ok.length} patch${e.ok.length === 1 ? '' : 'es'} will apply.`));
      for (const x of e.errors) row.append(el('p', 'mods-problem', x));
      for (const x of e.skipped) row.append(el('p', 'mods-warning', `Optional patch skipped: ${x}`));
      for (const x of e.warnings) row.append(el('p', 'mods-warning', x));
      exes.append(row);
    }
    detail.hidden = false;
  };

  // --- logs ---
  const logSelect = control('server-log-select', HTMLSelectElement);
  const logText = required('server-log-text');
  const logNote = required('server-log-note');
  const followLog = control('server-log-follow', HTMLInputElement);
  /** @type {number | undefined} */
  let logTimer;

  /** Shows the chosen log's end, keeping the view at the bottom if it was there. */
  const showLog = async () => {
    const id = selected;
    const [group, name] = (logSelect.value || '/').split('/', 2);
    if (!id || !name) {
      logText.textContent = '';
      return;
    }
    const atBottom = logText.scrollTop + logText.clientHeight >= logText.scrollHeight - 4;
    try {
      const log = object(await invoke('server_log', { id, group, name }));
      if (selected !== id) return;
      const cut = log.cut === true ? '… (only the end of the log is shown)\n' : '';
      logText.textContent = cut + string(log.text);
    } catch (error) {
      logText.textContent = errorMessage(error);
    }
    if (atBottom) logText.scrollTop = logText.scrollHeight;
  };

  /** Lists the server's logs, keeping the current choice. @param {string} id */
  const loadLogs = async (id) => {
    const keep = logSelect.value;
    const list = object(await invoke('server_logs', { id }));
    if (selected !== id) return;
    logNote.hidden = list.note === null;
    logNote.textContent = list.note === null ? '' : string(list.note);
    logSelect.replaceChildren();
    /** @type {Record<string, HTMLOptGroupElement>} */
    const groups = {};
    for (const item of array(list.logs, object)) {
      const group = string(item.group);
      groups[group] ??= Object.assign(document.createElement('optgroup'), {
        label: group === 'game' ? 'Game services (newest each)' : 'STAR FORGE',
      });
      const when = new Date(number(item.modified, 0, 1e13) * 1000).toLocaleString();
      const option = new Option(`${string(item.label)} — ${when}`, `${group}/${string(item.name)}`);
      groups[group].append(option);
    }
    logSelect.append(...Object.values(groups));
    if ([...logSelect.options].some((o) => o.value === keep)) logSelect.value = keep;
    await showLog();
  };

  const scheduleLog = () => {
    window.clearTimeout(logTimer);
    if (!followLog.checked || detail.hidden) return;
    logTimer = window.setTimeout(() => run(async () => {
      await showLog();
      scheduleLog();
    }), 2000);
  };

  logSelect.addEventListener('change', () => run(showLog));
  followLog.addEventListener('change', scheduleLog);
  required('server-log-refresh').addEventListener('click', () => {
    const id = selected;
    if (id) run(() => loadLogs(id));
  });
  required('server-game-logs').addEventListener('click', () => {
    const id = selected;
    if (id) run(() => invoke('open_server_folder', { id, which: 'game-logs' }));
  });

  /** @param {string} id */
  const select = (id) =>
    run(async () => {
      selected = id;
      renderList();
      detail.hidden = true;
      try {
        renderStatus(parseStatus(await invoke('server_status', { id })));
        await loadLogs(id);
        scheduleLog();
      } catch (error) {
        notice(errorMessage(error), 'error');
      }
    });

  /** @param {string | null} [prefer] */
  const refresh = async (prefer) => {
    all = array(await invoke('list_servers'), parseServer);
    const keep = prefer ?? selected;
    selected = all.some((s) => s.id === keep) ? keep : (all[0]?.id ?? null);
    renderList();
    if (selected) select(selected);
    else detail.hidden = true;
  };

  required('add-server').addEventListener('click', () =>
    run(async () => {
      form.reset();
      formError.hidden = true;
      const found = await invoke('suggest_server_cloud');
      gameCloud = typeof found === 'string' && !all.some((s) => s.cloud === found) ? found : null;
      useGame.hidden = !gameCloud;
      if (gameCloud) {
        cloudInput.value = gameCloud;
        nameInput.value = 'Local server';
      }
      openPanel(dialog);
    }),
  );
  useGame.addEventListener('click', () => {
    if (gameCloud) cloudInput.value = gameCloud;
  });
  required('server-browse').addEventListener('click', () =>
    run(async () => {
      const dir = await invoke('pick_folder', { title: 'Server cloud folder', start: cloudInput.value || null });
      if (typeof dir === 'string') cloudInput.value = dir;
    }),
  );
  form.addEventListener('submit', (event) => {
    event.preventDefault();
    run(async () => {
      try {
        const added = parseServer(await invoke('add_server', { name: nameInput.value, cloud: cloudInput.value }));
        dialog.close();
        await refresh(added.id);
        notice(`Added “${added.name}”.`, 'success');
      } catch (error) {
        formError.textContent = errorMessage(error);
        formError.hidden = false;
      }
    });
  });

  required('server-mods').addEventListener('click', () => {
    if (selected) openMods(selected);
  });
  /** @param {string} command @param {string} done */
  const proxyAction = (command, done) => {
    const id = selected;
    if (!id) return;
    run(async () => {
      try {
        renderStatus(parseStatus(await invoke(command, { id })));
        notice(done, 'success');
      } catch (error) {
        notice(errorMessage(error), 'error');
      }
    });
  };
  required('server-install').addEventListener('click', () =>
    proxyAction('install_server_proxy', 'Patcher installed. Restart the server for it to take effect.'),
  );
  required('server-uninstall').addEventListener('click', () =>
    proxyAction('uninstall_server_proxy', 'The game’s own DLL is back. starforge/ is left in place.'),
  );
  // Polls until the stack reaches `want` (or a minute and a half passes).
  /** @param {string} id @param {string} want */
  const follow = (id, want) => {
    pending = want;
    window.clearTimeout(pollTimer);
    const started = Date.now();
    const tick = async () => {
      if (selected !== id) {
        pending = null;
        return;
      }
      try {
        const st = parseStatus(await invoke('server_status', { id }));
        if (st.run.state === want || Date.now() - started > 90000) {
          if (st.run.state !== want) notice('The server is taking long; see its logs.', 'error');
          pending = null;
        }
        renderStatus(st);
      } catch (error) {
        pending = null;
        notice(errorMessage(error), 'error');
        return;
      }
      if (pending) pollTimer = window.setTimeout(tick, 1500);
    };
    pollTimer = window.setTimeout(tick, 500);
  };
  required('server-start').addEventListener('click', () => {
    const id = selected;
    if (!id) return;
    run(async () => {
      try {
        await invoke('start_server', { id });
        follow(id, 'running');
      } catch (error) {
        notice(errorMessage(error), 'error');
      }
    });
  });
  required('server-stop').addEventListener('click', () => {
    const id = selected;
    if (!id) return;
    run(async () => {
      try {
        await invoke('stop_server', { id });
        follow(id, 'stopped');
      } catch (error) {
        notice(errorMessage(error), 'error');
      }
    });
  });
  required('server-recheck').addEventListener('click', () => {
    if (selected) select(selected);
  });
  required('server-folder').addEventListener('click', () => {
    const id = selected;
    if (id) run(() => invoke('open_server_folder', { id, which: 'cloud' }));
  });
  required('server-logs').addEventListener('click', () => {
    const id = selected;
    if (id) run(() => invoke('open_server_folder', { id, which: 'logs' }));
  });
  required('server-remove').addEventListener('click', () => {
    const s = all.find((x) => x.id === selected);
    if (!s) return;
    required('server-confirm-text').textContent =
      `“${s.name}” is taken off the list. Its folder and files stay as they are.`;
    confirm.showModal();
  });
  required('server-confirm-remove').addEventListener('click', () => {
    const id = selected;
    confirm.close();
    if (!id) return;
    run(async () => {
      await invoke('remove_server', { id });
      await refresh(null);
    });
  });

  return {
    /** Called when the page is shown. */
    show: () => run(() => refresh()),
  };
}
