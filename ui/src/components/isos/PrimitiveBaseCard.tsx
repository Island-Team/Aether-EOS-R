import React, { useEffect, useRef, useState } from 'react';
import type { Language } from '../../types';
import { getNodeGraph, getProcessStatus, type NodeGraphDocument, type ProcessStatus } from '../../services/isos/node-graph';
import { stopDeploymentConverged } from './deployment-control';
import { eosMonotonicTime, resolvePrimitiveBindings, sendFence, sendPrimitive } from '../../services/eos/primitive-command';
import { defaultMovement, movementValid, type Movement } from './eos-control-model';
import { MovementEditor } from './PrimitiveMovementEditor';
import { readPrimitiveResult, ResultClientError, terminalResult } from '../../services/eos/primitive-result';
import { acceptPrimitiveResult, newPrimitiveRecord, pollAllowed, primitiveError, resultFence, type PrimitiveRecord } from './primitive-result-state';
import { monitorBadge, type AdapterStatus } from './eos-control-model';
import { stopReplyFeedback, stopFailureFeedback, type StopFeedback } from './stop-feedback';

interface Reading { status?: AdapterStatus; statusAt?: number; health?: { state: string }; healthAt?: number }
interface Props { lang: Language; graph: NodeGraphDocument | null; status: ProcessStatus | null; dirty: boolean; readOnly: boolean; bindingPath?: string; typeDescriptions?: Record<string, string>;
  readings: Record<string, Reading>; clock: number; visible?: boolean; stopFenced?: boolean; isStopFenced?: () => boolean; onStopIntent?: (runId: string | null) => void; onStopOutcome?: (feedback: StopFeedback, terminalRun?: string | null) => void; onStop: (status: ProcessStatus) => void; onOpenLogs: () => void }
const copy = {
  en: { title: 'Active primitive bindings', missing: 'No supported active @v0.1 primitive binding has exactly one target.', gated: 'Invoke requires a current managed run, one healthy target and valid typed arguments.', invoke: 'Invoke', stop: 'Stop deployment', stopping: 'Stopping…', logs: 'Current run logs', refresh: 'Refresh result', command: 'Command ID', revision: 'Active revision', binding: 'Binding', target: 'Target', result: 'Owner status', sending: 'Sending (browser)', following: 'In progress', unavailable: 'Unavailable', oldRun: 'Unavailable for this run', paused: 'In progress; automatic reads paused', updated: 'Last update', stopStatus: 'IsOS stop status', lease: 'Lease', invalid: 'Enter values within the hardware limits.' },
  cn: { title: '活动原语绑定', missing: '没有恰好一个目标的受支持活动 @v0.1 原语绑定。', gated: '调用需要当前受管运行、单一健康目标和有效的类型化参数。', invoke: '调用', stop: '停止部署', stopping: '正在停止…', logs: '当前运行日志', refresh: '刷新结果', command: '命令 ID', revision: '活动修订', binding: '绑定', target: '目标', result: '所有者状态', sending: '发送中（浏览器）', following: '执行中', unavailable: '不可用', oldRun: '不适用于本次运行', paused: '仍在执行；自动读取已暂停', updated: '最近更新', stopStatus: 'IsOS 停止状态', lease: '有效期', invalid: '参数超出硬件限值。' },
};
function errorText(error: unknown): { kind: 'transport' | 'owner' | 'protocol' | 'validation'; message: string } {
  return error instanceof ResultClientError ? { kind: error.kind, message: error.message } : { kind: 'transport', message: 'Service unavailable.' };
}
export function PrimitiveBaseCard({ lang, graph, status, dirty, readOnly, bindingPath, typeDescriptions, readings, clock, visible = true, stopFenced = false, isStopFenced, onStopIntent, onStopOutcome, onStop, onOpenLogs }: Props): React.ReactElement {
  const c = copy[lang];
  const bindings = graph ? resolvePrimitiveBindings(graph.graph) : [];
  const binding = bindings.find((candidate) => candidate.path === bindingPath) || (!bindingPath ? bindings[0] : undefined);
  const [value, setValue] = useState<Movement>({ type: 'body_velocity', x_m_s: 0, y_m_s: 0, yaw_rad_s: 0, valid_for_ms: 100 });
  useEffect(() => { if (binding) setValue(defaultMovement(binding.mode, binding.target)); }, [binding?.path, binding?.mode, binding?.targetPath]);
  const [record, setRecord] = useState<PrimitiveRecord | null>(null);
  const [stopResult, setStopResult] = useState<StopFeedback | null>(null);
  const [stoppedRunId, setStoppedRunId] = useState<string | null>(null);
  const [stopping, setStopping] = useState(false);
  const generation = useRef(0);
  const graphFlags = useRef({ dirty, readOnly, stopFenced });
  graphFlags.current = { dirty, readOnly, stopFenced };
  const liveSelection = useRef({ graph, status, binding });
  liveSelection.current = { graph, status, binding };
  const controller = useRef<AbortController | null>(null);
  const inFlight = useRef<number | null>(null);
  const sequence = useRef(0);
  const recordRef = useRef<PrimitiveRecord | null>(null);
  const update = (next: PrimitiveRecord): void => { recordRef.current = next; setRecord(next); };
  const cancel = (): void => { generation.current += 1; controller.current?.abort(); controller.current = null; inFlight.current = null; };
  const seal = (current: PrimitiveRecord, fence: 'old_run' | 'unavailable'): void => {
    if (current.error?.kind === 'old_run') return;
    cancel();
    update(primitiveError(current, fence === 'old_run' ? 'old_run' : 'identity_unavailable', fence === 'old_run' ? 'Old deployment run or revision.' : 'IsOS run identity unavailable.'));
  };
  useEffect(() => () => cancel(), []);
  useEffect(() => { cancel(); if (recordRef.current && recordRef.current.bindingPath !== binding?.path) update(primitiveError(recordRef.current, 'old_run', 'Binding selection changed.')); }, [binding?.path]);
  useEffect(() => { const current = recordRef.current; if (!current || current.error?.kind === 'old_run') return;
    const fence = status ? resultFence(current, status) : 'unavailable';
    if (fence !== 'current') seal(current, fence);
  }, [status?.run_id, status?.run_id_state, status?.saved_revision, status?.active_revision, status?.state]);
  useEffect(() => { const current = recordRef.current;
    if (!stopFenced || !current || current.runId !== status?.run_id || current.error?.kind === 'old_run') return;
    cancel(); update(primitiveError(current, 'old_run', 'Deployment Stop requested; result follow-up ended for this run.'));
  }, [stopFenced, status?.run_id]);
  const targetLive = binding && monitorBadge(readings[binding.targetPath], clock) === 'live';
  const canInvoke = Boolean(visible && binding && targetLive && graph && sendFence(graph.revision, status, dirty, readOnly) && !stopFenced && !isStopFenced?.() && status?.run_id !== stoppedRunId && value.type === binding.mode && movementValid(value, binding.target) && record?.phase !== 'sending' && !stopping);
  const follow = async (manual = false): Promise<void> => {
    const current = recordRef.current;
    if (!current || stopFenced || isStopFenced?.() || inFlight.current !== null || !current.submitAttempted || current.phase === 'sending' || current.phase === 'terminal' || (current.result && terminalResult(current.result.status)) || current.error?.kind === 'old_run' ||
      (!manual && !pollAllowed(current, Date.now(), document.visibilityState === 'visible' && visible, false))) return;
    const token = generation.current;
    inFlight.current = token;
    const abort = new AbortController(); controller.current = abort;
    try {
      const fresh = await getProcessStatus(abort.signal);
      if (token !== generation.current || abort.signal.aborted || isStopFenced?.()) return;
      const fence = resultFence(current, fresh);
      if (fence !== 'current') { update(primitiveError(current, fence === 'old_run' ? 'old_run' : 'identity_unavailable', fence === 'old_run' ? 'Old deployment run or revision.' : 'IsOS run identity unavailable.')); return; }
      const result = await readPrimitiveResult(current.commandId, current.activeRevision, abort.signal);
      if (token !== generation.current || abort.signal.aborted || isStopFenced?.() || recordRef.current?.commandId !== current.commandId) return;
      const afterRead = await getProcessStatus(abort.signal);
      if (token !== generation.current || abort.signal.aborted || isStopFenced?.() || recordRef.current?.commandId !== current.commandId) return;
      const afterFence = resultFence(current, afterRead);
      if (afterFence !== 'current') { update(primitiveError(recordRef.current, afterFence === 'old_run' ? 'old_run' : 'identity_unavailable', afterFence === 'old_run' ? 'Old deployment run or revision.' : 'IsOS run identity unavailable.')); return; }
      update(acceptPrimitiveResult({ ...recordRef.current, attempts: recordRef.current.attempts + 1 }, result));
    } catch (error) {
      if (token === generation.current && !abort.signal.aborted && recordRef.current?.commandId === current.commandId) { const safe = errorText(error); update(primitiveError({ ...recordRef.current, attempts: recordRef.current.attempts + 1 }, safe.kind, safe.message)); }
    } finally { if (inFlight.current === token) inFlight.current = null; if (controller.current === abort) controller.current = null; }
  };
  useEffect(() => {
    if (!record || stopFenced || isStopFenced?.() || record.phase === 'sending' || record.phase === 'terminal' || record.phase === 'paused' || record.error?.kind === 'old_run' || record.error?.kind === 'identity_unavailable' || !record.submitAttempted) return;
    if (record.attempts >= 12 || Date.now() - record.startedAt >= 60_000) { update({ ...record, phase: 'paused' }); return; }
    const delay = record.phase === 'unavailable' ? Math.min(10_000, 1000 * 2 ** Math.min(record.attempts, 4)) : 1000;
    const timer = window.setTimeout(() => { if (document.visibilityState === 'visible' && visible) void follow(); }, delay);
    const onVisible = (): void => { if (document.visibilityState === 'visible' && visible) { if (Date.now() - record.startedAt >= 60_000) update({ ...record, phase: 'paused' }); else void follow(); } };
    document.addEventListener('visibilitychange', onVisible);
    return () => { window.clearTimeout(timer); document.removeEventListener('visibilitychange', onVisible); };
  }, [record, stopFenced, visible]);
  const invoke = async (): Promise<void> => {
    if (!canInvoke || !binding || !graph) return;
    cancel(); const token = generation.current; const abort = new AbortController(); controller.current = abort;
    const commandId = `ui-${crypto.randomUUID()}`;
    const fresh = newPrimitiveRecord(status!.run_id!, graph.revision, binding.path, commandId, token);
    update(fresh);
    try {
      const [saved, process] = await Promise.all([getNodeGraph(), getProcessStatus(abort.signal)]);
      if (token !== generation.current || graphFlags.current.stopFenced || isStopFenced?.() || !sendFence(saved.revision, process, graphFlags.current.dirty, graphFlags.current.readOnly) || saved.revision !== fresh.activeRevision || !resolvePrimitiveBindings(saved.graph).some((candidate) => candidate.path === binding.path && candidate.targetPath === binding.targetPath && candidate.mode === binding.mode && candidate.primitiveName === binding.primitiveName)) throw new ResultClientError('validation', 'Active graph or run changed.');
      const beforeSend = await getProcessStatus(abort.signal);
      if (token !== generation.current || abort.signal.aborted || graphFlags.current.dirty || graphFlags.current.readOnly || graphFlags.current.stopFenced || isStopFenced?.() || resultFence(fresh, beforeSend) !== 'current') throw new ResultClientError('validation', 'Active run changed before send.');
      const eosTime = await eosMonotonicTime(abort.signal);
      const live = liveSelection.current;
      if (token !== generation.current || abort.signal.aborted || graphFlags.current.dirty || graphFlags.current.readOnly || graphFlags.current.stopFenced || isStopFenced?.() || recordRef.current?.commandId !== commandId || resultFence(fresh, beforeSend) !== 'current' || live.graph?.revision !== fresh.activeRevision || !live.status || resultFence(fresh, live.status) !== 'current' || live.binding?.path !== binding.path || live.binding.targetPath !== binding.targetPath || live.binding.mode !== binding.mode || live.binding.primitiveName !== binding.primitiveName) throw new ResultClientError('validation', 'Active run changed before send.');
      sequence.current += 1;
      const result = await sendPrimitive(binding, fresh.activeRevision, commandId, sequence.current, eosTime, value, abort.signal,
        () => update({ ...fresh, submitAttempted: true }));
      const current = recordRef.current;
      if (token === generation.current && !abort.signal.aborted && current?.commandId === commandId && current.phase === 'sending') update(acceptPrimitiveResult(current, result));
    } catch (error) { const current = recordRef.current; if (token === generation.current && !abort.signal.aborted && current?.commandId === commandId && current.phase === 'sending') { const safe = errorText(error); update(primitiveError(current, safe.kind, safe.message)); } }
    finally { if (controller.current === abort) controller.current = null; }
  };
  const stop = async (): Promise<void> => {
    const before = status;
    onStopIntent?.(before?.run_id || null);
    setStoppedRunId(before?.run_id || null);
    if (recordRef.current) seal(recordRef.current, 'old_run'); else cancel();
    setStopping(true); setStopResult({ text: c.stopping, alert: false });
    try {
      const result = await stopDeploymentConverged();
      const after = await getProcessStatus().catch(() => null);
      const feedback = stopReplyFeedback(result, before, lang, after); setStopResult(feedback); onStopOutcome?.(feedback, result.terminal?.run_id || null);
      if (after) onStop(after);
    } catch (error) { const feedback = stopFailureFeedback(error, lang); setStopResult(feedback); onStopOutcome?.(feedback); }
    finally { setStopping(false); }
  };
  return <article className="eos-control__card eos-control__card--typed"><h3>{c.title}</h3>
    {binding ? <><p>{lang === 'cn' ? '绑定类型描述' : 'Binding type description'}: {typeDescriptions?.[binding.node.node_type] || binding.node.node_type}</p><p>{lang === 'cn' ? '绑定实例描述' : 'Binding instance description'}: {binding.node.description || '—'}</p><p>{lang === 'cn' ? '目标类型描述' : 'Target type description'}: {typeDescriptions?.[binding.target.node_type] || binding.target.node_type}</p><p>{lang === 'cn' ? '目标实例描述' : 'Target instance description'}: {binding.target.description || '—'}</p><p>{lang === 'cn' ? '原语' : 'Primitive'}: <code>{binding.primitiveName}@{binding.primitiveVersion}</code> · {binding.mode}</p><p>{c.binding}: <code>{binding.path}</code></p><p>{c.target}: <code>{binding.targetPath}</code></p>
      <MovementEditor lang={lang} hardware={binding.target} value={value} onChange={setValue} />
      {(value.type !== binding.mode || !movementValid(value, binding.target)) && <p role="alert">{c.invalid}</p>}</> : <p>{c.missing}</p>}
    {!canInvoke && <p role="status" className="eos-control__warning">{c.gated}</p>}
    <div className="eos-control__actions"><button type="button" className="eos-control__primary" disabled={!canInvoke} onClick={() => void invoke()}>{c.invoke}</button><button type="button" disabled={stopping} onClick={() => void stop()}>{stopping ? c.stopping : c.stop}</button><button type="button" onClick={onOpenLogs}>{c.logs}</button></div>
    {stopResult && <p role={stopResult.alert ? 'alert' : 'status'}>{c.stopStatus}: {stopResult.text}</p>}
    {record && <div className="eos-control__result" role={record.result?.status === 'Failed' || record.result?.status === 'Rejected' || record.phase === 'unavailable' ? 'alert' : 'status'}><p>{c.command}: <code>{record.commandId}</code></p><p>{c.binding}: <code>{record.bindingPath}</code></p><p>{c.revision}: <code>{record.activeRevision}</code></p><p>{c.result}: <strong>{record.result?.status || (record.phase === 'sending' ? c.sending : c.unavailable)}</strong>{record.result?.reason ? ` — ${record.result.reason}` : ''}</p>{record.phase === 'paused' && <p>{c.paused}</p>}{record.phase === 'unavailable' && record.result && <p>{record.error?.kind === 'old_run' ? c.oldRun : c.unavailable}</p>}{record.error && <p>{record.error.message}</p>}<p>{c.updated}: {new Date(record.updatedAt).toLocaleTimeString()}</p>{record.submitAttempted && ['following', 'paused', 'unavailable'].includes(record.phase) && !(record.result && terminalResult(record.result.status)) && record.error?.kind !== 'old_run' && <button type="button" onClick={() => void follow(true)}>{c.refresh}</button>}</div>}
  </article>;
}
