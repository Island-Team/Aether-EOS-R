import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { acceptCurrentLogPage, emptyCurrentLogState } from '../../components/isos/log-cursor-state';

const envelope = (data: unknown, status = 200, error: string | null = null) => ({
  ok: status < 400, status,
  json: async () => ({ schema_version: '0.1.0', ok: status < 400, data, error: error ? { code: 'request_failed', message: error, details: data, correlation_id: 'req-1' } : null }),
});
const event = (event_code: string, run_id = 'r7') => ({ timestamp: '2026-11-01T10:00:00.000Z', level: 'info', source: 'eos', core: 'eos', event_code, run_id, message: event_code, node_path: null, correlation_id: null, fields: {}, pid: 42 });
const page = (run_id: string, code: string, lines: ReturnType<typeof event>[] = []) => ({
  scope: { kind: 'deployment_run', run_id, active_revision: null },
  anchor: { kind: 'first_ready_or_failure', state: 'found', event: event(code, run_id) },
  lines, next_cursor: `lc1.${run_id}`, rotated: false, diagnostics: [],
});

beforeEach(() => {
  vi.resetModules();
  vi.stubGlobal('ISOSUI_API_ORIGIN', 'http://127.0.0.1:16000');
});
afterEach(() => { vi.unstubAllGlobals(); });

describe('P07 stub IsOS API', () => {
  it('requests the opt-in view with bounded detail filters and no bearer token', async () => {
    const fetchMock = vi.fn().mockResolvedValue(envelope(page('r7', 'eos_ready')));
    vi.stubGlobal('fetch', fetchMock);
    const { getCurrentLogPage, isCurrentLogPage } = await import('./node-graph');
    const result = await getCurrentLogPage({ source: 'egia', level: 'warn', node_path: '/cores/egia/layers/agent/groups/runtime/nodes/a', correlation_id: 'op_7', cursor: 'lc1.old', limit: 500 });
    expect(isCurrentLogPage(result)).toBe(true);
    const [url, options] = fetchMock.mock.calls[0] as [string, RequestInit];
    const query = new URL(url).searchParams;
    expect(query.get('view')).toBe('current');
    expect(query.get('source')).toBe('egia');
    expect(query.get('node_path')).toBe('/cores/egia/layers/agent/groups/runtime/nodes/a');
    expect(query.get('correlation_id')).toBe('op_7');
    expect(query.get('cursor')).toBe('lc1.old');
    expect(query.get('limit')).toBe('200');
    expect(options.credentials).toBe('omit');
    expect(options.headers).not.toHaveProperty('Authorization');
  });

  it('keeps status authority and accepted order through rotation, gap and a new run', async () => {
    const ready = page('r7', 'eos_ready', [event('deployment_ready')]);
    const rotated = { ...page('r7', 'eos_ready', [event('deployment_stopped')]), rotated: true, diagnostics: [{ code: 'log_cursor_expired', count: null }] };
    const next = { ...page('r8', 'eos_start_failed'), diagnostics: [{ code: 'log_run_changed', count: null }] };
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(envelope({ schema_version: '1.0.0', state: 'stopped', run_id: 'r7', run_id_state: 'known', processes: [] }))
      .mockResolvedValueOnce(envelope(ready))
      .mockResolvedValueOnce(envelope(rotated))
      .mockResolvedValueOnce(envelope(next));
    vi.stubGlobal('fetch', fetchMock);
    const { getCurrentLogPage, getProcessStatus, isCurrentLogPage } = await import('./node-graph');
    const status = await getProcessStatus();
    expect(status.state).toBe('stopped');
    let state = emptyCurrentLogState;
    for (const cursor of [undefined, 'lc1.r7', 'lc1.rotated']) {
      const response = await getCurrentLogPage({ source: 'all', level: 'info', cursor });
      if (!isCurrentLogPage(response)) throw new Error('stub returned legacy page');
      state = acceptCurrentLogPage(state, response);
    }
    expect(state.scope?.run_id).toBe('r8');
    expect(state.anchor?.event?.event_code).toBe('eos_start_failed');
    expect(state.lines).toEqual([]);
    expect(state.boundary).toEqual({ previous: 'r7', current: 'r8' });
    expect(fetchMock).toHaveBeenCalledTimes(4);
  });

  it('preserves typed 503 scope and 500 publication failures', async () => {
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(envelope({ diagnostics: [{ path: '/scope', code: 'log_scope_unavailable', message: 'Current log scope unavailable' }] }, 503, 'log scope unavailable'))
      .mockResolvedValueOnce(envelope({ diagnostics: [{ path: '/run_id', code: 'log_scope_publish_failed', message: 'Log scope publication failed' }] }, 500, 'log scope publication failed'));
    vi.stubGlobal('fetch', fetchMock);
    const { getCurrentLogPage, controlProcesses } = await import('./node-graph');
    await expect(getCurrentLogPage({ source: 'all', level: 'info' })).rejects.toMatchObject({ status: 503, code: 'log_scope_unavailable' });
    await expect(controlProcesses('run')).rejects.toMatchObject({ status: 500, code: 'log_scope_publish_failed' });
  });

  it('identifies a legacy page without claiming it has an anchor', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(envelope({ lines: [event('legacy')], next_cursor: 'old', rotated: false })));
    const { getCurrentLogPage, isCurrentLogPage } = await import('./node-graph');
    expect(isCurrentLogPage(await getCurrentLogPage({ source: 'all', level: 'info' }))).toBe(false);
  });

  it('rejects a malformed current page instead of silently using legacy mode', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(envelope({
      ...page('r7', 'eos_ready', [event('wrong_run', 'r8')]),
      next_cursor: 'invalid cursor with spaces',
    })));
    const { getCurrentLogPage } = await import('./node-graph');
    await expect(getCurrentLogPage({ source: 'all', level: 'info' })).rejects.toThrow('invalid current log page');
  });
});
