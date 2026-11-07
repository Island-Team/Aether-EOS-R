import { describe, expect, it } from 'vitest';
import type { NodeGraph } from '@/services/isos/node-graph';
import { applyNodeSelection, graphLayoutKey, graphMirrorKey, parseNodeMirrors, parseNodePositions, removeSelectedNodes } from './graph-canvas-state';

const graph = (id: string): NodeGraph => ({ version: '1.0.0', robot: { id, name: id }, cores: {} });

describe('graph canvas state', () => {
  it('separates positions by robot identity, independent of graph revision', () => {
    expect(graphLayoutKey(graph('alpha'))).not.toBe(graphLayoutKey(graph('beta')));
    expect(graphLayoutKey(graph('alpha'))).toBe('isosui-v2-layout:alpha');
  });

  it('ignores corrupted or non-finite saved coordinates', () => {
    expect(parseNodePositions('{broken')).toEqual({});
    expect(parseNodePositions('{"good":{"x":2,"y":3},"bad":{"x":"2","y":3},"other":{"x":null,"y":0}}')).toEqual({ good: { x: 2, y: 3 } });
  });

  it('keeps mirrored ports only in local visual layout', () => {
    expect(graphMirrorKey(graph('alpha'))).toBe('isosui-v2-layout:alpha:mirrors');
    expect(parseNodeMirrors('{broken')).toEqual({});
    expect(parseNodeMirrors('{"a":true,"b":false,"c":"yes"}')).toEqual({ a: true });
    expect(parseNodeMirrors('{"cores/eos/layers/hardware/groups/camera/nodes/front":true}')).toEqual({ 'eos/hardware/camera/front': true });
    expect(parseNodePositions('{"cores/eos/layers/hardware/groups/camera/nodes/front":{"x":2,"y":3}}')).toEqual({ 'eos/hardware/camera/front': { x: 2, y: 3 } });
  });

  it('adds and removes nodes from rectangle selection without changing positions', () => {
    expect(applyNodeSelection(['first'], [{ id: 'second', type: 'select', selected: true }, { id: 'first', type: 'select', selected: false }])).toEqual(['second']);
    expect(applyNodeSelection(['second'], [{ id: 'second', type: 'position', position: { x: 4, y: 8 }, dragging: false }])).toEqual(['second']);
  });

  it('removes several selected nodes in one graph edit', () => {
    const current: NodeGraph = { ...graph('alpha'), cores: { eos: { layers: { hardware: { groups: { camera: { nodes: {
      first: { id: 'first', node_type: 'eos.hardware.camera@v1.0', label: 'First', description: '', settings: {} },
      second: { id: 'second', node_type: 'eos.hardware.camera@v1.0', label: 'Second', description: '', settings: {} },
    } } } } } } } };
    const paths = ['cores/eos/layers/hardware/groups/camera/nodes/first', 'cores/eos/layers/hardware/groups/camera/nodes/second'];
    const result = removeSelectedNodes(current, paths);
    expect(result.cores).toEqual({});
  });
});
