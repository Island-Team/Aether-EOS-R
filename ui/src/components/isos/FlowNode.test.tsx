// @vitest-environment jsdom
import React, { act } from 'react';
import { createRoot } from 'react-dom/client';
import { expect, it, vi } from 'vitest';
const liveMock = vi.hoisted(() => vi.fn());
vi.mock('@/services/eos/node-live', () => ({ getNodeLive: liveMock }));
vi.mock('reactflow', async (importOriginal) => ({ ...(await importOriginal<typeof import('reactflow')>()), Handle: () => <span data-testid="handle" /> }));
it('opens one floating node card by focus, hover and touch button without covering handles', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  const { FlowNode } = await import('./NodeGraphManager');
  const container = document.createElement('div'); document.body.appendChild(container); const root = createRoot(container);
  const entry = { path: 'eos/hardware/base/drive', node: { id: 'drive', node_type: 'eos.hardware.base@v0.1', label: 'Drive', description: 'Instance notes' }, definition: { id: 'eos.hardware.base@v0.1', core: 'eos', layer: 'hardware', group: 'base', kind: 'base', lifecycle: 'adapter', description: 'Catalog notes', capabilities: [], settings: [], inputs: [{ name: 'command', data_type: 'base_movement', required: true }], outputs: [] } };
  const props = { id: entry.path, type: 'profile', data: { entry, error: false, mirrored: false, lang: 'en', liveAllowed: false, liveRunId: null, dataTypes: [{ id: 'base_movement', version: '1.0', purpose: '', media_type: 'application/json', codec: 'json_v1', max_payload_bytes: 1024, schema: { properties: { x_m_s: { type: 'number', 'x-unit': 'm/s' } } } }] }, selected: false } as unknown as Parameters<typeof FlowNode>[0];
  await act(async () => root.render(<FlowNode {...props} />));
  const node = container.querySelector('.node-graph-card') as HTMLElement;
  await act(async () => node.dispatchEvent(new FocusEvent('focusin', { bubbles: true })));
  expect(document.body.querySelector('.node-graph-info-card')?.textContent).toContain('Catalog notes');
  expect(document.body.querySelector('.node-graph-info-card')?.textContent).toContain('Instance notes');
  expect(document.body.querySelector('.node-graph-info-card')?.textContent).toContain('m/s');
  expect(container.querySelector('[data-testid="handle"]')).not.toBeNull();
  expect(container.querySelector('.node-graph-card__details')).toBeNull();
  await act(async () => { (document.body.querySelector('.node-graph-info-card button') as HTMLButtonElement).click(); });
  expect(document.body.querySelector('.node-graph-info-card')).toBeNull();
  await act(async () => { node.dispatchEvent(new Event('pointerdown', { bubbles: true })); node.dispatchEvent(new FocusEvent('focusin', { bubbles: true })); node.dispatchEvent(new Event('pointerup', { bubbles: true })); });
  expect(document.body.querySelector('.node-graph-info-card')).toBeNull();
  await act(async () => node.dispatchEvent(new MouseEvent('mouseover', { bubbles: true, clientX: 790, clientY: 590 })));
  expect(document.body.querySelector('.node-graph-info-card')).toBeNull();
  await act(async () => { await new Promise((resolve) => setTimeout(resolve, 580)); });
  expect(document.body.querySelector('.node-graph-info-card')).not.toBeNull();
  await act(async () => node.dispatchEvent(new KeyboardEvent('keydown', { bubbles: true, key: 'Escape' })));
  expect(document.body.querySelector('.node-graph-info-card')).toBeNull();
  await act(async () => (container.querySelector('.node-graph-card__info') as HTMLButtonElement).click());
  expect(document.body.querySelector('.node-graph-info-card')).not.toBeNull();
  await act(async () => root.unmount()); container.remove(); vi.unstubAllGlobals();
});


it('refreshes an open EOS node card and replaces ready health with an owner fault', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  liveMock.mockReset().mockResolvedValueOnce({ status: 'ready', health: 'healthy' }).mockResolvedValue({ status: 'stopped', health: 'fault' });
  const { FlowNode } = await import('./NodeGraphManager');
  const container = document.createElement('div'); document.body.appendChild(container); const root = createRoot(container);
  const entry = { path: 'eos/hardware/base/drive', node: { id: 'drive', node_type: 'eos.hardware.base@v0.1', label: 'Drive', description: '' }, definition: { id: 'eos.hardware.base@v0.1', core: 'eos', layer: 'hardware', group: 'base', kind: 'base', lifecycle: 'adapter', description: 'Base type', capabilities: [], settings: [], inputs: [], outputs: [] } };
  const props = { id: entry.path, type: 'profile', data: { entry, error: false, mirrored: false, lang: 'en', liveAllowed: true, liveRunId: 'run-a', dataTypes: [] }, selected: false } as unknown as Parameters<typeof FlowNode>[0];
  await act(async () => root.render(<FlowNode {...props} />));
  await act(async () => (container.querySelector('.node-graph-card__info') as HTMLButtonElement).click());
  expect(document.body.querySelector('.node-graph-info-card')?.textContent).toContain('ready · healthy');
  await act(async () => { await new Promise((resolve) => setTimeout(resolve, 2100)); });
  expect(document.body.querySelector('.node-graph-info-card')?.textContent).toContain('stopped · fault');
  expect(document.body.querySelector('.node-graph-info-card')?.textContent).not.toContain('ready · healthy');
  await act(async () => root.unmount()); container.remove(); vi.unstubAllGlobals();
});
