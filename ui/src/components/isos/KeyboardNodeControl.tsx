import React, { useEffect, useRef, useState } from 'react';
import { publishKeyState, type KeyState, type KeyStateEvent } from '@/services/isos/node-events';
import type { NodeInstance } from '@/services/isos/node-graph';
import type { Language } from '@/types';

const REFRESH_MS = 100;
const MODIFIERS = ['Control', 'Shift', 'Alt', 'Meta'] as const;
type Modifier = typeof MODIFIERS[number];

function selectedModifiers(value: unknown): Modifier[] {
  return Array.isArray(value) ? MODIFIERS.filter((name) => value.includes(name)) : [];
}

export function keyMatches(event: KeyboardEvent, code: string, modifiers: Modifier[]): boolean {
  if (event.code !== code || !code) return false;
  const active: Record<Modifier, boolean> = { Control: event.ctrlKey, Shift: event.shiftKey, Alt: event.altKey, Meta: event.metaKey };
  return MODIFIERS.every((name) => active[name] === modifiers.includes(name));
}

export function editableTarget(target: EventTarget | null): boolean {
  return target instanceof Element && !!target.closest('input, textarea, select, [contenteditable="true"], [role="textbox"]');
}

interface Props { node: NodeInstance; path: string; runId: string | null; activeRevision: string | null; current: boolean; lang: Language }

/** Arming is explicit; the control is bound to the selected node and current run. */
export function KeyboardNodeControl({ node, path, runId, activeRevision, current, lang }: Props): React.ReactElement {
  const [armed, setArmed] = useState(false);
  const [message, setMessage] = useState('');
  const activation = useRef<{ id: string; sequence: number } | null>(null);
  const queue = useRef<Promise<unknown>>(Promise.resolve());
  const pending = useRef(0);
  const key = typeof node.settings?.key === 'string' ? node.settings.key : '';
  const modifiers = selectedModifiers(node.settings?.modifiers);
  const modifierKey = modifiers.join('+');
  const ready = current && !!runId && !!activeRevision && !!key;

  useEffect(() => {
    if (!armed || !ready || !runId || !activeRevision) return;
    const source = path;
    let disposed = false;
    const send = (state: KeyState): void => {
      if (state === 'refresh' && pending.current > 0) return;
      if (state === 'pressed') activation.current = { id: crypto.randomUUID(), sequence: 0 };
      const value = activation.current;
      if (!value) return;
      const event: KeyStateEvent = { schema_version: '0.1.0', source_node: source, output: 'state', data_type: 'key_state', run_id: runId, active_revision: activeRevision, payload: { state, activation_id: value.id, sequence: ++value.sequence } };
      // Serialize transitions so release cannot overtake a delayed press.
      pending.current += 1;
      queue.current = queue.current.catch(() => undefined).then(() => publishKeyState(event)).catch((error: unknown) => {
        if (!disposed) { setMessage(error instanceof Error ? error.message : String(error)); setArmed(false); }
      }).finally(() => { pending.current -= 1; });
      if (state === 'released') activation.current = null;
    };
    const release = (): void => { if (activation.current) send('released'); };
    const down = (event: KeyboardEvent): void => {
      if (editableTarget(event.target) || !keyMatches(event, key, modifiers)) return;
      event.preventDefault();
      event.stopImmediatePropagation();
      if (!event.repeat && !activation.current) send('pressed');
    };
    const up = (event: KeyboardEvent): void => {
      if (activation.current && (event.code === key || MODIFIERS.some((name) => modifiers.includes(name) && event.key === name))) release();
    };
    const visibility = (): void => { if (document.visibilityState !== 'visible') release(); };
    window.addEventListener('keydown', down, true);
    window.addEventListener('keyup', up);
    window.addEventListener('blur', release);
    document.addEventListener('visibilitychange', visibility);
    const timer = window.setInterval(() => { if (activation.current) send('refresh'); }, REFRESH_MS);
    return () => {
      disposed = true;
      release();
      window.clearInterval(timer);
      window.removeEventListener('keydown', down, true);
      window.removeEventListener('keyup', up);
      window.removeEventListener('blur', release);
      document.removeEventListener('visibilitychange', visibility);
    };
  }, [armed, ready, runId, activeRevision, path, key, modifierKey]);

  const copy = lang === 'cn'
    ? { title: '键盘输入', arm: '启用键盘输入', disarm: '停用键盘输入', idle: '需选择按键并运行当前已保存节点图。', active: '已启用。按住配置的按键发送有期限的输入；松开或失去焦点即释放。' }
    : { title: 'Keyboard input', arm: 'Arm keyboard input', disarm: 'Disarm keyboard input', idle: 'Select a key and run the current saved graph first.', active: 'Armed. Hold the configured key for leased input; release or focus loss ends it.' };
  return <section className="node-graph-keyboard-control"><h3>{copy.title}</h3><p><kbd>{[...modifiers, key || '—'].join('+')}</kbd></p><button type="button" disabled={!ready && !armed} onClick={() => { setMessage(''); setArmed((value) => !value); }}>{armed ? copy.disarm : copy.arm}</button><p role="status">{armed ? copy.active : copy.idle}</p>{message && <p role="alert" className="node-graph-error">{message}</p>}</section>;
}
