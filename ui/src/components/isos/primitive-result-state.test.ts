import { describe, expect, it } from 'vitest';
import type { ProcessStatus } from '../../services/isos/node-graph';
import { acceptPrimitiveResult, newPrimitiveRecord, pollAllowed, primitiveError, resultFence } from './primitive-result-state';

const revision = 'a'.repeat(64);
const status: ProcessStatus = { schema_version: '1.0.0', state: 'running', run_id_state: 'known', run_id: 'run-a', saved_revision: revision, active_revision: revision, processes: [] };

describe('R07 command state', () => {
  it('keeps InProgress open, distinguishes failures, and seals only correlated terminal results', () => {
    const initial = newPrimitiveRecord('run-a', revision, 'eos/dispatcher/base/velocity', 'move-1', 1, 1000);
    const progress = acceptPrimitiveResult(initial, { command_id: 'move-1', status: 'InProgress', reason: null });
    expect(progress.phase).toBe('following');
    expect(acceptPrimitiveResult(progress, { command_id: 'other', status: 'Completed', reason: null })).toBe(progress);
    const failed = acceptPrimitiveResult(progress, { command_id: 'move-1', status: 'Failed', reason: 'device fault' });
    expect(failed).toMatchObject({ phase: 'terminal', result: { status: 'Failed', reason: 'device fault' } });
    expect(primitiveError(progress, 'transport', 'offline')).toMatchObject({ phase: 'unavailable', result: { status: 'InProgress' }, commandId: 'move-1' });
  });
  it('checks run and revision and bounds visible single-flight polling', () => {
    const pending = newPrimitiveRecord('run-a', revision, 'eos/dispatcher/base/velocity', 'move-1', 1, 1000);
    const record = acceptPrimitiveResult({ ...pending, submitAttempted: true }, { command_id: 'move-1', status: 'InProgress', reason: null }, 1000);
    expect(resultFence(record, status)).toBe('current');
    expect(resultFence(record, { ...status, run_id: 'run-b' })).toBe('old_run');
    expect(resultFence(record, { ...status, run_id_state: 'unavailable' })).toBe('unavailable');
    expect(resultFence(record, { ...status, active_revision: 'b'.repeat(64) })).toBe('old_run');
    expect(resultFence(record, { ...status, saved_revision: 'b'.repeat(64) })).toBe('old_run');
    expect(pollAllowed(pending, 2000, true, false)).toBe(false);
    expect(pollAllowed(record, 2000, true, false)).toBe(true);
    expect(pollAllowed(record, 2000, false, false)).toBe(false);
    expect(pollAllowed(record, 2000, true, true)).toBe(false);
    expect(pollAllowed({ ...record, attempts: 12 }, 2000, true, false)).toBe(false);
    expect(pollAllowed(record, 61_000, true, false)).toBe(false);
    expect(pollAllowed(primitiveError(record, 'identity_unavailable', 'unknown'), 2000, true, false)).toBe(false);
  });
});
