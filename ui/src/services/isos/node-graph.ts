import { request } from './api-client';

/** One registered node type. Placement belongs to this definition, not an instance setting. */
export interface NodeTypeDefinition {
  id: string;
  description?: string;
  core: string;
  layer: string;
  group: string;
  kind: 'camera' | 'base' | 'manipulator' | 'software';
  lifecycle: 'internal' | 'process' | 'adapter';
  capabilities: string[];
  settings: SettingDefinition[];
  inputs: PortDefinition[];
  outputs: PortDefinition[];
  safety_adapter_type?: string | null;
}
export function catalogTypeKey(type: Pick<NodeTypeDefinition, 'core' | 'layer' | 'group' | 'id'>): string {
  return JSON.stringify([type.core, type.layer, type.group, type.id]);
}
export interface SettingDefinition { name: string; data_type: 'string' | 'integer' | 'number' | 'boolean' | 'array' | 'object' | 'positive_integer' | 'nonnegative_integer' | 'positive_number' | 'endpoint' | 'string_array' | 'string_table'; required: boolean; description?: string; unit?: string; min?: number; max?: number; choices?: string[]; default?: unknown; selector?: { provider: 'package' | 'registry' | 'graph' | 'ui'; resource: string; package_from?: string; filter?: Record<string, string> } }
export interface PortDefinition { name: string; data_type: string; required: boolean; cardinality?: 'one' | 'many'; description?: string; expected_rate_hz?: number | null; transport?: 'in_process' | 'unix_socket' | 'websocket' | 'ros2' | 'http' | string; delivery_mode?: 'request' | 'event' | 'stream' | 'snapshot' | string; data_class?: string }
export interface DataSchema { title?: string; description?: string; type?: string; properties?: Record<string, { type?: string; description?: string; 'x-unit'?: string; minimum?: number; maximum?: number }>; required?: string[]; [key: string]: unknown }
export interface DataTypeDefinition { id: string; version: string; purpose: string; media_type: string; codec: string; schema: DataSchema; max_payload_bytes: number }

export interface NodeSource { node: string; output: string; priority?: number; frequency_hz?: number; [key: string]: unknown }
export interface NodeInstance {
  id: string;
  node_type: string;
  label: string;
  description: string;
  package?: { id: string; version: string };
  capabilities?: string[];
  settings?: Record<string, unknown>;
  inputs?: Record<string, NodeSource | NodeSource[]>;
  references?: string[];
  frame_id?: string;
  joint_names?: string[];
  adapter?: { type: string; package?: { id: string; version: string }; settings?: Record<string, unknown> };
  health_timing?: { poll_interval_ms: number; stale_after_ms: number };
  observation?: { max_rate_hz: number; max_payload_bytes: number };
  movement?: { modes: string[]; limits: Record<string, number> };
  [key: string]: unknown;
}
export interface NodeGraph {
  version: string;
  robot: { id: string; name: string };
  cores: Record<string, { layers: Record<string, { groups: Record<string, { nodes: Record<string, NodeInstance> }> }> }>;
  [key: string]: unknown;
}
export interface NodeGraphDocument { revision: string; graph: NodeGraph; read_only?: boolean; diagnostics?: NodeGraphDiagnostic[] }
export interface NodeGraphDiagnostic { path: string; code?: string; message: string }
export interface ValidatedEdge { path: string; source: string; target: string; input: string; route_class: string; fixed: boolean; qos_eligible: boolean; routed: boolean; priority: number; frequency_hz?: number; data_type: string; transport: string }
export interface NodeGraphValidation { valid: boolean; diagnostics: NodeGraphDiagnostic[]; edges?: ValidatedEdge[] }
export interface ProcessStatus {
  schema_version: '1.0.0';
  saved_revision?: string;
  active_revision?: string | null;
  state?: 'stopped' | 'running' | 'degraded';
  last_error?: string | null;
  run_id?: string | null;
  run_id_state?: 'none' | 'known' | 'unavailable';
  vision_evidence?: import('../eos/vision').VisionEvidenceSelection | null;
  terminal?: StopTerminal | null;
  processes: { id: string; pid?: number; restarts: number; state?: string; last_error?: string | null }[];
}
/** Stop is a terminal owner reply, not a process-status snapshot. */
export interface StopTerminal { run_id: string; outcome: 'confirmed' | 'unknown'; reason_code: string | null; processes: { id: string; outcome: 'confirmed' | 'unknown'; reason_code: string | null }[] }
export interface StopControlReply { stopped: string[]; terminal: StopTerminal | null }
export interface InstalledPackage { schema_version: 1 | 2; id: string; version: string; source_commit: string | null; asset_sha256: string | null; local_link: string | null }
export interface NodeSetDefinition {
  package: { id: string; version: string };
  id: string;
  parameters: Record<string, { type: 'string'; default?: string }>;
  nodes: { ref: string; core: string; layer: string; group?: string; profile: NodeInstance }[];
}
export interface NodeSetPreview { graph: NodeGraph; added_paths: string[] }
export interface SetupStatus { ready?: boolean; issues?: string[]; [key: string]: unknown }
export interface LogLine { timestamp: string; level: string; source: string; message: string; core?: string; node_path?: string | null; event_code?: string; run_id?: string; correlation_id?: string | null; pid?: number | null; fields?: Record<string, string | number | boolean | null> }
export interface LogPage { lines: LogLine[]; next_cursor: string | null; rotated: boolean }
export type LogLevel = 'trace' | 'debug' | 'info' | 'warn' | 'error';
export interface LogScope { kind: 'manager_session' | 'deployment_run'; run_id: string; active_revision: string | null }
export interface LogAnchor { kind: 'first_prelaunch_failure' | 'first_ready_or_failure'; state: 'pending' | 'found' | 'unavailable'; event: LogLine | null }
export type LogDiagnosticCode = 'log_run_changed' | 'log_cursor_expired' | 'log_collector_gap' | 'log_anchor_unavailable';
export interface LogDiagnostic { code: LogDiagnosticCode; count: number | null }
export interface CurrentLogPage extends LogPage { scope: LogScope; anchor: LogAnchor; diagnostics: LogDiagnostic[]; next_cursor: string }
export interface CurrentLogQuery { source: string; level: LogLevel; node_path?: string; correlation_id?: string; cursor?: string; limit?: number }
export interface CoreReadinessEntry { core: string; path: string | null; version: string | null; compatible: boolean; configured: boolean; enabled?: boolean; graph_configured: boolean; running: boolean; ready: boolean; reason?: string | null; action?: string | null }
export interface CoreReadiness { cores: CoreReadinessEntry[] }
export interface LinkPreview { id: string; version: string; ready: boolean; issues: string[] }
export interface AdapterFamilyOption { id: string; device_kinds: string[]; package?: { id: string; version: string }; compiled: boolean; transport: string; description: string; available: boolean; reason?: string }
export interface PrimitiveBindingOption { node_type: string; device_kind: string; movement_types: string[]; available: boolean; reason?: string }
export interface HardwareTargetOption { path: string; node_type: string; device_kind: string; adapter_type: string; movement_types: string[]; available: boolean; reason?: string }
export interface SettingOption { value: string; label: string; description: string; available: boolean; reason_code: string | null; reason: string | null }
export interface SettingOptionGroup { node_path: string; setting: string; selected: string | null; options: SettingOption[] }
export interface NodeOptions { adapter_families: AdapterFamilyOption[]; primitive_bindings: PrimitiveBindingOption[]; targets: HardwareTargetOption[]; setting_options?: SettingOptionGroup[]; graph_revision?: string | null; read_only?: boolean }
export interface PrimitiveSpec { name: string; version: string; device_class: string; movement_type: string; description: string }

function isObject(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function isNodeSource(value: unknown): value is NodeSource {
  return isObject(value) && typeof value.node === 'string' && typeof value.output === 'string';
}

function isStringArray(value: unknown): value is string[] {
  return Array.isArray(value) && value.every((item) => typeof item === 'string');
}

/** A future graph may use a different tree; retain it for raw inspection. */
export function nodeGraphHasEditableShape(graph: unknown): graph is NodeGraph {
  if (!isObject(graph) || typeof graph.version !== 'string' || !isObject(graph.robot) || typeof graph.robot.id !== 'string' || typeof graph.robot.name !== 'string' || !isObject(graph.cores)) return false;
  for (const core of Object.values(graph.cores)) {
    if (!isObject(core) || !isObject(core.layers)) return false;
    for (const layer of Object.values(core.layers)) {
      if (!isObject(layer) || !isObject(layer.groups)) return false;
      for (const group of Object.values(layer.groups)) {
        if (!isObject(group) || !isObject(group.nodes)) return false;
        for (const node of Object.values(group.nodes)) {
          if (!isObject(node) || typeof node.id !== 'string' || typeof node.node_type !== 'string') return false;
          if (node.label !== undefined && typeof node.label !== 'string') return false;
          if (node.description !== undefined && typeof node.description !== 'string') return false;
          if (node.inputs !== undefined) {
            if (!isObject(node.inputs)) return false;
            for (const sources of Object.values(node.inputs)) {
              if (Array.isArray(sources) ? !sources.every(isNodeSource) : !isNodeSource(sources)) return false;
            }
          }
          if (node.settings !== undefined && !isObject(node.settings)) return false;
          if (node.package !== undefined && (!isObject(node.package) || typeof node.package.id !== 'string' || typeof node.package.version !== 'string')) return false;
          if (node.capabilities !== undefined && !isStringArray(node.capabilities)) return false;
          if (node.references !== undefined && !isStringArray(node.references)) return false;
          if (node.joint_names !== undefined && !isStringArray(node.joint_names)) return false;
          if (node.adapter !== undefined && (!isObject(node.adapter) || typeof node.adapter.type !== 'string' || (node.adapter.settings !== undefined && !isObject(node.adapter.settings)))) return false;
          if (node.health_timing !== undefined && (!isObject(node.health_timing) || typeof node.health_timing.poll_interval_ms !== 'number' || typeof node.health_timing.stale_after_ms !== 'number')) return false;
          if (node.observation !== undefined && (!isObject(node.observation) || typeof node.observation.max_rate_hz !== 'number' || typeof node.observation.max_payload_bytes !== 'number')) return false;
          if (node.movement !== undefined && (!isObject(node.movement) || !isStringArray(node.movement.modes) || !isObject(node.movement.limits) || Object.values(node.movement.limits).some((limit) => typeof limit !== 'number' || !Number.isFinite(limit)))) return false;
        }
      }
    }
  }
  return true;
}

/** Fail closed when the server or local catalog cannot establish graph compatibility. */
export function nodeGraphReadOnlyReason(document: NodeGraphDocument, types: NodeTypeDefinition[] | null): string | null {
  if (document.read_only) return document.diagnostics?.map((issue) => issue.message).join('; ') || 'IsOS opened this graph read-only.';
  if (!nodeGraphHasEditableShape(document.graph)) return 'This graph structure cannot be edited by this IsosUI build.';
  if (document.graph.version !== '0.2.0') return `Graph version ${document.graph.version} requires migration before editing.`;
  if (Object.keys(document.graph.cores).some((core) => core !== 'isos')) return 'Graph 0.2.0 must have only the isos core.';
  if (!types) return 'Node type catalog is unavailable.';
  const known = new Set(types.map(catalogTypeKey));
  for (const [coreId, core] of Object.entries(document.graph.cores)) {
    for (const [layerId, layer] of Object.entries(core.layers)) {
      for (const [groupId, group] of Object.entries(layer.groups)) {
        for (const [nodeId, node] of Object.entries(group.nodes)) {
          if (!known.has(catalogTypeKey({ core: coreId, layer: layerId, group: groupId, id: node.node_type }))) return `Unsupported node type ${node.node_type} at ${coreId}/${layerId}/${groupId}/${nodeId}.`;
        }
      }
    }
  }
  return null;
}

/** Exact instance pins are the only package requirement the editor interprets. */
export function graphPackagePins(graph: NodeGraph): { id: string; version: string }[] {
  const pins = new Map<string, { id: string; version: string }>();
  for (const core of Object.values(graph.cores)) for (const layer of Object.values(core.layers)) for (const group of Object.values(layer.groups)) for (const node of Object.values(group.nodes)) {
    if (node.package) pins.set(`${node.package.id}@${node.package.version}`, node.package);
  }
  return [...pins.values()];
}

export function getNodeGraph(): Promise<NodeGraphDocument> { return request('/graph'); }
export interface NodeTypeCatalog { types: NodeTypeDefinition[]; data_types?: DataTypeDefinition[]; primitive_specs?: PrimitiveSpec[] }
export interface GraphCatalogSnapshot { graph: NodeGraphDocument; selected: NodeTypeCatalog | null; global: NodeTypeCatalog | null; reason: string | null }
export function graphCatalogReadOnlyReason(snapshot: GraphCatalogSnapshot): string | null {
  if (snapshot.graph.read_only) return nodeGraphReadOnlyReason(snapshot.graph, snapshot.selected?.types || null);
  return snapshot.reason || nodeGraphReadOnlyReason(snapshot.graph, snapshot.selected?.types || null);
}
export function getNodeTypes(): Promise<NodeTypeCatalog> { return request('/catalog/node-types'); }
export function getSelectedNodeTypes(): Promise<{ graph_revision: string; catalog: NodeTypeCatalog }> { return request('/catalog/node-types/selected'); }
/** One bounded retry handles Apply between graph and catalog reads; mismatch never becomes editable. */
export async function getGraphCatalogSnapshot(): Promise<GraphCatalogSnapshot> {
  const global = await getNodeTypes().catch(() => null);
  let graph = await getNodeGraph();
  if (graph.graph.version !== '0.2.0') return { graph, selected: global, global, reason: null };
  for (let attempt = 0; attempt < 2; attempt += 1) {
    try {
      const response = await getSelectedNodeTypes();
      if (response.graph_revision === graph.revision) return { graph, selected: response.catalog, global, reason: null };
    } catch {
      return { graph, selected: null, global, reason: 'Selected node type catalog is unavailable.' };
    }
    if (attempt === 0) graph = await getNodeGraph();
  }
  return { graph, selected: null, global, reason: 'Selected node type catalog revision does not match the saved graph.' };
}
/** Selected owner types describe the applied graph; global types remain available for package discovery. */
export function combineNodeTypeCatalogs(selected: NodeTypeCatalog | null, global: NodeTypeCatalog | null): NodeTypeCatalog {
  const types = new Map<string, NodeTypeDefinition>();
  // Owner catalogs may omit empty arrays. Keep the editor's array contract stable
  // before a node is selected, since selection also happens during a drag.
  const normalize = (type: NodeTypeDefinition): NodeTypeDefinition => ({
    ...type,
    capabilities: Array.isArray(type.capabilities) ? type.capabilities : [],
    settings: Array.isArray(type.settings) ? type.settings : [],
    inputs: Array.isArray(type.inputs) ? type.inputs : [],
    outputs: Array.isArray(type.outputs) ? type.outputs : [],
  });
  for (const type of global?.types || []) types.set(catalogTypeKey(type), normalize(type));
  for (const type of selected?.types || []) types.set(catalogTypeKey(type), normalize(type));
  const dataTypes = new Map<string, DataTypeDefinition>();
  for (const type of global?.data_types || []) dataTypes.set(type.id, type);
  for (const type of selected?.data_types || []) dataTypes.set(type.id, type);
  return { types: [...types.values()], data_types: [...dataTypes.values()], primitive_specs: selected?.primitive_specs || global?.primitive_specs || [] };
}
export function getNodeSets(): Promise<{ sets: NodeSetDefinition[] }> { return request('/catalog/node-sets'); }
export function getInstalledPackages(): Promise<{ installed: InstalledPackage[] }> { return request('/packages'); }
export function previewNodeSet(set: NodeSetDefinition, graph: NodeGraph, values: Record<string, string>): Promise<NodeSetPreview> {
  return request('/catalog/node-sets/preview', 'POST', { package: set.package, set_id: set.id, graph, values });
}
export function getProcessStatus(signal?: AbortSignal): Promise<ProcessStatus> { return request('/runtime/process-status', 'GET', undefined, signal); }
export function installBackend(id: string, version: string): Promise<unknown> {
  return request('/packages/install', 'POST', { id, version });
}
export function linkBackend(id: string, version: string, path: string, manifest?: string): Promise<unknown> {
  return request('/packages/link', 'POST', { id, version, path, ...(manifest ? { manifest } : {}) });
}
export function getSetupStatus(): Promise<SetupStatus> { return request('/cores/setup-status'); }
export function controlProcesses(operation: 'run' | 'restart', revision?: string): Promise<ProcessStatus> {
  return request('/runtime/control', 'POST', { schema_version: '0.1.0', operation, ...(revision ? { revision } : {}) });
}
export function stopProcesses(): Promise<StopControlReply> {
  return request<StopControlReply>('/runtime/control', 'POST', { schema_version: '0.1.0', operation: 'stop' });
}
export function getCoreReadiness(): Promise<CoreReadiness> { return request('/cores/readiness'); }
export function previewBackendLink(path: string, manifest?: string): Promise<LinkPreview> {
  return request('/packages/preview-link', 'POST', { path, ...(manifest ? { manifest } : {}) });
}
export function getNodeOptions(graph: NodeGraph): Promise<NodeOptions> { return request('/catalog/node-options', 'POST', { schema_version: '0.1.0', graph }); }
export function getLogPage(source: string, level: LogLevel, cursor?: string, limit = 200): Promise<LogPage> {
  const params = new URLSearchParams({ source, level, limit: String(Math.max(1, Math.min(200, Math.floor(limit)))) });
  if (cursor) params.set('cursor', cursor);
  return request(`/runtime/logs?${params.toString()}`);
}
/** Request the opt-in P07 view; an older IsOS may still return the legacy shape. */
export async function getCurrentLogPage(query: CurrentLogQuery, signal?: AbortSignal): Promise<CurrentLogPage | LogPage> {
  const { source, level, node_path, correlation_id, cursor, limit = 100 } = query;
  const params = new URLSearchParams({ view: 'current', source, level, limit: String(Math.max(1, Math.min(200, Math.floor(limit)))) });
  if (node_path) params.set('node_path', node_path);
  if (correlation_id) params.set('correlation_id', correlation_id);
  if (cursor) params.set('cursor', cursor);
  const page = await request<unknown>(`/runtime/logs?${params.toString()}`, 'GET', undefined, signal);
  if (isCurrentLogPage(page)) return page;
  if (isObject(page) && Array.isArray(page.lines) && page.lines.every(isLogLine) && (typeof page.next_cursor === 'string' || page.next_cursor === null) && typeof page.rotated === 'boolean' && !('scope' in page)) return page as unknown as LogPage;
  throw new Error('IsOS returned an invalid current log page.');
}
function isLogLine(value: unknown): value is LogLine {
  return isObject(value) && typeof value.timestamp === 'string' && typeof value.level === 'string' && typeof value.source === 'string' && typeof value.message === 'string';
}
export function isCurrentLogPage(page: unknown): page is CurrentLogPage {
  if (!isObject(page) || !isObject(page.scope) || !isObject(page.anchor) || !Array.isArray(page.lines) || !Array.isArray(page.diagnostics)) return false;
  const scope = page.scope;
  const anchor = page.anchor;
  if (!['manager_session', 'deployment_run'].includes(String(scope.kind)) || typeof scope.run_id !== 'string' || !/^[A-Za-z0-9_-]{1,64}$/.test(scope.run_id) || (scope.active_revision !== null && typeof scope.active_revision !== 'string')) return false;
  if (!['first_prelaunch_failure', 'first_ready_or_failure'].includes(String(anchor.kind)) || !['pending', 'found', 'unavailable'].includes(String(anchor.state))) return false;
  if ((anchor.state === 'found') !== (isLogLine(anchor.event) && anchor.event.run_id === scope.run_id)) return false;
  if (anchor.state !== 'found' && anchor.event !== null) return false;
  if (!page.lines.every((line: unknown) => isLogLine(line) && line.run_id === scope.run_id)) return false;
  if (typeof page.next_cursor !== 'string' || page.next_cursor.length > 512 || !/^[\x21-\x7E]*$/.test(page.next_cursor) || typeof page.rotated !== 'boolean') return false;
  const codes = ['log_run_changed', 'log_cursor_expired', 'log_collector_gap', 'log_anchor_unavailable'];
  return page.diagnostics.every((item: unknown) => isObject(item) && codes.includes(String(item.code)) && (item.count === null || (Number.isSafeInteger(item.count) && Number(item.count) >= 0)));
}
export function validateNodeGraph(graph: NodeGraph): Promise<NodeGraphValidation> {
  return request('/graph/validate', 'POST', { schema_version: '0.1.0', graph });
}
export function applyNodeGraph(revision: string, graph: NodeGraph): Promise<NodeGraphDocument> {
  return request('/graph', 'PUT', { schema_version: '0.1.0', revision, graph });
}
