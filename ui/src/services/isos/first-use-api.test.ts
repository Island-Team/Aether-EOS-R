import { afterEach, expect, it, vi } from 'vitest';

afterEach(() => { vi.unstubAllGlobals(); });

it('loads the graph and selected catalog, starts its revision, and sends a run-bound keyboard event', async () => {
  vi.stubGlobal('ISOSUI_API_ORIGIN', 'http://127.0.0.1:16000');
  const graph = { version: '0.2.0', robot: { id: 'robot', name: 'Robot' }, cores: {} };
  const status = { schema_version: '1.0.0', state: 'running', run_id: 'run-a', run_id_state: 'known', saved_revision: 'rev-a', active_revision: 'rev-a', processes: [] };
  const calls: { path: string; method: string; body: unknown }[] = [];
  vi.stubGlobal('fetch', vi.fn(async (url: string, init: RequestInit) => {
    const path = new URL(url).pathname;
    const body: unknown = init.body ? JSON.parse(String(init.body)) : null;
    calls.push({ path, method: init.method || 'GET', body });
    const data = path.endsWith('/catalog/node-types/selected') ? { graph_revision: 'rev-a', catalog: { types: [], data_types: [] } }
      : path.endsWith('/catalog/node-types') ? { types: [], data_types: [] }
        : path.endsWith('/graph') ? { revision: 'rev-a', graph, read_only: false, diagnostics: [] }
          : path.endsWith('/runtime/process-status') || path.endsWith('/runtime/control') ? status
            : path.endsWith('/nodes/events') ? { accepted: true } : null;
    if (!data) throw new Error(`unexpected path ${path}`);
    return { ok: true, status: 200, json: async () => ({ schema_version: '0.1.0', ok: true, data, error: null }) };
  }));
  const { getGraphCatalogSnapshot, getProcessStatus, controlProcesses } = await import('./node-graph');
  const { publishKeyState } = await import('./node-events');
  const snapshot = await getGraphCatalogSnapshot();
  expect(snapshot.reason).toBeNull();
  expect(snapshot.selected?.types).toEqual([]);
  expect((await getProcessStatus()).run_id).toBe('run-a');
  await controlProcesses('run', snapshot.graph.revision);
  await publishKeyState({ schema_version: '0.1.0', source_node: 'isos/ui/keyboard/drive_key', output: 'state', data_type: 'key_state', run_id: 'run-a', active_revision: 'rev-a', payload: { state: 'pressed', activation_id: 'act-a', sequence: 1 } });
  expect(calls.map((call) => call.path)).toEqual([
    '/api/isos/v1/catalog/node-types', '/api/isos/v1/graph', '/api/isos/v1/catalog/node-types/selected',
    '/api/isos/v1/runtime/process-status', '/api/isos/v1/runtime/control', '/api/isos/v1/nodes/events',
  ]);
  expect(calls[4]).toMatchObject({ method: 'POST', body: { schema_version: '0.1.0', operation: 'run', revision: 'rev-a' } });
  expect(calls[5]).toMatchObject({ method: 'POST', body: { data_type: 'key_state', run_id: 'run-a', payload: { state: 'pressed', sequence: 1 } } });
});
