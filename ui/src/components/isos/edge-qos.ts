import type { NodeGraph, NodeSource, ValidatedEdge } from '@/services/isos/node-graph';
import { canonicalNodePath, inputSources } from './node-graph-edits';

const escapePointer = (value: string): string => value.replace(/~/g, '~0').replace(/\//g, '~1');

/** IsOS addresses an object source at the input and an array source at its index. */
export function sourcePointer(target: string, input: string, index: number, array: boolean): string {
  const parts = canonicalNodePath(target).split('/');
  if (parts.length !== 4) return '';
  const base = `/cores/${escapePointer(parts[0])}/layers/${escapePointer(parts[1])}/groups/${escapePointer(parts[2])}/nodes/${escapePointer(parts[3])}/inputs/${escapePointer(input)}`;
  return array ? `${base}/${index}` : base;
}

export function validatedSource(edges: ValidatedEdge[], target: string, input: string, index: number, array: boolean): ValidatedEdge | undefined {
  const pointer = sourcePointer(target, input, index, array);
  return edges.find((edge) => edge.path === pointer);
}

export function edgeSource(graph: NodeGraph, target: string, input: string, index: number): { source: NodeSource; array: boolean } | null {
  const parts = canonicalNodePath(target).split('/');
  const raw = parts.length === 4 ? graph.cores[parts[0]]?.layers[parts[1]]?.groups[parts[2]]?.nodes[parts[3]]?.inputs?.[input] : undefined;
  if (!raw) return null;
  const source = inputSources(raw)[index];
  return source ? { source, array: Array.isArray(raw) } : null;
}

export function parsePriority(raw: string): number | null {
  if (!/^-?\d+$/.test(raw.trim())) return null;
  const value = Number(raw.trim());
  return Number.isInteger(value) && value >= -20 && value <= 19 ? value : null;
}

export function parseFrequency(raw: string): number | null | undefined {
  if (!raw.trim()) return undefined;
  const value = Number(raw.trim());
  return Number.isFinite(value) && value > 0 ? value : null;
}

/** Copy only the addressed source. Preserve its shape, siblings, and unknown fields. */
export function updateEdgeQos(graph: NodeGraph, target: string, input: string, index: number, change: { priority?: number; frequency_hz?: number | null }): NodeGraph {
  const next = structuredClone(graph);
  const parts = canonicalNodePath(target).split('/');
  const node = parts.length === 4 ? next.cores[parts[0]]?.layers[parts[1]]?.groups[parts[2]]?.nodes[parts[3]] : undefined;
  const raw = node?.inputs?.[input];
  if (!node?.inputs || !raw) return graph;
  const list = inputSources(raw);
  const source = list[index];
  if (!source) return graph;
  if (change.priority !== undefined) source.priority = change.priority;
  if ('frequency_hz' in change) {
    if (change.frequency_hz === null) delete source.frequency_hz;
    else source.frequency_hz = change.frequency_hz;
  }
  node.inputs[input] = Array.isArray(raw) ? list : source;
  return next;
}
