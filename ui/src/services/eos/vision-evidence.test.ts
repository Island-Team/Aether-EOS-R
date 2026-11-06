import { webcrypto } from 'node:crypto';
import { beforeAll, describe, expect, it, vi } from 'vitest';
import { checkVisionAttestation, checkVisionEvidenceFrame, decodeVisionFrame, validVisionSelection, type VisionEvidenceSelection } from './vision';

const graph = '0'.repeat(64);
const projection = 'a'.repeat(64);
const stream = '8b4e9b70-5481-4d88-9bd6-4f07cb466ae4';
const selection: VisionEvidenceSelection = { gate_path: 'eos/interface/gate/front', source_path: 'eos/hardware/camera/front', projection_revision: projection, render: { max_width_px: 1280, jpeg_quality: 80 }, target_fps: 30 };
const attestation = { type: 'subscribed', graph_revision: graph, projection_revision: projection, gate_path: selection.gate_path, source_path: selection.source_path, runtime_stream_id: stream, gate_generation: 0 };
const jpeg = new Uint8Array([0xff, 0xd8, 0xff, 0xd9]);
function packet(patch: Record<string, unknown> = {}, metadataPadding = ''): ArrayBuffer {
  const metadata = { ...attestation, type: undefined, source_frame_id: 'frame-7', gate_generation: 7, width_px: 640, height_px: 480, render: selection.render, jpeg_sha256: '0'.repeat(64), ...patch, padding: metadataPadding };
  const bytes = new TextEncoder().encode(JSON.stringify(metadata));
  const output = new Uint8Array(4 + bytes.length + jpeg.length);
  new DataView(output.buffer).setUint32(0, bytes.length, true);
  output.set(bytes, 4); output.set(jpeg, 4 + bytes.length);
  return output.buffer;
}

describe('selected Vision evidence', () => {
  beforeAll(() => { vi.stubGlobal('crypto', webcrypto); });
  it('rejects absent or malformed IsOS binding and wrong EOS attestation', () => {
    expect(validVisionSelection(selection)).toBe(true);
    expect(validVisionSelection({ ...selection, render: { max_width_px: 640, jpeg_quality: 80 } })).toBe(false);
    expect(validVisionSelection(null)).toBe(false);
    expect(checkVisionAttestation(attestation, selection, graph).gate_generation).toBe(0);
    expect(() => checkVisionAttestation({ ...attestation, source_path: 'eos/hardware/camera/rear' }, selection, graph)).toThrow('source_mismatch');
    expect(() => checkVisionAttestation({ ...attestation, graph_revision: 'f'.repeat(64) }, selection, graph)).toThrow('revision_mismatch');
  });
  it('accepts only the exact JPEG digest and monotonic selected delivery', async () => {
    const actual = Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', jpeg.buffer)), (byte) => byte.toString(16).padStart(2, '0')).join('');
    const good = decodeVisionFrame(packet({ jpeg_sha256: actual }));
    const subscribed = checkVisionAttestation(attestation, selection, graph);
    expect(await checkVisionEvidenceFrame(good, selection, subscribed, 6)).toBe(7);
    await expect(checkVisionEvidenceFrame(good, selection, subscribed, 7)).rejects.toThrow('generation_stale');
    await expect(checkVisionEvidenceFrame(decodeVisionFrame(packet({ jpeg_sha256: 'f'.repeat(64) })), selection, subscribed, 6)).rejects.toThrow('digest_mismatch');
    await expect(checkVisionEvidenceFrame(decodeVisionFrame(packet({ source_path: 'eos/hardware/camera/rear', jpeg_sha256: actual })), selection, subscribed, 6)).rejects.toThrow('source_mismatch');
    await expect(checkVisionEvidenceFrame(decodeVisionFrame(packet({ gate_path: 'eos/interface/gate/rear', jpeg_sha256: actual })), selection, subscribed, 6)).rejects.toThrow('gate_mismatch');
    await expect(checkVisionEvidenceFrame(decodeVisionFrame(packet({ runtime_stream_id: 'ffffffff-ffff-4fff-8fff-ffffffffffff', jpeg_sha256: actual })), selection, subscribed, 6)).rejects.toThrow('stream_mismatch');
    await expect(checkVisionEvidenceFrame(decodeVisionFrame(packet({ projection_revision: 'f'.repeat(64), jpeg_sha256: actual })), selection, subscribed, 6)).rejects.toThrow('revision_mismatch');
    await expect(checkVisionEvidenceFrame(decodeVisionFrame(packet({ render: { max_width_px: 640, jpeg_quality: 80 }, jpeg_sha256: actual })), selection, subscribed, 6)).rejects.toThrow('render_mismatch');
  });
  it('rejects oversized metadata before parsing or hashing', () => {
    expect(() => decodeVisionFrame(packet({}, 'x'.repeat(4096)))).toThrow('Invalid Vision frame metadata length.');
  });
});
