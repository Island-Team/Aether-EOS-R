import { expect, it } from 'vitest';
import type { ProcessStatus, StopControlReply, StopTerminal } from '../../services/isos/node-graph';
import { IsosApiError } from '../../services/isos/api-client';
import { stopReplyFeedback, stopFailureFeedback } from './stop-feedback';

const before: ProcessStatus = { schema_version: '1.0.0', state: 'running', run_id: 'run-a', run_id_state: 'known', processes: [] };
const reply = (terminal: unknown): StopControlReply => ({ stopped: ['eos'], terminal } as StopControlReply);
const confirmed: StopTerminal = { run_id: 'run-a', outcome: 'confirmed', reason_code: null, processes: [{ id: 'eos', outcome: 'confirmed', reason_code: null }] };

it('confirms only a complete, run-matched owner terminal', () => {
  expect(stopReplyFeedback(reply(confirmed), before, 'en')).toEqual({ text: 'Stop confirmed: run-a', alert: false });
  expect(stopReplyFeedback(reply({ ...confirmed, run_id: 'run-b' }), before, 'en').alert).toBe(true);
  expect(stopReplyFeedback(reply(confirmed), { ...before, run_id_state: 'unavailable' }, 'en').alert).toBe(true);
  expect(stopReplyFeedback(reply({ ...confirmed, processes: [{ id: 'eos', outcome: 'unknown', reason_code: 'eos_stop_unconfirmed' }] }), before, 'en').alert).toBe(true);
  expect(stopReplyFeedback(reply({ ...confirmed, processes: [confirmed.processes[0], confirmed.processes[0]] }), before, 'en')).toEqual({ text: 'Stop unconfirmed', alert: true });
  expect(stopReplyFeedback(reply({ ...confirmed, processes: [] }), before, 'en')).toEqual({ text: 'Stop unconfirmed', alert: true });
  expect(stopReplyFeedback({ stopped: ['eos', 'eos'], terminal: confirmed }, before, 'en')).toEqual({ text: 'Stop unconfirmed', alert: true });
});

it('confirms the later run only when the final owner status matches its terminal', () => {
  const later = { ...confirmed, run_id: 'run-b' };
  const after = { ...before, state: 'stopped' as const, run_id: 'run-b', processes: [], terminal: later };
  expect(stopReplyFeedback(reply(later), before, 'en', after)).toEqual({ text: 'Stop confirmed: run-b', alert: false });
  expect(stopReplyFeedback(reply(later), before, 'cn', after)).toEqual({ text: '停止已确认: run-b', alert: false });
  expect(stopReplyFeedback(reply(later), before, 'en', { ...after, terminal: confirmed }).alert).toBe(true);
  expect(stopReplyFeedback(reply(later), before, 'en', { ...after, state: 'running' }).alert).toBe(true);
});

it('shows a fixed unknown reason as an alert in both languages', () => {
  const unknown = { ...confirmed, outcome: 'unknown', reason_code: 'eos_stop_unconfirmed', processes: [{ id: 'eos', outcome: 'unknown', reason_code: 'eos_stop_unconfirmed' }] };
  expect(stopReplyFeedback(reply(unknown), before, 'en')).toEqual({ text: 'Stop outcome unknown: eos_stop_unconfirmed', alert: true });
  expect(stopReplyFeedback(reply(unknown), before, 'cn')).toEqual({ text: '停止结果未知: eos_stop_unconfirmed', alert: true });
});

it.each([null, undefined, {}, { ...confirmed, outcome: 'mystery' }, { ...confirmed, reason_code: 'invented' }, { ...confirmed, processes: null }])('leaves a null, absent or malformed terminal unconfirmed', (terminal) => {
  expect(stopReplyFeedback(reply(terminal), before, 'en')).toEqual({ text: 'Stop unconfirmed', alert: true });
});

it('identifies a never-run home without inventing a stop confirmation', () => {
  expect(stopReplyFeedback({ stopped: [], terminal: null }, { ...before, run_id: null, run_id_state: 'none' }, 'en')).toEqual({ text: 'No managed run to stop', alert: false });
  expect(stopReplyFeedback(reply(null), { ...before, run_id: null, run_id_state: 'none' }, 'en')).toEqual({ text: 'Stop unconfirmed', alert: true });
  expect(stopReplyFeedback(reply(null), { ...before, run_id: null, run_id_state: 'unavailable' }, 'en').alert).toBe(true);
});

it('keeps transport failure separate from owner outcomes', () => {
  expect(stopFailureFeedback(new Error('socket closed'), 'en')).toEqual({ text: 'Stop response unavailable; check IsOS status and current run logs.', alert: true });
  expect(stopFailureFeedback(new IsosApiError('stop denied', 403, 'stop_denied'), 'en')).toEqual({ text: 'Stop rejected by IsOS: stop denied', alert: true });
  expect(stopFailureFeedback(new IsosApiError('stop denied', 403), 'cn')).toEqual({ text: 'IsOS 拒绝停止: stop denied', alert: true });
});
