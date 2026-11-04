// Browser workspace storage. The core uses ordinary files; here they live in
// Emscripten's file system, persisted to IndexedDB (IDBFS) after every change.
import type { FlowsModule } from './module';

export const root = '/flows';
export const workspacePath = `${root}/workspace`;
export const settingsPath = `${root}/settings`;

function sync(module: FlowsModule, populate: boolean): Promise<void> {
  return new Promise((resolve, reject) => module.FS.syncfs(populate, (error: unknown) => (error ? reject(error) : resolve())));
}

/** Mounts persistent storage. Falls back to memory if IndexedDB is unavailable. */
export async function mountWorkspace(module: FlowsModule): Promise<'browser' | 'memory'> {
  const FS = module.FS;
  FS.mkdir(root);
  let storage: 'browser' | 'memory' = 'browser';
  try {
    FS.mount(module.IDBFS, { autoPersist: true }, root);
    await sync(module, true);
  } catch {
    // Private windows or blocked storage: keep working for this session.
    try { FS.unmount(root); } catch { /* not mounted */ }
    storage = 'memory';
  }
  for (const path of [workspacePath, settingsPath]) if (!FS.analyzePath(path).exists) FS.mkdir(path);
  return storage;
}

/** Writes pending changes to IndexedDB (before the page hides or hands off). */
export function flushWorkspace(module: FlowsModule): Promise<void> {
  return sync(module, false).catch(() => {});
}

export async function storageEstimate(): Promise<{ persisted: boolean; usage?: number; quota?: number }> {
  const manager = navigator.storage;
  const [persisted, estimate] = await Promise.all([manager?.persisted?.() ?? false, manager?.estimate?.() ?? undefined]);
  return { persisted, usage: estimate?.usage, quota: estimate?.quota };
}
