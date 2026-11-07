export type Surface = 'control' | 'monitor' | 'vision' | 'logs';
export interface PanelLayout { open: boolean; collapsed: boolean; docked: boolean; x: number; y: number; width: number; height: number; z: number }
export type WorkspaceLayout = Record<Surface, PanelLayout>;
export const LAYOUT_VERSION = 1;
export const SURFACES: Surface[] = ['control', 'monitor', 'vision', 'logs'];

export function defaultWorkspaceLayout(): WorkspaceLayout {
  return Object.fromEntries(SURFACES.map((name, index) => [name, { open: name === 'control', collapsed: false, docked: true, x: 40 + index * 36, y: 130 + index * 32, width: 500, height: 470, z: index + 1 }])) as WorkspaceLayout;
}

export function clampPanel(panel: PanelLayout, viewport: { width: number; height: number }): PanelLayout {
  const width = Math.min(Math.max(320, panel.width), Math.max(1, viewport.width - 16));
  const height = Math.min(Math.max(240, panel.height), Math.max(1, viewport.height - 16));
  return { ...panel, width, height, z: Math.max(1, Math.min(90, panel.z)), x: Math.max(0, Math.min(panel.x, Math.max(0, viewport.width - width))), y: Math.max(0, Math.min(panel.y, Math.max(0, viewport.height - height))) };
}

export function layoutKey(robotId: string): string { return `isosui:workspace:v${LAYOUT_VERSION}:${robotId}`; }

export function readWorkspaceLayout(robotId: string, viewport: { width: number; height: number }, storage: Pick<Storage, 'getItem'> = localStorage): WorkspaceLayout {
  const fallback = defaultWorkspaceLayout();
  try {
    const raw: unknown = JSON.parse(storage.getItem(layoutKey(robotId)) || 'null');
    if (!raw || typeof raw !== 'object') return fallback;
    const data = raw as Record<string, unknown>;
    if (data.version !== LAYOUT_VERSION || !data.panels || typeof data.panels !== 'object') return fallback;
    const panels = data.panels as Record<string, unknown>;
    for (const name of SURFACES) {
      const value = panels[name];
      if (!value || typeof value !== 'object') return fallback;
      const panel = value as Record<string, unknown>;
      if (['open', 'collapsed', 'docked'].some((key) => typeof panel[key] !== 'boolean') || ['x', 'y', 'width', 'height', 'z'].some((key) => typeof panel[key] !== 'number' || !Number.isFinite(panel[key]))) return fallback;
    }
    return Object.fromEntries(SURFACES.map((name) => [name, clampPanel(panels[name] as PanelLayout, viewport)])) as WorkspaceLayout;
  } catch { return fallback; }
}
