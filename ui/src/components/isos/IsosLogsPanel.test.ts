import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';
vi.mock('../../services/isos/api-client', () => ({ request: vi.fn() }));
import { appendLogPage, IsosLogsPanel, visibleLogLines } from './IsosLogsPanel';
import type { LogLine } from '../../services/isos/node-graph';

const line = (level: string): LogLine => ({ timestamp: '2026-10-31T00:00:00Z', source: 'isos', level, message: level });

describe('IsOS log panel', () => {
  it('offers all sources and an informational display threshold in both languages', () => {
    const en = renderToStaticMarkup(React.createElement(IsosLogsPanel, { lang: 'en', onClose: () => undefined }));
    const cn = renderToStaticMarkup(React.createElement(IsosLogsPanel, { lang: 'cn', onClose: () => undefined }));
    expect(en).toContain('<option value="all" selected="">All sources</option>');
    expect(en).toContain('<option value="browser">Local browser logs</option>');
    expect(cn).toContain('<option value="browser">浏览器本地日志</option>');
    expect(en).toContain('<option value="info" selected="">Info and above</option>');
    expect(cn).toContain('所有来源');
    expect(cn).toContain('<option value="info" selected="">信息及以上</option>');
  });

  it('includes the selected managed process even when it is not a predefined source', () => {
    const html = renderToStaticMarkup(React.createElement(IsosLogsPanel, { lang: 'en', initialSource: 'eos-core', onClose: () => undefined }));
    expect(html).toContain('<option value="eos-core" selected="">eos-core</option>');
  });

  it('lists running process IDs as selectable sources in the unified view', () => {
    const html = renderToStaticMarkup(React.createElement(IsosLogsPanel, { lang: 'cn', sources: ['eos-core', 'egia-core'], onClose: () => undefined }));
    expect(html).toContain('<option value="all" selected="">所有来源</option>');
    expect(html).toContain('<option value="eos-core">eos-core</option>');
    expect(html).toContain('<option value="egia-core">egia-core</option>');
  });

  it('filters displayed severity without discarding the buffered stream', () => {
    const lines = ['trace', 'debug', 'info', 'warn', 'warning', 'error', 'unrecognized'].map(line);
    expect(visibleLogLines(lines, 'warn').map((entry) => entry.level)).toEqual(['warn', 'warning', 'error', 'unrecognized']);
    expect(visibleLogLines(lines, 'error').map((entry) => entry.level)).toEqual(['error']);
    expect(visibleLogLines(lines, 'info')).toHaveLength(5);
    expect(lines).toHaveLength(7);
  });

  it('keeps retained entries on rotation and bounds the panel', () => {
    expect(appendLogPage([line('info')], { lines: [line('error')], next_cursor: 'next', rotated: true })).toEqual([line('info'), line('error')]);
    expect(appendLogPage(Array.from({ length: 1000 }, () => line('info')), { lines: [line('error')], next_cursor: 'next', rotated: false })).toHaveLength(1000);
  });

  it('renders separate local browser logs and an unfiltered first-event region in both languages', () => {
    const en = renderToStaticMarkup(React.createElement(IsosLogsPanel, { lang: 'en', onClose: () => undefined }));
    const cn = renderToStaticMarkup(React.createElement(IsosLogsPanel, { lang: 'cn', onClose: () => undefined }));
    expect(en).toContain('Local browser logs');
    expect(en).toContain('Subsequent logs');
    expect(en).toContain('Jump to first event');
    expect(cn).toContain('浏览器本地日志');
    expect(cn).toContain('后续日志');
    expect(cn).toContain('跳转到首条事件');
  });
});
