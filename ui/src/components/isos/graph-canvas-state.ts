import type { NodeGraph } from '@/services/isos/node-graph';
import type { NodeChange } from 'reactflow';
import { canonicalNodePath, removeNode } from './node-graph-edits';

export type NodePositions = Record<string, { x: number; y: number }>;
export type NodeMirrors = Record<string, boolean>;

/** A stable graph identity keeps visual positions separate from graph revisions. */
export function graphLayoutKey(graph: NodeGraph): string {
  return `isosui-v2-layout:${encodeURIComponent(graph.robot.id)}`;
}

export function graphMirrorKey(graph: NodeGraph): string { return `${graphLayoutKey(graph)}:mirrors`; }
export function parseNodeMirrors(raw: string | null): NodeMirrors {
  try { const value: unknown = JSON.parse(raw || '{}'); return value && !Array.isArray(value) && typeof value === 'object' ? Object.fromEntries(Object.entries(value).filter((entry): entry is [string, boolean] => entry[1] === true).map(([path, mirrored]) => [canonicalNodePath(path), mirrored])) : {}; }
  catch { return {}; }
}

export function parseNodePositions(raw: string | null): NodePositions {
  if (!raw) return {};
  try {
    const parsed: unknown = JSON.parse(raw);
    if (!parsed || Array.isArray(parsed) || typeof parsed !== 'object') return {};
    return Object.fromEntries(Object.entries(parsed).filter((entry): entry is [string, { x: number; y: number }] => {
      const point = entry[1] as { x?: unknown; y?: unknown } | null;
      return Boolean(point && typeof point.x === 'number' && Number.isFinite(point.x) && typeof point.y === 'number' && Number.isFinite(point.y));
    }).map(([path, point]) => [canonicalNodePath(path), point]));
  } catch { return {}; }
}

export function removeSelectedNodes(graph: NodeGraph, paths: string[]): NodeGraph {
  return [...new Set(paths)].reduce((current, path) => removeNode(current, path), graph);
}

export function applyNodeSelection(current: string[], changes: NodeChange[]): string[] {
  const selection = changes.filter((change): change is Extract<NodeChange, { type: 'select' }> => change.type === 'select');
  if (!selection.length) return current;
  const next = new Set(current);
  for (const change of selection) if (change.selected) next.add(change.id); else next.delete(change.id);
  return [...next];
}
