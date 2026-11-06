// @vitest-environment jsdom
import React, { act } from 'react';
import { createRoot } from 'react-dom/client';
import { afterEach, expect, it, vi } from 'vitest';
import type { NodeGraphDocument, ProcessStatus, StopControlReply } from '../../services/isos/node-graph';
import { IsosApiError } from '../../services/isos/api-client';

const stopMock = vi.fn(async (): Promise<StopControlReply> => ({ stopped: ['eos'], terminal: { run_id: 'run-a', outcome: 'confirmed', reason_code: null, processes: [{ id: 'eos', outcome: 'confirmed', reason_code: null }] } }));
vi.mock('../../services/isos/node-graph', async (importOriginal) => {
  vi.stubGlobal('ISOSUI_API_ORIGIN', 'http://127.0.0.1:16000');
  return { ...(await importOriginal<typeof import('../../services/isos/node-graph')>()), stopProcesses: stopMock,
    getNodeGraph: async () => graphRead, getProcessStatus: async () => statusRead() };
});

const revision = 'a'.repeat(64);
const graph: NodeGraphDocument = { revision, graph: { version: '0.1.0', robot: { id: 'r', name: 'R' }, cores: { eos: { layers: {
  dispatcher: { groups: { base: { nodes: { velocity: { id: 'velocity', node_type: 'eos.dispatcher.primitive_base@v0.1', label: '', description: '', settings: { primitive_name: 'base.velocity', primitive_version: '1.0.0', movement_type: 'body_velocity' } } } } } },
  hardware: { groups: { base: { nodes: { drive: { id: 'drive', node_type: 'eos.hardware.base@v0.1', label: '', description: '', adapter: { type: 'mock-cxx' }, inputs: { command: [{ node: 'eos/dispatcher/base/velocity', output: 'command' }] }, movement: { modes: ['body_velocity'], limits: { max_linear_m_s: 0.5, max_angular_rad_s: 0.5, max_velocity_lease_ms: 300 } } } } } } },
} } } } };
let graphRead: NodeGraphDocument = graph;
const process: ProcessStatus = { schema_version: '1.0.0', state: 'running', run_id: 'run-a', run_id_state: 'known', saved_revision: revision, active_revision: 'b'.repeat(64), processes: [] };
let statusState = process;
let statusRead = async (): Promise<ProcessStatus> => statusState;
const ready = () => ({ 'eos/hardware/base/drive': { status: { state: 'ready' }, statusAt: Date.now(), health: { state: 'healthy' }, healthAt: Date.now() } });
const response = (value: unknown) => ({ ok: true, status: 200, text: async () => JSON.stringify(value) });

it('keeps IsOS Stop reachable through EOS outage and stale revision', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  const container = document.createElement('div'); document.body.appendChild(container);
  const root = createRoot(container); const onStop = vi.fn();
  const { PrimitiveBaseCard } = await import('./PrimitiveBaseCard');
  await act(async () => root.render(<PrimitiveBaseCard lang="en" graph={graph} status={process} dirty readOnly readings={{}} clock={Date.now()} onStop={onStop} onOpenLogs={() => undefined} />));
  const invoke = [...container.querySelectorAll('button')].find((button) => button.textContent === 'Invoke')!;
  const stop = [...container.querySelectorAll('button')].find((button) => button.textContent === 'Stop deployment')!;
  expect(invoke.disabled).toBe(true);
  expect(stop.disabled).toBe(false);
  await act(async () => stop.click());
  expect(stopMock).toHaveBeenCalledWith();
  expect(onStop).toHaveBeenCalled();
  expect(container.textContent).toContain('IsOS stop status: Stop confirmed: run-a');
  await act(async () => root.unmount()); container.remove(); vi.unstubAllGlobals();
});

it('shows pending then an unknown terminal alert even when status refresh fails', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  let finish!: (value: Awaited<ReturnType<typeof stopMock>>) => void;
  stopMock.mockImplementationOnce(() => new Promise((resolve) => { finish = resolve; }));
  statusRead = async () => { throw new Error('status offline'); };
  const container = document.createElement('div'); document.body.appendChild(container); const root = createRoot(container);
  const { PrimitiveBaseCard } = await import('./PrimitiveBaseCard');
  await act(async () => root.render(<PrimitiveBaseCard lang="en" graph={graph} status={process} dirty readOnly readings={{}} clock={Date.now()} onStop={() => undefined} onOpenLogs={() => undefined} />));
  await act(async () => [...container.querySelectorAll('button')].find((button) => button.textContent === 'Stop deployment')!.click());
  expect(container.textContent).toContain('Stopping…');
  expect([...container.querySelectorAll('button')].find((button) => button.textContent === 'Stopping…')!.disabled).toBe(true);
  await act(async () => finish({ stopped: ['eos'], terminal: { run_id: 'run-a', outcome: 'unknown', reason_code: 'eos_stop_unconfirmed', processes: [{ id: 'eos', outcome: 'unknown', reason_code: 'eos_stop_unconfirmed' }] } }));
  expect(container.querySelector('[role="alert"]')?.textContent).toContain('Stop outcome unknown: eos_stop_unconfirmed');
  await act(async () => root.unmount()); container.remove(); vi.unstubAllGlobals();
  statusRead = async () => statusState;
});

it('shows a transport failure without retrying Stop', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  stopMock.mockImplementationOnce(async () => { throw new Error('lost reply'); });
  const calls = stopMock.mock.calls.length;
  const container = document.createElement('div'); document.body.appendChild(container); const root = createRoot(container);
  const { PrimitiveBaseCard } = await import('./PrimitiveBaseCard');
  await act(async () => root.render(<PrimitiveBaseCard lang="en" graph={graph} status={process} dirty readOnly readings={{}} clock={Date.now()} onStop={() => undefined} onOpenLogs={() => undefined} />));
  await act(async () => [...container.querySelectorAll('button')].find((button) => button.textContent === 'Stop deployment')!.click());
  expect(stopMock.mock.calls.length).toBe(calls + 1);
  expect(container.querySelector('[role="alert"]')?.textContent).toContain('Stop response unavailable');
  await act(async () => root.unmount()); container.remove(); vi.unstubAllGlobals();
});

it('shows an IsOS owner rejection distinctly from a lost response', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  stopMock.mockImplementationOnce(async () => { throw new IsosApiError('stop denied', 403, 'stop_denied'); });
  const container = document.createElement('div'); document.body.appendChild(container); const root = createRoot(container);
  const { PrimitiveBaseCard } = await import('./PrimitiveBaseCard');
  await act(async () => root.render(<PrimitiveBaseCard lang="en" graph={graph} status={process} dirty readOnly readings={{}} clock={Date.now()} onStop={() => undefined} onOpenLogs={() => undefined} />));
  await act(async () => [...container.querySelectorAll('button')].find((button) => button.textContent === 'Stop deployment')!.click());
  expect(container.querySelector('[role="alert"]')?.textContent).toContain('Stop rejected by IsOS: stop denied');
  expect(container.textContent).not.toContain('Stop response unavailable');
  await act(async () => root.unmount()); container.remove(); vi.unstubAllGlobals();
});

it('sends once after a click and follows InProgress with the guarded public read', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  statusState = { ...process, active_revision: revision };
  statusRead = async () => statusState;
  const bodies: Record<string, unknown>[] = [];
  vi.stubGlobal('fetch', vi.fn(async (_url: string, init: RequestInit) => {
    const body = JSON.parse(init.body as string) as Record<string, unknown>; bodies.push(body);
    const result = body.op === 'time' ? { ok: true, monotonic_ns: 1_000_000_000 } :
      { ok: true, result: { command_id: body.op === 'primitive_typed' ? (body.header as { command_id: string }).command_id : body.command_id,
        status: body.op === 'primitive_typed' ? 'InProgress' : 'Completed', reason: null } };
    return { ok: true, status: 200, text: async () => JSON.stringify(result) };
  }));
  const container = document.createElement('div'); document.body.appendChild(container);
  const root = createRoot(container);
  const { PrimitiveBaseCard } = await import('./PrimitiveBaseCard');
  await act(async () => root.render(<PrimitiveBaseCard lang="en" graph={graph} status={statusState} dirty={false} readOnly={false}
    readings={ready()}
    clock={Date.now()} onStop={() => undefined} onOpenLogs={() => undefined} />));
  const invoke = [...container.querySelectorAll('button')].find((button) => button.textContent === 'Invoke')!;
  expect(invoke.disabled).toBe(false);
  await act(async () => invoke.click());
  expect(bodies.filter((body) => body.op === 'primitive_typed')).toHaveLength(1);
  expect(container.textContent).toContain('InProgress');
  await act(async () => { await new Promise((resolve) => setTimeout(resolve, 1100)); });
  expect(bodies.filter((body) => body.op === 'result')).toHaveLength(1);
  expect(container.textContent).toContain('Completed');
  expect((bodies.find((body) => body.op === 'result'))).toEqual({ op: 'result', command_id: (bodies.find((body) => body.op === 'primitive_typed')!.header as { command_id: string }).command_id, active_revision: revision });
  await act(async () => root.unmount()); container.remove(); vi.unstubAllGlobals();
});

it('does not poll during sending or let a late send reply overwrite an old-run seal', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  statusState = { ...process, active_revision: revision }; statusRead = async () => statusState;
  let resolveSend!: (value: ReturnType<typeof response>) => void;
  const pendingSend = new Promise<ReturnType<typeof response>>((resolve) => { resolveSend = resolve; });
  const bodies: Record<string, unknown>[] = [];
  vi.stubGlobal('fetch', vi.fn(async (_url: string, init: RequestInit) => {
    const body = JSON.parse(init.body as string) as Record<string, unknown>; bodies.push(body);
    if (body.op === 'time') return response({ ok: true, monotonic_ns: 1_000_000_000 });
    if (body.op === 'primitive_typed') return pendingSend;
    return response({ ok: true, result: { command_id: body.command_id, status: 'Completed', reason: null } });
  }));
  const container = document.createElement('div'); document.body.appendChild(container); const root = createRoot(container);
  const { PrimitiveBaseCard } = await import('./PrimitiveBaseCard');
  const render = (status: ProcessStatus) => <PrimitiveBaseCard lang="en" graph={graph} status={status} dirty={false} readOnly={false} readings={ready()} clock={Date.now()} onStop={() => undefined} onOpenLogs={() => undefined} />;
  await act(async () => root.render(render(statusState)));
  await act(async () => [...container.querySelectorAll('button')].find((button) => button.textContent === 'Invoke')!.click());
  expect(container.textContent).toContain('Sending (browser)');
  await act(async () => { await new Promise((resolve) => setTimeout(resolve, 1100)); });
  expect(bodies.filter((body) => body.op === 'result')).toHaveLength(0);
  statusState = { ...statusState, saved_revision: 'b'.repeat(64) };
  await act(async () => root.render(render(statusState)));
  expect(container.textContent).toContain('Old deployment run or revision.');
  await act(async () => resolveSend(response({ ok: true, result: { command_id: (bodies.find((body) => body.op === 'primitive_typed')!.header as { command_id: string }).command_id, status: 'Completed', reason: null } })));
  expect(container.textContent).not.toContain('Owner status: Completed');
  expect(container.textContent).toContain('Binding: eos/dispatcher/base/velocity');
  await act(async () => root.unmount()); container.remove(); vi.unstubAllGlobals();
});

it('marks a terminal owner result unavailable after run rotation and permits a fresh command', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  statusState = { ...process, active_revision: revision }; statusRead = async () => statusState;
  const sent: string[] = [];
  vi.stubGlobal('fetch', vi.fn(async (_url: string, init: RequestInit) => {
    const body = JSON.parse(init.body as string) as { op: string; header?: { command_id: string } };
    if (body.op === 'time') return response({ ok: true, monotonic_ns: 1_000_000_000 });
    if (body.op !== 'primitive_typed') throw new Error('terminal results must not be polled');
    sent.push(body.header!.command_id);
    return response({ ok: true, result: { command_id: body.header!.command_id, status: 'OutcomeUnknown', reason: 'journal uncertain' } });
  }));
  const container = document.createElement('div'); document.body.appendChild(container); const root = createRoot(container);
  const { PrimitiveBaseCard } = await import('./PrimitiveBaseCard');
  const render = (status: ProcessStatus | null = statusState) => <PrimitiveBaseCard lang="en" graph={graph} status={status} dirty={false} readOnly={false} readings={ready()} clock={Date.now()} onStop={() => undefined} onOpenLogs={() => undefined} />;
  await act(async () => root.render(render()));
  await act(async () => [...container.querySelectorAll('button')].find((button) => button.textContent === 'Invoke')!.click());
  expect(container.textContent).toContain('Owner status: OutcomeUnknown');
  expect(container.textContent).toContain(sent[0]);
  await act(async () => root.render(render(null)));
  expect(container.textContent).toContain('IsOS run identity unavailable.');
  expect([...container.querySelectorAll('button')].find((button) => button.textContent === 'Refresh result')).toBeUndefined();
  statusState = { ...statusState, run_id: 'run-b' };
  await act(async () => root.render(render()));
  expect(container.textContent).toContain('Owner status: OutcomeUnknown');
  expect(container.textContent).toContain('Unavailable for this run');
  expect(container.textContent).toContain(sent[0]);
  expect([...container.querySelectorAll('button')].find((button) => button.textContent === 'Refresh result')).toBeUndefined();
  await act(async () => [...container.querySelectorAll('button')].find((button) => button.textContent === 'Invoke')!.click());
  expect(sent).toHaveLength(2);
  expect(sent[1]).not.toBe(sent[0]);
  expect(container.textContent).toContain(sent[1]);
  expect(container.textContent).not.toContain(sent[0]);
  await act(async () => root.unmount()); container.remove(); vi.unstubAllGlobals();
});

it('keeps a sealed terminal old-run result sealed when Stop is clicked afterward', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  statusState = { ...process, active_revision: revision }; statusRead = async () => statusState;
  let commandId = '';
  vi.stubGlobal('fetch', vi.fn(async (_url: string, init: RequestInit) => {
    const body = JSON.parse(init.body as string) as { op: string; header?: { command_id: string } };
    if (body.op === 'time') return response({ ok: true, monotonic_ns: 1_000_000_000 });
    commandId = body.header!.command_id;
    return response({ ok: true, result: { command_id: commandId, status: 'OutcomeUnknown', reason: 'release uncertain' } });
  }));
  const container = document.createElement('div'); document.body.appendChild(container); const root = createRoot(container);
  const { PrimitiveBaseCard } = await import('./PrimitiveBaseCard');
  const render = () => <PrimitiveBaseCard lang="en" graph={graph} status={statusState} dirty={false} readOnly={false} readings={ready()} clock={Date.now()} onStop={() => undefined} onOpenLogs={() => undefined} />;
  await act(async () => root.render(render()));
  await act(async () => [...container.querySelectorAll('button')].find((button) => button.textContent === 'Invoke')!.click());
  statusState = { ...statusState, run_id: 'run-b' };
  await act(async () => root.render(render()));
  expect(container.textContent).toContain('Unavailable for this run');
  await act(async () => [...container.querySelectorAll('button')].find((button) => button.textContent === 'Stop deployment')!.click());
  expect(container.textContent).toContain(commandId);
  expect(container.textContent).toContain('Owner status: OutcomeUnknown');
  expect(container.textContent).toContain('Unavailable for this run');
  expect(container.textContent).not.toContain('automatic reads paused');
  expect([...container.querySelectorAll('button')].find((button) => button.textContent === 'Refresh result')).toBeUndefined();
  await act(async () => root.unmount()); container.remove(); vi.unstubAllGlobals();
});

it('seals an InProgress record on Stop and ignores its late result read', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  statusState = { ...process, active_revision: revision }; statusRead = async () => statusState;
  let resolveRead!: (value: ReturnType<typeof response>) => void;
  const pendingRead = new Promise<ReturnType<typeof response>>((resolve) => { resolveRead = resolve; });
  const sent: string[] = [];
  vi.stubGlobal('fetch', vi.fn(async (_url: string, init: RequestInit) => {
    const body = JSON.parse(init.body as string) as { op: string; header?: { command_id: string } };
    if (body.op === 'time') return response({ ok: true, monotonic_ns: 1_000_000_000 });
    if (body.op === 'primitive_typed') { sent.push(body.header!.command_id); return response({ ok: true, result: { command_id: body.header!.command_id, status: 'InProgress', reason: null } }); }
    return pendingRead;
  }));
  const container = document.createElement('div'); document.body.appendChild(container); const root = createRoot(container);
  const { PrimitiveBaseCard } = await import('./PrimitiveBaseCard');
  const render = () => <PrimitiveBaseCard lang="en" graph={graph} status={statusState} dirty={false} readOnly={false} readings={ready()} clock={Date.now()} onStop={() => undefined} onOpenLogs={() => undefined} />;
  await act(async () => root.render(render()));
  await act(async () => [...container.querySelectorAll('button')].find((button) => button.textContent === 'Invoke')!.click());
  expect(container.textContent).toContain('Owner status: InProgress');
  await act(async () => [...container.querySelectorAll('button')].find((button) => button.textContent === 'Refresh result')!.click());
  await act(async () => [...container.querySelectorAll('button')].find((button) => button.textContent === 'Stop deployment')!.click());
  expect(container.textContent).toContain('Owner status: InProgress');
  expect(container.textContent).toContain('Unavailable for this run');
  expect(container.textContent).toContain(sent[0]);
  expect([...container.querySelectorAll('button')].find((button) => button.textContent === 'Invoke')!.disabled).toBe(true);
  await act(async () => resolveRead(response({ ok: true, result: { command_id: sent[0], status: 'Completed', reason: null } })));
  expect(container.textContent).not.toContain('Owner status: Completed');
  statusState = { ...statusState, run_id: 'run-b' };
  await act(async () => root.render(render()));
  expect(container.textContent).toContain('Unavailable for this run');
  expect([...container.querySelectorAll('button')].find((button) => button.textContent === 'Invoke')!.disabled).toBe(false);
  await act(async () => root.unmount()); container.remove(); vi.unstubAllGlobals();
});

it('reads after a lost send once at a time and rechecks IsOS after the read', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  statusState = { ...process, active_revision: revision }; statusRead = async () => statusState;
  const bodies: Record<string, unknown>[] = [];
  let resolveRead!: (value: ReturnType<typeof response>) => void;
  const pendingRead = new Promise<ReturnType<typeof response>>((resolve) => { resolveRead = resolve; });
  vi.stubGlobal('fetch', vi.fn(async (_url: string, init: RequestInit) => {
    const body = JSON.parse(init.body as string) as Record<string, unknown>; bodies.push(body);
    if (body.op === 'time') return response({ ok: true, monotonic_ns: 1_000_000_000 });
    if (body.op === 'primitive_typed') throw new Error('lost send response');
    return pendingRead;
  }));
  const container = document.createElement('div'); document.body.appendChild(container); const root = createRoot(container);
  const { PrimitiveBaseCard } = await import('./PrimitiveBaseCard');
  await act(async () => root.render(<PrimitiveBaseCard lang="en" graph={graph} status={statusState} dirty={false} readOnly={false} readings={ready()} clock={Date.now()} onStop={() => undefined} onOpenLogs={() => undefined} />));
  await act(async () => [...container.querySelectorAll('button')].find((button) => button.textContent === 'Invoke')!.click());
  expect(container.textContent).toContain('EOS bridge unavailable.');
  await act(async () => { await new Promise((resolve) => setTimeout(resolve, 1100)); });
  expect(bodies.filter((body) => body.op === 'result')).toHaveLength(1);
  await act(async () => { await new Promise((resolve) => setTimeout(resolve, 2200)); });
  expect(bodies.filter((body) => body.op === 'result')).toHaveLength(1);
  statusState = { ...statusState, run_id: 'run-b' };
  await act(async () => resolveRead(response({ ok: true, result: { command_id: (bodies.find((body) => body.op === 'result')!.command_id), status: 'Completed', reason: null } })));
  expect(container.textContent).toContain('Old deployment run or revision.');
  expect(container.textContent).not.toContain('Owner status: Completed');
  expect(bodies.filter((body) => body.op === 'primitive_typed')).toHaveLength(1);
  await act(async () => root.unmount()); container.remove(); vi.unstubAllGlobals();
});

it('pauses a hidden InProgress command at the 60-second bound', async () => {
  vi.useFakeTimers(); vi.setSystemTime(new Date('2026-11-02T00:00:00Z'));
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  Object.defineProperty(document, 'visibilityState', { configurable: true, value: 'visible' });
  statusState = { ...process, active_revision: revision }; statusRead = async () => statusState;
  const bodies: Record<string, unknown>[] = [];
  vi.stubGlobal('fetch', vi.fn(async (_url: string, init: RequestInit) => {
    const body = JSON.parse(init.body as string) as Record<string, unknown>; bodies.push(body);
    return response(body.op === 'time' ? { ok: true, monotonic_ns: 1_000_000_000 } :
      { ok: true, result: { command_id: body.op === 'primitive_typed' ? (body.header as { command_id: string }).command_id : body.command_id, status: 'InProgress', reason: null } });
  }));
  const container = document.createElement('div'); document.body.appendChild(container); const root = createRoot(container);
  const { PrimitiveBaseCard } = await import('./PrimitiveBaseCard');
  await act(async () => root.render(<PrimitiveBaseCard lang="en" graph={graph} status={statusState} dirty={false} readOnly={false} readings={ready()} clock={Date.now()} onStop={() => undefined} onOpenLogs={() => undefined} />));
  await act(async () => [...container.querySelectorAll('button')].find((button) => button.textContent === 'Invoke')!.click());
  expect(container.textContent).toContain('InProgress');
  Object.defineProperty(document, 'visibilityState', { configurable: true, value: 'hidden' });
  await act(async () => { await vi.advanceTimersByTimeAsync(61_000); });
  expect(bodies.filter((body) => body.op === 'result')).toHaveLength(0);
  Object.defineProperty(document, 'visibilityState', { configurable: true, value: 'visible' });
  await act(async () => document.dispatchEvent(new Event('visibilitychange')));
  expect(container.textContent).toContain('automatic reads paused');
  expect(bodies.filter((body) => body.op === 'result')).toHaveLength(0);
  await act(async () => root.unmount()); container.remove(); vi.useRealTimers(); vi.unstubAllGlobals();
});

it('selects the exact requested binding among several active targets', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  statusState = { ...process, active_revision: revision };
  const mixed = JSON.parse(JSON.stringify(graph)) as NodeGraphDocument;
  const eos = mixed.graph.cores.eos.layers;
  eos.dispatcher.groups.base.nodes = { a_sim: { id: 'a_sim', node_type: 'eos.dispatcher.primitive_base@v0.1', label: '', description: '', settings: { primitive_name: 'base.velocity', primitive_version: '1.0.0', movement_type: 'body_velocity' } }, ...eos.dispatcher.groups.base.nodes };
  eos.hardware.groups.base.nodes.sim = { ...eos.hardware.groups.base.nodes.drive, id: 'sim', adapter: { type: 'mujoco-v2' }, inputs: { command: [{ node: 'eos/dispatcher/base/a_sim', output: 'command' }] } };
  const container = document.createElement('div'); document.body.appendChild(container); const root = createRoot(container);
  const { PrimitiveBaseCard } = await import('./PrimitiveBaseCard');
  await act(async () => root.render(<PrimitiveBaseCard lang="en" bindingPath="eos/dispatcher/base/velocity" graph={mixed} status={statusState} dirty={false} readOnly={false} readings={ready()} clock={Date.now()} onStop={() => undefined} onOpenLogs={() => undefined} />));
  expect(container.textContent).toContain('Binding: eos/dispatcher/base/velocity');
  expect(container.textContent).not.toContain('eos/dispatcher/base/a_sim');
  expect([...container.querySelectorAll('button')].find((button) => button.textContent === 'Invoke')!.disabled).toBe(false);
  await act(async () => root.unmount()); container.remove(); vi.unstubAllGlobals();
});

it('leaves legacy v1.0 bindings read-only in the U04 card', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  statusState = { ...process, active_revision: revision };
  const legacy = JSON.parse(JSON.stringify(graph)) as NodeGraphDocument;
  legacy.graph.cores.eos.layers.dispatcher.groups.base.nodes.velocity.node_type = 'eos.dispatcher.primitive_base@v1.0';
  legacy.graph.cores.eos.layers.hardware.groups.base.nodes.drive.node_type = 'eos.hardware.base@v1.0';
  const container = document.createElement('div'); document.body.appendChild(container); const root = createRoot(container);
  const { PrimitiveBaseCard } = await import('./PrimitiveBaseCard');
  await act(async () => root.render(<PrimitiveBaseCard lang="cn" graph={legacy} status={statusState} dirty={false} readOnly={false} readings={ready()} clock={Date.now()} onStop={() => undefined} onOpenLogs={() => undefined} />));
  expect([...container.querySelectorAll('button')].find((button) => button.textContent === '调用')!.disabled).toBe(true);
  expect(container.textContent).toContain('没有恰好一个目标');
  await act(async () => root.unmount()); container.remove(); vi.unstubAllGlobals();
});

it('invokes one active arm binding with typed payload and preserves a Failed owner result', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  statusState = { ...process, active_revision: revision }; statusRead = async () => statusState;
  const armGraph = JSON.parse(JSON.stringify(graph)) as NodeGraphDocument;
  armGraph.graph.cores.eos.layers.dispatcher.groups.manipulator = { nodes: { movej: { id: 'movej', node_type: 'eos.dispatcher.primitive_manipulator@v0.1', label: 'Arm move', description: 'Joint motion', settings: { primitive_name: 'arm.movej', primitive_version: '1.0.0', movement_type: 'joint_positions' } } } };
  armGraph.graph.cores.eos.layers.hardware.groups.manipulator = { nodes: { arm: { id: 'arm', node_type: 'eos.hardware.manipulator@v0.1', label: 'Arm', description: 'Tool', joint_names: ['j1'], inputs: { command: [{ node: 'eos/dispatcher/manipulator/movej', output: 'command' }] }, movement: { modes: ['joint_positions'], limits: { max_joint_position_rad: 1, max_joint_speed_rad_s: 1 } } } } };
  graphRead = armGraph;
  const bodies: Record<string, unknown>[] = [];
  vi.stubGlobal('fetch', vi.fn(async (_url: string, init: RequestInit) => { const body = JSON.parse(init.body as string) as Record<string, unknown>; bodies.push(body);
    return response(body.op === 'time' ? { ok: true, monotonic_ns: 100 } : { ok: true, result: { command_id: (body.header as { command_id: string }).command_id, status: 'Failed', reason: 'adapter failed' } }); }));
  const container = document.createElement('div'); document.body.appendChild(container); const root = createRoot(container);
  const { PrimitiveBaseCard } = await import('./PrimitiveBaseCard');
  await act(async () => root.render(<PrimitiveBaseCard lang="en" bindingPath="eos/dispatcher/manipulator/movej" graph={armGraph} status={statusState} dirty={false} readOnly={false} readings={{ 'eos/hardware/manipulator/arm': { status: { state: 'ready' }, statusAt: Date.now(), health: { state: 'healthy' }, healthAt: Date.now() } }} clock={Date.now()} onStop={() => undefined} onOpenLogs={() => undefined} />));
  await act(async () => [...container.querySelectorAll('button')].find((button) => button.textContent === 'Invoke')!.click());
  expect(bodies.filter((body) => body.op === 'primitive_typed')).toHaveLength(1);
  expect((bodies[1].header as Record<string, unknown>)).toMatchObject({ data_type_id: 'manipulator_movement', primitive_name: 'arm.movej', target_path: 'eos/hardware/manipulator/arm' });
  expect(container.textContent).toContain('Owner status: Failed');
  expect(container.textContent).toContain('adapter failed');
  await act(async () => root.unmount()); container.remove(); vi.unstubAllGlobals(); graphRead = graph;
});

it('fences every binding immediately after Stop intent and keeps them fenced through unknown Stop and status outage', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  statusState = { ...process, active_revision: revision };
  statusRead = async () => { throw new Error('status offline'); };
  const two = JSON.parse(JSON.stringify(graph)) as NodeGraphDocument;
  two.graph.cores.eos.layers.dispatcher.groups.base.nodes.second = { ...two.graph.cores.eos.layers.dispatcher.groups.base.nodes.velocity, id: 'second' };
  two.graph.cores.eos.layers.hardware.groups.base.nodes.other = { ...two.graph.cores.eos.layers.hardware.groups.base.nodes.drive, id: 'other', inputs: { command: [{ node: 'eos/dispatcher/base/second', output: 'command' }] } };
  let finish!: (value: StopControlReply) => void;
  stopMock.mockImplementationOnce(() => new Promise((resolve) => { finish = resolve; }));
  const container = document.createElement('div'); document.body.appendChild(container); const root = createRoot(container);
  const { PrimitiveBaseCard } = await import('./PrimitiveBaseCard');
  function Pair(): React.ReactElement {
    const [fencedRun, setFencedRun] = React.useState<string | null>(null);
    return <>{['velocity', 'second'].map((name) => <PrimitiveBaseCard key={name} lang="en" bindingPath={`eos/dispatcher/base/${name}`} graph={two} status={statusState} dirty={false} readOnly={false}
      readings={{ ...ready(), 'eos/hardware/base/other': { status: { state: 'ready' }, statusAt: Date.now(), health: { state: 'healthy' }, healthAt: Date.now() } }} clock={Date.now()}
      stopFenced={fencedRun === statusState.run_id} onStopIntent={setFencedRun} onStop={() => undefined} onOpenLogs={() => undefined} />)}</>;
  }
  await act(async () => root.render(<Pair />));
  const invokes = (): HTMLButtonElement[] => [...container.querySelectorAll('button')].filter((button) => button.textContent === 'Invoke');
  expect(invokes()).toHaveLength(2); expect(invokes().every((button) => !button.disabled)).toBe(true);
  await act(async () => [...container.querySelectorAll('button')].find((button) => button.textContent === 'Stop deployment')!.click());
  expect(invokes().every((button) => button.disabled)).toBe(true);
  await act(async () => finish({ stopped: ['eos'], terminal: { run_id: 'run-a', outcome: 'unknown', reason_code: 'eos_stop_unconfirmed', processes: [{ id: 'eos', outcome: 'unknown', reason_code: 'eos_stop_unconfirmed' }] } }));
  expect(invokes().every((button) => button.disabled)).toBe(true);
  expect(container.textContent).toContain('Stop outcome unknown');
  await act(async () => root.unmount()); container.remove(); vi.unstubAllGlobals(); statusRead = async () => statusState;
});

it('seals a sibling InProgress result on Stop and ignores a late Completed reply', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  statusState = { ...process, active_revision: revision }; statusRead = async () => statusState;
  const two = JSON.parse(JSON.stringify(graph)) as NodeGraphDocument;
  two.graph.cores.eos.layers.dispatcher.groups.base.nodes.second = { ...two.graph.cores.eos.layers.dispatcher.groups.base.nodes.velocity, id: 'second' };
  two.graph.cores.eos.layers.hardware.groups.base.nodes.other = { ...two.graph.cores.eos.layers.hardware.groups.base.nodes.drive, id: 'other', inputs: { command: [{ node: 'eos/dispatcher/base/second', output: 'command' }] } };
  graphRead = two;
  let finishResult!: (value: ReturnType<typeof response>) => void;
  const pendingResult = new Promise<ReturnType<typeof response>>((resolve) => { finishResult = resolve; });
  const bodies: Record<string, unknown>[] = [];
  vi.stubGlobal('fetch', vi.fn(async (_url: string, init: RequestInit) => { const body = JSON.parse(init.body as string) as Record<string, unknown>; bodies.push(body);
    if (body.op === 'time') return response({ ok: true, monotonic_ns: 100 });
    if (body.op === 'result') return pendingResult;
    return response({ ok: true, result: { command_id: (body.header as { command_id: string }).command_id, status: 'InProgress', reason: null } });
  }));
  let finishStop!: (value: StopControlReply) => void;
  stopMock.mockImplementationOnce(() => new Promise((resolve) => { finishStop = resolve; }));
  const container = document.createElement('div'); document.body.appendChild(container); const root = createRoot(container);
  const { PrimitiveBaseCard } = await import('./PrimitiveBaseCard');
  function Pair(): React.ReactElement {
    const [fencedRun, setFencedRun] = React.useState<string | null>(null);
    const fenceRef = React.useRef<string | null>(null);
    const markStop = (runId: string | null): void => { fenceRef.current = runId; setFencedRun(runId); };
    return <>{['velocity', 'second'].map((name) => <PrimitiveBaseCard key={name} lang="en" bindingPath={`eos/dispatcher/base/${name}`} graph={two} status={statusState} dirty={false} readOnly={false}
      readings={{ ...ready(), 'eos/hardware/base/other': { status: { state: 'ready' }, statusAt: Date.now(), health: { state: 'healthy' }, healthAt: Date.now() } }} clock={Date.now()}
      stopFenced={fencedRun === statusState.run_id} isStopFenced={() => fenceRef.current === statusState.run_id} onStopIntent={markStop} onStop={() => undefined} onOpenLogs={() => undefined} />)}</>;
  }
  await act(async () => root.render(<Pair />));
  const cards = (): HTMLElement[] => [...container.querySelectorAll<HTMLElement>('.eos-control__card')];
  await act(async () => (cards()[1].querySelector('button.eos-control__primary') as HTMLButtonElement).click());
  expect(cards()[1].textContent).toContain('InProgress');
  await act(async () => { await new Promise((resolve) => setTimeout(resolve, 1100)); });
  expect(bodies.filter((body) => body.op === 'result')).toHaveLength(1);
  await act(async () => (cards()[0].querySelectorAll('button')[1] as HTMLButtonElement).click());
  expect(cards()[1].textContent).toContain('Deployment Stop requested');
  const commandId = (bodies.find((body) => body.op === 'result') as { command_id: string }).command_id;
  await act(async () => finishResult(response({ ok: true, result: { command_id: commandId, status: 'Completed', reason: null } })));
  expect(cards()[1].textContent).toContain('InProgress');
  expect(cards()[1].textContent).not.toContain('Owner status: Completed');
  await act(async () => finishStop({ stopped: ['eos'], terminal: { run_id: 'run-a', outcome: 'unknown', reason_code: 'eos_stop_unconfirmed', processes: [{ id: 'eos', outcome: 'unknown', reason_code: 'eos_stop_unconfirmed' }] } }));
  await act(async () => root.unmount()); container.remove(); vi.unstubAllGlobals(); graphRead = graph;
});

it('reads the final IsOS fence before obtaining the short EOS command deadline', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  statusState = { ...process, active_revision: revision };
  let releaseStatus!: (value: ProcessStatus) => void;
  const pendingStatus = new Promise<ProcessStatus>((resolve) => { releaseStatus = resolve; });
  let statusReads = 0;
  statusRead = async () => { statusReads += 1; return statusReads === 2 ? pendingStatus : statusState; };
  const operations: string[] = [];
  vi.stubGlobal('fetch', vi.fn(async (_url: string, init: RequestInit) => {
    const body = JSON.parse(init.body as string) as Record<string, unknown>;
    operations.push(String(body.op));
    if (body.op === 'time') return response({ ok: true, monotonic_ns: 1_000_000_000 });
    expect((body.header as { deadline_monotonic_ns: number }).deadline_monotonic_ns).toBe(5_000_000_000);
    return response({ ok: true, result: { command_id: (body.header as { command_id: string }).command_id, status: 'Completed', reason: null } });
  }));
  const container = document.createElement('div'); document.body.appendChild(container);
  const root = createRoot(container); const { PrimitiveBaseCard } = await import('./PrimitiveBaseCard');
  await act(async () => root.render(<PrimitiveBaseCard lang="en" graph={graph} status={statusState} dirty={false} readOnly={false} readings={ready()} clock={Date.now()} visible onStop={() => undefined} onOpenLogs={() => undefined} />));
  await act(async () => { [...container.querySelectorAll('button')].find((button) => button.textContent === 'Invoke')!.click(); });
  expect(statusReads).toBe(2);
  expect(operations).toEqual([]);
  await act(async () => { releaseStatus(statusState); });
  expect(operations.slice(0, 2)).toEqual(['time', 'primitive_typed']);
  expect(container.textContent).toContain('Owner status: Completed');
  await act(async () => root.unmount()); container.remove(); vi.unstubAllGlobals(); statusRead = async () => statusState;
});

it('does not send if local authority changes while EOS time is pending', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  statusState = { ...process, active_revision: revision };
  statusRead = async () => statusState;
  const { PrimitiveBaseCard } = await import('./PrimitiveBaseCard');
  for (const change of ['dirty', 'stop', 'binding'] as const) {
    vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
    let releaseTime!: (value: ReturnType<typeof response>) => void;
    const pendingTime = new Promise<ReturnType<typeof response>>((resolve) => { releaseTime = resolve; });
    const operations: string[] = [];
    vi.stubGlobal('fetch', vi.fn(async (_url: string, init: RequestInit) => {
      const body = JSON.parse(init.body as string) as Record<string, unknown>;
      operations.push(String(body.op));
      if (body.op === 'time') return pendingTime;
      return response({ ok: true, result: { command_id: (body.header as { command_id: string }).command_id, status: 'Completed', reason: null } });
    }));
    const container = document.createElement('div'); document.body.appendChild(container);
    const root = createRoot(container);
    const render = (changed: boolean) => <PrimitiveBaseCard lang="en" graph={graph} status={statusState} dirty={changed && change === 'dirty'} readOnly={false} stopFenced={changed && change === 'stop'} bindingPath={changed && change === 'binding' ? 'missing-binding' : undefined} readings={ready()} clock={Date.now()} visible onStop={() => undefined} onOpenLogs={() => undefined} />;
    await act(async () => root.render(render(false)));
    await act(async () => { [...container.querySelectorAll('button')].find((button) => button.textContent === 'Invoke')!.click(); });
    expect(operations).toEqual(['time']);
    await act(async () => root.render(render(true)));
    await act(async () => { releaseTime(response({ ok: true, monotonic_ns: 1_000_000_000 })); });
    expect(operations).toEqual(['time']);
    await act(async () => root.unmount()); container.remove(); vi.unstubAllGlobals();
  }
  statusRead = async () => statusState;
});
