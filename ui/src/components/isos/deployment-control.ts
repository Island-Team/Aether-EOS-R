/** One page-wide coordinator for IsOS launch and Stop ordering across workbench surfaces. */
import { controlProcesses, stopProcesses, type ProcessStatus, type StopControlReply } from '@/services/isos/node-graph';

let epoch = 0;
let stopping: Promise<StopControlReply> | null = null;
const activeRuns = new Set<Promise<ProcessStatus>>();

export function deploymentStopEpoch(): number { return epoch; }

export function runDeployment(operation: 'run' | 'restart', revision: string): Promise<ProcessStatus> {
  if (stopping) return Promise.reject(new Error('Stop is in progress.'));
  const request = controlProcesses(operation, revision);
  activeRuns.add(request);
  void request.finally(() => { activeRuns.delete(request); }).catch(() => undefined);
  return request;
}

export function stopDeploymentConverged(): Promise<StopControlReply> {
  if (stopping) return stopping;
  epoch += 1;
  const pending = [...activeRuns];
  const request = (async (): Promise<StopControlReply> => {
    let first: { ok: true; reply: StopControlReply } | { ok: false; error: unknown };
    try { first = { ok: true, reply: await stopProcesses() }; }
    catch (error) { first = { ok: false, error }; }
    if (pending.length) {
      await Promise.allSettled(pending);
      // A late Run may start even when the first Stop response was lost.
      // Its settled request requires one final owner Stop before reporting outcome.
      return stopProcesses();
    }
    if (!first.ok) throw first.error;
    return first.reply;
  })();
  stopping = request;
  void request.finally(() => { if (stopping === request) stopping = null; }).catch(() => undefined);
  return request;
}
