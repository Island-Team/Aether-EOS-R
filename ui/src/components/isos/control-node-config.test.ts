import { describe, expect, it } from 'vitest';
import { DISPATCHER_PATH, withControlTarget, withSkillSteps } from './control-node-config';
import type { NodeInstance } from '../../services/isos/node-graph';

const primitive: NodeInstance = { id: 'primitive_1', node_type: 'eos.control.primitive@v1.0', label: '', description: '', settings: { movement_type: 'body_velocity' } };
const base: NodeInstance = { id: 'base_1', node_type: 'eos.hardware.base@v1.0', label: '', description: '', movement: { modes: ['body_velocity'], limits: {} } };
const arm: NodeInstance = { id: 'arm_1', node_type: 'eos.hardware.manipulator@v1.0', label: '', description: '', movement: { modes: ['joint_positions'], limits: {} } };
describe('control graph links', () => {
  it('changes typed observation input and mode when the actuator changes', () => {
    const basePath = 'eos/hardware/base/base_1';
    const armPath = 'eos/hardware/manipulator/arm_1';
    expect(withControlTarget(primitive, basePath, base).inputs).toEqual({ status: { node: DISPATCHER_PATH, output: 'status' }, base_observation: { node: basePath, output: 'observation' } });
    const update = withControlTarget(primitive, armPath, arm);
    expect(update.settings?.movement_type).toBe('joint_positions');
    expect(update.references).toEqual([armPath, DISPATCHER_PATH]);
    expect(update.inputs).toEqual({ status: { node: DISPATCHER_PATH, output: 'status' }, manipulator_observation: { node: armPath, output: 'observation' } });
  });
  it('preserves ordered skill steps while de-duplicating references', () => {
    const steps = ['primitive/a', 'primitive/b', 'primitive/a'];
    const update = withSkillSteps({ ...primitive, node_type: 'eos.control.skill@v1.0' }, steps);
    expect(update.settings?.steps).toEqual(steps);
    expect(update.references).toEqual([DISPATCHER_PATH, 'primitive/a', 'primitive/b']);
    expect(update.inputs?.status).toEqual({ node: DISPATCHER_PATH, output: 'status' });
  });
});
