import { describe, expect, it, vi } from 'vitest';
import { applyNodeGraph, combineNodeTypeCatalogs, controlProcesses, stopProcesses, getCoreReadiness, getGraphCatalogSnapshot, graphCatalogReadOnlyReason, previewBackendLink, getInstalledPackages, getLogPage, getNodeOptions, getNodeSets, installBackend, linkBackend, nodeGraphReadOnlyReason, previewNodeSet, validateNodeGraph, type NodeGraph, type NodeSetDefinition, type NodeTypeDefinition } from './node-graph';
import { request } from './api-client';

vi.mock('./api-client', () => ({ request: vi.fn() }));

describe('graph reader compatibility', () => {
  it('edits only the selected 0.2.0 isos core and keeps legacy graphs read-only', () => {
    const type: NodeTypeDefinition = { id: 'isos.eos_hardware.base@v0.2', core: 'isos', layer: 'eos_hardware', group: 'base', kind: 'base', lifecycle: 'adapter', capabilities: [], settings: [], inputs: [], outputs: [] };
    const current: NodeGraph = { version: '0.2.0', robot: { id: 'test', name: 'Test' }, cores: { isos: { layers: { eos_hardware: { groups: { base: { nodes: { drive: { id: 'drive', node_type: type.id, label: 'Drive', description: '' } } } } } } } } };
    expect(nodeGraphReadOnlyReason({ revision: 'r', graph: current }, [type])).toBeNull();
    expect(nodeGraphReadOnlyReason({ revision: 'r', graph: { ...current, version: '0.1.0' } }, [type])).toContain('migration');
    expect(nodeGraphReadOnlyReason({ revision: 'r', graph: { ...current, cores: { ...current.cores, eos: { layers: {} } } } }, [type])).toContain('only the isos core');
  });
  const knownType: NodeTypeDefinition = { id: 'isos.eos_dispatcher.control@v0.2', core: 'isos', layer: 'eos_dispatcher', group: 'control', kind: 'software', lifecycle: 'internal', capabilities: [], settings: [], inputs: [], outputs: [] };
  const graph: NodeGraph = { version: '0.2.0', robot: { id: 'test', name: 'Test' }, cores: { isos: { layers: { eos_dispatcher: { groups: { control: { nodes: { command: { id: 'command', node_type: knownType.id, label: 'Command', description: 'Admission', future_metadata: { retained: true } } } } } } } } }, future_graph_field: 'retained' };

  it('uses the server read-only state and preserves additive fields', () => {
    const document = { revision: 'saved', graph, read_only: true, diagnostics: [{ path: '/version', code: 'migration_required', message: 'Migrate this graph.' }] };
    expect(nodeGraphReadOnlyReason(document, [knownType])).toBe('Migrate this graph.');
    expect(JSON.parse(JSON.stringify(document.graph)).cores.isos.layers.eos_dispatcher.groups.control.nodes.command.future_metadata).toEqual({ retained: true });
  });

  it('blocks unknown versions, unknown types, and missing catalog without server guidance', () => {
    expect(nodeGraphReadOnlyReason({ revision: 'saved', graph: { ...graph, version: '2.0.0' } }, [knownType])).toContain('migration');
    expect(nodeGraphReadOnlyReason({ revision: 'saved', graph: { ...graph, cores: { isos: { layers: { eos_dispatcher: { groups: { control: { nodes: { command: { ...graph.cores.isos.layers.eos_dispatcher.groups.control.nodes.command, node_type: 'isos.eos_dispatcher.control@v0.3' } } } } } } } } } }, [knownType])).toContain('Unsupported node type');
    expect(nodeGraphReadOnlyReason({ revision: 'saved', graph }, null)).toContain('catalog is unavailable');
    expect(nodeGraphReadOnlyReason({ revision: 'saved', graph }, [knownType])).toBeNull();
  });
  it('uses revision-bound selected owner stages while retaining global package types', async () => {
    const selected = structuredClone(graph);
    selected.version = '0.2.0';
    selected.cores.isos.layers.ei_cognition = { groups: { context: { nodes: { builder: { id: 'builder', node_type: 'isos.ei_cognition.builder@v0.2', label: '', description: '' } } } } };
    const stageType: NodeTypeDefinition = { ...knownType, id: 'isos.ei_cognition.builder@v0.2', core: 'isos', layer: 'ei_cognition', group: 'context' };
    const global = { types: [knownType] };
    vi.mocked(request).mockImplementation(async (path) => {
      if (path === '/graph') return { revision: 'saved', graph: selected };
      if (path === '/catalog/node-types') return global;
      if (path === '/catalog/node-types/selected') return { graph_revision: 'saved', catalog: { types: [knownType, stageType] } };
      throw new Error(path);
    });
    const snapshot = await getGraphCatalogSnapshot();
    expect(snapshot.reason).toBeNull();
    expect(nodeGraphReadOnlyReason(snapshot.graph, snapshot.selected?.types || null)).toBeNull();
    expect(combineNodeTypeCatalogs(snapshot.selected, snapshot.global).types.map((type) => type.id)).toContain(stageType.id);
  });

  it('normalizes omitted empty catalog arrays before the editor selects a node', () => {
    const sparse = { id: 'isos.ei_cognition.builder@v0.2', core: 'isos', layer: 'ei_cognition', group: 'context', kind: 'software', lifecycle: 'internal' } as NodeTypeDefinition;
    const catalog = combineNodeTypeCatalogs({ types: [sparse] }, null);
    expect(catalog.types).toEqual([{ ...sparse, capabilities: [], settings: [], inputs: [], outputs: [] }]);
    expect(sparse).not.toHaveProperty('capabilities');
  });

  it('retries a mismatched selected revision once then fails closed', async () => {
    let graphReads = 0;
    vi.mocked(request).mockImplementation(async (path) => {
      if (path === '/catalog/node-types') return { types: [knownType] };
      if (path === '/graph') { graphReads += 1; return { revision: graphReads === 1 ? 'old' : 'new', graph: { ...graph, version: '0.2.0' } }; }
      if (path === '/catalog/node-types/selected') return { graph_revision: 'stale', catalog: { types: [knownType] } };
      throw new Error(path);
    });
    const snapshot = await getGraphCatalogSnapshot();
    expect(graphReads).toBe(2);
    expect(snapshot.selected).toBeNull();
    expect(snapshot.reason).toContain('does not match');
  });

  it('preserves raw owner graph and fails closed when selected catalog is unavailable', async () => {
    vi.mocked(request).mockImplementation(async (path) => {
      if (path === '/catalog/node-types') return { types: [knownType] };
      if (path === '/graph') return { revision: 'saved', graph: { ...graph, version: '0.2.0' }, read_only: true, diagnostics: [{ path: '/catalog', message: 'Owner unavailable.' }] };
      if (path === '/catalog/node-types/selected') throw new Error('503');
      throw new Error(path);
    });
    const snapshot = await getGraphCatalogSnapshot();
    expect(snapshot.graph.read_only).toBe(true);
    expect(snapshot.selected).toBeNull();
    expect(snapshot.reason).toContain('unavailable');
    expect(graphCatalogReadOnlyReason(snapshot)).toBe('Owner unavailable.');
  });

  it('accepts the blank starter revision when selected catalog matches it', async () => {
    vi.mocked(request).mockImplementation(async (path) => {
      if (path === '/catalog/node-types') return { types: [knownType] };
      if (path === '/graph') return { revision: '', graph: { ...graph, version: '0.2.0' } };
      if (path === '/catalog/node-types/selected') return { graph_revision: '', catalog: { types: [knownType] } };
      throw new Error(path);
    });
    const snapshot = await getGraphCatalogSnapshot();
    expect(snapshot.reason).toBeNull();
    expect(snapshot.selected?.types).toEqual([knownType]);
  });
  it('rejects a registered type ID at an unregistered placement', () => {
    const misplaced = structuredClone(graph);
    misplaced.cores.isos.layers.eos_dispatcher.groups.control.nodes.command.node_type = knownType.id;
    expect(nodeGraphReadOnlyReason({ revision: 'saved', graph: misplaced }, [{ ...knownType, group: 'different' }])).toContain('Unsupported node type');
  });

  it('keeps a future graph tree in raw inspection when its shape is unknown', () => {
    const future = { version: '0.2.0', robot: { id: 'test', name: 'Test' }, domains: {} } as unknown as NodeGraph;
    expect(nodeGraphReadOnlyReason({ revision: 'saved', graph: future }, [knownType])).toContain('structure cannot be edited');
  });

  it('blocks malformed inputs on a known graph before the UI renders a wire', () => {
    const malformed = structuredClone(graph);
    malformed.cores.isos.layers.eos_dispatcher.groups.control.nodes.command.inputs = { command: null } as unknown as NonNullable<typeof malformed.cores.isos.layers.eos_dispatcher.groups.control.nodes.command.inputs>;
    expect(nodeGraphReadOnlyReason({ revision: 'saved', graph: malformed }, [knownType])).toContain('structure cannot be edited');
  });

  it('blocks malformed hardware metadata before the inspector or controls use it', () => {
    const malformed = structuredClone(graph);
    const node = malformed.cores.isos.layers.eos_dispatcher.groups.control.nodes.command;
    node.movement = { modes: [], limits: null } as unknown as NonNullable<typeof node.movement>;
    expect(nodeGraphReadOnlyReason({ revision: 'saved', graph: malformed }, [knownType])).toContain('structure cannot be edited');
    delete node.movement;
    node.capabilities = {} as unknown as string[];
    expect(nodeGraphReadOnlyReason({ revision: 'saved', graph: malformed }, [knownType])).toContain('structure cannot be edited');
  });

  it('blocks object-valued labels before React renders node cards', () => {
    const malformed = structuredClone(graph);
    malformed.cores.isos.layers.eos_dispatcher.groups.control.nodes.command.label = { text: 'Command' } as unknown as string;
    expect(nodeGraphReadOnlyReason({ revision: 'saved', graph: malformed }, [knownType])).toContain('structure cannot be edited');
  });
});

describe('unified log API', () => {
  it('requests a specific source, minimum severity, and cursor', async () => {
    vi.mocked(request).mockClear().mockResolvedValue({ lines: [], next_cursor: null, rotated: false });
    await getLogPage('eos/core', 'warn', 'page 1', 500);
    expect(vi.mocked(request).mock.calls).toEqual([
      ['/runtime/logs?source=eos%2Fcore&level=warn&limit=200&cursor=page+1'],
    ]);
  });
});

describe('IsOS v1 graph and process API', () => {
  it('sends v1 request bodies for validate, apply, and run', async () => {
    const graph: NodeGraph = { version: '1.0.0', robot: { id: 'test', name: 'Test' }, cores: {} };
    vi.mocked(request).mockClear().mockResolvedValue({});
    await validateNodeGraph(graph);
    await applyNodeGraph('saved-revision', graph);
    await controlProcesses('run', 'saved-revision');
    expect(vi.mocked(request).mock.calls).toEqual([
      ['/graph/validate', 'POST', { schema_version: '0.1.0', graph }],
      ['/graph', 'PUT', { schema_version: '0.1.0', revision: 'saved-revision', graph }],
      ['/runtime/control', 'POST', { schema_version: '0.1.0', operation: 'run', revision: 'saved-revision' }],
    ]);
  });
  it('keeps Stop on its separate terminal reply route', async () => {
    const reply = { stopped: ['eos'], terminal: { run_id: 'r1', outcome: 'confirmed', reason_code: null, processes: [] } };
    vi.mocked(request).mockClear().mockResolvedValue(reply);
    expect(await stopProcesses()).toEqual(reply);
    expect(vi.mocked(request).mock.calls).toEqual([['/runtime/control', 'POST', { schema_version: '0.1.0', operation: 'stop' }]]);
  });
});

describe('package node set API', () => {
  it('lists installed contributions and previews composition without applying a graph revision', async () => {
    vi.mocked(request).mockClear();
    const graph: NodeGraph = { version: '1.0.0', robot: { id: 'test', name: 'Test' }, cores: {} };
    const set: NodeSetDefinition = { package: { id: 'island/mujoco-sim', version: '0.1.1' }, id: 'hardware', parameters: { scene: { type: 'string', default: 'demo' } }, nodes: [] };
    vi.mocked(request).mockResolvedValueOnce({ sets: [set] }).mockResolvedValueOnce({ graph, added_paths: [] });
    expect(await getNodeSets()).toEqual({ sets: [set] });
    expect(await previewNodeSet(set, graph, { scene: 'demo' })).toEqual({ graph, added_paths: [] });
    expect(vi.mocked(request).mock.calls).toEqual([
      ['/catalog/node-sets'],
      ['/catalog/node-sets/preview', 'POST', { package: set.package, set_id: set.id, graph, values: { scene: 'demo' } }],
    ]);
  });
});

describe('generic package acquisition API', () => {
  it('reads installed receipts for every package kind', async () => {
    vi.mocked(request).mockClear();
    const installed = [{ schema_version: 1, id: 'island/mujoco-sim', version: '0.1.1', source_commit: null, asset_sha256: null, local_link: '/opt/mujoco' }];
    vi.mocked(request).mockResolvedValueOnce({ installed });
    expect(await getInstalledPackages()).toEqual({ installed });
    expect(vi.mocked(request).mock.calls).toEqual([['/packages']]);
  });
  it('accepts explicit package identity and optional local manifest path', async () => {
    vi.mocked(request).mockClear();
    vi.mocked(request).mockResolvedValue(undefined);
    await installBackend('island/mujoco-sim', '0.1.1');
    await linkBackend('island/mujoco-sim', '0.1.1', '/opt/mujoco', '/opt/mujoco/release.json');
    await linkBackend('island/mujoco-sim', '0.1.1', '/opt/mujoco');
    expect(vi.mocked(request).mock.calls).toEqual([
      ['/packages/install', 'POST', { id: 'island/mujoco-sim', version: '0.1.1' }],
      ['/packages/link', 'POST', { id: 'island/mujoco-sim', version: '0.1.1', path: '/opt/mujoco', manifest: '/opt/mujoco/release.json' }],
      ['/packages/link', 'POST', { id: 'island/mujoco-sim', version: '0.1.1', path: '/opt/mujoco' }],
    ]);
  });
});

describe('readiness and local bundle preview API', () => {
  it('reads configured core readiness and previews bundle identity before linking', async () => {
    const graph: NodeGraph = { version: '1.0.0', robot: { id: 'test', name: 'Test' }, cores: {} };
    vi.mocked(request).mockClear().mockResolvedValueOnce({ cores: [] }).mockResolvedValueOnce({ id: 'island/mujoco-sim', version: '0.1.1', ready: true, issues: [] }).mockResolvedValueOnce({ adapter_families: [], primitive_bindings: [], targets: [] });
    await getCoreReadiness();
    await previewBackendLink('/opt/mujoco', '/opt/mujoco/release.json');
    await getNodeOptions(graph);
    expect(vi.mocked(request).mock.calls).toEqual([
      ['/cores/readiness'],
      ['/packages/preview-link', 'POST', { path: '/opt/mujoco', manifest: '/opt/mujoco/release.json' }],
      ['/catalog/node-options', 'POST', { schema_version: '0.1.0', graph }],
    ]);
  });
  it('retains P03 setting groups and draft revision metadata', async () => {
    const graph: NodeGraph = { version: '0.1.0', robot: { id: 'test', name: 'Test' }, cores: {} };
    const reply = { adapter_families: [], primitive_bindings: [], targets: [], graph_revision: null, read_only: false, setting_options: [{ node_path: 'eos/process/mujoco/simulator', setting: 'scene', selected: 'pick-place', options: [{ value: 'pick-place', label: 'Pick and place', description: 'Scene', available: false, reason_code: 'package_changed', reason: 'Relink release' }] }] };
    vi.mocked(request).mockClear().mockResolvedValueOnce(reply);
    expect(await getNodeOptions(graph)).toEqual(reply);
    expect(vi.mocked(request).mock.calls).toEqual([['/catalog/node-options', 'POST', { schema_version: '0.1.0', graph }]]);
  });
});
