// @vitest-environment jsdom
import React, { act } from 'react';
import { createRoot } from 'react-dom/client';
import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import type { GraphCatalogSnapshot, ProcessStatus } from '@/services/isos/node-graph';
import { getBrowserLogs } from '@/services/isos/browser-logs';

const mocks = vi.hoisted(() => ({ snapshot: vi.fn(), status: vi.fn(), eos: vi.fn() }));
vi.mock('@/services/isos/node-graph', async (importOriginal) => ({
  ...(await importOriginal<typeof import('@/services/isos/node-graph')>()),
  getGraphCatalogSnapshot: mocks.snapshot,
  getProcessStatus: mocks.status,
}));
vi.mock('@/services/eos/control', () => ({ eosRequest: mocks.eos }));
vi.mock('./VisionPanel', () => ({ VisionPanel: () => null }));
vi.mock('./PrimitiveBaseCard', () => ({
  PrimitiveBaseCard: ({ readOnly, graph, status, readings }: { readOnly: boolean; graph: { revision: string; graph: { robot: { id: string } } } | null; status: ProcessStatus | null; readings: Record<string, { health?: { state: string } }> }) =>
    <button type="button" disabled={readOnly || !graph || graph.revision !== status?.active_revision || (graph.graph.robot.id === 'live' && readings['isos/eos_hardware/base/drive']?.health?.state !== 'healthy')}>Invoke</button>,
}));

const revisionA = 'a'.repeat(64);
const revisionB = 'b'.repeat(64);
const graph = { version: '0.2.0', robot: { id: 'test', name: 'Test' }, cores: {} };
const liveGraph = { version: '0.2.0', robot: { id: 'live', name: 'Live' }, cores: { isos: { layers: { eos_hardware: { groups: { base: { nodes: { drive: { id: 'drive', node_type: 'isos.eos_hardware.base@v0.2', label: 'Drive', description: '' } } } } } } } } };
const liveType = { id: 'isos.eos_hardware.base@v0.2', core: 'isos', layer: 'eos_hardware', group: 'base', kind: 'base' as const, lifecycle: 'adapter' as const, capabilities: [], settings: [], inputs: [], outputs: [] };
const snapshot = (revision: string, reason: string | null = null): GraphCatalogSnapshot => ({
  graph: { revision, graph }, selected: reason ? null : { types: [] }, global: { types: [] }, reason,
});
const liveSnapshot = (): GraphCatalogSnapshot => ({ graph: { revision: revisionA, graph: liveGraph }, selected: { types: [liveType] }, global: { types: [] }, reason: null });
const status = (revision: string): ProcessStatus => ({
  schema_version: '1.0.0', state: 'running', run_id: 'run-a', run_id_state: 'known',
  saved_revision: revision, active_revision: revision, processes: [],
});
function deferred<T>() {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((done) => { resolve = done; });
  return { promise, resolve };
}
async function mount() {
  const { EosWorkbench } = await import('./EosWorkbench');
  const callbacks = { onStatus: vi.fn(), onRobot: vi.fn(), onConnectionError: vi.fn() };
  const container = document.createElement('div'); document.body.appendChild(container);
  const root = createRoot(container);
  await act(async () => root.render(<EosWorkbench lang="en" graphDirty={false} onOpenGraph={() => undefined} {...callbacks} />));
  const invoke = () => [...container.querySelectorAll('button')].find((button) => button.textContent === 'Invoke') as HTMLButtonElement;
  const refresh = () => [...container.querySelectorAll('button')].find((button) => button.textContent === 'Refresh') as HTMLButtonElement;
  return { container, root, callbacks, invoke, refresh };
}

beforeEach(() => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  vi.useFakeTimers();
  mocks.status.mockResolvedValue(status(revisionA));
  mocks.eos.mockResolvedValue({ nodes: [] });
});
afterEach(() => {
  vi.useRealTimers(); vi.clearAllMocks(); vi.unstubAllGlobals(); document.body.innerHTML = '';
});

it('publishes a catalog read that outlasts two automatic refresh ticks', async () => {
  const slow = deferred<GraphCatalogSnapshot>();
  mocks.snapshot.mockReturnValueOnce(slow.promise).mockResolvedValue(snapshot(revisionA));
  const view = await mount();
  await act(async () => { await vi.advanceTimersByTimeAsync(11_000); });
  expect(mocks.snapshot).toHaveBeenCalledTimes(1);
  await act(async () => slow.resolve(snapshot(revisionA)));
  expect(view.container.textContent).toContain(revisionA);
  expect(view.invoke().disabled).toBe(false);
  expect(mocks.eos).toHaveBeenCalledWith({ op: 'discover' });
  await act(async () => { await vi.advanceTimersByTimeAsync(5000); });
  expect(mocks.snapshot).toHaveBeenCalledTimes(2);
  await act(async () => view.root.unmount());
});

it('coalesces repeated manual clicks, suppresses the old result and gates Invoke until the new owner response', async () => {
  const old = deferred<GraphCatalogSnapshot>();
  const fresh = deferred<GraphCatalogSnapshot>();
  mocks.snapshot.mockReturnValueOnce(old.promise).mockReturnValueOnce(fresh.promise);
  mocks.status.mockResolvedValueOnce(status(revisionA)).mockResolvedValueOnce(status(revisionB));
  const view = await mount();
  await act(async () => { view.refresh().click(); view.refresh().click(); });
  expect(view.invoke().disabled).toBe(true);
  await act(async () => old.resolve(snapshot(revisionA)));
  expect(mocks.snapshot).toHaveBeenCalledTimes(2);
  expect(view.container.textContent).not.toContain(revisionA);
  expect(view.invoke().disabled).toBe(true);
  await act(async () => fresh.resolve(snapshot(revisionB)));
  expect(view.container.textContent).toContain(revisionB);
  expect(view.invoke().disabled).toBe(false);
  expect(mocks.snapshot).toHaveBeenCalledTimes(2);
  await act(async () => view.root.unmount());
});

it('keeps a selected-catalog mismatch read-only despite a populated global catalog', async () => {
  mocks.snapshot.mockResolvedValue(snapshot(revisionA, 'Selected node type catalog revision does not match the saved graph.'));
  const view = await mount();
  expect(view.container.textContent).toContain('Read-only graph: Selected node type catalog revision does not match');
  expect(view.invoke().disabled).toBe(true);
  expect(mocks.eos).not.toHaveBeenCalled();
  await act(async () => view.root.unmount());
});

it('revokes the last run when IsOS process status becomes unavailable', async () => {
  mocks.snapshot.mockResolvedValue(snapshot(revisionA));
  const view = await mount();
  expect(view.invoke().disabled).toBe(false);
  mocks.status.mockRejectedValueOnce(new Error('status unavailable'));
  const logCount = getBrowserLogs().length;
  await act(async () => { await vi.advanceTimersByTimeAsync(5000); });
  expect(view.invoke().disabled).toBe(true);
  expect(getBrowserLogs().slice(logCount).map((line) => line.message)).toContain('status unavailable');
  expect(view.callbacks.onStatus).toHaveBeenLastCalledWith(null);
  await act(async () => view.root.unmount());
});

it('ignores a catalog completion after unmount and starts no queued manual read', async () => {
  const slow = deferred<GraphCatalogSnapshot>();
  mocks.snapshot.mockReturnValue(slow.promise);
  const view = await mount();
  await act(async () => view.refresh().click());
  await act(async () => view.root.unmount());
  await act(async () => slow.resolve(snapshot(revisionA)));
  await act(async () => { await vi.advanceTimersByTimeAsync(15_000); });
  expect(mocks.snapshot).toHaveBeenCalledTimes(1);
  expect(view.callbacks.onStatus).not.toHaveBeenCalled();
  expect(view.callbacks.onRobot).not.toHaveBeenCalled();
  expect(view.callbacks.onConnectionError).not.toHaveBeenCalled();
});

it('ignores discovery after unmount and keeps a slow read through callback identity changes', async () => {
  const slow = deferred<GraphCatalogSnapshot>();
  const discovery = deferred<{ nodes: [] }>();
  mocks.snapshot.mockReturnValue(slow.promise);
  mocks.eos.mockReturnValue(discovery.promise);
  const view = await mount();
  const replacement = vi.fn();
  const { EosWorkbench } = await import('./EosWorkbench');
  await act(async () => view.root.render(<EosWorkbench lang="en" graphDirty={false} onOpenGraph={() => undefined} onStatus={replacement} />));
  expect(mocks.snapshot).toHaveBeenCalledTimes(1);
  await act(async () => slow.resolve(snapshot(revisionA)));
  expect(view.callbacks.onStatus).not.toHaveBeenCalled();
  expect(replacement).toHaveBeenCalledOnce();
  await act(async () => view.root.unmount());
  await act(async () => discovery.resolve({ nodes: [] }));
  await act(async () => { await vi.advanceTimersByTimeAsync(10_000); });
  expect(mocks.snapshot).toHaveBeenCalledTimes(1);
  expect(view.callbacks.onConnectionError).not.toHaveBeenCalled();
});

it('restarts one owner read after StrictMode effect cleanup without publishing the old flight', async () => {
  const old = deferred<GraphCatalogSnapshot>();
  mocks.snapshot.mockReturnValueOnce(old.promise).mockResolvedValueOnce(snapshot(revisionA));
  const { EosWorkbench } = await import('./EosWorkbench');
  const container = document.createElement('div'); document.body.appendChild(container);
  const root = createRoot(container);
  const onStatus = vi.fn();
  await act(async () => root.render(<React.StrictMode><EosWorkbench lang="en" graphDirty={false} onOpenGraph={() => undefined} onStatus={onStatus} /></React.StrictMode>));
  expect(mocks.snapshot).toHaveBeenCalledTimes(1);
  await act(async () => old.resolve(snapshot(revisionB)));
  expect(mocks.snapshot).toHaveBeenCalledTimes(2);
  expect(container.textContent).toContain(revisionA);
  expect(container.textContent).not.toContain(revisionB);
  expect(onStatus).toHaveBeenCalledOnce();
  await act(async () => root.unmount());
});

it('closes an active control immediately on manual Refresh and keeps it closed on owner failure', async () => {
  const failing = deferred<GraphCatalogSnapshot>();
  mocks.snapshot.mockResolvedValueOnce(snapshot(revisionA)).mockReturnValueOnce(failing.promise);
  const view = await mount();
  expect(view.invoke().disabled).toBe(false);
  await act(async () => view.refresh().click());
  expect(view.invoke().disabled).toBe(true);
  await act(async () => failing.resolve(snapshot(revisionA, 'Selected node type catalog is unavailable.')));
  expect(view.container.textContent).toContain('Read-only graph: Selected node type catalog is unavailable.');
  expect(view.invoke().disabled).toBe(true);
  await act(async () => view.root.unmount());
});

it('clears old live health before exposing a new run with the same graph revision', async () => {
  const nextDiscovery = deferred<{ nodes: { path: string; node_id: string; kind: string; label: string; facets: string[]; capabilities: string[] }[] }>();
  const oldHealth = deferred<{ health: { state: string } }>();
  const newHealth = deferred<{ health: { state: string } }>();
  const discovered = { nodes: [{ path: 'isos/eos_hardware/base/drive', node_id: 'drive', kind: 'base', label: 'Drive', facets: [], capabilities: [] }] };
  mocks.snapshot.mockResolvedValue(liveSnapshot());
  mocks.status.mockResolvedValueOnce(status(revisionA)).mockResolvedValueOnce({ ...status(revisionA), run_id: 'run-b' });
  mocks.eos.mockImplementation(({ op }: { op: string }) => {
    if (op === 'discover') return mocks.eos.mock.calls.filter(([body]) => body.op === 'discover').length === 1 ? Promise.resolve(discovered) : nextDiscovery.promise;
    if (op === 'status') return Promise.resolve({ status: { state: 'ready' } });
    if (op === 'health') {
      const count = mocks.eos.mock.calls.filter(([body]) => body.op === 'health').length;
      return count === 1 ? Promise.resolve({ health: { state: 'healthy' } }) : count === 2 ? oldHealth.promise : newHealth.promise;
    }
    return Promise.resolve({ observation: {} });
  });
  const view = await mount();
  expect(view.invoke().disabled).toBe(false);
  await act(async () => { await vi.advanceTimersByTimeAsync(2000); });
  await act(async () => { await vi.advanceTimersByTimeAsync(5000); });
  expect(view.callbacks.onStatus).toHaveBeenLastCalledWith(expect.objectContaining({ run_id: 'run-b' }));
  expect(view.invoke().disabled).toBe(true);
  await act(async () => nextDiscovery.resolve(discovered));
  await act(async () => oldHealth.resolve({ health: { state: 'healthy' } }));
  expect(view.invoke().disabled).toBe(true);
  await act(async () => newHealth.resolve({ health: { state: 'healthy' } }));
  expect(view.invoke().disabled).toBe(false);
  await act(async () => view.root.unmount());
});
