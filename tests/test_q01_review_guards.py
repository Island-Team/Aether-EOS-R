"""Review regressions for raw browser and cross-case byte gates."""

import base64
import copy
import json
from pathlib import Path
import tempfile
from unittest import TestCase, main

from q01_browser_raw import verify_browser_raw, verify_served_map
from q01_selection import sha256, verify_case_build_bytes
from verify_q01 import verify


def encoded(raw):
    return base64.b64encode(raw).decode()


class ReviewGuards(TestCase):
    def browser_fixture(self, root):
        local = 'http://127.0.0.1:10001/en/isos'
        hosted_en = 'http://127.0.0.1:10002/en/isos'
        hosted_cn = 'http://127.0.0.1:10002/cn/isos'
        (root/'browser-ready.json').write_text(json.dumps({
            'revision':'a'*64,'url':local,
            'hosted_urls':{'en':hosted_en,'cn':hosted_cn}}))
        dom = '<main class="node-graph-manager"><div class="node-graph-canvas"><div class="react-flow__pane"></div></div></main>'
        def row(label, url, script_url, target):
            manifest_url=('http://127.0.0.1:10001/manifest.json' if target=='local'
                          else 'http://127.0.0.1:10002/isosui/manifest.json')
            manifest=json.dumps({'schema_version':1,'version':'0.1.0',
                'source_commit':'b'*40,'api_schema_version':1,
                'build_target':target}).encode()
            manifest_fetch={'url':manifest_url,'status':200,
                            'body_base64':encoded(manifest)}
            control_html=(f'<nav><button aria-current="page">EOS</button></nav>'
                f'<code>{"a"*64}</code><article class="eos-control__card--typed">'
                '<code>eos/dispatcher/base/base-velocity</code>'
                '<button class="eos-control__primary">Invoke</button></article>')
            return {'url':url,'dom_html':dom,
                'control':{'url':url+'?page=eos','dom_html':control_html,
                    'binding_path':'eos/dispatcher/base/base-velocity',
                    'revision':'a'*64},
                'provenance':{'navigation':[url],'scripts':[script_url],
                    'resources':[{'url':script_url,'initiator_type':'script'}],
                    'manifest_fetch':manifest_fetch},
                'responses':[
                {'window':label,'url':url,'status':200,
                 'body_base64':encoded(b'<html>editor</html>')},
                {'window':label,'url':script_url,'status':200,
                 'body_base64':encoded(b'javascript')},
                {'window':label,**manifest_fetch}]}
        raw = {'local':row('local',local,'http://127.0.0.1:10001/assets/main.js','local'),
               'hosted':{
                   'en':row('hosted.en',hosted_en,'http://127.0.0.1:10002/isosui/assets/main.js','hosted'),
                   'cn':row('hosted.cn',hosted_cn,'http://127.0.0.1:10002/isosui/assets/main.js','hosted')},
               'vision':{'url':local,'active_revision':'a'*64,
                   'source_commit':'b'*40,'image_base64':encoded(b'\xff\xd8frame\xff\xd9'),
                   'dom_html':dom+'<img src="blob:frame" />'}}
        (root/'q03-browser-raw.json').write_text(json.dumps(raw))
        files={'index.html':sha256(b'<html>editor</html>'),
               'assets/main.js':sha256(b'javascript')}
        local_files={**files,'manifest.json':sha256(json.dumps({
            'schema_version':1,'version':'0.1.0','source_commit':'b'*40,
            'api_schema_version':1,'build_target':'local'}).encode())}
        hosted_files={**files,'manifest.json':sha256(json.dumps({
            'schema_version':1,'version':'0.1.0','source_commit':'b'*40,
            'api_schema_version':1,'build_target':'hosted'}).encode())}
        return raw,local_files,hosted_files

    def test_raw_browser_checks_both_routes_dom_and_received_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            raw,local_files,hosted_files=self.browser_fixture(root)
            observed=verify_browser_raw(root,local_files,hosted_files,'a'*64,'b'*40)
            self.assertEqual(observed['hosted']['cn'],hosted_files)
            for change in ('false-editor','substituted-route','missing-script',
                           'initial-script-substitution','bad-manifest',
                           'forged-vision'):
                forged=copy.deepcopy(raw)
                if change=='false-editor':
                    forged['hosted']['cn']['dom_html']='<main>loading</main>'
                elif change=='substituted-route':
                    forged['hosted']['cn']['url']=forged['hosted']['en']['url']
                elif change=='missing-script':
                    forged['hosted']['cn']['responses'].pop(1)
                elif change=='initial-script-substitution':
                    forged['hosted']['cn']['responses'][1]['body_base64']=encoded(b'bad initial script')
                elif change=='bad-manifest':
                    body=json.loads(base64.b64decode(
                        forged['hosted']['cn']['responses'][2]['body_base64']))
                    body['source_commit']='0'*40
                    forged['hosted']['cn']['responses'][2]['body_base64']=encoded(
                        json.dumps(body).encode())
                    forged['hosted']['cn']['provenance']['manifest_fetch']=(
                        {key:value for key,value in forged['hosted']['cn']['responses'][2].items()
                         if key!='window'})
                else:
                    forged['vision']['image_base64']=encoded(b'not a jpeg')
                (root/'q03-browser-raw.json').write_text(json.dumps(forged))
                with self.subTest(change=change),self.assertRaises(AssertionError):
                    verify_browser_raw(root,local_files,hosted_files,'a'*64,'b'*40)
            forged=copy.deepcopy(raw)
            forged['hosted']['cn']['url']='http://127.0.0.1:10002/isosui/cn/isos'
            (root/'q03-browser-raw.json').write_text(json.dumps(forged))
            ready=json.loads((root/'browser-ready.json').read_text())
            ready['hosted_urls']['cn']=forged['hosted']['cn']['url']
            (root/'browser-ready.json').write_text(json.dumps(ready))
            with self.assertRaises(AssertionError):
                verify_browser_raw(root,local_files,hosted_files,'a'*64,'b'*40)

    def test_resealed_case_alternate_executable_and_ui_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            case=Path(directory)
            (case/'bin').mkdir()
            (case/'package-input').mkdir()
            (case/'ui-dist').mkdir()
            (case/'bin/isos').write_bytes(b'main binary')
            (case/'package-input/asset.tar.gz').write_bytes(b'package')
            (case/'ui-dist/index.html').write_bytes(b'case UI')
            manifest=case/'evidence.sha256.json'
            manifest.write_text('{}\n')
            entry={'manifest_sha256':sha256(manifest.read_bytes())}
            build={'binaries_sha256':{'isos':sha256(b'main binary')},
                   'package':{'asset_sha256':sha256(b'package')}}
            receipt={'cases':{'p01-p03-u01':{
                'manifest_sha256':entry['manifest_sha256'],
                'ui_files_sha256':{'index.html':sha256(b'case UI')}}}}
            verify_case_build_bytes('p01-p03-u01',case,entry,build,receipt)
            (case/'bin/isos').write_bytes(b'alternate binary')
            manifest.write_text('{"resealed":true}\n')
            entry['manifest_sha256']=sha256(manifest.read_bytes())
            receipt['cases']['p01-p03-u01']['manifest_sha256']=entry['manifest_sha256']
            with self.assertRaises(AssertionError):
                verify_case_build_bytes('p01-p03-u01',case,entry,build,receipt)
            (case/'bin/isos').write_bytes(b'main binary')
            (case/'ui-dist/index.html').write_bytes(b'substituted UI')
            with self.assertRaisesRegex(AssertionError,'UI bytes differ'):
                verify_case_build_bytes('p01-p03-u01',case,entry,build,receipt)

    def test_late_control_chunk_substitution_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            raw,local_files,hosted_files=self.browser_fixture(root)
            late='http://127.0.0.1:10001/assets/control.js'
            raw['local']['provenance']['resources'].append(
                {'url':late,'initiator_type':'script'})
            raw['local']['responses'].append({'window':'local','url':late,
                'status':200,'body_base64':encoded(b'altered late chunk')})
            local_files['assets/control.js']=sha256(b'selected late chunk')
            (root/'q03-browser-raw.json').write_text(json.dumps(raw))
            with self.assertRaisesRegex(AssertionError,'loaded browser script bytes'):
                verify_browser_raw(root,local_files,hosted_files,'a'*64,'b'*40)

    def test_resealed_hosted_served_map_missing_script_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            dist=Path(directory)
            (dist/'assets').mkdir()
            (dist/'index.html').write_bytes(b'index')
            (dist/'assets/main.js').write_bytes(b'script')
            build={'index.html':sha256(b'index'),
                   'assets/main.js':sha256(b'script')}
            served={'index.html':sha256(b'index')}
            with self.assertRaisesRegex(AssertionError,'served asset membership'):
                verify_served_map(dist,build,served)

    def test_missing_case_receipt_rejected_before_archive(self):
        pins={name:digit*40 for name,digit in
              (('isos','a'),('eos','b'),('egia','c'),('ui','d'),('mujoco','e'))}
        selection={'owners':{name:{'commit':pin} for name,pin in pins.items()}}
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(AssertionError,'case receipt required'):
                verify(Path(directory),expected_pins=pins,selection=selection,
                       selection_sha256='f'*64,build_receipt={})


if __name__=='__main__':
    main()
