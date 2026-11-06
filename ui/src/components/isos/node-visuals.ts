import type { NodeInstance, NodeTypeDefinition, PortDefinition } from '@/services/isos/node-graph';

export type PortFamily = 'camera' | 'observation' | 'movement' | 'request' | 'outcome' | 'health' | 'status' | 'context' | 'plan' | 'other';

/** Stable layer hue with a bounded group shade for both editor themes. */
export function nodeVisualStyle(definition?: NodeTypeDefinition): Record<string, string> {
  if (!definition) return { '--node-layer': '#64748b', '--node-group-light': '#475569', '--node-group-dark': '#94a3b8' };
  const hues: Record<string, number> = { process: 215, hardware: 194, dispatcher: 224, control: 265, common: 145, data: 145 };
  const hash = [...definition.layer].reduce((value, character) => (value * 31 + character.charCodeAt(0)) >>> 0, 0);
  const hue = hues[definition.layer] ?? hash % 360;
  const groupHash = [...definition.group].reduce((value, character) => (value * 31 + character.charCodeAt(0)) >>> 0, 0);
  const step = groupHash % 5;
  return {
    '--node-layer': `hsl(${hue} 65% 46%)`,
    '--node-group-light': `hsl(${hue} ${58 + step * 3}% ${34 + step * 3}%)`,
    '--node-group-dark': `hsl(${hue} ${62 + step * 3}% ${65 + step * 3}%)`,
  };
}

export function portFamily(dataType: string): PortFamily {
  const value = dataType.toLowerCase();
  if (/camera|frame|depth|image/.test(value)) return 'camera';
  if (/movement|command/.test(value)) return 'movement';
  if (/observation|pose|state/.test(value)) return 'observation';
  if (/control.request|request/.test(value)) return 'request';
  if (/outcome|result/.test(value)) return 'outcome';
  if (/health/.test(value)) return 'health';
  if (/status/.test(value)) return 'status';
  if (/context|snapshot/.test(value)) return 'context';
  if (/plan|proposal/.test(value)) return 'plan';
  return 'other';
}

export function dataTypeColor(dataType: string): string {
  if (!dataType) return '#64748b';
  const hash = [...dataType].reduce((value, character) => (Math.imul(value ^ character.charCodeAt(0), 16777619)) >>> 0, 2166136261);
  return `hsl(${hash % 360} 70% 45%)`;
}

export function frequencyPattern(rate?: number | null): 'unknown' | 'low' | 'medium' | 'high' | 'solid' {
  if (typeof rate !== 'number' || !Number.isFinite(rate) || rate <= 0) return 'unknown';
  if (rate < 1) return 'low';
  if (rate < 10) return 'medium';
  if (rate < 30) return 'high';
  return 'solid';
}

export function portVisual(port: PortDefinition): { className: string; style: Record<string, string> } {
  const transport = (port.transport || 'in_process').toLowerCase().replace(/[^a-z0-9_-]/g, '-');
  const delivery = (port.delivery_mode || 'snapshot').toLowerCase().replace(/[^a-z0-9_-]/g, '-');
  return { className: `node-graph-port-transport--${transport} node-graph-port-delivery--${delivery}`, style: { '--port': dataTypeColor(port.data_type) } };
}

/** Anchor shape is transport, anchor color is exact type, line pattern is frequency. */
export function interfaceVisual(port?: PortDefinition, edgeCap?: number): { className: string; style: Record<string, string>; label: string } {
  const rate = edgeCap;
  const transport = (port?.transport || 'in_process').toLowerCase().replace(/[^a-z0-9_-]/g, '-');
  return { className: `node-graph-edge-frequency--${rate === undefined ? 'unlimited' : frequencyPattern(rate)}`, style: { '--wire': '#64748b' }, label: `${typeof rate === 'number' && rate > 0 ? `${rate} Hz cap` : 'Unlimited cap'} · ${transport} · ${port?.delivery_mode || 'snapshot'} · ${port?.data_type || 'unknown type'}` };
}

export function nodeSummary(definition: NodeTypeDefinition | undefined, node: NodeInstance): string[] {
  if (!definition) return [];
  const settings = node.settings || {};
  const selected = definition.settings.flatMap((field) => {
    const value = settings[field.name];
    if (value === undefined || value === null || value === '') return [];
    if (Array.isArray(value)) return [`${field.name}: ${value.length} items`];
    if (typeof value === 'object') return [`${field.name}: ${Object.keys(value).length} fields`];
    return [`${field.name}: ${String(value)}`];
  });
  if (node.package) selected.unshift(`package: ${node.package.id}@${node.package.version}`);
  const connections = Object.values(node.inputs || {}).reduce((count, source) => count + (Array.isArray(source) ? source.length : 1), 0);
  if (connections) selected.push(`connections: ${connections}`);
  return selected.slice(0, 3);
}
