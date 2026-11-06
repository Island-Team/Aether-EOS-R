import type { LogLine } from './node-graph';

const LIMIT = 1000;
const lines: LogLine[] = [];
const listeners = new Set<() => void>();

export function browserLog(level: LogLine['level'], message: string): void {
  lines.push({ timestamp: new Date().toISOString(), level, source: 'browser', message });
  if (lines.length > LIMIT) lines.splice(0, lines.length - LIMIT);
  listeners.forEach((listener) => listener());
}

export function getBrowserLogs(): LogLine[] { return [...lines]; }
export function subscribeBrowserLogs(listener: () => void): () => void {
  listeners.add(listener);
  return () => { listeners.delete(listener); };
}

function printable(value: unknown): string {
  if (value instanceof Error) return value.stack || value.message;
  if (typeof value === 'string') return value;
  try { return JSON.stringify(value); } catch { return String(value); }
}

export function installBrowserErrorLogging(): void {
  for (const level of ['log', 'info', 'debug', 'warn', 'error'] as const) {
    const original = console[level].bind(console);
    console[level] = (...args: unknown[]): void => {
      browserLog(level === 'log' ? 'info' : level, args.map(printable).join(' ').slice(0, 2000));
      original(...args);
    };
  }
  window.addEventListener('error', (event) => browserLog('error', `${event.message} (${event.filename}:${event.lineno}:${event.colno})`));
  window.addEventListener('unhandledrejection', (event) => browserLog('error', `Unhandled promise rejection: ${String(event.reason)}`));
}
