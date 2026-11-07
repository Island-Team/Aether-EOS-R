import { describe, expect, it } from 'vitest';
import type { NodeGraph, NodeTypeDefinition } from '@/services/isos/node-graph';
import { addNode, bindPrimitiveTarget, canonicalNodePath, connectNodes, disconnectInput, graphNodes, inputSources, nextNodeId, nodePath, removeNode, removedNodePaths, renameNode, updateNode } from './node-graph-edits';
import { withControlTarget, withSkillSteps } from './control-node-config';

const dispatcher: NodeTypeDefinition = { id: 'eos.dispatcher.control@v1.0', core: 'eos', layer: 'dispatcher', group: 'control', kind: 'software', lifecycle: 'internal', capabilities: ['admission'], settings: [], inputs: [], outputs: [{ name: 'base_command', data_type: 'base_movement', required: false }] };
const base: NodeTypeDefinition = { id: 'eos.hardware.base@v1.0', core: 'eos', layer: 'hardware', group: 'base', kind: 'base', lifecycle: 'adapter', capabilities: ['body_velocity'], settings: [], inputs: [{ name: 'command', data_type: 'base_movement', required: true }], outputs: [{ name: 'status', data_type: 'status', required: false }] };
const camera: NodeTypeDefinition = { id: 'eos.hardware.camera@v1.0', core: 'eos', layer: 'hardware', group: 'camera', kind: 'camera', lifecycle: 'adapter', capabilities: ['rgb8', 'aligned_depth_u16'], settings: [], inputs: [], outputs: [{ name: 'frame', data_type: 'camera_frame', required: false }] };
const empty = (): NodeGraph => ({ version: '1.0.0', robot: { id: 'robot', name: 'Robot' }, cores: {} });

describe('v1 node graph edits', () => {
  it('detects a package preview that removes an existing node', () => {
    const graph = addNode(empty(), camera, 'front');
    expect(removedNodePaths(graph, addNode(graph, camera, 'rear'))).toEqual([]);
    expect(removedNodePaths(graph, empty())).toEqual(['eos/hardware/camera/front']);
  });
  it('uses the final node type and next free index as the default ID', () => {
    const first = nextNodeId(empty(), camera);
    expect(first).toBe('camera_1');
    const graph = addNode(empty(), camera, first);
    expect(nextNodeId(graph, camera)).toBe('camera_2');
    expect(graphNodes(addNode(graph, camera, 'camera_2'), [camera])).toHaveLength(2);
  });
  it('places an instance under its registered core, layer and group', () => {
    const graph = addNode(empty(), camera, 'front');
    const entry = graphNodes(graph, [camera])[0];
    expect(entry.path).toBe('eos/hardware/camera/front');
    expect(canonicalNodePath('cores/eos/layers/hardware/groups/camera/nodes/front')).toBe(entry.path);
    expect(entry.node.node_type).toBe('eos.hardware.camera@v1.0');
    expect(entry.node.capabilities).toEqual(['rgb8']);
    expect(entry.node.movement).toBeUndefined();
  });
  it('resolves duplicate type IDs by the complete catalog placement', () => {
    const other = { ...camera, group: 'inspection', description: 'Other placement' };
    const graph = addNode(addNode(empty(), camera, 'front'), other, 'second');
    const entries = graphNodes(graph, [other, camera]);
    expect(entries.find((entry) => entry.path === 'eos/hardware/camera/front')?.definition).toBe(camera);
    expect(entries.find((entry) => entry.path === 'eos/hardware/inspection/second')?.definition).toBe(other);
    expect(graphNodes(graph, [camera])[1].definition).toBeUndefined();
  });
  it('does not invent hardware values for a new target type', () => {
    const target = { ...base, id: 'eos.hardware.base@v0.1' };
    const node = graphNodes(addNode(empty(), target, 'new-base'), [target])[0].node;
    expect(node.adapter).toBeUndefined();
    expect(node.movement).toBeUndefined();
    expect(node.observation).toBeUndefined();
    expect(node.settings).toBeUndefined();
    expect(node.capabilities).toBeUndefined();
  });

  it('connects only matching typed ports and admits one source per input', () => {
    const graph = addNode(addNode(empty(), dispatcher, 'control'), base, 'drive');
    const source = nodePath(dispatcher, 'control');
    const target = nodePath(base, 'drive');
    expect(() => connectNodes(graph, [dispatcher, base], source, 'missing', target, 'command')).toThrow('same data type');
    const connected = connectNodes(graph, [dispatcher, base], source, 'base_command', target, 'command');
    expect(graphNodes(connected, [dispatcher, base]).find((entry) => entry.path === target)?.node.inputs?.command).toEqual({ node: source, output: 'base_command' });
    expect(() => connectNodes(connected, [dispatcher, base], source, 'base_command', target, 'command')).toThrow('already has a provider');
    expect(graphNodes(disconnectInput(connected, target, 'command'), [dispatcher, base]).find((entry) => entry.path === target)?.node.inputs).toEqual({});
  });

  it('rejects transport and delivery mismatches before changing the draft', () => {
    const producer = { ...dispatcher, outputs: [{ name: 'state', data_type: 'key_state', required: false, transport: 'http', delivery_mode: 'event' }] };
    const consumer = { ...base, inputs: [{ name: 'state', data_type: 'key_state', required: true, transport: 'unix_socket', delivery_mode: 'event' }] };
    const graph = addNode(addNode(empty(), producer, 'keyboard'), consumer, 'gateway');
    expect(() => connectNodes(graph, [producer, consumer], nodePath(producer, 'keyboard'), 'state', nodePath(consumer, 'gateway'), 'state')).toThrow('same transport');
    const sameTransport = { ...consumer, inputs: [{ ...consumer.inputs[0], transport: 'http', delivery_mode: 'request' }] };
    expect(() => connectNodes(graph, [producer, sameTransport], nodePath(producer, 'keyboard'), 'state', nodePath(consumer, 'gateway'), 'state')).toThrow('same delivery');
    expect(graphNodes(graph, []).find((entry) => entry.path === nodePath(consumer, 'gateway'))?.node.inputs).toEqual({});
  });

  it('rewrites canonical references on rename and removes inbound connections on delete', () => {
    const initial = addNode(addNode(empty(), dispatcher, 'control'), base, 'drive');
    const source = nodePath(dispatcher, 'control');
    const target = nodePath(base, 'drive');
    const connected = connectNodes(initial, [dispatcher, base], source, 'base_command', target, 'command');
    const renamed = renameNode(connected, source, 'admission');
    const newSource = nodePath(dispatcher, 'admission');
    expect(graphNodes(renamed, [dispatcher, base]).find((entry) => entry.path === target)?.node.inputs?.command && inputSources(graphNodes(renamed, [dispatcher, base]).find((entry) => entry.path === target)!.node.inputs!.command)[0].node).toBe(newSource);
    const removed = removeNode(renamed, newSource);
    expect(graphNodes(removed, [dispatcher, base]).find((entry) => entry.path === target)?.node.inputs).toEqual({});
    expect(connected).toEqual(connectNodes(initial, [dispatcher, base], source, 'base_command', target, 'command'));
  });
  it('supports multiple primitive bindings and rewires one target at a time', () => {
    const binding: NodeTypeDefinition = { ...dispatcher, id: 'eos.dispatcher.primitive_base@v1.0', group: 'base', outputs: [{ name: 'command', data_type: 'base_movement', required: false }] };
    const multiBase: NodeTypeDefinition = { ...base, inputs: [{ name: 'command', data_type: 'base_movement', required: true, cardinality: 'many' }] };
    let graph = addNode(addNode(addNode(addNode(empty(), binding, 'first'), binding, 'second'), multiBase, 'drive'), multiBase, 'other');
    const first = nodePath(binding, 'first'), second = nodePath(binding, 'second'), drive = nodePath(base, 'drive'), other = nodePath(base, 'other');
    graph = bindPrimitiveTarget(graph, [binding, multiBase], first, drive);
    graph = bindPrimitiveTarget(graph, [binding, multiBase], second, drive);
    expect(inputSources(graphNodes(graph, []).find((entry) => entry.path === drive)!.node.inputs!.command)).toHaveLength(2);
    graph = bindPrimitiveTarget(graph, [binding, multiBase], first, other);
    expect(inputSources(graphNodes(graph, []).find((entry) => entry.path === drive)!.node.inputs!.command)).toEqual([{ node: second, output: 'command' }]);
    expect(inputSources(graphNodes(graph, []).find((entry) => entry.path === other)!.node.inputs!.command)).toEqual([{ node: first, output: 'command' }]);
  });
  it('rewrites control targets and ordered skill steps when nodes are renamed', () => {
    const primitive: NodeTypeDefinition = { ...dispatcher, id: 'eos.control.primitive@v1.0', layer: 'control', group: 'primitive' };
    const skill: NodeTypeDefinition = { ...dispatcher, id: 'eos.control.skill@v1.0', layer: 'control', group: 'skill' };
    let graph = addNode(addNode(addNode(empty(), base, 'drive'), primitive, 'move'), skill, 'sequence');
    const basePath = nodePath(base, 'drive'); const primitivePath = nodePath(primitive, 'move'); const skillPath = nodePath(skill, 'sequence');
    graph = updateNode(graph, primitivePath, withControlTarget(graphNodes(graph, []).find((item) => item.path === primitivePath)!.node, basePath, graphNodes(graph, []).find((item) => item.path === basePath)!.node));
    graph = updateNode(graph, skillPath, withSkillSteps(graphNodes(graph, []).find((item) => item.path === skillPath)!.node, [primitivePath]));
    graph = renameNode(graph, basePath, 'new_drive');
    expect(graphNodes(graph, []).find((item) => item.path === primitivePath)?.node.settings?.target).toBe(nodePath(base, 'new_drive'));
    graph = renameNode(graph, primitivePath, 'new_move');
    expect(graphNodes(graph, []).find((item) => item.path === skillPath)?.node.settings?.steps).toEqual([nodePath(primitive, 'new_move')]);
  });
});
