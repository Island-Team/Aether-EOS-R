import type { NodeGraph, NodeInstance, ProcessStatus } from '../isos/node-graph';
import { publicControl, decodeDispatchResult, ResultClientError, validCommandId, validRevision, type DispatchResult } from './primitive-result';
import { canonicalNodePath } from '../../components/isos/node-graph-edits';
import { movementTypes, movementValid, type Movement, type MovementType } from '../../components/isos/eos-control-model';
import { eosGraphNodes } from '../../components/isos/eos-control-model';

export interface BaseBinding { path: string; targetPath: string; target: NodeInstance; version: 'v0.1' | 'v1.0' }
export interface BodyVelocity { type: 'body_velocity'; x_m_s: number; y_m_s: number; yaw_rad_s: number; valid_for_ms: number }
export interface PrimitiveBinding { path: string; node: NodeInstance; targetPath: string; target: NodeInstance; mode: MovementType; primitiveName: string; primitiveVersion: string; dataTypeId: 'base_movement' | 'manipulator_movement' }
const PRIMITIVES: Record<MovementType, string> = { body_velocity: 'base.velocity', relative_pose: 'base.navigate', joint_positions: 'arm.movej', end_effector_pose: 'arm.movel' };
/** A binding is executable only when the saved graph resolves exactly one matching command edge. */
export function resolvePrimitiveBindings(graph: NodeGraph): PrimitiveBinding[] {
  const entries = eosGraphNodes(graph);
  return entries.flatMap(({ path, node }) => {
    const match = /^(?:eos\.dispatcher\.primitive_(base|manipulator)@v0\.1|isos\.eos_dispatcher\.primitive_(base|manipulator)@v0\.2)$/.exec(node.node_type);
    const kind = match?.[1] || match?.[2];
    if (!kind || !path.startsWith(`${graph.version === '0.2.0' ? 'isos/eos_dispatcher' : 'eos/dispatcher'}/${kind}/`)) return [];
    const mode = node.settings?.movement_type;
    if (typeof mode !== 'string' || !Object.prototype.hasOwnProperty.call(PRIMITIVES, mode) || node.settings?.primitive_name !== PRIMITIVES[mode as MovementType] || node.settings?.primitive_version !== '1.0.0') return [];
    if ((kind === 'base') !== (mode === 'body_velocity' || mode === 'relative_pose')) return [];
    const targets = entries.filter(({ path: candidatePath, node: candidate }) => {
      const sources = Array.isArray(candidate.inputs?.command) ? candidate.inputs.command : candidate.inputs?.command ? [candidate.inputs.command] : [];
      const prefix = graph.version === '0.2.0' ? `isos/eos_hardware/${kind}/` : `eos/hardware/${kind}/`;
      const type = graph.version === '0.2.0' ? `isos.eos_hardware.${kind}@v0.2` : `eos.hardware.${kind}@v0.1`;
      return candidatePath.startsWith(prefix) && candidate.node_type === type && sources.length === 1 && canonicalNodePath(sources[0].node) === path && sources[0].output === 'command';
    });
    if (targets.length !== 1 || !movementTypes(targets[0].node).includes(mode as MovementType)) return [];
    return [{ path, node, targetPath: targets[0].path, target: targets[0].node, mode: mode as MovementType,
      primitiveName: PRIMITIVES[mode as MovementType], primitiveVersion: '1.0.0', dataTypeId: kind === 'base' ? 'base_movement' : 'manipulator_movement' }];
  });
}
export function typedPrimitiveRequest(binding: PrimitiveBinding, activeRevision: string, commandId: string, sequence: number, eosTime: number, payload: Movement): Record<string, unknown> {
  if (!validRevision(activeRevision) || !validCommandId(commandId) || !Number.isSafeInteger(sequence) || sequence < 1 ||
      !Number.isSafeInteger(eosTime) || eosTime < 0 || eosTime > Number.MAX_SAFE_INTEGER - 4_000_000_000 ||
      payload.type !== binding.mode || !movementValid(payload, binding.target)) throw new ResultClientError('validation', 'Invalid binding, payload, revision or EOS deadline.');
  const bytes = new TextEncoder().encode(JSON.stringify(payload)).length;
  if (bytes < 1 || bytes > 16384) throw new ResultClientError('validation', 'Invalid payload length.');
  return { op: 'primitive_typed', header: { data_type_id: binding.dataTypeId, data_type_version: '1.0', primitive_name: binding.primitiveName,
    primitive_version: binding.primitiveVersion, source_path: binding.path, target_path: binding.targetPath, active_revision: activeRevision,
    command_id: commandId, sequence, deadline_monotonic_ns: eosTime + 4_000_000_000, payload_length_bytes: bytes, codec: 'json_v1' }, payload };
}
export async function sendPrimitive(binding: PrimitiveBinding, activeRevision: string, commandId: string, sequence: number, eosTime: number, payload: Movement, signal?: AbortSignal, onAttempt?: () => void): Promise<DispatchResult> {
  const request = typedPrimitiveRequest(binding, activeRevision, commandId, sequence, eosTime, payload);
  onAttempt?.();
  return decodeDispatchResult(await publicControl(request, signal), commandId);
}
export function resolveBaseBindings(graph: NodeGraph): BaseBinding[] {
  const entries = eosGraphNodes(graph);
  return entries.flatMap(({ path, node }) => {
    const match = /^(?:eos\.dispatcher\.primitive_base@(v0\.1|v1\.0)|isos\.eos_dispatcher\.primitive_base@(v0\.2))$/.exec(node.node_type);
    if (!match || node.settings?.primitive_name !== 'base.velocity' || node.settings?.primitive_version !== '1.0.0' || node.settings?.movement_type !== 'body_velocity') return [];
    const targets = entries.filter(({ node: candidate }) => /^(?:eos\.hardware\.base@(v0\.1|v1\.0)|isos\.eos_hardware\.base@v0\.2)$/.test(candidate.node_type) &&
      (Array.isArray(candidate.inputs?.command) ? candidate.inputs.command : candidate.inputs?.command ? [candidate.inputs.command] : [])
        .some((source) => source.node === path && source.output === 'command'));
    if (targets.length !== 1) return [];
    const target = targets[0];
    return [{ path, targetPath: target.path, target: target.node, version: (match[1] || match[2]) as BaseBinding['version'] }];
  });
}
export function sendFence(graphRevision: string, status: ProcessStatus | null, dirty: boolean, readOnly: boolean): boolean {
  return !dirty && !readOnly && validRevision(graphRevision) && status?.saved_revision === graphRevision &&
    status.active_revision === graphRevision && status.state === 'running' && status.run_id_state === 'known' && Boolean(status.run_id);
}
export function validBodyVelocity(value: BodyVelocity, target: NodeInstance): boolean {
  const limits = target.movement?.limits;
  return Boolean(target.movement?.modes.includes('body_velocity') && limits &&
    Number.isFinite(limits.max_linear_m_s) && limits.max_linear_m_s >= 0 && Number.isFinite(limits.max_angular_rad_s) && limits.max_angular_rad_s >= 0 &&
    Number.isSafeInteger(limits.max_velocity_lease_ms) && limits.max_velocity_lease_ms > 0 &&
    [value.x_m_s, value.y_m_s].every((number) => Number.isFinite(number) && Math.abs(number) <= limits.max_linear_m_s) &&
    Number.isFinite(value.yaw_rad_s) && Math.abs(value.yaw_rad_s) <= limits.max_angular_rad_s &&
    Number.isSafeInteger(value.valid_for_ms) && value.valid_for_ms > 0 && value.valid_for_ms <= limits.max_velocity_lease_ms);
}
export async function eosMonotonicTime(signal?: AbortSignal): Promise<number> {
  const reply = await publicControl({ op: 'time' }, signal) as { ok?: unknown; monotonic_ns?: unknown };
  if (reply?.ok !== true || !Number.isSafeInteger(reply.monotonic_ns) || (reply.monotonic_ns as number) < 0) throw new ResultClientError('protocol', 'Unsafe EOS monotonic time.');
  return reply.monotonic_ns as number;
}
export function typedBaseRequest(binding: BaseBinding, activeRevision: string, commandId: string, sequence: number, eosTime: number, payload: BodyVelocity): Record<string, unknown> {
  if (!validRevision(activeRevision) || !validCommandId(commandId) || !Number.isSafeInteger(sequence) || sequence < 1 ||
      !Number.isSafeInteger(eosTime) || eosTime < 0 || eosTime > Number.MAX_SAFE_INTEGER - 4_000_000_000 || !validBodyVelocity(payload, binding.target)) {
    throw new ResultClientError('validation', 'Invalid binding, payload, revision or EOS deadline.');
  }
  const bytes = new TextEncoder().encode(JSON.stringify(payload)).length;
  if (bytes < 1 || bytes > 16384) throw new ResultClientError('validation', 'Invalid payload length.');
  return { op: 'primitive_typed', header: { data_type_id: 'base_movement', data_type_version: '1.0', primitive_name: 'base.velocity',
    primitive_version: '1.0.0', source_path: binding.path, target_path: binding.targetPath, active_revision: activeRevision,
    command_id: commandId, sequence, deadline_monotonic_ns: eosTime + 4_000_000_000, payload_length_bytes: bytes, codec: 'json_v1' }, payload };
}
export async function sendPrimitiveBase(binding: BaseBinding, activeRevision: string, commandId: string, sequence: number, eosTime: number, payload: BodyVelocity, signal?: AbortSignal, onAttempt?: () => void): Promise<DispatchResult> {
  const request = typedBaseRequest(binding, activeRevision, commandId, sequence, eosTime, payload);
  onAttempt?.();
  return decodeDispatchResult(await publicControl(request, signal), commandId);
}
