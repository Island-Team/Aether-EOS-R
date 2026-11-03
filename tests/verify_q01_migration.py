#!/usr/bin/env python3
"""Check the retained Q01 graph compatibility replay without a live IsOS home."""

import argparse
import hashlib
import json
import re
import shutil
import tempfile
import traceback
from pathlib import Path


CASES = {
    'future_2_0_0':('/version','unsupported_graph_version'),
    'unknown_type_1_0_0':('/cores/eos/layers/dispatcher/groups/control/nodes/command/node_type','unsupported_node_type'),
    'unsafe_component_1_0_0':('/cores/isos/layers/process/groups/component/nodes/eos/node_type','unsafe_process_node'),
    'legacy_unmapped_1_0_0':('/cores/eos/layers/control/groups/manual/nodes/manual_0/node_type','unmapped_node_type'),
}
FIXTURE_SHA256 = {
    'legacy_1_0_0': '7a8daee6b7db9105f5483506ffc1743625b63089710dfbf936622329c471016b',
    'legacy_1_0_0_whitespace': '27e137a1387393d7fb2e1abc1b735315cbdefd47a02ae2b265fa3596720158e9',
    'future_2_0_0': 'f4552929021608b7bb8dc4b141e4a93982636cb8010bc61c88598dc3589e4b29',
    'unknown_type_1_0_0': '087ce65c200debb12631d0fee566de8031dfcf1f68c76bddee46e23d1f8ec802',
    'unsafe_component_1_0_0': '09a8af69de54448e30d4bbfb7ed027e7b58ffba009578848458a5147e9cd7273',
    'legacy_optional_defaults_1_0_0': '44bc481fddc7cb910b7fab49d0e7dc417f91e845bcbb576cf8ec21c3231aac5f',
    'legacy_unmapped_1_0_0': 'bd0213124fe81b54497f00a8ad034798ebccf5eee8312d0be88a261eeb1c584f',
}


def load(path):
    return json.loads(path.read_text())


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def response(case, prefix):
    return responses(case,prefix)[-1]


def responses(case, prefix):
    found=sorted(case.glob(prefix+'-*.json'))
    assert found,(case,prefix)
    return [load(path) for path in found]


def envelope(reply, status, ok):
    assert reply['status']==status
    body=reply['body']
    assert body['schema_version']==1 and body['ok'] is ok
    if status==409:
        assert body['data'] is None
    else:
        assert isinstance(body['data'],dict)
    if ok:
        assert body['error'] is None
    else:
        assert isinstance(body['error'],str) and body['error']
    return body['data']


def at_pointer(value, pointer):
    for token in pointer.split('/')[1:]:
        value=value[token.replace('~1','/').replace('~0','~')]
    return value


def verify(root, selected_pins=None, selection=None):
    assert root.is_dir()
    seal_path=root/'evidence.sha256.json'
    if seal_path.is_file():
        seal=load(seal_path)
        found={path.relative_to(root).as_posix() for path in root.rglob('*')
               if path.is_file() and path!=seal_path}
        assert found==set(seal),(sorted(found-set(seal)),sorted(set(seal)-found))
        for name,expected_sha in seal.items():
            assert sha(root/name)==expected_sha,name
    if selected_pins is not None or selection is not None:
        pins=load(root/'selected-pins.json')
        if selection is None:
            assert pins['isos_source']==selected_pins['isos']==\
                '00c1d7aa184de8b969e6e40472bf9b1fa55032ff'
            assert pins['isos_tree']=='90d7a87c5c98dacbdcca0c2eacbdcead3fb4774b'
        else:
            owner=selection['owners']['isos']
            assert pins['isos_source']==owner['commit']
            assert pins['isos_tree']==owner['tree']
            canonical=(json.dumps(selection,sort_keys=True,separators=(',',':'))+'\n').encode()
            assert pins['selection_sha256']==hashlib.sha256(canonical).hexdigest()
            if selected_pins is not None:
                assert selected_pins['isos']==owner['commit']
        assert pins['isos_binary_sha256']==sha(root/'selected-isos.bin')
        if selected_pins is not None and 'isos_bin_sha256' in selected_pins:
            assert pins['isos_binary_sha256']==selected_pins['isos_bin_sha256']
    expected=set(CASES)|{'legacy_1_0_0','legacy_1_0_0_whitespace',
                          'legacy_optional_defaults_1_0_0','explicit-stock-migration'}
    assert {p.name for p in root.iterdir() if p.is_dir()}==expected
    for name in expected-{'explicit-stock-migration'}:
        case=root/name
        before=case/'before.graph.json'; after=case/'after.graph.json'
        assert before.read_bytes()==after.read_bytes()
        assert sha(before)==FIXTURE_SHA256[name]
        assert sha(before)==load(case/'before-revision.json')['sha256']
        assert sha(after)==load(case/'after-revision.json')['sha256']
        gets=responses(case,'api-node-graph-get')
        assert len(gets)==2,name
        for item in gets:
            data=envelope(item,200,True)
            assert data['revision']==sha(before)
            assert data['graph']==load(before)
        got=gets[-1]['body']['data']
        if name in CASES:
            path,code=CASES[name]
            for item in gets:
                data=item['body']['data']
                assert data['read_only'] is True
                assert 'legacy_projection' not in data
                assert (data['diagnostics'][0]['path'],data['diagnostics'][0]['code'])==(path,code)
            validations=responses(case,'api-node-graph-validate-post')
            assert len(validations)==1
            checked=envelope(validations[0],200,True)
            assert checked['valid'] is False
            assert (checked['diagnostics'][0]['path'],checked['diagnostics'][0]['code'])==(path,code)
            applies=responses(case,'api-node-graph-put')
            assert len(applies)==1
            applied=envelope(applies[0],422,False)
            assert (applied['diagnostics'][0]['path'],applied['diagnostics'][0]['code'])==(path,code)
            commands=(case/'commands.log').read_text()
            assert re.search(r'\$ [^\n]+ run\nexit=(?!0\n)\d+\n',commands)
            statuses=responses(case,'api-process-status-get')
            assert len(statuses)==1
            state=envelope(statuses[0],200,True)
            assert state['active_revision'] is None and state['run_id'] is None
            assert state['processes']==[] and state['state']=='stopped'
        else:
            for item in gets:
                projection=item['body']['data']['legacy_projection']
                assert projection['read_only'] is True
                assert projection['source_revision']==sha(before)
            statuses=responses(case,'api-process-status-get')
            assert len(statuses)==1
            state=envelope(statuses[0],200,True)
            assert state['active_revision'] is None and state['processes']==[]
            if name=='legacy_optional_defaults_1_0_0':
                validations=responses(case,'api-node-graph-validate-post')
                assert len(validations)==1
                assert envelope(validations[0],200,True)['valid'] is True
                applies=responses(case,'api-node-graph-put')
                assert len(applies)==1
                assert envelope(applies[0],200,True)['revision']==sha(before)
            if name=='legacy_1_0_0':
                previews=responses(case,'api-node-graph-migration-preview-post')
                assert len(previews)==1
                assert envelope(previews[0],200,True)['revision']==sha(before)
                stale=responses(case,'api-node-graph-migration-apply-post')
                assert len(stale)==1
                envelope(stale[0],409,False)
    standard=root/'legacy_1_0_0'
    whitespace=root/'legacy_1_0_0_whitespace'
    assert sha(standard/'before.graph.json')!=sha(whitespace/'before.graph.json')
    normal_projection=response(standard,'api-node-graph-get')['body']['data']['legacy_projection']
    whitespace_projection=response(whitespace,'api-node-graph-get')['body']['data']['legacy_projection']
    assert {key:value for key,value in normal_projection.items() if key!='source_revision'} == \
           {key:value for key,value in whitespace_projection.items() if key!='source_revision'}
    case=root/'explicit-stock-migration'
    source=case/'source.graph.json'
    migrated=case/'migrated.graph.json'
    assert sha(source)==FIXTURE_SHA256['legacy_1_0_0']
    assert source.read_bytes()==(case/'after-preview.graph.json').read_bytes()
    assert source.read_bytes()==(case/'after-stale.graph.json').read_bytes()
    assert sha(source)==load(case/'source-revision.json')['sha256']
    gets=responses(case,'api-node-graph-get')
    assert len(gets)==1
    initial=envelope(gets[0],200,True)
    assert initial['revision']==sha(source) and initial['graph']==load(source)
    previews=responses(case,'api-node-graph-migration-preview-post')
    assert len(previews)==1
    preview=envelope(previews[0],200,True)
    assert preview['can_apply'] and preview['revision']==sha(source)
    proposed=preview['proposed_revision']
    assert sha(migrated)==proposed==load(case/'migrated-revision.json')['sha256']
    commits=sorted(case.glob('api-node-graph-migration-apply-post-*.json'))
    assert len(commits)==2
    assert [load(path)['status'] for path in commits]==[409,200]
    graph=load(migrated)
    assert graph==preview['graph'] and graph['version']=='0.1.0'
    assert not graph.get('packages')
    source_graph=load(source)
    changes=preview['changes']
    assert changes and changes[0]=={'path':'/version','from':'1.0.0','to':'0.1.0'}
    reviewed={'eos.dispatcher.control@v1.0':'eos.dispatcher.control@v0.1',
              'isos.process.component@v1.1':'isos.process.component@v0.1'}
    for change in changes:
        assert at_pointer(source_graph,change['path'])==change['from']
        assert at_pointer(graph,change['path'])==change['to']
        if change['path']!='/version':
            assert change['path'].endswith('/node_type')
            assert reviewed[change['from']]==change['to']
    def unchanged(old,new,path=''):
        if path in {change['path'] for change in changes}:
            return
        if isinstance(old,dict) and isinstance(new,dict):
            assert old.keys()==new.keys(),path
            for key in old:
                unchanged(old[key],new[key],path+'/'+key)
        else:
            assert old==new,path
    unchanged(source_graph,graph)
    envelope(load(commits[0]),409,False)
    applied=envelope(load(commits[-1]),200,True)
    assert applied['revision']==proposed and applied['graph']==graph
    assert load(commits[0])['body']['ok'] is False
    assert load(commits[-1])['body']['ok'] is True


def tamper_suite(root):
    """Reseal semantic mutations so checksum failure cannot hide a weak check."""
    assert (root/'evidence.sha256.json').is_file()
    with tempfile.TemporaryDirectory(prefix='q01-migration-tamper-') as temp_name:
        copy=Path(temp_name)/'archive'
        shutil.copytree(root,copy)
        manifest=copy/'evidence.sha256.json'
        original_manifest=manifest.read_bytes()

        def trial(label, mutate):
            changed={}

            def replace(path, data):
                relative=path.relative_to(copy).as_posix()
                changed.setdefault(relative,path.read_bytes())
                path.write_bytes(data)

            def change_json(path, update):
                value=load(path)
                update(value)
                replace(path,(json.dumps(value,indent=2,sort_keys=True)+'\n').encode())

            mutate(replace,change_json)
            seal=json.loads(original_manifest)
            for name in changed:
                seal[name]=sha(copy/name)
            manifest.write_text(json.dumps(seal,indent=2,sort_keys=True)+'\n')
            try:
                verify(copy)
            except AssertionError:
                print('Q01_MIGRATION_TAMPER_REJECTED',label)
                print(traceback.format_exc().rstrip())
            else:
                raise AssertionError(f'semantic tamper accepted: {label}')
            finally:
                for name,original in changed.items():
                    (copy/name).write_bytes(original)
                manifest.write_bytes(original_manifest)

        def fixture_bytes(replace,change_json):
            case=copy/'legacy_optional_defaults_1_0_0'
            before=(case/'before.graph.json').read_bytes()
            assert before.endswith(b'\n')
            changed=before[:-1]+b' \n'
            assert json.loads(changed)==json.loads(before)
            revision=hashlib.sha256(changed).hexdigest()
            for label in ('before','after'):
                replace(case/f'{label}.graph.json',changed)
                change_json(case/f'{label}-revision.json',
                            lambda value: value.update(sha256=revision,bytes=len(changed)))
            for path in case.glob('api-node-graph-get-*.json'):
                def update_get(value):
                    value['body']['data']['revision']=revision
                    value['body']['data']['legacy_projection']['source_revision']=revision
                change_json(path,update_get)
            for path in case.glob('api-node-graph-put-*.json'):
                change_json(path,lambda value: value['body']['data'].update(revision=revision))

        def first_get_revision(replace,change_json):
            case=copy/'future_2_0_0'
            path=sorted(case.glob('api-node-graph-get-*.json'))[0]
            change_json(path,lambda value: value['body']['data'].update(revision='0'*64))

        def first_get_status(replace,change_json):
            case=copy/'future_2_0_0'
            path=sorted(case.glob('api-node-graph-get-*.json'))[0]
            change_json(path,lambda value: value.update(status=201))

        trial('frozen-fixture-whitespace-byte',fixture_bytes)
        trial('first-get-revision',first_get_revision)
        trial('first-get-status',first_get_status)
        verify(copy)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive',type=Path)
    parser.add_argument('--tamper-suite',action='store_true')
    args=parser.parse_args()
    verify(args.archive.resolve())
    if args.tamper_suite:
        tamper_suite(args.archive.resolve())
    print('Q01_MIGRATION_ARCHIVE_VERIFIED')


if __name__=='__main__':
    main()
