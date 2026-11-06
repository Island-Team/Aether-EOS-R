import type { NodeGraph, NodeOptions } from '@/services/isos/node-graph';
import type { GraphNode } from './node-graph-edits';
import { settingOptionGroup, unavailableSelections } from './CatalogSelector';
import { uiSelectorValueValid } from './ui-selectors';

export interface OptionSnapshot { draft: string; options: NodeOptions }

export function optionResponseIsCurrent(requestId: number, latestRequestId: number, requestedDraft: string, currentDraft: string): boolean {
  return requestId === latestRequestId && requestedDraft === currentDraft;
}

export function catalogReloadIsCurrent(requestId: number, latestRequestId: number): boolean {
  return requestId === latestRequestId;
}

export function catalogReloadGraph(savedRevision: string, currentRevision: string, currentGraph: NodeGraph | null, savedGraph: NodeGraph): NodeGraph | null {
  return savedRevision === currentRevision ? currentGraph || savedGraph : null;
}

export function catalogSelectionBlocks(graphVersion: string | undefined, entries: GraphNode[], options: NodeOptions | null): boolean {
  if (graphVersion !== '0.2.0') return false;
  const selectors = entries.flatMap((entry) => (entry.definition?.settings || []).filter((field) => field.selector).map((field) => ({ entry, field })));
  if (selectors.some(({ entry, field }) => field.selector?.provider === 'ui' && (field.required || entry.node.settings?.[field.name] !== undefined) && !uiSelectorValueValid(field.selector.resource, entry.node.settings?.[field.name]))) return true;
  const remote = selectors.filter(({ field }) => field.selector?.provider !== 'ui');
  if (!remote.length) return !!unavailableSelections(options).length;
  if (!options || unavailableSelections(options).length) return true;
  return remote.some(({ entry, field }) => !settingOptionGroup(options, entry.path, field.name) || (field.required && !String(entry.node.settings?.[field.name] || '').trim()));
}
