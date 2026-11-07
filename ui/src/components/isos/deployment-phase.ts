import type { ProcessStatus } from '@/services/isos/node-graph';

export function hasActiveDeployment(status: ProcessStatus | null): boolean {
  return status?.state === 'running' || (status?.state === 'degraded' && status.active_revision != null);
}
