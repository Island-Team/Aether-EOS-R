"""Bounded real Firefox check of Q03 loaded-byte and Vision capture wiring."""

import base64
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import socketserver
from threading import Thread
import unittest

from q01_browser_raw import _responses
from q03_browser_capture import (capture_assets, capture_vision,
                                  firefox_options, open_vision)
from q03_recording_proxy import RecordingProxy


JPEG=b'\xff\xd8stub-frame\xff\xd9'
DIGEST=hashlib.sha256(JPEG).hexdigest()


class WebSocketStub(socketserver.BaseRequestHandler):
    def handle(self):
        request=bytearray()
        while b'\r\n\r\n' not in request:
            data=self.request.recv(4096)
            if not data:
                return
            request.extend(data)
        headers={}
        for line in request.decode('latin-1').split('\r\n')[1:]:
            if ': ' in line:
                key,value=line.split(': ',1)
                headers[key.lower()]=value
        import hashlib as _hashlib
        accept=base64.b64encode(_hashlib.sha1((headers['sec-websocket-key']+
            '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
        self.request.sendall(('HTTP/1.1 101 Switching Protocols\r\n'
            'Upgrade: websocket\r\nConnection: Upgrade\r\n'
            f'Sec-WebSocket-Accept: {accept}\r\n\r\n').encode())
        self.request.sendall(b'\x81\x02ok')


class SiteStub(BaseHTTPRequestHandler):
    ws_port=0

    def log_message(self, format, *args):
        pass

    def do_GET(self):
        if self.path=='/en/isos':
            body=b'''<html><body><main class="node-graph-manager"><div class="node-graph-canvas"><div class="react-flow__pane"></div></div></main><div class="isos-workbench__panel-menu"><button aria-pressed="true">Control</button><button aria-pressed="false" id="vision-button">Vision</button></div><section id="eos-vision" hidden><div class="eos-vision__image"></div><code>'''+DIGEST.encode()+b'''</code></section><p id="socket"></p><script src="/assets/main.js"></script></body></html>'''
        elif self.path=='/assets/main.js':
            body=(f'''const ws=new WebSocket('ws://127.0.0.1:{self.ws_port}/stream');
ws.onmessage=e=>document.querySelector('#socket').textContent=e.data;
document.querySelector('#vision-button').onclick=e=>{{
  e.target.setAttribute('aria-pressed','true');
  const blob=new Blob([new Uint8Array({list(JPEG)})],{{type:'image/jpeg'}});
  const img=document.createElement('img'); img.src=URL.createObjectURL(blob);
  document.querySelector('.eos-vision__image').append(img);
  document.querySelector('#eos-vision').hidden=false;
}};''').encode()
        elif self.path=='/manifest.json':
            import json
            body=json.dumps({'schema_version':1,'version':'0.1.0',
                'source_commit':'b'*40,'api_schema_version':1,
                'build_target':'local'}).encode()
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header('Content-Type','text/html' if self.path=='/en/isos' else 'application/javascript')
        self.send_header('Content-Length',str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class FirefoxStub(unittest.TestCase):
    def test_localhost_proxy_loaded_assets_websocket_and_vision_panel(self):
        try:
            from selenium import webdriver
            from selenium.webdriver.firefox.service import Service
            from selenium.webdriver.support.ui import WebDriverWait
        except ImportError:
            self.skipTest('Selenium runtime unavailable')
        ws=socketserver.ThreadingTCPServer(('127.0.0.1',0),WebSocketStub)
        ws.daemon_threads=True
        SiteStub.ws_port=ws.server_address[1]
        site=ThreadingHTTPServer(('127.0.0.1',0),SiteStub)
        ws_thread=Thread(target=ws.serve_forever,daemon=True)
        site_thread=Thread(target=site.serve_forever,daemon=True)
        ws_thread.start(); site_thread.start()
        proxy=RecordingProxy().start()
        driver=None
        try:
            driver=webdriver.Firefox(service=Service('/snap/bin/geckodriver'),
                options=firefox_options(proxy.port))
            driver.set_script_timeout(15)
            url=f'http://127.0.0.1:{site.server_port}/en/isos'
            files={'index.html':'0'*64,'assets/main.js':'0'*64,
                   'manifest.json':'0'*64}
            row=capture_assets(driver,proxy,'local',url,files)
            loaded=[response for response in row['responses']
                    if response['url'] in (url,url.replace('/en/isos','/assets/main.js'))]
            self.assertEqual(len(loaded),2, 'Firefox bypassed recording proxy')
            origin=f'http://127.0.0.1:{site.server_port}'
            expected={'index.html':hashlib.sha256(base64.b64decode(
                loaded[0]['body_base64'])).hexdigest(),
                'assets/main.js':hashlib.sha256(base64.b64decode(
                loaded[1]['body_base64'])).hexdigest(),
                'manifest.json':next(hashlib.sha256(base64.b64decode(
                    response['body_base64'])).hexdigest() for response in row['responses']
                    if response['url'].endswith('/manifest.json'))}
            self.assertEqual(_responses(row,expected,origin,'/en/isos','/',
                                       'local','b'*40,'local'),expected)
            WebDriverWait(driver,10).until(lambda browser: browser.execute_script(
                "return document.querySelector('#socket').textContent==='ok'"))
            open_vision(driver)
            vision=capture_vision(driver,url,'a'*64,'b'*40)
            self.assertEqual(hashlib.sha256(base64.b64decode(
                vision['image_base64'])).hexdigest(),DIGEST)
        finally:
            if driver:
                driver.quit()
            proxy.close()
            site.shutdown(); ws.shutdown()
            site.server_close(); ws.server_close()
            site_thread.join(timeout=5); ws_thread.join(timeout=5)
