import { describe, expect, it } from 'vitest';
import type { ProcessStatus } from '@/services/isos/node-graph';
import { processRunFeedback } from './process-run-feedback';

const status = (state: ProcessStatus['state'], last_error?: string): ProcessStatus => ({ schema_version: '1.0.0', state, last_error, processes: [] });

describe('process Run feedback', () => {
  it('reports degraded state and the server error instead of success', () => {
    expect(processRunFeedback(status('degraded', 'EOS exited'), 'run', 'en')).toBe('EOS exited');
    expect(processRunFeedback(status('degraded'), 'restart', 'cn')).toBe('进程状态异常。');
  });
  it('reports success for a running revision', () => {
    expect(processRunFeedback(status('running'), 'run', 'en')).toBe('Current graph started.');
  });
});
