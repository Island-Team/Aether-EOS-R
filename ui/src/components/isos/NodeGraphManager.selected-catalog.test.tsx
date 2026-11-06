// @vitest-environment jsdom
import React, { act } from 'react';
import { createRoot } from 'react-dom/client';
import { afterEach, expect, it, vi } from 'vitest';
import type { GraphCatalogSnapshot, ProcessStatus } from '@/services/isos/node-graph';

const mocks = vi.hoisted(() => ({ snapshot: vi.fn(), validate: vi.fn(), apply: vi.fn(), stop: vi.fn(), control: vi.fn(), status: vi.fn(), readiness: vi.fn(), sets: vi.fn() }));
vi.mock('reactflow', () => ({
  default: ({ children, nodes, onNodesChange }: { children?: React.ReactNode; nodes?: { id: string }[]; onNodesChange?: (changes: { id: string; type: 'select'; selected: boolean }[]) => void }) => <div className="react-flow__pane">{nodes?.map((node) => <button key={node.id} type="button" onClick={() => onNodesChange?.([{ id: node.id, type: 'select', selected: true }])}>Select {node.id}</button>)}{children}</div>,
  ReactFlowProvider: ({ children }: { children?: React.ReactNode }) => <div>{children}</div>,
  Background: () => null, MiniMap: () => null, Handle: () => null,
  Position: { Left: 'left', Right: 'right' }, SelectionMode: { Partial: 'partial' },
}));
vi.mock('@/services/isos/node-graph', async (importOriginal) => ({
  ...(await importOriginal<typeof import('@/services/isos/node-graph')>()),
  getGraphCatalogSnapshot: mocks.snapshot,
  getNodeSets: mocks.sets,
  getProcessStatus: mocks.status,
  getCoreReadiness: mocks.readiness,
  getNodeOptions: async () => ({ adapter_families: [], primitive_bindings: [], targets: [] }),
  validateNodeGraph: mocks.validate,
  applyNodeGraph: mocks.apply,
  stopProcesses: mocks.stop,
  controlProcesses: mocks.control,
}));

const graph = { version: '0.2.0', robot: { id: 'test', name: 'Test' }, cores: {} };
const snapshot = (revision: string): GraphCatalogSnapshot => ({ graph: { revision, graph }, selected: { types: [] }, global: { types: [] }, reason: null });
const status: ProcessStatus = { schema_version: '1.0.0', state: 'running', run_id: 'run-a', run_id_state: 'known', saved_revision: 'a'.repeat(64), active_revision: 'a'.repeat(64), processes: [] };

afterEach(() => { vi.resetAllMocks(); vi.unstubAllGlobals(); document.body.innerHTML = ''; });

it('applies a saved sole camera gate before Save and Run starts vision', async () => {
  const revision = 'a'.repeat(64);
  const cameraGraph = { ...graph, cores: { isos: { layers: { eos_interface: { groups: { gate: { nodes: { preview: { id: 'preview', node_type: 'isos.eos_interface.camera_gate@v0.2', inputs: {} } } } } } } } } };
  mocks.snapshot.mockResolvedValue({ graph: { revision, graph: cameraGraph }, selected: { types: [{ id: 'isos.eos_interface.camera_gate@v0.2', core: 'isos', layer: 'eos_interface', group: 'gate', kind: 'software', lifecycle: 'internal' }] }, global: { types: [] }, reason: null });
  mocks.status.mockResolvedValue({ ...status, state: 'stopped', run_id: null, active_revision: null, vision_evidence: null });
  mocks.validate.mockResolvedValue({ valid: true, diagnostics: [], edges: [] });
  mocks.apply.mockResolvedValue({ revision, graph: cameraGraph });
  mocks.control.mockResolvedValue(status);
  const { container, root } = await mount();
  await act(async () => [...container.querySelectorAll('button')].find((item) => item.textContent === 'Save and Run')!.click());
  expect(mocks.apply).toHaveBeenCalledWith(revision, cameraGraph);
  expect(mocks.control).toHaveBeenCalled();
  await act(async () => root.unmount());
});

async function mount() {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  vi.stubGlobal('ResizeObserver', class { observe() {} disconnect() {} });
  vi.stubGlobal('requestAnimationFrame', () => 1);
  vi.stubGlobal('cancelAnimationFrame', () => undefined);
  mocks.sets.mockResolvedValue({ sets: [] });
  mocks.readiness.mockResolvedValue(null);
  const { NodeGraphManager } = await import('./NodeGraphManager');
  const container = document.createElement('div'); document.body.appendChild(container); const root = createRoot(container);
  await act(async () => root.render(<NodeGraphManager lang="en" />));
  return { container, root };
}

it('renders a revision checked graph while optional status and readiness are stalled', async () => {
  mocks.snapshot.mockResolvedValue(snapshot('a'.repeat(64)));
  mocks.status.mockImplementation(() => new Promise(() => undefined));
  mocks.readiness.mockImplementation(() => new Promise(() => undefined));
  mocks.sets.mockImplementation(() => new Promise(() => undefined));
  const { container, root } = await mount();
  expect(container.querySelector('.node-graph-canvas .react-flow__pane')).toBeTruthy();
  expect(container.textContent).toContain('Saved: aaaaaaaa');
  await act(async () => root.unmount());
});

it('keeps the editor open when a selected owner type omits empty catalog arrays', async () => {
  const sparseType = { id: 'isos.ei_cognition.builder@v0.2', core: 'isos', layer: 'ei_cognition', group: 'context', kind: 'software', lifecycle: 'internal' };
  const nodePath = 'isos/ei_cognition/context/builder';
  const graphWithNode = { version: '0.2.0', robot: { id: 'test', name: 'Test' }, cores: { isos: { layers: { ei_cognition: { groups: { context: { nodes: { builder: { id: 'builder', node_type: sparseType.id, label: 'Builder', description: '' } } } } } } } } };
  mocks.snapshot.mockResolvedValue({ graph: { revision: 'a'.repeat(64), graph: graphWithNode }, selected: { types: [sparseType] }, global: { types: [] }, reason: null });
  mocks.status.mockResolvedValue(status);
  const { container, root } = await mount();
  await act(async () => [...container.querySelectorAll('button')].find((button) => button.textContent === `Select ${nodePath}`)!.click());
  expect(container.textContent).toContain('Capabilities');
  expect((container.querySelector('.node-graph-side input[value="Builder"]') as HTMLInputElement | null)?.value).toBe('Builder');
  await act(async () => root.unmount());
});

it('keeps the last graph visible while an explicit snapshot reload is slow', async () => {
  mocks.snapshot.mockResolvedValueOnce(snapshot('a'.repeat(64)));
  mocks.status.mockResolvedValue(status);
  const { container, root } = await mount();
  let finishReload!: (value: GraphCatalogSnapshot) => void;
  mocks.snapshot.mockImplementationOnce(() => new Promise((resolve) => { finishReload = resolve; }));
  await act(async () => { const details = container.querySelector('details.node-graph-advanced')!; details.setAttribute('open', ''); details.dispatchEvent(new Event('toggle')); });
  await act(async () => [...container.querySelectorAll('button')].find((item) => item.textContent === 'Reload saved graph')!.click());
  expect(container.querySelector('.node-graph-canvas .react-flow__pane')).toBeTruthy();
  expect(container.textContent).toContain('Saved: aaaaaaaa');
  await act(async () => finishReload(snapshot('b'.repeat(64))));
  expect(container.textContent).toContain('Saved: bbbbbbbb');
  await act(async () => root.unmount());
});

it('leaves Stop enabled during a pending graph validation', async () => {
  mocks.status.mockResolvedValue(status);
  mocks.snapshot.mockResolvedValue(snapshot('a'.repeat(64)));
  let finishValidation!: (value: { valid: boolean; diagnostics: [] }) => void;
  mocks.validate.mockImplementation(() => new Promise((resolve) => { finishValidation = resolve; }));
  mocks.stop.mockResolvedValue({ stopped: [], terminal: { run_id: 'run-a', outcome: 'confirmed', reason_code: null, processes: [] } });
  const { container, root } = await mount();
  await act(async () => { const details = container.querySelector('details.node-graph-advanced')!; details.setAttribute('open', ''); details.dispatchEvent(new Event('toggle')); });
  const button = (text: string) => [...container.querySelectorAll('button')].find((item) => item.textContent === text)!;
  await act(async () => button('Check graph').click());
  expect((button('Stop') as HTMLButtonElement).disabled).toBe(false);
  await act(async () => button('Stop').click());
  expect(mocks.stop).toHaveBeenCalledOnce();
  await act(async () => finishValidation({ valid: true, diagnostics: [] }));
  await act(async () => root.unmount());
});

it('does not let an old refresh overwrite a newer package catalog fence', async () => {
  mocks.status.mockResolvedValue(status);
  mocks.snapshot.mockResolvedValueOnce(snapshot('a'.repeat(64)));
  const { container, root } = await mount();
  let finishOld!: (value: GraphCatalogSnapshot) => void;
  mocks.snapshot.mockImplementationOnce(() => new Promise((resolve) => { finishOld = resolve; }))
    .mockResolvedValueOnce(snapshot('b'.repeat(64)));
  await act(async () => { const details = container.querySelector('details.node-graph-advanced')!; details.setAttribute('open', ''); details.dispatchEvent(new Event('toggle')); });
  await act(async () => [...container.querySelectorAll('button')].find((item) => item.textContent === 'Reload saved graph')!.click());
  await act(async () => window.dispatchEvent(new Event('isosui:packages-changed')));
  expect(container.textContent).toContain('Saved graph revision changed; refresh before editing.');
  await act(async () => finishOld(snapshot('c'.repeat(64))));
  expect(container.textContent).toContain('Saved graph revision changed; refresh before editing.');
  expect(container.textContent).not.toContain('cccccccc');
  await act(async () => root.unmount());
});

it('keeps Stop available during a launch from stopped and converges after a late Run', async () => {
  const stopped = { ...status, state: 'stopped', run_id: null, processes: [] } as ProcessStatus;
  mocks.status.mockResolvedValue(stopped);
  mocks.snapshot.mockResolvedValue(snapshot('a'.repeat(64)));
  mocks.validate.mockResolvedValue({ valid: true, diagnostics: [], edges: [] });
  let finishRun!: (value: ProcessStatus) => void;
  mocks.control.mockImplementation(() => new Promise((resolve) => { finishRun = resolve; }));
  mocks.stop.mockResolvedValue({ stopped: [], terminal: { run_id: 'run-a', outcome: 'confirmed', reason_code: null, processes: [] } });
  const { container, root } = await mount();
  const button = (text: string) => [...container.querySelectorAll('button')].find((item) => item.textContent === text)!;
  await act(async () => button('Save and Run').click());
  expect((button('Stop') as HTMLButtonElement).disabled).toBe(false);
  await act(async () => button('Stop').click());
  expect(mocks.stop).toHaveBeenCalledOnce();
  await act(async () => finishRun(status));
  expect(mocks.stop).toHaveBeenCalledTimes(2);
  expect(container.textContent).not.toContain('Run confirmed');
  await act(async () => root.unmount());
});

it('fences Graph Run when the workspace Stop arrives from another surface', async () => {
  const stopped = { ...status, state: 'stopped', run_id: null, processes: [] } as ProcessStatus;
  mocks.status.mockResolvedValue(stopped);
  mocks.snapshot.mockResolvedValue(snapshot('a'.repeat(64)));
  mocks.validate.mockResolvedValue({ valid: true, diagnostics: [], edges: [] });
  let finishRun!: (value: ProcessStatus) => void;
  mocks.control.mockImplementation(() => new Promise((resolve) => { finishRun = resolve; }));
  mocks.stop.mockResolvedValue({ stopped: [], terminal: { run_id: 'run-a', outcome: 'confirmed', reason_code: null, processes: [] } });
  const { stopDeploymentConverged } = await import('./deployment-control');
  const { container, root } = await mount();
  await act(async () => [...container.querySelectorAll('button')].find((item) => item.textContent === 'Save and Run')!.click());
  let workspaceStop!: Promise<unknown>;
  await act(async () => { workspaceStop = stopDeploymentConverged(); });
  expect(mocks.stop).toHaveBeenCalledOnce();
  await act(async () => finishRun(status));
  await act(async () => { await workspaceStop; });
  expect(mocks.stop).toHaveBeenCalledTimes(2);
  expect(container.textContent).not.toContain('Run confirmed');
  await act(async () => root.unmount());
});
