import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import ReactFlow, { Background, Handle, MiniMap, Position, ReactFlowProvider, SelectionMode, type Connection, type Edge, type Node, type NodeChange, type NodeProps, type ReactFlowInstance } from 'reactflow';
import 'reactflow/dist/style.css';
import { catalogTypeKey, combineNodeTypeCatalogs, type NodeOptions, getGraphCatalogSnapshot, getNodeGraph, getNodeSets, getProcessStatus, getCoreReadiness, getNodeOptions, graphCatalogReadOnlyReason, nodeGraphHasEditableShape, nodeGraphReadOnlyReason, previewNodeSet, validateNodeGraph, type CoreReadiness, type DataTypeDefinition, type NodeGraph, type NodeGraphDiagnostic, type NodeInstance, type NodeSetDefinition, type NodeSetPreview, type NodeTypeDefinition, type ProcessStatus, type ValidatedEdge } from '@/services/isos/node-graph';
import type { Language } from '@/types';
import { hasActiveDeployment } from './deployment-phase';
import { addNode, bindPrimitiveTarget, canonicalNodePath, connectNodes, disconnectInput, graphNodes, inputSources, nextNodeId, nodePath, removeNode, removedNodePaths, renameNode, sameNodePath, updateNode, type GraphNode } from './node-graph-edits';
import { interfaceVisual, nodeVisualStyle, portVisual, dataTypeColor } from './node-visuals';
import { applyNodeSelection, graphLayoutKey, graphMirrorKey, parseNodeMirrors, parseNodePositions, removeSelectedNodes, type NodeMirrors, type NodePositions } from './graph-canvas-state';
import { DISPATCHER_PATH, withControlTarget, withSkillSteps } from './control-node-config';
import { GraphCoreWindow, readCoreWindow, type CoreWindowState } from './GraphCoreWindow';
import { edgeContracts, edgeInputDiagnostics } from './edge-idl';
import { edgeSource, updateEdgeQos, validatedSource } from './edge-qos';
import { EdgeQosInspector } from './EdgeQosInspector';
import { validatedApply } from './validated-apply';
import { EgiaWorkbench } from './EgiaWorkbench';
import { clampInfoCard } from './node-info-card';
import { getNodeLive } from '@/services/eos/node-live';
import { processRunFeedback } from './process-run-feedback';
import { stopReplyFeedback, stopFailureFeedback } from './stop-feedback';
import { requestRunStop } from './run-stop-fence';
import { deploymentStopEpoch, runDeployment, stopDeploymentConverged } from './deployment-control';
import { browserLog } from '@/services/isos/browser-logs';
import { publishUiMessage } from '@/services/isos/ui-messages';
import { draftValidationStamp, validationDisplayIsCurrent, validationResponseIsCurrent, type DraftValidationStamp } from './draft-validation';
import { CatalogSelector, settingOptionGroup } from './CatalogSelector';
import { CatalogBrowser } from './CatalogBrowser';
import { KeyboardNodeControl } from './KeyboardNodeControl';
import { UiKeySelector } from './UiKeySelector';
import { catalogReloadGraph, catalogReloadIsCurrent, catalogSelectionBlocks, optionResponseIsCurrent, type OptionSnapshot } from './catalog-options';
import './node-graph-manager.scss';

interface Props { lang: Language; visible?: boolean; onDirtyChange?: (dirty: boolean) => void; onOpenPanel?: (name: 'control' | 'logs') => void; onStopIntent?: (runId: string | null) => void; onStopOutcome?: (feedback: { text: string; alert: boolean }, terminalRun?: string | null) => void }
interface FlowData { entry: GraphNode; error: boolean; mirrored: boolean; lang: Language; liveAllowed: boolean; liveRunId: string | null; dataTypes: DataTypeDefinition[] }
const COPY = {
  en: { nodeSets: 'Package node sets', previewRemoves: 'Package preview would remove existing nodes. Edit the graph explicitly before adding this set.', noNodeSets: 'No installed package offers a node set.', chooseSet: 'Choose a node set', previewSet: 'Preview nodes', addSet: 'Add previewed nodes', previewCount: 'nodes will be added', previewStale: 'The graph changed. Preview the set again.', title: 'Node graph', connect: 'Connect to local IsOS', add: 'Add node', refresh: 'Refresh', validate: 'Check graph', apply: 'Save graph', saved: 'Graph saved and core configuration generated. Restart to use this revision.', valid: 'Graph is valid.', dirty: 'Unsaved changes', clean: 'Saved', catalog: 'Node library', inspector: 'Node configuration', diagnostics: 'Diagnostics', noSelection: 'Select a node to edit it.', selectedCount: 'nodes selected', noNodes: 'Add a registered node type to start.', id: 'Node ID', label: 'Label', description: 'Description', settings: 'Settings', gui: 'Show MuJoCo window when EOS starts this backend', details: 'Hardware and advanced fields (JSON)', applyDetails: 'Apply fields', remove: 'Remove node', capabilities: 'Capabilities', source: 'Source', target: 'Target', invalid: 'Invalid JSON object.', duplicate: 'Node ID already exists or is invalid.', port: 'Connect matching typed ports by dragging from an output to an input.', robot: 'Robot', name: 'Name', lifecycle: 'Lifecycle', type: 'Type', disconnected: 'IsOS node graph API is unavailable.', stale: 'Graph changed on the server. Refresh and retry.', discard: 'Discard unsaved changes?', choose: 'Choose a type', process: 'Process status', noStatus: 'Process status is not available from this IsOS build.', search: 'Search types', undo: 'Undo', redo: 'Redo', disconnect: 'Disconnect line', hint: 'Left drag selects nodes; middle drag pans. Right-click to add or disconnect. Delete removes selected nodes.', savedRevision: 'Saved', activeRevision: 'Active' },
  cn: { nodeSets: '软件包节点集', previewRemoves: '软件包预览会删除现有节点。请先明确编辑节点图，再添加此节点集。', noNodeSets: '已安装的软件包没有提供节点集。', chooseSet: '选择节点集', previewSet: '预览节点', addSet: '添加已预览节点', previewCount: '个节点将被添加', previewStale: '节点图已更改，请重新预览。', title: '节点图', connect: '连接本机 IsOS', add: '添加节点', refresh: '刷新', validate: '检查节点图', apply: '保存节点图', saved: '节点图已保存，核心配置已生成。重启后使用此修订。', valid: '节点图有效。', dirty: '未保存更改', clean: '已保存', catalog: '节点库', inspector: '节点配置', diagnostics: '诊断', noSelection: '选择节点后编辑配置。', selectedCount: '个节点已选中', noNodes: '添加已注册的节点类型以开始。', id: '节点 ID', label: '标签', description: '描述', settings: '设置', gui: 'EOS 启动此后端时显示 MuJoCo 窗口', details: '硬件与高级字段（JSON）', applyDetails: '应用字段', remove: '删除节点', capabilities: '能力', source: '来源', target: '目标', invalid: 'JSON 对象无效。', duplicate: '节点 ID 已存在或无效。', port: '从输出端口拖到类型相同的输入端口以连接。', robot: '机器人', name: '名称', lifecycle: '生命周期', type: '类型', disconnected: 'IsOS 节点图 API 不可用。', stale: '服务器上的图已更改。刷新后重试。', discard: '放弃未保存的更改？', choose: '选择类型', process: '进程状态', noStatus: '当前 IsOS 版本未提供进程状态。', search: '搜索类型', undo: '撤销', redo: '重做', disconnect: '断开连线', hint: '左键拖动框选节点，中键拖动平移。右键添加节点或断开连线。Delete 删除选中节点。', savedRevision: '已保存', activeRevision: '运行中' },
} as const;

export function FlowNode({ data, selected }: NodeProps<FlowData>): React.ReactElement {
  const { entry } = data;
  const [open, setOpen] = useState(false);
  const [location, setLocation] = useState({ left: 8, top: 8 });
  const [live, setLive] = useState<{ status: string; health: string; at: number } | null>(null);
  const [liveClock, setLiveClock] = useState(Date.now());
  const hideTimer = useRef<number | null>(null);
  const hoverTimer = useRef<number | null>(null);
  const pointerFocus = useRef(false);
  const cardRef = useRef<HTMLElement | null>(null);
  const place = (x: number, y: number): void => setLocation(clampInfoCard(x + 16, y + 16, 340, 340, window.innerWidth, window.innerHeight));
  const cancelHover = (): void => { if (hoverTimer.current !== null) { window.clearTimeout(hoverTimer.current); hoverTimer.current = null; } };
  const show = (x: number, y: number): void => { cancelHover(); if (hideTimer.current !== null) window.clearTimeout(hideTimer.current); place(x, y); setOpen(true); };
  const showAfterPause = (x: number, y: number): void => { if (open || pointerFocus.current) return; cancelHover(); hoverTimer.current = window.setTimeout(() => show(x, y), 550); };
  const hideSoon = (): void => { cancelHover(); hideTimer.current = window.setTimeout(() => setOpen(false), 120); };
  useEffect(() => () => { cancelHover(); if (hideTimer.current !== null) window.clearTimeout(hideTimer.current); }, []);
  useEffect(() => { if (!open || !data.liveAllowed || !/^eos\.hardware\.(camera|base|manipulator)@v0\.1$/.test(entry.node.node_type)) { setLive(null); return; }
    let disposed = false; let pending = false;
    const poll = async (): Promise<void> => {
      const now = Date.now(); setLiveClock(now);
      if (document.visibilityState !== 'visible') { setLive(null); return; }
      if (pending) return;
      pending = true;
      try { const value = await getNodeLive(entry.path); if (!disposed) { const at = Date.now(); setLive({ ...value, at }); setLiveClock(at); } }
      catch { if (!disposed) setLive(null); }
      finally { pending = false; }
    };
    void poll(); const timer = window.setInterval(() => { void poll(); }, 2000);
    return () => { disposed = true; window.clearInterval(timer); };
  }, [open, data.liveAllowed, data.liveRunId, entry.path, entry.node.node_type]);
  useEffect(() => { if (!open) return; const clamp = (): void => setLocation((value) => clampInfoCard(value.left, value.top, 340, 340, window.innerWidth, window.innerHeight)); window.addEventListener('resize', clamp); return () => window.removeEventListener('resize', clamp); }, [open]);
  const labels = data.lang === 'cn' ? { info: '节点信息', type: '类型描述', instance: '实例描述', input: '输入', output: '输出', status: 'EOS 实时状态', stale: 'EOS 状态不可用或已过期', missing: '目录无描述（兼容版本）', close: '关闭' } : { info: 'Node information', type: 'Type description', instance: 'Instance description', input: 'Inputs', output: 'Outputs', status: 'EOS live status', stale: 'EOS status unavailable or stale', missing: 'No catalog description (compatibility version)', close: 'Close' };
  return <div className={`node-graph-card${selected ? ' node-graph-card--selected' : ''}${data.error ? ' node-graph-card--error' : ''}${data.mirrored ? ' node-graph-card--mirrored' : ''}`} style={nodeVisualStyle(entry.definition) as React.CSSProperties} tabIndex={0} aria-label={`${entry.node.label || entry.node.id}: ${entry.node.node_type}`} onMouseEnter={(event) => showAfterPause(event.clientX, event.clientY)} onMouseMove={(event) => showAfterPause(event.clientX, event.clientY)} onMouseLeave={hideSoon} onPointerDown={() => { pointerFocus.current = true; cancelHover(); setOpen(false); }} onPointerUp={() => { pointerFocus.current = false; }} onPointerCancel={() => { pointerFocus.current = false; }} onFocus={(event) => { if (pointerFocus.current) return; const bounds = event.currentTarget.getBoundingClientRect(); show(bounds.right, bounds.top); }} onBlur={(event) => { if (!cardRef.current?.contains(event.relatedTarget as globalThis.Node)) hideSoon(); }} onKeyDown={(event) => { if (event.key === 'Escape') { setOpen(false); event.stopPropagation(); } }}>
    <div className="node-graph-card__heading"><small>{entry.definition?.layer || 'Unknown layer'} / {entry.definition?.group || 'unknown'}</small><span>{entry.node.node_type.split('@').pop() || 'v1.0'}</span></div>
    <strong>{entry.node.label || entry.node.id}</strong><button type="button" className="node-graph-card__info nodrag nowheel" aria-label={`${labels.info}: ${entry.node.label || entry.node.id}`} aria-expanded={open} onPointerDown={(event) => event.stopPropagation()} onClick={(event) => { event.stopPropagation(); const bounds = event.currentTarget.getBoundingClientRect(); show(bounds.right, bounds.top); }}>ⓘ</button>
    <div className="node-graph-card__badges"><span>{entry.definition?.core || 'unknown'}</span><span>{entry.definition?.kind || 'unknown'}</span>{entry.node.package && <span>{entry.node.package.id}@{entry.node.package.version}</span>}</div>
    <div className="node-graph-card__ports"><div>{entry.definition?.inputs.map((port) => <div className={`node-graph-card__port ${portVisual(port).className}`} style={portVisual(port).style as React.CSSProperties} key={port.name}><Handle type="target" position={data.mirrored ? Position.Right : Position.Left} id={`in:${port.name}`} title={`${port.name}: ${port.data_type} · ${port.transport || 'in_process'} · ${port.delivery_mode || 'snapshot'}`} /><span>{port.name}{port.required ? ' *' : ''}</span><small>{port.data_type}</small></div>)}</div><div>{entry.definition?.outputs.map((port) => <div className={`node-graph-card__port node-graph-card__port--output ${portVisual(port).className}`} style={portVisual(port).style as React.CSSProperties} key={port.name}><span>{port.name}</span><small>{port.data_type}</small><Handle type="source" position={data.mirrored ? Position.Left : Position.Right} id={`out:${port.name}`} title={`${port.name}: ${port.data_type} · ${port.transport || 'in_process'} · ${port.delivery_mode || 'snapshot'}`} /></div>)}</div></div>
    {open && createPortal(<aside ref={cardRef} className="node-graph-info-card nodrag nowheel" role="note" aria-label={`${labels.info}: ${entry.node.label || entry.node.id}`} style={location} onMouseEnter={(event) => { event.stopPropagation(); if (hideTimer.current !== null) window.clearTimeout(hideTimer.current); }} onMouseLeave={(event) => { event.stopPropagation(); hideSoon(); }} onPointerDown={(event) => event.stopPropagation()} onKeyDown={(event) => { if (event.key === 'Escape') { setOpen(false); event.stopPropagation(); } }}><button type="button" onClick={() => setOpen(false)} aria-label={labels.close}>×</button><h3>{entry.node.label || entry.node.id}</h3><code>{entry.node.node_type}</code><p><strong>{labels.type}:</strong> {entry.definition?.description || labels.missing}</p><p><strong>{labels.instance}:</strong> {entry.node.description || '—'}</p>{([['input', entry.definition?.inputs], ['output', entry.definition?.outputs]] as const).map(([direction, ports]) => <section key={direction}><h4>{labels[direction]}</h4><ul>{ports?.map((port) => { const schema = data.dataTypes.find((item) => item.id === port.data_type)?.schema; const units = [...new Set(Object.values(schema?.properties || {}).map((field) => field['x-unit']).filter(Boolean))]; return <li key={port.name}><code>{port.name}</code> · {port.data_type}{units.length ? ` · ${units.join(', ')}` : ''}{port.description ? ` · ${port.description}` : ''}{port.transport ? ` · ${port.transport}` : ''}{port.delivery_mode ? ` · ${port.delivery_mode}` : ''}</li>; })}</ul></section>)}{data.liveAllowed && /^eos\.hardware\.(camera|base|manipulator)@v0\.1$/.test(entry.node.node_type) && <p role="status"><strong>{labels.status}:</strong> {live && liveClock - live.at <= 5000 ? `${live.status} · ${live.health}` : labels.stale}</p>}</aside>, document.body)}
  </div>;
}
const nodeTypes = { profile: FlowNode };
function DataIdl({ definition, lang }: { definition: DataTypeDefinition; lang: Language }): React.ReactElement {
  const fields = Object.entries(definition.schema.properties || {});
  return <><h3>{definition.schema.title || definition.id}</h3><p><code>{definition.id}@{definition.version}</code> · {definition.media_type} · {definition.codec}</p><p>{definition.purpose}</p><p>{definition.schema.description}</p><p>{lang === 'cn' ? '最大负载' : 'Maximum payload'}: {definition.max_payload_bytes} bytes</p><dl>{fields.map(([name, field]) => <React.Fragment key={name}><dt>{name} <small>{field.type || 'value'}{field['x-unit'] ? ` · ${field['x-unit']}` : ''}{definition.schema.required?.includes(name) ? ' *' : ''}</small></dt><dd>{field.description || '—'}{field.minimum !== undefined || field.maximum !== undefined ? ` (${field.minimum ?? '−∞'}…${field.maximum ?? '+∞'})` : ''}</dd></React.Fragment>)}</dl></>;
}
function EdgeIdlPanel({ edge, entries, dataTypes, diagnostics, validated, processStatus, onOpenMonitor, lang, graph, validatedEdges, readOnly, onQosChange, onInvalidChange }: { edge: Edge; entries: GraphNode[]; dataTypes: DataTypeDefinition[]; diagnostics: NodeGraphDiagnostic[]; validated: boolean; processStatus: ProcessStatus | null; onOpenMonitor: () => void; lang: Language; graph: NodeGraph; validatedEdges: ValidatedEdge[]; readOnly: boolean; onQosChange: (change: { priority?: number; frequency_hz?: number | null }) => void; onInvalidChange: (invalid: boolean) => void }): React.ReactElement {
  const { sourceType, targetType, sourceIdl, targetIdl } = edgeContracts(entries, edge, dataTypes);
  const inputIssues = validated ? edgeInputDiagnostics({ ...edge, inputIndex: edge.data?.inputIndex, inputCount: edge.data?.inputCount }, diagnostics) : [];
  const typeUnknown = !sourceType || !targetType;
  const typeMismatch = !typeUnknown && sourceType !== targetType;
  const missing = lang === 'cn' ? '目录中没有此数据类型的 IDL。' : 'No IDL for this data type in the catalog.';
  const input = String(edge.targetHandle).replace(/^in:/, '');
  const index = edge.data?.inputIndex ?? 0;
  const selectedSource = edgeSource(graph, edge.target, input, index);
  const authority = selectedSource ? validatedSource(validatedEdges, edge.target, input, index, selectedSource.array) : undefined;
  const producerHint = entries.find((entry) => entry.path === edge.source)?.definition?.outputs.find((port) => port.name === String(edge.sourceHandle).replace(/^out:/, ''))?.expected_rate_hz;
  return <section className="node-graph-edge-idl"><h2>{lang === 'cn' ? '数据接口 IDL' : 'Data interface IDL'}</h2><p><code>{edge.source}.{String(edge.sourceHandle).replace('out:', '')}</code> → <code>{edge.target}.{String(edge.targetHandle).replace('in:', '')}</code></p><p>{edge.label}</p><p>{lang === 'cn' ? '来源类型' : 'Source type'}: <code>{sourceType || '—'}</code></p><p>{lang === 'cn' ? '目标类型' : 'Target type'}: <code>{targetType || '—'}</code></p><h3>{lang === 'cn' ? '草稿连接检查' : 'Draft connection check'}</h3><p role={typeMismatch || inputIssues.length ? 'alert' : 'status'}>{typeMismatch ? (lang === 'cn' ? '端口类型不匹配。' : 'Port types do not match.') : typeUnknown ? (lang === 'cn' ? '端口类型元数据不可用；请检查节点图。' : 'Port type metadata is unavailable; check the graph.') : !validated ? (lang === 'cn' ? '尚未检查当前草稿。' : 'Current draft has not been checked.') : inputIssues.length ? (lang === 'cn' ? 'IsOS 拒绝了此输入端口。' : 'IsOS rejected this input port.') : (lang === 'cn' ? '此输入端口没有 IsOS 诊断。' : 'No IsOS diagnostic for this input port.')}</p>{inputIssues.map((issue, index) => <p className="node-graph-error" key={index}><code>{issue.code || issue.path}</code>: {issue.message}</p>)}{selectedSource && <EdgeQosInspector source={selectedSource.source} authority={authority} current={validated} readOnly={readOnly} lang={lang} producerHint={producerHint} onChange={onQosChange} onInvalidChange={onInvalidChange} />}<h3>{lang === 'cn' ? '运行状态' : 'Runtime state'}</h3><p>{lang === 'cn' ? '部署' : 'Deployment'}: {processStatus?.state || (lang === 'cn' ? '未知' : 'unknown')}. {lang === 'cn' ? '节点实时健康请查看 EOS 监视器。' : 'See the EOS monitor for live node health.'}</p><button type="button" onClick={onOpenMonitor}>{lang === 'cn' ? '打开 EOS 监视器' : 'Open EOS monitor'}</button>{sourceIdl ? <DataIdl definition={sourceIdl} lang={lang} /> : <p>{missing}</p>}{targetType && targetType !== sourceType && <><h3>{lang === 'cn' ? '目标接口' : 'Target interface'}</h3>{targetIdl ? <DataIdl definition={targetIdl} lang={lang} /> : <p>{missing}</p>}</>}</section>;
}
const DETAIL_FIELDS = ['frame_id', 'joint_names', 'adapter', 'health_timing', 'observation', 'movement', 'references'] as const;

export function settingValue(value: unknown, dataType: string): string {
  if (dataType === 'endpoint') return JSON.stringify(value ?? null, null, 2);
  if (dataType === 'string_array') return Array.isArray(value) ? value.join('\n') : '';
  if (dataType === 'string_table' || dataType === 'array' || dataType === 'object') return JSON.stringify(value ?? (dataType === 'array' ? [] : {}), null, 2);
  return value === undefined || value === null ? '' : String(value);
}
export function parseSetting(raw: string, dataType: string): unknown {
  if (dataType === 'boolean') { if (raw !== 'true' && raw !== 'false') throw new Error('Expected true or false.'); return raw === 'true'; }
  if (dataType === 'array' || dataType === 'object') { const parsed: unknown = JSON.parse(raw); if (dataType === 'array' ? !Array.isArray(parsed) : !parsed || Array.isArray(parsed) || typeof parsed !== 'object') throw new Error(`Expected a JSON ${dataType}.`); return parsed; }
  if (dataType === 'string_array') return raw.split('\n').map((part) => part.trim()).filter(Boolean);
  if (dataType === 'string_table') { const parsed: unknown = JSON.parse(raw); if (!parsed || Array.isArray(parsed) || typeof parsed !== 'object' || Object.values(parsed).some((value) => typeof value !== 'string')) throw new Error('Expected an object of strings.'); return parsed; }
  if (dataType === 'positive_integer' || dataType === 'nonnegative_integer') { const number = Number(raw); if (!Number.isInteger(number) || (dataType === 'positive_integer' ? number < 1 : number < 0)) throw new Error('Expected a valid integer.'); return number; }
  if (dataType === 'positive_number') { const number = Number(raw); if (!Number.isFinite(number) || number <= 0) throw new Error('Expected a positive number.'); return number; }
  if (dataType === 'number' || dataType === 'integer') { const number = Number(raw); if (!Number.isFinite(number) || (dataType === 'integer' && !Number.isInteger(number))) throw new Error(`Expected a valid ${dataType}.`); return number; }
  return raw;
}

function SettingEditor({ name, dataType, required, value, onChange, description, unit, min, max, choices, lang = 'en' }: { name: string; dataType: string; required: boolean; value: unknown; onChange: (value: unknown) => void; description?: string; unit?: string; min?: number; max?: number; choices?: string[]; lang?: Language }): React.ReactElement {
  const helpId = React.useId();
  const [draft, setDraft] = useState(() => settingValue(value, dataType));
  const [error, setError] = useState('');
  useEffect(() => { setDraft(settingValue(value, dataType)); setError(''); }, [value, dataType]);
  const commit = (): void => { try { if (!draft.trim() && !required) { onChange(undefined); setError(''); return; } const parsed = parseSetting(draft, dataType); if (typeof parsed === 'number' && (min !== undefined && parsed < min || max !== undefined && parsed > max)) throw new Error(`Expected ${min ?? '−∞'}…${max ?? '+∞'}.`); onChange(parsed); setError(''); } catch (issue) { setError(issue instanceof Error ? issue.message : String(issue)); } };
  const guidance = `${dataType}${unit ? ` · ${unit}` : ''}${description ? ` · ${description}` : ''}`;
  const effectiveChoices = dataType === 'boolean' ? ['true', 'false'] : choices;
  return <label className="node-graph-setting"><span>{name}{required ? ' *' : ''}</span>{dataType === 'endpoint' ? <><pre aria-describedby={helpId}>{draft}</pre><small>{lang === 'cn' ? '端点由归属方管理；请在归属方配置中修改。' : 'Owner-managed endpoint; edit the owning configuration.'}</small></> : effectiveChoices?.length ? <select aria-describedby={helpId} value={draft} onChange={(event) => { setDraft(event.target.value); onChange(dataType === 'boolean' ? event.target.value === 'true' : event.target.value); }}><option value="">—</option>{!effectiveChoices.includes(draft) && draft && <option value={draft}>{draft}</option>}{effectiveChoices.map((choice) => <option key={choice} value={choice}>{choice}</option>)}</select> : ['string_array', 'string_table', 'array', 'object'].includes(dataType) ? <textarea aria-describedby={helpId} rows={dataType === 'string_table' || dataType === 'object' ? 4 : 3} value={draft} onChange={(event) => setDraft(event.target.value)} onBlur={commit} /> : <input aria-describedby={helpId} type={dataType.includes('integer') || dataType === 'positive_number' || dataType === 'number' ? 'number' : 'text'} step={dataType === 'positive_number' || dataType === 'number' ? 'any' : undefined} min={min} max={max} value={draft} onChange={(event) => setDraft(event.target.value)} onBlur={commit} onKeyDown={(event) => { if (event.key === 'Enter') event.currentTarget.blur(); }} />}<small id={helpId} className="node-graph-setting__metadata">{guidance}{min !== undefined || max !== undefined ? ` · ${min ?? '−∞'}…${max ?? '+∞'}` : ''}</small>{error && <small role="alert" className="node-graph-error">{error}</small>}</label>;
}

function HardwareFields({ node, path, definition, options, lang, onChange }: { node: NodeInstance; path: string; definition: NodeTypeDefinition; options: NodeOptions | null; lang: Language; onChange: (change: Partial<NodeInstance>) => void }): React.ReactElement {
  const adapter = node.adapter || { type: '', settings: {} };
  const adapterSettings = adapter.settings || {};
  const setAdapter = (key: string, value: unknown): void => onChange({ adapter: { ...adapter, settings: { ...adapterSettings, [key]: value } } });
  const deviceGroup = settingOptionGroup(options, path, 'adapter.settings.device_id');
  const simulatorGroup = settingOptionGroup(options, path, 'adapter.settings.simulator_node');
  const timing = node.health_timing;
  const observation = node.observation;
  const movement = node.movement;
  const limits = definition.kind === 'base' ? ['max_linear_m_s', 'max_angular_rad_s', 'max_velocity_lease_ms'] : definition.kind === 'manipulator' ? ['max_joint_position_rad', 'max_joint_speed_rad_s'] : [];
  return <div className="node-graph-hardware-fields">
    <h3>Adapter</h3>
    <label>{lang === 'cn' ? '适配器族' : 'Adapter family'}<select value={adapter.type} onChange={(event) => onChange({ adapter: { ...adapter, type: event.target.value } })}><option value={adapter.type}>{adapter.type}</option>{options?.adapter_families.filter((option) => option.device_kinds.includes(definition.kind) && option.id !== adapter.type).map((option) => <option key={option.id} value={option.id} disabled={!option.available}>{option.id}{option.available ? '' : ` — ${option.reason || 'unavailable'}`}</option>)}</select></label>
    {options?.adapter_families.find((option) => option.id === adapter.type)?.reason && <small role="status">{options.adapter_families.find((option) => option.id === adapter.type)?.reason}</small>}
    {(['backend', 'socket_path'] as const).map((key) => <SettingEditor key={`${node.id}:${key}`} name={key} dataType="string" required value={adapterSettings[key]} onChange={(value) => setAdapter(key, value)} />)}
    {deviceGroup ? <CatalogSelector group={deviceGroup} pin={node.adapter?.package} label="device_id" value={String(adapterSettings.device_id || '')} lang={lang} onChange={(value) => setAdapter('device_id', value)} /> : <SettingEditor name="device_id" dataType="string" required value={adapterSettings.device_id} onChange={(value) => setAdapter('device_id', value)} />}
    {simulatorGroup ? <CatalogSelector group={simulatorGroup} pin={node.adapter?.package} label="simulator_node" value={String(adapterSettings.simulator_node || '')} lang={lang} onChange={(value) => setAdapter('simulator_node', value)} /> : adapterSettings.simulator_node !== undefined ? <SettingEditor name="simulator_node" dataType="string" required value={adapterSettings.simulator_node} onChange={(value) => setAdapter('simulator_node', value)} /> : null}
    <SettingEditor name="frame_id" dataType="string" required value={node.frame_id} onChange={(value) => onChange({ frame_id: String(value) })} />
    {definition.kind === 'manipulator' && <SettingEditor name="joint_names" dataType="string_array" required value={node.joint_names} onChange={(value) => onChange({ joint_names: value as string[] })} />}
    <h3>Health timing</h3>
    {(['poll_interval_ms', 'stale_after_ms'] as const).map((key) => <SettingEditor key={`${node.id}:${key}`} name={key} dataType="positive_integer" required unit="ms" value={timing?.[key]} onChange={(value) => onChange({ health_timing: { ...timing, [key]: value as number } as NodeInstance['health_timing'] })} />)}
    <h3>Observation</h3>
    <label>max_rate_hz<small>number · Hz</small><input type="number" min={0.1} step="0.1" value={observation?.max_rate_hz ?? ''} onChange={(event) => { const value = Number(event.target.value); if (Number.isFinite(value) && value > 0) onChange({ observation: { ...observation, max_rate_hz: value } as NodeInstance['observation'] }); }} /></label>
    <SettingEditor name="max_payload_bytes" dataType="positive_integer" required unit="bytes" value={observation?.max_payload_bytes} onChange={(value) => onChange({ observation: { ...observation, max_payload_bytes: value as number } as NodeInstance['observation'] })} />
    {limits.length > 0 && <><h3>Movement limits</h3><SettingEditor name="modes" dataType="string_array" required value={movement?.modes} onChange={(value) => onChange({ movement: { ...movement, modes: value as string[] } as NodeInstance['movement'] })} />{limits.map((key) => <label key={key}>{key}<small>number · {key.endsWith('_ms') ? 'ms' : key.includes('rad') ? 'rad or rad/s' : 'm/s'}</small><input type="number" min={0} step="any" value={movement?.limits[key] ?? ''} onChange={(event) => { const value = Number(event.target.value); if (Number.isFinite(value) && value >= 0) onChange({ movement: { ...movement, limits: { ...movement?.limits, [key]: value } } as NodeInstance['movement'] }); }} /></label>)}</>}
  </div>;
}

function ControlNodeSettings({ entry, entries, lang, onChange }: { entry: GraphNode; entries: GraphNode[]; lang: Language; onChange: (change: Partial<NodeInstance>) => void }): React.ReactElement {
  const isSkill = entry.node.node_type === 'eos.control.skill@v1.0';
  const isPrimitive = entry.node.node_type === 'eos.control.primitive@v1.0';
  const dispatcher = entries.some(({ path, node }) => path === DISPATCHER_PATH && node.node_type === 'eos.dispatcher.control@v1.0');
  const hardware = entries.filter(({ node }) => node.node_type === 'eos.hardware.base@v1.0' || node.node_type === 'eos.hardware.manipulator@v1.0');
  const primitives = entries.filter(({ node }) => node.node_type === 'eos.control.primitive@v1.0' && node.settings?.target);
  const targetPath = typeof entry.node.settings?.target === 'string' ? entry.node.settings.target : '';
  const target = hardware.find(({ path }) => path === targetPath);
  const steps = Array.isArray(entry.node.settings?.steps) ? entry.node.settings.steps.filter((value): value is string => typeof value === 'string') : [];
  const [nextStep, setNextStep] = useState('');
  const labels = lang === 'cn' ? { target: '目标硬件', mode: '运动模式', steps: '原语步骤（按顺序）', add: '添加步骤', up: '上移', down: '下移', remove: '移除', dispatcher: '需要 ID 为 command 的 EOS 调度器节点。', empty: '先添加并配置原语节点。' }
    : { target: 'Target hardware', mode: 'Movement mode', steps: 'Primitive steps (in order)', add: 'Add step', up: 'Move up', down: 'Move down', remove: 'Remove', dispatcher: 'Add the EOS dispatcher node with ID command.', empty: 'Add and configure a primitive node first.' };
  const setSteps = (updated: string[]): void => onChange(withSkillSteps(entry.node, updated));
  return <div className="node-graph-control-settings">
    {!dispatcher && <p role="alert" className="node-graph-error">{labels.dispatcher}</p>}
    {isSkill ? <><h3>{labels.steps}</h3>{primitives.length || steps.length ? <><ol className="node-graph-control-steps">{steps.map((path, index) => <li key={`${index}:${path}`}><span>{primitives.find((item) => item.path === path)?.node.label || path}</span><div><button type="button" aria-label={`${labels.up} ${index + 1}`} disabled={index === 0} onClick={() => { const next = [...steps]; [next[index - 1], next[index]] = [next[index], next[index - 1]]; setSteps(next); }}>↑</button><button type="button" aria-label={`${labels.down} ${index + 1}`} disabled={index === steps.length - 1} onClick={() => { const next = [...steps]; [next[index + 1], next[index]] = [next[index], next[index + 1]]; setSteps(next); }}>↓</button><button type="button" aria-label={`${labels.remove} ${index + 1}`} onClick={() => setSteps(steps.filter((_, i) => i !== index))}>×</button></div></li>)}</ol><label>{labels.add}<select value={nextStep} onChange={(event) => setNextStep(event.target.value)}><option value="">—</option>{primitives.map((item) => <option key={item.path} value={item.path}>{item.node.label || item.node.id}</option>)}</select></label><button type="button" disabled={!nextStep || steps.length >= 16} onClick={() => { setSteps([...steps, nextStep]); setNextStep(''); }}>{labels.add}</button></> : <p>{labels.empty}</p>}</> : <><label>{labels.target}<select value={targetPath} onChange={(event) => { const selected = hardware.find((item) => item.path === event.target.value); if (selected) onChange(withControlTarget(entry.node, selected.path, selected.node)); }}><option value="">—</option>{hardware.map((item) => <option key={item.path} value={item.path}>{item.node.label || item.node.id} · {item.node.node_type.split('.')[2]}</option>)}</select></label>
      {isPrimitive && target && <label>{labels.mode}<select value={String(entry.node.settings?.movement_type || '')} onChange={(event) => onChange({ settings: { ...entry.node.settings, movement_type: event.target.value } })}><option value="">—</option>{target.node.movement?.modes.map((mode) => <option key={mode} value={mode}>{mode}</option>)}</select></label>}</>}
  </div>;
}

function NodeInspector({ entry, entries, lang, options, runId, activeRevision, currentRun, onChange, onBind, onRename, onRemove }: { entry: GraphNode; entries: GraphNode[]; lang: Language; options: NodeOptions | null; runId: string | null; activeRevision: string | null; currentRun: boolean; onChange: (change: Partial<NodeInstance>) => void; onBind: (target: string) => void; onRename: (id: string) => void; onRemove: () => void }): React.ReactElement {
  const copy = COPY[lang];
  const [id, setId] = useState(entry.node.id);
  const [raw, setRaw] = useState('');
  const [error, setError] = useState('');
  const bindingTarget = entries.find((target) => { const value = target.node.inputs?.command; return value && inputSources(value).some((source) => sameNodePath(source.node, entry.path)); })?.path || '';
  useEffect(() => { setId(entry.node.id); setRaw(JSON.stringify(Object.fromEntries(Object.entries(entry.node).filter(([key]) => (DETAIL_FIELDS as readonly string[]).includes(key))), null, 2)); setError(''); }, [entry.path, entry.node]);
  const commitDetails = (): void => { try { const parsed: unknown = JSON.parse(raw); if (!parsed || Array.isArray(parsed) || typeof parsed !== 'object' || Object.keys(parsed).some((key) => !(DETAIL_FIELDS as readonly string[]).includes(key))) throw new Error(copy.invalid); const details = parsed as Partial<NodeInstance>; for (const field of DETAIL_FIELDS) if (!(field in details) && field in entry.node) Object.assign(details, { [field]: undefined }); onChange(details); setError(''); } catch { setError(copy.invalid); } };
  return <section className="node-graph-inspector">
    <h2>{copy.inspector}</h2><p><strong>{lang === 'cn' ? '类型描述' : 'Type description'}:</strong> {entry.definition?.description || (lang === 'cn' ? '当前目录没有类型描述（兼容版本）。' : 'No type description in this catalog (compatibility version).')}</p><p><strong>{copy.type}:</strong> {entry.node.node_type}</p><p><strong>{copy.lifecycle}:</strong> {entry.definition?.lifecycle || '—'}</p>
    <label>{copy.id}<input value={id} onChange={(event) => setId(event.target.value.toLowerCase())} onBlur={() => { if (id !== entry.node.id) onRename(id.trim()); }} onKeyDown={(event) => { if (event.key === 'Enter') event.currentTarget.blur(); }} /></label>
    <label>{copy.label}<input value={entry.node.label} onChange={(event) => onChange({ label: event.target.value })} /></label>
    <label>{lang === 'cn' ? '实例描述' : 'Instance description'}<textarea rows={2} value={entry.node.description} onChange={(event) => onChange({ description: event.target.value })} /></label>
    <h3>{copy.capabilities}</h3><div className="node-graph-capabilities">{entry.definition?.capabilities.map((capability) => <label key={capability}><input type="checkbox" checked={entry.node.capabilities?.includes(capability) || false} onChange={(event) => onChange({ capabilities: event.target.checked ? [...(entry.node.capabilities || []), capability] : (entry.node.capabilities || []).filter((item) => item !== capability) })} />{capability}</label>)}</div>
    {(/^(?:eos\.dispatcher|isos\.eos_dispatcher)\.primitive_/.test(entry.node.node_type)) && <section className="node-graph-binding"><h3>{lang === 'cn' ? '原语绑定' : 'Primitive binding'}</h3><p>{options?.primitive_bindings.find((option) => option.node_type === entry.node.node_type)?.reason || ''}</p><label>{lang === 'cn' ? '目标硬件' : 'Target hardware'}<select value={bindingTarget} onChange={(event) => onBind(event.target.value)}><option value="">—</option>{bindingTarget && !options?.targets.some((target) => sameNodePath(target.path, bindingTarget)) && <option value={bindingTarget}>{bindingTarget}</option>}{options?.targets.filter((target) => target.device_kind === (entry.node.node_type.includes('_base') ? 'base' : 'manipulator')).map((target) => <option key={target.path} value={target.path} disabled={!target.available}>{target.path}{target.available ? '' : ` — ${target.reason || 'unavailable'}`}</option>)}</select></label>{!options && <p role="status">{lang === 'cn' ? '节点选项服务不可用' : 'Node options service unavailable'}</p>}</section>}
    {(/^(?:eos\.control|isos\.eos_control)\./.test(entry.node.node_type)) && <ControlNodeSettings entry={entry} entries={entries} lang={lang} onChange={onChange} />}
    {!!entry.definition?.settings.length && <><h3>{copy.settings}</h3>{entry.definition.settings.map((field) => {
      const group = settingOptionGroup(options, entry.path, field.name);
      const update = (value: unknown): void => {
        const settings = { ...entry.node.settings };
        if (value === undefined) delete settings[field.name]; else settings[field.name] = value;
        onChange({ settings });
      };
      if (field.selector?.provider === 'ui') return field.selector.resource === 'keys'
        ? <UiKeySelector key={`${entry.path}:${field.name}`} label={field.name} value={String(entry.node.settings?.[field.name] ?? '')} lang={lang} description={field.description} modifiers={Array.isArray(entry.node.settings?.modifiers) ? entry.node.settings.modifiers.filter((value): value is string => typeof value === 'string') : []} onCapture={(code, modifiers) => { const settings = { ...entry.node.settings, [field.name]: code }; if (entry.definition?.settings.some((candidate) => candidate.name === 'modifiers' && ['array', 'string_array'].includes(candidate.data_type))) settings.modifiers = modifiers; onChange({ settings }); }} />
        : <p key={`${entry.path}:${field.name}`} role="alert" className="node-graph-error">{lang === 'cn' ? '不支持此浏览器选择器。' : 'Unsupported browser selector.'}</p>;
      if (group) return <CatalogSelector key={`${entry.path}:${field.name}`} group={group} pin={entry.node.package} help={field.description} label={field.name} value={String(entry.node.settings?.[field.name] ?? '')} lang={lang} onChange={update} />;
      return <div key={`${entry.path}:${field.name}`}>{field.selector && <p role="status">{lang === 'cn' ? '目录选项暂不可用；输入已注册的稳定 ID。' : 'Catalog options unavailable; enter a registered stable ID.'}</p>}<SettingEditor lang={lang} name={field.name} dataType={field.data_type} required={field.required} description={field.description} unit={field.unit} min={field.min} max={field.max} choices={field.choices} value={entry.node.settings?.[field.name] ?? field.default} onChange={update} /></div>;
    })}</>}
    {((entry.definition?.core === 'isosui' && entry.definition.layer === 'input') || (entry.definition?.core === 'isos' && entry.definition.layer === 'ui')) && entry.definition.group === 'keyboard' && <KeyboardNodeControl node={entry.node} path={entry.path} runId={runId} activeRevision={activeRevision} current={currentRun} lang={lang} />}
    {entry.definition?.lifecycle === 'adapter' && <HardwareFields node={entry.node} path={entry.path} definition={entry.definition} options={options} lang={lang} onChange={onChange} />}
    {entry.definition?.lifecycle === 'adapter' && <><h3>{copy.details}</h3><textarea rows={12} value={raw} onChange={(event) => setRaw(event.target.value)} /><button type="button" onClick={commitDetails}>{copy.applyDetails}</button></>}
    {error && <p role="alert" className="node-graph-error">{error}</p>}<button type="button" className="node-graph-remove" onClick={onRemove}>{copy.remove}</button>
  </section>;
}

export function NodeGraphManager({ lang, visible = true, onDirtyChange, onOpenPanel, onStopIntent, onStopOutcome }: Props): React.ReactElement {
  const copy = COPY[lang];
  const [graph, setGraph] = useState<NodeGraph | null>(null);
  const [rawGraph, setRawGraph] = useState<unknown>(null);
  const [saved, setSaved] = useState('');
  const [revision, setRevision] = useState('');
  const [readOnlyReason, setReadOnlyReason] = useState<string | null>(null);
  const [types, setTypes] = useState<NodeTypeDefinition[]>([]);
  const [dataTypes, setDataTypes] = useState<DataTypeDefinition[]>([]);
  const [nodeSets, setNodeSets] = useState<NodeSetDefinition[]>([]);
  const [setKey, setSetKey] = useState('');
  const [setValues, setSetValues] = useState<Record<string, string>>({});
  const [setPreview, setSetPreview] = useState<{ result: NodeSetPreview; source: string } | null>(null);
  const [processStatus, setProcessStatus] = useState<ProcessStatus | null>(null);
  const [selected, setSelected] = useState<string[]>([]);
  const [typeId, setTypeId] = useState('');
  const [newId, setNewId] = useState('');
  const [quickSearch, setQuickSearch] = useState('');
  const [quickAdd, setQuickAdd] = useState<{ left: number; top: number; x: number; y: number } | null>(null);
  const [quickDetail, setQuickDetail] = useState<string>('');
  const [selectedEdge, setSelectedEdge] = useState<string>('');
  const [edgeMenu, setEdgeMenu] = useState<{ left: number; top: number; id: string } | null>(null);
  const [libraryCollapsed, setLibraryCollapsed] = useState(false);
  const [sideCollapsed, setSideCollapsed] = useState(false);
  const [showOverview, setShowOverview] = useState(false);
  const [flowInstance, setFlowInstance] = useState<ReactFlowInstance | null>(null);
  const lastCanvasSize = useRef('');
  const canvasRef = useRef<HTMLDivElement>(null);
  const quickSearchRef = useRef<HTMLInputElement>(null);
  const overviewTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const [undoStack, setUndoStack] = useState<NodeGraph[]>([]);
  const [redoStack, setRedoStack] = useState<NodeGraph[]>([]);
  const [diagnostics, setDiagnostics] = useState<NodeGraphDiagnostic[]>([]);
  const [validationStamp, setValidationStamp] = useState<DraftValidationStamp | null>(null);
  const [validatedEdges, setValidatedEdges] = useState<ValidatedEdge[]>([]);
  const [edgeInputInvalid, setEdgeInputInvalid] = useState(false);
  const [diagnosticsOpen, setDiagnosticsOpen] = useState(false);
  const [frontNode, setFrontNode] = useState<string | null>(null);
  const setMessage = (text: string, error = false): void => { publishUiMessage(text, error ? 'error' : 'info'); };
  const [optionSnapshot, setOptionSnapshot] = useState<OptionSnapshot | null>(null);
  const optionRequest = useRef(0);
  const catalogReloadRequest = useRef(0);
  const statusRequest = useRef(0);
  const readinessRequest = useRef(0);
  const graphForCatalog = useRef(graph);
  const revisionForCatalog = useRef(revision);
  graphForCatalog.current = graph;
  revisionForCatalog.current = revision;
  const [packageGeneration, setPackageGeneration] = useState(0);
  const [busy, setBusy] = useState(false);
  const [stopping, setStopping] = useState(false);
  const [advancedOpen, setAdvancedOpen] = useState(false);
  const [readiness, setReadiness] = useState<CoreReadiness | null>(null);
  const [viewLost, setViewLost] = useState(false);
  const initialFitDone = useRef(false);
  const layoutKeyRef = useRef('');
  const editGeneration = useRef(0);
  const activeGesture = useRef(false);
  const [positions, setPositions] = useState<NodePositions>({});
  const [mirrors, setMirrors] = useState<NodeMirrors>({});
  const [viewOpen, setViewOpen] = useState(false);
  const [coreWindows, setCoreWindows] = useState<Record<'egia', CoreWindowState>>(() => ({ egia: readCoreWindow('isosui-graph-egia-window', 190) }));
  const dirty = Boolean(graph && JSON.stringify(graph) !== saved);
  const readOnly = readOnlyReason !== null;
  const draftKey = graph ? JSON.stringify(graph) : '';
  const currentDraft = useRef(draftKey);
  currentDraft.current = draftKey;
  const nodeOptions = optionSnapshot?.draft === draftKey ? optionSnapshot.options : null;
  useEffect(() => {
    const requestId = ++optionRequest.current;
    if (!graph || readOnly) return;
    const draft = JSON.stringify(graph);
    const timer = window.setTimeout(() => { void getNodeOptions(graph).then((options) => {
      if (optionResponseIsCurrent(requestId, optionRequest.current, draft, currentDraft.current)) setOptionSnapshot({ draft, options });
    }).catch(() => { if (requestId === optionRequest.current) setOptionSnapshot(null); }); }, 250);
    return () => { window.clearTimeout(timer); optionRequest.current += 1; };
  }, [graph, readOnly, packageGeneration]);
  useEffect(() => { onDirtyChange?.(dirty); }, [dirty, onDirtyChange]);
  const entries = useMemo(() => graph ? graphNodes(graph, types) : [], [graph, types]);
  const selectorBlocked = catalogSelectionBlocks(graph?.version, entries, nodeOptions);
  useEffect(() => { if (graph) browserLog('debug', `Graph revision ${revision.slice(0, 8) || 'unsaved'}: ${entries.length} nodes; visible=${visible}`); }, [graph, revision, visible]);
  useEffect(() => {
    if (!visible || !flowInstance || !entries.length) return;
    const canvas = canvasRef.current;
    if (!canvas) return;
    const observer = new ResizeObserver(() => {
      const size = `${canvas.clientWidth}x${canvas.clientHeight}`;
      if (size === lastCanvasSize.current) return;
      lastCanvasSize.current = size;
      browserLog(canvas.clientWidth && canvas.clientHeight ? 'debug' : 'warn', `Graph canvas ${size}; viewport ${JSON.stringify(flowInstance.getViewport())}`);
    });
    observer.observe(canvas);
    return () => observer.disconnect();
  }, [visible, flowInstance, entries.length]);
  useEffect(() => {
    if (!visible || !flowInstance || !entries.length || initialFitDone.current) return;
    let frame = 0;
    let cancelled = false;
    let attempts = 0;
    const fitWhenReady = (): void => {
      if (cancelled || initialFitDone.current) return;
      if (activeGesture.current) { initialFitDone.current = true; return; }
      const canvas = canvasRef.current;
      const measured = flowInstance.getNodes().some((node) => Boolean(node.width && node.height));
      if (!canvas?.clientWidth || !canvas.clientHeight || !measured) {
        if (++attempts < 120) frame = requestAnimationFrame(fitWhenReady);
        else browserLog('warn', 'Graph initial fit skipped because the canvas or nodes were not measured');
        return;
      }
      initialFitDone.current = true;
      void flowInstance.fitView({ padding: 0.15 });
    };
    frame = requestAnimationFrame(fitWhenReady);
    return () => { cancelled = true; cancelAnimationFrame(frame); };
  }, [visible, flowInstance, entries.length]);
  useEffect(() => {
    if (!visible || !flowInstance || !entries.length) return;
    const timer = window.setTimeout(() => {
      const canvas = canvasRef.current;
      if (!canvas || !canvas.clientWidth || !canvas.clientHeight) return;
      const bounds = canvas.getBoundingClientRect();
      const cards = canvas.querySelectorAll('.react-flow__node');
      const onScreen = Array.from(cards).some((card) => {
        const rect = card.getBoundingClientRect();
        return rect.right > bounds.left && rect.left < bounds.right && rect.bottom > bounds.top && rect.top < bounds.bottom;
      });
      if (cards.length && !onScreen) {
        browserLog('warn', `Graph nodes outside canvas; ${entries.length} graph nodes, ${cards.length} rendered nodes, viewport ${JSON.stringify(flowInstance.getViewport())}`);
        setViewLost(true);
      } else if (onScreen) {
        setViewLost(false);
      }
    }, 100);
    return () => window.clearTimeout(timer);
  }, [graph, visible, flowInstance, entries.length]);
  const quickTypes = useMemo(() => types.filter((type) => `${type.id} ${type.core} ${type.layer} ${type.group} ${type.kind}`.toLowerCase().includes(quickSearch.toLowerCase())), [types, quickSearch]);
  const selectedSet = nodeSets.find((set) => `${set.package.id}@${set.package.version}/${set.id}` === setKey);
  useEffect(() => { if (quickAdd) quickSearchRef.current?.focus(); }, [quickAdd]);
  useEffect(() => { if (diagnostics.length) setDiagnosticsOpen(true); }, [diagnostics]);
  useEffect(() => () => { if (overviewTimer.current) clearTimeout(overviewTimer.current); }, []);
  const revealOverview = (): void => {
    setShowOverview(true);
    if (overviewTimer.current) clearTimeout(overviewTimer.current);
    overviewTimer.current = setTimeout(() => setShowOverview(false), 1800);
  };
  const chosen = selected.length === 1 ? entries.find((entry) => entry.path === selected[0]) : undefined;
  const refresh = useCallback(async (confirm = false): Promise<void> => {
    if (confirm && dirty && !window.confirm(copy.discard)) return;
    const generation = editGeneration.current;
    const catalogRequestId = ++catalogReloadRequest.current;
    const statusRequestId = ++statusRequest.current;
    const readinessRequestId = ++readinessRequest.current;
    setBusy(true);
    setValidationStamp(null);
    setValidatedEdges([]);
    setEdgeInputInvalid(false);
    setDiagnostics([]);
    try {
      const snapshot = await getGraphCatalogSnapshot();
      const { graph: document } = snapshot;
      const catalog = combineNodeTypeCatalogs(snapshot.selected, snapshot.global);
      if (catalogRequestId !== catalogReloadRequest.current) return;
      if (generation !== editGeneration.current) { setMessage(lang === 'cn' ? '刷新期间节点图已更改，保留未保存的修改。' : 'Graph changed during refresh; unsaved edits were preserved.'); return; }
      const shaped = nodeGraphHasEditableShape(document.graph);
      if (shaped) {
        const layoutKey = graphLayoutKey(document.graph);
        if (layoutKeyRef.current !== layoutKey) {
          setPositions(parseNodePositions(localStorage.getItem(layoutKey)));
          setMirrors(parseNodeMirrors(localStorage.getItem(graphMirrorKey(document.graph))));
          layoutKeyRef.current = layoutKey;
        }
      }
      setGraph(shaped ? document.graph : null);
      setRawGraph(shaped ? null : document.graph);
      setSaved(JSON.stringify(document.graph));
      setRevision(document.revision);
      setTypes(catalog?.types || []);
      setDataTypes(catalog?.data_types || []);
      setNodeSets([]);
      setSetKey(''); setSetValues({}); setSetPreview(null);
      void getNodeSets().then((sets) => { if (catalogRequestId === catalogReloadRequest.current && generation === editGeneration.current) setNodeSets(sets.sets); }).catch(() => undefined);
      void getProcessStatus().then((status) => { if (catalogRequestId === catalogReloadRequest.current && statusRequestId === statusRequest.current) setProcessStatus(status); }).catch(() => undefined);
      void getCoreReadiness().then((cores) => { if (catalogRequestId === catalogReloadRequest.current && readinessRequestId === readinessRequest.current) setReadiness(cores); }).catch(() => undefined);
      setTypeId('');
      setSelected((current) => shaped ? current.filter((path) => graphNodes(document.graph, []).some((entry) => entry.path === path)) : []);
      setDiagnostics(document.diagnostics || []);
      setReadOnlyReason(graphCatalogReadOnlyReason(snapshot));
      setUndoStack([]); setRedoStack([]); setMessage('');
    }
    catch (error) { if (catalogRequestId === catalogReloadRequest.current && generation === editGeneration.current) { setReadOnlyReason('Graph compatibility could not be checked.'); setMessage(error instanceof Error ? error.message : copy.disconnected, true); } }
    finally { if (catalogRequestId === catalogReloadRequest.current) setBusy(false); }
  }, [dirty, copy.discard, copy.disconnected, lang]);
  useEffect(() => { void refresh(); }, []);
  useEffect(() => {
    if (!visible) return;
    let active = true;
    let statusPending = false;
    let readinessPending = false;
    const statusTimer = window.setInterval(() => {
      if (document.visibilityState !== 'visible' || statusPending) return;
      statusPending = true;
      const requestId = ++statusRequest.current;
      void getProcessStatus().then((status) => { if (active && requestId === statusRequest.current) setProcessStatus(status); })
        .catch(() => undefined).finally(() => { statusPending = false; });
    }, 3000);
    const readinessTimer = window.setInterval(() => {
      if (document.visibilityState !== 'visible' || readinessPending) return;
      readinessPending = true;
      const requestId = ++readinessRequest.current;
      void getCoreReadiness().then((readiness) => { if (active && requestId === readinessRequest.current) setReadiness(readiness); })
        .catch(() => undefined).finally(() => { readinessPending = false; });
    }, 30000);
    return () => { active = false; ++statusRequest.current; ++readinessRequest.current; window.clearInterval(statusTimer); window.clearInterval(readinessTimer); };
  }, [visible]);
  useEffect(() => {
    const reloadCatalog = (): void => { setValidatedEdges([]); const requestId = ++catalogReloadRequest.current; optionRequest.current += 1; setOptionSnapshot(null); setPackageGeneration((value) => value + 1); void Promise.all([getGraphCatalogSnapshot(), getNodeSets()]).then(([snapshot, sets]) => {
      if (!catalogReloadIsCurrent(requestId, catalogReloadRequest.current)) return;
      const { graph: document } = snapshot;
      const catalog = combineNodeTypeCatalogs(snapshot.selected, snapshot.global);
      setTypes(catalog.types); setDataTypes(catalog.data_types || []); setNodeSets(sets.sets); setSetKey(''); setSetValues({}); setSetPreview(null);
      const currentGraph = catalogReloadGraph(document.revision, revisionForCatalog.current, graphForCatalog.current, document.graph);
      const savedReason = graphCatalogReadOnlyReason(snapshot);
      setReadOnlyReason(savedReason || (currentGraph
        ? nodeGraphReadOnlyReason({ ...document, graph: currentGraph }, catalog.types)
        : 'Saved graph revision changed; refresh before editing.'));
    }).catch((error: unknown) => { if (catalogReloadIsCurrent(requestId, catalogReloadRequest.current)) { setReadOnlyReason('Graph compatibility could not be checked.'); setMessage(error instanceof Error ? error.message : String(error)); } }); };
    window.addEventListener('isosui:packages-changed', reloadCatalog);
    return () => { window.removeEventListener('isosui:packages-changed', reloadCatalog); catalogReloadRequest.current += 1; };
  }, []);
  useEffect(() => { if (!graph) return; const key = graphLayoutKey(graph); const timer = window.setTimeout(() => localStorage.setItem(key, JSON.stringify(positions)), 250); return () => window.clearTimeout(timer); }, [graph?.robot.id, positions]);
  useEffect(() => { if (graph) localStorage.setItem(graphMirrorKey(graph), JSON.stringify(mirrors)); }, [graph?.robot.id, mirrors]);
  useEffect(() => { for (const id of ['egia'] as const) localStorage.setItem(`isosui-graph-${id}-window`, JSON.stringify(coreWindows[id])); }, [coreWindows]);
  const updateCoreWindow = (id: 'egia', patch: Partial<CoreWindowState>): void => setCoreWindows((current) => ({ ...current, [id]: { ...current[id], ...patch } }));
  useEffect(() => { const beforeUnload = (event: BeforeUnloadEvent): void => { if (dirty) event.preventDefault(); }; window.addEventListener('beforeunload', beforeUnload); return () => window.removeEventListener('beforeunload', beforeUnload); }, [dirty]);
  const edit = (change: (current: NodeGraph) => NodeGraph): void => {
    if (!graph || readOnly) return;
    const next = change(graph);
    if (next === graph || JSON.stringify(next) === JSON.stringify(graph)) return;
    editGeneration.current += 1; setUndoStack((stack) => [...stack.slice(-49), graph]); setRedoStack([]); setGraph(next); setSetPreview(null); setValidatedEdges([]); setDiagnostics([]); setValidationStamp(null);
  };
  const undo = (): void => { if (!graph || readOnly || !undoStack.length) return; editGeneration.current += 1; setRedoStack((stack) => [...stack, graph]); setGraph(undoStack[undoStack.length - 1]); setSetPreview(null); setDiagnostics([]); setValidationStamp(null); setValidatedEdges([]); setUndoStack(undoStack.slice(0, -1)); };
  const redo = (): void => { if (!graph || readOnly || !redoStack.length) return; editGeneration.current += 1; setUndoStack((stack) => [...stack, graph]); setGraph(redoStack[redoStack.length - 1]); setSetPreview(null); setDiagnostics([]); setValidationStamp(null); setValidatedEdges([]); setRedoStack(redoStack.slice(0, -1)); };
  const previewPackageSet = async (): Promise<void> => {
    if (!graph || readOnly || !selectedSet) return;
    setBusy(true);
    try {
      const source = JSON.stringify(graph);
      const result = await previewNodeSet(selectedSet, graph, setValues);
      if (removedNodePaths(graph, result.graph).length) throw new Error(copy.previewRemoves);
      setSetPreview({ result, source });
      setMessage(`${result.added_paths.length} ${copy.previewCount}.`);
    }
    catch (error) { setSetPreview(null); setMessage(error instanceof Error ? error.message : String(error), true); }
    finally { setBusy(false); }
  };
  const addPreviewedSet = (): void => {
    if (!graph || readOnly || !setPreview || setPreview.source !== JSON.stringify(graph)) { setMessage(copy.previewStale, true); setSetPreview(null); return; }
    edit(() => setPreview.result.graph);
    setSelected(setPreview.result.added_paths.map(canonicalNodePath));
    setMessage(`${setPreview.result.added_paths.length} ${copy.previewCount}.`);
    setSetPreview(null);
  };
  const add = (automatic = false): void => { const definition = types.find((type) => catalogTypeKey(type) === typeId); if (!definition || !graph || readOnly) return; const id = automatic || !newId.trim() ? nextNodeId(graph, definition) : newId.trim(); try { edit((current) => addNode(current, definition, id)); setSelected([nodePath(definition, id)]); setNewId(''); setMessage(''); } catch (error) { setMessage(error instanceof Error ? error.message : copy.duplicate, true); } };
  const addAtCursor = (definition: NodeTypeDefinition): void => {
    if (!quickAdd || !graph || readOnly) return;
    const id = nextNodeId(graph, definition);
    try {
      edit((current) => addNode(current, definition, id));
      const path = nodePath(definition, id);
      setPositions((current) => ({ ...current, [path]: { x: quickAdd.x, y: quickAdd.y } }));
      setSelected([path]); setQuickAdd(null); setMessage('');
    } catch (error) { setMessage(error instanceof Error ? error.message : copy.duplicate, true); }
  };
  const openQuickAdd = (event: React.MouseEvent<HTMLDivElement>): void => {
    event.preventDefault();
    if (!graph || readOnly || !flowInstance || !canvasRef.current) return;
    const rect = canvasRef.current.getBoundingClientRect();
    const x = event.clientX - rect.left;
    const y = event.clientY - rect.top;
    const position = flowInstance.project({ x, y });
    setEdgeMenu(null); setQuickDetail(''); setQuickSearch('');
    setQuickAdd({ left: Math.max(8, Math.min(x, rect.width - 284)), top: Math.max(8, Math.min(y, rect.height - 300)), x: position.x, y: position.y });
  };
  const openEdgeMenu = (event: React.MouseEvent, edge: Edge): void => {
    event.preventDefault(); event.stopPropagation();
    if (readOnly) return;
    const rect = canvasRef.current?.getBoundingClientRect();
    if (!rect) return;
    setQuickAdd(null);
    setEdgeMenu({ left: Math.max(8, Math.min(event.clientX - rect.left, rect.width - 180)), top: Math.max(8, Math.min(event.clientY - rect.top, rect.height - 56)), id: edge.id });
  };
  const connect = (connection: Connection): void => { if (!graph || readOnly || !connection.source || !connection.target) return; try { edit((current) => connectNodes(current, types, connection.source!, connection.sourceHandle?.replace(/^out:/, '') || '', connection.target!, connection.targetHandle?.replace(/^in:/, '') || '')); setMessage(''); } catch (error) { setMessage(error instanceof Error ? error.message : String(error), true); } };
  useEffect(() => { const onKey = (event: KeyboardEvent): void => {
    if (!visible || readOnly) return;
    const target = event.target as HTMLElement;
    if (event.key === 'Escape' && (quickAdd || edgeMenu)) { setQuickAdd(null); setEdgeMenu(null); return; }
    if (target.closest('input, textarea, select, [contenteditable="true"]')) return;
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'z') { event.preventDefault(); if (event.shiftKey) redo(); else undo(); }
    else if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'y') { event.preventDefault(); redo(); }
    else if ((event.key === 'Delete' || event.key === 'Backspace') && selected.length) { event.preventDefault(); edit((current) => removeSelectedNodes(current, selected)); setPositions((current) => Object.fromEntries(Object.entries(current).filter(([path]) => !selected.includes(path)))); setSelected([]); }
    else if (event.key.toLowerCase() === 'r' && !event.ctrlKey && !event.metaKey && !quickAdd && !edgeMenu && selected.length) { event.preventDefault(); setMirrors((current) => { const next = { ...current }; for (const path of selected) next[path] = !next[path]; return next; }); }
  }; window.addEventListener('keydown', onKey); return () => window.removeEventListener('keydown', onKey); }, [graph, undoStack, redoStack, selected, quickAdd, edgeMenu, visible, readOnly]);
  const validate = async (): Promise<boolean> => { if (!graph || readOnly) return false; const generation = editGeneration.current; setBusy(true); try { const result = await validateNodeGraph(graph); if (!validationResponseIsCurrent(generation, editGeneration.current)) return false; setDiagnostics(result.diagnostics); setValidatedEdges(result.valid && Array.isArray(result.edges) ? result.edges : []); setValidationStamp(draftValidationStamp(graph, generation)); setMessage(result.valid ? copy.valid : `${result.diagnostics.length} ${copy.diagnostics.toLowerCase()}`, !result.valid); return result.valid; } catch (error) { if (validationResponseIsCurrent(generation, editGeneration.current)) { setValidationStamp(null); setMessage(error instanceof Error ? error.message : String(error), true); } return false; } finally { setBusy(false); } };
  const saveSnapshot = async (snapshot: NodeGraph, expectedRevision: string): Promise<string | null> => {
    if (selectorBlocked || edgeInputInvalid) { setMessage(edgeInputInvalid ? (lang === 'cn' ? '请修复连线服务质量字段。' : 'Repair the edge QoS fields.') : (lang === 'cn' ? '请等待选项更新并修复不可用的目录选项。' : 'Wait for options to refresh and repair unavailable catalog selections.'), true); return null; }
    const generation = editGeneration.current;
    const result = await validatedApply(snapshot, expectedRevision, () => validationResponseIsCurrent(generation, editGeneration.current), (checked) => {
      setDiagnostics(checked.diagnostics);
      setValidatedEdges(checked.valid && Array.isArray(checked.edges) ? checked.edges : []);
      setValidationStamp(draftValidationStamp(snapshot, generation));
    });
    if (result.state === 'stale') return null;
    if (result.state === 'rejected') { setMessage(result.validation.diagnostics.map((item) => item.message).join('; ') || copy.invalid, true); setSideCollapsed(false); setDiagnosticsOpen(true); return null; }
    const document = result.document;
    catalogReloadRequest.current += 1;
    const newerDraft = editGeneration.current !== generation;
    setSaved(JSON.stringify(document.graph));
    setRevision(document.revision);
    if (!newerDraft) setGraph(document.graph);
    let selected;
    try { selected = await getGraphCatalogSnapshot(); }
    catch { setReadOnlyReason('Graph compatibility could not be checked.'); setMessage('Graph compatibility could not be checked.', true); return null; }
    const selectedReason = graphCatalogReadOnlyReason(selected);
    if (selected.graph.revision !== document.revision || selectedReason) {
      const reason = selectedReason || 'Saved graph revision changed; refresh before running.';
      setReadOnlyReason(reason);
      setMessage(reason, true);
      return null;
    }
    const catalog = combineNodeTypeCatalogs(selected.selected, selected.global);
    setTypes(catalog.types); setDataTypes(catalog.data_types || []);
    setReadOnlyReason(null);
    setMessage(newerDraft ? (lang === 'cn' ? '已保存先前版本，新的修改仍未保存。' : 'Earlier draft saved; newer edits remain unsaved.') : copy.saved);
    return newerDraft ? null : document.revision;
  };
  const apply = async (): Promise<void> => { if (!graph || readOnly || busy || selectorBlocked || edgeInputInvalid) return; setBusy(true); try { await saveSnapshot(graph, revision); } catch (error) { setMessage(error instanceof Error ? error.message : String(error), true); } finally { setBusy(false); } };
  const running = hasActiveDeployment(processStatus);
  const primaryLabel = lang === 'cn' ? (dirty ? (running ? '保存并重启' : '保存并运行') : (running ? '重启' : '保存并运行')) : (dirty ? (running ? 'Save and Restart' : 'Save and Run') : (running ? 'Restart' : 'Save and Run'));
  const runPrimary = async (): Promise<void> => {
    if (!graph || readOnly || busy || stopping || selectorBlocked || edgeInputInvalid) return;
    setBusy(true);
    const generation = editGeneration.current;
    const stopAtStart = deploymentStopEpoch();
    const cameraGates = entries.filter((entry) => entry.node.node_type === 'eos.interface.camera_gate@v0.1' || entry.node.node_type === 'isos.eos_interface.camera_gate@v0.2');
    const needsVisionApply = !dirty && cameraGates.length === 1 && !processStatus?.vision_evidence;
    try {
      let nextRevision = revision;
      if (dirty || needsVisionApply) {
        const savedRevision = await saveSnapshot(graph, revision);
        if (!savedRevision) return;
        nextRevision = savedRevision;
      } else {
        const checked = await validateNodeGraph(graph);
        if (!validationResponseIsCurrent(generation, editGeneration.current)) return;
        setDiagnostics(checked.diagnostics);
        setValidatedEdges(checked.valid && Array.isArray(checked.edges) ? checked.edges : []);
        setValidationStamp(draftValidationStamp(graph, generation));
        if (!checked.valid) { setMessage(checked.diagnostics.map((item) => item.message).join('; ') || copy.invalid, true); setSideCollapsed(false); setDiagnosticsOpen(true); return; }
      }
      if (!dirty && !needsVisionApply) {
        const snapshot = await getGraphCatalogSnapshot();
        const selectedReason = graphCatalogReadOnlyReason(snapshot);
        if (snapshot.graph.revision !== nextRevision || selectedReason) {
          setReadOnlyReason(selectedReason || 'Saved graph revision changed; refresh before running.');
          setMessage(selectedReason || 'Saved graph revision changed; refresh before running.', true);
          return;
        }
        const catalog = combineNodeTypeCatalogs(snapshot.selected, snapshot.global);
        setTypes(catalog.types); setDataTypes(catalog.data_types || []);
      }
      if (generation !== editGeneration.current || stopAtStart !== deploymentStopEpoch()) { setMessage(lang === 'cn' ? '运行前节点图或停止状态已更改，请再次检查。' : 'Graph or Stop state changed before run. Check again.', true); return; }
      const latestStatus = await getProcessStatus();
      if (generation !== editGeneration.current || stopAtStart !== deploymentStopEpoch()) { setMessage(lang === 'cn' ? '运行前节点图或停止状态已更改，请再次检查。' : 'Graph or Stop state changed before run. Check again.', true); return; }
      const operation = hasActiveDeployment(latestStatus) ? 'restart' : 'run';
      const status = await runDeployment(operation, nextRevision);
      if (stopAtStart !== deploymentStopEpoch()) return;
      setProcessStatus(status);
      setMessage(processRunFeedback(status, operation, lang));
    } catch (error) { if (stopAtStart === deploymentStopEpoch()) setMessage(error instanceof Error ? error.message : String(error), true); }
    finally { setBusy(false); }
  };
  const stop = async (): Promise<void> => {
    if (stopping) return;
    const before = processStatus;
    const request = requestRunStop(before, onStopIntent, stopDeploymentConverged);
    setStopping(true);
    setMessage(lang === 'cn' ? '正在停止…' : 'Stopping…');
    try {
      const reply = await request;
      const after = await getProcessStatus().catch(() => null);
      const feedback = stopReplyFeedback(reply, before, lang, after);
      setMessage(feedback.text, feedback.alert); onStopOutcome?.(feedback, reply.terminal?.run_id || null);
      if (after) setProcessStatus(after);
    } catch (error) { const feedback = stopFailureFeedback(error, lang); setMessage(feedback.text, feedback.alert); onStopOutcome?.(feedback); }
    finally { setStopping(false); }
  };
  const validationCurrent = !!graph && validationDisplayIsCurrent(validationStamp, graph, editGeneration.current);
  const nodes = useMemo<Node<FlowData>[]>(() => entries.map((entry, index) => ({ id: entry.path, type: 'profile', position: positions[entry.path] || { x: 50 + (index % 3) * 330, y: 50 + Math.floor(index / 3) * 220 }, zIndex: frontNode === entry.path ? 20 : selected.includes(entry.path) ? 1 : 0, data: { entry, error: diagnostics.some((item) => item.path.includes(entry.path)), mirrored: !!mirrors[entry.path], lang, dataTypes, liveRunId: processStatus?.run_id || null, liveAllowed: !dirty && !readOnly && processStatus?.run_id_state === 'known' && !!processStatus.run_id && processStatus.state === 'running' && processStatus.active_revision === revision && processStatus.saved_revision === revision }, selected: selected.includes(entry.path) })), [entries, positions, selected, frontNode, diagnostics, mirrors, lang, dataTypes, dirty, readOnly, processStatus, revision]);
  const edges = useMemo<Edge[]>(() => entries.flatMap((entry) => Object.entries(entry.node.inputs || {}).flatMap(([input, sources]) => {
    const sourceList = inputSources(sources);
    return sourceList.map((source, inputIndex) => {
    const port = entries.find((candidate) => sameNodePath(candidate.path, source.node))?.definition?.outputs.find((item) => item.name === source.output);
    const visual = interfaceVisual(port, source.frequency_hz);
    const id = `${entry.path}:${input}:${inputIndex}:${canonicalNodePath(source.node)}:${source.output}`;
    const authority = validatedSource(validatedEdges, entry.path, input, inputIndex, Array.isArray(sources));
    const routeLabel = authority?.routed === false ? (lang === 'cn' ? '仅设计用途' : 'design only') : '';
    const label = `${port?.data_type || source.output} · ${visual.label} · nice ${source.priority ?? 0}${routeLabel ? ` · ${routeLabel}` : ''}`;
    const edge = { source: canonicalNodePath(source.node), sourceHandle: `out:${source.output}`, target: entry.path, targetHandle: `in:${input}`, data: { inputIndex, inputCount: sourceList.length } };
    const destinationType = entry.definition?.inputs.find((item) => item.name === input)?.data_type;
    const mismatch = !!port?.data_type && !!destinationType && port.data_type !== destinationType;
    const rejected = validationCurrent && edgeInputDiagnostics({ ...edge, inputIndex, inputCount: edge.data.inputCount }, diagnostics).length > 0;
    const reason = mismatch ? (lang === 'cn' ? '端口类型不匹配' : 'Port type mismatch') : rejected ? (lang === 'cn' ? 'IsOS 拒绝了此输入端口' : 'IsOS rejected this input port') : '';
    return { id, ...edge, label: reason ? `${label} · ${reason}` : label, ariaLabel: reason ? `${label} · ${reason}` : label, interactionWidth: 24, focusable: true, selected: selectedEdge === id, className: `${visual.className} node-graph-edge-priority--${(source.priority ?? 0) < 0 ? 'high' : (source.priority ?? 0) > 0 ? 'low' : 'normal'}${reason ? ' node-graph-edge--invalid' : ''}`, style: visual.style as React.CSSProperties, animated: false };
    });
  })), [entries, selectedEdge, diagnostics, validatedEdges, validationCurrent, lang]);
  const edgeDetail = selectedEdge ? edges.find((edge) => edge.id === selectedEdge) : undefined;
  const onNodesChange = (changes: NodeChange[]): void => {
    if (changes.some((change) => change.type === 'select')) setSelected((current) => applyNodeSelection(current, changes));
    setPositions((current) => {
    const moved = changes.filter((change): change is Extract<NodeChange, { type: 'position' }> => change.type === 'position' && Boolean(change.position));
    if (!moved.length) return current;
    const next = { ...current };
    let changed = false;
    for (const change of moved) if (change.position && (current[change.id]?.x !== change.position.x || current[change.id]?.y !== change.position.y)) { next[change.id] = change.position; changed = true; }
    return changed ? next : current;
    });
  };
  return <ReactFlowProvider><main className="node-graph-manager">
    <header><h1>{copy.title}</h1><div role="toolbar" aria-label={copy.title}>
      <button className="node-graph-icon-button" type="button" onClick={undo} disabled={readOnly || !undoStack.length || busy} title={`${copy.undo} (Ctrl+Z)`} aria-label={copy.undo}><span className="node-graph-history-arrow" aria-hidden="true" /></button>
      <button className="node-graph-icon-button" type="button" onClick={redo} disabled={readOnly || !redoStack.length || busy} title={`${copy.redo} (Ctrl+Shift+Z)`} aria-label={copy.redo}><span className="node-graph-history-arrow node-graph-history-arrow--redo" aria-hidden="true" /></button>
      <div className="node-graph-view"><button type="button" aria-expanded={viewOpen} onClick={() => setViewOpen((value) => !value)}>{lang === 'cn' ? '视图' : 'View'} ▾</button>{viewOpen && <div className="node-graph-view__menu" role="menu"><button type="button" role="menuitemcheckbox" aria-checked={!libraryCollapsed} onClick={() => setLibraryCollapsed((value) => !value)}>{copy.catalog}</button><button type="button" role="menuitemcheckbox" aria-checked={!sideCollapsed} onClick={() => setSideCollapsed((value) => !value)}>{copy.inspector}</button><button type="button" role="menuitemcheckbox" aria-checked={diagnosticsOpen && !sideCollapsed} onClick={() => { if (diagnosticsOpen && !sideCollapsed) setDiagnosticsOpen(false); else { setSideCollapsed(false); setDiagnosticsOpen(true); } }}>{copy.diagnostics}</button><button type="button" role="menuitem" onClick={() => onOpenPanel?.('logs')}>{lang === 'cn' ? '日志' : 'Logs'}</button><button type="button" role="menuitem" onClick={() => onOpenPanel?.('control')}>EOS</button><button type="button" role="menuitemcheckbox" aria-checked={coreWindows.egia.open} onClick={() => updateCoreWindow('egia', { open: !coreWindows.egia.open })}>EI</button></div>}</div>
      <button type="button" onClick={() => { browserLog('info', `Fit graph requested; ${entries.length} nodes; viewport ${JSON.stringify(flowInstance?.getViewport())}`); setViewLost(false); void flowInstance?.fitView({ padding: 0.15, duration: 200 }); }}>{lang === 'cn' ? '显示全图' : 'Fit graph'}</button>
      <button type="button" className="node-graph-primary-action" onClick={() => void runPrimary()} disabled={readOnly || busy || !graph || selectorBlocked || edgeInputInvalid} title={lang === 'cn' ? '保存更改（如有）并启动；运行中则重启' : 'Save changes if needed, then start or restart the deployment'}>{primaryLabel}</button>
      {(running || busy) && <button type="button" onClick={() => void stop()} disabled={stopping}>{lang === 'cn' ? '停止' : 'Stop'}</button>}
      <details className="node-graph-advanced" open={advancedOpen} onToggle={(event) => setAdvancedOpen(event.currentTarget.open)}><summary>{lang === 'cn' ? '高级操作' : 'Advanced'} ▾</summary><div>
        <button type="button" onClick={() => void validate()} disabled={readOnly || busy || !graph}>{copy.validate}</button><small>{lang === 'cn' ? '检查草稿，不保存' : 'Check draft without saving'}</small>
        <button type="button" onClick={() => void apply()} disabled={readOnly || busy || !graph || !dirty || selectorBlocked || edgeInputInvalid}>{copy.apply}</button><small>{lang === 'cn' ? '检查并保存草稿，同时生成核心配置' : 'Check and save draft; generate core configuration'}</small>
        <button type="button" onClick={() => void refresh(true)} disabled={busy}>{lang === 'cn' ? '重新载入已保存节点图' : 'Reload saved graph'}</button><small>{lang === 'cn' ? '丢弃草稿并载入 IsOS 中已保存的节点图' : 'Discard draft and load the saved graph from IsOS'}</small>
      </div></details>
      <span role="status">{dirty ? copy.dirty : copy.clean} · {copy.savedRevision}: {revision.slice(0, 8) || '—'} · {copy.activeRevision}: {processStatus?.active_revision?.slice(0, 8) || '—'}</span>
    </div></header>
    {selectorBlocked && <p className="node-graph-error" role="status">{lang === 'cn' ? '当前草稿的目录选项尚未就绪，或已保存选项不可用。检查图仍可使用；修复后才能保存或运行。' : 'Catalog options for this draft are pending or a saved choice is unavailable. Check graph remains available; repair the choice before saving or running.'}</p>}
    {readOnlyReason && <p className="node-graph-error" role="alert">{lang === 'cn' ? '只读节点图：' : 'Read-only graph: '}{readOnlyReason}</p>}
    {readiness?.cores.map((core) => {
      const disabled = core.enabled === false;
      const alert = !disabled && !core.ready;
      return <p key={core.core} className={alert ? 'node-graph-error' : 'node-graph-note'} role={alert ? 'alert' : 'status'}>
        <strong>{core.core.toUpperCase()}</strong>: {core.configured ? `${core.version || '—'} · ${core.path || '—'}` : (lang === 'cn' ? '未配置程序路径' : 'Binary path not configured')}
        {' · '}{disabled ? (lang === 'cn' ? '保存的节点图中未启用' : 'Not enabled in the saved graph') : (core.running ? (lang === 'cn' ? '运行中' : 'Running') : (lang === 'cn' ? '已停止' : 'Stopped'))}
        {!disabled && core.reason ? ` · ${core.reason}` : ''}{!disabled && core.action ? ` · ${core.action}` : ''}
      </p>;
    })}
    {viewLost && <p className="node-graph-note" role="status">{lang === 'cn' ? '节点可能在视野之外。使用“显示全图”恢复。' : 'Nodes may be outside the view. Use Fit graph to recover.'}</p>}{processStatus?.last_error && <p className="node-graph-error" role="alert">{processStatus.last_error}</p>}
    <div className={`node-graph-body${libraryCollapsed ? ' node-graph-body--library-collapsed' : ''}${sideCollapsed ? ' node-graph-body--side-collapsed' : ''}`}><aside className="node-graph-library" hidden={libraryCollapsed}><h2>{copy.catalog}</h2><CatalogBrowser types={types} value={typeId} lang={lang} onChange={setTypeId} /><label>{copy.id}<input value={newId} placeholder={graph && types.find((type) => catalogTypeKey(type) === typeId) ? nextNodeId(graph, types.find((type) => catalogTypeKey(type) === typeId)!) : ''} onChange={(event) => setNewId(event.target.value.toLowerCase())} /></label><button type="button" onClick={() => add()} disabled={readOnly || !graph || !typeId}>{copy.add}</button><p>{copy.port}</p><p>{copy.hint}</p><div className="node-graph-legend" aria-label={lang === 'cn' ? '端口颜色' : 'Port colors'}><h3>{lang === 'cn' ? '端口颜色：准确数据类型' : 'Port color: exact data type'}</h3>{(dataTypes.length ? dataTypes.slice(0, 5).map((item) => item.id) : ['camera_frame', 'vision_frame', 'base_observation']).map((id) => <span key={id} style={{ '--port': dataTypeColor(id) } as React.CSSProperties}><i aria-hidden="true" />{id}</span>)}</div><div className="node-graph-interface-legend" aria-label={lang === 'cn' ? '接口视觉规则' : 'Interface visual rules'}><strong>{lang === 'cn' ? '接口视觉规则' : 'Interface visual rules'}</strong><span>{lang === 'cn' ? '锚点形状：进程内圆形 / Unix 方形 / HTTP 盾形 / WebSocket 菱形 / ROS 2 六边形' : 'Anchor shape: in-process circle / Unix square / HTTP shield / WebSocket diamond / ROS 2 hexagon'}</span><span>{lang === 'cn' ? '连线图案表示连线上限：<1 Hz 稀疏点；1–<10 Hz 点线；10–<30 Hz 长划线；≥30 Hz 实线；无限制为实线' : 'Line pattern shows edge cap: <1 Hz sparse dots; 1–<10 Hz dotted; 10–<30 Hz long dashes; ≥30 Hz solid; unlimited solid'}</span><span>{lang === 'cn' ? '连线颜色：中性灰；优先级线宽：高优先级更粗，低优先级更细' : 'Line color: neutral gray; higher priority is thicker, lower priority thinner'}</span></div><section className="node-graph-set-library"><h2>{copy.nodeSets}</h2>{nodeSets.length ? <><label>{copy.chooseSet}<select value={setKey} onChange={(event) => { const key = event.target.value; const chosenSet = nodeSets.find((set) => `${set.package.id}@${set.package.version}/${set.id}` === key); setSetKey(key); setSetValues(Object.fromEntries(Object.entries(chosenSet?.parameters || {}).map(([name, parameter]) => [name, parameter.default || '']))); setSetPreview(null); }}><option value="">—</option>{nodeSets.map((set) => { const key = `${set.package.id}@${set.package.version}/${set.id}`; return <option key={key} value={key}>{set.package.id}@{set.package.version} / {set.id}</option>; })}</select></label>{selectedSet && <><p>{selectedSet.nodes.length} {copy.previewCount}</p>{Object.entries(selectedSet.parameters).map(([name, parameter]) => <label key={name}>{name}<input type="text" value={setValues[name] ?? parameter.default ?? ''} onChange={(event) => { setSetValues((current) => ({ ...current, [name]: event.target.value })); setSetPreview(null); }} /></label>)}<button type="button" disabled={readOnly || !graph || busy} onClick={() => void previewPackageSet()}>{copy.previewSet}</button>{setPreview && <div className="node-graph-set-preview" role="status"><strong>{setPreview.result.added_paths.length} {copy.previewCount}</strong><ul>{setPreview.result.added_paths.map((path) => <li key={path}><code>{path}</code></li>)}</ul><button type="button" onClick={addPreviewedSet} disabled={readOnly || busy || setPreview.source !== JSON.stringify(graph)}>{copy.addSet}</button></div>}</>}</> : <p>{copy.noNodeSets}</p>}</section>{graph && <><h2>{copy.robot}</h2><label>{copy.id}<input value={graph.robot.id} onChange={(event) => edit((current) => ({ ...current, robot: { ...current.robot, id: event.target.value } }))} /></label><label>{copy.name}<input value={graph.robot.name} onChange={(event) => edit((current) => ({ ...current, robot: { ...current.robot, name: event.target.value } }))} /></label></>}</aside>
      <div className="node-graph-canvas" ref={canvasRef} onContextMenu={openQuickAdd} onPointerDownCapture={(event) => { if (event.button === 0 && !(event.target as HTMLElement).closest('.node-graph-edge-menu, .node-graph-quick-add')) { setQuickAdd(null); setEdgeMenu(null); } activeGesture.current = true; }} onPointerUpCapture={() => { activeGesture.current = false; }} onPointerCancelCapture={() => { activeGesture.current = false; }} onMouseMove={revealOverview} onTouchMove={revealOverview}>{graph ? <ReactFlow nodes={nodes} edges={edges} nodeTypes={nodeTypes} nodesConnectable={!readOnly} onInit={(instance) => { setFlowInstance(instance); browserLog('debug', 'Graph canvas initialized'); }} onEdgeContextMenu={openEdgeMenu} onEdgeClick={(_, edge) => { setEdgeInputInvalid(false); setSelectedEdge(edge.id); setSideCollapsed(false); setQuickAdd(null); }} onMove={revealOverview} onNodesChange={onNodesChange} onConnect={connect} onNodeClick={(_, node) => { setFrontNode(node.id); setQuickAdd(null); setSelectedEdge(''); setEdgeInputInvalid(false); }} onNodeDragStart={(_, node) => setFrontNode(node.id)} onSelectionStart={() => { activeGesture.current = true; }} onSelectionEnd={() => { activeGesture.current = false; }} selectionOnDrag selectionMode={SelectionMode.Partial} panOnDrag={[1]} deleteKeyCode={null} onPaneClick={() => { setSelected([]); setQuickAdd(null); setEdgeMenu(null); setSelectedEdge(''); setEdgeInputInvalid(false); }} onDoubleClick={(event) => { if ((event.target as HTMLElement).classList.contains('react-flow__pane')) add(true); }} minZoom={0.2} maxZoom={2}><Background color="var(--border)" /><MiniMap className={showOverview ? 'node-graph-minimap--visible' : 'node-graph-minimap--hidden'} pannable zoomable /></ReactFlow> : rawGraph ? <pre className="node-graph-raw" aria-label={lang === 'cn' ? '原始节点图' : 'Raw node graph'}>{JSON.stringify(rawGraph, null, 2)}</pre> : <div className="node-graph-empty"><p>{copy.disconnected}</p><button type="button" onClick={() => void refresh()}>{copy.connect}</button></div>}{graph && !entries.length && <p className="node-graph-empty">{copy.noNodes}</p>}{edgeMenu && <div className="node-graph-edge-menu" role="menu" style={{ left: edgeMenu.left, top: edgeMenu.top }}><button type="button" role="menuitem" onClick={() => { const edge = edges.find((item) => item.id === edgeMenu.id); if (edge) edit((current) => disconnectInput(current, edge.target, String(edge.targetHandle).replace(/^in:/, ''), edge.source, String(edge.sourceHandle).replace(/^out:/, ''))); setEdgeMenu(null); }}>{copy.disconnect}</button></div>}{quickAdd && <div className="node-graph-quick-add" role="dialog" aria-label={copy.add} style={{ left: quickAdd.left, top: quickAdd.top }} onContextMenu={(event) => event.stopPropagation()}><input ref={quickSearchRef} type="search" aria-label={copy.search} placeholder={copy.search} value={quickSearch} onChange={(event) => setQuickSearch(event.target.value)} /><div className="node-graph-quick-add__list">{quickTypes.map((type) => <button type="button" key={type.id} onMouseEnter={() => setQuickDetail(type.id)} onFocus={() => setQuickDetail(type.id)} onClick={() => addAtCursor(type)}><strong>{type.group}</strong><small>{type.core} / {type.layer} · {type.id}</small></button>)}</div>{quickDetail && <div className="node-graph-quick-add__detail" role="note">{(() => { const type = quickTypes.find((item) => item.id === quickDetail); return type ? <><strong>{type.id}</strong><p>{type.core} / {type.layer} / {type.group} · {type.lifecycle}</p><p>{type.capabilities.join(', ') || '—'}</p><small>{type.inputs.length} {lang === 'cn' ? '输入' : 'inputs'} · {type.outputs.length} {lang === 'cn' ? '输出' : 'outputs'} · {type.settings.length} {lang === 'cn' ? '设置' : 'settings'}</small></> : null; })()}</div>}</div>}
        {visible && <><GraphCoreWindow title="EI" state={coreWindows.egia} onChange={(patch) => updateCoreWindow('egia', patch)} lang={lang}><EgiaWorkbench lang={lang} onOpenGraph={() => updateCoreWindow('egia', { open: false })} /></GraphCoreWindow></>}
      </div>
      <aside className="node-graph-side" hidden={sideCollapsed}>{edgeDetail ? <EdgeIdlPanel key={edgeDetail.id} edge={edgeDetail} entries={entries} dataTypes={dataTypes} diagnostics={diagnostics} validated={validationCurrent} processStatus={processStatus} onOpenMonitor={() => onOpenPanel?.('control')} lang={lang} graph={graph!} validatedEdges={validatedEdges} readOnly={readOnly} onInvalidChange={setEdgeInputInvalid} onQosChange={(change) => { const input = String(edgeDetail.targetHandle).replace(/^in:/, ''); edit((current) => updateEdgeQos(current, edgeDetail.target, input, edgeDetail.data?.inputIndex ?? 0, change)); }} /> : chosen ? <><fieldset disabled={readOnly} className="node-graph-readonly-fieldset"><NodeInspector key={chosen.path} entry={chosen} entries={entries} lang={lang} options={nodeOptions} runId={processStatus?.run_id || null} activeRevision={processStatus?.active_revision || null} currentRun={!dirty && !readOnly && processStatus?.state === 'running' && processStatus.run_id_state === 'known' && processStatus.active_revision === revision && processStatus.saved_revision === revision} onBind={(target) => { try { edit((current) => bindPrimitiveTarget(current, types, chosen.path, target)); setMessage(''); } catch (error) { setMessage(error instanceof Error ? error.message : String(error), true); } }} onChange={(change) => edit((current) => updateNode(current, chosen.path, change))} onRename={(id) => { try { edit((current) => renameNode(current, chosen.path, id)); setSelected([`${chosen.path.slice(0, chosen.path.lastIndexOf('/') + 1)}${id}`]); setMessage(''); } catch { setMessage(copy.duplicate, true); } }} onRemove={() => { edit((current) => removeNode(current, chosen.path)); setPositions((current) => Object.fromEntries(Object.entries(current).filter(([path]) => path !== chosen.path))); setSelected([]); }} /></fieldset><h3>{copy.target}</h3><ul>{Object.entries(chosen.node.inputs || {}).flatMap(([input, sources]) => inputSources(sources).map((source) => <li key={`${input}:${source.node}:${source.output}`}>{input} ← {canonicalNodePath(source.node)}.{source.output}<button type="button" aria-label={`${copy.remove} ${input}`} disabled={readOnly} onClick={() => edit((current) => disconnectInput(current, chosen.path, input, source.node, source.output))}>×</button></li>))}</ul></> : selected.length > 1 ? <div className="node-graph-multi-selection"><p><strong>{selected.length}</strong> {copy.selectedCount}</p><button type="button" disabled={readOnly} onClick={() => { edit((current) => removeSelectedNodes(current, selected)); setPositions((current) => Object.fromEntries(Object.entries(current).filter(([path]) => !selected.includes(path)))); setSelected([]); }}>{copy.remove}</button></div> : <p>{copy.noSelection}</p>}
        <details className="node-graph-panel" open={diagnosticsOpen} onToggle={(event) => setDiagnosticsOpen(event.currentTarget.open)}>
          <summary><strong>{copy.diagnostics}</strong><span>{diagnostics.length || '✓'}</span></summary>
          {validationCurrent ? diagnostics.length ? <ul>{diagnostics.map((item, index) => <li key={index}><button className="node-graph-diagnostic" type="button" onClick={() => { setSelected([entries.find((entry) => item.path.includes(entry.path))?.path || ''].filter(Boolean)); setSideCollapsed(false); }}><code>{item.path}</code><span>{item.message}</span></button></li>)}</ul> : <p>{copy.valid}</p> : <p>{lang === 'cn' ? '当前草稿尚未检查。' : 'Current draft has not been checked.'}</p>}
        </details>
        <details className="node-graph-panel">
          <summary><strong>{copy.process}</strong><span>{processStatus?.processes.length ?? '—'}</span></summary>
          {processStatus ? <ul>{processStatus.processes.map((process) => <li key={process.id}><strong>{process.id}</strong><span>{process.state || 'running'} · PID {process.pid ?? '—'} · {process.restarts} restarts</span></li>)}</ul> : <p>{copy.noStatus}</p>}
        </details></aside></div>
  </main></ReactFlowProvider>;
}
