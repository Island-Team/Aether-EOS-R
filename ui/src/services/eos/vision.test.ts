import { describe, expect, it } from 'vitest';
import { chooseVisionGate, decodeVisionFrame } from './vision';

function packet(metadata: unknown, body: number[] | Uint8Array = [0xff, 0xd8, 0xff, 0xd9]): ArrayBuffer {
  const header = new TextEncoder().encode(JSON.stringify(metadata));
  const bytes = new Uint8Array(4 + header.length + body.length);
  new DataView(bytes.buffer).setUint32(0, header.length, true);
  bytes.set(header, 4); bytes.set(body, 4 + header.length);
  return bytes.buffer;
}

describe('EOS Vision binary frames', () => {
  it('selects the gate linked by the active graph and retains an explicit source choice', () => {
    const gates = [
      { path: 'cores/eos/layers/interface/groups/gate/nodes/front-preview' },
      { path: 'eos/interface/gate/rear-preview' },
    ];
    expect(chooseVisionGate(gates, '', 'eos/interface/gate/front-preview')).toBe(gates[0].path);
    expect(chooseVisionGate(gates, gates[1].path, 'eos/interface/gate/front-preview')).toBe(gates[1].path);
    expect(chooseVisionGate(gates, 'removed', 'eos/interface/gate/front-preview')).toBe(gates[0].path);
    expect(chooseVisionGate(gates, '', 'eos/interface/gate/unknown')).toBe('');
  });
  it('decodes bounded metadata and JPEG bytes', () => {
    const result = decodeVisionFrame(packet({ gate_path: 'cores/eos/layers/interface/groups/gate/nodes/front-preview', width_px: 640, height_px: 480, sequence: 7 }));
    expect(result.metadata.sequence).toBe(7);
    expect(result.jpeg.type).toBe('image/jpeg');
    expect(result.jpeg.size).toBe(4);
    const jpeg = new Uint8Array(5_000_000); jpeg[0] = 0xff; jpeg[1] = 0xd8; jpeg[jpeg.length - 2] = 0xff; jpeg[jpeg.length - 1] = 0xd9;
    expect(decodeVisionFrame(packet({ gate_path: 'gate', width_px: 640, height_px: 480 }, jpeg)).jpeg.size).toBe(5_000_000);
  });
  it('rejects invalid dimensions, JPEG and oversized payloads', () => {
    expect(() => decodeVisionFrame(packet({ gate_path: 'gate', width_px: 5000, height_px: 480 }))).toThrow();
    expect(() => decodeVisionFrame(packet({ gate_path: 'gate', width_px: 640, height_px: 480 }, [1, 2, 3, 4]))).toThrow();
    expect(() => decodeVisionFrame(new ArrayBuffer(8 * 1024 * 1024 + 16_384 + 5))).toThrow();
  });
});
