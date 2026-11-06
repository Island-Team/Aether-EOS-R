import type { ProcessStatus } from '../../services/isos/node-graph';

export interface RunStopFence {
  phase: 'pending' | 'settled';
  pendingRequests: number;
  knownRuns: string[];
  sawStopped: boolean;
  confirmed: boolean;
}

function knownRun(value: string | null | undefined): value is string {
  return typeof value === 'string' && /^[A-Za-z0-9_-]{1,64}$/.test(value);
}

/** A Stop intent applies to the whole workspace, including stale page projections. */
export function beginRunStopFence(callerRun: string | null, owner: ProcessStatus | null, prior: RunStopFence | null = null): RunStopFence {
  return { phase: 'pending', pendingRequests: (prior?.phase === 'pending' ? prior.pendingRequests : 0) + 1, knownRuns: [...new Set([...(prior?.knownRuns || []), callerRun, owner?.run_id].filter(knownRun))], sawStopped: false, confirmed: false };
}

export function settleRunStopFence(fence: RunStopFence, terminalRun: string | null, confirmed = true): RunStopFence {
  return { ...fence, phase: fence.pendingRequests > 1 ? 'pending' : 'settled', pendingRequests: Math.max(0, fence.pendingRequests - 1), knownRuns: [...new Set([...fence.knownRuns, terminalRun].filter(knownRun))], confirmed: fence.pendingRequests > 1 ? false : confirmed };
}

/** Only a fresh IsOS status read after settlement can clear a Stop fence. */
export function observeRunStopFence(fence: RunStopFence, status: ProcessStatus | null): RunStopFence | null {
  if (fence.phase === 'pending' || !status || status.run_id_state === 'unavailable') return fence;
  if (!fence.confirmed) return fence;
  if (status.state === 'stopped' || status.run_id_state === 'none') return fence.sawStopped ? fence : { ...fence, sawStopped: true };
  if (status.state === 'running' && status.run_id_state === 'known' && knownRun(status.run_id) &&
    (fence.sawStopped || (fence.knownRuns.length > 0 && !fence.knownRuns.includes(status.run_id)))) return null;
  return fence;
}

/** Used by Graph Stop to establish the workspace fence before the owner request begins. */
export async function requestRunStop<T>(before: ProcessStatus | null, onIntent: ((runId: string | null) => void) | undefined, request: () => Promise<T>): Promise<T> {
  onIntent?.(before?.run_id || null);
  return request();
}
