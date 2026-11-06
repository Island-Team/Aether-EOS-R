import React, { useEffect, useState } from 'react';
import type { Language } from '@/types';
import { validKeyCode } from './ui-selectors';

const MODIFIERS = ['Control', 'Shift', 'Alt', 'Meta'] as const;
const MODIFIER_CODES = new Set(['ControlLeft', 'ControlRight', 'ShiftLeft', 'ShiftRight', 'AltLeft', 'AltRight', 'MetaLeft', 'MetaRight']);

interface Props { label: string; value: string; lang: Language; description?: string; modifiers?: string[]; onCapture: (code: string, modifiers: string[]) => void }

/** Browser-owned selector: no package or node type knowledge enters the editor. */
export function UiKeySelector({ label, value, lang, description, modifiers, onCapture }: Props): React.ReactElement {
  const [capturing, setCapturing] = useState(false);
  useEffect(() => {
    if (!capturing) return;
    const keydown = (event: KeyboardEvent): void => {
      event.preventDefault();
      event.stopImmediatePropagation();
      if (event.key === 'Escape') { setCapturing(false); return; }
      if (MODIFIER_CODES.has(event.code) || !validKeyCode(event.code)) return;
      onCapture(event.code, MODIFIERS.filter((name) => ({ Control: event.ctrlKey, Shift: event.shiftKey, Alt: event.altKey, Meta: event.metaKey })[name]));
      setCapturing(false);
    };
    const blur = (): void => setCapturing(false);
    window.addEventListener('keydown', keydown, true);
    window.addEventListener('blur', blur);
    return () => { window.removeEventListener('keydown', keydown, true); window.removeEventListener('blur', blur); };
  }, [capturing, onCapture]);
  const copy = lang === 'cn' ? { capture: '录入按键', waiting: '请按下按键或组合键；Esc 取消', invalid: '按键代码无效' } : { capture: 'Capture key', waiting: 'Press a key or chord; Escape cancels', invalid: 'Invalid key code' };
  return <div className="node-graph-setting"><span>{label}</span><p><kbd>{[...(modifiers || []), value || '—'].join('+')}</kbd></p><button type="button" aria-pressed={capturing} onClick={() => setCapturing(true)}>{capturing ? copy.waiting : copy.capture}</button>{description && <small>{description}</small>}{value && !validKeyCode(value) && <small role="alert" className="node-graph-error">{copy.invalid}</small>}</div>;
}
