"""Additional owner observations for the full Q01 first-use replay.

The M4 harness supplies the supervised mission, browser pause, journal and
Restart sequence. These checks bind its setup to a real package install and
the first failed Apply in the same fresh manager session.
"""

import copy
import hashlib
import json
from pathlib import Path
import shutil
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from threading import Thread
from urllib.parse import urlencode
from urllib.request import urlopen

from m4_affected_replay import api, save, sha


def capture_selected_vision(out, home, run_id, revision, bridge_port, ui_port):
    import asyncio
    from q01_camera_replay import websocket_frame, inspection, vision_socket_paths
    run_root = home/'run/control-grants/default'/run_id
    binding = json.loads((run_root/'egia/vision-binding.json').read_text())
    assert binding['graph_revision'] == revision
    save(out,'q03-vision-binding.json',binding)
    gate = binding['gate_path']
    subscribe = {'op':'subscribe','gate_path':gate,
                 'revision':binding['projection_revision'],'target_fps':30,
                 'max_width_px':1280,'jpeg_quality':80}
    owner_grant = (run_root/'egia-vision.grant').read_text().strip()
    inspection_grant = (run_root/'vision-inspection.grant').read_text().strip()
    owner_socket, inspection_socket = vision_socket_paths(binding, home, run_id)
    async def sample():
        owner, browser = await asyncio.gather(
            websocket_frame('ws://localhost/api/eos/v2/vision/owner-ws',
                            subscribe,unix=owner_socket,
                            bearer=owner_grant),
            websocket_frame(f'ws://127.0.0.1:{bridge_port}/api/eos/v2/vision/ws',
                            subscribe,browser_origin=f'http://127.0.0.1:{ui_port}'))
        egia = await inspection(inspection_socket,
                                inspection_grant,
                                generation=owner[1]['gate_generation'])
        return owner,browser,egia
    identity = ('graph_revision','projection_revision','runtime_stream_id',
                'source_path','source_frame_id','gate_path','gate_generation',
                'render','jpeg_sha256')
    for _ in range(16):
        owner,browser,egia = asyncio.run(sample())
        if all(owner[1][key] == row[key] for key in identity
               for row in (browser[1],egia)):
            break
    else:
        raise AssertionError('selected Vision consumers did not share one generation')
    for label,frame in (('owner',owner),('bridge',browser)):
        handshake,metadata,jpeg=frame
        save(out,f'q03-vision-{label}.json',{'handshake':handshake,
            'metadata':metadata,'jpeg_length_bytes':len(jpeg),
            'jpeg_verified_sha256':hashlib.sha256(jpeg).hexdigest()})
    save(out,'q03-vision-egia.json',egia)


def start_hosted_ui(dist, port):
    """Serve hosted editors at owner routes and assets under /isosui/."""
    class Handler(SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=str(dist), **kwargs)

        def translate_path(self, path):
            route = path.split('?', 1)[0].split('#', 1)[0]
            if route in ('/en/isos', '/cn/isos'):
                suffix = 'index.html'
            else:
                assert route.startswith('/isosui/')
                suffix = route[len('/isosui/'):]
            target = (dist / suffix).resolve()
            assert target.is_relative_to(dist.resolve())
            return str(target)

        def log_message(self, format, *args):
            pass

    server = ThreadingHTTPServer(('127.0.0.1', port), Handler)
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server


def verify_hosted_ui(out, hosted_port):
    dist = out/'ui-hosted-dist'
    origin = f'http://127.0.0.1:{hosted_port}'
    served = {}
    for path in (dist/'index.html', *sorted(dist.rglob('*.js'))):
        relative = path.relative_to(dist).as_posix()
        route = '/en/isos' if relative == 'index.html' else f'/isosui/{relative}'
        with urlopen(f'{origin}{route}', timeout=10) as response:
            body = response.read()
        digest = hashlib.sha256(body).hexdigest()
        assert digest == sha(path), relative
        served[relative] = digest
    with urlopen(f'{origin}/cn/isos', timeout=10) as response:
        chinese_index = hashlib.sha256(response.read()).hexdigest()
    assert chinese_index == served['index.html']
    save(out,'q03-hosted-ui-served.json',{'origin':origin,
        'routes':{'en':'/en/isos','cn':'/cn/isos'},
        'served_sha256':served,'cn_index_sha256':chinese_index})


def verify_served_ui(out, ui_port, api_port, bridge_port):
    """Bind the served bytes to the selected build before any graph launch."""
    dist=out/'ui-dist'
    origin=f'http://127.0.0.1:{ui_port}'
    scripts=sorted(path for path in dist.rglob('*.js') if path.is_file())
    assert scripts
    payload=b'\n'.join(path.read_bytes() for path in scripts)
    assert f'http://127.0.0.1:{api_port}'.encode() in payload
    assert f'http://127.0.0.1:{bridge_port}'.encode() in payload
    served={}
    for path in [dist/'index.html',*scripts]:
        relative=path.relative_to(dist).as_posix()
        served_path='en/isos' if relative=='index.html' else relative
        with urlopen(f'{origin}/{served_path}',timeout=10) as response:
            body=response.read()
        assert hashlib.sha256(body).hexdigest()==sha(path),relative
        served[relative]=hashlib.sha256(body).hexdigest()
    save(out,'q01-ui-served.json',{'origin':origin,
         'isos_api_origin':f'http://127.0.0.1:{api_port}',
         'eos_api_origin':f'http://127.0.0.1:{bridge_port}',
         'served_sha256':served})


def archive_package(out, bundle, asset):
    assert asset.is_file()
    release = json.loads((bundle/'release.json').read_text())
    assert release['id'] == 'island/mujoco-sim' and release['version'] == '0.1.1'
    assert any(entry['sha256'] == sha(asset) for entry in release['assets'])
    package = out/'package-input'
    package.mkdir()
    for source, name in ((asset,'asset.tar.gz'), (bundle/'release.json','release.json')):
        shutil.copy2(source, package/name)
    save(out,'q01-package-input.json', {'source_commit':release['source_commit'],
         'asset_sha256':sha(package/'asset.tar.gz'),
         'release_sha256':sha(package/'release.json')})


def install_and_check(out, home, binary, env, asset, bundle, before, before_status):
    from real_home import command
    graph = home/'config/deployments/default.nodes.json'
    assert graph.read_bytes() == before, 'package install changed saved graph'
    after_status=json.loads(command(binary,env,'status','--json').stdout)
    for key in ('active_revision','run_id','state','processes','run_id_state'):
        assert after_status[key]==before_status[key],(key,before_status[key],after_status[key])
    save(out,'q01-status-after-install.json',after_status)
    listing = json.loads(command(binary, env, 'pkg', 'list', '--json').stdout)
    receipt = next(item for item in listing if item['id']=='island/mujoco-sim'
                   and item['version']=='0.1.1')
    source = home/'data/packages/island/mujoco-sim/0.1.1'
    assert receipt['schema_version']==2 and receipt['source_kind']=='local_candidate'
    assert receipt['asset_sha256']==sha(asset)
    assert receipt['release_sha256']==sha(source/'release.json')
    assert receipt['sidecar_sha256']==sha(source/'node-types.json')
    assert receipt['payload_sha256']
    installed = out/'package-installed'
    shutil.copytree(source,installed,symlinks=True)
    save(out,'q01-package-receipt.json',receipt)
    save(out,'q01-install-state.json',{'graph_sha256':hashlib.sha256(before).hexdigest(),
         'graph_unchanged':True,'active_revision':None,'run_id':None,'processes':[]})


def _options(out, api_port, graph, name, *, require_available=True):
    response = api(api_port,'node-options','POST',{'schema_version':1,'graph':graph})
    save(out,name,response)
    assert response['graph_revision'] is None
    rows = response['setting_options']
    assert rows
    for node in ('eos/hardware/camera/front','eos/hardware/base/drive',
                 'eos/hardware/manipulator/arm'):
        selected = {row['setting']:row for row in rows if row['node_path']==node}
        device = selected['adapter.settings.device_id']['selected']
        if require_available or node!='eos/hardware/camera/front':
            assert device == selected['adapter.settings.simulator_node']['selected']
        assert any(option['value']==device and
                   (option['available'] or not require_available)
                   for option in selected['adapter.settings.device_id']['options'])
    return response


def before_apply(out, home, api_port, saved, graph):
    _options(out,api_port,graph,'q01-options-draft.json')
    initial_status = api(api_port,'process-status')
    assert initial_status['active_revision'] is None and not initial_status['processes']
    pending = api(api_port,'logs?view=current&source=all')
    save(out,'q01-logs-pending.json',pending)
    assert pending['scope']['kind']=='manager_session'
    assert pending['anchor']['state']=='pending'
    candidate = copy.deepcopy(graph)
    candidate['cores']['egia']['layers']['io']['groups']['link']['nodes']['gateway']['inputs']['admitted'] = {
        'node':'egia/governance/safety/gate','output':'admission'}
    validation = api(api_port,'node-graph/validate','POST',
                     {'schema_version':1,'graph':candidate})
    assert not validation['valid']
    diagnostics = validation['diagnostics']
    assert any(item['code']=='edge_fixed_route' for item in diagnostics)
    save(out,'q01-invalid-validation.json',validation)
    status, reply = api(api_port,'node-graph','PUT',
                        {'schema_version':1,'revision':saved['revision'],'graph':candidate},checked=False)
    assert status==422 and not reply['ok']
    assert any(item['code']=='edge_fixed_route' for item in reply['data']['diagnostics'])
    save(out,'q01-invalid-apply.json',{'http_status':status,'reply':reply})
    current = api(api_port,'node-graph')
    assert current['revision']==saved['revision'] and current['graph']==saved['graph']
    assert api(api_port,'process-status')==initial_status
    first = api(api_port,'logs?view=current&source=all')
    save(out,'q01-logs-first-failure.json',first)
    assert first['scope']['kind']=='manager_session'
    assert first['anchor']['state']=='found'
    assert first['anchor']['event']['event_code']=='deployment_preflight_failed'
    assert first['anchor']['event']['fields']['diagnostic_code']=='edge_fixed_route'
    assert first['anchor']['event']['fields']['diagnostic_path']==diagnostics[0]['path']
    assert first['next_cursor'].startswith('lc1.')
    save(out,'q01-preapply-state.json',{'saved_revision':saved['revision'],
        'saved_graph_sha256':sha(home/'config/deployments/default.nodes.json'),
        'active_revision':initial_status['active_revision'],
        'run_id':initial_status['run_id'],'processes':initial_status['processes']})
    unavailable=copy.deepcopy(graph)
    unavailable['cores']['eos']['layers']['hardware']['groups']['camera']['nodes']['front']['adapter']['settings']['device_id']='ghost-device'
    options=_options(out,api_port,unavailable,'q01-options-unavailable.json',
                     require_available=False)
    row=next(row for row in options['setting_options'] if row['node_path']=='eos/hardware/camera/front'
             and row['setting']=='adapter.settings.device_id')
    assert row['selected']=='ghost-device'
    assert any(option['value']=='ghost-device' and not option['available'] for option in row['options'])
    code, rejected=api(api_port,'node-graph','PUT',
                      {'schema_version':1,'revision':saved['revision'],'graph':unavailable},checked=False)
    save(out,'q01-unavailable-apply.json',{'http_status':code,'reply':rejected})
    assert code==422 and any(item['code']=='mujoco_device_unavailable'
                             and item['path']=='/cores/eos/layers/hardware/groups/camera/nodes/front/adapter/settings/device_id'
                             for item in rejected['data']['diagnostics']), (code,rejected)
    assert sha(home/'config/deployments/default.nodes.json')==saved['revision']


def after_apply(out, home, api_port, revision, graph):
    actual = api(api_port,'node-graph')
    assert actual['revision']==revision and actual['graph']==graph
    status = api(api_port,'process-status')
    assert status['active_revision'] is None and not status['processes']
    options = api(api_port,'node-options')
    assert options['graph_revision']==revision
    save(out,'q01-options-saved.json',options)
    # Stale Apply must not alter saved bytes or start children.
    code, stale = api(api_port,'node-graph','PUT',
                      {'schema_version':1,'revision':'0'*64,'graph':graph},checked=False)
    assert code==409 and not stale['ok']
    save(out,'q01-stale-apply.json',{'http_status':code,'reply':stale})
    assert sha(home/'config/deployments/default.nodes.json')==revision
    assert api(api_port,'process-status')==status


def running_logs(out, api_port, status):
    current = api(api_port,'logs?view=current&source=all')
    save(out,'q01-logs-old-unfiltered.json',current)
    assert current['scope']['run_id']==status['run_id']
    assert current['anchor']['state']=='found'
    cursor = current['next_cursor']
    assert cursor.startswith('lc1.')
    status_code, mismatch = api(api_port,'logs?'+urlencode({'view':'current','source':'eos',
                    'cursor':cursor}),checked=False)
    save(out,'q01-logs-filter-mismatch.json',{'http_status':status_code,'reply':mismatch})
    assert status_code==400 and any(d['code']=='log_cursor_filter_mismatch'
                                   for d in mismatch['data']['diagnostics'])


def correlated_logs(out, home, api_port, mission, run_id):
    events=[json.loads(line) for line in (home/'data/logs/default/events.log').read_text().splitlines()]
    operation=[e['fields']['operation_id'] for e in events
               if e.get('event_code')=='egia_proposal_created' and e.get('run_id')==run_id
               and e.get('fields',{}).get('mission_id')==mission['terminal']['mission_id']]
    assert len(operation)==1
    browser_observation=load_json(out/'browser-dom-observation.json')
    browser=browser_observation['command_id']
    revision=browser_observation['active_revision']
    assert browser!=operation[0]
    gaps=[]
    for label,command_id in (('mission',operation[0]),('browser',browser)):
        cursor=None
        page_index=[]
        found=False
        # IsOS scans at most 64 KiB per filtered request. A page can contain
        # no matching lines while its cursor still advances through the run.
        for number in range(1, 513):
            query={'view':'current','source':'all','correlation_id':command_id}
            if cursor is not None:
                query['cursor']=cursor
            page=api(api_port,'logs?'+urlencode(query))
            name=(f'q01-logs-{label}-correlated.json' if number==1 else
                  f'q01-logs-{label}-correlated-page-{number:04d}.json')
            save(out,name,page)
            page_index.append({'file':name,'request_cursor':cursor})
            assert page['scope']=={'kind':'deployment_run','run_id':run_id,
                                   'active_revision':revision}, page['scope']
            assert page['anchor']['state']=='found', page['anchor']
            assert page['diagnostics']==[], page['diagnostics']
            assert all(line['correlation_id']==command_id for line in page['lines'])
            found |= bool(page['lines'])
            following=page['next_cursor']
            assert isinstance(following,str) and following.startswith('lc1.')
            if following==cursor:
                break
            cursor=following
        else:
            raise AssertionError(f'{label} correlated log cursor did not stabilize')
        save(out,f'q01-logs-{label}-correlated-pages.json',page_index)
        if not found:
            gaps.append({'source':label,'correlation_id':command_id,
                         'reason':'no_matching_unified_log_line'})
    save(out,'q01-log-correlation-gaps.json',gaps)


def load_json(path):
    return json.loads(path.read_text())
