import type { DataTypeDefinition, NodeGraphDiagnostic } from '@/services/isos/node-graph';
import { canonicalNodePath, type GraphNode } from './node-graph-edits';

export interface EdgePorts { source: string; target: string; sourceHandle?: string | null; targetHandle?: string | null; inputIndex?: number; inputCount?: number }
export interface EdgeContracts { sourceType: string; targetType: string; sourceIdl?: DataTypeDefinition; targetIdl?: DataTypeDefinition }

/** Resolve both catalog ends; mismatched graph declarations stay visible for diagnosis. */
export function edgeContracts(entries: GraphNode[], edge: EdgePorts, types: DataTypeDefinition[]): EdgeContracts {
  const sourceName = edge.sourceHandle?.replace(/^out:/, '') || '';
  const targetName = edge.targetHandle?.replace(/^in:/, '') || '';
  const sourceType = entries.find((entry) => entry.path === edge.source)?.definition?.outputs.find((port) => port.name === sourceName)?.data_type || '';
  const targetType = entries.find((entry) => entry.path === edge.target)?.definition?.inputs.find((port) => port.name === targetName)?.data_type || '';
  const find = (name: string): DataTypeDefinition | undefined => types.find((item) => item.id === name || item.id.split('@')[0] === name);
  return { sourceType, targetType, sourceIdl: find(sourceType), targetIdl: find(targetType) };
}

/** Match an IsOS input diagnostic to its destination port, never to live health. */
export function edgeInputDiagnostics(edge: EdgePorts, diagnostics: NodeGraphDiagnostic[]): NodeGraphDiagnostic[] {
  const port = edge.targetHandle?.replace(/^in:/, '');
  if (!port) return [];
  const target = canonicalNodePath(edge.target).split('/');
  if (target.length !== 4) return [];
  const expanded = ['cores', target[0], 'layers', target[1], 'groups', target[2], 'nodes', target[3], 'inputs', port];
  const compact = [...target, 'inputs', port];
  return diagnostics.filter(({ path }) => {
    const parts = path.replace(/\[(\d+)\]/g, '.$1').split(/[/.]/).filter(Boolean);
    const prefix = [expanded, compact].find((candidate) => parts.some((part, index) => part === candidate[0] && candidate.every((segment, offset) => parts[index + offset] === segment)));
    if (!prefix) return false;
    const start = parts.findIndex((part, index) => part === prefix[0] && prefix.every((segment, offset) => parts[index + offset] === segment));
    const sourceIndex = parts[start + prefix.length];
    if (sourceIndex !== undefined && /^\d+$/.test(sourceIndex)) return Number(sourceIndex) === (edge.inputIndex ?? 0);
    return (edge.inputCount ?? 1) === 1;
  });
}
