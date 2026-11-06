import { describe, expect, it } from 'vitest';
import { browserLog, getBrowserLogs, subscribeBrowserLogs } from './browser-logs';

describe('browser log stream', () => {
  it('retains browser events for a log panel opened later and notifies live listeners', () => {
    let updates = 0;
    const unsubscribe = subscribeBrowserLogs(() => { updates += 1; });
    browserLog('warn', 'Graph nodes outside canvas');
    expect(getBrowserLogs()[getBrowserLogs().length - 1]).toMatchObject({ source: 'browser', level: 'warn', message: 'Graph nodes outside canvas' });
    expect(updates).toBe(1);
    unsubscribe();
    browserLog('info', 'Graph shown');
    expect(updates).toBe(1);
  });
});
