// Browser workspace storage. The core uses ordinary files; here they live in
// Emscripten's file system, persisted to IndexedDB (IDBFS) after every change.
import type { FlowsModule } from './module';

export const root = '/flows';
export const workspacePath = `${root}/workspace`;
export const settingsPath = `${root}/settings`;

function sync(module: FlowsModule, populate: boolean): Promise<void> {
  return new Promise((resolve, reject) => module.FS.syncfs(populate, (error: unknown) => (error ? reject(error) : resolve())));
}

// Saving makes IndexedDB match the tree in memory, deleting what memory lacks.
// IDBFS autoPersist saves after every change, including the files the initial
// load creates, so it is held until the load finishes (a partly loaded tree
// would delete the rest), and for good once another tab owns the workspace.
let loaded = false;
let released = false;
let saving = false;
const saved: (() => void)[] = [];
const hold = () => {};
function idle(): Promise<void> {
  return saving ? new Promise(resolve => saved.push(resolve)) : Promise.resolve();
}

/** Mounts persistent storage. Falls back to memory if IndexedDB is unavailable. */
export async function mountWorkspace(module: FlowsModule): Promise<'browser' | 'memory'> {
  const { FS, IDBFS } = module;
  FS.mkdir(root);
  let storage: 'browser' | 'memory' = 'browser';
  const queuePersist = IDBFS.queuePersist;
  // Holding saves relies on Emscripten's IDBFS calling this by name.
  if (typeof queuePersist !== 'function') throw new Error('This engine build cannot hold browser storage saves.');
  IDBFS.onAutoPersistStateChanged = (active: boolean) => {
    saving = active;
    if (!active) saved.splice(0).forEach(resolve => resolve());
  };
  IDBFS.queuePersist = hold;
  try {
    FS.mount(IDBFS, { autoPersist: true }, root);
    await sync(module, true);
  } catch {
    // Private windows or blocked storage: keep working for this session.
    try { FS.unmount(root); } catch { /* not mounted */ }
    storage = 'memory';
  } finally {
    if (!released) IDBFS.queuePersist = queuePersist;
  }
  for (const path of [workspacePath, settingsPath]) if (!FS.analyzePath(path).exists) FS.mkdir(path);
  loaded = true;
  return storage;
}

/** Writes pending changes to IndexedDB before the page hides. */
export function flushWorkspace(module: FlowsModule): Promise<void> {
  if (!loaded || released) return Promise.resolve();
  return sync(module, false).catch(() => {});
}

/** Another tab takes over: saves this tab's changes once, then stops this tab
 * writing to IndexedDB. A tab still loading has nothing of its own to save. */
export async function releaseWorkspace(module: FlowsModule): Promise<void> {
  if (released) return;
  released = true;
  module.IDBFS.queuePersist = hold;
  if (!loaded) return;
  await idle();
  await sync(module, false).catch(() => {});
  // A save scheduled just before the hold still starts; let it finish.
  await new Promise(resolve => setTimeout(resolve, 0));
  await idle();
}

export async function storageEstimate(): Promise<{ persisted: boolean; usage?: number; quota?: number }> {
  const manager = navigator.storage;
  const [persisted, estimate] = await Promise.all([manager?.persisted?.() ?? false, manager?.estimate?.() ?? undefined]);
  return { persisted, usage: estimate?.usage, quota: estimate?.quota };
}
