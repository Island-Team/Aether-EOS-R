import { applyNodeGraph, validateNodeGraph, type NodeGraph, type NodeGraphDocument, type NodeGraphValidation } from '@/services/isos/node-graph';

export type CheckedApplyResult =
  | { state: 'stale' }
  | { state: 'rejected'; validation: NodeGraphValidation }
  | { state: 'applied'; validation: NodeGraphValidation; document: NodeGraphDocument };

/** Keep the UI's draft fence between exact IsOS Validate and revision checked Apply. */
export async function validatedApply(graph: NodeGraph, revision: string, current: () => boolean, onValidation: (validation: NodeGraphValidation) => void): Promise<CheckedApplyResult> {
  const validation = await validateNodeGraph(graph);
  if (!current()) return { state: 'stale' };
  onValidation(validation);
  if (!validation.valid) return { state: 'rejected', validation };
  const document = await applyNodeGraph(revision, graph);
  return { state: 'applied', validation, document };
}
