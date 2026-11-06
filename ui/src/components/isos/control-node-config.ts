import type { NodeInstance } from '@/services/isos/node-graph';
import { canonicalNodePath } from './node-graph-edits';

export const DISPATCHER_PATH = 'eos/dispatcher/control/command';
export const ISOS_DISPATCHER_PATH = 'isos/eos_dispatcher/control/command';

/** Keep the visible picker, typed data edge, and registry references in sync. */
export function withControlTarget(node: NodeInstance, targetPath: string, target: NodeInstance): Partial<NodeInstance> {
  const dispatcherPath = node.node_type.startsWith('isos.') ? ISOS_DISPATCHER_PATH : DISPATCHER_PATH;
  const mode = String(node.settings?.movement_type || '');
  const modes = target.movement?.modes || [];
  const isPrimitive = node.node_type === 'eos.control.primitive@v1.0' || node.node_type === 'isos.eos_control.primitive@v0.2';
  const observationInput = target.node_type === 'eos.hardware.base@v1.0' || target.node_type === 'isos.eos_hardware.base@v0.2' ? 'base_observation' : 'manipulator_observation';
  return {
    settings: { ...node.settings, target: canonicalNodePath(targetPath), ...(isPrimitive ? { movement_type: modes.includes(mode) ? mode : modes[0] || '' } : {}) },
    references: [canonicalNodePath(targetPath), dispatcherPath],
    inputs: {
      status: { node: dispatcherPath, output: 'status' },
      [observationInput]: { node: canonicalNodePath(targetPath), output: 'observation' },
    },
  };
}

/** Skill step order is meaningful; references are a de-duplicated dependency set. */
export function withSkillSteps(node: NodeInstance, steps: string[]): Partial<NodeInstance> {
  const dispatcherPath = node.node_type.startsWith('isos.') ? ISOS_DISPATCHER_PATH : DISPATCHER_PATH;
  return {
    settings: { ...node.settings, steps: steps.map(canonicalNodePath) },
    references: [dispatcherPath, ...new Set(steps.map(canonicalNodePath))],
    inputs: { status: { node: dispatcherPath, output: 'status' } },
  };
}
