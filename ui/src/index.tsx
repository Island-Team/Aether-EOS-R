import React, { lazy, Suspense, useCallback, useEffect, useRef, useState } from 'react';
import { createRoot } from 'react-dom/client';
import { IsosHome } from '@/components/isos/IsosHome';
import { IsosLogsPanel } from '@/components/isos/IsosLogsPanel';
import { UiMessages } from '@/components/isos/UiMessages';
import { WorkspacePanel } from '@/components/isos/WorkspacePanel';
import { clampPanel, defaultWorkspaceLayout, layoutKey, readWorkspaceLayout, SURFACES, type PanelLayout, type Surface, type WorkspaceLayout } from '@/components/isos/workspace-layout';
import { getProcessStatus, type ProcessStatus } from '@/services/isos/node-graph';
import { stopDeploymentConverged } from '@/components/isos/deployment-control';
import { stopFailureFeedback, stopReplyFeedback } from '@/components/isos/stop-feedback';
import { beginRunStopFence, observeRunStopFence, settleRunStopFence, type RunStopFence } from '@/components/isos/run-stop-fence';
import '@/components/isos/workbench.scss';
import type { Language } from '@/types';
import { installBrowserErrorLogging } from '@/services/isos/browser-logs';
import { publishUiMessage } from '@/services/isos/ui-messages';
import '@/styles/fonts.css';
import '@/styles/global.scss';

const NodeGraphManager = lazy(async () => ({ default: (await import('@/components/isos/NodeGraphManager')).NodeGraphManager }));
const EosWorkbench = lazy(async () => ({ default: (await import('@/components/isos/EosWorkbench')).EosWorkbench }));
const EgiaWorkbench = lazy(async () => ({ default: (await import('@/components/isos/EgiaWorkbench')).EgiaWorkbench }));

installBrowserErrorLogging();

const theme = localStorage.getItem('island-theme');
const systemTheme = window.matchMedia('(prefers-color-scheme: dark)');
const rootElement = document.documentElement;
rootElement.dataset.theme = theme === 'light' || theme === 'dark' ? theme : systemTheme.matches ? 'dark' : 'light';
const accent = localStorage.getItem('island-accent');
rootElement.dataset.accent = ['teal', 'pink', 'green', 'blue', 'violet', 'amber'].includes(accent || '') ? accent! : 'teal';
if (theme !== 'light' && theme !== 'dark') {
  systemTheme.addEventListener('change', (event) => { rootElement.dataset.theme = event.matches ? 'dark' : 'light'; });
}

function detectLanguage(): Language {
  if (window.location.pathname.startsWith('/cn/')) return 'cn';
  if (window.location.pathname.startsWith('/en/')) return 'en';
  return navigator.language.toLowerCase().startsWith('zh') ? 'cn' : 'en';
}

function applyDocumentLanguage(lang: Language): void {
  rootElement.lang = lang === 'cn' ? 'zh-CN' : 'en';
  document.title = lang === 'cn' ? 'IsOS 配置' : 'IsOS Configuration';
}

const initialLanguage = detectLanguage();
applyDocumentLanguage(initialLanguage);

function App(): React.ReactElement {
  const [lang, setLang] = useState<Language>(initialLanguage);
  const [graphDirty, setGraphDirty] = useState(false);
  const [robotId, setRobotId] = useState('unknown');
  const [viewport, setViewport] = useState(() => ({ width: window.innerWidth, height: window.innerHeight }));
  const [layout, setLayout] = useState<WorkspaceLayout>(() => { const initial = defaultWorkspaceLayout(); if (new URLSearchParams(window.location.search).get('logs') === '1') initial.logs.open = true; return initial; });
  const [layoutRobot, setLayoutRobot] = useState('unknown');
  const [status, setStatus] = useState<ProcessStatus | null>(null);
  const [connectionError, setConnectionError] = useState('');
  useEffect(() => { if (connectionError) publishUiMessage(connectionError, 'error'); }, [connectionError]);
  const [stopFeedback, setStopFeedback] = useState('');
  const [stopAlert, setStopAlert] = useState(false);
  const statusRef = useRef<ProcessStatus | null>(null);
  const stopFence = useRef<RunStopFence | null>(null);
  const [stopFenceEpoch, setStopFenceEpoch] = useState(0);
  const onStatus = useCallback((next: ProcessStatus | null): void => { statusRef.current = next; setStatus(next); setConnectionError(next ? '' : lang === 'cn' ? 'IsOS 连接失败' : 'IsOS connection unavailable'); }, [lang]);
  const onConnectionError = useCallback((message: string): void => setConnectionError(message), []);
  const onRobot = useCallback((id: string): void => { setRobotId(id || 'unknown'); }, []);
  const onStopIntent = useCallback((runId: string | null): void => { stopFence.current = beginRunStopFence(runId, statusRef.current, stopFence.current); setStopFenceEpoch((value) => value + 1); setStopFeedback(lang === 'cn' ? '停止请求已发送；等待 IsOS 确认。' : 'Stop requested; waiting for IsOS confirmation.'); setStopAlert(false); }, [lang]);
  const onStopOutcome = useCallback((feedback: { text: string; alert: boolean }, terminalRun: string | null = null): void => { if (stopFence.current) { stopFence.current = settleRunStopFence(stopFence.current, terminalRun, !feedback.alert); setStopFenceEpoch((value) => value + 1); } if (stopFence.current?.phase === 'pending') { setStopFeedback(lang === 'cn' ? '仍有停止请求待确认。' : 'A Stop request is still pending.'); setStopAlert(false); } else { setStopFeedback(feedback.alert ? feedback.text : ''); setStopAlert(feedback.alert); } }, [lang]);
  const isRunStopFenced = useCallback((_runId: string | null): boolean => stopFence.current !== null, []);
  const updatePanel = useCallback((name: Surface, patch: Partial<PanelLayout>): void => { setLayout((current) => ({ ...current, [name]: clampPanel({ ...current[name], ...patch, z: patch.z === undefined ? current[name].z : Math.min(90, Math.max(...SURFACES.map((item) => current[item].z)) + 1) }, { width: window.innerWidth, height: window.innerHeight }) })); }, []);
  const openPanel = useCallback((name: Surface): void => updatePanel(name, { open: true, collapsed: false }), [updatePanel]);
  useEffect(() => { const next = readWorkspaceLayout(robotId, { width: window.innerWidth, height: window.innerHeight }); if (new URLSearchParams(window.location.search).get('logs') === '1') next.logs.open = true; setLayout(next); setLayoutRobot(robotId); }, [robotId]);
  useEffect(() => { if (robotId !== 'unknown' && robotId === layoutRobot) localStorage.setItem(layoutKey(robotId), JSON.stringify({ version: 1, panels: layout })); }, [layout, robotId, layoutRobot]);
  useEffect(() => { const resize = (): void => { const next = { width: window.innerWidth, height: window.innerHeight }; setViewport(next); setLayout((current) => Object.fromEntries(SURFACES.map((name) => [name, clampPanel(current[name], next)])) as WorkspaceLayout); }; window.addEventListener('resize', resize); return () => window.removeEventListener('resize', resize); }, []);
  useEffect(() => {
    const fence = stopFence.current;
    if (!fence || fence.phase !== 'settled') return;
    let disposed = false;
    let pending = false;
    const poll = async (): Promise<void> => {
      if (disposed || pending || document.visibilityState !== 'visible') return;
      pending = true;
      try {
        const fresh = await getProcessStatus();
        if (disposed || stopFence.current !== fence) return;
        onStatus(fresh);
        const next = observeRunStopFence(fence, fresh);
        stopFence.current = next;
        if (!next) { setStopFeedback(''); setStopAlert(false); setStopFenceEpoch((value) => value + 1); }
        else if (next !== fence) { setStopFenceEpoch((value) => value + 1); }
      } catch { /* An unavailable owner identity keeps the Stop fence. */ }
      finally { pending = false; }
    };
    void poll();
    const timer = window.setInterval(() => { void poll(); }, 3000);
    const onVisible = (): void => { if (document.visibilityState === 'visible') void poll(); };
    document.addEventListener('visibilitychange', onVisible);
    return () => { disposed = true; window.clearInterval(timer); document.removeEventListener('visibilitychange', onVisible); };
  }, [stopFenceEpoch, onStatus]);
  const stopDeployment = async (): Promise<void> => { const before = status; onStopIntent(before?.run_id || null); try { const reply = await stopDeploymentConverged(); const after = await getProcessStatus().catch(() => null); const feedback = stopReplyFeedback(reply, before, lang, after); publishUiMessage(feedback.text, feedback.alert ? 'error' : 'info'); onStopOutcome(feedback, reply.terminal?.run_id || null); onStatus(after); } catch (error) { const feedback = stopFailureFeedback(error, lang); publishUiMessage(feedback.text, 'error'); onStopOutcome(feedback); } };
  type Page = 'home' | 'graph' | 'eos' | 'egia';
  const [page, setPage] = useState<Page>(() => {
    const segment = new URLSearchParams(window.location.search).get('page');
    return segment === 'home' || segment === 'eos' || segment === 'egia' ? segment : 'graph';
  });
  const openPage = (next: Page): void => {
    window.history.pushState({ page: next }, '', `/${lang}/isos${next === 'graph' ? '' : `?page=${next}`}`);
    setPage(next);
  };
  const changeLanguage = (value: Language): void => {
    window.history.replaceState(window.history.state, '', `/${value}/isos${page === 'graph' ? '' : `?page=${page}`}`);
    applyDocumentLanguage(value);
    setLang(value);
  };
  React.useEffect(() => {
    const onPop = (): void => { const segment = new URLSearchParams(window.location.search).get('page'); setPage(segment === 'home' || segment === 'eos' || segment === 'egia' ? segment : 'graph'); };
    window.addEventListener('popstate', onPop);
    return () => window.removeEventListener('popstate', onPop);
  }, []);
  const labels = lang === 'cn' ? { home: '首页', graph: '节点图', eos: 'EOS', egia: 'EI', language: '语言', stop: '停止部署', logs: '日志', monitor: '监控', vision: '视觉', control: '控制', connection: '连接', revision: '运行中修订', stale: '保存修订与运行中修订不同', unknownRun: '运行身份未知', draft: '有未应用的节点图修改' } : { home: 'Home', graph: 'Graph', eos: 'EOS', egia: 'EI', language: 'Language', stop: 'Stop deployment', logs: 'Logs', monitor: 'Monitor', vision: 'Vision', control: 'Control', connection: 'Connection', revision: 'Active revision', stale: 'Saved and active revisions differ', unknownRun: 'Run identity unknown', draft: 'Unapplied graph edits' };
  return <div className="isos-workbench"><UiMessages lang={lang} /><nav className="isos-workbench__nav" aria-label="IsOS"><a className="isos-workbench__brand" href="https://www.os-island.com" aria-label="Island website">ISOS<span> / Aether</span></a><div role="group" aria-label="Pages">{(['graph', 'eos', 'egia'] as Page[]).map((name) => <button type="button" key={name} className={page === name ? 'active' : ''} aria-current={page === name ? 'page' : undefined} onClick={() => openPage(name)}>{labels[name]}</button>)}</div><button type="button" className="isos-workbench__stop" onClick={() => void stopDeployment()}>{labels.stop}</button><button type="button" onClick={() => changeLanguage(lang === 'en' ? 'cn' : 'en')} aria-label={labels.language}>{lang === 'en' ? '中文' : 'EN'}</button></nav>
    {(connectionError || status?.saved_revision && status.active_revision && status.saved_revision !== status.active_revision || status?.run_id_state === 'unavailable' || graphDirty || stopFeedback) && <div className="isos-workbench__safety" role="region" aria-label={labels.connection}>{connectionError && <span role="alert">{connectionError}</span>}{status?.saved_revision && status.active_revision && status.saved_revision !== status.active_revision && <span role="alert">{labels.stale}</span>}{status?.run_id_state === 'unavailable' && <span role="alert">{labels.unknownRun}</span>}{graphDirty && <span role="status">{labels.draft}</span>}{stopFeedback && <span role={stopAlert ? 'alert' : 'status'}>{stopFeedback}</span>}</div>}
    <div className="isos-workbench__panel-menu" role="toolbar" aria-label={lang === 'cn' ? '工作面板' : 'Workspace panels'}>{SURFACES.map((name) => <button type="button" key={name} aria-pressed={layout[name].open} onClick={() => updatePanel(name, { open: !layout[name].open })}>{labels[name]}</button>)}</div>
    {page === 'home' && <IsosHome lang={lang} onOpenGraph={() => openPage('graph')} onOpenLogs={() => openPanel('logs')} onStopIntent={onStopIntent} onStopOutcome={onStopOutcome} graphDirty={graphDirty} />}
    <div className="isos-workbench__graph-view" hidden={page !== 'graph'}><Suspense fallback={<p role="status">{lang === 'cn' ? '正在加载节点图…' : 'Loading node graph…'}</p>}><NodeGraphManager lang={lang} visible={page === 'graph'} onDirtyChange={setGraphDirty} onOpenPanel={openPanel} onStopIntent={onStopIntent} onStopOutcome={onStopOutcome} /></Suspense></div>
    <div className="isos-workbench__panels"><Suspense fallback={<p role="status">{lang === 'cn' ? '正在加载 EOS…' : 'Loading EOS…'}</p>}><EosWorkbench lang={lang} onOpenGraph={() => openPage('graph')} onOpenLogs={() => openPanel('logs')} graphDirty={graphDirty} layout={layout} viewport={viewport} onPanelChange={updatePanel} onRobot={onRobot} onStatus={onStatus} onConnectionError={onConnectionError} isRunStopFenced={isRunStopFenced} onRunStopIntent={onStopIntent} onStopOutcome={onStopOutcome} /></Suspense>{layout.logs.open && <WorkspacePanel title={labels.logs} lang={lang} layout={layout.logs} viewport={viewport} onChange={(patch) => updatePanel('logs', patch)}><IsosLogsPanel lang={lang} visible={!layout.logs.collapsed} onClose={() => updatePanel('logs', { open: false })} /></WorkspacePanel>}</div>
    {page === 'egia' && <Suspense fallback={<p role="status">{lang === 'cn' ? '正在加载 EI…' : 'Loading EI…'}</p>}><EgiaWorkbench lang={lang} onOpenGraph={() => openPage('graph')} /></Suspense>}
  </div>;
}

const root = document.getElementById('root');
if (!root) throw new Error('Missing IsOS root element.');
createRoot(root).render(<App />);
