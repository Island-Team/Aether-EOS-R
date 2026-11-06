import type { CurrentLogPage, LogAnchor, LogDiagnostic, LogDiagnosticCode, LogLine, LogScope } from '../../services/isos/node-graph';

export interface CurrentLogState {
  scope: LogScope | null;
  anchor: LogAnchor | null;
  lines: LogLine[];
  cursor?: string;
  diagnostics: LogDiagnostic[];
  acknowledgedDiagnostics: Partial<Record<LogDiagnosticCode, number | null>>;
  boundary: { previous: string; current: string } | null;
}

export const emptyCurrentLogState: CurrentLogState = {
  scope: null, anchor: null, lines: [], diagnostics: [], acknowledgedDiagnostics: {}, boundary: null,
};

function sameScope(a: LogScope | null, b: LogScope): boolean {
  return a?.kind === b.kind && a.run_id === b.run_id;
}

/** Rotation preserves retained rows; only an owner-reported scope change clears them. */
export function acceptCurrentLogPage(state: CurrentLogState, page: CurrentLogPage): CurrentLogState {
  const changed = state.scope !== null && !sameScope(state.scope, page.scope);
  const lines = page.anchor.state === 'found'
    ? [...(changed ? [] : state.lines), ...page.lines].slice(-1000)
    : [];
  const acknowledgedDiagnostics = changed ? {} : { ...state.acknowledgedDiagnostics };
  const byCode = new Map<LogDiagnosticCode, LogDiagnostic>((changed ? [] : state.diagnostics).map((item) => [item.code, item]));
  for (const item of page.diagnostics) {
    // The scope boundary already reports a new run; it is not a retention gap.
    if (item.code === 'log_run_changed') continue;
    const acknowledged = acknowledgedDiagnostics[item.code];
    if (acknowledged !== undefined && (item.count === null || (acknowledged !== null && item.count <= acknowledged))) continue;
    byCode.set(item.code, item);
  }
  return {
    scope: page.scope,
    anchor: page.anchor,
    lines,
    cursor: page.next_cursor,
    diagnostics: [...byCode.values()],
    acknowledgedDiagnostics,
    boundary: changed ? { previous: state.scope!.run_id, current: page.scope.run_id } : state.boundary,
  };
}

/** Dismiss only evidence already seen in this scope; a larger count can alert again. */
export function acknowledgeCurrentLogDiagnostics(state: CurrentLogState): CurrentLogState {
  const acknowledgedDiagnostics = { ...state.acknowledgedDiagnostics };
  for (const item of state.diagnostics) acknowledgedDiagnostics[item.code] = item.count;
  return { ...state, diagnostics: [], acknowledgedDiagnostics };
}

/** Filters affect details and cursor only; the owner-pinned first event stays visible. */
export function resetCurrentLogDetails(state: CurrentLogState): CurrentLogState {
  return { ...state, lines: [], cursor: undefined };
}

export function validNodeFilter(value: string): boolean {
  return value.length <= 512 && /^\/cores\/[A-Za-z0-9_-]+\/layers\/[A-Za-z0-9_-]+\/groups\/[A-Za-z0-9_-]+\/nodes\/[A-Za-z0-9_-]+$/.test(value);
}

export function validCorrelationFilter(value: string): boolean {
  return /^[A-Za-z0-9_-]{1,64}$/.test(value);
}
