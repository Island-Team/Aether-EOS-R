"""HTTP and browser stubs for loaded-response and advancing Vision capture."""

import base64
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import http.client
import json
from pathlib import Path
import tempfile
from threading import Thread
import time
from unittest import TestCase
from unittest.mock import patch

from q01_browser_raw import _responses
from q03_browser_capture import capture_assets, capture_matching_vision
from q03_recording_proxy import RecordingProxy


DOM = '<main class="node-graph-manager"><div class="node-graph-canvas"><div class="react-flow__pane"></div></div></main>'


class StubSite(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass

    def do_GET(self):
        if self.path == '/en/isos':
            body = b'<script src="/assets/main.js"></script>'
        elif self.path == '/assets/main.js':
            body = b'correct loaded script'
        elif self.path == '/assets/control.js':
            body = b'correct delayed control chunk'
        elif self.path == '/manifest.json':
            body=json.dumps({'schema_version':1,'version':'0.1.0',
                'source_commit':'b'*40,'api_schema_version':1,
                'build_target':'local'}).encode()
        elif self.path == '/api/private':
            body=b'grant-secret-owner-prompt'
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class BrowserStub:
    def __init__(self, proxy_port):
        self.proxy_port = proxy_port
        self.url = None

    def get(self, url):
        self.url = url
        connection = http.client.HTTPConnection('127.0.0.1', self.proxy_port)
        for path in (url, url.replace('/en/isos', '/assets/main.js'),
                     url.replace('/en/isos', '/api/private')):
            connection.request('GET', path)
            response=connection.getresponse()
            assert response.status == 200
            response.read()
        connection.close()

    def execute_script(self, script):
        if 'Boolean(document.querySelector' in script:
            return True
        if 'navigation:' in script:
            asset = self.url.replace('/en/isos', '/assets/main.js')
            return {'navigation':[self.url], 'scripts':[asset],
                    'resources':[{'url':asset,'initiator_type':'script'}]}
        return DOM

    def execute_async_script(self, script, url=None):
        if url is None:
            return True
        connection=http.client.HTTPConnection('127.0.0.1',self.proxy_port)
        connection.request('GET',url)
        response=connection.getresponse()
        body=response.read()
        result={'url':url,'status':response.status,
                'body_base64':base64.b64encode(body).decode()}
        connection.close()
        return result


class DelayedControlBrowser(BrowserStub):
    class Button:
        def __init__(self,browser,index):
            self.browser=browser
            self.index=index
        def click(self):
            browser=self.browser
            if self.index==1:
                browser.current_url=browser.url+'?page=eos'
                def late():
                    time.sleep(.3)
                    connection=http.client.HTTPConnection('127.0.0.1',browser.proxy_port)
                    connection.request('GET',browser.url.replace('/en/isos','/assets/control.js'))
                    response=connection.getresponse(); response.read(); connection.close()
                    browser.control_ready=True
                Thread(target=late,daemon=True).start()
            else:
                browser.current_url=browser.url

    def __init__(self,proxy_port):
        super().__init__(proxy_port)
        self.control_ready=False
        self.current_url=None

    def get(self,url):
        super().get(url)
        self.current_url=url

    def find_elements(self,kind,selector):
        return [self.Button(self,index) for index in range(3)]

    def execute_script(self,script,*args):
        if 'Q03_CONTROL_READY' in script:
            return ({'binding_path':'eos/dispatcher/base/base-velocity',
                     'revision':args[0]} if self.control_ready else None)
        if 'navigation:' in script:
            main=self.url.replace('/en/isos','/assets/main.js')
            late=self.url.replace('/en/isos','/assets/control.js')
            return {'navigation':[self.url],'scripts':[main],
                    'resources':[{'url':main,'initiator_type':'script'},
                                 {'url':late,'initiator_type':'script'}]}
        if 'return document.documentElement.outerHTML' in script and 'page=eos' in self.current_url:
            return (f'<nav><button aria-current="page">EOS</button></nav>'
                    f'<code>{"a"*64}</code>'
                    '<article class="eos-control__card--typed">'
                    '<code>eos/dispatcher/base/base-velocity</code>'
                    '<button class="eos-control__primary">Invoke</button></article>')
        return super().execute_script(script)


class CaptureLiveStub(TestCase):
    def test_hidden_control_waits_for_delayed_chunk_and_rejects_substitution(self):
        server=ThreadingHTTPServer(('127.0.0.1',0),StubSite)
        thread=Thread(target=server.serve_forever,daemon=True)
        thread.start()
        url=f'http://127.0.0.1:{server.server_port}/en/isos'
        manifest=json.dumps({'schema_version':1,'version':'0.1.0',
            'source_commit':'b'*40,'api_schema_version':1,
            'build_target':'local'}).encode()
        expected={'index.html':hashlib.sha256(b'<script src="/assets/main.js"></script>').hexdigest(),
            'assets/main.js':hashlib.sha256(b'correct loaded script').hexdigest(),
            'assets/control.js':hashlib.sha256(b'correct delayed control chunk').hexdigest(),
            'manifest.json':hashlib.sha256(manifest).hexdigest()}
        try:
            for replacement in (None,b'altered delayed chunk'):
                proxy=RecordingProxy(rewrite=lambda path,body:
                    replacement if replacement and path.endswith('control.js') else body).start()
                try:
                    row=capture_assets(DelayedControlBrowser(proxy.port),proxy,
                        'local',url,expected,interact=True,revision='a'*64)
                finally:
                    proxy.close()
                self.assertEqual(row['control']['binding_path'],
                                 'eos/dispatcher/base/base-velocity')
                self.assertEqual(len([entry for entry in row['responses']
                    if entry['url'].endswith('control.js')]),1)
                origin=f'http://127.0.0.1:{server.server_port}'
                if replacement:
                    with self.assertRaises(AssertionError):
                        _responses(row,expected,origin,'/en/isos','/','local','b'*40,'local')
                else:
                    self.assertEqual(_responses(row,expected,origin,
                        '/en/isos','/','local','b'*40,'local'),expected)
        finally:
            server.shutdown(); server.server_close(); thread.join(timeout=5)

    def test_proxy_records_initial_loaded_response_and_rejects_substitution(self):
        server = ThreadingHTTPServer(('127.0.0.1', 0), StubSite)
        thread = Thread(target=server.serve_forever, daemon=True)
        thread.start()
        url=f'http://127.0.0.1:{server.server_port}/en/isos'
        expected={'index.html':hashlib.sha256(b'<script src="/assets/main.js"></script>').hexdigest(),
                  'assets/main.js':hashlib.sha256(b'correct loaded script').hexdigest(),
                  'manifest.json':hashlib.sha256(json.dumps({'schema_version':1,
                      'version':'0.1.0','source_commit':'b'*40,
                      'api_schema_version':1,'build_target':'local'}).encode()).hexdigest()}
        try:
            with tempfile.TemporaryDirectory() as directory:
                root=Path(directory)
                (root/'browser-ready.json').write_text(json.dumps({'url':url,'revision':'a'*64,
                    'hosted_urls':{'en':url,'cn':url.replace('/en/isos','/cn/isos')}}))
                for replacement in (None, b'altered initial script'):
                    def rewrite(path, body):
                        return replacement if replacement and path.endswith('.js') else body
                    proxy=RecordingProxy(rewrite=rewrite).start()
                    try:
                        row=capture_assets(BrowserStub(proxy.port),proxy,'local',url,expected)
                    finally:
                        proxy.close()
                    # The proxy records the bytes delivered by the initial page load.
                    loaded=base64.b64decode(row['responses'][1]['body_base64'])
                    self.assertNotIn('grant-secret-owner-prompt',json.dumps(row))
                    origin=f'http://127.0.0.1:{server.server_port}'
                    if replacement:
                        self.assertEqual(loaded,replacement)
                        with self.assertRaises(AssertionError):
                            _responses(row,expected,origin,'/en/isos','/','local','b'*40,'local')
                    else:
                        self.assertEqual(loaded,b'correct loaded script')
                        self.assertEqual(_responses(row,expected,origin,
                            '/en/isos','/','local','b'*40,'local'),expected)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)

    def test_fresh_owner_sample_retries_after_browser_frame_advance(self):
        class Driver:
            def __init__(self):
                self.frame=0
            def execute_script(self, script):
                self.frame += 1
                digest=hashlib.sha256(b'\xff\xd8next\xff\xd9').hexdigest()
                return {'src':'blob:next','html':DOM+
                    '<section id="eos-vision"><p>Gate generation: 2 · Stream: '
                    'stream-1 · JPEG digest: '+digest+'</p>'
                    '<div class="eos-vision__image"><img src="blob:next"></div></section>'}
            def execute_async_script(self, script, url):
                return {'url':url,'status':200,'body_base64':base64.b64encode(b'\xff\xd8next\xff\xd9').decode()}
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            calls=[]
            def sample(out, home, run_id, revision, bridge_port, ui_port):
                calls.append(1)
                digest=hashlib.sha256(b'\xff\xd8old\xff\xd9' if len(calls)==1
                                       else b'\xff\xd8next\xff\xd9').hexdigest()
                (out/'q03-vision-owner.json').write_text(json.dumps(
                    {'metadata':{'jpeg_sha256':digest,
                                 'runtime_stream_id':'stream-1',
                                 'gate_generation':2}}))
            ready={'isos_home':'/tmp/stub','run_id':'run','revision':'a'*64,
                   'bridge_port':1,'url':'http://127.0.0.1:10001/en/isos'}
            with patch('q01_full_replay.capture_selected_vision',side_effect=sample):
                result=capture_matching_vision(Driver(),root,ready,'b'*40)
            self.assertEqual(len(calls),2)
            self.assertEqual(result['source_commit'],'b'*40)
