import { describe, expect, it } from 'vitest';
import { acceptCurrentLogPage, acknowledgeCurrentLogDiagnostics, emptyCurrentLogState, resetCurrentLogDetails, validCorrelationFilter, validNodeFilter } from './log-cursor-state';
import type { CurrentLogPage, LogLine } from '../../services/isos/node-graph';

const event = (code: string, run_id = 'r7'): LogLine => ({ timestamp: '2026-11-01T10:00:00.000Z', level: 'info', source: 'eos', core: 'eos', event_code: code, run_id, message: code });
const page = (overrides: Partial<CurrentLogPage> = {}): CurrentLogPage => ({
  scope: { kind: 'deployment_run', run_id: 'r7', active_revision: null },
  anchor: { kind: 'first_ready_or_failure', state: 'found', event: event('eos_ready') },
  lines: [event('deployment_ready')], next_cursor: 'lc1.next', rotated: false, diagnostics: [], ...overrides,
});

describe('P07 current-run log state', () => {
  it('keeps manager Apply/package failure ahead of filters and waits for first run event', () => {
    const manager = acceptCurrentLogPage(emptyCurrentLogState, page({
      scope: { kind: 'manager_session', run_id: 'm7', active_revision: null },
      anchor: { kind: 'first_prelaunch_failure', state: 'found', event: event('package_load_failed', 'm7') },
      lines: [],
    }));
    expect(manager.anchor?.event?.event_code).toBe('package_load_failed');
    expect(resetCurrentLogDetails(manager).anchor).toEqual(manager.anchor);
    const next = acceptCurrentLogPage(manager, page({ scope: { kind: 'deployment_run', run_id: 'r8', active_revision: 'sha' }, anchor: { kind: 'first_ready_or_failure', state: 'pending', event: null }, lines: [], next_cursor: 'lc1.pending' }));
    expect(next.boundary).toEqual({ previous: 'm7', current: 'r8' });
    expect(next.lines).toEqual([]);
    expect(next.anchor?.state).toBe('pending');
    expect(next.cursor).toBe('lc1.pending');
  });

  it('preserves rows across retained rotation and exposes an expired gap', () => {
    const first = acceptCurrentLogPage(emptyCurrentLogState, page());
    const rotated = acceptCurrentLogPage(first, page({ lines: [event('deployment_stopped')], rotated: true, next_cursor: 'lc1.rotated' }));
    expect(rotated.lines.map((line) => line.event_code)).toEqual(['deployment_ready', 'deployment_stopped']);
    const gap = acceptCurrentLogPage(rotated, page({ lines: [], rotated: true, diagnostics: [{ code: 'log_cursor_expired', count: null }] }));
    expect(gap.diagnostics).toEqual([{ code: 'log_cursor_expired', count: null }]);
    expect(acceptCurrentLogPage(gap, page({ lines: [] })).diagnostics).toEqual(gap.diagnostics);
  });

  it('clears old rows only on new scope and keeps owner anchor during stopped status', () => {
    const old = acceptCurrentLogPage(emptyCurrentLogState, page());
    const stopped = acceptCurrentLogPage(old, page({ scope: { kind: 'deployment_run', run_id: 'r7', active_revision: null }, lines: [event('deployment_stopped')] }));
    expect(stopped.lines).toHaveLength(2);
    const fresh = acceptCurrentLogPage(stopped, page({ scope: { kind: 'deployment_run', run_id: 'r8', active_revision: null }, anchor: { kind: 'first_ready_or_failure', state: 'found', event: event('eos_start_failed', 'r8') }, lines: [], diagnostics: [{ code: 'log_run_changed', count: null }] }));
    expect(fresh.lines).toEqual([]);
    expect(fresh.anchor?.event?.event_code).toBe('eos_start_failed');
    expect(fresh.boundary).toEqual({ previous: 'r7', current: 'r8' });
  });

  it('keeps pending, unavailable and filtered-empty states distinct', () => {
    const pending = acceptCurrentLogPage(emptyCurrentLogState, page({ anchor: { kind: 'first_ready_or_failure', state: 'pending', event: null }, lines: [] }));
    expect(pending.anchor?.state).toBe('pending');
    const missing = acceptCurrentLogPage(pending, page({ anchor: { kind: 'first_ready_or_failure', state: 'unavailable', event: null }, lines: [], diagnostics: [{ code: 'log_anchor_unavailable', count: null }] }));
    expect(missing.anchor?.state).toBe('unavailable');
    const filtered = acceptCurrentLogPage(missing, page({ lines: [] }));
    expect(filtered.anchor?.state).toBe('found');
    expect(filtered.lines).toEqual([]);
    expect(filtered.diagnostics[0].code).toBe('log_anchor_unavailable');
  });

  it('rejects malformed node and correlation filters before request', () => {
    expect(validNodeFilter('/cores/egia/layers/agent/groups/runtime/nodes/a')).toBe(true);
    expect(validNodeFilter('/cores/egia/layers/agent/groups/runtime/nodes/a/settings')).toBe(false);
    expect(validNodeFilter('/tmp/private')).toBe(false);
    expect(validCorrelationFilter('op_7-ok')).toBe(true);
    expect(validCorrelationFilter('with space')).toBe(false);
    expect(validCorrelationFilter('x'.repeat(65))).toBe(false);
  });

  it('bounds changing gap counts by code and suppresses acknowledged evidence in the same run', () => {
    let state = acceptCurrentLogPage(emptyCurrentLogState, page({ diagnostics: [{ code: 'log_collector_gap', count: 1 }] }));
    for (let count = 2; count <= 100; count += 1) state = acceptCurrentLogPage(state, page({ lines: [], diagnostics: [{ code: 'log_collector_gap', count }] }));
    expect(state.diagnostics).toEqual([{ code: 'log_collector_gap', count: 100 }]);
    state = acknowledgeCurrentLogDiagnostics(state);
    expect(state.diagnostics).toEqual([]);
    state = acceptCurrentLogPage(state, page({ lines: [], diagnostics: [{ code: 'log_collector_gap', count: 100 }] }));
    expect(state.diagnostics).toEqual([]);
    state = acceptCurrentLogPage(state, page({ lines: [], diagnostics: [{ code: 'log_collector_gap', count: 101 }] }));
    expect(state.diagnostics).toEqual([{ code: 'log_collector_gap', count: 101 }]);
    const nextRun = acceptCurrentLogPage(state, page({ scope: { kind: 'deployment_run', run_id: 'r8', active_revision: null }, anchor: { kind: 'first_ready_or_failure', state: 'pending', event: null }, lines: [], diagnostics: [{ code: 'log_collector_gap', count: 1 }] }));
    expect(nextRun.diagnostics).toEqual([{ code: 'log_collector_gap', count: 1 }]);
    expect(nextRun.acknowledgedDiagnostics).toEqual({});
  });
});
