import { browserLog } from './browser-logs';

export type UiMessageLevel = 'info' | 'warn' | 'error';
export interface UiMessage { id: number; text: string; level: UiMessageLevel }

const DISPLAY_MS = 5000;
let nextId = 0;
let messages: UiMessage[] = [];
const listeners = new Set<() => void>();
const timers = new Map<number, ReturnType<typeof setTimeout>>();

function emit(): void { listeners.forEach((listener) => listener()); }
export function getUiMessages(): UiMessage[] { return messages; }
export function subscribeUiMessages(listener: () => void): () => void {
  listeners.add(listener);
  return () => { listeners.delete(listener); };
}

export function dismissUiMessage(id: number): void {
  const timer = timers.get(id);
  if (timer) clearTimeout(timer);
  timers.delete(id);
  messages = messages.filter((message) => message.id !== id);
  emit();
}

/** Show a brief notice and retain its text in the browser section of Logs. */
export function publishUiMessage(text: string, level: UiMessageLevel = 'info'): number | null {
  const trimmed = text.trim().slice(0, 2000);
  if (!trimmed) return null;
  const existing = messages.find((message) => message.text === trimmed && message.level === level);
  if (existing) return existing.id;
  const id = ++nextId;
  browserLog(level, trimmed);
  if (messages.length === 5) dismissUiMessage(messages[0].id);
  messages = [...messages, { id, text: trimmed, level }].slice(-5);
  emit();
  timers.set(id, setTimeout(() => dismissUiMessage(id), DISPLAY_MS));
  return id;
}
