#!/usr/bin/env python3
"""Bounded loopback OpenAI-compatible model for the M4 live mission replay.

The response is deliberately deterministic. No prompt or Authorization header is
written to the request log. EGIA accepts JSON even when it asks for SSE.
"""

from __future__ import annotations

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import signal
import threading


PLAN = 'plan add move "Robot has moved."'
MOVE = (
    'step move\n'
    'base.velocity@robot-a/drive linear_x_m_s=0.1 '
    'linear_y_m_s=0 angular_z_rad_s=0 valid_for_ms=100'
)
FINAL = 'final movement complete'
PROPOSER_EMPTY = '{"missions":[]}'
INVALID_Q01_REPLY = 'INVALID_Q01_REPLY'
MAX_BODY = 1_048_576


class ModelStub:
    """Start with ``start()``, pass ``endpoint`` into models.toml, then close."""

    def __init__(self, *, max_requests: int = 32):
        self.max_requests = max_requests
        self.requests: list[dict[str, object]] = []
        self._controller_calls = 0
        self._mode = 'normal'
        self._transitions: list[dict[str, object]] = [{'after_request_index': 0, 'mode': 'normal'}]
        self._lock = threading.Lock()
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self) -> None:
                if self.path != '/v1/chat/completions':
                    self.send_error(404)
                    return
                try:
                    size = int(self.headers.get('Content-Length', '-1'))
                    if not 0 < size <= MAX_BODY:
                        raise ValueError('invalid request size')
                    payload = json.loads(self.rfile.read(size))
                    if not isinstance(payload, dict):
                        raise ValueError('request is not an object')
                    messages = payload['messages']
                    if not isinstance(messages, list):
                        raise ValueError('messages is not a list')
                    system = ' '.join(str(msg.get('content', '')) for msg in messages
                                      if isinstance(msg, dict) and msg.get('role') == 'system').lower()
                    sentinel = 'fast safety sentinel' in system
                    proposer = 'you propose grounded missions' in system
                except (KeyError, ValueError, TypeError, json.JSONDecodeError):
                    self.send_error(400)
                    return

                with owner._lock:
                    if len(owner.requests) >= owner.max_requests:
                        self.send_error(429)
                        return
                    if sentinel:
                        lane, reply = 'sentinel', 'continue'
                    elif proposer:
                        lane, reply = 'proposer', PROPOSER_EMPTY
                    else:
                        owner._controller_calls += 1
                        lane = 'controller'
                        reply = (INVALID_Q01_REPLY if owner._mode == 'invalid' else
                                 PLAN if owner._controller_calls == 1 else
                                 MOVE if owner._controller_calls == 2 else FINAL)
                    owner.requests.append({
                        'index': len(owner.requests) + 1,
                        'lane': lane,
                        'reply': 'move' if reply == MOVE else reply,
                        'reply_lines': len(reply.splitlines()),
                        'model': str(payload.get('model', ''))[:80],
                        'stream_requested': payload.get('stream') is True,
                        'mode': owner._mode,
                    })

                body = json.dumps({
                    'model': str(payload.get('model', 'm4-stub'))[:80],
                    'choices': [{'message': {'role': 'assistant', 'content': reply},
                                 'finish_reason': 'stop'}],
                    'usage': {'prompt_tokens': 10, 'completion_tokens': 10,
                              'total_tokens': 20},
                }).encode()
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, _format: str, *_args: object) -> None:
                return

        self._server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self._server.daemon_threads = True
        self._thread: threading.Thread | None = None

    @property
    def endpoint(self) -> str:
        return f'http://127.0.0.1:{self._server.server_port}/v1'

    def start(self) -> ModelStub:
        if self._thread is not None:
            raise RuntimeError('model stub already started')
        self._thread = threading.Thread(target=self._server.serve_forever,
                                        name='m4-model-stub', daemon=True)
        self._thread.start()
        return self

    def snapshot(self) -> list[dict[str, object]]:
        with self._lock:
            return [dict(item) for item in self.requests]

    def set_mode(self, mode: str) -> None:
        """Switch controller replies without touching in-flight response bytes."""
        if mode not in {'normal', 'invalid'}:
            raise ValueError('unknown model stub mode')
        with self._lock:
            self._mode = mode
            if mode == 'normal':
                self._controller_calls = 0
            self._transitions.append({'after_request_index': len(self.requests), 'mode': mode})

    def snapshot_transitions(self) -> list[dict[str, object]]:
        with self._lock:
            return [dict(item) for item in self._transitions]

    def close(self) -> None:
        self._server.shutdown()
        self._server.server_close()
        if self._thread is not None:
            self._thread.join(timeout=5)

    def __enter__(self) -> ModelStub:
        return self.start()

    def __exit__(self, *_exc: object) -> None:
        self.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port-file', required=True, type=Path)
    parser.add_argument('--log-file', required=True, type=Path)
    parser.add_argument('--max-requests', type=int, default=32)
    args = parser.parse_args()
    if args.max_requests < 2:
        parser.error('--max-requests must be at least 2')
    stop = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    with ModelStub(max_requests=args.max_requests) as stub:
        args.port_file.write_text(stub.endpoint + '\n')
        while not stop.wait(0.2):
            pass
        args.log_file.write_text(json.dumps(stub.snapshot(), indent=2) + '\n')


if __name__ == '__main__':
    main()
