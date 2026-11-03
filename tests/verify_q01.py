#!/usr/bin/env python3
"""Verify retained Q01 main-flow evidence without a live home or network.

The complete qualification also requires the camera and negative matrix cases.
This verifier accepts an archive only when those separately retained cases are
listed in ``q01-matrix.json`` and their manifests verify.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import sys
import tomllib

from q01_selection import (HISTORICAL, check_harness, load_build_receipt,
                           load_case_receipt, load_selection,
                           selected_pins as frozen_pins,
                           verify_case_build_bytes)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load(root, name):
    return json.loads((root/name).read_text())


def correlated_log_pages(root, label, command_id, run_id, revision):
    first=f'q01-logs-{label}-correlated.json'
    index_path=root/f'q01-logs-{label}-correlated-pages.json'
    index=(load(root,index_path.name) if index_path.exists() else
           [{'file':first,'request_cursor':None}])
    assert isinstance(index,list) and 1 <= len(index) <= 512
    assert index[0]=={'file':first,'request_cursor':None}
    pages=[]
    cursor=None
    for number,item in enumerate(index,1):
        expected_name=(first if number==1 else
                       f'q01-logs-{label}-correlated-page-{number:04d}.json')
        assert item=={'file':expected_name,'request_cursor':cursor}, item
        page=load(root,expected_name)
        assert page['scope']=={'kind':'deployment_run','run_id':run_id,
                               'active_revision':revision}
        assert page['anchor']['state']=='found'
        assert page['diagnostics']==[]
        assert all(line['correlation_id']==command_id for line in page['lines'])
        following=page['next_cursor']
        assert isinstance(following,str) and following.startswith('lc1.')
        if number < len(index):
            assert following!=cursor, 'cursor stabilized before final page'
        elif index_path.exists():
            assert following==cursor, 'correlated cursor was not exhausted'
        cursor=following
        pages.append(page)
    return pages


def files(root):
    result = {}
    for path in root.rglob('*'):
        mode = path.lstat().st_mode
        if stat.S_ISDIR(mode):
            continue
        assert stat.S_ISREG(mode), f'nonregular archive member: {path}'
        if path != root/'evidence.sha256.json':
            result[path.relative_to(root).as_posix()] = digest(path)
    return result


def payload_digest(root):
    tree=hashlib.sha256()
    tree.update(b'isos-package-tree-v1\0')
    entries=sorted((p.relative_to(root).as_posix(),p) for p in root.rglob('*')
                   if p.relative_to(root).as_posix() not in
                   {'receipt.json','release.json','node-types.json'})
    for name,path in entries:
        assert not path.is_symlink()
        info=path.stat()
        is_file=path.is_file()
        assert is_file or path.is_dir()
        encoded=name.encode()
        tree.update(len(encoded).to_bytes(4,'little'))
        tree.update(encoded)
        tree.update(b'F' if is_file else b'D')
        tree.update(b'1' if is_file and info.st_mode & 0o111 else b'0')
        tree.update((info.st_size if is_file else 0).to_bytes(8,'little'))
        tree.update(bytes.fromhex(digest(path)) if is_file else bytes(32))
    return tree.hexdigest()


def seal(root):
    (root/'evidence.sha256.json').write_text(json.dumps(files(root),indent=2,sort_keys=True)+'\n')


def verify(root, *, main_only=False, expected_pins=None, selection=None,
           selection_sha256=None, build_receipt=None, case_receipt=None):
    if not main_only:
        assert expected_pins is not None, 'full Q01 gate requires five external owner pins'
        assert set(expected_pins) == {'isos', 'eos', 'egia', 'ui', 'mujoco'}
        assert all(len(pin) == 40 and all(c in '0123456789abcdef' for c in pin)
                   for pin in expected_pins.values())
        if selection is not None:
            assert expected_pins == frozen_pins(selection), \
                'CLI pins conflict with external selection'
            assert selection_sha256 is not None, 'selection SHA-256 required'
            assert build_receipt is not None, 'reviewer-owned build receipt required'
            assert case_receipt is not None, 'reviewer-owned case receipt required'
            check_harness(selection, Path(__file__).parent)
    assert root.is_dir() and root.is_absolute()
    expected = load(root,'evidence.sha256.json')
    assert expected == files(root), 'archive member missing, added or changed'
    if selection is not None:
        assert load(root, 'q03-selection-sha256.json') == \
            {'profile': 'selected-q03', 'sha256': selection_sha256}, \
            'archive selection identity differs'
        assert load(root, 'q03-selected-selection.json') == selection, \
            'archive selection copy differs'
        archived_harness = root/'harness'
        check_harness(selection, archived_harness)
        assert load(root,'q03-build-receipt.json') == build_receipt, \
            'archived build receipt differs from reviewer-owned receipt'
        assert load(root,'q03-case-build-receipt.json') == case_receipt, \
            'archived case receipt differs from reviewer-owned receipt'
        assert build_receipt['package'] == selection['package']
        for directory, key in (('selected-binaries','binaries_sha256'),
                               ('ui-dist','ui_local_sha256'),
                               ('ui-hosted-dist','ui_hosted_sha256')):
            assert {p.relative_to(root/directory).as_posix():digest(p)
                    for p in (root/directory).rglob('*') if p.is_file()} == \
                build_receipt[key], f'{directory} differs from build receipt'
        assert {name:digest(root/name) for name in build_receipt['build_logs_sha256']} == \
            build_receipt['build_logs_sha256']
        assert load(root,'q01-eos-build.json')['crates_tree_oid'] == \
            selection['owners']['eos']['subtrees']['crates']
        isos_build = load(root,'q01-isos-build.json')
        assert isos_build['source_tree_oid'] == selection['owners']['isos']['tree']
        assert isos_build['cargo_lock_sha256'] == \
            selection['owners']['isos']['locks']['Cargo.lock']
        assert digest(root/'package-input/release.json') == \
            selection['package']['release_sha256']
        assert digest(root/'package-input/asset.tar.gz') == \
            selection['package']['asset_sha256']
        from q01_browser_raw import (verify_browser_raw, verify_served_map,
                                     verify_vision_dom)
        local_served = load(root,'q01-ui-served.json')['served_sha256']
        expected_local_served = verify_served_map(root/'ui-dist',
            build_receipt['ui_local_sha256'],local_served)
        hosted = load(root,'q03-hosted-ui-served.json')
        assert hosted['routes'] == {'en':'/en/isos','cn':'/cn/isos'}
        expected_hosted_served = verify_served_map(root/'ui-hosted-dist',
            build_receipt['ui_hosted_sha256'],hosted['served_sha256'])
        assert hosted['cn_index_sha256'] == build_receipt['ui_hosted_sha256']['index.html']
        browser_local_files={name:value for name,value in
            build_receipt['ui_local_sha256'].items() if name in
            ('index.html','manifest.json') or name.endswith(('.js','.css'))}
        browser_hosted_files={name:value for name,value in
            build_receipt['ui_hosted_sha256'].items() if name in
            ('index.html','manifest.json') or name.endswith(('.js','.css'))}
        browser_raw = verify_browser_raw(root,browser_local_files,
            browser_hosted_files,load(root,'status-running.json')['active_revision'],
            selection['owners']['ui']['commit'])
        from q03_hosted_origin import verify_hosted_origin
        hosted_origin_path=root/'q03-simulated-hosted-origin.json'
        hosted_origin_proof=verify_hosted_origin(
            load(root,'q03-simulated-hosted-origin.json'),
            load(root,'browser-ready.json'))
        assert load(root,'q03-hosted-origin-validation.json') == {
            'source_sha256':digest(hosted_origin_path),
            'proof':hosted_origin_proof}, 'hosted-Origin validation differs'
        if not main_only:
            assert case_receipt['hosted_origin_sha256'] == digest(hosted_origin_path), \
                'reviewer-owned hosted-Origin digest differs'
        assert load(root,'q03-browser-raw-validation.json') == {
            'source_sha256':digest(root/'q03-browser-raw.json'),
            'local':browser_raw['local'],'hosted':browser_raw['hosted'],
            'vision_jpeg_sha256':browser_raw['vision_jpeg_sha256']}
        assert all(local_served[name] == digest for name,digest in
                   browser_raw['local'].items() if name in local_served)
        assert all(expected_hosted_served[name] == digest for observed in
                   browser_raw['hosted'].values() for name,digest in
                   observed.items() if name in expected_hosted_served)
        browser_hosted = load(root,'q03-hosted-browser-observation.json')
        assert set(browser_hosted) == {'en','cn'}
        for language in ('en','cn'):
            row = browser_hosted[language]
            assert row['path'] == f'/{language}/isos'
            assert row['editor_loaded'] is True
            assert row['source_commit'] == selection['owners']['ui']['commit']
            assert row['index_sha256'] == build_receipt['ui_hosted_sha256']['index.html']
            assert row['script_sha256'] == {name:digest(root/'ui-hosted-dist'/name)
                for name in browser_raw['hosted'][language] if name.endswith('.js')}
            assert row['index_sha256'] == browser_raw['hosted'][language]['index.html']
            assert row['script_sha256'] == {name:browser_raw['hosted'][language][name]
                for name in browser_raw['hosted'][language] if name.endswith('.js')}
        vision_binding = load(root,'q03-vision-binding.json')
        owner_vision = load(root,'q03-vision-owner.json')
        bridge_vision = load(root,'q03-vision-bridge.json')
        egia_vision = load(root,'q03-vision-egia.json')
        browser_vision = load(root,'q03-browser-vision-observation.json')
        current_revision = load(root,'status-running.json')['active_revision']
        assert vision_binding['graph_revision'] == current_revision
        run_id = load(root,'status-running.json')['run_id']
        assert vision_binding['deployment_run_id'] == run_id
        endpoint = vision_binding['eos_endpoint']
        assert endpoint['transport'] == 'uds'
        owner_socket = Path(endpoint['path'])
        endpoint_name = hashlib.sha256(f'default\0{run_id}'.encode()).hexdigest()[:32]
        assert owner_socket.is_absolute()
        selected_home = Path(load(root,'browser-ready.json')['isos_home'])
        assert selected_home.is_absolute()
        assert owner_socket == selected_home / 'run/v' / endpoint_name / 'eos-vision-owner.sock'
        identity = ('graph_revision','projection_revision','runtime_stream_id',
                    'source_path','source_frame_id','gate_path','gate_generation',
                    'render','jpeg_sha256')
        metadata = owner_vision['metadata']
        assert metadata['graph_revision'] == current_revision
        assert metadata['gate_path'] == vision_binding['gate_path']
        assert metadata['projection_revision'] == vision_binding['projection_revision']
        assert type(metadata['gate_generation']) is int and metadata['gate_generation'] >= 0
        for peer in (bridge_vision['metadata'],egia_vision,
                     browser_vision['metadata']):
            assert all(peer[key] == metadata[key] for key in identity)
        assert owner_vision['jpeg_verified_sha256'] == \
            bridge_vision['jpeg_verified_sha256'] == \
            browser_vision['jpeg_verified_sha256'] == metadata['jpeg_sha256']
        assert browser_raw['vision_jpeg_sha256'] == metadata['jpeg_sha256']
        verify_vision_dom(browser_raw['vision_dom_html'],metadata)
        assert browser_vision['rendered'] is True
        assert browser_vision['active_revision'] == current_revision
        assert browser_vision['source_commit'] == selection['owners']['ui']['commit']
    inputs = load(root,'selected-inputs.json')
    for owner in ('isos', 'eos', 'egia', 'ui'):
        pin = inputs[owner]
        assert len(pin) == 40 and all(c in '0123456789abcdef' for c in pin), owner
    if expected_pins is not None:
        assert {owner: inputs[owner] for owner in ('isos', 'eos', 'egia', 'ui')} == \
            {owner: expected_pins[owner] for owner in ('isos', 'eos', 'egia', 'ui')}, \
            'archive owner pins differ from the selected integration commits'
        assert load(root,'q01-package-input.json')['source_commit'] == expected_pins['mujoco'], \
            'archive MuJoCo package differs from the selected integration commit'
        if not main_only and selection is None:
            assert expected_pins == HISTORICAL, \
                'new owner heads require external selected-q03 selection'
    selected_pins = {**inputs,
                     'mujoco': load(root,'q01-package-input.json')['source_commit']}
    product_selection=load(root,'q01-island-selection.json')
    authored=(root/'q01-island-source.toml').read_bytes()
    selected=(root/'q01-island-selected.toml').read_bytes()
    assert digest(root/'q01-island-source.toml')==product_selection['authored_sha256']
    assert digest(root/'q01-island-selected.toml')==product_selection['selected_sha256']
    assert authored.count(b'"../Island-eos/share"')==1
    expected=authored.replace(b'"../Island-eos/share"',
                              ('"'+product_selection['selected_eos_share']+'"').encode())
    assert selected==expected
    config=tomllib.loads(selected.decode())
    assert config['share'][1]==product_selection['selected_eos_share']
    egia_root=Path(product_selection['egia_source_root'])
    eos_root=Path(product_selection['eos_source_root'])
    assert egia_root.is_absolute() and eos_root.is_absolute()
    assert not Path(config['share'][1]).is_absolute()
    assert os.path.normpath(str(egia_root/config['share'][1]))==str(eos_root/'share')
    eos_build=load(root,'q01-eos-build.json')
    assert eos_build['source_commit']==inputs['eos']
    assert len(eos_build['crates_tree_oid']) == 40
    assert all(c in '0123456789abcdef' for c in eos_build['crates_tree_oid'])
    assert eos_build['command']=='cargo build --locked --workspace --bins'
    assert eos_build['binary_sha256']=={
        name:digest(root/'selected-binaries'/name)
        for name in ('aether-runtime','aether-profile','aether-http-bridge',
                     'aether-config-server')}
    isos_build=load(root,'q01-isos-build.json')
    assert isos_build['source_commit']==inputs['isos']
    assert isos_build['source_head']==inputs['isos']
    assert isos_build['source_tree_oid']==inputs['isos_source_tree_oid']
    assert isos_build['cargo_lock_sha256']==inputs['isos_cargo_lock_sha256']
    assert isos_build['command']=='cargo build --locked --bin isos'
    assert isos_build['binary_sha256']==digest(root/'bin/isos')
    assert 'Finished' in (root/'isos-selected-build.log').read_text()
    assert digest(root/'bin/isos')==inputs['isos_bin_sha256']
    assert digest(root/'selected-binaries/aether-runtime')==inputs['eos_runtime_sha256']
    assert digest(root/'selected-binaries/egia')==inputs['egia_launcher_sha256']
    assert digest(root/'selected-binaries/egia-config')==inputs['egia_config_sha256']
    assert digest(root/'ui-dist/index.html')==inputs['ui_index_sha256']
    assert load(root/'ui-dist','manifest.json')['source_commit']==inputs['ui']
    ui_build=load(root,'q01-ui-rebuild.json')
    assert ui_build['source_commit']==inputs['ui']
    assert ui_build['isos_api_origin']==f"http://127.0.0.1:{inputs['api_port']}"
    assert ui_build['eos_api_origin']==f"http://127.0.0.1:{inputs['bridge_port']}"
    assert ui_build['files_sha256']=={p.relative_to(root/'ui-dist').as_posix():digest(p)
                                      for p in (root/'ui-dist').rglob('*') if p.is_file()}
    ui_served=load(root,'q01-ui-served.json')
    assert ui_served['isos_api_origin']==ui_build['isos_api_origin']
    assert ui_served['eos_api_origin']==ui_build['eos_api_origin']
    assert ui_served['served_sha256']=={
        p.relative_to(root/'ui-dist').as_posix():digest(p)
        for p in (root/'ui-dist').rglob('*')
        if p.is_file() and (p.name=='index.html' or p.suffix=='.js')}
    receipt = load(root,'q01-package-receipt.json')
    package = root/'package-input'
    installed = root/'package-installed'
    assert receipt['schema_version']==2
    assert (receipt['id'],receipt['version'],receipt['source_kind']) == (
        'island/mujoco-sim','0.1.1','local_candidate')
    assert receipt['source_commit']==load(root,'q01-package-input.json')['source_commit']
    assert receipt['asset_sha256']==digest(package/'asset.tar.gz')
    assert digest(package/'release.json')==inputs['mujoco_release_sha256']
    assert receipt['release_sha256']==digest(installed/'release.json')
    assert receipt['sidecar_sha256']==digest(installed/'node-types.json')
    assert receipt['payload_sha256']==payload_digest(installed)
    install=load(root,'q01-install-state.json')
    assert install['graph_unchanged'] is True
    assert digest(root/'graph-before-install.raw.json')==install['graph_sha256']
    assert (root/'graph-before-install.raw.json').read_bytes()==(root/'graph-after-install.raw.json').read_bytes()
    before_install=load(root,'q01-status-before-install.json')
    after_install=load(root,'q01-status-after-install.json')
    for key in ('active_revision','run_id','state','processes','run_id_state'):
        assert before_install[key]==after_install[key]
    assert digest(root/'saved-graph.raw.json')==load(root,'status-running.json')['saved_revision']
    cleanup=load(root,'q01-cleanup-proof.json')
    assert cleanup['errors']==[] and cleanup['survivors']==[]
    assert cleanup['stop_confirmed'] is True
    cleanup_status=load(root,'q01-cleanup-status-after.json')
    assert cleanup_status['processes']==[] and cleanup_status['state']=='stopped'
    assert cleanup_status['terminal']['outcome']=='confirmed'
    assert set(cleanup['pids'])=={item['id'] for item in
                                 load(root,'q01-status-new-running.json')['processes']}
    graph=load(root,'graph.json')
    simulator=graph['cores']['eos']['layers']['process']['groups']['mujoco']['nodes']['simulator']
    assert simulator['package']=={'id':'island/mujoco-sim','version':'0.1.1'}
    home=(root/'home-path.txt').read_text().strip()
    socket_path=str(Path(home)/'run/mujoco.sock')
    assert simulator['settings']['socket']==socket_path
    for group,node in (('camera','front'),('base','drive'),('manipulator','arm')):
        adapter=graph['cores']['eos']['layers']['hardware']['groups'][group]['nodes'][node]['adapter']
        assert adapter['package']==simulator['package']
        assert adapter['settings']['socket_path']==socket_path
        assert adapter['settings']['device_id']==adapter['settings']['simulator_node']
    invalid = load(root,'q01-invalid-apply.json')
    assert invalid['http_status']==422 and not invalid['reply']['ok']
    diagnostic = invalid['reply']['data']['diagnostics'][0]
    assert diagnostic['code']=='edge_fixed_route'
    assert diagnostic['path']=='/cores/egia/layers/io/groups/link/nodes/gateway/inputs/admitted'
    pending = load(root,'q01-logs-pending.json')
    first = load(root,'q01-logs-first-failure.json')
    assert pending['scope']['kind']==first['scope']['kind']=='manager_session'
    assert pending['scope']['run_id']==first['scope']['run_id']
    assert pending['anchor']['state']=='pending'
    anchor = first['anchor']['event']
    assert first['anchor']['state']=='found' and anchor['event_code']=='deployment_preflight_failed'
    assert anchor['fields']=={'diagnostic_code':diagnostic['code'],
                              'diagnostic_path':diagnostic['path']}
    assert load(root,'q01-preapply-state.json')['saved_graph_sha256']==load(root,'q01-install-state.json')['graph_sha256']
    assert load(root,'q01-stale-apply.json')['http_status']==409
    draft = load(root,'q01-options-draft.json')
    saved = load(root,'q01-options-saved.json')
    revision = load(root,'status-running.json')['saved_revision']
    assert draft['graph_revision'] is None and saved['graph_revision']==revision
    for node in ('eos/hardware/camera/front','eos/hardware/base/drive',
                 'eos/hardware/manipulator/arm'):
        rows = {r['setting']:r for r in draft['setting_options'] if r['node_path']==node}
        device = rows['adapter.settings.device_id']['selected']
        assert rows['adapter.settings.simulator_node']['selected']==device
        assert any(o['value']==device and o['available']
                   for o in rows['adapter.settings.device_id']['options'])
    unavailable=load(root,'q01-options-unavailable.json')
    camera=next(r for r in unavailable['setting_options']
                if r['node_path']=='eos/hardware/camera/front'
                and r['setting']=='adapter.settings.device_id')
    assert camera['selected']=='ghost-device'
    assert any(o['value']=='ghost-device' and not o['available'] for o in camera['options'])
    unavailable_apply=load(root,'q01-unavailable-apply.json')
    assert unavailable_apply['http_status']==422
    assert any(d['code']=='mujoco_device_unavailable' and
               d['path']=='/cores/eos/layers/hardware/groups/camera/nodes/front/adapter/settings/device_id'
               for d in unavailable_apply['reply']['data']['diagnostics'])
    old = load(root,'q01-logs-old-unfiltered.json')
    changed = load(root,'q01-logs-run-changed.json')
    assert old['scope']['run_id']==load(root,'status-running.json')['run_id']
    assert changed['scope']['run_id']==load(root,'q01-status-new-running.json')['run_id']
    assert any(d['code']=='log_run_changed' for d in changed['diagnostics'])
    mismatch = load(root,'q01-logs-filter-mismatch.json')
    assert mismatch['http_status']==400
    assert any(d['code']=='log_cursor_filter_mismatch'
               for d in mismatch['reply']['data']['diagnostics'])
    browser = load(root,'browser-dom-observation.json')
    result = load(root,'browser-result.json')
    assert browser['active_revision']==revision and browser['invoke_enabled_before_click']
    assert browser['binding']=='eos/dispatcher/base/base-velocity'
    assert browser['target']=='eos/hardware/base/drive'
    assert browser['owner_status_after_click']==result['result']['status']=='Completed'
    assert browser['command_id']==result['result']['command_id']
    records = [json.loads(path.read_text())['body'] for path in (root/'journal').glob('*.rec')]
    matches = [r for r in records if r['command_id']==browser['command_id']]
    assert len(matches)==1 and matches[0]['phase']=='terminal'
    assert matches[0]['result']['status']=='Completed'
    admission = matches[0]['operation']['identity']['admission']
    assert admission['run_id']==old['scope']['run_id']
    assert admission['active_revision']==revision
    assert admission['binding_path']==browser['binding']
    assert admission['ingress']=='private_bridge'
    assert admission['authority_role']=='operator_control'
    assert matches[0]['node_path']==browser['target']
    identity=matches[0]['operation']['identity']
    assert identity['kind']=='movement' and identity['primitive']=='base.velocity'
    assert identity['target']=={'hardware':{'node_path':browser['target']}}
    assert json.loads(matches[0]['signature'])[2]['Base']['BodyVelocity']=={
        'x_m_s':0.1,'y_m_s':0.0,'yaw_rad_s':0.0,'valid_for_ms':250}
    gaps=load(root,'q01-log-correlation-gaps.json')
    assert isinstance(gaps,list)
    derived_gaps=[]
    for label,command_id in (('mission',load(root,'q01-summary.json')['old_operation_id']),
                             ('browser',browser['command_id'])):
        pages=correlated_log_pages(root,label,command_id,old['scope']['run_id'],revision)
        lines=[line for page in pages for line in page['lines']]
        if not lines:
            derived_gaps.append({'source':label,'correlation_id':command_id,
                                 'reason':'no_matching_unified_log_line'})
        if label=='mission':
            assert any(line['event_code']=='egia_proposal_created' and
                       line['fields']['operation_id']==command_id for line in lines)
    assert gaps==derived_gaps,'reported correlation gaps differ from archived log pages'
    # Reuse the strict scoped verifier for fault, same-manager Restart,
    # owner events, raw journal and confirmed final Stop predicates.
    from verify_q01_restart import main as verify_restart
    previous = sys.argv
    try:
        sys.argv=['verify_q01_restart.py',str(root)]
        verify_restart()
    finally:
        sys.argv=previous
    if not main_only:
        assert not gaps,'correlated owner log coverage is incomplete'
        matrix = load(root,'q01-matrix.json')
        required = {'camera','g06-r03','p01-p03-u01','p07-u03','r06-a03','migration'}
        assert set(matrix)==required
        from verify_q01_cases import verify_case
        for name, entry in matrix.items():
            case = root/'cases'/name
            assert case.is_dir() and entry['manifest_sha256']==digest(case/'evidence.sha256.json')
            assert load(case,'evidence.sha256.json')==files(case), name
            assert entry['selected_pins']==selected_pins
            if selection:
                assert entry['selection_sha256'] == selection_sha256, name
                verify_case_build_bytes(name,case,entry,build_receipt,case_receipt)
            verify_case(name,case,selected_pins,selection=selection)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive',type=Path)
    parser.add_argument('--seal',action='store_true',help='write checksum manifest after replay')
    parser.add_argument('--main-only',action='store_true',help='verify main flow while matrix remains open')
    parser.add_argument('--expect-selection',type=Path,
                        help='reviewer-owned absolute selected-q03 JSON file')
    parser.add_argument('--expect-selection-sha256',
                        help='reviewer-recorded SHA-256 of canonical selection bytes')
    parser.add_argument('--expect-build-receipt',type=Path,
                        help='reviewer-owned immutable selected build receipt')
    parser.add_argument('--expect-build-receipt-sha256',
                        help='reviewer-recorded SHA-256 of canonical build receipt bytes')
    parser.add_argument('--expect-case-receipt',type=Path,
                        help='reviewer-owned six-case byte receipt')
    parser.add_argument('--expect-case-receipt-sha256',
                        help='reviewer-recorded SHA-256 of canonical case receipt bytes')
    for owner in ('isos', 'eos', 'egia', 'ui', 'mujoco'):
        parser.add_argument(f'--expect-{owner}', dest=f'expect_{owner}',
                            help=f'external selected {owner} integration commit')
    args = parser.parse_args()
    assert bool(args.expect_selection) == bool(args.expect_selection_sha256), \
        'external selection path and SHA-256 must be supplied together'
    selection = (load_selection(args.expect_selection, args.expect_selection_sha256)
                 if args.expect_selection else None)
    assert bool(args.expect_build_receipt) == bool(args.expect_build_receipt_sha256), \
        'external build receipt path and SHA-256 must be supplied together'
    if selection is None:
        assert args.expect_build_receipt is None, 'build receipt requires selection'
    build_receipt = (load_build_receipt(args.expect_build_receipt,
                     args.expect_build_receipt_sha256,args.expect_selection_sha256)
                     if args.expect_build_receipt else None)
    assert bool(args.expect_case_receipt) == bool(args.expect_case_receipt_sha256), \
        'external case receipt path and SHA-256 must be supplied together'
    if selection is None:
        assert args.expect_case_receipt is None, 'case receipt requires selection'
    case_receipt = (load_case_receipt(args.expect_case_receipt,
                    args.expect_case_receipt_sha256,args.expect_selection_sha256,
                    args.expect_build_receipt_sha256)
                    if args.expect_case_receipt else None)
    root = args.archive.resolve()
    expected_pins = {owner: getattr(args, f'expect_{owner}')
                     for owner in ('isos', 'eos', 'egia', 'ui', 'mujoco')}
    if not args.main_only:
        assert all(expected_pins.values()), 'full Q01 gate requires five external owner pins'
    elif not any(expected_pins.values()):
        expected_pins = None
    else:
        assert all(expected_pins.values()), 'provide all five owner pins together'
    if expected_pins is not None:
        for owner, pin in expected_pins.items():
            assert len(pin) == 40 and all(c in '0123456789abcdef' for c in pin), owner
    if args.seal:
        seal(root)
    verify(root,main_only=args.main_only,expected_pins=expected_pins,
           selection=selection,selection_sha256=args.expect_selection_sha256,
           build_receipt=build_receipt,case_receipt=case_receipt)
    print('Q01_MAIN_BOUNDARIES_VERIFIED_WITH_OPEN_GAPS' if args.main_only
          else 'Q01_FULL_VERIFIED')


if __name__=='__main__':
    main()
