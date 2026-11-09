/** @typedef {'checking' | 'ready' | 'launching' | 'running' | 'failed'} LauncherPhase */
/** @typedef {'retry' | 'launch' | 'fix'} LauncherAction */
/**
 * @typedef {{
 *   revision: number,
 *   phase: LauncherPhase,
 *   primaryAction: LauncherAction | null,
 *   busy: boolean,
 *   blockingReason: string | null,
 *   fixLabel: string | null,
 *   gameDirectory: string | null,
 *   proton: string | null,
 * }} LauncherSnapshot
 */

/** @param {unknown} value */
function record(value) {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw new Error('Invalid launcher snapshot: expected an object');
  }
  return /** @type {Record<string, unknown>} */ (value);
}

/** @param {unknown} value @returns {LauncherPhase} */
function phase(value) {
  switch (value) {
    case 'checking':
    case 'ready':
    case 'launching':
    case 'running':
    case 'failed':
      return value;
    default:
      throw new Error('Invalid launcher snapshot: phase');
  }
}

/** @param {unknown} value @returns {LauncherAction | null} */
function action(value) {
  switch (value) {
    case null:
    case 'retry':
    case 'launch':
    case 'fix':
      return value;
    default:
      throw new Error('Invalid launcher snapshot: action');
  }
}

/** @param {unknown} value */
function optionalText(value) {
  if (value === null || value === undefined) return null;
  if (typeof value !== 'string') throw new Error('Invalid launcher snapshot: text');
  return value;
}

/** Validate the native boundary before publishing any state to the UI.
 * @param {unknown} input @returns {LauncherSnapshot}
 */
export function parseSnapshot(input) {
  const value = record(input);
  if (
    typeof value.revision !== 'number' ||
    !Number.isSafeInteger(value.revision) ||
    value.revision < 0 ||
    typeof value.busy !== 'boolean'
  ) {
    throw new Error('Invalid launcher snapshot: field type');
  }
  return Object.freeze({
    revision: value.revision,
    phase: phase(value.phase),
    primaryAction: action(value.primaryAction),
    busy: value.busy,
    blockingReason: optionalText(value.blockingReason),
    fixLabel: optionalText(value.fixLabel),
    gameDirectory: optionalText(value.gameDirectory),
    proton: optionalText(value.proton),
  });
}

/** @param {typeof import('./bridge.js').launcherBridge} bridge */
export function createLauncherStore(bridge) {
  /** @type {LauncherSnapshot | null} */
  let snapshot = null;
  let pending = false;
  /** @type {Set<() => void>} */
  const listeners = new Set();
  /** @type {(() => void) | undefined} */
  let unlisten;
  const publish = () => listeners.forEach((listener) => listener());
  /** @param {unknown} input */
  const receive = (input) => {
    const next = parseSnapshot(input);
    if (snapshot && next.revision <= snapshot.revision) return;
    snapshot = next;
    publish();
  };
  return {
    getSnapshot: () => snapshot,
    isRequestPending: () => pending,
    /** @param {() => void} listener */
    subscribe(listener) {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
    async start() {
      unlisten = await bridge.listen(receive);
      try {
        receive(await bridge.getSnapshot());
      } catch (error) {
        unlisten();
        throw error;
      }
    },
    dispose() {
      unlisten?.();
      listeners.clear();
    },
    /** @param {LauncherAction} requested */
    async performAction(requested) {
      if (pending || snapshot?.busy) throw new Error('An action is already in progress');
      if (!snapshot || requested !== snapshot.primaryAction)
        throw new Error('This action is not available');
      pending = true;
      publish();
      try {
        await bridge.performAction(requested);
      } finally {
        pending = false;
        publish();
      }
    },
  };
}
