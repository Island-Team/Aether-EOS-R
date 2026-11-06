// @vitest-environment jsdom
import React, { act } from 'react';
import { createRoot } from 'react-dom/client';
import { expect, it, vi } from 'vitest';
import { WorkspacePanel } from './WorkspacePanel';
import { defaultWorkspaceLayout } from './workspace-layout';

it('keeps a collapsed panel mounted, restores focus controls and supports keyboard resize', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  const container = document.createElement('div'); document.body.appendChild(container);
  const root = createRoot(container); const onChange = vi.fn();
  const layout = { ...defaultWorkspaceLayout().control, open: true, docked: false, width: 500, height: 400 };
  await act(async () => root.render(<WorkspacePanel title="Control" lang="en" layout={layout} viewport={{ width: 800, height: 600 }} onChange={onChange}><input aria-label="Pending command" defaultValue="ui-123" /></WorkspacePanel>));
  const resize = container.querySelector('[role="separator"]')!;
  await act(async () => resize.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowRight', bubbles: true })));
  expect(onChange).toHaveBeenCalledWith({ width: 520, height: 400 });
  const collapsed = { ...layout, collapsed: true };
  await act(async () => root.render(<WorkspacePanel title="Control" lang="en" layout={collapsed} viewport={{ width: 800, height: 600 }} onChange={onChange}><input aria-label="Pending command" defaultValue="ui-123" /></WorkspacePanel>));
  expect(container.querySelector('input')?.value).toBe('ui-123');
  expect(container.querySelector('.workspace-panel__body')?.hasAttribute('hidden')).toBe(true);
  await act(async () => root.render(<WorkspacePanel title="Control" lang="en" layout={{ ...layout, open: false }} viewport={{ width: 800, height: 600 }} onChange={onChange}><input aria-label="Pending command" defaultValue="new" /></WorkspacePanel>));
  expect(container.querySelector('input')?.value).toBe('ui-123');
  await act(async () => root.render(<WorkspacePanel title="Control" lang="en" layout={layout} viewport={{ width: 800, height: 600 }} onChange={onChange}><input aria-label="Pending command" defaultValue="new" /></WorkspacePanel>));
  expect(container.querySelector('input')?.value).toBe('ui-123');
  await act(async () => root.render(<WorkspacePanel title="Control" lang="en" layout={layout} viewport={{ width: 390, height: 640 }} onChange={onChange}><input aria-label="Pending command" defaultValue="new" /></WorkspacePanel>));
  expect([...container.querySelectorAll('button')].find((button) => button.textContent === 'Dock')).toBeDefined();
  await act(async () => root.render(<WorkspacePanel title="Control" lang="en" layout={{ ...layout, docked: true }} viewport={{ width: 390, height: 640 }} onChange={onChange}><input aria-label="Pending command" defaultValue="new" /></WorkspacePanel>));
  expect([...container.querySelectorAll('button')].find((button) => button.textContent === 'Float')?.disabled).toBe(true);
  await act(async () => root.unmount()); container.remove(); vi.unstubAllGlobals();
});
