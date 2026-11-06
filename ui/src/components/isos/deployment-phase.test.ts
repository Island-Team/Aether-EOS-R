import { describe, expect, it } from 'vitest';
import type { ProcessStatus } from '@/services/isos/node-graph';
import { hasActiveDeployment } from './deployment-phase';

const status = (state: ProcessStatus['state'], active_revision: string | null): ProcessStatus => ({
  schema_version: '1.0.0', state, active_revision, processes: [],
});

describe('deployment phase', () => {
  it('uses Run for an inactive prior run with unresolved stop history', () => {
    expect(hasActiveDeployment(status('degraded', null))).toBe(false);
  });

  it('uses Restart when a degraded deployment still owns an active revision', () => {
    expect(hasActiveDeployment(status('degraded', 'revision'))).toBe(true);
    expect(hasActiveDeployment(status('running', 'revision'))).toBe(true);
  });
});
