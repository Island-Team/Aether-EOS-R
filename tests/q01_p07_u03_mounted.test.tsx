// @vitest-environment jsdom
import React, { act } from 'react';
import { createRoot } from 'react-dom/client';
import { afterEach, expect, it, vi } from 'vitest';

const row = (code: string) => ({ timestamp: '2026-11-01T10:00:00.000Z', level: 'info', source: 'eos', core: 'eos', node_path: null, event_code: code, run_id: 'r7', correlation_id: null, message: code, fields: {}, pid: 42 });
const page = (cursor: string, lines: ReturnType<typeof row>[], diagnostics: { code: string; count: number | null }[] = []) => ({
  scope: { kind: 'deployment_run', run_id: 'r7', active_revision: null },
  anchor: { kind: 'first_ready_or_failure', state: 'found', event: row('eos_ready') },
  lines, next_cursor: cursor, rotated: diagnostics.length > 0, diagnostics,
});
const reply = (data: unknown) => ({ ok: true, status: 200, json: async () => ({ schema_version: 1, ok: true, data, error: null }) });

afterEach(() => { vi.useRealTimers(); vi.unstubAllGlobals(); });

it('Q01 mounted logs preserve same-scope cursor and rows through outage and show an authenticated expiry gap', async () => {
  vi.useFakeTimers();
  vi.stubGlobal('ISOSUI_API_ORIGIN', 'http://127.0.0.1:16000');
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  Object.defineProperty(document, 'visibilityState', { configurable: true, value: 'visible' });
  const urls: string[] = [];
  let statusCalls = 0;
  let logCalls = 0;
  const fetchMock = vi.fn(async (url: string) => {
    urls.push(url);
    if (url.includes('/process-status')) {
      statusCalls += 1;
      return reply({ state: 'running', run_id: 'r7', run_id_state: 'known', processes: [] });
    }
    logCalls += 1;
    if (logCalls === 2) throw new Error('collector offline');
    if (logCalls === 1) return reply(page('lc1.first', [row('deployment_ready')]));
    return reply(page('lc1.resumed', [row('deployment_stopped')], [{ code: 'log_cursor_expired', count: null }]));
  });
  vi.stubGlobal('fetch', fetchMock);
  const container = document.createElement('div'); document.body.appendChild(container);
  const root = createRoot(container);
  try {
    const { IsosLogsPanel } = await import('./IsosLogsPanel');
    await act(async () => root.render(<IsosLogsPanel lang="en" visible onClose={() => undefined} />));
    expect(container.querySelector('pre[role="log"]')?.textContent?.split('\n').filter((line) => line.includes('deployment_ready'))).toHaveLength(1);
    await act(async () => vi.advanceTimersByTimeAsync(1200));
    expect(container.querySelector('[role="alert"]')?.textContent).toContain('Cannot reach IsOS');
    expect(container.querySelector('pre[role="log"]')?.textContent?.split('\n').filter((line) => line.includes('deployment_ready'))).toHaveLength(1);
    await act(async () => vi.advanceTimersByTimeAsync(30_000));
    const requests = urls.filter((url) => url.includes('/logs'));
    expect(requests).toHaveLength(3);
    expect(new URL(requests[1]).searchParams.get('cursor')).toBe('lc1.first');
    expect(new URL(requests[2]).searchParams.get('cursor')).toBe('lc1.first');
    expect(statusCalls).toBe(2);
    expect(container.querySelector('pre[role="log"]')?.textContent?.split('\n').filter((line) => line.includes('deployment_ready'))).toHaveLength(1);
    expect(container.querySelector('pre[role="log"]')?.textContent).toContain('deployment_stopped');
    expect([...container.querySelectorAll('[role="alert"]')].some((item) => item.textContent?.includes('log_cursor_expired'))).toBe(true);
    await act(async () => root.render(<IsosLogsPanel lang="en" visible={false} onClose={() => undefined} />));
    await act(async () => vi.advanceTimersByTimeAsync(5000));
    expect(urls.filter((url) => url.includes('/logs'))).toHaveLength(3);
  } finally {
    await act(async () => root.unmount());
    container.remove();
  }
});
