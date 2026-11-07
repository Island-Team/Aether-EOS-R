import { EOS_CONTROL_URL } from './control';

export type DispatchStatus = 'Completed' | 'Failed' | 'Cancelled' | 'Rejected' | 'InProgress' | 'OutcomeUnknown';
export interface DispatchResult { command_id: string; status: DispatchStatus; reason: string | null }
export type ResultErrorKind = 'validation' | 'transport' | 'owner' | 'protocol';
export class ResultClientError extends Error {
  constructor(readonly kind: ResultErrorKind, message: string, readonly httpStatus?: number) { super(message); this.name = 'ResultClientError'; }
}
export const validRevision = (value: unknown): value is string => typeof value === 'string' && /^[a-f0-9]{64}$/.test(value);
export const validCommandId = (value: unknown): value is string => typeof value === 'string' && /^[A-Za-z0-9_.:-]+$/.test(value) && new TextEncoder().encode(value).length <= 128;
const statuses: DispatchStatus[] = ['Completed', 'Failed', 'Cancelled', 'Rejected', 'InProgress', 'OutcomeUnknown'];
export const terminalResult = (status: DispatchStatus): boolean => status !== 'InProgress';

export function decodeDispatchResult(reply: unknown, commandId: string): DispatchResult {
  if (!reply || typeof reply !== 'object' || Array.isArray(reply)) throw new ResultClientError('protocol', 'Malformed EOS response.');
  const envelope = reply as Record<string, unknown>;
  if (envelope.ok !== true || !envelope.result || typeof envelope.result !== 'object' || Array.isArray(envelope.result)) throw new ResultClientError('protocol', 'Malformed EOS result.');
  const result = envelope.result as Record<string, unknown>;
  if (result.command_id !== commandId || !statuses.includes(result.status as DispatchStatus) ||
      (result.reason !== null && (typeof result.reason !== 'string' || new TextEncoder().encode(result.reason).length > 512))) {
    throw new ResultClientError('protocol', 'EOS result identity or status is invalid.');
  }
  return { command_id: commandId, status: result.status as DispatchStatus, reason: result.reason as string | null };
}

/** The public bridge boundary never accepts a private grant or an arbitrary target. */
export async function publicControl(body: Record<string, unknown>, signal?: AbortSignal): Promise<unknown> {
  const controller = new AbortController();
  const abort = (): void => controller.abort();
  if (signal?.aborted) controller.abort();
  signal?.addEventListener('abort', abort, { once: true });
  const timeout = setTimeout(abort, 4000);
  let response: Response;
  let text: string;
  try {
    response = await fetch(EOS_CONTROL_URL, { method: 'POST', credentials: 'omit', cache: 'no-store', mode: 'cors',
      targetAddressSpace: 'loopback', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body), signal: controller.signal } as RequestInit);
    text = await response.text();
  } catch { throw new ResultClientError('transport', 'EOS bridge unavailable.'); }
  finally { clearTimeout(timeout); signal?.removeEventListener('abort', abort); }
  if (new TextEncoder().encode(text).length > 4096) throw new ResultClientError('protocol', 'EOS result exceeds size limit.');
  let reply: unknown;
  try { reply = JSON.parse(text); } catch { throw new ResultClientError('protocol', 'Malformed EOS response.'); }
  const envelope = reply as { ok?: unknown; error?: unknown } | null;
  if (!response.ok || envelope?.ok === false) {
    const message = typeof envelope?.error === 'string' ? envelope.error.slice(0, 256) : `EOS request failed (${response.status}).`;
    throw new ResultClientError('owner', message, response.status);
  }
  return reply;
}

export async function readPrimitiveResult(commandId: string, activeRevision: string, signal?: AbortSignal): Promise<DispatchResult> {
  if (!validCommandId(commandId) || !validRevision(activeRevision)) throw new ResultClientError('validation', 'Invalid command ID or active revision.');
  return decodeDispatchResult(await publicControl({ op: 'result', command_id: commandId, active_revision: activeRevision }, signal), commandId);
}
