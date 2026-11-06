import React, { useEffect, useRef, useState } from 'react';
import {
  getCurrentLogPage, getProcessStatus, isCurrentLogPage,
  type LogLevel, type LogLine, type LogPage, type ProcessStatus,
} from '../../services/isos/node-graph';
import { IsosApiError } from '../../services/isos/api-client';
import type { Language } from '@/types';
import { getBrowserLogs, subscribeBrowserLogs } from '../../services/isos/browser-logs';
import {
  acceptCurrentLogPage, acknowledgeCurrentLogDiagnostics, emptyCurrentLogState, resetCurrentLogDetails,
  validCorrelationFilter, validNodeFilter,
} from './log-cursor-state';
import './isos-logs-panel.scss';

const SOURCES = ['all', 'isos', 'packages', 'graph', 'browser', 'ui', 'services', 'eos', 'egia', 'mujoco', 'bridge', 'config'];
export const LOG_LEVELS = ['trace', 'debug', 'info', 'warn', 'error'] as const;
const POLL_MS = 1200;
const RETRY_MS = 30_000;

/** Browser logs are separate local diagnostics and may use legacy level names. */
export function visibleLogLines(lines: LogLine[], threshold: LogLevel): LogLine[] {
  const minimum = LOG_LEVELS.indexOf(threshold);
  return lines.filter((line) => {
    const level = line.level.toLowerCase() === 'warning' ? 'warn' : line.level.toLowerCase();
    const rank = LOG_LEVELS.indexOf(level as LogLevel);
    return (rank < 0 ? LOG_LEVELS.indexOf('warn') : rank) >= minimum;
  });
}

/** A retained rotation is not a loss; only the current-view scope changes rows. */
export function appendLogPage(current: LogLine[], page: LogPage): LogLine[] {
  return [...current, ...page.lines].slice(-1000);
}

function displayLine(line: LogLine): string {
  const identity = [line.source, line.core, line.event_code, line.run_id].filter(Boolean).join('/');
  const related = [line.node_path, line.correlation_id ? `correlation:${line.correlation_id}` : null, line.pid ? `pid:${line.pid}` : null].filter(Boolean).join(' ');
  return `${line.timestamp} ${line.level.toUpperCase()} [${identity || line.source}] ${line.message}${related ? ` (${related})` : ''}${line.fields && Object.keys(line.fields).length ? ` ${JSON.stringify(line.fields)}` : ''}`;
}

export function IsosLogsPanel({ lang, onClose, initialSource = 'all', sources = [], visible = true }: { lang: Language; onClose: () => void; initialSource?: string; sources?: string[]; visible?: boolean }): React.ReactElement {
  const [source, setSource] = useState(initialSource);
  const [level, setLevel] = useState<LogLevel>('info');
  const [nodePath, setNodePath] = useState('');
  const [correlationId, setCorrelationId] = useState('');
  const [current, setCurrent] = useState(emptyCurrentLogState);
  const [legacyLines, setLegacyLines] = useState<LogLine[]>([]);
  const [compatibility, setCompatibility] = useState(false);
  const [stale, setStale] = useState(false);
  const [anchorAnnouncement, setAnchorAnnouncement] = useState(false);
  const [status, setStatus] = useState<ProcessStatus | null>(null);
  const [error, setError] = useState('');
  const [browserLines, setBrowserLines] = useState(getBrowserLogs);
  const [following, setFollowing] = useState(true);
  const cursor = useRef<string | undefined>(undefined);
  const anchorRef = useRef<HTMLPreElement>(null);
  const observedAnchor = useRef<{ key: string; state: string } | null>(null);
  const announcedAnchor = useRef<string | null>(null);
  useEffect(() => { if (!visible) return; setBrowserLines(getBrowserLogs()); return subscribeBrowserLogs(() => setBrowserLines(getBrowserLogs())); }, [visible]);
  useEffect(() => {
    cursor.current = undefined;
    setCurrent(resetCurrentLogDetails);
    setLegacyLines([]);
    setError('');
  }, [source, level, nodePath, correlationId]);
  useEffect(() => {
    if (!following || !visible || source === 'browser') return;
    const invalidFilter = (nodePath && !validNodeFilter(nodePath)) || (correlationId && !validCorrelationFilter(correlationId));
    if (invalidFilter) return;
    let stopped = false;
    let pending = false;
    let needsStatus = true;
    let statusIdentityUnavailable = false;
    let retryAt = 0;
    let timer: number | undefined;
    let generation = 0;
    let controller: AbortController | null = null;
    const schedule = (delay: number): void => { if (!stopped) timer = window.setTimeout(() => { void poll(); }, delay); };
    const poll = async (): Promise<void> => {
      if (stopped || pending || document.visibilityState !== 'visible' || Date.now() < retryAt) return;
      pending = true;
      const requestGeneration = generation;
      const requestController = new AbortController();
      controller = requestController;
      try {
        if (needsStatus) {
          const fresh = await getProcessStatus(requestController.signal);
          if (stopped || requestGeneration !== generation) return;
          setStatus(fresh);
          statusIdentityUnavailable = fresh.run_id_state === 'unavailable';
          if (statusIdentityUnavailable) setStale(true);
          needsStatus = false;
        }
        const page = await getCurrentLogPage({ source, level, ...(nodePath ? { node_path: nodePath } : {}), ...(correlationId ? { correlation_id: correlationId } : {}), cursor: cursor.current, limit: 200 }, requestController.signal);
        if (stopped || requestGeneration !== generation) return;
        if (isCurrentLogPage(page)) {
          const anchorKey = `${page.scope.kind}:${page.scope.run_id}`;
          if (observedAnchor.current?.key !== anchorKey) setAnchorAnnouncement(false);
          if (observedAnchor.current?.key === anchorKey && observedAnchor.current.state === 'pending' && page.anchor.state === 'found' && announcedAnchor.current !== anchorKey) {
            announcedAnchor.current = anchorKey;
            setAnchorAnnouncement(true);
          }
          observedAnchor.current = { key: anchorKey, state: page.anchor.state };
          const oldScope = currentScope.current;
          cursor.current = page.next_cursor;
          currentScope.current = `${page.scope.kind}:${page.scope.run_id}`;
          setCurrent((state) => acceptCurrentLogPage(state, page));
          setLegacyLines([]);
          setCompatibility(false);
          setStale(statusIdentityUnavailable);
          if (oldScope && oldScope !== currentScope.current) {
            const fresh = await getProcessStatus(requestController.signal);
            if (stopped || requestGeneration !== generation) return;
            setStatus(fresh);
            statusIdentityUnavailable = fresh.run_id_state === 'unavailable';
            setStale(statusIdentityUnavailable);
          }
        } else {
          observedAnchor.current = null;
          setAnchorAnnouncement(false);
          cursor.current = page.next_cursor ?? cursor.current;
          currentScope.current = null;
          setCurrent(emptyCurrentLogState);
          setLegacyLines((rows) => appendLogPage(rows, page));
          setCompatibility(true);
          setStale(statusIdentityUnavailable);
        }
        retryAt = 0;
        setError('');
        schedule(POLL_MS);
      } catch (reason) {
        if (!stopped && requestGeneration === generation) {
          needsStatus = true;
          retryAt = Date.now() + RETRY_MS;
          if (reason instanceof IsosApiError && (reason.code === 'log_cursor_filter_mismatch' || reason.code === 'log_cursor_invalid')) {
            cursor.current = undefined;
            setCurrent(resetCurrentLogDetails);
          }
          if (reason instanceof IsosApiError && reason.code === 'log_scope_unavailable') {
            // Keep the old rows for inspection, but withdraw their current claim.
            setStale(true);
          }
          const message = reason instanceof IsosApiError && reason.code === 'log_scope_unavailable'
            ? (lang === 'cn' ? '当前运行日志范围不可用；请查看 IsOS 状态。' : 'Current log scope unavailable; check IsOS status.')
            : reason instanceof Error ? reason.message : String(reason);
          setError(message);
          schedule(RETRY_MS);
        }
      } finally {
        if (requestGeneration === generation) {
          pending = false;
          controller = null;
        }
      }
    };
    const onVisible = (): void => {
      if (document.visibilityState === 'visible' && Date.now() >= retryAt) {
        if (timer !== undefined) window.clearTimeout(timer);
        void poll();
      }
    };
    const onScopeRecheck = (): void => {
      generation += 1;
      controller?.abort();
      controller = null;
      pending = false;
      setStale(true);
      needsStatus = true;
      retryAt = 0;
      if (timer !== undefined) window.clearTimeout(timer);
      void poll();
    };
    void poll();
    document.addEventListener('visibilitychange', onVisible);
    window.addEventListener('isosui:log-scope-recheck', onScopeRecheck);
    return () => { stopped = true; generation += 1; controller?.abort(); if (timer !== undefined) window.clearTimeout(timer); document.removeEventListener('visibilitychange', onVisible); window.removeEventListener('isosui:log-scope-recheck', onScopeRecheck); };
  }, [source, level, nodePath, correlationId, following, visible, lang]);
  const currentScope = useRef<string | null>(null);

  const copy = lang === 'cn'
    ? { title: 'IsOS 与浏览器日志', source: '来源', all: '所有来源', level: '最低级别', follow: '实时跟踪', close: '关闭', empty: '没有匹配的日志', pendingRun: '等待本次运行的首条就绪或故障事件', pendingManager: '等待管理器会话的首条软件包或 Apply 故障', unavailable: '首条事件不可用；已检测到日志缺口', identityUnavailable: 'IsOS 无法确认当前运行身份；旧日志已标为过期。', anchor: '本次首条事件', anchorAvailable: '本次首条事件已可查看', details: '后续日志', browser: '浏览器本地日志', status: '实时状态', unknown: '未知', legacy: '旧版 IsOS 仅提供未固定首条事件的日志', stale: '上次范围（已过期，非当前运行）', node: '节点路径', correlation: '关联 ID', invalidNode: '节点路径格式无效', invalidCorrelation: '关联 ID 格式无效', jump: '跳转到首条事件', copyRun: '复制运行 ID', copyCorrelation: '复制关联 ID', boundary: '新的运行范围', gap: '日志缺口', acknowledge: '已知悉缺口', trace: '跟踪及以上', debug: '调试及以上', info: '信息及以上', warn: '警告及以上', error: '仅错误' }
    : { title: 'IsOS and browser logs', source: 'Source', all: 'All sources', level: 'Minimum level', follow: 'Follow', close: 'Close', empty: 'No matching entries', pendingRun: 'Waiting for the first ready or failure event in this run', pendingManager: 'Waiting for the first package or Apply failure in this manager session', unavailable: 'First event unavailable; a log gap was detected', identityUnavailable: 'IsOS cannot confirm the current run identity; previous logs are stale.', anchor: 'First event in this scope', anchorAvailable: 'First event is available in this scope', details: 'Subsequent logs', browser: 'Local browser logs', status: 'Live status', unknown: 'unknown', legacy: 'Older IsOS provides logs without a pinned first event', stale: 'Previous scope (stale, not current)', node: 'Node path', correlation: 'Correlation ID', invalidNode: 'Invalid node path format', invalidCorrelation: 'Invalid correlation ID format', jump: 'Jump to first event', copyRun: 'Copy run ID', copyCorrelation: 'Copy correlation ID', boundary: 'New run scope', gap: 'Log gap', acknowledge: 'Acknowledge gap', trace: 'Trace and above', debug: 'Debug and above', info: 'Info and above', warn: 'Warnings and errors', error: 'Errors only' };
  const invalidNode = nodePath !== '' && !validNodeFilter(nodePath);
  const invalidCorrelation = correlationId !== '' && !validCorrelationFilter(correlationId);
  const browserVisible = visibleLogLines(browserLines, level);
  const serverLines = compatibility ? legacyLines : current.lines;
  const pendingText = current.scope?.kind === 'manager_session' ? copy.pendingManager : copy.pendingRun;
  const diagText = current.diagnostics.filter((item) => item.code !== 'log_run_changed').map((item) => `${item.code}${item.count === null ? '' : ` (${item.count})`}`).join(', ');
  const copySafeId = (value: string): void => { if (navigator.clipboard) void navigator.clipboard.writeText(value); };
  return <section className="node-graph-logs" aria-label={copy.title}>
    <header>
      <strong>{copy.title}</strong>
      <label>{copy.source}<select value={source} onChange={(event) => setSource(event.target.value)}>{[...new Set([source, ...SOURCES, ...sources])].map((name) => <option key={name} value={name}>{name === 'all' ? copy.all : name === 'browser' ? copy.browser : name}</option>)}</select></label>
      <label>{copy.level}<select value={level} onChange={(event) => setLevel(event.target.value as LogLevel)}>{LOG_LEVELS.map((name) => <option key={name} value={name}>{copy[name]}</option>)}</select></label>
      <label>{copy.node}<input value={nodePath} onChange={(event) => setNodePath(event.target.value)} aria-invalid={invalidNode} /></label>
      <label>{copy.correlation}<input value={correlationId} onChange={(event) => setCorrelationId(event.target.value)} aria-invalid={invalidCorrelation} /></label>
      <label className="node-graph-logs__follow"><input type="checkbox" checked={following} onChange={(event) => setFollowing(event.target.checked)} />{copy.follow}</label>
      <button type="button" onClick={() => anchorRef.current?.scrollIntoView({ block: 'nearest' })} disabled={source === 'browser' || !current.anchor?.event}>{copy.jump}</button>
      <button type="button" onClick={onClose} aria-label={`${copy.close} ${copy.title}`}>×</button>
    </header>
    <div className="node-graph-logs__context">
      <span>{copy.status}: {status?.state ?? copy.unknown}</span>
      {source !== 'browser' && current.scope && <span>{stale ? copy.stale : current.scope.kind}: {current.scope.run_id} <button type="button" onClick={() => copySafeId(current.scope!.run_id)}>{copy.copyRun}</button></span>}
      {status?.run_id_state === 'unavailable' && <span role="alert">{copy.identityUnavailable}</span>}
    </div>
    {invalidNode && <p role="alert">{copy.invalidNode}</p>}
    {invalidCorrelation && <p role="alert">{copy.invalidCorrelation}</p>}
    {error && <p role="alert">{error}</p>}
    {compatibility && <p role="status">{copy.legacy}</p>}
    {current.boundary && !compatibility && <p aria-live="polite">{copy.boundary}: {current.boundary.previous} → {current.boundary.current}</p>}
    {diagText && !compatibility && <p role="alert">{copy.gap}: {diagText} <button type="button" onClick={() => setCurrent(acknowledgeCurrentLogDiagnostics)}>{copy.acknowledge}</button></p>}
    {anchorAnnouncement && !compatibility && <p role="status" aria-live="polite">{copy.anchorAvailable}</p>}
    {source !== 'browser' && !compatibility && current.anchor?.state === 'found' && current.anchor.event && <div className="node-graph-logs__anchor"><strong>{copy.anchor}</strong>{current.anchor.event.correlation_id && <button type="button" onClick={() => copySafeId(current.anchor!.event!.correlation_id!)}>{copy.copyCorrelation}</button>}<pre ref={anchorRef}>{displayLine(current.anchor.event)}</pre></div>}
    {source !== 'browser' && !compatibility && current.anchor?.state === 'pending' && <p role="status">{pendingText}</p>}
    {source !== 'browser' && !compatibility && current.anchor?.state === 'unavailable' && <p role="alert">{copy.unavailable}</p>}
    {source !== 'browser' && <div className="node-graph-logs__stream"><strong>{copy.details}</strong><pre role="log" aria-live="off">{serverLines.length ? serverLines.map(displayLine).join('\n') : current.anchor?.state === 'pending' ? pendingText : copy.empty}</pre></div>}
    {(source === 'all' || source === 'browser') && <div className="node-graph-logs__stream node-graph-logs__browser"><strong>{copy.browser}</strong><pre role="log" aria-live="off">{browserVisible.length ? browserVisible.map(displayLine).join('\n') : copy.empty}</pre></div>}
  </section>;
}
