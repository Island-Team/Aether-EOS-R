import { afterEach, expect, it, vi } from 'vitest';
import { getBrowserLogs } from './browser-logs';
import { dismissUiMessage, getUiMessages, publishUiMessage } from './ui-messages';

afterEach(() => {
  for (const message of getUiMessages()) dismissUiMessage(message.id);
  vi.useRealTimers();
});

it('keeps a notification briefly while retaining a single browser log entry', () => {
  vi.useFakeTimers();
  const before = getBrowserLogs().length;
  publishUiMessage('  Apply completed  ');
  expect(getUiMessages().map((message) => message.text)).toEqual(['Apply completed']);
  expect(getBrowserLogs().slice(before).map((line) => line.message)).toEqual(['Apply completed']);
  vi.advanceTimersByTime(5000);
  expect(getUiMessages()).toEqual([]);
  expect(getBrowserLogs().slice(before).map((line) => line.message)).toEqual(['Apply completed']);
});

it('dismisses a notification without logging again', () => {
  vi.useFakeTimers();
  const before = getBrowserLogs().length;
  const id = publishUiMessage('Stop failed', 'error');
  expect(id).not.toBeNull();
  dismissUiMessage(id!);
  expect(getUiMessages()).toEqual([]);
  expect(getBrowserLogs().slice(before).map((line) => [line.level, line.message])).toEqual([['error', 'Stop failed']]);
});

it('coalesces the same visible notice from multiple workbench panels', () => {
  vi.useFakeTimers();
  const before = getBrowserLogs().length;
  const first = publishUiMessage('IsOS unavailable', 'error');
  expect(publishUiMessage('IsOS unavailable', 'error')).toBe(first);
  expect(getUiMessages()).toHaveLength(1);
  expect(getBrowserLogs().slice(before).map((line) => line.message)).toEqual(['IsOS unavailable']);
});
