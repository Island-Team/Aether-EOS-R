// @vitest-environment jsdom
import React, { act } from 'react';
import { createRoot } from 'react-dom/client';
import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { CatalogBrowser } from './CatalogBrowser';
import { CatalogSelector, settingOptionGroup, unavailableSelections } from './CatalogSelector';
import { catalogReloadGraph, catalogReloadIsCurrent, catalogSelectionBlocks, optionResponseIsCurrent } from './catalog-options';
import { catalogTypeKey, type NodeOptions, type NodeTypeDefinition, type SettingOptionGroup } from '../../services/isos/node-graph';

const group: SettingOptionGroup = { node_path: 'eos/process/mujoco/simulator', setting: 'scene', selected: 'pick-place', options: [
  { value: 'navigation', label: 'Navigation', description: 'Drive to a goal', available: true, reason_code: null, reason: null },
  { value: 'pick-place', label: 'Pick and place', description: 'Saved scene', available: false, reason_code: 'package_changed', reason: 'Relink the exact release' },
] };
const options: NodeOptions = { adapter_families: [], primitive_bindings: [], targets: [], setting_options: [group], graph_revision: null, read_only: false };

describe('P03 catalog options', () => {
  it('uses exact node path and setting, and blocks an unavailable saved ID', () => {
    expect(settingOptionGroup(options, group.node_path, 'scene')).toBe(group);
    expect(settingOptionGroup(options, 'eos/process/mujoco/other', 'scene')).toBeUndefined();
    expect(unavailableSelections(options)).toEqual([group]);
    const html = renderToStaticMarkup(<CatalogSelector group={group} label="Scene" value="pick-place" lang="en" onChange={() => undefined} />);
    expect(html).toContain('aria-invalid="true"');
    expect(html).toContain('package_changed Relink the exact release');
    expect(html).toContain('2 options');
    expect(html).toContain('value="pick-place"');
  });

  it('keeps a selected ID visible even if the server omitted its row', () => {
    const html = renderToStaticMarkup(<CatalogSelector group={{ ...group, options: [group.options[0]] }} label="场景" value="old-scene" lang="cn" onChange={() => undefined} />);
    expect(html).toContain('old-scene');
    expect(html).toContain('不可用');
    expect(html).toContain('role="alert"');
  });

  it('moves from search into the native keyboard selector and clears search on Escape', async () => {
    (globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT: boolean }).IS_REACT_ACT_ENVIRONMENT = true;
    const host = document.createElement('div'); document.body.append(host);
    const root = createRoot(host);
    await act(async () => { root.render(<CatalogSelector group={group} label="Scene" value="navigation" lang="en" onChange={() => undefined} />); });
    const search = host.querySelector('input')!;
    const select = host.querySelector('select')!;
    await act(async () => { Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set?.call(search, 'nav'); search.dispatchEvent(new Event('input', { bubbles: true })); });
    expect(host.textContent).toContain('1 options');
    await act(async () => { search.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })); });
    expect(host.textContent).toContain('2 options');
    await act(async () => { search.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })); });
    expect(document.activeElement).toBe(select);
    await act(async () => { search.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })); });
    expect(document.activeElement).toBe(select);
    await act(async () => { root.unmount(); }); host.remove();
  });

  it('discards a delayed response after a newer draft request', async () => {
    let resolveOld!: (value: NodeOptions) => void;
    const old = new Promise<NodeOptions>((resolve) => { resolveOld = resolve; });
    let latest = 1;
    const accepted = vi.fn();
    const pending = old.then((result) => { if (optionResponseIsCurrent(1, latest, 'old draft', 'new draft')) accepted(result); });
    latest = 2;
    resolveOld(options);
    await pending;
    expect(accepted).not.toHaveBeenCalled();
    expect(optionResponseIsCurrent(2, latest, 'new draft', 'new draft')).toBe(true);
    expect(optionResponseIsCurrent(1, 2, 'same draft', 'same draft')).toBe(false);
  });

  it('discards an older package catalog reload that resolves last', async () => {
    let resolveOld!: (value: string) => void;
    const old = new Promise<string>((resolve) => { resolveOld = resolve; });
    let generation = 1;
    const published: string[] = [];
    const pending = old.then((catalog) => { if (catalogReloadIsCurrent(1, generation)) published.push(catalog); });
    generation = 2;
    if (catalogReloadIsCurrent(2, generation)) published.push('new catalog');
    resolveOld('old catalog');
    await pending;
    expect(published).toEqual(['new catalog']);
  });

  it('keeps the latest edited draft when a package reload resolves', async () => {
    const saved = { version: '0.1.0', robot: { id: 'robot', name: 'Before' }, cores: {} };
    const edited = { ...saved, robot: { ...saved.robot, name: 'After edit' } };
    let resolveReload!: (value: typeof saved) => void;
    const reload = new Promise<typeof saved>((resolve) => { resolveReload = resolve; });
    const current = { graph: saved, revision: 'same-revision' };
    const pending = reload.then((response) => catalogReloadGraph('same-revision', current.revision, current.graph, response));
    current.graph = edited;
    resolveReload(saved);
    expect(await pending).toBe(edited);
  });

  it('blocks an absent required selector and an unavailable selected value for any registered type', () => {
    const type: NodeTypeDefinition = { id: 'example.process.worker@v0.1', core: 'eos', layer: 'process', group: 'worker', kind: 'software', lifecycle: 'process', capabilities: [], settings: [{ name: 'scene', data_type: 'string', required: true, selector: { provider: 'package', resource: 'scene', package_from: 'node.package' } }], inputs: [], outputs: [] };
    const entry = { path: group.node_path, definition: type, node: { id: 'simulator', node_type: type.id, label: 'Simulator', description: '', settings: {} as Record<string, unknown> } };
    expect(catalogSelectionBlocks('0.2.0', [entry], { ...options, setting_options: [{ ...group, selected: null }] })).toBe(true);
    entry.node.settings.scene = 'pick-place';
    expect(catalogSelectionBlocks('0.2.0', [entry], options)).toBe(true);
    expect(catalogSelectionBlocks('1.0.0', [entry], options)).toBe(false);
  });

  it('does not require undeclared adapter-specific settings', () => {
    const type: NodeTypeDefinition = { id: 'example.hardware.sensor@v0.1', core: 'eos', layer: 'hardware', group: 'sensor', kind: 'software', lifecycle: 'adapter', capabilities: [], settings: [], inputs: [], outputs: [] };
    const entry = { path: 'eos/hardware/sensor/front', definition: type, node: { id: 'front', node_type: type.id, label: 'Front', description: '', adapter: { type: 'mujoco-v2', settings: {} } } };
    expect(catalogSelectionBlocks('0.2.0', [entry], null)).toBe(false);
  });

  it('validates a UI-owned key selector without requiring server option groups', () => {
    const type: NodeTypeDefinition = { id: 'isos.ui.key@v0.2', core: 'isos', layer: 'ui', group: 'keyboard', kind: 'software', lifecycle: 'internal', capabilities: [], settings: [{ name: 'key', data_type: 'string', required: true, selector: { provider: 'ui', resource: 'keys' } }], inputs: [], outputs: [] };
    const entry = { path: 'isos/ui/keyboard/drive', definition: type, node: { id: 'drive', node_type: type.id, label: 'Drive', description: '', settings: { key: 'KeyW' } } };
    expect(catalogSelectionBlocks('0.2.0', [entry], null)).toBe(false);
    expect(catalogSelectionBlocks('0.2.0', [{ ...entry, node: { ...entry.node, settings: { key: 'Unidentified' } } }], null)).toBe(true);
  });
});

describe('catalog hierarchy', () => {
  const base: NodeTypeDefinition = { id: 'same@v0.1', core: 'eos', layer: 'hardware', group: 'base', kind: 'base', lifecycle: 'adapter', description: 'Drive a mobile base.', capabilities: [], settings: [], inputs: [], outputs: [] };
  it('keeps the exact core, layer, group and type tuple', () => {
    const other = { ...base, group: 'camera', description: 'Capture frames.' };
    expect(catalogTypeKey(base)).not.toBe(catalogTypeKey(other));
    const html = renderToStaticMarkup(<CatalogBrowser types={[base, other]} value="" lang="en" onChange={() => undefined} />);
    expect(html).toContain('Core');
    expect(html).toContain('Layer');
    expect(html).toContain('Group');
    expect(html).toContain('Node type');
    expect(html).toContain('Search node types');
    expect(renderToStaticMarkup(<CatalogBrowser types={[base]} value="" lang="cn" onChange={() => undefined} />)).toContain('节点类型');
  });
});

afterEach(() => { document.body.innerHTML = ''; });
