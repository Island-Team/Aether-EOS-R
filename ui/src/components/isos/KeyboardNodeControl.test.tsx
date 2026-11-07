// @vitest-environment jsdom
import React, { act } from 'react';
import { createRoot } from 'react-dom/client';
import { afterEach, expect, it, vi } from 'vitest';
import { KeyboardNodeControl, keyMatches } from './KeyboardNodeControl';

const publish = vi.hoisted(() => vi.fn().mockResolvedValue({}));
vi.mock('@/services/isos/node-events', () => ({ publishKeyState: publish }));

afterEach(() => { vi.useRealTimers(); publish.mockClear(); document.body.innerHTML = ''; });

const node = { id: 'drive_key', node_type: 'isosui.input.keyboard@v0.1', label: 'Drive key', description: '', settings: { key: 'KeyW', modifiers: ['Shift'] } };
function key(type: 'keydown' | 'keyup', code: string, options: KeyboardEventInit = {}): void {
  window.dispatchEvent(new KeyboardEvent(type, { bubbles: true, code, key: code === 'ShiftLeft' ? 'Shift' : 'w', shiftKey: true, ...options }));
}

it('publishes ordered held-key transitions and releases on focus loss', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  vi.useFakeTimers();
  const host = document.createElement('div'); document.body.append(host); const root = createRoot(host);
  await act(async () => root.render(<KeyboardNodeControl node={node} path="isosui/input/keyboard/drive_key" runId="run-a" activeRevision="rev-a" current lang="en" />));
  await act(async () => (host.querySelector('button') as HTMLButtonElement).click());
  await act(async () => key('keydown', 'KeyW'));
  await act(async () => key('keydown', 'KeyW', { repeat: true }));
  await act(async () => { await vi.advanceTimersByTimeAsync(100); });
  await act(async () => window.dispatchEvent(new Event('blur')));
  await act(async () => { await Promise.resolve(); });
  expect(publish.mock.calls.map(([event]) => event.payload.state)).toEqual(['pressed', 'refresh', 'released']);
  expect(publish.mock.calls.map(([event]) => event.payload.sequence)).toEqual([1, 2, 3]);
  expect(publish.mock.calls[0][0]).toMatchObject({ source_node: 'isosui/input/keyboard/drive_key', run_id: 'run-a', active_revision: 'rev-a' });
  await act(async () => root.unmount());
  vi.unstubAllGlobals();
});

it('requires a current run and configured chord', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  const host = document.createElement('div'); document.body.append(host); const root = createRoot(host);
  await act(async () => root.render(<KeyboardNodeControl node={node} path="isosui/input/keyboard/drive_key" runId="run-a" activeRevision="rev-a" current={false} lang="cn" />));
  expect((host.querySelector('button') as HTMLButtonElement).disabled).toBe(true);
  key('keydown', 'KeyW');
  expect(publish).not.toHaveBeenCalled();
  expect(keyMatches(new KeyboardEvent('keydown', { code: 'KeyW', shiftKey: true }), 'KeyW', ['Shift'])).toBe(true);
  expect(keyMatches(new KeyboardEvent('keydown', { code: 'KeyW' }), 'KeyW', ['Shift'])).toBe(false);
  await act(async () => root.unmount()); vi.unstubAllGlobals();
});
