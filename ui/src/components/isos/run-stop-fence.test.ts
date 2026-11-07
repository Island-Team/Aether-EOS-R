import { describe, expect, it, vi } from 'vitest';
import type { ProcessStatus } from '../../services/isos/node-graph';
import { beginRunStopFence, observeRunStopFence, requestRunStop, settleRunStopFence } from './run-stop-fence';

function owner(runId: string | null, state: 'running' | 'stopped' = 'running', identity: ProcessStatus['run_id_state'] = runId ? 'known' : 'none'): ProcessStatus {
  return { schema_version: '1.0.0', state, run_id: runId, run_id_state: identity, processes: [] };
}

describe('workspace Stop fence', () => {
  it('fences all pages during a pending Stop despite stale Home A and current EOS B', () => {
    const pending = beginRunStopFence('A', owner('B'));
    expect(pending.knownRuns).toEqual(['A', 'B']);
    expect(pending.pendingRequests).toBe(1);
    expect(observeRunStopFence(pending, owner('C'))).toBe(pending);
    const settled = settleRunStopFence(pending, 'B');
    expect(observeRunStopFence(settled, owner('B'))).toBe(settled);
    expect(observeRunStopFence(settled, owner(null, 'running', 'unavailable'))).toBe(settled);
    expect(observeRunStopFence(settled, owner('C'))).toBeNull();
  });
  it('requires a stopped observation when the original run identity was unknown', () => {
    const pending = beginRunStopFence(null, null);
    const uncertain = settleRunStopFence(pending, null);
    expect(observeRunStopFence(uncertain, owner('C'))).toBe(uncertain);
    const stopped = observeRunStopFence(uncertain, owner(null, 'stopped'))!;
    expect(stopped.sawStopped).toBe(true);
    expect(observeRunStopFence(stopped, owner('C'))).toBeNull();
    expect(beginRunStopFence('C', owner('C'), stopped).sawStopped).toBe(false);
  });
  it('keeps a second Stop pending when the first response arrives late', () => {
    const first = beginRunStopFence('A', owner('B'));
    const second = beginRunStopFence('B', owner('B'), first);
    const firstReply = settleRunStopFence(second, 'A');
    expect(firstReply.phase).toBe('pending');
    expect(observeRunStopFence(firstReply, owner('C'))).toBe(firstReply);
    expect(settleRunStopFence(firstReply, 'B').phase).toBe('settled');
  });
  it('uses the owner terminal run when the page and shell had no identity', () => {
    const uncertain = settleRunStopFence(beginRunStopFence(null, null), 'B');
    expect(observeRunStopFence(uncertain, owner('B'))).toBe(uncertain);
    expect(observeRunStopFence(uncertain, owner('C'))).toBeNull();
  });
  it('retains a failed Stop fence when a delayed Run gets a new identity', () => {
    const failed = settleRunStopFence(beginRunStopFence('A', owner('A')), null, false);
    expect(observeRunStopFence(failed, owner('B'))).toBe(failed);
    expect(observeRunStopFence(failed, owner('B', 'stopped'))).toBe(failed);
    const confirmed = settleRunStopFence(beginRunStopFence('B', owner('B'), failed), 'B', true);
    const observed = observeRunStopFence(confirmed, owner('B', 'stopped'))!;
    expect(observed.sawStopped).toBe(true);
    expect(observeRunStopFence(observed, owner('C'))).toBeNull();
  });
  it('publishes Graph Stop intent synchronously before the owner request, including an unknown run', async () => {
    const order: string[] = [];
    let release!: (value: string) => void;
    const request = vi.fn(() => new Promise<string>((resolve) => { order.push('request'); release = resolve; }));
    const intent = vi.fn((runId: string | null) => { order.push(`intent:${runId ?? 'unknown'}`); });
    const result = requestRunStop(owner('A'), intent, request);
    expect(order).toEqual(['intent:A', 'request']);
    release('done');
    expect(await result).toBe('done');
    requestRunStop(null, intent, () => Promise.resolve('done'));
    expect(intent).toHaveBeenLastCalledWith(null);
  });
});
