import { describe, expect, it } from 'vitest';
import { graphPackagePins, type NodeGraph } from '@/services/isos/node-graph';

describe('Home package requirements', () => {
  it('lists distinct exact pins from unrelated node families', () => {
    const graph: NodeGraph = { version: '0.1.0', robot: { id: 'robot', name: 'Robot' }, cores: {
      eos: { layers: { process: { groups: { simulator: { nodes: {
        sim: { id: 'sim', node_type: 'sample.sim@v0.1', label: '', description: '', package: { id: 'example/simulator', version: '1.2.0' } },
        sim2: { id: 'sim2', node_type: 'sample.sim@v0.1', label: '', description: '', package: { id: 'example/simulator', version: '1.2.0' } },
      } } } } } },
      egia: { layers: { cognition: { groups: { tools: { nodes: {
        tool: { id: 'tool', node_type: 'sample.tool@v0.1', label: '', description: '', package: { id: 'example/tools', version: '2.0.0' } },
      } } } } } },
    } };
    expect(graphPackagePins(graph)).toEqual([{ id: 'example/simulator', version: '1.2.0' }, { id: 'example/tools', version: '2.0.0' }]);
  });
});
