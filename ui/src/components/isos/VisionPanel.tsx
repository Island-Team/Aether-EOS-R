import React, { useCallback, useEffect, useRef, useState } from 'react';
import type { NodeGraph, ProcessStatus } from '../../services/isos/node-graph';
import { checkVisionAttestation, checkVisionEvidenceFrame, decodeVisionFrame, getVisionGates, validVisionSelection, visionSocketUrl, type VisionAttestation, type VisionEvidenceSelection, type VisionFrameMetadata } from '../../services/eos/vision';
import type { Language } from '../../types';

type DisplayState = 'stopped' | 'stale' | 'unavailable' | 'connecting' | 'no_frame' | 'connected' | 'disconnected';

export function VisionPanel({ lang, graph, active, revision, status, stopFenced = false }: { lang: Language; graph: NodeGraph | null; active: boolean; revision: string; status?: ProcessStatus | null; stopFenced?: boolean }): React.ReactElement {
  const copy = lang === 'cn' ? {
    title: '视觉', source: '画面来源', run: '运行', generation: '网关代次', stream: '流', digest: 'JPEG 摘要', fps: '实际帧率', gaps: '跳过的代次', cap: '订阅上限',
    stopped: '已停止', stale: '运行中节点图已过期', unavailable: '视觉证据不可用', connecting: '正在连接', no_frame: '尚无新画面', connected: '正在接收画面', disconnected: '连接已断开', rejected: '画面被拒绝', retry: '重试',
    off: '运行已保存的节点图后查看相机画面。', staleGraph: '已保存节点图与运行中节点图不同。请重启节点图后查看相机。',
  } : {
    title: 'Vision', source: 'Frame source', run: 'Run', generation: 'Gate generation', stream: 'Stream', digest: 'JPEG digest', fps: 'Delivered FPS', gaps: 'Skipped generations', cap: 'Subscription cap',
    stopped: 'Stopped', stale: 'Stale running graph', unavailable: 'Vision evidence unavailable', connecting: 'Connecting', no_frame: 'No new frame', connected: 'Receiving frames', disconnected: 'Disconnected', rejected: 'Frame rejected', retry: 'Retry',
    off: 'Run the saved graph to view camera frames.', staleGraph: 'The saved graph differs from the running graph. Restart the graph before viewing its camera.',
  };
  const selection = validVisionSelection(status?.vision_evidence) ? status.vision_evidence : null;
  const runId = status?.run_id_state === 'known' && status.run_id ? status.run_id : null;
  const current = Boolean(active && graph && !stopFenced && status?.state === 'running' && runId && revision && revision === status?.saved_revision && revision === status?.active_revision && selection);
  const identity = current ? JSON.stringify([runId, revision, selection]) : '';
  const [display, setDisplay] = useState<DisplayState>('unavailable');
  const [reason, setReason] = useState('');
  const [imageUrl, setImageUrl] = useState('');
  const [metadata, setMetadata] = useState<VisionFrameMetadata | null>(null);
  const [fps, setFps] = useState(0);
  const [gaps, setGaps] = useState(0);
  const [attempt, setAttempt] = useState(0);
  const token = useRef(0);
  const socket = useRef<WebSocket | null>(null);
  const imageRef = useRef('');
  const times = useRef<number[]>([]);
  const latest = useRef(0);
  const clear = useCallback(() => {
    ++token.current;
    const prior = socket.current; socket.current = null; prior?.close();
    if (imageRef.current) URL.revokeObjectURL(imageRef.current);
    imageRef.current = ''; latest.current = 0; times.current = [];
    setImageUrl(''); setMetadata(null); setFps(0); setGaps(0); setReason('');
  }, []);
  useEffect(() => {
    clear();
    if (!current || !selection) {
      setDisplay(!active || stopFenced || status?.state === 'stopped' ? 'stopped' : !graph || revision !== status?.saved_revision || revision !== status?.active_revision ? 'stale' : 'unavailable');
      return;
    }
    const selected: VisionEvidenceSelection = selection;
    const generationToken = token.current;
    let disposed = false;
    let attestation: VisionAttestation | null = null;
    setDisplay('connecting');
    const connect = async () => {
      try {
        const catalog = await getVisionGates();
        if (disposed || generationToken !== token.current) return;
        if (catalog.revision !== selected.projection_revision || !catalog.gates.some((gate) => gate.path === selected.gate_path && gate.source_path === selected.source_path)) throw new Error('selection_mismatch');
        const ws = new WebSocket(visionSocketUrl()); socket.current = ws; ws.binaryType = 'arraybuffer';
        ws.onopen = () => { if (generationToken !== token.current) return; ws.send(JSON.stringify({ op: 'subscribe', gate_path: selected.gate_path, revision: selected.projection_revision, target_fps: selected.target_fps, ...selected.render })); };
        ws.onmessage = async (event) => {
          if (disposed || generationToken !== token.current || ws !== socket.current) return;
          try {
            if (typeof event.data === 'string') {
              const reply = JSON.parse(event.data) as { type?: string; message?: string };
              if (reply.type === 'error') throw new Error(reply.message || 'subscription_error');
              if (reply.type === 'subscribed') {
                try { attestation = checkVisionAttestation(reply, selected, revision); setDisplay('no_frame'); setReason(''); }
                catch { socket.current = null; ws.close(); setDisplay('unavailable'); setReason('provenance_unavailable'); }
              }
              return;
            }
            if (!attestation) throw new Error('attestation_missing');
            const frame = decodeVisionFrame(event.data instanceof ArrayBuffer ? event.data : await (event.data as Blob).arrayBuffer());
            const nextGeneration = await checkVisionEvidenceFrame(frame, selected, attestation, latest.current);
            if (disposed || generationToken !== token.current || ws !== socket.current) return;
            const previousGeneration = latest.current;
            if (nextGeneration <= previousGeneration) throw new Error('generation_stale');
            if (previousGeneration > 0 && nextGeneration > previousGeneration + 1) setGaps((value) => value + nextGeneration - previousGeneration - 1);
            latest.current = nextGeneration;
            const next = URL.createObjectURL(frame.jpeg);
            if (imageRef.current) URL.revokeObjectURL(imageRef.current);
            imageRef.current = next; setImageUrl(next); setMetadata(frame.metadata); setDisplay('connected'); setReason('');
            const now = performance.now(); times.current = [...times.current.filter((time) => now - time < 2000), now];
            setFps(times.current.length > 1 ? Math.round((times.current.length - 1) * 10000 / (now - times.current[0])) / 10 : 0);
          } catch (error) {
            if (disposed || generationToken !== token.current) return;
            setReason(error instanceof Error ? error.message : 'frame_invalid');
          }
        };
        ws.onerror = () => { if (!disposed && generationToken === token.current) setReason('socket_error'); };
        ws.onclose = () => { if (!disposed && generationToken === token.current) { socket.current = null; setDisplay('disconnected'); if (imageRef.current) URL.revokeObjectURL(imageRef.current); imageRef.current = ''; setImageUrl(''); setMetadata(null); setFps(0); } };
      } catch (error) {
        if (disposed || generationToken !== token.current) return;
        setDisplay('unavailable'); setReason(error instanceof Error ? error.message : 'catalog_unavailable');
      }
    };
    void connect();
    return () => { disposed = true; clear(); };
  }, [identity, attempt, clear]);
  useEffect(() => () => { clear(); }, [clear]);
  return <section id="eos-vision" className="eos-workbench__section eos-vision" aria-label={copy.title}>
    <h2>{copy.title}</h2>
    {!active && <p>{copy.off}</p>}
    {active && !graph && <p role="alert">{copy.staleGraph}</p>}
    <p role="status">{copy.title}: {copy[display]} · {copy.fps}: {fps.toFixed(1)} Hz · {copy.gaps}: {gaps}</p>
    {selection && <p>{copy.source}: <code>{selection.source_path}</code> · <code>{selection.gate_path}</code> · {copy.cap}: {selection.target_fps} Hz</p>}
    {runId && <p>{copy.run}: <code>{runId}</code></p>}
    {metadata && <p>{copy.generation}: {metadata.gate_generation} · {copy.stream}: <code>{metadata.runtime_stream_id}</code> · {copy.digest}: <code>{metadata.jpeg_sha256}</code> · {metadata.width_px} × {metadata.height_px}</p>}
    {reason && <p role="alert" className="eos-control__warning">{copy.rejected}: <code>{reason}</code></p>}
    {display === 'disconnected' && <button type="button" onClick={() => setAttempt((value) => value + 1)}>{copy.retry}</button>}
    <div className="eos-vision__image">{imageUrl && display === 'connected' ? <img src={imageUrl} alt={`${copy.title}: ${selection?.source_path || ''}`} /> : <p>{copy[display]}</p>}</div>
  </section>;
}
