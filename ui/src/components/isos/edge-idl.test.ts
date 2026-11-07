import { describe, expect, it } from 'vitest';
import { edgeContracts, edgeInputDiagnostics } from './edge-idl';
import type { DataTypeDefinition } from '@/services/isos/node-graph';
import type { GraphNode } from './node-graph-edits';

const descriptors: DataTypeDefinition[] = ['camera_frame', 'vision_frame'].map((id) => ({ id, version: '2.0.0', purpose: id, media_type: 'application/json', codec: 'json', schema: { type: 'object' }, max_payload_bytes: 1000 }));
const entries: GraphNode[] = [
  { path: 'source', node: { id: 'source', node_type: 'camera', label: '', description: '' }, definition: { id: 'camera', core: 'eos', layer: 'hardware', group: 'mujoco', kind: 'camera', lifecycle: 'adapter', capabilities: [], settings: [], inputs: [], outputs: [{ name: 'frame', data_type: 'camera_frame', required: true }] } },
  { path: 'target', node: { id: 'target', node_type: 'gate', label: '', description: '' }, definition: { id: 'gate', core: 'eos', layer: 'interface', group: 'gate', kind: 'software', lifecycle: 'internal', capabilities: [], settings: [], inputs: [{ name: 'frame', data_type: 'vision_frame', required: true }], outputs: [] } },
];

describe('edge IDL resolution', () => {
  it('shows both declared payload types when endpoints differ', () => {
    expect(edgeContracts(entries, { source: 'source', target: 'target', sourceHandle: 'out:frame', targetHandle: 'in:frame' }, descriptors)).toMatchObject({ sourceType: 'camera_frame', targetType: 'vision_frame', sourceIdl: descriptors[0], targetIdl: descriptors[1] });
  });
  it('keeps missing endpoint descriptors explicit', () => {
    expect(edgeContracts(entries, { source: 'source', target: 'target', sourceHandle: 'out:frame', targetHandle: 'in:missing' }, descriptors)).toMatchObject({ sourceType: 'camera_frame', targetType: '' });
  });
});

describe('edge input diagnostics', () => {
  it('selects only diagnostics for the destination input, including slash and dot paths', () => {
    const target = 'cores/eos/layers/interface/groups/gate/nodes/front';
    const diagnostics = [
      { path: `${target}/inputs/frame`, code: 'type_mismatch', message: 'Frame type mismatch' },
      { path: 'cores.eos.layers.interface.groups.gate.nodes.front.inputs.other', message: 'Other input' },
      { path: 'cores.eos.layers.interface.groups.gate.nodes.other.inputs.frame', message: 'Other node' },
    ];
    expect(edgeInputDiagnostics({ source: 'camera', target, targetHandle: 'in:frame' }, diagnostics)).toEqual([diagnostics[0]]);
    expect(edgeInputDiagnostics({ source: 'camera', target, targetHandle: null }, diagnostics)).toEqual([]);
  });
  it('keeps two source indices and similarly named ports distinct', () => {
    const target = 'eos/interface/gate/front';
    const pointer = '/cores/eos/layers/interface/groups/gate/nodes/front';
    const diagnostics = [
      { path: `${pointer}/inputs/frame/0`, message: 'First source rejected' },
      { path: `${pointer}/inputs/frame/1`, message: 'Second source rejected' },
      { path: `${pointer}/inputs/frame_extra/1`, message: 'Other port' },
      { path: `${pointer}/inputs/frame`, message: 'Whole port' },
      { path: `${pointer}2/inputs/frame/1`, message: 'Other node' },
    ];
    expect(edgeInputDiagnostics({ source: 'camera_a', target, targetHandle: 'in:frame', inputIndex: 0, inputCount: 2 }, diagnostics)).toEqual([diagnostics[0]]);
    expect(edgeInputDiagnostics({ source: 'camera_b', target, targetHandle: 'in:frame', inputIndex: 1, inputCount: 2 }, diagnostics)).toEqual([diagnostics[1]]);
  });
});
