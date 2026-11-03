"""Local HTTP proxy retaining the responses actually delivered to Firefox."""

import base64
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import select
import socket
from threading import Lock, Thread
from urllib.parse import urlsplit


HOP_HEADERS = {'connection', 'proxy-connection', 'keep-alive',
               'transfer-encoding', 'content-length'}
CORS_HEADERS = {'access-control-allow-origin', 'access-control-allow-methods',
                'access-control-allow-headers', 'access-control-allow-private-network',
                'access-control-max-age', 'vary'}


class RecordingProxy:
    def __init__(self, rewrite=None, *, hosted_origin=None, isos_port=None,
                 eos_port=None):
        if hosted_origin is not None:
            parsed_origin = urlsplit(hosted_origin)
            assert (parsed_origin.scheme == 'http' and parsed_origin.hostname == '127.0.0.1'
                    and parsed_origin.port and parsed_origin.path == ''
                    and not parsed_origin.query and not parsed_origin.fragment)
            assert isinstance(isos_port, int) and 0 < isos_port < 65536
            assert isinstance(eos_port, int) and 0 < eos_port < 65536
        else:
            assert isos_port is None and eos_port is None
        self.lock = Lock()
        self.rows = []
        self.cors_rows = []
        self.window = None
        self.allowed_urls = set()
        self.rewrite = rewrite
        self.hosted_origin = hosted_origin
        self.isos_port = isos_port
        self.eos_port = eos_port

        proxy = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'

            def log_message(self, format, *args):
                pass

            def do_GET(self):
                self.forward()

            def do_POST(self):
                self.forward()

            def do_OPTIONS(self):
                self.forward()

            def do_CONNECT(self):
                target = self.path.split(':', 1)
                host = target[0]
                port = int(target[1]) if len(target) == 2 else 443
                upstream = socket.create_connection((host, port), timeout=15)
                self.send_response(200, 'Connection Established')
                self.end_headers()
                self.tunnel(upstream)

            def tunnel(self, upstream):
                self.connection.settimeout(None)
                upstream.settimeout(None)
                try:
                    while True:
                        readable, _, _ = select.select([self.connection, upstream], [], [], 60)
                        if not readable:
                            break
                        for source in readable:
                            data = source.recv(65536)
                            if not data:
                                return
                            (upstream if source is self.connection else self.connection).sendall(data)
                finally:
                    upstream.close()
                    self.close_connection = True

            def forward_upgrade(self, parsed):
                upstream = socket.create_connection((parsed.hostname, parsed.port or 80), timeout=15)
                request = f'{self.command} {parsed.path or "/"}{"?" + parsed.query if parsed.query else ""} HTTP/1.1\r\n'
                headers = ''.join(f'{key}: {value}\r\n' for key, value in self.headers.items()
                                  if key.lower() != 'proxy-connection')
                upstream.sendall((request + headers + '\r\n').encode('latin-1'))
                response = bytearray()
                while b'\r\n\r\n' not in response:
                    chunk = upstream.recv(4096)
                    assert chunk and len(response) < 65536, 'WebSocket handshake failed'
                    response.extend(chunk)
                self.connection.sendall(response)
                self.tunnel(upstream)

            def forward(self):
                parsed = urlsplit(self.path)
                assert parsed.scheme in ('http', 'ws') and parsed.hostname, self.path
                if self.headers.get('Upgrade', '').lower() == 'websocket':
                    self.forward_upgrade(parsed)
                    return
                path = (parsed.path or '/') + (('?' + parsed.query) if parsed.query else '')
                body = self.rfile.read(int(self.headers.get('Content-Length', '0')))
                connection = http.client.HTTPConnection(parsed.hostname,
                    parsed.port or 80, timeout=30)
                headers = {key:value for key,value in self.headers.items()
                           if key.lower() not in HOP_HEADERS and key.lower() != 'host'}
                headers['Accept-Encoding'] = 'identity'
                original_origin = self.headers.get('Origin')
                selected_api = (parsed.scheme == 'http' and parsed.hostname == '127.0.0.1'
                    and ((parsed.port == proxy.isos_port and
                          path.startswith('/api/isos/v1/')) or
                         (parsed.port == proxy.eos_port and
                          path.startswith('/api/eos/v2/'))))
                simulated = (proxy.hosted_origin is not None and selected_api and
                             original_origin == proxy.hosted_origin and
                             self.command in ('GET', 'POST', 'OPTIONS'))
                if simulated:
                    headers={key:value for key,value in headers.items()
                             if key.lower() != 'origin'}
                    headers['Origin'] = 'https://os-island.com'
                try:
                    connection.request(self.command, path, body=body or None,
                                       headers=headers)
                    response = connection.getresponse()
                    received = response.read()
                    status = response.status
                    response_headers = response.getheaders()
                finally:
                    connection.close()
                raw_acao = next((value for key,value in response_headers
                                 if key.lower() == 'access-control-allow-origin'), None)
                rewritten_acao = (proxy.hosted_origin if simulated and
                                  raw_acao == 'https://os-island.com' else raw_acao)
                delivered = (proxy.rewrite(self.path, received)
                             if proxy.rewrite else received)
                if simulated:
                    raw_cors={key.lower():value for key,value in response_headers
                              if key.lower() in CORS_HEADERS}
                    delivered_cors={**raw_cors}
                    if raw_acao == 'https://os-island.com':
                        delivered_cors['access-control-allow-origin']=rewritten_acao
                    with proxy.lock:
                        proxy.cors_rows.append({
                            'method':self.command, 'url':self.path,
                            'original_origin':original_origin,
                            'forwarded_origin':headers['Origin'],
                            'raw_status':status, 'delivered_status':status,
                            'raw_access_control_allow_origin':raw_acao,
                            'delivered_access_control_allow_origin':rewritten_acao,
                            'preflight_request_headers':{
                                key.lower():value for key,value in self.headers.items()
                                if key.lower().startswith('access-control-request-')},
                            'raw_cors_headers':raw_cors,
                            'delivered_cors_headers':delivered_cors,
                            'simulated_hosted_origin':True})
                self.send_response(status)
                for key, value in response_headers:
                    if key.lower() not in HOP_HEADERS and key.lower() != 'content-encoding':
                        if key.lower() == 'access-control-allow-origin':
                            value = rewritten_acao
                        self.send_header(key, value)
                self.send_header('Content-Length', str(len(delivered)))
                self.end_headers()
                self.wfile.write(delivered)
                if self.command == 'GET':
                    with proxy.lock:
                        if proxy.window is not None and self.path in proxy.allowed_urls:
                            proxy.rows.append({'window':proxy.window,
                                'url':self.path,'status':status,
                                'body_base64':base64.b64encode(delivered).decode()})

        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.thread = Thread(target=self.server.serve_forever, daemon=True)

    @property
    def port(self):
        return self.server.server_port

    def start(self):
        self.thread.start()
        return self

    def capture(self, name, allowed_urls):
        with self.lock:
            self.window = name
            self.allowed_urls = set(allowed_urls)
            self.rows = []

    def finish(self):
        with self.lock:
            rows = list(self.rows)
            self.window = None
            self.allowed_urls = set()
        return rows

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)
