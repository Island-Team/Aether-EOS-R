// @vitest-environment jsdom
import React, { act } from 'react';
import { expect, it, vi } from 'vitest';

const mocks = vi.hoisted(() => ({ stop: vi.fn(), status: vi.fn(), logs: vi.fn() }));
vi.mock('@/components/isos/deployment-control', () => ({ stopDeploymentConverged: mocks.stop, deploymentStopEpoch: () => 0, runDeployment: vi.fn() }));
vi.mock('@/components/isos/EosWorkbench', async () => {
  const React = await import('react');
  return { EosWorkbench: ({ onStatus }: { onStatus: (value: unknown) => void }) => {
    React.useEffect(() => onStatus({ state: 'running', run_id: 'r7', run_id_state: 'known', saved_revision: 'a'.repeat(64), active_revision: 'a'.repeat(64), processes: [] }), [onStatus]);
    return <div>Selected EOS workbench</div>;
  } };
});
vi.mock('@/components/isos/NodeGraphManager', () => ({ NodeGraphManager: () => <div>Selected graph</div> }));
vi.mock('@/services/isos/node-graph', async (importOriginal) => {
  const original = await importOriginal<typeof import('@/services/isos/node-graph')>();
  return { ...original, getProcessStatus: mocks.status, getCurrentLogPage: mocks.logs };
});

it('Q01 mounted workspace keeps owner Stop reachable when Logs is closed', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  vi.stubGlobal('ISOSUI_API_ORIGIN', 'http://127.0.0.1:16000');
  vi.stubGlobal('matchMedia', () => ({ matches: false, addEventListener: () => undefined, removeEventListener: () => undefined }));
  window.history.replaceState({}, '', '/en/isos?logs=1&page=eos');
  mocks.status.mockResolvedValue({ state: 'running', run_id: 'r7', run_id_state: 'known', saved_revision: 'a'.repeat(64), active_revision: 'a'.repeat(64), processes: [] });
  mocks.logs.mockResolvedValue({ scope: { kind: 'deployment_run', run_id: 'r7', active_revision: 'a'.repeat(64) }, anchor: { kind: 'first_ready_or_failure', state: 'pending', event: null }, lines: [], next_cursor: 'lc1.stub', rotated: false, diagnostics: [] });
  let settle!: (value: unknown) => void;
  mocks.stop.mockImplementation(() => new Promise((resolve) => { settle = resolve; }));
  document.body.innerHTML = '<div id="root"></div>';
  await act(async () => { await import('@/index'); });
  const toolbar = document.querySelector('[role="toolbar"][aria-label="Workspace panels"]')!;
  const logsButton = [...toolbar.querySelectorAll('button')].find((button) => button.textContent === 'Logs')!;
  expect(logsButton.getAttribute('aria-pressed')).toBe('true');
  await act(async () => logsButton.click());
  expect(logsButton.getAttribute('aria-pressed')).toBe('false');
  expect(document.querySelector('.node-graph-logs')).toBeNull();
  const safety = document.querySelector('[role="region"][aria-label="Connection"]')!;
  const stop = [...safety.querySelectorAll('button')].find((button) => button.textContent === 'Stop deployment')!;
  expect(stop).toBeTruthy();
  await act(async () => stop.click());
  expect(mocks.stop).toHaveBeenCalledTimes(1);
  expect(safety.textContent).toContain('Stop requested; waiting for IsOS confirmation.');
  expect(logsButton.getAttribute('aria-pressed')).toBe('false');
  await act(async () => settle({ stopped: ['eos'], terminal: { run_id: 'r7', outcome: 'confirmed', reason_code: null, processes: [{ id: 'eos', outcome: 'confirmed', reason_code: null }] } }));
  expect(safety.textContent).toContain('Stop confirmed: r7');
  vi.unstubAllGlobals();
});
