import { describe, expect, it } from 'vitest';
import { clampPanel, defaultWorkspaceLayout, layoutKey, readWorkspaceLayout } from './workspace-layout';

const viewport = { width: 390, height: 640 };
describe('robot workspace layout', () => {
  it('uses versioned robot identity and rejects malformed or old layouts', () => {
    const panels = defaultWorkspaceLayout();
    const storage = { getItem: (key: string) => key === layoutKey('r1') ? JSON.stringify({ version: 1, panels }) : null };
    expect(readWorkspaceLayout('r1', viewport, storage).control.open).toBe(true);
    expect(readWorkspaceLayout('r2', viewport, storage)).toEqual(defaultWorkspaceLayout());
    expect(readWorkspaceLayout('r1', viewport, { getItem: () => JSON.stringify({ version: 0, panels }) })).toEqual(defaultWorkspaceLayout());
    expect(readWorkspaceLayout('r1', viewport, { getItem: () => JSON.stringify({ version: 1, panels: { ...panels, vision: { ...panels.vision, x: 'bad' } } }) })).toEqual(defaultWorkspaceLayout());
  });
  it('clamps a reopened float and its focus order into a narrow viewport', () => {
    const panels = defaultWorkspaceLayout(); panels.monitor = { ...panels.monitor, open: true, docked: false, x: 9000, y: 9000, width: 900, height: 900, z: 999999 };
    const next = readWorkspaceLayout('r', viewport, { getItem: () => JSON.stringify({ version: 1, panels }) });
    expect(next.monitor).toMatchObject({ x: 16, y: 16, width: 374, height: 624, z: 90 });
    expect(next.monitor.y + next.monitor.height).toBeLessThanOrEqual(viewport.height);
    expect(clampPanel({ ...next.monitor, width: 100, height: 100, x: -30, y: -40 }, viewport)).toMatchObject({ x: 0, y: 0, width: 320, height: 240 });
    expect(clampPanel(next.monitor, { width: 200, height: 180 })).toMatchObject({ width: 184, height: 164, x: 16, y: 16 });
  });
});
