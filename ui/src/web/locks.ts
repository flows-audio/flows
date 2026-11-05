// One tab owns the browser workspace at a time (Web Locks). A second tab can
// take over: the owner saves, stops and releases, then the new tab starts.
const lockName = 'flows-workspace';

export interface WorkspaceClaim {
  /** Ends ownership (page unload or handoff). */
  release(): void;
}
export interface ClaimHandlers {
  /** Another tab owns the workspace. Call takeOver() when the user chooses to. */
  busy(takeOver: () => void): void;
  /** Another tab took over; save and stop before resolving. */
  lost(): Promise<void>;
}

export function claimWorkspace(handlers: ClaimHandlers): Promise<WorkspaceClaim> {
  const locks = navigator.locks;
  const channel = typeof BroadcastChannel === 'function' ? new BroadcastChannel(lockName) : undefined;
  // Without Web Locks (very old browsers) there is no cross-tab coordination.
  if (!locks) return Promise.resolve({ release: () => channel?.close() });
  return new Promise(resolve => {
    let releaseLock: (() => void) | undefined;
    let ended = false;
    const end = async () => {
      if (ended) return;
      ended = true;
      await handlers.lost();
      releaseLock?.();
      channel?.close();
    };
    const hold = (lock: Lock | null) => {
      if (!lock) return undefined;
      return new Promise<void>(done => {
        releaseLock = done;
        if (channel) channel.onmessage = event => { if (event.data?.type === 'handoff') void end(); };
        resolve({ release: () => { ended = true; done(); channel?.close(); } });
      });
    };
    // A stolen lock rejects the holder's request; treat it as a handoff.
    const watch = (request: Promise<unknown>) => request.catch(() => void end());
    watch(locks.request(lockName, { ifAvailable: true }, lock => {
      if (lock) return hold(lock);
      handlers.busy(() => {
        channel?.postMessage({ type: 'handoff' });
        const controller = new AbortController();
        const timer = setTimeout(() => controller.abort(), 3000);
        let granted = false;
        locks.request(lockName, { signal: controller.signal }, lock => { granted = true; clearTimeout(timer); return hold(lock); })
          .catch(error => {
            // Once held, a rejection means a later tab stole the lock: a handoff.
            if (granted) void end();
            // Before that, AbortError is the timeout: the owner did not answer.
            else if (error?.name === 'AbortError') watch(locks.request(lockName, { steal: true }, hold));
          });
      });
      return undefined;
    }));
  });
}
