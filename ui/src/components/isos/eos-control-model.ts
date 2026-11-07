import { canonicalNodePath } from './node-graph-edits';
import type { EosDiscoveryNode } from '@/services/eos/control';
import type { NodeGraph, NodeGraphDocument, NodeInstance, ProcessStatus } from '@/services/isos/node-graph';

export interface EosGraphNode { path: string; node: NodeInstance }

export function eosGraphNodes(graph: NodeGraph): EosGraphNode[] {
  const coreId = graph.version === '0.2.0' ? 'isos' : 'eos';
  const core = graph.cores[coreId];
  if (!core) return [];
  return Object.entries(core.layers).filter(([layerId]) => coreId === 'eos' || layerId.startsWith('eos_')).flatMap(([layerId, layer]) =>
    Object.entries(layer.groups).flatMap(([groupId, group]) => Object.entries(group.nodes).map(([id, node]) => ({ path: `${coreId}/${layerId}/${groupId}/${id}`, node }))));
}

export function eosControlNodes(graph: NodeGraph): EosGraphNode[] {
  return eosGraphNodes(graph).filter(({ node }) => /^(eos\.control\.(manual|primitive|skill)@v1\.0|isos\.eos_control\.(manual|primitive|skill)@v0\.2)$/.test(node.node_type));
}

export function discoveredHardware(graph: NodeGraph, discovery: EosDiscoveryNode[]): EosGraphNode[] {
  const paths = new Set(discovery.map((node) => canonicalNodePath(node.path)));
  return eosGraphNodes(graph).filter(({ path, node }) => /^(eos\.hardware\.(camera|base|manipulator)@(v0\.1|v1\.0)|isos\.eos_hardware\.(camera|base|manipulator)@v0\.2)$/.test(node.node_type) && paths.has(path));
}

export function activeGraphIsCurrent(revision: string, active: string | null | undefined): boolean {
  return Boolean(revision && active && revision === active);
}

/** Read-only Vision follows the running EOS revision, even if the saved graph has changed. */
export function visionRuntimeState(document: NodeGraphDocument | null, status: ProcessStatus | null): { active: boolean; graph: NodeGraph | null } {
  const active = Boolean(status?.active_revision && status.state !== 'stopped');
  return { active, graph: active && document && activeGraphIsCurrent(document.revision, status?.active_revision) ? document.graph : null };
}

export type HardwareKind = 'camera' | 'base' | 'manipulator';
export function hardwareKind(nodeType: string): HardwareKind | null {
  const match = /^(?:eos\.hardware\.|isos\.eos_hardware\.)(camera|base|manipulator)@(?:v0\.1|v1\.0|v0\.2)$/.exec(nodeType);
  return match ? match[1] as HardwareKind : null;
}

export interface AdapterStatus { state: string; active_command_id?: string | null }
export interface MonitorReading { status?: AdapterStatus; statusAt?: number; health?: { state: string }; healthAt?: number }
export type MonitorBadge = 'live' | 'stale' | 'stopped' | 'starting' | 'degraded' | 'fault';
export function monitorBadge(reading: MonitorReading | undefined, now: number): MonitorBadge {
  if (!reading?.status || !reading.statusAt || now - reading.statusAt > 5000) return 'stale';
  switch (reading.status.state) {
    case 'stopped': return 'stopped';
    case 'starting': return 'starting';
    case 'ready': {
      if (!reading.health || !reading.healthAt || now - reading.healthAt > 5000) return 'stale';
      if (reading.health.state === 'healthy') return 'live';
      if (reading.health.state === 'degraded') return 'degraded';
      return 'fault';
    }
    default: return 'fault';
  }
}

export type MovementType = 'body_velocity' | 'relative_pose' | 'joint_positions' | 'end_effector_pose';
export type Movement =
  | { type: 'body_velocity'; x_m_s: number; y_m_s: number; yaw_rad_s: number; valid_for_ms: number }
  | { type: 'relative_pose'; x_m: number; y_m: number; yaw_rad: number; frame_id: string }
  | { type: 'joint_positions'; joint_names: string[]; joint_positions_rad: number[]; max_speed_rad_s: number | null }
  | { type: 'end_effector_pose'; pose: { position_m: [number, number, number]; orientation_xyzw: [number, number, number, number]; frame_id: string }; max_speed_m_s: number | null };

export function movementTypes(node: NodeInstance): MovementType[] {
  const names = node.movement?.modes || [];
  return names.filter((mode): mode is MovementType => ['body_velocity', 'relative_pose', 'joint_positions', 'end_effector_pose'].includes(mode));
}

export function defaultMovement(type: MovementType, hardware: NodeInstance): Movement {
  switch (type) {
    case 'body_velocity': return { type, x_m_s: 0, y_m_s: 0, yaw_rad_s: 0, valid_for_ms: Math.min(250, hardware.movement?.limits.max_velocity_lease_ms || 250) };
    case 'relative_pose': return { type, x_m: 0, y_m: 0, yaw_rad: 0, frame_id: hardware.frame_id || 'world' };
    case 'joint_positions': return { type, joint_names: hardware.joint_names || [], joint_positions_rad: (hardware.joint_names || []).map(() => 0), max_speed_rad_s: hardware.movement?.limits.max_joint_speed_rad_s || null };
    case 'end_effector_pose': return { type, pose: { position_m: [0, 0, 0], orientation_xyzw: [0, 0, 0, 1], frame_id: hardware.frame_id || 'world' }, max_speed_m_s: hardware.movement?.limits.max_end_effector_speed_m_s || null };
  }
}

export function movementValid(value: Movement, hardware: NodeInstance): boolean {
  const limits = hardware.movement?.limits || {};
  if (!movementTypes(hardware).includes(value.type)) return false;
  const bounded = (number: number, max: number): boolean => Number.isFinite(number) && Math.abs(number) <= max;
  switch (value.type) {
    case 'body_velocity': return bounded(value.x_m_s, limits.max_linear_m_s ?? 0) && bounded(value.y_m_s, limits.max_linear_m_s ?? 0) && bounded(value.yaw_rad_s, limits.max_angular_rad_s ?? 0) && Number.isInteger(value.valid_for_ms) && value.valid_for_ms > 0 && value.valid_for_ms <= (limits.max_velocity_lease_ms ?? 0);
    case 'relative_pose': return bounded(value.x_m, limits.max_translation_m ?? 0) && bounded(value.y_m, limits.max_translation_m ?? 0) && bounded(value.yaw_rad, limits.max_rotation_rad ?? 0) && Boolean(value.frame_id.trim());
    case 'joint_positions': return value.joint_names.length > 0 && value.joint_names.length === value.joint_positions_rad.length && value.joint_names.every((name, index) => name === hardware.joint_names?.[index]) && value.joint_positions_rad.every((position) => bounded(position, limits.max_joint_position_rad ?? 0)) && (value.max_speed_rad_s === null || (value.max_speed_rad_s > 0 && value.max_speed_rad_s <= (limits.max_joint_speed_rad_s ?? 0)));
    case 'end_effector_pose': return value.pose.position_m.every((position) => bounded(position, limits.max_end_effector_translation_m ?? 0)) && value.pose.orientation_xyzw.every(Number.isFinite) && Math.abs(value.pose.orientation_xyzw.reduce((sum, n) => sum + n * n, 0) - 1) < 0.02 && Boolean(value.pose.frame_id.trim()) && (value.max_speed_m_s === null || (value.max_speed_m_s > 0 && value.max_speed_m_s <= (limits.max_end_effector_speed_m_s ?? 0)));
  }
}
