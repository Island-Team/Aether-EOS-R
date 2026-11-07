import type { Language } from '../../types';
import type { ProcessStatus, StopControlReply, StopTerminal } from '../../services/isos/node-graph';
import { IsosApiError } from '../../services/isos/api-client';

const reasons = new Set([
  'child_exit_unexpected', 'stop_signal_failed', 'stop_deadline_exceeded', 'stop_forced_kill',
  'stop_group_unreaped', 'eos_stop_unconfirmed', 'eos_receipt_missing', 'eos_receipt_invalid',
  'eos_receipt_late', 'control_grant_retire_failed', 'stop_result_persist_failed', 'stop_interrupted',
]);
const runId = (value: unknown): value is string => typeof value === 'string' && /^[A-Za-z0-9_-]{1,64}$/.test(value);
const processId = (value: unknown): value is string => typeof value === 'string' && /^[A-Za-z0-9_-]{1,48}$/.test(value);
const object = (value: unknown): value is Record<string, unknown> => value !== null && typeof value === 'object' && !Array.isArray(value);
const reason = (value: unknown): value is string => typeof value === 'string' && reasons.has(value);

function validTerminal(value: unknown): value is StopTerminal {
  if (!object(value) || !runId(value.run_id) || !Array.isArray(value.processes)) return false;
  if (value.outcome === 'confirmed' && value.reason_code !== null) return false;
  if (value.outcome === 'unknown' && !reason(value.reason_code)) return false;
  if (value.outcome !== 'confirmed' && value.outcome !== 'unknown') return false;
  if (!value.processes.every((item: unknown) => object(item) && processId(item.id) &&
    ((item.outcome === 'confirmed' && item.reason_code === null) || (item.outcome === 'unknown' && reason(item.reason_code))))) return false;
  if (new Set(value.processes.map((item: { id: string }) => item.id)).size !== value.processes.length) return false;
  return value.outcome !== 'confirmed' || value.processes.every((item: { outcome: string }) => item.outcome === 'confirmed');
}

export interface StopFeedback { text: string; alert: boolean }

export function stopReplyFeedback(reply: StopControlReply, before: ProcessStatus | null, lang: Language, after?: ProcessStatus | null): StopFeedback {
  const c = lang === 'cn' ? {
    confirmed: '停止已确认', unknown: '停止结果未知', unconfirmed: '停止未获确认', noRun: '没有需要停止的受管运行',
  } : { confirmed: 'Stop confirmed', unknown: 'Stop outcome unknown', unconfirmed: 'Stop unconfirmed', noRun: 'No managed run to stop' };
  if (!object(reply) || !Array.isArray(reply.stopped) || !reply.stopped.every(processId) || new Set(reply.stopped).size !== reply.stopped.length) return { text: c.unconfirmed, alert: true };
  const terminal = reply.terminal;
  if (terminal === null && reply.stopped.length === 0 && before?.run_id_state === 'none' && before.run_id == null) return { text: c.noRun, alert: false };
  if (!validTerminal(terminal)) return { text: c.unconfirmed, alert: true };
  const matchedBefore = before?.run_id_state === 'known' && terminal.run_id === before.run_id;
  const matchedStoppedOwner = after?.state === 'stopped' && after.run_id_state === 'known' && after.run_id === terminal.run_id &&
    after.processes.length === 0 && JSON.stringify(after.terminal) === JSON.stringify(terminal);
  if (!matchedBefore && !matchedStoppedOwner) return { text: c.unconfirmed, alert: true };
  if (reply.stopped.some((id) => !terminal.processes.some((process) => process.id === id))) return { text: c.unconfirmed, alert: true };
  if (terminal.outcome === 'unknown') return { text: `${c.unknown}: ${terminal.reason_code}`, alert: true };
  return { text: `${c.confirmed}: ${terminal.run_id}`, alert: false };
}

export function stopFailureFeedback(error: unknown, lang: Language): StopFeedback {
  if (error instanceof IsosApiError) return { text: `${lang === 'cn' ? 'IsOS 拒绝停止' : 'Stop rejected by IsOS'}: ${error.message}`, alert: true };
  return { text: lang === 'cn' ? '停止响应不可用；请检查 IsOS 状态与当前运行日志。' : 'Stop response unavailable; check IsOS status and current run logs.', alert: true };
}
