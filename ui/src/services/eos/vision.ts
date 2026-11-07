const ORIGIN = typeof ISOSUI_EOS_API_ORIGIN === 'undefined' ? 'http://127.0.0.1:16020' : ISOSUI_EOS_API_ORIGIN;
const BASE = `${ORIGIN}/api/eos/v2/vision`;
const MAX_JPEG_BYTES = 8 * 1024 * 1024;
const MAX_METADATA_BYTES = 4096;
const MAX_FRAME_BYTES = 4 + MAX_METADATA_BYTES + MAX_JPEG_BYTES;

export interface VisionGate { path: string; label?: string; output_port?: string; source_path?: string; source_rate_hz?: number }
export interface VisionGates { revision: string; gates: VisionGate[] }
export interface VisionRender { max_width_px: number; jpeg_quality: number }
export interface VisionEvidenceSelection { gate_path: string; source_path: string; projection_revision: string; render: VisionRender; target_fps: number }
export interface VisionFrameMetadata { gate_path: string; sequence?: number; timestamp_unix_ms?: number; width_px: number; height_px: number; source_rate_hz?: number; graph_revision?: string; projection_revision?: string; runtime_stream_id?: string; source_path?: string; source_frame_id?: string; gate_generation?: number; render?: VisionRender; jpeg_sha256?: string }
export interface VisionAttestation { type: 'subscribed'; graph_revision: string; projection_revision: string; runtime_stream_id: string; gate_path: string; source_path: string; gate_generation: number }
export interface VisionFrame { metadata: VisionFrameMetadata; jpeg: Blob; jpegBytes: Uint8Array }
export interface VisionSubscription { op: 'subscribe'; gate_path: string; revision: string; target_fps?: number; max_width_px?: number; jpeg_quality?: number }
type LoopbackRequestInit = RequestInit & { targetAddressSpace?: 'loopback' };

/** Retain an operator choice while it exists; otherwise use only the graph-linked gate. */
export function chooseVisionGate(gates: VisionGate[], current: string, linked: string): string {
  const canonical = (path: string): string => path.replace(/^cores\/([^/]+)\/layers\/([^/]+)\/groups\/([^/]+)\/nodes\/([^/]+)$/, '$1/$2/$3/$4');
  return gates.find((gate) => gate.path === current)?.path || gates.find((gate) => canonical(gate.path) === linked)?.path || '';
}

export async function getVisionGates(): Promise<VisionGates> {
  const options: LoopbackRequestInit = { credentials: 'omit', cache: 'no-store', mode: 'cors', targetAddressSpace: 'loopback', signal: AbortSignal.timeout(4000) };
  const response = await fetch(`${BASE}/gates`, options);
  if (!response.ok) throw new Error(`EOS Vision discovery failed (${response.status}).`);
  const body = await response.json() as VisionGates;
  if (!body || typeof body.revision !== 'string' || !body.revision || !Array.isArray(body.gates) || body.gates.some((gate) => !gate || typeof gate.path !== 'string' || !gate.path)) throw new Error('EOS returned an invalid Vision gate catalog.');
  return body;
}

export function visionSocketUrl(): string {
  const url = new URL(`${BASE}/ws`);
  url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:';
  return url.toString();
}

/** LE length-prefixed metadata followed by a JPEG body; reject oversized or malformed frames. */
export function decodeVisionFrame(buffer: ArrayBuffer): VisionFrame {
  if (buffer.byteLength < 8 || buffer.byteLength > MAX_FRAME_BYTES) throw new Error('Vision frame exceeds display limits.');
  const metadataLength = new DataView(buffer).getUint32(0, true);
  if (metadataLength < 2 || metadataLength > MAX_METADATA_BYTES || metadataLength + 6 > buffer.byteLength) throw new Error('Invalid Vision frame metadata length.');
  let metadata: VisionFrameMetadata;
  try { metadata = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(new Uint8Array(buffer, 4, metadataLength))) as VisionFrameMetadata; }
  catch { throw new Error('invalid_metadata_json'); }
  if (!metadata || typeof metadata.gate_path !== 'string' || !Number.isInteger(metadata.width_px) || !Number.isInteger(metadata.height_px) || metadata.width_px < 1 || metadata.height_px < 1 || metadata.width_px > 4096 || metadata.height_px > 4096) throw new Error('Invalid Vision frame dimensions.');
  const jpeg = new Uint8Array(buffer, 4 + metadataLength);
  if (jpeg.byteLength > MAX_JPEG_BYTES) throw new Error('Vision JPEG exceeds display limits.');
  if (jpeg[0] !== 0xff || jpeg[1] !== 0xd8 || jpeg[jpeg.length - 2] !== 0xff || jpeg[jpeg.length - 1] !== 0xd9) throw new Error('Invalid Vision JPEG payload.');
  return { metadata, jpeg: new Blob([jpeg], { type: 'image/jpeg' }), jpegBytes: jpeg };
}

const HEX64 = /^[0-9a-f]{64}$/;
const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
const PATH = /^[A-Za-z0-9_.-]+(?:\/[A-Za-z0-9_.-]+){3,}$/;
const generation = (value: unknown): value is number => Number.isSafeInteger(value) && (value as number) >= 0;

export function validVisionSelection(value: unknown): value is VisionEvidenceSelection {
  if (!value || typeof value !== 'object') return false;
  const item = value as Partial<VisionEvidenceSelection>;
  return typeof item.gate_path === 'string' && PATH.test(item.gate_path) && typeof item.source_path === 'string' && PATH.test(item.source_path)
    && typeof item.projection_revision === 'string' && HEX64.test(item.projection_revision)
    && item.render?.max_width_px === 1280 && item.render?.jpeg_quality === 80 && item.target_fps === 30;
}

export function checkVisionAttestation(value: unknown, selection: VisionEvidenceSelection, graphRevision: string): VisionAttestation {
  const item = value as Partial<VisionAttestation> | null;
  if (!item || item.type !== 'subscribed' || !HEX64.test(graphRevision) || item.graph_revision !== graphRevision || item.projection_revision !== selection.projection_revision) throw new Error('revision_mismatch');
  if (item.gate_path !== selection.gate_path) throw new Error('gate_mismatch');
  if (item.source_path !== selection.source_path) throw new Error('source_mismatch');
  if (typeof item.runtime_stream_id !== 'string' || !UUID.test(item.runtime_stream_id) || !generation(item.gate_generation)) throw new Error('stream_mismatch');
  return item as VisionAttestation;
}

export async function checkVisionEvidenceFrame(frame: VisionFrame, selection: VisionEvidenceSelection, attestation: VisionAttestation, lastGeneration: number): Promise<number> {
  const item = frame.metadata;
  if (item.graph_revision !== attestation.graph_revision || item.projection_revision !== selection.projection_revision) throw new Error('revision_mismatch');
  if (item.gate_path !== selection.gate_path) throw new Error('gate_mismatch');
  if (item.source_path !== selection.source_path) throw new Error('source_mismatch');
  if (item.runtime_stream_id !== attestation.runtime_stream_id) throw new Error('stream_mismatch');
  if (!generation(item.gate_generation) || item.gate_generation === 0 || item.gate_generation < attestation.gate_generation || item.gate_generation <= lastGeneration) throw new Error('generation_stale');
  if (!item.render || item.render.max_width_px !== selection.render.max_width_px || item.render.jpeg_quality !== selection.render.jpeg_quality) throw new Error('render_mismatch');
  if (typeof item.source_frame_id !== 'string' || !item.source_frame_id.trim() || item.source_frame_id.length > 128) throw new Error('source_frame_missing');
  if (typeof item.jpeg_sha256 !== 'string' || !HEX64.test(item.jpeg_sha256)) throw new Error('digest_missing');
  const digest = await crypto.subtle.digest('SHA-256', new Uint8Array(frame.jpegBytes).buffer as ArrayBuffer);
  const actual = Array.from(new Uint8Array(digest), (byte) => byte.toString(16).padStart(2, '0')).join('');
  if (actual !== item.jpeg_sha256) throw new Error('digest_mismatch');
  return item.gate_generation;
}
