import React, { useRef } from 'react';

export type CoreWindowState = { open: boolean; collapsed: boolean; maximized: boolean; docked: boolean; x: number; y: number; width: number; height: number };
export const defaultCoreWindow = (x: number): CoreWindowState => ({ open: false, collapsed: false, maximized: false, docked: true, x, y: 48, width: 550, height: 470 });
export function readCoreWindow(key: string, x: number): CoreWindowState {
  const fallback = defaultCoreWindow(x);
  try {
    const raw: unknown = JSON.parse(localStorage.getItem(key) || 'null');
    if (!raw || typeof raw !== 'object') return fallback;
    const data = raw as Record<string, unknown>;
    if (['open', 'collapsed', 'maximized', 'docked'].some((name) => typeof data[name] !== 'boolean') || ['x', 'y', 'width', 'height'].some((name) => !Number.isFinite(data[name]))) return fallback;
    return { open: data.open as boolean, collapsed: data.collapsed as boolean, maximized: data.maximized as boolean, docked: data.docked as boolean, x: Number(data.x), y: Number(data.y), width: Number(data.width), height: Number(data.height) };
  } catch { return fallback; }
}

export function GraphCoreWindow({ title, state, onChange, children, lang }: { title: string; state: CoreWindowState; onChange: (patch: Partial<CoreWindowState>) => void; children: React.ReactNode; lang: 'en' | 'cn' }): React.ReactElement | null {
  const drag = useRef<{ x: number; y: number; left: number; top: number } | null>(null);
  const resize = useRef<{ x: number; y: number; width: number; height: number } | null>(null);
  if (!state.open) return null;
  const labels = lang === 'cn' ? { float: '浮动', dock: '停靠', big: '放大', restore: '恢复', collapse: '收起', expand: '展开', close: '关闭' } : { float: 'Float', dock: 'Dock', big: 'Maximize', restore: 'Restore', collapse: 'Collapse', expand: 'Expand', close: 'Close' };
  const style: React.CSSProperties = state.maximized ? {} : state.docked ? {} : { left: state.x, top: state.y, width: state.width, height: state.collapsed ? 42 : state.height };
  return <section className={`graph-core-window nodrag nopan${state.docked ? ' graph-core-window--docked' : ''}${state.maximized ? ' graph-core-window--maximized' : ''}${state.collapsed ? ' graph-core-window--collapsed' : ''}`} style={style} aria-label={title}>
    <div className="graph-core-window__title" onPointerDown={(event) => { if ((event.target as HTMLElement).closest('button')) return; if (state.docked) onChange({ docked: false }); if (state.maximized) return; drag.current = { x: event.clientX, y: event.clientY, left: state.x, top: state.y }; event.currentTarget.setPointerCapture(event.pointerId); }} onPointerMove={(event) => { if (!drag.current) return; const canvas = event.currentTarget.closest('.node-graph-canvas')?.getBoundingClientRect(); onChange({ x: Math.max(0, Math.min((canvas?.width || 1000) - 80, drag.current.left + event.clientX - drag.current.x)), y: Math.max(0, Math.min((canvas?.height || 800) - 42, drag.current.top + event.clientY - drag.current.y)) }); }} onPointerUp={() => { drag.current = null; }} onPointerCancel={() => { drag.current = null; }}>
      <strong>{title}</strong><div role="toolbar" aria-label={title}><button type="button" onClick={() => onChange({ docked: !state.docked, maximized: false })}>{state.docked ? labels.float : labels.dock}</button><button type="button" onClick={() => onChange({ maximized: !state.maximized, docked: false, collapsed: false })}>{state.maximized ? labels.restore : labels.big}</button><button type="button" aria-expanded={!state.collapsed} onClick={() => onChange({ collapsed: !state.collapsed, maximized: false })}>{state.collapsed ? labels.expand : labels.collapse}</button><button type="button" aria-label={`${labels.close} ${title}`} onClick={() => onChange({ open: false })}>×</button></div>
    </div>
    {!state.collapsed && <div className="graph-core-window__body">{children}</div>}
    {!state.docked && !state.maximized && !state.collapsed && <div className="graph-core-window__resize" role="separator" aria-label={`${title} resize`} onPointerDown={(event) => { resize.current = { x: event.clientX, y: event.clientY, width: state.width, height: state.height }; event.currentTarget.setPointerCapture(event.pointerId); }} onPointerMove={(event) => { if (resize.current) onChange({ width: Math.max(320, resize.current.width + event.clientX - resize.current.x), height: Math.max(240, resize.current.height + event.clientY - resize.current.y) }); }} onPointerUp={() => { resize.current = null; }} onPointerCancel={() => { resize.current = null; }} />}
  </section>;
}
