/** A validation result belongs to one draft and one edit generation. */
export interface DraftValidationStamp { graph: string; generation: number }

export function draftValidationStamp(graph: unknown, generation: number): DraftValidationStamp {
  return { graph: JSON.stringify(graph), generation };
}

export function validationResponseIsCurrent(requestGeneration: number, currentGeneration: number): boolean {
  return requestGeneration === currentGeneration;
}

export function validationDisplayIsCurrent(stamp: DraftValidationStamp | null, graph: unknown, generation: number): boolean {
  return !!stamp && validationResponseIsCurrent(stamp.generation, generation) && stamp.graph === JSON.stringify(graph);
}
