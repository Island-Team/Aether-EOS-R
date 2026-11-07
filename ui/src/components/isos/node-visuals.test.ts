import { describe, expect, it } from 'vitest';
import { dataTypeColor, frequencyPattern, interfaceVisual, nodeVisualStyle, portFamily, portVisual, nodeSummary } from './node-visuals';
import type { NodeTypeDefinition } from '@/services/isos/node-graph';

const definition = (core: string, layer: string, group: string): NodeTypeDefinition => ({ id: `${core}.${layer}.${group}@v1.0`, core, layer, group, kind: 'software', lifecycle: 'internal', capabilities: [], settings: [], inputs: [], outputs: [] });
describe('node visual semantics', () => {
  it('uses independent transport shapes, exact type colors, and rate patterns', () => {
    const slow = { name: 'preview', data_type: 'vision_frame', required: false, expected_rate_hz: 0.1, transport: 'websocket' } as const;
    const fast = { ...slow, expected_rate_hz: 60 };
    expect(portVisual(slow).className).toContain('transport--websocket');
    expect(portVisual({ ...slow, transport: 'http', delivery_mode: 'event' }).className).toContain('transport--http');
    expect(portVisual({ ...slow, transport: 'http', delivery_mode: 'event' }).className).toContain('delivery--event');
    expect(portVisual(slow).style['--port']).toBe(dataTypeColor('vision_frame'));
    expect(dataTypeColor('vision_frame')).not.toBe(dataTypeColor('camera_frame'));
    expect(interfaceVisual(slow).className).toContain('frequency--unlimited');
    expect(interfaceVisual(slow, 0.1).className).toContain('frequency--low');
    expect(interfaceVisual(fast, 60).className).toContain('frequency--solid');
    expect(interfaceVisual(slow).style['--wire']).toBe(interfaceVisual(fast).style['--wire']);
    expect(frequencyPattern(undefined)).toBe('unknown');
  });
  it('keeps port colors tied to typed payloads', () => {
    expect(portFamily('CameraFrame')).toBe('camera');
    expect(portFamily('BaseMovementCommand')).toBe('movement');
    expect(portFamily('HealthSample')).toBe('health');
  });
  it('uses a stable layer hue and group shade without changing port semantics', () => {
    const camera = nodeVisualStyle(definition('eos', 'hardware', 'camera'));
    const mujoco = nodeVisualStyle(definition('eos', 'hardware', 'mujoco'));
    expect(camera['--node-layer']).toBe(mujoco['--node-layer']);
    expect(camera['--node-group-light']).not.toBe(mujoco['--node-group-light']);
    expect(nodeVisualStyle(definition('eos', 'hardware', 'mujoco'))).toEqual(mujoco);
    expect(nodeVisualStyle(definition('egia', 'common', 'mujoco'))['--node-layer']).not.toBe(mujoco['--node-layer']);
    expect(mujoco['--node-group-dark']).toMatch(/^hsl\(194 /);
    expect(portFamily('CameraFrame')).toBe('camera');
  });
  it('summarizes only declared settings, exact pins, and typed connections', () => {
    const process = { ...definition('eos', 'process', 'backend'), lifecycle: 'process' as const, settings: [{ name: 'scene', data_type: 'string' as const, required: true }] };
    expect(nodeSummary(process, { id: 'sim', node_type: process.id, label: 'sim', description: '', package: { id: 'example/sim', version: '1.0.0' }, settings: { scene: 'default', payload: 'hidden' } })).toEqual(['package: example/sim@1.0.0', 'scene: default']);
    const other = { ...definition('eos', 'hardware', 'base'), settings: [{ name: 'gain', data_type: 'number' as const, required: true }] };
    expect(nodeSummary(other, { id: 'drive', node_type: other.id, label: 'Drive', description: '', settings: { gain: 0.5, secret: 'hidden' }, inputs: { command: { node: 'eos/dispatcher/base/velocity', output: 'command' } } })).toEqual(['gain: 0.5', 'connections: 1']);
  });
});
