import { afterEach, describe, expect, it, vi } from 'vitest';
import { defaultCoreWindow, readCoreWindow } from './GraphCoreWindow';

const values = new Map<string, string>();
vi.stubGlobal('localStorage', { getItem: (key: string) => values.get(key) || null });
afterEach(() => values.clear());

describe('graph core window layout', () => {
  it('restores floating position and maximum state', () => {
    values.set('window', JSON.stringify({ ...defaultCoreWindow(90), open: true, docked: false, maximized: true, x: 130 }));
    expect(readCoreWindow('window', 90)).toMatchObject({ open: true, docked: false, maximized: true, x: 130 });
    values.set('isosui-graph-egia-window', JSON.stringify({ ...defaultCoreWindow(190), open: true, docked: false, x: 240 }));
    expect(readCoreWindow('isosui-graph-egia-window', 190)).toMatchObject({ open: true, docked: false, x: 240 });
  });
  it('rejects malformed saved coordinates', () => {
    values.set('window', JSON.stringify({ ...defaultCoreWindow(90), x: 'offscreen' }));
    expect(readCoreWindow('window', 90)).toEqual(defaultCoreWindow(90));
  });
});
