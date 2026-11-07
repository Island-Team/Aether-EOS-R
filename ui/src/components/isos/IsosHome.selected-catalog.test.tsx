// @vitest-environment jsdom
import React, { act } from 'react';
import { createRoot } from 'react-dom/client';
import { afterEach, expect, it, vi } from 'vitest';
import type { GraphCatalogSnapshot, ProcessStatus } from '@/services/isos/node-graph';

const mocks = vi.hoisted(() => ({
  snapshot: vi.fn(), status: vi.fn(), control: vi.fn(), stop: vi.fn(),
}));
vi.mock('@/services/isos/node-graph', async (importOriginal) => ({
  ...(await importOriginal<typeof import('@/services/isos/node-graph')>()),
  getGraphCatalogSnapshot: mocks.snapshot,
  getProcessStatus: mocks.status,
  getInstalledPackages: async () => ({ installed: [] }),
  getSetupStatus: async () => null,
  getCoreReadiness: async () => null,
  controlProcesses: mocks.control,
  stopProcesses: mocks.stop,
}));

const graph = { version: '0.2.0', robot: { id: 'test', name: 'Test' }, cores: {} };
const snapshot = (revision: string): GraphCatalogSnapshot => ({ graph: { revision, graph }, selected: { types: [] }, global: { types: [] }, reason: null });
const status: ProcessStatus = { schema_version: '1.0.0', state: 'running', run_id: 'run-a', run_id_state: 'known', saved_revision: 'b'.repeat(64), active_revision: 'b'.repeat(64), processes: [] };

afterEach(() => { vi.clearAllMocks(); vi.unstubAllGlobals(); document.body.innerHTML = ''; });

it('polls status without repeating graph/package reads and skips an overlapping status request', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  let tick!: () => void;
  const interval = vi.spyOn(window, 'setInterval').mockImplementation((callback) => { tick = callback as () => void; return 1 as unknown as ReturnType<typeof setInterval>; });
  const clear = vi.spyOn(window, 'clearInterval').mockImplementation(() => undefined);
  mocks.snapshot.mockResolvedValue(snapshot('b'.repeat(64)));
  mocks.status.mockResolvedValueOnce(status).mockImplementation(() => new Promise(() => undefined));
  const { IsosHome } = await import('./IsosHome');
  const container = document.createElement('div'); document.body.appendChild(container); const root = createRoot(container);
  await act(async () => root.render(<IsosHome lang="en" onOpenGraph={() => undefined} graphDirty={false} />));
  await act(async () => { tick(); tick(); });
  expect(mocks.status).toHaveBeenCalledTimes(2);
  expect(mocks.snapshot).toHaveBeenCalledOnce();
  await act(async () => root.unmount());
  interval.mockRestore(); clear.mockRestore();
});

it('keeps the newer selected graph when an older refresh completes late', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  let finishOld!: (value: GraphCatalogSnapshot) => void;
  mocks.snapshot.mockImplementationOnce(() => new Promise((resolve) => { finishOld = resolve; }))
    .mockResolvedValueOnce(snapshot('b'.repeat(64)));
  mocks.status.mockResolvedValue(status);
  const { IsosHome } = await import('./IsosHome');
  const container = document.createElement('div'); document.body.appendChild(container); const root = createRoot(container);
  await act(async () => root.render(<IsosHome lang="en" onOpenGraph={() => undefined} graphDirty={false} />));
  await act(async () => [...container.querySelectorAll('button')].find((button) => button.textContent === 'Refresh')!.click());
  expect(container.textContent).toContain('b'.repeat(64));
  await act(async () => finishOld(snapshot('a'.repeat(64))));
  expect(container.textContent).toContain('b'.repeat(64));
  expect(container.textContent).not.toContain('a'.repeat(64));
  await act(async () => root.unmount());
});

it('keeps Stop callable while a Run request is pending', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  mocks.snapshot.mockResolvedValue(snapshot('b'.repeat(64)));
  const stopped = { ...status, state: 'stopped', run_id: null } as ProcessStatus;
  mocks.status.mockResolvedValueOnce(status).mockResolvedValue(stopped);
  let finishRun!: (value: ProcessStatus) => void;
  mocks.control.mockImplementation(() => new Promise((resolve) => { finishRun = resolve; }));
  mocks.stop.mockResolvedValue({ stopped: [], terminal: { run_id: 'run-a', outcome: 'confirmed', reason_code: null, processes: [] } });
  const { IsosHome } = await import('./IsosHome');
  const container = document.createElement('div'); document.body.appendChild(container); const root = createRoot(container);
  await act(async () => root.render(<IsosHome lang="en" onOpenGraph={() => undefined} graphDirty={false} />));
  const button = (text: string) => [...container.querySelectorAll('button')].find((item) => item.textContent === text)!;
  await act(async () => button('Run').click());
  expect((button('Stop') as HTMLButtonElement).disabled).toBe(false);
  await act(async () => button('Stop').click());
  expect(mocks.stop).toHaveBeenCalledOnce();
  await act(async () => finishRun(status));
  expect(mocks.stop).toHaveBeenCalledTimes(2);
  expect(container.querySelector('.isos-home__overview')?.textContent).toContain('stopped');
  expect(mocks.snapshot).toHaveBeenCalledTimes(2);
  await act(async () => root.unmount());
});

it('fences Home Run when the workspace Stop arrives from another surface', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  mocks.snapshot.mockResolvedValue(snapshot('b'.repeat(64)));
  mocks.status.mockResolvedValue(status);
  let finishRun!: (value: ProcessStatus) => void;
  mocks.control.mockImplementation(() => new Promise((resolve) => { finishRun = resolve; }));
  mocks.stop.mockResolvedValue({ stopped: [], terminal: { run_id: 'run-a', outcome: 'confirmed', reason_code: null, processes: [] } });
  const { stopDeploymentConverged } = await import('./deployment-control');
  const { IsosHome } = await import('./IsosHome');
  const container = document.createElement('div'); document.body.appendChild(container); const root = createRoot(container);
  await act(async () => root.render(<IsosHome lang="en" onOpenGraph={() => undefined} graphDirty={false} />));
  await act(async () => [...container.querySelectorAll('button')].find((item) => item.textContent === 'Run')!.click());
  let workspaceStop!: Promise<unknown>;
  await act(async () => { workspaceStop = stopDeploymentConverged(); });
  expect(mocks.stop).toHaveBeenCalledOnce();
  await act(async () => finishRun(status));
  await act(async () => { await workspaceStop; });
  expect(mocks.stop).toHaveBeenCalledTimes(2);
  expect(mocks.snapshot).toHaveBeenCalledTimes(2);
  await act(async () => root.unmount());
});
