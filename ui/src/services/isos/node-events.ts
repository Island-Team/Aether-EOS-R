import { request } from './api-client';

export type KeyState = 'pressed' | 'refresh' | 'released';

/** The graph, not the browser event, owns the binding and movement parameters. */
export interface KeyStateEvent {
  schema_version: '0.1.0';
  source_node: string;
  output: 'state';
  data_type: 'key_state';
  run_id: string;
  active_revision: string;
  payload: { state: KeyState; activation_id: string; sequence: number };
}

export function publishKeyState(event: KeyStateEvent): Promise<unknown> {
  return request('/nodes/events', 'POST', event);
}
