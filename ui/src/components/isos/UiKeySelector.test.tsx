// @vitest-environment jsdom
import React, { act } from 'react';
import { createRoot } from 'react-dom/client';
import { afterEach, expect, it, vi } from 'vitest';
import { UiKeySelector } from './UiKeySelector';

afterEach(() => { document.body.innerHTML = ''; });

it('captures a physical key and held modifiers without choosing a node type', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  const host = document.createElement('div'); document.body.append(host); const root = createRoot(host);
  const capture = vi.fn();
  await act(async () => root.render(<UiKeySelector label="key" value="" lang="en" onCapture={capture} />));
  await act(async () => (host.querySelector('button') as HTMLButtonElement).click());
  await act(async () => window.dispatchEvent(new KeyboardEvent('keydown', { code: 'ShiftLeft', key: 'Shift', shiftKey: true })));
  expect(capture).not.toHaveBeenCalled();
  await act(async () => window.dispatchEvent(new KeyboardEvent('keydown', { code: 'KeyW', key: 'W', shiftKey: true })));
  expect(capture).toHaveBeenCalledWith('KeyW', ['Shift']);
  expect((host.querySelector('button') as HTMLButtonElement).getAttribute('aria-pressed')).toBe('false');
  await act(async () => root.unmount()); vi.unstubAllGlobals();
});

it('cancels capture on Escape', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  const host = document.createElement('div'); document.body.append(host); const root = createRoot(host);
  const capture = vi.fn();
  await act(async () => root.render(<UiKeySelector label="按键" value="KeyS" lang="cn" onCapture={capture} />));
  await act(async () => (host.querySelector('button') as HTMLButtonElement).click());
  await act(async () => window.dispatchEvent(new KeyboardEvent('keydown', { code: 'Escape', key: 'Escape' })));
  expect(capture).not.toHaveBeenCalled();
  expect((host.querySelector('button') as HTMLButtonElement).getAttribute('aria-pressed')).toBe('false');
  await act(async () => root.unmount()); vi.unstubAllGlobals();
});
