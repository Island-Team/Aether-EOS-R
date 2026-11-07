import { afterEach, expect, it, vi } from 'vitest';
import type { ProcessStatus, StopControlReply } from '@/services/isos/node-graph';

const mocks = vi.hoisted(() => ({ run: vi.fn(), stop: vi.fn() }));
vi.mock('@/services/isos/node-graph', async (importOriginal) => ({
  ...(await importOriginal<typeof import('@/services/isos/node-graph')>()),
  controlProcesses: mocks.run,
  stopProcesses: mocks.stop,
}));

const runB: ProcessStatus = { schema_version: '1.0.0', state: 'running', run_id: 'B', run_id_state: 'known', processes: [] };
const stoppedB: StopControlReply = { stopped: ['eos'], terminal: { run_id: 'B', outcome: 'confirmed', reason_code: null, processes: [{ id: 'eos', outcome: 'confirmed', reason_code: null }] } };

afterEach(() => { vi.clearAllMocks(); });

it('sends a final Stop for late Run B even if the first Stop transport fails', async () => {
  const { runDeployment, stopDeploymentConverged } = await import('./deployment-control');
  let finishRun!: (status: ProcessStatus) => void;
  mocks.run.mockImplementation(() => new Promise((resolve) => { finishRun = resolve; }));
  mocks.stop.mockRejectedValueOnce(new Error('connection lost')).mockResolvedValueOnce(stoppedB);
  const run = runDeployment('run', 'a'.repeat(64));
  const stop = stopDeploymentConverged();
  await Promise.resolve();
  expect(mocks.stop).toHaveBeenCalledTimes(1);
  finishRun(runB);
  expect(await run).toEqual(runB);
  expect(await stop).toEqual(stoppedB);
  expect(mocks.stop).toHaveBeenCalledTimes(2);
});

it('uses the final Stop for Run B after the first Stop observed no run', async () => {
  const { runDeployment, stopDeploymentConverged } = await import('./deployment-control');
  let finishRun!: (status: ProcessStatus) => void;
  mocks.run.mockImplementation(() => new Promise((resolve) => { finishRun = resolve; }));
  mocks.stop.mockResolvedValueOnce({ stopped: [], terminal: null }).mockResolvedValueOnce(stoppedB);
  const run = runDeployment('run', 'a'.repeat(64));
  const stop = stopDeploymentConverged();
  expect(mocks.stop).toHaveBeenCalledTimes(1);
  finishRun(runB);
  await run;
  expect(await stop).toEqual(stoppedB);
  expect(mocks.stop).toHaveBeenCalledTimes(2);
});

it('does not replay an uncertain standalone Stop with no pending Run', async () => {
  const { stopDeploymentConverged } = await import('./deployment-control');
  mocks.stop.mockRejectedValueOnce(new Error('connection lost'));
  await expect(stopDeploymentConverged()).rejects.toThrow('connection lost');
  expect(mocks.stop).toHaveBeenCalledTimes(1);
});
