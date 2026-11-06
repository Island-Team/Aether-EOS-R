import { describe, expect, it } from 'vitest';
import type { NodeGraph, NodeInstance } from '../isos/node-graph';
import { resolvePrimitiveBindings, typedPrimitiveRequest } from './primitive-command';
import { defaultMovement } from '../../components/isos/eos-control-model';
const revision = 'a'.repeat(64);
function fixture(): NodeGraph {
  const base: NodeInstance = { id: 'drive', node_type: 'eos.hardware.base@v0.1', label: 'Drive', description: 'Base', inputs: { command: [{ node: 'eos/dispatcher/base/velocity', output: 'command' }] }, movement: { modes: ['body_velocity'], limits: { max_linear_m_s: 1, max_angular_rad_s: 1, max_velocity_lease_ms: 300 } } };
  const arm: NodeInstance = { id: 'arm', node_type: 'eos.hardware.manipulator@v0.1', label: 'Arm', description: 'Manipulator', inputs: { command: [{ node: 'eos/dispatcher/manipulator/movej', output: 'command' }] }, joint_names: ['j1'], movement: { modes: ['joint_positions'], limits: { max_joint_position_rad: 1, max_joint_speed_rad_s: 1 } } };
  return { version: '0.1.0', robot: { id: 'r', name: 'R' }, cores: { eos: { layers: {
    dispatcher: { groups: { base: { nodes: { velocity: { id: 'velocity', node_type: 'eos.dispatcher.primitive_base@v0.1', label: 'Velocity', description: 'Drive', settings: { primitive_name: 'base.velocity', primitive_version: '1.0.0', movement_type: 'body_velocity' } } } }, manipulator: { nodes: { movej: { id: 'movej', node_type: 'eos.dispatcher.primitive_manipulator@v0.1', label: 'MoveJ', description: 'Arm', settings: { primitive_name: 'arm.movej', primitive_version: '1.0.0', movement_type: 'joint_positions' } } } } } },
    hardware: { groups: { base: { nodes: { drive: base } }, manipulator: { nodes: { arm } } } },
  } } } };
}
describe('active primitive bindings', () => {
  it('resolves each exact base and arm target and builds typed envelopes', () => {
    const bindings = resolvePrimitiveBindings(fixture());
    expect(bindings.map((item) => item.primitiveName)).toEqual(['base.velocity', 'arm.movej']);
    const arm = bindings[1];
    const request = typedPrimitiveRequest(arm, revision, 'ui-1', 1, 100, defaultMovement(arm.mode, arm.target));
    expect(request.op).toBe('primitive_typed');
    expect(request.header).toMatchObject({ data_type_id: 'manipulator_movement', source_path: arm.path, target_path: arm.targetPath, active_revision: revision });
    expect(() => typedPrimitiveRequest(arm, revision, 'ui-1', 1, 100, defaultMovement(bindings[0].mode, bindings[0].target))).toThrow();
  });
  it('refuses ambiguous, missing and legacy bindings', () => {
    const graph = fixture();
    graph.cores.eos.layers.hardware.groups.base.nodes.second = { ...graph.cores.eos.layers.hardware.groups.base.nodes.drive, id: 'second' };
    expect(resolvePrimitiveBindings(graph).map((item) => item.primitiveName)).toEqual(['arm.movej']);
    graph.cores.eos.layers.dispatcher.groups.manipulator.nodes.movej.node_type = 'eos.dispatcher.primitive_manipulator@v1.0';
    expect(resolvePrimitiveBindings(graph)).toEqual([]);
  });
  it('refuses a target command input with another source', () => {
    const graph = fixture();
    graph.cores.eos.layers.hardware.groups.base.nodes.drive.inputs!.command = [
      { node: 'eos/dispatcher/base/velocity', output: 'command' },
      { node: 'eos/dispatcher/base/other', output: 'command' },
    ];
    expect(resolvePrimitiveBindings(graph).map((item) => item.primitiveName)).toEqual(['arm.movej']);
  });
});
