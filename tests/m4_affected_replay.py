#!/usr/bin/env python3
"""Selected M4 affected-boundary replay on exact merged owner checkouts.

This is an M4 affected-boundary replay. A03 restart/latch fault injection is
qualified separately from this clean-home mission and browser run.
"""
import argparse
import asyncio
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time
import zipfile
from urllib.request import Request, urlopen
from urllib.error import HTTPError

from q01_selection import (check_harness, load_selection, selected_pins,
                           replay_home, verify_checkout, verify_ui_build_inputs)

PINS = {
    'isos': '2bdf6ea0ce8dbf72ee73e563d0151364c93ff0c0',
    'eos': '1b850f56b84b79d1cdc4fcab3ca83999e01ca520',
    'egia': '0c0c76708627468bda9ca8eab1bc43a575515c19',
    'ui': '6afd5bdc84a4a3aa2ca73a93342a09d7067d1cc1',
}


def args():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('isos-root', 'eos-root', 'egia-root', 'isos-bin', 'eos-bin-dir',
                 'bundle', 'mujoco-root', 'ui-dist', 'egia-python', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--egia-install-prefix', type=Path,
                        help='exercise a CMake/wheel installed EGIA managed child')
    parser.add_argument('--browser-release-file', type=Path,
                        help='pause after readiness until this file is created for a live browser replay')
    parser.add_argument('--browser-pause-seconds', type=int, default=300,
                        help='maximum browser pause at readiness')
    parser.add_argument('--mission', action='store_true',
                        help='run one supervised EGIA model-driven mission through the fixed route')
    parser.add_argument('--recovery-negative', action='store_true',
                        help='after baseline Stop, qualify new-run old-ID opacity and selected latch/fault behavior')
    parser.add_argument('--isos-commit',
                        help='exact IsOS source commit for a reviewed owner candidate replay')
    parser.add_argument('--q01-restart', action='store_true',
                        help='scoped Q01 model-fault and explicit new-run Restart replay')
    parser.add_argument('--q01-full', action='store_true',
                        help='fresh archive install, invalid Apply, options and cursor evidence')
    parser.add_argument('--package-asset', type=Path,
                        help='exact MuJoCo archive for --q01-full')
    parser.add_argument('--eos-commit')
    parser.add_argument('--egia-commit')
    parser.add_argument('--ui-commit',
                        help='exact IsosUI source commit for the selected replay')
    parser.add_argument('--selection',type=Path,
                        help='external reviewer-owned selected-q03 JSON')
    parser.add_argument('--expect-selection-sha256',
                        help='reviewer-recorded digest of selection JSON')
    parser.add_argument('--mujoco-source-root',type=Path,
                        help='MuJoCo Git checkout for selected-q03 preflight')
    result = parser.parse_args()
    if result.egia_install_prefix:
        prefix = result.egia_install_prefix.resolve()
        assert prefix.is_dir() and not (prefix/'src/egia').exists(), \
            'EGIA installed prefix must not contain source checkout modules'
        assert Path(os.path.abspath(result.egia_python)).is_relative_to(prefix), \
            'installed EGIA interpreter must be inside the selected prefix'
    assert bool(result.selection) == bool(result.expect_selection_sha256), \
        'selection and reviewer-recorded SHA-256 must be supplied together'
    result.frozen_selection = (load_selection(result.selection,
                                             result.expect_selection_sha256)
                               if result.selection else None)
    if result.frozen_selection:
        assert result.q01_full and result.mujoco_source_root, \
            'selected-q03 requires full replay and MuJoCo source checkout'
        pins = selected_pins(result.frozen_selection)
        for owner in ('isos','eos','egia','ui'):
            flag = getattr(result, f'{owner}_commit')
            assert flag is None or flag == pins[owner], \
                f'{owner} CLI commit conflicts with external selection'
            setattr(result, f'{owner}_commit', pins[owner])
    else:
        for owner in ('isos','eos','egia','ui'):
            if getattr(result, f'{owner}_commit') is None:
                setattr(result, f'{owner}_commit', PINS[owner])
    return result


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_installed_egia(prefix, python, home, out):
    """Tie the managed code roots and interpreter to one installed wheel."""
    wheels = list((prefix/'wheels').glob('egia-0.1.0-*.whl'))
    assert len(wheels) == 1, wheels
    wheel = wheels[0]
    manifest = json.loads((prefix/'q03-wheel-payload.json').read_text())
    assert manifest['wheel_sha256'] == sha(wheel)
    files = manifest['files']
    assert files and all(name.startswith(('egia/','island/')) for name in files)
    with zipfile.ZipFile(wheel) as archive:
        packaged = {name:hashlib.sha256(archive.read(name)).hexdigest()
                    for name in archive.namelist()
                    if name.startswith(('egia/','island/')) and not name.endswith('/')}
    assert files == packaged, 'wheel payload manifest differs from wheel'
    purelib = subprocess.check_output([str(python),'-B','-c',
        'import sysconfig; print(sysconfig.get_paths()["purelib"])'],
        cwd=home,env={'PATH':os.environ['PATH'],'PYTHONDONTWRITEBYTECODE':'1'},text=True).strip()
    assert Path(purelib).is_relative_to(prefix)
    for name, digest in files.items():
        for root in (prefix/'lib/island',Path(purelib)):
            path=root/name
            assert path.is_file() and sha(path) == digest, (path,digest)
    env={'PATH':os.environ['PATH'],'ISLAND_PREFIX':str(prefix),'ISLAND_MODE':'system',
         'ISLAND_ETC':str(prefix/'etc/island'),'ISLAND_SHARE':str(prefix/'share/island'),
         'ISLAND_CONFIG':str(prefix/'etc/island/island.toml'),
         'EGIA_ETC':str(prefix/'etc/island/egia'),
         'PYTHONPATH':str(prefix/'lib/island')+':'+str(prefix/'libexec/island'),
         'PYTHONDONTWRITEBYTECODE':'1'}
    probe=subprocess.run([str(python),'-B','-c',
        'import egia,island,json; print(json.dumps({"egia":egia.__file__,"island":island.__file__}))'],
        cwd=home,env=env,text=True,capture_output=True,check=True)
    modules=json.loads(probe.stdout)
    assert all(Path(value).resolve().is_relative_to(prefix/'lib/island')
               for value in modules.values()), modules
    save(out,'q03-egia-installed-wheel.json',{'wheel':str(wheel),
         'wheel_sha256':sha(wheel),'payload_manifest_sha256':sha(prefix/'q03-wheel-payload.json'),
         'files':files,'venv_purelib':purelib,'imports':modules})


def pin(root, expected, *, harness=False):
    actual = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
    if harness:
        subprocess.run(['git', '-C', str(root), 'merge-base', '--is-ancestor', expected, actual], check=True)
        subprocess.run(['git', '-C', str(root), 'diff', '--quiet', expected, actual,
                        '--', 'crates', 'src', 'bin', 'share'], check=True)
    else:
        assert actual == expected, (root, actual, expected)
        dirty = subprocess.check_output(['git', '-C', str(root), 'status', '--porcelain', '--untracked-files=no'], text=True)
        assert not dirty, (root, dirty)


def save(out, name, value):
    (out / name).write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def write_selected_isos_config(home: Path, isos_root: Path) -> Path:
    source = (isos_root / 'config/isos.toml').read_text()
    assert '\n[vision_evidence]' not in source, 'selected IsOS config already selects Vision'
    config = home / 'config/isos.toml'
    config.write_text(source +
        '\n[vision_evidence]\ngate_path = "eos/interface/gate/front-preview"\n')
    return config


def cleanup_owned_run(out, api_port, running, serve):
    """Ask the owner to Stop before terminating either CLI parent."""
    observed={'stop_attempted':False,'stop_confirmed':False,'pids':{},
              'survivors':[],'errors':[]}
    for name in ('status-running.json','q01-status-new-running.json'):
        path=out/name
        if path.is_file():
            for item in json.loads(path.read_text()).get('processes',[]):
                observed['pids'][item['id']]=item['pid']
    if serve.poll() is None:
        try:
            current=api(api_port,'process-status',timeout=45)
            save(out,'q01-cleanup-status-before.json',current)
            for item in current.get('processes',[]):
                observed['pids'][item['id']]=item['pid']
            if current['state']=='running':
                observed['stop_attempted']=True
                status,reply=api(api_port,'process-control','POST',
                    {'schema_version':1,'operation':'stop',
                     'revision':current['active_revision']},checked=False,timeout=90)
                save(out,'q01-cleanup-stop-response.json',
                     {'http_status':status,'reply':reply})
                observed['stop_confirmed']=status==200 and reply.get('ok') and \
                    (reply.get('data') or {}).get('terminal',{}).get('outcome')=='confirmed'
                if not observed['stop_confirmed']:
                    observed['errors'].append('owner_stop_not_confirmed')
            else:
                observed['stop_confirmed']=current['state']=='stopped' and \
                    (current.get('terminal') or {}).get('outcome')=='confirmed'
                if observed['pids'] and not observed['stop_confirmed']:
                    observed['errors'].append('owner_terminal_not_confirmed')
            after=api(api_port,'process-status',timeout=45)
            save(out,'q01-cleanup-status-after.json',after)
            if after['state']!='stopped' or \
               (after.get('terminal') or {}).get('outcome')!='confirmed':
                observed['errors'].append('owner_terminal_status_not_confirmed')
            if after['processes']:
                observed['errors'].append('owner_status_has_children')
        except Exception as error:
            observed['errors'].append(f'owner_cleanup_unavailable:{type(error).__name__}:{error}')
    elif running is not None and running.poll() is None:
        observed['errors'].append('manager_unavailable_with_run_cli_alive')
    deadline=time.monotonic()+30
    while time.monotonic()<deadline and any(Path(f'/proc/{pid}').exists()
                                           for pid in observed['pids'].values()):
        time.sleep(0.1)
    observed['survivors']=[{'id':name,'pid':pid} for name,pid in observed['pids'].items()
                           if Path(f'/proc/{pid}').exists()]
    if observed['survivors']:
        observed['errors'].append('owned_children_still_live')
    save(out,'q01-cleanup-proof.json',observed)
    return observed

def api(isos_port, path, method='GET', data=None, *, checked=True, timeout=180):
    request = Request(f'http://127.0.0.1:{isos_port}/api/isos/v1/{path}',
                      data=None if data is None else json.dumps(data).encode(), method=method,
                      headers={'Content-Type':'application/json','Origin':f'http://127.0.0.1:{isos_port}'})
    try:
        with urlopen(request, timeout=timeout) as response:
            status, body = response.status, json.load(response)
    except HTTPError as error:
        status, body = error.code, json.loads(error.read())
    if checked:
        assert status == 200 and body['ok'], (method, path, status, body)
        return body['data']
    return status, body


def negative(api_port, saved_revision, graph, out):
    """Reject edits at their exact edge paths without modifying saved bytes."""
    import copy
    cases = []
    for label, mutation, expected in (
        ('third-route-qos', ('cores','egia','layers','cognition','groups','decision','nodes','planner','inputs','snapshot','priority'), 'edge_qos_forbidden'),
        ('fixed-safety-route', ('cores','egia','layers','io','groups','link','nodes','gateway','inputs','admitted'), 'edge_fixed_route'),
    ):
        candidate = copy.deepcopy(graph)
        target = candidate
        for part in mutation[:-1]:
            target = target[part]
        target[mutation[-1]] = 1 if label == 'third-route-qos' else {'node':'egia/governance/safety/gate','output':'admission'}
        status, reply = api(api_port, 'node-graph/validate', 'POST',
                            {'schema_version':1,'graph':candidate}, checked=False)
        assert status == 200 and reply['ok'], (label,status,reply)
        diagnostics = reply['data']['diagnostics']
        assert not reply['data']['valid'] and any(item['code'] == expected for item in diagnostics), (label,diagnostics)
        cases.append({'case':label,'diagnostics':diagnostics})
        assert api(api_port,'node-graph')['revision'] == saved_revision
    save(out,'negative-edges.json',cases)

async def exercise(grpc_port, grant_path, revision, api_port):
    from egia.core.link.client import EosClient, LinkPrimitiveError
    from egia.lib.config import GatewayConfig
    os.environ['EGIA_EOS_CONTROL_GRANT_FILE'] = str(grant_path)
    client = EosClient(GatewayConfig(host='127.0.0.1', grpc_port=grpc_port), backend='default', expected_revision=revision)
    try:
        discovery = await client.initialize()
        assert client.active_revision == revision
        assert discovery.health in {'ready', 'degraded'}
        print('M4_DISCOVERY', {'robot_id': discovery.robot_id, 'devices': len(discovery.devices), 'primitives': len(client.primitives), 'revision_match': True}, flush=True)
        stale = EosClient(GatewayConfig(host='127.0.0.1', grpc_port=grpc_port), backend='default', expected_revision='0'*64)
        try:
            try:
                await stale.initialize()
                raise AssertionError('stale revision initialized')
            except LinkPrimitiveError:
                print('M4_STALE_REVISION_REJECTED', True, flush=True)
        finally:
            await stale.aclose()
        ack = await client.call_primitive('base.velocity', 'drive', {
            'linear_x_m_s': 0.1, 'linear_y_m_s': 0.0, 'angular_z_rad_s': 0.0, 'valid_for_ms': 100
        }, command_id=f'm4-live-move-{time.time_ns()}')
        print('M4_MOVEMENT', {'id': ack.command_id, 'status': ack.status.value, 'accepted': ack.accepted}, flush=True)
        stop_ack = await client.call_primitive('robot.stop', '', {}, command_id=f'm4-live-stop-{time.time_ns()}')
        print('M4_ROBOT_STOP', {'id': stop_ack.command_id, 'status': stop_ack.status.value, 'accepted': stop_ack.accepted}, flush=True)
        stopped = await asyncio.to_thread(api, api_port, 'process-control', 'POST',
                                          {'schema_version':1,'operation':'stop','revision':revision})
        try:
            await client.initialize(force=True)
            raise AssertionError('disconnected EOS falsely rediscovered')
        except LinkPrimitiveError:
            assert client.active_revision is None
            print('M4_DISCONNECT_FENCED', True, flush=True)
        return {'movement': {'id':ack.command_id,'status':ack.status.value,'accepted':ack.accepted},
                'stop': {'id':stop_ack.command_id,'status':stop_ack.status.value,'accepted':stop_ack.accepted}}, stopped
    finally:
        await client.aclose()

async def exercise_mission(port, correlation):
    import websockets
    import socket
    deadline = time.monotonic() + 30
    while True:
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=1):
                break
        except OSError:
            assert time.monotonic() < deadline, f'EGIA northbound listener {port} did not bind'
            await asyncio.sleep(0.2)
    frames = []
    async with websockets.connect(f'ws://127.0.0.1:{port}/api/aether/egia/missions/stream') as stream:
        frames.append(json.loads(await asyncio.wait_for(stream.recv(), timeout=10)))
        async with websockets.connect(f'ws://127.0.0.1:{port}/api/aether/egia/missions/command') as command_ws:
            await command_ws.send(json.dumps({'operation':'create','order':'move base forward',
                                              'priority_band':'high','correlation_id':correlation}))
            created = json.loads(await asyncio.wait_for(command_ws.recv(), timeout=10))
        assert created['ok'] and created['correlation_id'] == correlation, created
        mission_id = created['mission']['mission_id']
        mission_deadline = time.monotonic() + 150
        while True:
            remaining = mission_deadline - time.monotonic()
            assert remaining > 0, f'mission {mission_id} did not reach a terminal state'
            frame = json.loads(await asyncio.wait_for(stream.recv(), timeout=min(90, remaining)))
            frames.append(frame)
            matches = [item for item in frame.get('missions', []) + frame.get('history', [])
                       if item.get('mission_id') == mission_id]
            if matches and matches[0].get('state') in {'succeeded','failed','cancelled'}:
                return {'command':created,'frames':frames,'terminal':matches[0]}

def main():
    a = args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    if a.frozen_selection:
        selected = a.frozen_selection
        for name, root in (('isos',a.isos_root),('eos',a.eos_root),
                           ('egia',a.egia_root),('ui',a.ui_dist.parents[1]),
                           ('mujoco',a.mujoco_source_root)):
            verify_checkout(selected,name,root)
        check_harness(selected,Path(__file__).parent)
        verify_ui_build_inputs(selected,a.ui_dist.parents[1])
        assert sha(a.bundle/'release.json')==selected['package']['release_sha256']
        assert sha(a.package_asset)==selected['package']['asset_sha256']
        save(out,'q03-selected-selection.json',selected)
        save(out,'q03-selection-sha256.json',
             {'profile':'selected-q03','sha256':a.expect_selection_sha256})
    if a.q01_full:
        def on_term(signum, _frame):
            save(out,'q01-harness-signal.json',{'signal':signum,'phase':'interrupted',
                                                'observed_unix_s':time.time()})
            raise KeyboardInterrupt('Q01 replay received SIGTERM')
        signal.signal(signal.SIGTERM,on_term)
    save(out, 'command-argv.json', sys.argv)
    assert not a.q01_restart or (a.mission and not a.recovery_negative)
    assert not a.q01_full or (a.q01_restart and a.package_asset and a.browser_release_file)
    pin(a.isos_root, a.isos_commit)
    pin(a.eos_root, a.eos_commit, harness=not bool(a.frozen_selection))
    pin(a.egia_root, a.egia_commit)
    pin(a.ui_dist.parents[1], a.ui_commit)
    if a.q01_full:
        assert a.eos_bin_dir.resolve() == (a.eos_root/'target/debug').resolve(), \
            'Q01 EOS binaries must be built in the pinned source worktree'
        cargo = shutil.which('cargo') or str(Path.home()/'.cargo/bin/cargo')
        assert Path(cargo).is_file(), 'Cargo executable unavailable for selected source build'
        with (out/'eos-selected-build.log').open('w') as build_log:
            subprocess.run([cargo,'build','--locked','--workspace','--bins'],
                           cwd=a.eos_root,stdout=build_log,stderr=subprocess.STDOUT,check=True)
        binaries=('aether-runtime','aether-profile','aether-http-bridge','aether-config-server')
        save(out,'q01-eos-build.json',{
            'source_commit':a.eos_commit,
            'source_head':subprocess.check_output(['git','-C',str(a.eos_root),'rev-parse','HEAD'],text=True).strip(),
            'crates_tree_oid':subprocess.check_output(['git','-C',str(a.eos_root),'rev-parse',
                                                     f'{a.eos_commit}:crates'],text=True).strip(),
            'command':'cargo build --locked --workspace --bins',
            'cargo_version':subprocess.check_output([cargo,'--version'],text=True).strip(),
            'binary_sha256':{name:sha(a.eos_bin_dir/name) for name in binaries}})
        assert a.isos_bin.resolve()==(a.isos_root/'target/debug/isos').resolve(), \
            'Q01 IsOS executable must be built in the pinned source checkout'
        with (out/'isos-selected-build.log').open('w') as build_log:
            subprocess.run([cargo,'build','--locked','--bin','isos'],
                           cwd=a.isos_root,stdout=build_log,stderr=subprocess.STDOUT,check=True)
        save(out,'q01-isos-build.json',{
            'source_commit':a.isos_commit,
            'source_head':subprocess.check_output(['git','-C',str(a.isos_root),
                                                  'rev-parse','HEAD'],text=True).strip(),
            'source_tree_oid':subprocess.check_output(['git','-C',str(a.isos_root),
                                                      'rev-parse',f'{a.isos_commit}^{{tree}}'],
                                                      text=True).strip(),
            'cargo_lock_sha256':sha(a.isos_root/'Cargo.lock'),
            'command':'cargo build --locked --bin isos',
            'cargo_version':subprocess.check_output([cargo,'--version'],text=True).strip(),
            'binary_sha256':sha(a.isos_bin)})
    stable_bin = out/'bin/isos'
    stable_bin.parent.mkdir()
    shutil.copy2(a.isos_bin, stable_bin)
    assert sha(stable_bin) == sha(a.isos_bin)
    sys.path.insert(0, str(a.isos_root / 'tests'))
    sys.path.insert(0, str((a.egia_install_prefix.resolve()/'lib/island')
                           if a.egia_install_prefix else (a.egia_root/'src')))
    from real_home import await_condition, command, port
    home = replay_home(f'm4e-{os.getpid()}')
    assert not home.exists()
    (out/'home-path.txt').write_text(str(home)+'\n')
    if a.egia_install_prefix:
        verify_installed_egia(a.egia_install_prefix.resolve(),a.egia_python,out,out)
    model_stub = None
    mission_port = None
    egia_product = a.egia_install_prefix.resolve() if a.egia_install_prefix else a.egia_root
    egia_etc = egia_product/('etc/island/egia' if a.egia_install_prefix else 'etc/egia')
    egia_defaults = egia_etc
    if a.mission:
        from m4_model_stub import ModelStub
        from real_home import port
        model_stub = ModelStub().start()
        # IsOS owns the generated northbound binding and currently writes 16060.
        # Read its selected output after Apply before connecting to EGIA.
        egia_defaults = home/'egia-defaults'
        shutil.copytree(egia_etc, egia_defaults)
        models_path = egia_defaults/'models.toml'
        models = models_path.read_text()
        assert models.count('http://127.0.0.1:18011/v1') == 2
        models_path.write_text(models.replace('http://127.0.0.1:18011/v1', model_stub.endpoint))
    env = dict(os.environ, ISOS_HOME=str(home), ISOS_HTTP_PORT=str(port()), ISOSUI_PORT=str(port()),
               ISOS_AETHER_BIN_DIR=str(a.eos_bin_dir), ISOSUI_DIST_DIR=str(a.ui_dist), MUJOCO_ROOT=str(a.mujoco_root),
               ISOS_EGIA_BIN_DIR=str(egia_product/'bin'), ISOS_EGIA_DEFAULTS_DIR=str(egia_defaults),
               ISOS_EGIA_FACTORY_DEFAULTS_DIR=str(egia_product/'share/defaults/egia'),
               EGIA_PYTHON=str(a.egia_python), ISLAND_PREFIX=str(egia_product))
    if a.egia_install_prefix:
        env.update(ISLAND_MODE='system', ISLAND_ETC=str(egia_product/'etc/island'),
                   ISLAND_SHARE=str(egia_product/'share/island'),
                   ISLAND_CONFIG=str(egia_product/'etc/island/island.toml'),
                   EGIA_ETC=str(egia_product/'etc/island/egia'),
                   ISLAND_CACHE=str(home/'data/cache/default'),
                   ISLAND_LOG=str(home/'data/logs/default'),
                   ISLAND_RUN=str(home/'run'),
                   ISLAND_TMP=str(home/'tmp'),
                   ISOS_PHASE_TIMING='1')
        for inherited in ('PYTHONPATH', 'ISLAND_LIB', 'ISLAND_LIBEXEC', 'ISLAND_PYTHON'):
            env.pop(inherited, None)
        # The replay driver also imports the installed EGIA client for governed
        # Stop/recovery assertions. Give its resource loader the same prefix.
        for name in ('ISLAND_MODE','ISLAND_PREFIX','ISLAND_ETC','ISLAND_SHARE',
                     'ISLAND_CONFIG','EGIA_ETC','ISLAND_CACHE','ISLAND_LOG',
                     'ISLAND_RUN','ISLAND_TMP'):
            os.environ[name] = env[name]
    env.pop('ISOS_CONFIG', None)
    api_port = int(env['ISOS_HTTP_PORT'])
    grpc_port, bridge_port, config_port = port(), port(), port()
    if a.q01_full:
        ui_root = a.ui_dist.parents[1]
        ui_env = dict(os.environ, ISOSUI_API_ORIGIN=f'http://127.0.0.1:{api_port}',
                      ISOSUI_EOS_API_ORIGIN=f'http://127.0.0.1:{bridge_port}')
        with (out/'ui-rebuild.log').open('w') as build_log:
            subprocess.run(['npm','run','build:local'],cwd=ui_root,env=ui_env,
                           stdout=build_log,stderr=subprocess.STDOUT,check=True)
        ui_files = {path.relative_to(a.ui_dist).as_posix():sha(path)
                    for path in a.ui_dist.rglob('*') if path.is_file()}
        save(out,'q01-ui-rebuild.json',{
            'source_commit':a.ui_commit,
            'command':f'ISOSUI_API_ORIGIN=http://127.0.0.1:{api_port} '
                      f'ISOSUI_EOS_API_ORIGIN=http://127.0.0.1:{bridge_port} npm run build:local',
            'isos_api_origin':f'http://127.0.0.1:{api_port}',
            'eos_api_origin':f'http://127.0.0.1:{bridge_port}',
            'node_version':subprocess.check_output(['node','--version'],text=True).strip(),
            'npm_version':subprocess.check_output(['npm','--version'],text=True).strip(),
            'files_sha256':ui_files})
        if a.frozen_selection:
            with (out/'q03-ui-hosted-build.log').open('w') as build_log:
                subprocess.run(['npm','run','build:hosted'],cwd=ui_root,env=ui_env,
                               stdout=build_log,stderr=subprocess.STDOUT,check=True)
            hosted_dist=ui_root/'dist/hosted'
            assert (hosted_dist/'index.html').is_file()
            shutil.copytree(hosted_dist,out/'ui-hosted-dist',symlinks=False)
            save(out,'q03-ui-hosted-build.json',{
                'source_commit':a.ui_commit,
                'command':'npm run build:hosted',
                'selection_sha256':a.expect_selection_sha256,
                'files_sha256':{p.relative_to(hosted_dist).as_posix():sha(p)
                                for p in hosted_dist.rglob('*') if p.is_file()},
                'build_log_sha256':sha(out/'q03-ui-hosted-build.log')})
    save(out,'selected-inputs.json',dict(PINS, isos=a.isos_commit, eos=a.eos_commit,
         egia=a.egia_commit, ui=a.ui_commit, isos_bin=str(stable_bin),
         isos_source_bin=str(a.isos_bin), isos_bin_sha256=sha(stable_bin), eos_runtime_sha256=sha(a.eos_bin_dir/'aether-runtime'),
         isos_source_tree_oid=subprocess.check_output(['git','-C',str(a.isos_root),'rev-parse',
                                                      f'{a.isos_commit}^{{tree}}'],text=True).strip(),
         isos_cargo_lock_sha256=sha(a.isos_root/'Cargo.lock'),
         egia_launcher_sha256=sha(egia_product/'bin/egia'),
         egia_config_sha256=sha(egia_product/'bin/egia-config'),
         mujoco_release_sha256=sha(a.bundle/'release.json'),
         ui_index_sha256=sha(a.ui_dist/'index.html'),
         grpc_port=grpc_port,bridge_port=bridge_port,api_port=api_port,mission_port=mission_port))
    if a.q01_full:
        selected = out/'selected-binaries'
        selected.mkdir()
        for binary in ('aether-runtime','aether-profile','aether-http-bridge','aether-config-server'):
            shutil.copy2(a.eos_bin_dir/binary,selected/binary)
        shutil.copy2(egia_product/'bin/egia',selected/'egia')
        shutil.copy2(egia_product/'bin/egia-config',selected/'egia-config')
        if a.frozen_selection:
            shutil.copy2(stable_bin,selected/'isos')
        shutil.copytree(a.ui_dist,out/'ui-dist',symlinks=True)
        harness=out/'harness'
        harness.mkdir()
        for name in ('m4_affected_replay.py','m4_model_stub.py','q01_restart_replay.py',
                     'q01_full_replay.py','verify_q01_restart.py','verify_q01.py',
                     'verify_q01_cases.py','q01_migration_replay.py',
                     'verify_q01_migration.py','q01_tamper.py'):
            shutil.copy2(Path(__file__).parent/name,harness/name)
        if a.frozen_selection:
            for name in a.frozen_selection['harness']:
                shutil.copy2(Path(__file__).parent/Path(name).name,
                             harness/Path(name).name)
            save(out,'q03-build-receipt.json',{
                'schema_version':1,
                'profile':'selected-q03',
                'selection_sha256':a.expect_selection_sha256,
                'binaries_sha256':{p.name:sha(p) for p in selected.iterdir()},
                'ui_local_sha256':{p.relative_to(out/'ui-dist').as_posix():sha(p)
                                   for p in (out/'ui-dist').rglob('*') if p.is_file()},
                'ui_hosted_sha256':{p.relative_to(out/'ui-hosted-dist').as_posix():sha(p)
                                    for p in (out/'ui-hosted-dist').rglob('*') if p.is_file()},
                'build_logs_sha256':{name:sha(out/name) for name in
                    ('eos-selected-build.log','isos-selected-build.log',
                     'ui-rebuild.log','q03-ui-hosted-build.log')},
                'package':a.frozen_selection['package']})
    setup = [('init',('init','--mode','dev'))]
    if a.q01_full:
        from q01_full_replay import archive_package, install_and_check
        archive_package(out, a.bundle, a.package_asset)
        setup.append(('pkg-install', ('pkg','install','--manifest',str(a.bundle/'release.json'),
                    '--asset',str(a.package_asset),'--allow-unapproved')))
    else:
        setup.insert(0, ('pkg-link', ('pkg','link','island/mujoco-sim','0.1.1',str(a.bundle),'--manifest',str(a.bundle/'release.json'))))
    setup.append(('version-check',('version','check')))
    before_install = None
    status_before_install = None
    for name, command_args in setup:
        if name == 'pkg-install':
            before_install = (home/'config/deployments/default.nodes.json').read_bytes()
            (out/'graph-before-install.raw.json').write_bytes(before_install)
            status_before_install = json.loads(command(stable_bin, env, 'status', '--json').stdout)
            save(out,'q01-status-before-install.json',status_before_install)
        result = command(stable_bin, env, *command_args)
        (out/f'{name}.stdout').write_text(result.stdout)
        (out/f'{name}.stderr').write_text(result.stderr)
    if a.q01_full:
        product_source = a.egia_root/'etc/island.toml'
        authored = product_source.read_text()
        assert authored.count('"../Island-eos/share"') == 1
        selected_eos_share = os.path.relpath(a.eos_root.resolve()/'share',
                                             a.egia_root.resolve())
        assert not Path(selected_eos_share).is_absolute()
        assert (a.egia_root.resolve()/selected_eos_share).resolve() == \
            (a.eos_root.resolve()/'share')
        selected_product = home/'config/selected-island.toml'
        selected_product.write_text(authored.replace('"../Island-eos/share"',
                                                    f'"{selected_eos_share}"'))
        env['ISLAND_CONFIG'] = str(selected_product)
        os.environ['ISLAND_CONFIG'] = str(selected_product)
        (out/'q01-island-source.toml').write_bytes(product_source.read_bytes())
        (out/'q01-island-selected.toml').write_bytes(selected_product.read_bytes())
        save(out,'q01-island-selection.json',{
            'egia_source_root':str(a.egia_root.resolve()),
            'eos_source_root':str(a.eos_root.resolve()),
            'authored_sha256':sha(product_source),
            'selected_sha256':sha(selected_product),
            'selected_eos_share':selected_eos_share})
    if a.q01_full:
        install_and_check(out, home, stable_bin, env, a.package_asset, a.bundle,
                          before_install, status_before_install)
        (out/'graph-after-install.raw.json').write_bytes(
            (home/'config/deployments/default.nodes.json').read_bytes())
        if a.frozen_selection:
            config=write_selected_isos_config(home,a.isos_root)
            env['ISOS_CONFIG']=str(config)
            (out/'q03-selected-isos.toml').write_bytes(config.read_bytes())
    serve_log = (out/'serve.log').open('w')
    run_log = (out/'run.log').open('w')
    serve = subprocess.Popen([str(stable_bin),'serve'], env=env, cwd=home if a.egia_install_prefix else None,
                             stdout=serve_log, stderr=subprocess.STDOUT, text=True)
    running = None
    hosted_server = None
    try:
        await_condition(lambda: (home/'run/manager.sock').exists())
        if a.q01_full:
            from q01_full_replay import verify_served_ui
            verify_served_ui(out,int(env['ISOSUI_PORT']),api_port,bridge_port)
            if a.frozen_selection:
                from q01_full_replay import start_hosted_ui, verify_hosted_ui
                hosted_port=port()
                hosted_server=start_hosted_ui(out/'ui-hosted-dist',hosted_port)
                verify_hosted_ui(out,hosted_port)
        graph = api(api_port,'node-graph')['graph']
        values={'scene':'pick-place','socket':str(home/'run/mujoco.sock'),
                'camera_device':'cam-0','base_device':'base-0','manipulator_device':'arm-0'}
        preview = api(api_port,'node-sets/preview','POST',{
            'package':{'id':'island/mujoco-sim','version':'0.1.1'},
            'set_id':'sim-hardware','graph':graph,'values':values})
        graph = preview['graph']
        if a.q01_full:
            from q01_full_replay import before_apply, after_apply
            save(out,'q01-node-set-preview.json',preview)
        graph['cores']['eos']['layers']['process']['groups']['mujoco']['nodes']['simulator']['settings']['env']={'MUJOCO_GUI':'0'}
        if a.frozen_selection:
            graph['cores']['eos']['layers']['interface']['groups']['gate']['nodes'][
                'front-preview']['inputs']['frame']['frequency_hz']=1.0
        components = graph['cores']['isos']['layers']['process']['groups']['component']['nodes']
        components['eos']['settings']['args'] += ['--grpc-port',str(grpc_port)]
        for node, chosen in [('bridge',bridge_port),('config',config_port)]:
            components[node]['settings']['args'][-1]=f'127.0.0.1:{chosen}'
            components[node]['settings']['ready_tcp']=f'127.0.0.1:{chosen}'
        fixture = json.loads((a.isos_root/'tests/fixtures/egia_stage_reader/valid_egia.json').read_text())
        graph['cores']['egia'] = fixture['cores']['egia']
        graph['cores']['egia']['layers']['io']['groups']['link']['nodes']['gateway']['settings']['gateway']['port'] = grpc_port
        graph['cores']['egia']['layers']['evidence']['groups']['perception']['nodes']['admission']['inputs']['resource'].update(priority=-2, frequency_hz=20)
        graph['cores']['egia']['layers']['cognition']['groups']['context']['nodes']['builder']['inputs']['observations'].update(priority=3, frequency_hz=10)
        egia_dependency = 'bridge' if a.frozen_selection else 'eos'
        components['egia'] = {'id':'egia','node_type':'isos.process.component@v0.1',
          'label':'EGIA agent','description':'A02 selected process replay','capabilities':['process'],
          'settings':{'role':'egia','args':[], 'depends_on':[
              f'cores/isos/layers/process/groups/component/nodes/{egia_dependency}']}}
        saved = api(api_port,'node-graph')
        if a.q01_full:
            before_apply(out, home, api_port, saved, graph)
        negative(api_port,saved['revision'],graph,out)
        validation = api(api_port,'node-graph/validate','POST',{'schema_version':1,'graph':graph})
        assert validation['valid'], validation
        routes = [edge for edge in validation['edges'] if edge.get('route_class') == 'egia_local' and edge.get('routed')]
        assert len(routes) == 2 and all(edge['qos_eligible'] for edge in routes), routes
        applied = api(api_port,'node-graph','PUT',{'schema_version':1,'revision':saved['revision'],'graph':graph})
        revision = applied['revision']
        saved_graph = home/'config/deployments/default.nodes.json'
        assert sha(saved_graph) == revision
        if a.q01_full:
            after_apply(out, home, api_port, revision, graph)
        (out/'saved-graph.raw.json').write_bytes(saved_graph.read_bytes())
        leaf_root = home/'data/generated/default/v2'/revision
        selection = json.loads((home/'data/generated/default/selection.json').read_text())
        generation = selection['entries'][revision]['generation']
        overlay = json.loads((leaf_root/generation/'egia/stages.json').read_text())
        if a.mission:
            import tomllib
            mission_port = tomllib.loads((leaf_root/generation/'egia/egia.toml').read_text())['server']['port']
        assert overlay['revision'] == revision
        expected_overlay = json.dumps({'revision':revision,'version':'0.1.0','cores':{'egia':graph['cores']['egia']}},sort_keys=True,separators=(',',':')).encode()+b'\n'
        assert (leaf_root/generation/'egia/stages.json').read_bytes() == expected_overlay
        (out/'egia-stages.overlay.json').write_bytes(expected_overlay)
        if a.egia_install_prefix:
            manifest = json.loads((leaf_root/generation/'manifest.json').read_text())
            chain = manifest['egia_chain']
            assert chain['mode'] == 'installed' and chain['prefix'] == str(egia_product)
            assert chain['interpreter'] == str(a.egia_python)
            assert chain['binding']['PYTHONPATH'] == ''
            assert chain['binding']['ISLAND_LIB'] == str(egia_product/'lib/island')
            assert chain['binding']['ISLAND_LIBEXEC'].startswith(str(egia_product/'libexec'))
            assert chain['code'] and all(Path(entry['root']).is_relative_to(egia_product)
                                         for entry in chain['code'])
            save(out,'q03-egia-installed-chain.json',chain)
        save(out,'graph.json',graph); save(out,'validation.json',validation); save(out,'selection.json',selection)
        running=subprocess.Popen([str(stable_bin),'run'],env=env,
                                 cwd=home if a.egia_install_prefix else None,
                                 stdout=run_log,stderr=subprocess.STDOUT,text=True)
        status=await_condition(lambda: (s if s['state']=='running' and {'eos','egia','config','bridge'} <= {p['id'] for p in s['processes']} else None)
                               if (s:=api(api_port,'process-status')) else None,
                               timeout=300 if a.egia_install_prefix else 180)
        assert status['active_revision'] == revision
        save(out,'status-running.json',status)
        egia_pid = next(p['pid'] for p in status['processes'] if p['id']=='egia')
        egia_env = Path(f'/proc/{egia_pid}/environ').read_bytes().split(b'\0')
        egia_argv = [part.decode() for part in Path(f'/proc/{egia_pid}/cmdline').read_bytes().split(b'\0') if part]
        state_root = home/'data/state/default'
        assert '--state-root' in egia_argv and egia_argv[egia_argv.index('--state-root')+1] == str(state_root), egia_argv
        assert state_root.is_dir() and state_root.stat().st_mode & 0o777 == 0o700
        save(out,'egia-managed-launch.json',{'pid':egia_pid,'argv':egia_argv,
              'state_root':str(state_root),'mode':'0700'})
        grant_path = home/'run/control-grants/default'/status['run_id']/'egia-control.grant'
        assert any(value == f'EGIA_EOS_CONTROL_GRANT_FILE={grant_path}'.encode() for value in egia_env)
        assert grant_path.stat().st_mode & 0o777 == 0o600
        if a.egia_install_prefix:
            child_env = {item.split(b'=',1)[0].decode():item.split(b'=',1)[1].decode()
                         for item in egia_env if b'=' in item}
            child_pythonpath = child_env.get('PYTHONPATH','').split(':')
            assert child_pythonpath == [str(egia_product/'lib/island'),
                                        str(egia_product/'libexec/island')], child_pythonpath
            assert str(a.egia_python) in egia_argv, egia_argv
            child_cwd = Path(f'/proc/{egia_pid}/cwd').resolve()
            assert child_cwd == home.resolve(), child_cwd
            assert all(not child_cwd.is_relative_to(source) for source in
                       (a.egia_root,a.eos_root,a.isos_root,a.ui_dist.parents[1],
                        a.mujoco_source_root) if source is not None)
            for name in ('EGIA_PYTHON','ISLAND_PREFIX','ISLAND_LIB','ISLAND_LIBEXEC'):
                assert child_env[name] == chain['binding'][name], (name,child_env.get(name))
            for name, path in {'ISLAND_CACHE':home/'data/cache/default',
                               'ISLAND_LOG':home/'data/logs/default',
                               'ISLAND_RUN':home/'run',
                               'ISLAND_TMP':home/'tmp'}.items():
                assert child_env[name] == str(path), (name,child_env.get(name))
            save(out,'q03-egia-installed-child.json',{'pid':egia_pid,'argv':egia_argv,
                'cwd':str(Path(f'/proc/{egia_pid}/cwd').resolve()),
                'binding':{name:child_env[name] for name in
                           ('EGIA_PYTHON','ISLAND_PREFIX','ISLAND_LIB','ISLAND_LIBEXEC','PYTHONPATH',
                            'ISLAND_CACHE','ISLAND_LOG','ISLAND_RUN','ISLAND_TMP')}})
        print('M4_SUPERVISED_GRANT_AND_OVERLAY', {'egia_pid':egia_pid,'revision_match':True,'grant_path_only':True},flush=True)
        current = api(api_port,'logs?view=current&source=all&level=info')
        save(out,'logs-current-running.json',current)
        assert current['scope']['run_id'] == status['run_id'], current
        if a.q01_full:
            from q01_full_replay import running_logs
            running_logs(out, api_port, status)
        if a.mission:
            assert mission_port is not None and model_stub is not None
            mission = asyncio.run(exercise_mission(mission_port, f'm4-{status["run_id"]}'))
            save(out,'mission.json',mission)
            save(out,'model-requests.json',model_stub.snapshot())
            assert mission['terminal']['state'] == 'succeeded', mission['terminal']
            assert any(step['actions'] for step in mission['terminal']['document']['plan']['steps']), 'mission settled without a governed action'
        if a.browser_release_file is not None:
            ready={'url':f"http://127.0.0.1:{env['ISOSUI_PORT']}/en/isos",
                                          'api_port':api_port,'bridge_port':bridge_port,
                                          'run_id':status['run_id'],'revision':revision}
            if a.frozen_selection:
                ready['hosted_urls']={language:f'http://127.0.0.1:{hosted_port}/{language}/isos'
                                      for language in ('en','cn')}
                ready['isos_home']=str(home)
            save(out,'browser-ready.json',ready)
            deadline = time.monotonic() + a.browser_pause_seconds
            while not a.browser_release_file.exists():
                assert time.monotonic() < deadline, 'browser replay release timed out'
                time.sleep(0.2)
        if a.q01_full:
            from q01_full_replay import correlated_logs
            correlated_logs(out, home, api_port, mission, status['run_id'])
        if a.q01_restart:
            from q01_restart_replay import run_restart
            result, stop, status = run_restart(out=out, home=home, api_port=api_port,
                grpc_port=grpc_port, mission_port=mission_port, revision=revision,
                first_status=status, first_mission=mission, model_stub=model_stub,
                first_grant=grant_path, selected_egia_config=leaf_root/generation/'egia',
                factory_egia_defaults=egia_product/'share/defaults/egia')
        else:
            result, stop = asyncio.run(exercise(grpc_port, grant_path, revision, api_port))
        save(out,'primitive-result.json',result); save(out,'stop-response.json',stop)
        stopped = await_condition(lambda: (s if s['state']!='running' else None) if (s:=api(api_port,'process-status')) else None, timeout=30)
        save(out,'status-stopped.json',stopped)
        assert stop['terminal']['outcome'] == 'confirmed', stop
        child_pids = {item['id']: item['pid'] for item in status['processes']}
        await_condition(lambda: all(not Path(f'/proc/{pid}').exists() for pid in child_pids.values()), timeout=10)
        save(out,'reap-proof.json',{'run_id':status['run_id'],'pids':child_pids,
                                   'all_absent':True,'capture_phase':'immediately_after_stop',
                                   'observed_unix_s':time.time()})
        current = api(api_port,'logs?view=current&source=all&level=info')
        save(out,'logs-current-stopped.json',current)
        raw = home/'data/logs/default/events.log'
        if raw.is_file(): (out/'events.raw.jsonl').write_bytes(raw.read_bytes())
        journal = home/'data/journal/default'
        if journal.is_dir():
            archived = out/'journal'
            archived.mkdir()
            manifest = {}
            for record in sorted(journal.glob('*.rec')):
                shutil.copy2(record, archived/record.name)
                manifest[record.name] = sha(record)
            save(out,'journal-sha256.json',manifest)
        print('M4_STOP', {'terminal':stop['terminal']['outcome'],'state':stopped['state']},flush=True)
        if a.recovery_negative:
            assert a.browser_release_file is None, 'use a separate fresh home for recovery negatives'
            assert a.mission, 'recovery negative requires a supervised mission operation'
            owner_events = [json.loads(line) for line in (out/'events.raw.jsonl').read_text().splitlines()]
            proposals = [item for item in owner_events
                         if item.get('source') == 'egia' and item.get('run_id') == status['run_id']
                         and item.get('event_code') == 'egia_proposal_created'
                         and item.get('fields', {}).get('mission_id') == mission['terminal']['mission_id']]
            assert len(proposals) == 1, 'expected one supervised mission proposal'
            mission_operation_id = proposals[0]['fields']['operation_id']
            running.wait(timeout=15)
            from m4_recovery_replay import run_in_session
            recovery = run_in_session(out=out/'a03-recovery', home=home, graph=graph,
                                      revision=revision, prior_operation_id=mission_operation_id,
                                      api_port=api_port, grpc_port=grpc_port, isos_bin=stable_bin,
                                      env=env, egia_root=a.egia_root, baseline_run_id=status['run_id'])
            save(out, 'a03-recovery-result.json', recovery)
        if a.browser_release_file is not None:
            assert (out/'browser-dom-observation.json').is_file() and (out/'browser-result.json').is_file(), \
                'browser result evidence is required when the browser pause is requested'
            if a.frozen_selection:
                assert (out/'q03-hosted-browser-observation.json').is_file(), \
                    'both hosted editor observations required'
                assert (out/'q03-browser-vision-observation.json').is_file(), \
                    'rendered selected Vision identity required'
                assert (out/'q03-browser-raw.json').is_file(), \
                    'raw browser DOM, response and Vision JPEG bytes required'
                from q03_hosted_origin import verify_hosted_origin
                hosted_origin_path=out/'q03-simulated-hosted-origin.json'
                assert hosted_origin_path.is_file(), \
                    'selected simulated hosted-Origin evidence required'
                hosted_origin_proof=verify_hosted_origin(
                    json.loads(hosted_origin_path.read_text()),
                    json.loads((out/'browser-ready.json').read_text()))
                save(out,'q03-hosted-origin-validation.json',{
                    'source_sha256':sha(hosted_origin_path),
                    'proof':hosted_origin_proof})
                from q01_browser_raw import verify_browser_raw
                receipt=json.loads((out/'q03-build-receipt.json').read_text())
                local_files={name:digest for name,digest in receipt['ui_local_sha256'].items()
                             if name in ('index.html','manifest.json') or
                             name.endswith(('.js','.css'))}
                hosted_files={name:digest for name,digest in receipt['ui_hosted_sha256'].items()
                              if name in ('index.html','manifest.json') or
                              name.endswith(('.js','.css'))}
                browser_proof=verify_browser_raw(out,local_files,hosted_files,revision,
                    a.frozen_selection['owners']['ui']['commit'])
                save(out,'q03-browser-raw-validation.json',{
                    'source_sha256':sha(out/'q03-browser-raw.json'),
                    'local':browser_proof['local'],
                    'hosted':browser_proof['hosted'],
                    'vision_jpeg_sha256':browser_proof['vision_jpeg_sha256']})
    finally:
        if hosted_server is not None:
            hosted_server.shutdown()
            hosted_server.server_close()
        cleanup = None
        if a.q01_full:
            cleanup=cleanup_owned_run(out,api_port,running,serve)
        if running is not None and running.poll() is None:
            running.send_signal(signal.SIGINT)
            try: running.wait(timeout=10)
            except subprocess.TimeoutExpired: running.kill(); running.wait()
        if serve.poll() is None:
            serve.send_signal(signal.SIGINT)
            try: serve.wait(timeout=10)
            except subprocess.TimeoutExpired: serve.kill(); serve.wait()
        serve_log.close(); run_log.close()
        # Preserve owner evidence even when an assertion stops the replay early.
        raw_events = home/'data/logs/default/events.log'
        if raw_events.is_file():
            (out/'events.raw.jsonl').write_bytes(raw_events.read_bytes())
        owner_stderr = home/'data/logs/default/owner/egia/stderr.log'
        if owner_stderr.is_file():
            (out/'egia-stderr.raw.log').write_bytes(owner_stderr.read_bytes())
        owner_log = home/'data/state/default/egia/egia.jsonl'
        if owner_log.is_file() and (out/'status-running.json').is_file():
            full_path = out/'egia-owner-full.raw.jsonl'
            full_path.write_bytes(owner_log.read_bytes())
            save(out, 'egia-owner-full-sha256.json', {'sha256': sha(full_path)})
            save(out, 'egia-owner-archive-provenance.json', {
                'capture': 'run finalizer', 'source': str(owner_log),
                'source_sha256': sha(owner_log), 'bytes': owner_log.stat().st_size})
            run_id = json.loads((out/'status-running.json').read_text())['run_id']
            selected_path = out/'egia-owner-run.raw.jsonl'
            with selected_path.open('w') as selected:
                for line in owner_log.open():
                    if f'"run_id": "{run_id}"' in line:
                        selected.write(line)
            save(out, 'egia-owner-run-sha256.json', {'sha256': sha(selected_path), 'run_id': run_id})
        if model_stub is not None:
            save(out,'model-requests-final.json',model_stub.snapshot())
            model_stub.close()
        if cleanup is not None and cleanup['errors'] and sys.exc_info()[0] is None:
            raise AssertionError(f'Q01 cleanup incomplete: {cleanup["errors"]}')
    print('Q01_RESTART_SCOPED_PASS' if a.q01_restart else 'M4_AFFECTED_REPLAY_PASS',flush=True)

if __name__ == '__main__':
    main()
