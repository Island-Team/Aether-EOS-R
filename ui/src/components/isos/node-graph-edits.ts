import { catalogTypeKey, type NodeGraph, type NodeInstance, type NodeSource, type NodeTypeDefinition } from '../../services/isos/node-graph';

export interface GraphNode { path: string; node: NodeInstance; definition?: NodeTypeDefinition }
export const nodeIdPattern = /^[a-z][a-z0-9_-]{0,47}$/;

/** The four-segment path is canonical; legacy expanded paths remain readable during migration. */
export function canonicalNodePath(path: string): string {
  const parts = path.split('/');
  if (parts.length === 8 && parts[0] === 'cores' && parts[2] === 'layers' && parts[4] === 'groups' && parts[6] === 'nodes') return `${parts[1]}/${parts[3]}/${parts[5]}/${parts[7]}`;
  return path;
}
export function sameNodePath(left: string, right: string): boolean { return canonicalNodePath(left) === canonicalNodePath(right); }
export function inputSources(source: NodeSource | NodeSource[]): NodeSource[] { return Array.isArray(source) ? source : [source]; }

/** Choose a stable type-and-index default without colliding with any graph node. */
export function nextNodeId(graph: NodeGraph, definition: NodeTypeDefinition): string {
  const type = definition.id.split('@')[0].split('.').pop()?.replace(/[^a-z0-9-]/g, '-') || 'node';
  const used = new Set(graphNodes(graph, []).map((entry) => entry.node.id));
  for (let index = 1; index < 100000; index += 1) {
    const suffix = `_${index}`;
    const candidate = `${type.slice(0, 48 - suffix.length)}${suffix}`;
    if (!used.has(candidate)) return candidate;
  }
  throw new Error('No available node ID.');
}

export function nodePath(definition: Pick<NodeTypeDefinition, 'core' | 'layer' | 'group'>, id: string): string {
  return `${definition.core}/${definition.layer}/${definition.group}/${id}`;
}

export function graphNodes(graph: NodeGraph, types: NodeTypeDefinition[]): GraphNode[] {
  const definitions = new Map(types.map((type) => [catalogTypeKey(type), type]));
  return Object.entries(graph.cores).flatMap(([coreId, core]) => Object.entries(core.layers).flatMap(([layerId, layer]) =>
    Object.entries(layer.groups).flatMap(([groupId, group]) => Object.entries(group.nodes).map(([id, node]) => ({
      path: `${coreId}/${layerId}/${groupId}/${id}`,
      node,
      definition: definitions.get(catalogTypeKey({ core: coreId, layer: layerId, group: groupId, id: node.node_type })),
    })))));
}

/** Package previews may add nodes but must never remove existing graph nodes. */
export function removedNodePaths(current: NodeGraph, preview: NodeGraph): string[] {
  const nextPaths = new Set(graphNodes(preview, []).map((entry) => entry.path));
  return graphNodes(current, []).map((entry) => entry.path).filter((path) => !nextPaths.has(path));
}

function graphCopy(graph: NodeGraph): NodeGraph { return structuredClone(graph); }

function nodeSlot(graph: NodeGraph, path: string): { nodes: Record<string, NodeInstance>; id: string } | undefined {
  const parts = canonicalNodePath(path).split('/');
  if (parts.length !== 4) return;
  const nodes = graph.cores[parts[0]]?.layers[parts[1]]?.groups[parts[2]]?.nodes;
  return nodes ? { nodes, id: parts[3] } : undefined;
}

export function makeNode(id: string, definition: NodeTypeDefinition): NodeInstance {
  const node: NodeInstance = { id, node_type: definition.id, label: id, description: '' };
  // Target types receive required values from the operator or a verified node set.
  if (definition.id.endsWith('@v0.1') || definition.id.endsWith('@v0.2')) return node;
  node.capabilities = [];
  node.settings = {};
  node.inputs = {};
  if (definition.lifecycle === 'adapter') {
    node.frame_id = `${id}_frame`;
    node.adapter = { type: 'mock-cxx' };
    node.health_timing = { poll_interval_ms: 100, stale_after_ms: 500 };
    node.observation = { max_rate_hz: 30, max_payload_bytes: definition.kind === 'camera' ? 8388608 : 4096 };
    node.capabilities = definition.kind === 'camera' ? ['rgb8'] : [definition.capabilities[0]];
    if (definition.kind === 'manipulator') node.joint_names = ['joint1'];
    if (definition.kind === 'base') node.movement = { modes: ['body_velocity'], limits: { max_linear_m_s: 1, max_angular_rad_s: 1, max_velocity_lease_ms: 500, max_translation_m: 5, max_rotation_rad: 3.14 } };
    if (definition.kind === 'manipulator') node.movement = { modes: ['joint_positions'], limits: { max_joint_position_rad: 3.14, max_joint_speed_rad_s: 1, max_end_effector_speed_m_s: 0.2, max_end_effector_translation_m: 1 } };
  }
  return node;
}

export function addNode(graph: NodeGraph, definition: NodeTypeDefinition, id: string): NodeGraph {
  if (!nodeIdPattern.test(id)) throw new Error('Invalid node ID.');
  if (graphNodes(graph, []).some((entry) => entry.path === nodePath(definition, id))) throw new Error('A node with this ID already exists in this group.');
  if (definition.lifecycle === 'adapter' && graphNodes(graph, []).some((entry) => /^(eos\/hardware|isos\/eos_hardware)\//.test(entry.path) && entry.node.id === id)) throw new Error('A hardware node with this ID already exists.');
  const next = graphCopy(graph);
  const core = next.cores[definition.core] ||= { layers: {} };
  const layer = core.layers[definition.layer] ||= { groups: {} };
  const group = layer.groups[definition.group] ||= { nodes: {} };
  group.nodes[id] = makeNode(id, definition);
  return next;
}

export function updateNode(graph: NodeGraph, path: string, update: Partial<NodeInstance>): NodeGraph {
  const next = graphCopy(graph);
  const slot = nodeSlot(next, path);
  if (!slot || !slot.nodes[slot.id]) return graph;
  slot.nodes[slot.id] = { ...slot.nodes[slot.id], ...update, id: slot.id, node_type: slot.nodes[slot.id].node_type };
  return next;
}

export function removeNode(graph: NodeGraph, path: string): NodeGraph {
  const next = graphCopy(graph);
  const slot = nodeSlot(next, path);
  if (!slot || !slot.nodes[slot.id]) return graph;
  delete slot.nodes[slot.id];
  for (const entry of graphNodes(next, [])) {
    entry.node.inputs = Object.fromEntries(Object.entries(entry.node.inputs || {}).flatMap(([input, source]) => {
      const remaining = inputSources(source).filter((item) => !sameNodePath(item.node, path));
      return remaining.length ? [[input, Array.isArray(source) ? remaining : remaining[0]]] : [];
    }));
    entry.node.references = entry.node.references?.filter((reference) => !sameNodePath(reference, path));
  }
  const parts = canonicalNodePath(path).split('/');
  const core = next.cores[parts[0]];
  const layer = core.layers[parts[1]];
  const group = layer.groups[parts[2]];
  if (!Object.keys(group.nodes).length) delete layer.groups[parts[2]];
  if (!Object.keys(layer.groups).length) delete core.layers[parts[1]];
  if (!Object.keys(core.layers).length) delete next.cores[parts[0]];
  return next;
}

export function renameNode(graph: NodeGraph, path: string, id: string): NodeGraph {
  if (!nodeIdPattern.test(id)) throw new Error('Invalid node ID.');
  const next = graphCopy(graph);
  const slot = nodeSlot(next, path);
  if (!slot || !slot.nodes[slot.id]) return graph;
  if (slot.nodes[id] && id !== slot.id) throw new Error('A node with this ID already exists in this group.');
  const old = slot.nodes[slot.id];
  delete slot.nodes[slot.id];
  slot.nodes[id] = { ...old, id };
  const newPath = `${canonicalNodePath(path).slice(0, canonicalNodePath(path).lastIndexOf('/') + 1)}${id}`;
  for (const entry of graphNodes(next, [])) {
    for (const value of Object.values(entry.node.inputs || {})) for (const source of inputSources(value)) if (sameNodePath(source.node, path)) source.node = newPath;
    entry.node.references = entry.node.references?.map((reference) => sameNodePath(reference, path) ? newPath : reference);
    if (entry.node.node_type === 'eos.control.manual@v1.0' || entry.node.node_type === 'eos.control.primitive@v1.0') {
      if (typeof entry.node.settings?.target === 'string' && sameNodePath(entry.node.settings.target, path)) entry.node.settings = { ...entry.node.settings, target: newPath };
    }
    if (entry.node.node_type === 'eos.control.skill@v1.0' && Array.isArray(entry.node.settings?.steps)) {
      entry.node.settings = { ...entry.node.settings, steps: entry.node.settings.steps.map((step) => typeof step === 'string' && sameNodePath(step, path) ? newPath : step) };
    }
  }
  return next;
}

export function connectNodes(graph: NodeGraph, types: NodeTypeDefinition[], providerPath: string, output: string, consumerPath: string, input: string): NodeGraph {
  const nodes = graphNodes(graph, types);
  const provider = nodes.find((entry) => sameNodePath(entry.path, providerPath));
  const consumer = nodes.find((entry) => sameNodePath(entry.path, consumerPath));
  if (!provider || !consumer || sameNodePath(providerPath, consumerPath)) throw new Error('Choose two different nodes.');
  const outputPort = provider.definition?.outputs.find((port) => port.name === output);
  const outputType = outputPort?.data_type;
  const inputPort = consumer.definition?.inputs.find((port) => port.name === input);
  const inputType = inputPort?.data_type;
  if (!outputType || !inputType || outputType !== inputType) throw new Error('Ports must have the same data type.');
  if ((outputPort?.transport || 'in_process') !== (inputPort.transport || 'in_process')) throw new Error('Ports must use the same transport or an explicit bridge node.');
  if (outputPort?.delivery_mode && inputPort.delivery_mode && outputPort.delivery_mode !== inputPort.delivery_mode) throw new Error('Ports must have the same delivery mode.');
  if (provider.definition?.core !== consumer.definition?.core && (outputPort?.transport || 'in_process') === 'in_process') throw new Error('Cross-core edges need a declared transport and bridge.');
  const existing = consumer.node.inputs?.[input];
  if (existing && inputPort?.cardinality !== 'many') throw new Error('This input already has a provider.');
  if (existing && inputSources(existing).some((item) => sameNodePath(item.node, providerPath) && item.output === output)) throw new Error('This connection already exists.');
  const next = graphCopy(graph);
  const slot = nodeSlot(next, consumerPath)!;
  const target = slot.nodes[slot.id];
  const source: NodeSource = { node: canonicalNodePath(providerPath), output };
  target.inputs = { ...target.inputs, [input]: inputPort?.cardinality === 'many' ? [...(existing ? inputSources(existing) : []), source] : source };
  return next;
}

export function disconnectInput(graph: NodeGraph, consumerPath: string, input: string, providerPath?: string, output?: string): NodeGraph {
  const next = graphCopy(graph);
  const slot = nodeSlot(next, consumerPath);
  if (!slot || !slot.nodes[slot.id]?.inputs?.[input]) return graph;
  const current = slot.nodes[slot.id].inputs![input];
  if (providerPath) {
    const remaining = inputSources(current).filter((item) => !(sameNodePath(item.node, providerPath) && (!output || item.output === output)));
    if (remaining.length === inputSources(current).length) return graph;
    if (remaining.length) slot.nodes[slot.id].inputs![input] = Array.isArray(current) ? remaining : remaining[0];
    else delete slot.nodes[slot.id].inputs![input];
  } else delete slot.nodes[slot.id].inputs![input];
  return next;
}

/** A primitive binding commands one device; changing its target rewires only that binding. */
export function bindPrimitiveTarget(graph: NodeGraph, types: NodeTypeDefinition[], bindingPath: string, targetPath: string): NodeGraph {
  const binding = graphNodes(graph, types).find((entry) => sameNodePath(entry.path, bindingPath));
  if (!binding || !binding.node.node_type.startsWith('eos.dispatcher.primitive_')) throw new Error('Choose a primitive binding.');
  let next = graphCopy(graph);
  for (const entry of graphNodes(next, types)) {
    const value = entry.node.inputs?.command;
    if (value && inputSources(value).some((source) => sameNodePath(source.node, bindingPath))) next = disconnectInput(next, entry.path, 'command', bindingPath, 'command');
  }
  return targetPath ? connectNodes(next, types, bindingPath, 'command', targetPath, 'command') : next;
}
