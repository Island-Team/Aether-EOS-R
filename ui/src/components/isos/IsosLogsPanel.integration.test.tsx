// @vitest-environment jsdom
import React, { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

const event = (event_code: string, run_id: string, source = 'eos') => ({ timestamp: '2026-11-01T10:00:00.000Z', level: 'error', source, core: source === 'packages' ? 'isos' : 'eos', node_path: null, event_code, run_id, correlation_id: null, message: event_code, fields: {}, pid: 42 });
const reply = (data: unknown, status = 200, error: string | null = null) => ({ ok: status < 400, status, json: async () => ({ schema_version: '0.1.0', ok: status < 400, data, error: error ? { code: 'request_failed', message: error, details: data, correlation_id: 'req-1' } : null }) });
const page = (run_id: string, anchorCode: string, lines: ReturnType<typeof event>[] = [], kind: 'manager_session' | 'deployment_run' = 'deployment_run') => ({
  scope: { kind, run_id, active_revision: null },
  anchor: { kind: kind === 'manager_session' ? 'first_prelaunch_failure' : 'first_ready_or_failure', state: 'found', event: event(anchorCode, run_id, kind === 'manager_session' ? 'packages' : 'eos') },
  lines, next_cursor: `lc1.${run_id}`, rotated: false, diagnostics: [],
});

let container: HTMLDivElement;
let root: Root;
beforeEach(() => {
  vi.resetModules();
  vi.useFakeTimers();
  vi.stubGlobal('ISOSUI_API_ORIGIN', 'http://127.0.0.1:16000');
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  Object.defineProperty(document, 'visibilityState', { configurable: true, value: 'visible' });
  container = document.createElement('div');
  document.body.appendChild(container);
  root = createRoot(container);
});
afterEach(async () => {
  await act(async () => root.unmount());
  container.remove();
  vi.useRealTimers();
  vi.unstubAllGlobals();
});

describe('P07 mounted panel with stub IsOS', () => {
  it('pins a manager package failure outside source filters and separates browser logs', async () => {
    const fetchMock = vi.fn(async (url: string) => url.includes('/process-status')
      ? reply({ state: 'stopped', run_id: null, run_id_state: 'none', processes: [] })
      : reply(page('m7', 'package_load_failed', [], 'manager_session')));
    vi.stubGlobal('fetch', fetchMock);
    const { IsosLogsPanel } = await import('./IsosLogsPanel');
    await act(async () => root.render(<IsosLogsPanel lang="en" onClose={() => undefined} initialSource="eos" />));
    expect(container.textContent).toContain('package_load_failed');
    expect(container.textContent).toContain('manager_session: m7');
    expect(container.textContent).toContain('Live status: stopped');
    expect(container.querySelector('.node-graph-logs__browser')).toBeNull();
    const logUrl = fetchMock.mock.calls.find(([url]) => url.includes('/logs'))?.[0];
    expect(new URL(logUrl!).searchParams.get('view')).toBe('current');
    expect(new URL(logUrl!).searchParams.get('source')).toBe('eos');
  });

  it('keeps the opaque same-run cursor while collapsed and rechecks status on reopen', async () => {
    const urls: string[] = [];
    const fetchMock = vi.fn(async (url: string) => {
      urls.push(url);
      return url.includes('/process-status') ? reply({ state: 'running', run_id: 'r7', run_id_state: 'known', processes: [] }) : reply(page('r7', 'eos_ready'));
    });
    vi.stubGlobal('fetch', fetchMock);
    const { IsosLogsPanel } = await import('./IsosLogsPanel');
    await act(async () => root.render(<IsosLogsPanel lang="en" visible onClose={() => undefined} />));
    expect(urls.filter((url) => url.includes('/logs'))).toHaveLength(1);
    await act(async () => root.render(<IsosLogsPanel lang="en" visible={false} onClose={() => undefined} />));
    await act(async () => { await vi.advanceTimersByTimeAsync(5000); });
    expect(urls.filter((url) => url.includes('/logs'))).toHaveLength(1);
    await act(async () => root.render(<IsosLogsPanel lang="en" visible onClose={() => undefined} />));
    const logs = urls.filter((url) => url.includes('/logs'));
    expect(logs).toHaveLength(2);
    expect(new URL(logs[1]).searchParams.get('cursor')).toBe('lc1.r7');
    expect(urls.filter((url) => url.includes('/process-status'))).toHaveLength(2);
    await act(async () => root.render(<></>));
    await act(async () => root.render(<IsosLogsPanel lang="en" visible onClose={() => undefined} />));
    const reopened = urls.filter((url) => url.includes('/logs'));
    expect(reopened).toHaveLength(3);
    expect(new URL(reopened[2]).searchParams.has('cursor')).toBe(false);
    expect(urls.filter((url) => url.includes('/process-status'))).toHaveLength(3);
  });

  it('keeps retained rows on rotation, shows gap, then clears them on a new run', async () => {
    const pages = [
      page('r7', 'eos_ready', [event('deployment_ready', 'r7')]),
      { ...page('r7', 'eos_ready', [event('deployment_stopped', 'r7')]), rotated: true, diagnostics: [{ code: 'log_cursor_expired', count: null }] },
      { ...page('r8', 'eos_start_failed'), diagnostics: [{ code: 'log_run_changed', count: null }] },
    ];
    const fetchMock = vi.fn(async (url: string) => url.includes('/process-status')
      ? reply({ state: 'stopped', run_id: 'r7', run_id_state: 'known', processes: [] })
      : reply(pages.shift()));
    vi.stubGlobal('fetch', fetchMock);
    const { IsosLogsPanel } = await import('./IsosLogsPanel');
    await act(async () => root.render(<IsosLogsPanel lang="en" onClose={() => undefined} />));
    expect(container.textContent).toContain('deployment_ready');
    await act(async () => { await vi.advanceTimersByTimeAsync(1200); });
    expect(container.textContent).toContain('deployment_stopped');
    expect(container.textContent).toContain('log_cursor_expired');
    await act(async () => { await vi.advanceTimersByTimeAsync(1200); });
    expect(container.textContent).toContain('r7 → r8');
    expect(container.textContent).toContain('eos_start_failed');
    expect(container.textContent).not.toContain('deployment_stopped');
  });

  it('retains rows through collector loss, backs off for 30 seconds, then resumes', async () => {
    let logCalls = 0;
    const fetchMock = vi.fn(async (url: string) => {
      if (url.includes('/process-status')) return reply({ state: 'running', run_id: 'r7', run_id_state: 'known', processes: [] });
      logCalls += 1;
      if (logCalls === 2) throw new Error('collector offline');
      return reply(page('r7', 'eos_ready', logCalls === 1 ? [event('deployment_ready', 'r7')] : [event('deployment_stopped', 'r7')]));
    });
    vi.stubGlobal('fetch', fetchMock);
    const { IsosLogsPanel } = await import('./IsosLogsPanel');
    await act(async () => root.render(<IsosLogsPanel lang="en" onClose={() => undefined} />));
    await act(async () => { await vi.advanceTimersByTimeAsync(1200); });
    expect(container.textContent).toContain('deployment_ready');
    expect(container.textContent).toContain('Cannot reach IsOS');
    await act(async () => { await vi.advanceTimersByTimeAsync(29_999); });
    expect(logCalls).toBe(2);
    await act(async () => { await vi.advanceTimersByTimeAsync(1); });
    expect(logCalls).toBe(3);
    expect(container.textContent).toContain('deployment_stopped');
    expect(container.textContent).not.toContain('Cannot reach IsOS');
    expect(fetchMock.mock.calls.filter(([url]) => url.includes('/process-status'))).toHaveLength(2);
  });

  it('marks old rows stale on typed 503 without relabeling them as current', async () => {
    let logCalls = 0;
    const fetchMock = vi.fn(async (url: string) => {
      if (url.includes('/process-status')) return reply({ state: 'failed', run_id: null, run_id_state: 'unavailable', processes: [] });
      logCalls += 1;
      if (logCalls === 1) return reply(page('r7', 'eos_ready', [event('deployment_ready', 'r7')]));
      return reply({ diagnostics: [{ path: '/scope', code: 'log_scope_unavailable', message: 'Current log scope unavailable' }] }, 503, 'log scope unavailable');
    });
    vi.stubGlobal('fetch', fetchMock);
    const { IsosLogsPanel } = await import('./IsosLogsPanel');
    await act(async () => root.render(<IsosLogsPanel lang="en" onClose={() => undefined} />));
    expect(container.textContent).toContain('deployment_ready');
    await act(async () => { await vi.advanceTimersByTimeAsync(1200); });
    expect(container.textContent).toContain('Current log scope unavailable');
    expect(container.textContent).toContain('deployment_ready');
    expect(container.textContent).toContain('Previous scope (stale, not current): r7');
    expect(container.textContent).not.toContain('deployment_run: r7');
  });

  it('drops a rejected opaque cursor and retries the same run from its anchor', async () => {
    let logCalls = 0;
    const fetchMock = vi.fn(async (url: string) => {
      if (url.includes('/process-status')) return reply({ state: 'stopped', run_id: 'r7', run_id_state: 'known', processes: [] });
      logCalls += 1;
      if (logCalls === 2) return reply({ diagnostics: [{ path: '/query/cursor', code: 'log_cursor_invalid', message: 'Cursor invalid' }] }, 400, 'Cursor invalid');
      return reply(page('r7', 'eos_ready', logCalls === 1 ? [event('deployment_ready', 'r7')] : [event('deployment_stopped', 'r7')]));
    });
    vi.stubGlobal('fetch', fetchMock);
    const { IsosLogsPanel } = await import('./IsosLogsPanel');
    await act(async () => root.render(<IsosLogsPanel lang="en" onClose={() => undefined} />));
    await act(async () => { await vi.advanceTimersByTimeAsync(1200); });
    expect(container.textContent).toContain('Cursor invalid');
    await act(async () => { await vi.advanceTimersByTimeAsync(30_000); });
    const urls = fetchMock.mock.calls.map(([url]) => url).filter((url) => url.includes('/logs'));
    expect(urls).toHaveLength(3);
    expect(new URL(urls[1]).searchParams.get('cursor')).toBe('lc1.r7');
    expect(new URL(urls[2]).searchParams.has('cursor')).toBe(false);
    expect(container.textContent).toContain('deployment_stopped');
  });

  it('ignores a late pre-publication log response and immediately rechecks owner status and scope', async () => {
    let releaseOld!: (value: ReturnType<typeof reply>) => void;
    const oldLog = new Promise<ReturnType<typeof reply>>((resolve) => { releaseOld = resolve; });
    let statusCalls = 0;
    let logCalls = 0;
    const fetchMock = vi.fn((url: string, _options?: RequestInit) => {
      if (url.includes('/process-status')) {
        statusCalls += 1;
        return Promise.resolve(reply({ state: 'stopped', run_id: statusCalls === 1 ? 'r7' : 'r8', run_id_state: 'known', processes: [] }));
      }
      logCalls += 1;
      return logCalls === 1 ? oldLog : Promise.resolve(reply(page('r8', 'eos_start_failed')));
    });
    vi.stubGlobal('fetch', fetchMock);
    const { IsosLogsPanel } = await import('./IsosLogsPanel');
    await act(async () => root.render(<IsosLogsPanel lang="en" onClose={() => undefined} />));
    expect(logCalls).toBe(1);
    await act(async () => window.dispatchEvent(new Event('isosui:log-scope-recheck')));
    expect(statusCalls).toBeGreaterThanOrEqual(2);
    expect(logCalls).toBe(2);
    expect(container.textContent).toContain('eos_start_failed');
    const oldOptions = fetchMock.mock.calls.find(([url]) => url.includes('/logs'))?.[1];
    expect(oldOptions?.signal?.aborted).toBe(true);
    await act(async () => releaseOld(reply(page('r7', 'eos_ready', [event('old_detail', 'r7')]))));
    expect(container.textContent).toContain('eos_start_failed');
    expect(container.textContent).not.toContain('old_detail');
    expect(container.textContent).not.toContain('deployment_run: r7');
  });

  it('announces a pending anchor when it becomes found exactly once', async () => {
    const pending = { ...page('r7', 'eos_ready'), anchor: { kind: 'first_ready_or_failure', state: 'pending', event: null }, lines: [] };
    const pages = [pending, page('r7', 'eos_ready'), page('r7', 'eos_ready'), page('r7', 'eos_ready')];
    vi.stubGlobal('fetch', vi.fn(async (url: string) => url.includes('/process-status')
      ? reply({ state: 'running', run_id: 'r7', run_id_state: 'known', processes: [] })
      : reply(pages.shift())));
    const { IsosLogsPanel } = await import('./IsosLogsPanel');
    await act(async () => root.render(<IsosLogsPanel lang="en" onClose={() => undefined} />));
    expect(container.textContent).toContain('Waiting for the first ready or failure event');
    expect(container.textContent).not.toContain('First event is available in this scope');
    await act(async () => { await vi.advanceTimersByTimeAsync(1200); });
    const announcement = [...container.querySelectorAll('[aria-live="polite"]')].find((item) => item.textContent === 'First event is available in this scope');
    expect(announcement).toBeTruthy();
    await act(async () => { await vi.advanceTimersByTimeAsync(1200); });
    expect([...container.querySelectorAll('[aria-live="polite"]')].filter((item) => item.textContent === 'First event is available in this scope')).toEqual([announcement]);
    await act(async () => root.render(<IsosLogsPanel lang="cn" onClose={() => undefined} />));
    expect(container.textContent).toContain('本次首条事件已可查看');
    expect([...container.querySelectorAll('[aria-live="polite"]')].filter((item) => item.textContent === '本次首条事件已可查看')).toHaveLength(1);
  });

  it('uses distinct English and Chinese run identity unavailable copy without claiming anchor loss', async () => {
    vi.stubGlobal('fetch', vi.fn(async (url: string) => url.includes('/process-status')
      ? reply({ state: 'failed', run_id: null, run_id_state: 'unavailable', processes: [] })
      : reply({ diagnostics: [{ path: '/scope', code: 'log_scope_unavailable', message: 'Scope unavailable' }] }, 503, 'Scope unavailable')));
    const { IsosLogsPanel } = await import('./IsosLogsPanel');
    await act(async () => root.render(<IsosLogsPanel lang="en" onClose={() => undefined} />));
    expect(container.textContent).toContain('IsOS cannot confirm the current run identity');
    expect(container.textContent).not.toContain('First event unavailable; a log gap was detected');
    await act(async () => root.render(<IsosLogsPanel lang="cn" onClose={() => undefined} />));
    expect(container.textContent).toContain('IsOS 无法确认当前运行身份');
    expect(container.textContent).not.toContain('首条事件不可用；已检测到日志缺口');
  });

  it('keeps an acknowledged same-run gap dismissed across repeated owner pages', async () => {
    const gapPage = { ...page('r7', 'eos_ready'), diagnostics: [{ code: 'log_collector_gap', count: 4 }] };
    vi.stubGlobal('fetch', vi.fn(async (url: string) => url.includes('/process-status')
      ? reply({ state: 'running', run_id: 'r7', run_id_state: 'known', processes: [] })
      : reply(gapPage)));
    const { IsosLogsPanel } = await import('./IsosLogsPanel');
    await act(async () => root.render(<IsosLogsPanel lang="en" onClose={() => undefined} />));
    expect(container.textContent).toContain('log_collector_gap (4)');
    const acknowledge = [...container.querySelectorAll('button')].find((item) => item.textContent === 'Acknowledge gap');
    await act(async () => acknowledge?.dispatchEvent(new MouseEvent('click', { bubbles: true })));
    expect(container.textContent).not.toContain('log_collector_gap (4)');
    await act(async () => { await vi.advanceTimersByTimeAsync(1200); });
    expect(container.textContent).not.toContain('log_collector_gap (4)');
  });
});
