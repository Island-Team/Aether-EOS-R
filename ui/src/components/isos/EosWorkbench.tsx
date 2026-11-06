import { canonicalNodePath } from './node-graph-edits';
import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { eosRequest, type EosControlReply, type EosDiscoveryNode } from '@/services/eos/control';
import { VisionPanel } from './VisionPanel';
import { PrimitiveBaseCard } from './PrimitiveBaseCard';
import { resolvePrimitiveBindings } from '@/services/eos/primitive-command';
import { combineNodeTypeCatalogs, getGraphCatalogSnapshot, getProcessStatus, graphCatalogReadOnlyReason, nodeGraphHasEditableShape, type NodeGraphDocument, type NodeInstance, type ProcessStatus } from '@/services/isos/node-graph';
import type { Language } from '@/types';
import { publishUiMessage } from '@/services/isos/ui-messages';
import { activeGraphIsCurrent, defaultMovement, discoveredHardware, eosControlNodes, eosGraphNodes, hardwareKind, monitorBadge, movementTypes, movementValid, visionRuntimeState, type AdapterStatus, type EosGraphNode, type Movement, type MovementType } from './eos-control-model';
import './eos-workbench.scss';
import { WorkspacePanel } from './WorkspacePanel';
import type { WorkspaceLayout, Surface, PanelLayout } from './workspace-layout';

interface Props { lang: Language; onOpenGraph: () => void; onOpenLogs?: () => void; graphDirty: boolean; layout?: WorkspaceLayout; viewport?: { width: number; height: number }; onPanelChange?: (name: Surface, patch: Partial<PanelLayout>) => void; onRobot?: (id: string) => void; onStatus?: (status: ProcessStatus | null) => void; onConnectionError?: (message: string) => void; isRunStopFenced?: (runId: string | null) => boolean; onRunStopIntent?: (runId: string | null) => void; onStopOutcome?: (feedback: { text: string; alert: boolean }, terminalRun?: string | null) => void }
interface Health { state: string; code?: string | null; message?: string | null }
interface Reading { status?: AdapterStatus; statusAt?: number; health?: Health; healthAt?: number; observation?: Record<string, unknown>; observationAt?: number; error?: string }
const COPY = {
  en: { title: 'EOS control', intro: 'Run graph-declared primitives and skills through EOS admission. Inspect live hardware data below.', refresh: 'Refresh', graph: 'Edit graph', active: 'Active revision', saved: 'Saved revision', off: 'EOS is not running with this saved graph. Apply and run the graph before calling controls.', dirty: 'Apply graph edits before calling controls.', controls: 'Controls', noControls: 'Add a manual, primitive, or skill node in Graph to expose a control here.', target: 'Target', mode: 'Movement', invoke: 'Invoke', stop: 'Stop', execute: 'Execute skill', cancel: 'Cancel skill', result: 'Last result', pending: 'Running', parameters: 'Parameters', invalid: 'Enter values within the hardware limits.', unavailable: 'The target hardware node is missing or not admitted.', monitor: 'Data monitor', camera: 'Camera flow', noCamera: 'No camera is declared and discovered.', noHardware: 'No graph-declared hardware is discovered by EOS.', live: 'Live', stale: 'Stale', starting: 'Starting', degraded: 'Degraded', fault: 'Fault', rearm: 'Target is stopped. Restart EOS to rearm it.', notReady: 'Waiting for fresh ready status and healthy health.', health: 'Health', observation: 'Observation', frame: 'Frame', sequence: 'Sequence', depth: 'Depth', dimensions: 'Dimensions', update: 'Last update', stopped: 'Stopped', choose: 'Select camera', noData: 'Waiting for a sample…', status: 'Status', command: 'Command' },
  cn: { title: 'EOS 控制', intro: '通过 EOS 准入执行节点图声明的原语和技能，并查看下方实时硬件数据。', refresh: '刷新', graph: '编辑节点图', active: '运行中修订', saved: '已保存修订', off: 'EOS 尚未运行此已保存节点图。请先应用并运行节点图。', dirty: '请先应用节点图修改，再调用控制。', controls: '控制', noControls: '在节点图中添加手动、原语或技能节点后，这里会显示对应控制。', target: '目标', mode: '运动模式', invoke: '调用', stop: '停止', execute: '执行技能', cancel: '取消技能', result: '最近结果', pending: '运行中', parameters: '参数', invalid: '参数超出硬件限制。', unavailable: '目标硬件节点缺失或未获准入。', monitor: '数据监视器', camera: '相机画面', noCamera: '没有已声明并被发现的相机。', noHardware: 'EOS 未发现节点图中声明的硬件。', live: '实时', stale: '过期', starting: '启动中', degraded: '降级', fault: '故障', rearm: '目标已停止。请重启 EOS 以重新启用。', notReady: '正在等待新的就绪状态和健康数据。', health: '健康', observation: '观测', frame: '帧', sequence: '序号', depth: '深度', dimensions: '尺寸', update: '最近更新', stopped: '已停止', choose: '选择相机', noData: '等待采样…', status: '状态', command: '命令' },
} as const;

function asNumber(value: string): number { return value.trim() ? Number(value) : Number.NaN; }
function NumberInput({ label, value, unit, max, onChange }: { label: string; value: number; unit: string; max?: number; onChange: (value: number) => void }): React.ReactElement {
  return <label className="eos-control__field"><span>{label} <small>{unit}</small></span><input type="number" step="any" min={max === undefined ? undefined : -max} max={max} value={Number.isNaN(value) ? '' : value} onChange={(event) => onChange(asNumber(event.target.value))} /></label>;
}

function MovementEditor({ value, hardware, onChange, lang }: { value: Movement; hardware: NodeInstance; onChange: (value: Movement) => void; lang: Language }): React.ReactElement {
  const limits = hardware.movement?.limits || {};
  if (value.type === 'body_velocity') return <div className="eos-control__fields">
    <NumberInput label="X" unit="m/s" max={limits.max_linear_m_s} value={value.x_m_s} onChange={(n) => onChange({ ...value, x_m_s: n })} />
    <NumberInput label="Y" unit="m/s" max={limits.max_linear_m_s} value={value.y_m_s} onChange={(n) => onChange({ ...value, y_m_s: n })} />
    <NumberInput label="Yaw" unit="rad/s" max={limits.max_angular_rad_s} value={value.yaw_rad_s} onChange={(n) => onChange({ ...value, yaw_rad_s: n })} />
    <NumberInput label={lang === 'cn' ? '有效期' : 'Lease'} unit="ms" max={limits.max_velocity_lease_ms} value={value.valid_for_ms} onChange={(n) => onChange({ ...value, valid_for_ms: n })} />
  </div>;
  if (value.type === 'relative_pose') return <div className="eos-control__fields">
    <NumberInput label="X" unit="m" max={limits.max_translation_m} value={value.x_m} onChange={(n) => onChange({ ...value, x_m: n })} />
    <NumberInput label="Y" unit="m" max={limits.max_translation_m} value={value.y_m} onChange={(n) => onChange({ ...value, y_m: n })} />
    <NumberInput label="Yaw" unit="rad" max={limits.max_rotation_rad} value={value.yaw_rad} onChange={(n) => onChange({ ...value, yaw_rad: n })} />
    <label className="eos-control__field"><span>Frame ID</span><input value={value.frame_id} onChange={(event) => onChange({ ...value, frame_id: event.target.value })} /></label>
  </div>;
  if (value.type === 'joint_positions') return <div className="eos-control__fields">
    {value.joint_names.map((name, index) => <NumberInput key={name} label={name} unit="rad" max={limits.max_joint_position_rad} value={value.joint_positions_rad[index]} onChange={(n) => onChange({ ...value, joint_positions_rad: value.joint_positions_rad.map((position, i) => i === index ? n : position) })} />)}
    <NumberInput label={lang === 'cn' ? '最大速度' : 'Max speed'} unit="rad/s" max={limits.max_joint_speed_rad_s} value={value.max_speed_rad_s ?? Number.NaN} onChange={(n) => onChange({ ...value, max_speed_rad_s: Number.isNaN(n) ? null : n })} />
  </div>;
  const pose = value.pose;
  return <div className="eos-control__fields">
    {pose.position_m.map((position, index) => <NumberInput key={index} label={['X', 'Y', 'Z'][index]} unit="m" max={limits.max_end_effector_translation_m} value={position} onChange={(n) => onChange({ ...value, pose: { ...pose, position_m: pose.position_m.map((old, i) => i === index ? n : old) as [number, number, number] } })} />)}
    {pose.orientation_xyzw.map((orientation, index) => <NumberInput key={index} label={['Qx', 'Qy', 'Qz', 'Qw'][index]} unit="" max={1} value={orientation} onChange={(n) => onChange({ ...value, pose: { ...pose, orientation_xyzw: pose.orientation_xyzw.map((old, i) => i === index ? n : old) as [number, number, number, number] } })} />)}
    <label className="eos-control__field"><span>Frame ID</span><input value={pose.frame_id} onChange={(event) => onChange({ ...value, pose: { ...pose, frame_id: event.target.value } })} /></label>
    <NumberInput label={lang === 'cn' ? '最大速度' : 'Max speed'} unit="m/s" max={limits.max_end_effector_speed_m_s} value={value.max_speed_m_s ?? Number.NaN} onChange={(n) => onChange({ ...value, max_speed_m_s: Number.isNaN(n) ? null : n })} />
  </div>;
}

function MovementControl({ entry, hardware, reading, clock, active, lang }: { entry: EosGraphNode; hardware?: EosGraphNode; reading?: Reading; clock: number; active: boolean; lang: Language }): React.ReactElement {
  const copy = COPY[lang];
  const fixed = entry.node.node_type === 'eos.control.primitive@v1.0' ? entry.node.settings?.movement_type as MovementType : undefined;
  const modes = hardware ? movementTypes(hardware.node).filter((mode) => !fixed || mode === fixed) : [];
  const [mode, setMode] = useState<MovementType>(modes[0] || 'body_velocity');
  const [movement, setMovement] = useState<Movement>(() => defaultMovement(modes[0] || 'body_velocity', hardware?.node || entry.node));
  const [busy, setBusy] = useState(false);
  useEffect(() => { const next = modes[0] || 'body_velocity'; setMode(next); setMovement(defaultMovement(next, hardware?.node || entry.node)); }, [entry.path, hardware?.path, fixed]);
  const valid = Boolean(hardware && modes.includes(mode) && movementValid(movement, hardware.node));
  const targetBadge = monitorBadge(reading, clock);
  const ready = targetBadge === 'live';
  const invoke = async (): Promise<void> => {
    if (!active || !valid || !ready) return;
    setBusy(true);
    try {
      const time = await eosRequest<EosControlReply & { monotonic_ns: number }>({ op: 'time' });
      const reply = await eosRequest<EosControlReply & { result?: unknown }>({ op: fixed ? 'primitive' : 'manual', node_path: entry.path, command_id: crypto.randomUUID(), deadline_monotonic_ns: time.monotonic_ns + 5_000_000_000, movement });
      publishUiMessage(JSON.stringify(reply.result ?? reply));
    } catch (error) { publishUiMessage(error instanceof Error ? error.message : String(error), 'error'); }
    finally { setBusy(false); }
  };
  const stop = async (): Promise<void> => {
    if (!active || fixed) return;
    try { await eosRequest({ op: 'manual_stop', node_path: entry.path }); publishUiMessage(copy.stopped); }
    catch (error) { publishUiMessage(error instanceof Error ? error.message : String(error), 'error'); }
  };
  return <article className="eos-control__card"><header><span className="eos-control__type">{fixed ? 'PRIMITIVE' : 'MANUAL'}</span><h3>{entry.node.label || entry.node.id}</h3><code>{entry.node.id}</code></header>
    <p>{copy.target}: <strong>{hardware?.node.label || hardware?.node.id || copy.unavailable}</strong></p>
    {hardware && modes.length > 0 ? <><label className="eos-control__field"><span>{copy.mode}</span><select value={mode} disabled={Boolean(fixed)} onChange={(event) => { const next = event.target.value as MovementType; setMode(next); setMovement(defaultMovement(next, hardware.node)); }}>{modes.map((name) => <option key={name} value={name}>{name}</option>)}</select></label><MovementEditor value={movement} hardware={hardware.node} onChange={setMovement} lang={lang} />
      {!valid && <p role="alert" className="eos-control__warning">{copy.invalid}</p>}
      {active && !ready && <p role="status" className="eos-control__warning">{targetBadge === 'stopped' ? copy.rearm : copy.notReady} <code>{reading?.status?.state || copy.stale}</code></p>}
      <div className="eos-control__actions"><button type="button" className="eos-control__primary" disabled={!active || !valid || !ready || busy} onClick={() => void invoke()}>{busy ? copy.pending : copy.invoke}</button>{!fixed && <button type="button" disabled={!active} onClick={() => void stop()}>{copy.stop}</button>}</div></> : <p role="alert" className="eos-control__warning">{copy.unavailable}</p>}
  </article>;
}

interface SkillStep { primitive: EosGraphNode; hardware: EosGraphNode; movement: Movement }
function SkillControl({ entry, nodes, hardware, readings, clock, active, lang }: { entry: EosGraphNode; nodes: EosGraphNode[]; hardware: EosGraphNode[]; readings: Record<string, Reading>; clock: number; active: boolean; lang: Language }): React.ReactElement {
  const copy = COPY[lang];
  const initial = useMemo(() => {
    const paths = Array.isArray(entry.node.settings?.steps) ? entry.node.settings.steps : [];
    return paths.flatMap((path) => {
      const primitive = nodes.find((candidate) => candidate.path === canonicalNodePath(path) && candidate.node.node_type === 'eos.control.primitive@v1.0');
      const target = hardware.find((candidate) => candidate.path === canonicalNodePath(String(primitive?.node.settings?.target || '')));
      const type = primitive?.node.settings?.movement_type as MovementType;
      return primitive && target && movementTypes(target.node).includes(type) ? [{ primitive, hardware: target, movement: defaultMovement(type, target.node) }] : [];
    });
  }, [entry, nodes, hardware]);
  const [steps, setSteps] = useState<SkillStep[]>(initial);
  const [busy, setBusy] = useState(false);
  const [executionId, setExecutionId] = useState('');
  useEffect(() => { setSteps(initial); }, [initial]);
  const configuredCount = Array.isArray(entry.node.settings?.steps) ? entry.node.settings.steps.length : 0;
  const valid = configuredCount > 0 && configuredCount <= 16 && steps.length === configuredCount && steps.every((step) => movementValid(step.movement, step.hardware.node));
  const blockedTargets = steps.filter((step) => monitorBadge(readings[step.hardware.path], clock) !== 'live');
  const execute = async (): Promise<void> => {
    if (!active || !valid || blockedTargets.length || busy) return;
    const id = crypto.randomUUID(); setBusy(true); setExecutionId(id);
    try { const reply = await eosRequest<EosControlReply & { execution?: unknown }>({ op: 'skill_execute', node_path: entry.path, execution_id: id, steps: steps.map(({ movement }) => ({ movement })) }); publishUiMessage(JSON.stringify(reply.execution ?? reply)); }
    catch (error) { publishUiMessage(error instanceof Error ? error.message : String(error), 'error'); setBusy(false); }
  };
  useEffect(() => {
    if (!busy || !executionId) return;
    let disposed = false;
    const timer = window.setInterval(async () => {
      if (disposed || document.visibilityState !== 'visible') return;
      try {
        const reply = await eosRequest<EosControlReply & { execution?: { status: string } }>({ op: 'skill_result', execution_id: executionId });
        if (disposed) return;
        const snapshot = reply.execution;
        if (snapshot && ['completed', 'failed', 'cancelled'].includes(snapshot.status)) publishUiMessage(JSON.stringify(snapshot ?? reply), snapshot.status === 'failed' ? 'error' : 'info');
        if (snapshot && ['completed', 'failed', 'cancelled'].includes(snapshot.status)) setBusy(false);
      } catch (error) { if (!disposed) { publishUiMessage(error instanceof Error ? error.message : String(error), 'error'); setBusy(false); } }
    }, 1000);
    return () => { disposed = true; window.clearInterval(timer); };
  }, [busy, executionId]);
  const cancel = async (): Promise<void> => {
    try { const reply = await eosRequest<EosControlReply & { execution?: unknown }>({ op: 'skill_cancel', execution_id: executionId }); publishUiMessage(JSON.stringify(reply.execution ?? reply)); setBusy(false); }
    catch (error) { publishUiMessage(error instanceof Error ? error.message : String(error), 'error'); }
  };
  return <article className="eos-control__card eos-control__card--skill"><header><span className="eos-control__type">SKILL</span><h3>{entry.node.label || entry.node.id}</h3><code>{entry.node.id}</code></header>
    {steps.map((step, index) => <section className="eos-control__step" key={`${index}:${step.primitive.path}`}><h4>{index + 1}. {step.primitive.node.label || step.primitive.node.id} → {step.hardware.node.label || step.hardware.node.id}</h4><p>{step.movement.type}</p><MovementEditor lang={lang} hardware={step.hardware.node} value={step.movement} onChange={(movement) => setSteps((current) => current.map((item, i) => i === index ? { ...item, movement } : item))} /></section>)}
    {!valid && <p role="alert" className="eos-control__warning">{copy.invalid}</p>}
    {active && blockedTargets.length > 0 && <p role="status" className="eos-control__warning">{blockedTargets.some((step) => monitorBadge(readings[step.hardware.path], clock) === 'stopped') ? copy.rearm : copy.notReady} {blockedTargets.map((step) => step.hardware.node.label || step.hardware.node.id).join(', ')}</p>}
    <div className="eos-control__actions"><button type="button" className="eos-control__primary" disabled={!active || !valid || blockedTargets.length > 0 || busy} onClick={() => void execute()}>{busy ? copy.pending : copy.execute}</button>{busy && <button type="button" onClick={() => void cancel()}>{copy.cancel}</button>}</div>
  </article>;
}

function Observation({ kind, value }: { kind: string; value?: Record<string, unknown> }): React.ReactElement {
  if (!value) return <span>—</span>;
  if (kind === 'base') return <dl className="eos-monitor__metrics">{(['x_m', 'y_m', 'yaw_rad', 'x_m_s', 'y_m_s', 'yaw_rad_s'] as const).map((key) => <React.Fragment key={key}><dt>{key}</dt><dd>{typeof value[key] === 'number' ? (value[key] as number).toFixed(3) : '—'}</dd></React.Fragment>)}</dl>;
  if (kind === 'manipulator') { const names = value.joint_names; const positions = value.joint_positions_rad; return <dl className="eos-monitor__metrics">{Array.isArray(names) && Array.isArray(positions) ? names.map((name, index) => <React.Fragment key={`${index}`}><dt>{String(name)}</dt><dd>{typeof positions[index] === 'number' ? positions[index].toFixed(3) : '—'} rad</dd></React.Fragment>) : null}</dl>; }
  return <span>—</span>;
}

export function EosWorkbench({ lang, onOpenGraph, onOpenLogs, graphDirty, layout, viewport, onPanelChange, onRobot, onStatus, onConnectionError, isRunStopFenced, onRunStopIntent, onStopOutcome }: Props): React.ReactElement {
  const copy = COPY[lang];
  const [graphDoc, setGraphDoc] = useState<NodeGraphDocument | null>(null);
  const [readOnlyReason, setReadOnlyReason] = useState<string | null>(null);
  const [status, setStatus] = useState<ProcessStatus | null>(null);
  const [stopFenceRunId, setStopFenceRunId] = useState<string | null>(null);
  const stopFenceRef = useRef<string | null>(null);
  const markStopIntent = (runId: string | null): void => { stopFenceRef.current = runId; setStopFenceRunId(runId); onRunStopIntent?.(runId); };
  const [discovery, setDiscovery] = useState<EosDiscoveryNode[]>([]);
  const [typeDescriptions, setTypeDescriptions] = useState<Record<string, string>>({});
  const [readings, setReadings] = useState<Record<string, Reading>>({});
  const lastError = useRef('');
  const [clock, setClock] = useState(Date.now());
  const [manualPending, setManualPending] = useState(false);
  const callbacks = useRef({ onStatus, onRobot, onConnectionError });
  callbacks.current = { onStatus, onRobot, onConnectionError };
  const mounted = useRef(false);
  const lifecycle = useRef(0);
  const refreshPending = useRef(false);
  const manualQueued = useRef(false);
  const publishedIdentity = useRef<string | null>(null);
  const refreshRequest = useRef(0);
  const refresh = useCallback(async (manual = false): Promise<void> => {
    if (!mounted.current) return;
    if (manual) {
      ++refreshRequest.current;
      setManualPending(true);
      setDiscovery([]);
      setReadings({});
      publishedIdentity.current = null;
    }
    if (refreshPending.current) {
      if (manual) manualQueued.current = true;
      return;
    }
    refreshPending.current = true;
    const requestId = ++refreshRequest.current;
    const requestLifecycle = lifecycle.current;
    const current = (): boolean => mounted.current && requestLifecycle === lifecycle.current && requestId === refreshRequest.current;
    let processAvailable = false;
    try {
      const [snapshot, process] = await Promise.all([getGraphCatalogSnapshot(), getProcessStatus()]);
      const { graph } = snapshot;
      if (!current()) return;
      processAvailable = true;
      const controlReadOnlyReason = graphCatalogReadOnlyReason(snapshot);
      const identity = JSON.stringify([graph.revision, process.run_id || null]);
      if (publishedIdentity.current !== identity || controlReadOnlyReason) {
        setDiscovery([]);
        setReadings({});
      }
      publishedIdentity.current = identity;
      setTypeDescriptions(Object.fromEntries(combineNodeTypeCatalogs(snapshot.selected, snapshot.global).types.map((type) => [type.id, type.description || ''])));
      setReadOnlyReason(controlReadOnlyReason);
      setGraphDoc((previous) => nodeGraphHasEditableShape(graph.graph) ? previous?.revision === graph.revision ? previous : graph : null); setStatus(process); callbacks.current.onStatus?.(process); callbacks.current.onRobot?.(nodeGraphHasEditableShape(graph.graph) ? graph.graph.robot.id : 'unknown'); lastError.current = '';
      if (!controlReadOnlyReason && activeGraphIsCurrent(graph.revision, process.active_revision)) {
        const found = await eosRequest<EosControlReply & { nodes: EosDiscoveryNode[] }>({ op: 'discover' });
        if (!current()) return;
        setDiscovery(found.nodes); callbacks.current.onConnectionError?.('');
      } else { setDiscovery([]); setReadings({}); callbacks.current.onConnectionError?.(''); }
    } catch (reason) {
      if (!current()) return;
      const message = reason instanceof Error ? reason.message : String(reason);
      if (lastError.current !== message) publishUiMessage(message, 'error'); lastError.current = message; setReadOnlyReason('Graph compatibility could not be checked.'); setDiscovery([]); setReadings({});
      publishedIdentity.current = null;
      if (!processAvailable) { setStatus(null); callbacks.current.onStatus?.(null); }
      callbacks.current.onConnectionError?.(message);
    } finally {
      refreshPending.current = false;
      if (mounted.current && requestLifecycle !== lifecycle.current) {
        manualQueued.current = false;
        void refresh();
      } else if (mounted.current && manualQueued.current) {
        manualQueued.current = false;
        void refresh();
      } else if (current()) setManualPending(false);
    }
  }, []);
  useEffect(() => {
    ++lifecycle.current;
    mounted.current = true;
    void refresh();
    const timer = window.setInterval(() => { if (document.visibilityState === 'visible') void refresh(); }, 5000);
    return () => { mounted.current = false; ++lifecycle.current; ++refreshRequest.current; manualQueued.current = false; window.clearInterval(timer); };
  }, [refresh]);
  const panelVisible = (name: Surface): boolean => Boolean(layout?.[name].open && !layout?.[name].collapsed);
  const panel = (name: Surface, title: string, children: React.ReactNode): React.ReactNode => layout && viewport && onPanelChange ? <WorkspacePanel title={title} lang={lang} layout={layout[name]} viewport={viewport} onChange={(patch) => onPanelChange(name, patch)}>{children}</WorkspacePanel> : children;
  const active = Boolean(graphDoc && !manualPending && !readOnlyReason && activeGraphIsCurrent(graphDoc.revision, status?.active_revision) && status?.state === 'running' && !graphDirty && !isRunStopFenced?.(status?.run_id || null));
  const { active: visionActive, graph: visionGraph } = visionRuntimeState(graphDoc, status);
  const nodes = useMemo(() => graphDoc ? eosGraphNodes(graphDoc.graph) : [], [graphDoc]);
  const controls = useMemo(() => graphDoc ? eosControlNodes(graphDoc.graph) : [], [graphDoc]);
  const discoveredPaths = discovery.map((entry) => entry.path).join('|');
  const hardware = useMemo(() => graphDoc ? discoveredHardware(graphDoc.graph, discovery).slice(0, 12) : [], [graphDoc, discoveredPaths]);
  useEffect(() => {
    if (!active || !hardware.length || (layout && !panelVisible('control') && !panelVisible('monitor'))) return;
    let disposed = false; let pending = false;
    const poll = async (): Promise<void> => {
      if (disposed || pending || document.visibilityState !== 'visible') return;
      pending = true;
      await Promise.all(hardware.map(async ({ path, node }) => {
        const [statusResult, healthResult, observationResult] = await Promise.allSettled([
          eosRequest<EosControlReply & { status: AdapterStatus }>({ op: 'status', node_path: path }),
          eosRequest<EosControlReply & { health: Health }>({ op: 'health', node_path: path }),
          hardwareKind(node.node_type) === 'camera' ? Promise.resolve(null) : eosRequest<EosControlReply & { observation: Record<string, unknown> }>({ op: 'observe', node_path: path }),
        ]);
        if (disposed) return;
        const at = Date.now();
        const errors = [statusResult, healthResult, observationResult].flatMap((result) => result.status === 'rejected' ? [result.reason instanceof Error ? result.reason.message : String(result.reason)] : []);
        setReadings((current) => {
          const previous = current[path] || {};
          return { ...current, [path]: {
            ...previous,
            ...(statusResult.status === 'fulfilled' ? { status: statusResult.value.status, statusAt: at } : {}),
            ...(healthResult.status === 'fulfilled' ? { health: healthResult.value.health, healthAt: at } : {}),
            ...(observationResult.status === 'fulfilled' && observationResult.value ? { observation: observationResult.value.observation, observationAt: at } : {}),
            error: errors.length ? errors.join('; ') : undefined,
          } };
        });
      }));
      pending = false; if (!disposed) setClock(Date.now());
    };
    void poll(); const timer = window.setInterval(() => { void poll(); }, 2000);
    return () => { disposed = true; window.clearInterval(timer); };
  }, [active, status?.run_id, graphDoc?.revision, layout?.control.open, layout?.control.collapsed, layout?.monitor.open, layout?.monitor.collapsed, hardware.map(({ path }) => path).join('|')]);
  return <div className="eos-workbench">
    <header className="eos-workbench__header"><div><p className="isos-eyebrow">EOS · Aether v2</p><h1>{copy.title}</h1><p>{copy.intro}</p></div><div className="eos-control__actions"><button type="button" onClick={() => void refresh(true)}>{copy.refresh}</button><button type="button" onClick={onOpenGraph}>{copy.graph}</button></div></header>
    <nav className="eos-workbench__section-nav" aria-label={lang === 'cn' ? 'EOS 面板' : 'EOS sections'}><a href="#eos-controls">{copy.controls}</a><a href="#eos-monitor">{copy.monitor}</a><a href="#eos-vision">{lang === 'cn' ? '视觉' : 'Vision'}</a></nav>
    <div className="eos-workbench__revisions"><span>{copy.saved}: <code>{graphDoc?.revision || '—'}</code></span><span>{copy.active}: <code>{status?.active_revision || '—'}</code></span></div>
    {graphDirty && <p role="status" className="eos-control__warning">{copy.dirty}</p>}
    {readOnlyReason && <p role="alert" className="eos-control__warning">{lang === 'cn' ? '只读节点图：' : 'Read-only graph: '}{readOnlyReason}</p>}
    {!active && <p role="status" className="eos-control__warning">{copy.off}</p>}
    {panel('control', copy.controls, <section id="eos-controls" className="eos-workbench__section"><h2>{copy.controls}</h2><div className="eos-workbench__controls">{(graphDoc ? resolvePrimitiveBindings(graphDoc.graph) : []).map((binding) => <PrimitiveBaseCard visible={!layout || panelVisible('control')} key={binding.path} bindingPath={binding.path} typeDescriptions={typeDescriptions} stopFenced={Boolean(status?.run_id && (status.run_id === stopFenceRunId || isRunStopFenced?.(status.run_id)))} isStopFenced={() => Boolean(status?.run_id && (status.run_id === stopFenceRef.current || isRunStopFenced?.(status.run_id)))} onStopIntent={markStopIntent} onStopOutcome={onStopOutcome} lang={lang} graph={graphDoc} status={status} dirty={graphDirty} readOnly={Boolean(readOnlyReason || manualPending)} readings={readings} clock={clock} onStop={(next) => { setStatus(next); onStatus?.(next); }} onOpenLogs={onOpenLogs || (() => { window.location.href = `/${lang}/isos?page=home&logs=1`; })} />)}{(!graphDoc || resolvePrimitiveBindings(graphDoc.graph).length === 0) && <PrimitiveBaseCard visible={!layout || panelVisible('control')} lang={lang} stopFenced={Boolean(status?.run_id && (status.run_id === stopFenceRunId || isRunStopFenced?.(status.run_id)))} isStopFenced={() => Boolean(status?.run_id && (status.run_id === stopFenceRef.current || isRunStopFenced?.(status.run_id)))} onStopIntent={markStopIntent} onStopOutcome={onStopOutcome} graph={graphDoc} status={status} dirty={graphDirty} readOnly={Boolean(readOnlyReason || manualPending)} readings={readings} clock={clock} onStop={(next) => { setStatus(next); onStatus?.(next); }} onOpenLogs={onOpenLogs || (() => { window.location.href = `/${lang}/isos?page=home&logs=1`; })} />}</div></section>)}
    {panel('monitor', copy.monitor, <section id="eos-monitor" className="eos-workbench__section"><h2>{copy.monitor}</h2>{hardware.length ? <div className="eos-monitor__grid">{hardware.map((entry) => {
      const kind = hardwareKind(entry.node.node_type); const reading = readings[entry.path]; const badge = monitorBadge(reading, clock);
      return <article className={`eos-monitor__card eos-monitor__card--${kind}`} key={entry.path}><header><h3>{entry.node.label || entry.node.id}</h3><span className={`eos-monitor__${badge}`}>{copy[badge]}</span></header><p><code>{kind}</code> · {copy.status}: {reading?.status?.state || '—'} · {copy.health}: {reading?.health?.state || '—'}</p>{reading?.status?.active_command_id && <p>{copy.command}: <code>{reading.status.active_command_id}</code></p>}{reading?.health?.message && <p>{reading.health.message}</p>}{reading?.error && <p role="alert">{reading.error}</p>}{kind !== 'camera' && <><h4>{copy.observation}</h4><Observation kind={kind || ''} value={reading?.observation} />{reading?.observationAt && <p>{copy.update}: {new Date(reading.observationAt).toLocaleTimeString()}</p>}</>}</article>;
    })}</div> : <p>{copy.noHardware}</p>}</section>)}
    {panel('vision', lang === 'cn' ? '视觉' : 'Vision', <VisionPanel lang={lang} graph={visionGraph} active={visionActive && (!layout || panelVisible('vision'))} revision={graphDoc?.revision || ''} status={status} stopFenced={Boolean(status?.run_id && (status.run_id === stopFenceRunId || isRunStopFenced?.(status.run_id)))} />)}
  </div>;
}
