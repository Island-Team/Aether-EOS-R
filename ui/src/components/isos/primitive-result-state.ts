import type { ProcessStatus } from '../../services/isos/node-graph';
import { terminalResult, type DispatchResult, type ResultErrorKind } from '../../services/eos/primitive-result';

export type BrowserPhase = 'sending' | 'following' | 'terminal' | 'paused' | 'unavailable';
export interface PrimitiveRecord {
  runId: string; activeRevision: string; bindingPath: string; commandId: string;
  result: DispatchResult | null; phase: BrowserPhase; error: { kind: ResultErrorKind | 'old_run' | 'identity_unavailable'; message: string } | null;
  submitAttempted: boolean;
  generation: number; startedAt: number; attempts: number; updatedAt: number;
}
export function newPrimitiveRecord(runId: string, activeRevision: string, bindingPath: string, commandId: string, generation: number, now = Date.now()): PrimitiveRecord {
  return { runId, activeRevision, bindingPath, commandId, generation, result: null, phase: 'sending', error: null, submitAttempted: false, startedAt: now, attempts: 0, updatedAt: now };
}
export function acceptPrimitiveResult(record: PrimitiveRecord, result: DispatchResult, now = Date.now()): PrimitiveRecord {
  if (record.commandId !== result.command_id || record.phase === 'terminal') return record;
  return { ...record, result, phase: terminalResult(result.status) ? 'terminal' : 'following', error: null, updatedAt: now };
}
export function primitiveError(record: PrimitiveRecord, kind: ResultErrorKind | 'old_run' | 'identity_unavailable', message: string, now = Date.now()): PrimitiveRecord {
  return { ...record, phase: 'unavailable', error: { kind, message }, updatedAt: now };
}
export function resultFence(record: PrimitiveRecord, status: ProcessStatus): 'current' | 'old_run' | 'unavailable' {
  if (status.run_id_state !== 'known' || !status.run_id) return 'unavailable';
  if (status.state !== 'running' || status.run_id !== record.runId || status.active_revision !== record.activeRevision || status.saved_revision !== record.activeRevision) return 'old_run';
  return 'current';
}
export function pollAllowed(record: PrimitiveRecord, now: number, visible: boolean, inFlight: boolean): boolean {
  return visible && !inFlight && record.submitAttempted &&
    (record.phase === 'following' || (record.phase === 'unavailable' && record.error?.kind !== 'old_run' && record.error?.kind !== 'identity_unavailable')) &&
    record.attempts < 12 && now - record.startedAt < 60_000;
}
