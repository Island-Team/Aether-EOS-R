// @vitest-environment jsdom
import React, { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { createHash } from 'node:crypto';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { NodeGraph, ProcessStatus } from '../../services/isos/node-graph';
import { VisionPanel } from './VisionPanel';

const graphRevision = '0'.repeat(64);
const projection = 'a'.repeat(64);
const stream = '8b4e9b70-5481-4d88-9bd6-4f07cb466ae4';
const gate = 'eos/interface/gate/front';
const source = 'eos/hardware/camera/front';
const jpeg = new Uint8Array([0xff, 0xd8, 0xff, 0xd9]);
const selection = { gate_path: gate, source_path: source, projection_revision: projection, render: { max_width_px: 1280, jpeg_quality: 80 }, target_fps: 30 };
const status: ProcessStatus = { schema_version: '1.0.0', state: 'running', run_id_state: 'known', run_id: 'run-7', saved_revision: graphRevision, active_revision: graphRevision, processes: [], vision_evidence: selection };
const graph = { version: '0.1.0', robot: { id: 'robot', name: 'Robot' }, cores: {} } as NodeGraph;
class StubSocket {
  static instances: StubSocket[] = [];
  binaryType = '';
  onopen: (() => void) | null = null;
  onmessage: ((event: { data: string | ArrayBuffer }) => void) | null = null;
  onclose: (() => void) | null = null;
  onerror: (() => void) | null = null;
  sent: string[] = [];
  closed = false;
  constructor(public url: string) { StubSocket.instances.push(this); }
  send(message: string) { this.sent.push(message); }
  close() { this.closed = true; }
}
function packet(digest: string, generation: number): ArrayBuffer {
  const metadata = new TextEncoder().encode(JSON.stringify({ graph_revision: graphRevision, projection_revision: projection, runtime_stream_id: stream, gate_path: gate, source_path: source, gate_generation: generation, source_frame_id: `frame-${generation}`, width_px: 2, height_px: 2, render: selection.render, jpeg_sha256: digest }));
  const bytes = new Uint8Array(4 + metadata.length + jpeg.length);
  new DataView(bytes.buffer).setUint32(0, metadata.length, true); bytes.set(metadata, 4); bytes.set(jpeg, 4 + metadata.length);
  return bytes.buffer;
}
let container: HTMLDivElement;
let root: Root;
beforeEach(() => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  vi.stubGlobal('WebSocket', StubSocket);
  vi.stubGlobal('crypto', { subtle: { digest: async (_algorithm: string, bytes: ArrayBuffer) => Uint8Array.from(createHash('sha256').update(new Uint8Array(bytes)).digest()).buffer } });
  vi.stubGlobal('fetch', vi.fn(async () => ({ ok: true, json: async () => ({ revision: projection, gates: [{ path: gate, source_path: source }] }) })));
  vi.stubGlobal('URL', Object.assign(URL, { createObjectURL: vi.fn(() => 'blob:frame'), revokeObjectURL: vi.fn() }));
  StubSocket.instances = [];
  container = document.createElement('div'); document.body.appendChild(container); root = createRoot(container);
});
afterEach(async () => { await act(async () => root.unmount()); container.remove(); vi.unstubAllGlobals(); });

describe('mounted Vision evidence display', () => {
  it('keeps generation 2 when its digest resolves before generation 1', async () => {
    const digest = createHash('sha256').update(jpeg).digest('hex');
    await act(async () => root.render(<VisionPanel lang="en" graph={graph} active revision={graphRevision} status={status} />));
    const ws = StubSocket.instances[0];
    await act(async () => ws.onmessage?.({ data: JSON.stringify({ type: 'subscribed', graph_revision: graphRevision, projection_revision: projection, runtime_stream_id: stream, gate_path: gate, source_path: source, gate_generation: 0 }) }));
    const pending: Array<(value: ArrayBuffer) => void> = [];
    vi.stubGlobal('crypto', { subtle: { digest: vi.fn(() => new Promise<ArrayBuffer>((resolve) => pending.push(resolve))) } });
    await act(async () => { ws.onmessage?.({ data: packet(digest, 1) }); ws.onmessage?.({ data: packet(digest, 2) }); await Promise.resolve(); });
    expect(pending).toHaveLength(2);
    const actual = Uint8Array.from(createHash('sha256').update(jpeg).digest()).buffer;
    await act(async () => { pending[1](actual); await Promise.resolve(); await Promise.resolve(); });
    expect(container.textContent).toContain('Gate generation: 2');
    await act(async () => { pending[0](actual); await Promise.resolve(); await Promise.resolve(); });
    expect(container.textContent).toContain('Gate generation: 2');
    expect(container.textContent).toContain('generation_stale');
    expect(container.querySelector('img')).not.toBeNull();
  });
  it('does not display an unproved legacy subscription', async () => {
    await act(async () => root.render(<VisionPanel lang="en" graph={graph} active revision={graphRevision} status={status} />));
    const ws = StubSocket.instances[0];
    await act(async () => ws.onmessage?.({ data: '{"type":"subscribed"}' }));
    expect(ws.closed).toBe(true);
    expect(container.querySelector('img')).toBeNull();
    expect(container.textContent).toContain('Vision evidence unavailable');
    expect(container.textContent).toContain('provenance_unavailable');
  });
  it('binds the public subscription and revokes a frame on Stop before a late digest completes', async () => {
    const digest = createHash('sha256').update(jpeg).digest('hex');
    await act(async () => root.render(<VisionPanel lang="en" graph={graph} active revision={graphRevision} status={status} />));
    const ws = StubSocket.instances[0]; expect(ws.url).toContain('/api/eos/v2/vision/ws');
    await act(async () => ws.onopen?.());
    expect(JSON.parse(ws.sent[0])).toEqual({ op: 'subscribe', gate_path: gate, revision: projection, target_fps: 30, max_width_px: 1280, jpeg_quality: 80 });
    await act(async () => ws.onmessage?.({ data: JSON.stringify({ type: 'subscribed', graph_revision: graphRevision, projection_revision: projection, runtime_stream_id: stream, gate_path: gate, source_path: source, gate_generation: 0 }) }));
    expect(container.textContent).toContain('No new frame');
    await act(async () => { ws.onmessage?.({ data: packet(digest, 1) }); await Promise.resolve(); await Promise.resolve(); });
    expect(container.querySelector('img')?.getAttribute('src'), container.textContent || '').toBe('blob:frame');
    expect(container.textContent).toContain('Gate generation: 1');
    let release!: (value: ArrayBuffer) => void;
    const pending = new Promise<ArrayBuffer>((resolve) => { release = resolve; });
    vi.stubGlobal('crypto', { subtle: { digest: vi.fn(() => pending) } });
    await act(async () => { ws.onmessage?.({ data: packet(digest, 2) }); await Promise.resolve(); });
    await act(async () => root.render(<VisionPanel lang="en" graph={graph} active={false} revision={graphRevision} status={{ ...status, state: 'stopped', vision_evidence: null }} stopFenced />));
    expect(ws.closed).toBe(true);
    expect(container.querySelector('img')).toBeNull();
    await act(async () => release(Uint8Array.from(createHash('sha256').update(jpeg).digest()).buffer));
    expect(container.querySelector('img')).toBeNull();
    expect(container.textContent).toContain('Stopped');
  });
});
