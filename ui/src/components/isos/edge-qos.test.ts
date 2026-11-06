import { describe, expect, it } from 'vitest';
import type { NodeGraph, ValidatedEdge } from '@/services/isos/node-graph';
import { edgeSource, parseFrequency, parsePriority, sourcePointer, updateEdgeQos, validatedSource } from './edge-qos';

const target = 'eos/interface/gate/front';
const graph: NodeGraph = {
  version: '0.1.0', robot: { id: 'r', name: 'r' }, future: { keep: true },
  cores: { eos: { layers: { interface: { groups: { gate: { nodes: {
    front: { id: 'front', label: 'front', description: '', node_type: 'eos.interface.camera_gate@v0.1', inputs: {
      frame: [
        { node: 'eos/hardware/camera/a', output: 'frame', extension: 'a' },
        { node: 'eos/hardware/camera/b', output: 'frame', extension: 'b' },
      ],
      single: { node: 'eos/hardware/camera/a', output: 'frame', extension: 'single' },
    } },
  } } } } } } },
};
const admitted: ValidatedEdge[] = [0, 1].map((index) => ({ path: sourcePointer(target, 'frame', index, true), source: `eos/hardware/camera/${index}`, target, input: 'frame', route_class: 'eos_local', fixed: false, routed: true, qos_eligible: true, priority: 0, data_type: 'camera_frame', transport: 'in_process' }));

describe('selected edge QoS', () => {
  it('matches exact IsOS source pointers for object and array shapes', () => {
    expect(sourcePointer(target, 'frame', 1, true)).toBe('/cores/eos/layers/interface/groups/gate/nodes/front/inputs/frame/1');
    expect(sourcePointer(target, 'single', 0, false)).toBe('/cores/eos/layers/interface/groups/gate/nodes/front/inputs/single');
    expect(validatedSource(admitted, target, 'frame', 1, true)).toBe(admitted[1]);
    expect(validatedSource(admitted, target, 'single', 0, false)).toBeUndefined();
  });
  it('edits one source and preserves siblings, unknown fields, and legacy omissions', () => {
    const next = updateEdgeQos(graph, target, 'frame', 1, { priority: -20, frequency_hz: 15 });
    expect(edgeSource(next, target, 'frame', 1)?.source).toMatchObject({ priority: -20, frequency_hz: 15, extension: 'b' });
    expect(edgeSource(next, target, 'frame', 0)?.source).toEqual(edgeSource(graph, target, 'frame', 0)?.source);
    expect(edgeSource(graph, target, 'frame', 1)?.source.priority).toBeUndefined();
    expect((next as Record<string, unknown>).future).toEqual({ keep: true });
    const cleared = updateEdgeQos(next, target, 'frame', 1, { frequency_hz: null });
    expect(edgeSource(cleared, target, 'frame', 1)?.source.frequency_hz).toBeUndefined();
    expect(Array.isArray(cleared.cores.eos.layers.interface.groups.gate.nodes.front.inputs?.frame)).toBe(true);
    expect(Array.isArray(updateEdgeQos(graph, target, 'single', 0, { priority: 19 }).cores.eos.layers.interface.groups.gate.nodes.front.inputs?.single)).toBe(false);
  });
  it('validates nice bounds and optional finite positive cap', () => {
    expect([-20, 0, 19].map((value) => parsePriority(String(value)))).toEqual([-20, 0, 19]);
    for (const value of ['-21', '20', '1.5', '', 'Infinity']) expect(parsePriority(value)).toBeNull();
    expect(parseFrequency('')).toBeUndefined();
    expect(parseFrequency('0.01')).toBe(0.01);
    for (const value of ['0', '-1', 'Infinity', 'NaN']) expect(parseFrequency(value)).toBeNull();
  });
});
