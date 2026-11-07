import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';
vi.mock('../../services/isos/api-client', () => ({ request: vi.fn() }));
import { EgiaWorkbench, egiaTabs } from './EgiaWorkbench';

describe('EGIA workbench navigation', () => {
  it('shows overview, logs and graph navigation in both languages', () => {
    expect(egiaTabs('en')).toEqual({ overview: 'Overview', logs: 'Logs' });
    expect(egiaTabs('cn')).toEqual({ overview: '概览', logs: '日志' });
    const html = renderToStaticMarkup(React.createElement(EgiaWorkbench, { lang: 'en', onOpenGraph: () => undefined }));
    expect(html).toContain('Overview');
    expect(html).toContain('Logs');
    expect(html).toContain('Graph');
    expect(html).toContain('No EI process status is available');
  });
});
