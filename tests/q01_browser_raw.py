"""Derive selected browser predicates from retained DOM and response bytes."""

import base64
import hashlib
from html.parser import HTMLParser
import json
from pathlib import Path
import re
from urllib.parse import urlsplit


class EditorDom(HTMLParser):
    def __init__(self):
        super().__init__()
        self.main = False
        self.canvas = False
        self.pane = False
        self.image = False

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        classes = set(attrs.get('class', '').split())
        self.main |= tag == 'main' and 'node-graph-manager' in classes
        self.canvas |= 'node-graph-canvas' in classes
        self.pane |= 'react-flow__pane' in classes
        self.image |= tag == 'img' and attrs.get('src', '').startswith('blob:')


class VisionDom(HTMLParser):
    VOID = {'area','base','br','col','embed','hr','img','input','link','meta',
            'param','source','track','wbr'}
    def __init__(self):
        super().__init__()
        self.depth = 0
        self.image = False
        self.parts = []

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == 'section' and attrs.get('id') == 'eos-vision':
            assert self.depth == 0, 'nested Vision section'
            self.depth = 1
        elif self.depth and tag not in self.VOID:
            self.depth += 1
        if self.depth and tag == 'img' and attrs.get('src', '').startswith('blob:'):
            self.image = True

    def handle_endtag(self, tag):
        if self.depth:
            self.depth -= 1

    def handle_startendtag(self, tag, attrs):
        self.handle_starttag(tag, attrs)
        if tag not in self.VOID:
            self.handle_endtag(tag)

    def handle_data(self, data):
        if self.depth:
            self.parts.append(data)


def verify_vision_dom(html, metadata):
    dom = VisionDom()
    dom.feed(html)
    text = ''.join(dom.parts)
    assert dom.image and dom.depth == 0, 'open Vision panel image missing'
    assert metadata['jpeg_sha256'] in text, 'Vision DOM digest differs'
    assert metadata['runtime_stream_id'] in text, 'Vision DOM stream differs'
    assert f": {metadata['gate_generation']} ·" in text, \
        'Vision DOM generation differs'


def _bytes(value):
    assert isinstance(value, str)
    return base64.b64decode(value, validate=True)


def _sha(value):
    return hashlib.sha256(value).hexdigest()


def _responses(row, expected, origin, index_route, prefix, label,
               selected_ui_commit, target):
    provenance = row['provenance']
    assert set(provenance) == {'navigation','scripts','resources','manifest_fetch'}
    assert provenance['navigation'] == [row['url']], 'browser navigation differs'
    resource_urls = {item['url'] for item in provenance['resources']}
    assert all(urlsplit(url).path.endswith(('.js','.css')) for url in resource_urls)
    assert provenance['scripts'] and set(provenance['scripts']) <= resource_urls, \
        'script elements lack browser resource timing'
    seen = {}
    asset_urls_seen = set()
    manifest_body = None
    for response in row['responses']:
        assert set(response) == {'window', 'url', 'status', 'body_base64'}
        assert response['window'] == label
        url = urlsplit(response['url'])
        if not (url.path == index_route or
                url.path.endswith(('.js','.css')) or
                url.path == prefix+'manifest.json'):
            continue
        assert response['status'] == 200
        assert f'{url.scheme}://{url.netloc}' == origin
        assert not url.query and not url.fragment
        if url.path == index_route:
            name = 'index.html'
        else:
            assert url.path.startswith(prefix)
            name = url.path[len(prefix):]
        assert name in expected and name not in seen, name
        body = _bytes(response['body_base64'])
        seen[name] = _sha(body)
        if name == 'manifest.json':
            manifest_body = body
            assert response == {'window':label, **provenance['manifest_fetch']}, \
                'browser manifest fetch differs from intercepted response'
        elif name != 'index.html':
            assert response['url'] in resource_urls, 'asset not loaded by browser'
            asset_urls_seen.add(response['url'])
    assert seen.get('index.html') == expected['index.html'], \
        'initial navigation response differs from selected build'
    assert asset_urls_seen == resource_urls, \
        'browser resource timing differs from intercepted responses'
    assert seen.get('manifest.json') == expected['manifest.json']
    assert json.loads(manifest_body) == {'schema_version':1,'version':'0.1.0',
        'source_commit':selected_ui_commit,'api_schema_version':1,
        'build_target':target}, 'browser loaded manifest identity differs'
    assert all(seen[name] == expected[name] for name in seen), \
        'loaded browser script bytes differ from selected build'
    assert all(url in {r['url'] for r in row['responses']}
               for url in provenance['scripts']), 'document script not intercepted'
    return seen


def verify_served_map(dist: Path, build_files: dict, served: dict):
    expected = {name:_sha((dist/name).read_bytes()) for name in build_files
                if name == 'index.html' or name.endswith('.js')}
    assert served == expected, 'served asset membership or bytes differ'
    return expected


def verify_control_observation(row, revision):
    control=row['control']
    assert set(control)=={'url','dom_html','binding_path','revision'}
    url=urlsplit(control['url'])
    page=urlsplit(row['url'])
    assert (url.scheme,url.netloc,url.path)==(page.scheme,page.netloc,page.path)
    assert url.query=='page=eos' and not url.fragment
    assert control['revision']==revision
    assert control['binding_path'].startswith('eos/')
    html=control['dom_html']
    assert 'eos-control__card--typed' in html and \
        control['binding_path'] in html and revision in html
    assert 'aria-current="page"' in html and '>EOS</button>' in html
    invoke=re.search(r'<button\b[^>]*class="[^"]*\beos-control__primary\b[^"]*"[^>]*>',html)
    assert invoke and 'disabled' not in invoke.group(), 'Control Invoke not ready'


def verify_browser_raw(root: Path, local_files: dict, hosted_files: dict,
                       selected_revision: str, selected_ui_commit: str):
    raw = json.loads((root/'q03-browser-raw.json').read_text())
    assert set(raw) == {'local', 'hosted', 'vision'}
    ready = json.loads((root/'browser-ready.json').read_text())
    assert ready['revision'] == selected_revision
    local = raw['local']
    assert local['url'] == ready['url']
    local_url = urlsplit(local['url'])
    assert local_url.path == '/en/isos' and not local_url.query and not local_url.fragment
    local_origin = f'{local_url.scheme}://{local_url.netloc}'
    local_seen = _responses(local, local_files, local_origin,
                            local_url.path, '/', 'local',selected_ui_commit,'local')
    dom = EditorDom()
    dom.feed(local['dom_html'])
    assert dom.main and dom.canvas and dom.pane, 'local editor DOM not loaded'
    verify_control_observation(local,selected_revision)
    assert set(raw['hosted']) == {'en', 'cn'}
    hosted_seen = {}
    for language in ('en', 'cn'):
        row = raw['hosted'][language]
        assert row['url'] == ready['hosted_urls'][language]
        url = urlsplit(row['url'])
        assert url.path == f'/{language}/isos' and not url.query and not url.fragment
        origin = f'{url.scheme}://{url.netloc}'
        hosted_seen[language] = _responses(row, hosted_files, origin,
            url.path, '/isosui/', f'hosted.{language}',selected_ui_commit,'hosted')
        dom = EditorDom()
        dom.feed(row['dom_html'])
        assert dom.main and dom.canvas and dom.pane, \
            f'{language} hosted editor DOM not loaded'
        verify_control_observation(row,selected_revision)
    vision = raw['vision']
    assert vision['url'] == ready['url']
    assert vision['active_revision'] == selected_revision
    assert vision['source_commit'] == selected_ui_commit
    jpeg = _bytes(vision['image_base64'])
    assert jpeg.startswith(b'\xff\xd8') and jpeg.endswith(b'\xff\xd9')
    html = vision['dom_html']
    dom = EditorDom()
    dom.feed(html)
    assert dom.main and dom.image, 'browser Vision image missing from DOM'
    return {'local': local_seen, 'hosted': hosted_seen,
            'vision_jpeg_sha256': _sha(jpeg), 'vision_dom_html': html}
