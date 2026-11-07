import { describe, expect, it, vi } from 'vitest';
import cases from '../../../tests/fixtures/browser_results/cases.json';
import { decodeDispatchResult, readPrimitiveResult, ResultClientError } from './primitive-result';
import { resolveBaseBindings, sendFence, typedBaseRequest, validBodyVelocity, type BodyVelocity } from './primitive-command';
import type { NodeGraph, ProcessStatus } from '../isos/node-graph';

const fixture = cases.cases[0];
const revision = cases.revision_a;
const payload = fixture.request!.payload as BodyVelocity;
const graph: NodeGraph = { version: '0.1.0', robot: { id: 'r', name: 'Robot' }, cores: { eos: { layers: {
  dispatcher: { groups: { base: { nodes: { velocity: { id: 'velocity', node_type: 'eos.dispatcher.primitive_base@v0.1', label: '', description: '', settings: { primitive_name: 'base.velocity', primitive_version: '1.0.0', movement_type: 'body_velocity' } } } } } },
  hardware: { groups: { base: { nodes: { drive: { id: 'drive', node_type: 'eos.hardware.base@v0.1', label: '', description: '', adapter: { type: 'mock-cxx' }, inputs: { command: [{ node: 'eos/dispatcher/base/velocity', output: 'command' }] }, movement: { modes: ['body_velocity'], limits: { max_linear_m_s: 0.5, max_angular_rad_s: 0.5, max_velocity_lease_ms: 300 } } } } } } },
} } } };
const status: ProcessStatus = { schema_version: '1.0.0', saved_revision: revision, active_revision: revision, run_id: 'run-a', run_id_state: 'known', state: 'running', processes: [] };

describe('R07 public typed boundary', () => {
  it('resolves one graph target and exactly matches the approved send fixture', () => {
    const [binding] = resolveBaseBindings(graph);
    expect(typedBaseRequest(binding, revision, 'move-1', 1, 1_000_000_000, payload)).toEqual(fixture.request);
    expect(sendFence(revision, status, false, false)).toBe(true);
    expect(sendFence(revision, { ...status, active_revision: cases.revision_b }, false, false)).toBe(false);
    expect(sendFence(revision, { ...status, run_id_state: 'unavailable' }, false, false)).toBe(false);
    expect(validBodyVelocity({ ...payload, x_m_s: 5 }, binding.target)).toBe(false);
    expect(() => typedBaseRequest(binding, revision, 'move-1', 1, Number.MAX_SAFE_INTEGER, payload)).toThrow();
    const small = { ...payload, x_m_s: 0.000001 };
    const smallRequest = typedBaseRequest(binding, revision, 'move-small', 2, 1_000_000_000, small);
    expect((smallRequest.header as { payload_length_bytes: number }).payload_length_bytes).toBe(new TextEncoder().encode(JSON.stringify(small)).length);
    const bases = graph.cores.eos.layers.hardware.groups.base.nodes;
    bases.other = { ...bases.drive, id: 'other' };
    expect(resolveBaseBindings(graph)).toHaveLength(0);
    delete bases.other;
    bases.drive.inputs = { command: [{ node: 'another/path', output: 'command' }] };
    expect(resolveBaseBindings(graph)).toHaveLength(0);
  });
  it('decodes all six owner statuses and rejects mismatches', () => {
    for (const item of cases.cases.filter((entry) => entry.category === 'owner_result')) {
      expect(decodeDispatchResult(item.response, 'move-1').status).toBe(item.response!.result!.status);
    }
    for (const item of cases.cases.filter((entry) => entry.category === 'protocol' && entry.response)) {
      expect(() => decodeDispatchResult(item.response, 'move-1')).toThrow(ResultClientError);
    }
  });
  it('reads exactly the three public fields and keeps owner errors distinct', async () => {
    const fetchMock = vi.fn(async (_url: string, init: RequestInit) => ({ ok: true, status: 200, text: async () => JSON.stringify({ ok: true, result: { command_id: 'move-1', status: 'InProgress', reason: null } }) }));
    vi.stubGlobal('fetch', fetchMock);
    expect((await readPrimitiveResult('move-1', revision)).status).toBe('InProgress');
    const body = JSON.parse(fetchMock.mock.calls[0][1].body as string);
    expect(body).toEqual({ op: 'result', command_id: 'move-1', active_revision: revision });
    expect(fetchMock.mock.calls[0][1].credentials).toBe('omit');
    expect(() => readPrimitiveResult('', revision)).rejects.toMatchObject({ kind: 'validation' });
    vi.stubGlobal('fetch', vi.fn(async () => ({ ok: false, status: 503, text: async () => JSON.stringify({ ok: false, error: 'operator control unavailable' }) })));
    await expect(readPrimitiveResult('move-1', revision)).rejects.toMatchObject({ kind: 'owner', httpStatus: 503 });
    vi.unstubAllGlobals();
  });
  it('keeps opaque IDs, bridge outages and malformed replies unavailable', async () => {
    for (const [status, message] of [[502, 'runtime unavailable'], [503, 'operator control unavailable'], [200, 'unknown command ID']] as const) {
      vi.stubGlobal('fetch', vi.fn(async () => ({ ok: status < 400, status, text: async () => JSON.stringify({ ok: false, error: message }) })));
      await expect(readPrimitiveResult('move-1', revision)).rejects.toMatchObject({ kind: 'owner', message });
    }
    vi.stubGlobal('fetch', vi.fn(async () => ({ ok: true, status: 200, text: async () => JSON.stringify({ ok: true, result: { command_id: 'other', status: 'Completed', reason: null } }) })));
    await expect(readPrimitiveResult('move-1', revision)).rejects.toMatchObject({ kind: 'protocol' });
    vi.stubGlobal('fetch', vi.fn(async () => ({ ok: true, status: 200, text: async () => 'x'.repeat(4097) })));
    await expect(readPrimitiveResult('move-1', revision)).rejects.toMatchObject({ kind: 'protocol' });
    vi.stubGlobal('fetch', vi.fn(async () => { throw new Error('offline'); }));
    await expect(readPrimitiveResult('move-1', revision)).rejects.toMatchObject({ kind: 'transport' });
    vi.unstubAllGlobals();
  });
});
