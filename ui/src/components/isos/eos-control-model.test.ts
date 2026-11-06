import { describe, expect, it } from 'vitest';
import { decodeRgbFrame, type EosCameraFrame } from '../../services/eos/control';
import { activeGraphIsCurrent, defaultMovement, discoveredHardware, eosControlNodes, hardwareKind, monitorBadge, movementValid, visionRuntimeState } from './eos-control-model';
import type { NodeGraph, NodeInstance } from '../../services/isos/node-graph';

const base: NodeInstance = { id: 'base_1', node_type: 'eos.hardware.base@v1.0', label: 'Base', description: '', movement: { modes: ['body_velocity'], limits: { max_linear_m_s: 1, max_angular_rad_s: 0.5, max_velocity_lease_ms: 300 } } };
const manual: NodeInstance = { id: 'manual_1', node_type: 'eos.control.manual@v1.0', label: 'Drive', description: '', settings: { target: 'cores/eos/layers/hardware/groups/base/nodes/base_1' } };
const graph: NodeGraph = { version: '1.0.0', robot: { id: 'robot', name: 'Robot' }, cores: { eos: { layers: { hardware: { groups: { base: { nodes: { base_1: base } } } }, control: { groups: { manual: { nodes: { manual_1: manual } } } } } } } };

describe('EOS workbench admission model', () => {
  it('shows only registered graph control nodes and discovered graph hardware', () => {
    expect(eosControlNodes(graph).map(({ node }) => node.id)).toEqual(['manual_1']);
    expect(discoveredHardware(graph, [{ path: 'cores/eos/layers/hardware/groups/base/nodes/base_1', node_id: 'base_1', kind: 'base', label: 'Base', facets: [], capabilities: [] }, { path: 'cores/eos/layers/hardware/groups/camera/nodes/unconfigured', node_id: 'unconfigured', kind: 'camera', label: 'Other', facets: [], capabilities: [] }]).map(({ node }) => node.id)).toEqual(['base_1']);
  });
  it('discovers EOS hardware and controls within the single isos core', () => {
    const unified: NodeGraph = { version: '0.2.0', robot: graph.robot, cores: { isos: { layers: {
      eos_hardware: { groups: { base: { nodes: { base_1: { ...base, node_type: 'isos.eos_hardware.base@v0.2' } } } } },
      eos_control: { groups: { manual: { nodes: { manual_1: { ...manual, node_type: 'isos.eos_control.manual@v0.2' } } } } },
    } } } };
    expect(eosControlNodes(unified).map(({ path }) => path)).toEqual(['isos/eos_control/manual/manual_1']);
    expect(discoveredHardware(unified, [{ path: 'cores/isos/layers/eos_hardware/groups/base/nodes/base_1', node_id: 'base_1', kind: 'base', label: 'Base', facets: [], capabilities: [] }]).map(({ path }) => path)).toEqual(['isos/eos_hardware/base/base_1']);
    expect(hardwareKind('isos.eos_hardware.base@v0.2')).toBe('base');
  });
  it('rejects stale revisions and movements outside hardware limits', () => {
    expect(activeGraphIsCurrent('saved', 'active')).toBe(false);
    expect(activeGraphIsCurrent('saved', 'saved')).toBe(true);
    const movement = defaultMovement('body_velocity', base);
    expect(movementValid(movement, base)).toBe(true);
    if (movement.type === 'body_velocity') {
      expect(movementValid({ ...movement, x_m_s: 1.1 }, base)).toBe(false);
      expect(movementValid({ ...movement, valid_for_ms: 400 }, base)).toBe(false);
    }
  });
  it('allows read-only Vision on an older active graph without using a stale default edge', () => {
    const document = { revision: 'new', graph };
    const status = { schema_version: '1.0.0' as const, active_revision: 'old', state: 'running' as const, processes: [] };
    expect(visionRuntimeState(document, status)).toEqual({ active: true, graph: null });
    expect(visionRuntimeState(document, { ...status, active_revision: 'new' })).toEqual({ active: true, graph });
    expect(visionRuntimeState(document, { ...status, active_revision: null, state: 'stopped' })).toEqual({ active: false, graph: null });
  });
  it('bounds and decodes an RGB frame with row stride', () => {
    const frame: EosCameraFrame = { meta: {}, width_px: 1, height_px: 2, stride_bytes: 4, rgb8_base64: btoa(String.fromCharCode(1, 2, 3, 0, 4, 5, 6, 0)), depth: null };
    expect([...decodeRgbFrame(frame)]).toEqual([1, 2, 3, 255, 4, 5, 6, 255]);
    expect(() => decodeRgbFrame({ ...frame, width_px: 4096 })).toThrow('display limits');
    expect(() => decodeRgbFrame({ ...frame, rgb8_base64: btoa('short') })).toThrow('length');
  });
  it('uses the hardware family and adapter state for monitor labels', () => {
    expect(hardwareKind('eos.hardware.base@v1.0')).toBe('base');
    expect(hardwareKind('eos.hardware.manipulator@v1.0')).toBe('manipulator');
    expect(hardwareKind('eos.hardware.camera@v1.0')).toBe('camera');
    expect(hardwareKind('eos.control.manual@v1.0')).toBeNull();
    expect(monitorBadge({ status: { state: 'ready' }, statusAt: 1000, health: { state: 'healthy' }, healthAt: 1000 }, 1200)).toBe('live');
    expect(monitorBadge({ status: { state: 'ready' }, statusAt: 1000, health: { state: 'degraded' }, healthAt: 1000 }, 1200)).toBe('degraded');
    expect(monitorBadge({ status: { state: 'ready' }, statusAt: 1000, health: { state: 'unavailable' }, healthAt: 1000 }, 1200)).toBe('fault');
    expect(monitorBadge({ status: { state: 'ready' }, statusAt: 1000, health: { state: 'healthy' }, healthAt: 1000 }, 7001)).toBe('stale');
    expect(monitorBadge({ status: { state: 'ready' }, statusAt: 1000 }, 1200)).toBe('stale');
    expect(monitorBadge({ status: { state: 'stopped' }, statusAt: 1000 }, 1200)).toBe('stopped');
    expect(monitorBadge({ status: { state: 'faulted' }, statusAt: 1000 }, 1200)).toBe('fault');
    expect(monitorBadge({ status: { state: 'stopped' }, statusAt: 1000 }, 7001)).toBe('stale');
  });
});
