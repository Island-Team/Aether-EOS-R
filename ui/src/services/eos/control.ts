/** Loopback EOS v2 bridge. IsOS supplies the graph; EOS admits every live call. */
const ORIGIN = typeof ISOSUI_EOS_API_ORIGIN === 'undefined' ? 'http://127.0.0.1:16020' : ISOSUI_EOS_API_ORIGIN;
const BASE = `${ORIGIN}/api/eos/v2`;
export const EOS_CONTROL_URL = `${BASE}/control`;

type LoopbackRequestInit = RequestInit & { targetAddressSpace?: 'loopback' };

export interface EosDiscoveryNode {
  path: string;
  node_id: string;
  kind: 'camera' | 'base' | 'manipulator' | string;
  label: string;
  facets: string[];
  capabilities: string[];
}

export interface EosCameraFrame {
  meta: { sequence?: number; monotonic_ns?: number; [key: string]: unknown };
  width_px: number;
  height_px: number;
  stride_bytes: number;
  rgb8_base64: string;
  depth: unknown | null;
}

export type EosControlReply = { ok: boolean; error?: string; [key: string]: unknown };

export async function eosRequest<T extends EosControlReply>(body: Record<string, unknown>): Promise<T> {
  const options: LoopbackRequestInit = {
    method: 'POST',
    credentials: 'omit',
    cache: 'no-store',
    mode: 'cors',
    targetAddressSpace: 'loopback',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
    signal: AbortSignal.timeout(4000),
  };
  let response: Response;
  try { response = await fetch(EOS_CONTROL_URL, options); }
  catch { throw new Error(`Cannot reach EOS at ${ORIGIN}. Check that IsOS is running EOS.`); }
  const reply = await response.json() as T;
  if (!response.ok || !reply.ok) throw new Error(reply.error || `EOS request failed (${response.status}).`);
  return reply;
}

/** RGB8 is drawn to canvas; image and byte limits bound browser memory use. */
export function decodeRgbFrame(frame: EosCameraFrame, maxBytes = 8_000_000): Uint8ClampedArray {
  const { width_px: width, height_px: height, stride_bytes: stride } = frame;
  if (!Number.isInteger(width) || !Number.isInteger(height) || !Number.isInteger(stride) ||
      width < 1 || height < 1 || width > 2048 || height > 1536 || width * height > 2_000_000 ||
      stride < width * 3 || stride > 8192 || stride * height > Math.min(8_000_000, maxBytes)) throw new Error('Camera frame exceeds display limits.');
  if (frame.rgb8_base64.length > Math.ceil(Math.min(8_000_000, maxBytes) / 3) * 4) throw new Error('Camera frame exceeds display limits.');
  const binary = atob(frame.rgb8_base64);
  if (binary.length !== stride * height) throw new Error('Camera frame data length does not match its dimensions.');
  const rgba = new Uint8ClampedArray(width * height * 4);
  for (let y = 0; y < height; y += 1) for (let x = 0; x < width; x += 1) {
    const source = y * stride + x * 3;
    const destination = (y * width + x) * 4;
    rgba[destination] = binary.charCodeAt(source);
    rgba[destination + 1] = binary.charCodeAt(source + 1);
    rgba[destination + 2] = binary.charCodeAt(source + 2);
    rgba[destination + 3] = 255;
  }
  return rgba;
}
