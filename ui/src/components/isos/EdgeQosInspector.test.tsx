// @vitest-environment jsdom
import React, { act } from 'react';
import { createRoot } from 'react-dom/client';
import { afterEach, describe, expect, it, vi } from 'vitest';
import type { ValidatedEdge } from '@/services/isos/node-graph';
import { EdgeQosInspector } from './EdgeQosInspector';

const admitted: ValidatedEdge = { path: '/inputs/frame', source: 'a', target: 'b', input: 'frame', route_class: 'eos_local', fixed: false, routed: true, qos_eligible: true, priority: 0, data_type: 'camera_frame', transport: 'in_process' };
let host: HTMLDivElement;
let root: ReturnType<typeof createRoot>;
afterEach(async () => { if (root) await act(async () => root.unmount()); host?.remove(); });

async function render(authority?: ValidatedEdge): Promise<{ change: ReturnType<typeof vi.fn>; invalid: ReturnType<typeof vi.fn> }> {
  (globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT: boolean }).IS_REACT_ACT_ENVIRONMENT = true;
  host = document.createElement('div'); document.body.append(host); root = createRoot(host);
  const change = vi.fn(); const invalid = vi.fn();
  await act(async () => root.render(<EdgeQosInspector source={{ node: 'a', output: 'frame' }} authority={authority} current readOnly={false} lang="en" producerHint={30} onChange={change} onInvalidChange={invalid} />));
  return { change, invalid };
}

describe('edge QoS inspector', () => {
  it('shows legacy defaults and disables fixed or unrouted edges', async () => {
    await render({ ...admitted, routed: false, qos_eligible: false, route_class: 'design_only' });
    expect(host.textContent).toContain('Design only');
    expect(host.textContent).toContain('unlimited');
    expect(host.querySelector('fieldset')?.disabled).toBe(true);
    expect(host.textContent).toContain('Producer expected rate (hint): 30 Hz');
  });
  it('commits a keyboard edit and blocks malformed input before save', async () => {
    const { change, invalid } = await render(admitted);
    const input = host.querySelector('input')!;
    await act(async () => { const setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!; setter.call(input, '-21'); input.dispatchEvent(new Event('input', { bubbles: true })); });
    await act(async () => { input.dispatchEvent(new FocusEvent('focusout', { bubbles: true })); });
    expect(change).not.toHaveBeenCalled();
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('-20 to 19');
    expect(invalid).toHaveBeenLastCalledWith(true);
  });
  it('requires a fresh owner check after a committed draft change', async () => {
    const { change, invalid } = await render(admitted);
    const priority = host.querySelector('input')!;
    await act(async () => { Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set?.call(priority, '5'); priority.dispatchEvent(new Event('input', { bubbles: true })); });
    await act(async () => priority.dispatchEvent(new FocusEvent('focusout', { bubbles: true })));
    expect(change).toHaveBeenCalledWith({ priority: 5 });
    await act(async () => root.render(<EdgeQosInspector source={{ node: 'a', output: 'frame', priority: 5 }} current={false} readOnly={false} lang="en" onChange={change} onInvalidChange={invalid} />));
    expect(host.querySelector('fieldset')?.disabled).toBe(true);
    expect(host.textContent).toContain('Check graph to see IsOS route status');
  });
});
