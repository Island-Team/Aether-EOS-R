#!/usr/bin/env python3
"""Selected-pin IsOS → EOS → MuJoCo → EGIA camera boundary replay.

Requires already built exact selected binaries and a verified MuJoCo package
bundle. Retains only metadata and digests, never JPEG or grant bytes.
"""

import argparse
import asyncio
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import time

from m4_affected_replay import api, pin, save, sha, cleanup_owned_run
from verify_q01_camera import PINS, browser_after_follows
from q01_selection import check_harness, load_selection, replay_home, verify_checkout


def vision_socket_paths(binding: dict, home: Path, run_id: str,
                        deployment: str = 'default') -> tuple[Path, Path]:
    """Use the IsOS-selected endpoint, not the control-grant directory."""
    endpoint = binding['eos_endpoint']
    assert endpoint['transport'] == 'uds'
    assert binding['deployment_run_id'] == run_id
    owner = Path(endpoint['path'])
    name = hashlib.sha256(f'{deployment}\0{run_id}'.encode()).hexdigest()[:32]
    expected = home / 'run/v' / name / 'eos-vision-owner.sock'
    assert owner.is_absolute() and owner == expected
    return owner, owner.with_name('egia-vision-inspection.sock')


def owner_request(path: Path, request: dict) -> dict:
    with socket.socket(socket.AF_UNIX) as peer:
        peer.settimeout(3)
        peer.connect(str(path))
        peer.sendall(json.dumps(request, separators=(',', ':')).encode() + b'\n')
        data = bytearray()
        while not data.endswith(b'\n'):
            chunk = peer.recv(65536)
            assert chunk and len(data) + len(chunk) < 16 * 1024 * 1024
            data += chunk
    return json.loads(data)


def stale_test_request(path: Path, request: dict) -> dict:
    """Call the selected EGIA child's private, same-UID one-shot test socket."""
    with socket.socket(socket.AF_UNIX) as peer:
        peer.settimeout(3)
        peer.connect(str(path))
        peer.sendall(json.dumps(request, separators=(',', ':')).encode() + b'\n')
        data = bytearray()
        while not data.endswith(b'\n'):
            chunk = peer.recv(16384)
            assert chunk and len(data) + len(chunk) <= 16384
            data += chunk
    return json.loads(data)


def http_denial(path: Path, bearer: str) -> dict:
    with socket.socket(socket.AF_UNIX) as peer:
        peer.settimeout(3)
        peer.connect(str(path))
        request = (f'GET /api/eos/v2/vision/owner-ws HTTP/1.1\r\nHost: localhost\r\n'
                   f'Authorization: Bearer {bearer}\r\nConnection: close\r\n\r\n')
        peer.sendall(request.encode())
        chunks = []
        while True:
            chunk = peer.recv(4096)
            if not chunk:
                break
            chunks.append(chunk)
            assert sum(map(len, chunks)) <= 4096
        response = b''.join(chunks).decode()
    status = int(response.split(' ', 2)[1])
    return {'http_status': status, 'body': json.loads(response.split('\r\n\r\n', 1)[1])}


async def websocket_frame(uri: str, subscribe: dict, *, unix: Path | None = None,
                          bearer: str | None = None,
                          browser_origin: str | None = None) -> tuple[dict, dict, bytes]:
    import websockets
    headers = {'Authorization': f'Bearer {bearer}'} if bearer else None
    if unix is None:
        assert browser_origin is not None
        connection = websockets.connect(uri, origin=browser_origin,
                                         additional_headers=headers)
    else:
        connection = websockets.unix_connect(str(unix), uri=uri,
                                              additional_headers=headers)
    async with connection as ws:
        await ws.send(json.dumps(subscribe))
        handshake = json.loads(await asyncio.wait_for(ws.recv(), 5))
        assert handshake['type'] == 'subscribed', handshake
        payload = await asyncio.wait_for(ws.recv(), 15)
        assert isinstance(payload, bytes) and len(payload) >= 4
        length = struct.unpack('<I', payload[:4])[0]
        assert 0 < length <= 4096
        metadata = json.loads(payload[4:4 + length])
        jpeg = payload[4 + length:]
        assert jpeg.startswith(b'\xff\xd8') and jpeg.endswith(b'\xff\xd9')
        assert hashlib.sha256(jpeg).hexdigest() == metadata['jpeg_sha256']
        return handshake, metadata, jpeg


async def inspection(path: Path, bearer: str, *, generation: int) -> dict:
    import websockets
    async with websockets.unix_connect(str(path),
            uri='ws://localhost/api/aether/egia/vision/evidence',
            additional_headers={'Authorization': f'Bearer {bearer}'}) as ws:
        deadline = time.monotonic() + 25
        while True:
            snapshot = json.loads(await asyncio.wait_for(ws.recv(), 10))
            if snapshot['state'] == 'available' and snapshot['gate_generation'] >= generation:
                return snapshot
            assert time.monotonic() < deadline, snapshot


async def rejected_owner_subscription(path: Path, bearer: str, request: dict) -> dict:
    import websockets
    async with websockets.unix_connect(str(path),
            uri='ws://localhost/api/eos/v2/vision/owner-ws',
            additional_headers={'Authorization': f'Bearer {bearer}'}) as ws:
        await ws.send(json.dumps(request))
        reply = json.loads(await asyncio.wait_for(ws.recv(), 5))
        assert reply['type'] == 'error' and reply.get('reason_code')
        try:
            followup = await asyncio.wait_for(ws.recv(), 5)
        except websockets.exceptions.ConnectionClosed:
            followup = None
        assert followup is None and ws.close_code == 1008
        return {'request': request, 'reply': reply, 'close_code': ws.close_code,
                'binary_frames': 0}


async def consumer_snapshot(browser_uri: str, origin: str, subscribe: dict,
                            inspection_socket: Path, inspection_grant: str) -> dict:
    (handshake, browser, jpeg), egia = await asyncio.gather(
        websocket_frame(browser_uri, subscribe, browser_origin=origin),
        inspection(inspection_socket, inspection_grant, generation=0))
    return {'browser': dict(browser, jpeg_length_bytes=len(jpeg),
                            jpeg_verified_sha256=hashlib.sha256(jpeg).hexdigest(),
                            handshake=handshake), 'egia': egia}


async def browser_snapshot(browser_uri: str, origin: str, subscribe: dict) -> dict:
    handshake, metadata, jpeg = await websocket_frame(
        browser_uri, subscribe, browser_origin=origin)
    return dict(metadata, jpeg_length_bytes=len(jpeg),
                jpeg_verified_sha256=hashlib.sha256(jpeg).hexdigest(),
                handshake=handshake)


def consumer_identity(snapshot: dict) -> tuple:
    fields = ('graph_revision', 'projection_revision', 'runtime_stream_id',
              'source_path', 'source_frame_id', 'gate_path', 'gate_generation',
              'jpeg_sha256')
    return tuple(snapshot['browser'][key] for key in fields)


async def live_rejection_case(owner_socket: Path, owner_grant: str,
                              browser_uri: str, origin: str, subscribe: dict,
                              inspection_socket: Path, inspection_grant: str,
                              request: dict, expected_reason: str,
                              out: Path, archive_name: str) -> dict:
    attempts = []
    deadline = time.monotonic() + 20
    while len(attempts) < 12 and time.monotonic() < deadline:
        before = await consumer_snapshot(browser_uri, origin, subscribe,
                                         inspection_socket, inspection_grant)
        rejection = await rejected_owner_subscription(owner_socket, owner_grant,
                                                        request)
        after = await consumer_snapshot(browser_uri, origin, subscribe,
                                        inspection_socket, inspection_grant)
        assert rejection['reply']['reason_code'] == expected_reason, rejection
        before_identity = consumer_identity(before)
        after_identity = consumer_identity(after)
        stable = (before_identity == after_identity
                  and all(tuple(snapshot['egia'][key] for key in
                      ('graph_revision', 'projection_revision', 'runtime_stream_id',
                       'source_path', 'source_frame_id', 'gate_path',
                       'gate_generation', 'jpeg_sha256')) == before_identity
                          for snapshot in (before, after))
                  and before['egia']['accepted_count'] == after['egia']['accepted_count'])
        attempts.append({'before': before, 'rejection': rejection,
                         'after': after, 'consumers_unchanged': stable})
        result = {'expected_reason': expected_reason, 'attempts': attempts,
                  'stable_attempt_index': len(attempts) - 1 if stable else None}
        save(out, archive_name, result)
        if stable:
            return result
    raise AssertionError(f'live {expected_reason} did not find a stable consumer window')


def args():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('isos-root', 'eos-root', 'egia-root', 'isos-bin', 'eos-bin-dir',
                 'bundle', 'mujoco-root', 'egia-python', 'ui-dist', 'output'):
        p.add_argument('--' + name, required=True, type=Path)
    p.add_argument('--selection',type=Path)
    p.add_argument('--expect-selection-sha256')
    p.add_argument('--mujoco-source-root',type=Path)
    p.add_argument('--ui-root',type=Path)
    return p.parse_args()


def selected_build_receipts(a, out: Path) -> dict:
    """Freeze clean selected trees, lockfiles, build output and launched bytes."""
    snapshots = out / 'source-snapshot'
    snapshots.mkdir()
    receipts = {}
    for owner, root in (('isos', a.isos_root), ('eos', a.eos_root),
                        ('egia', a.egia_root)):
        assert subprocess.check_output(['git', '-C', str(root), 'status',
            '--porcelain'], text=True).strip() == '', owner
        commit = subprocess.check_output(['git', '-C', str(root), 'rev-parse',
            'HEAD'], text=True).strip()
        tree = subprocess.check_output(['git', '-C', str(root), 'rev-parse',
            'HEAD^{tree}'], text=True).strip()
        assert commit == (a.selection_data['owners'][owner]['commit']
                          if a.selection_data else PINS[owner])
        snapshot = snapshots / f'{owner}.tar'
        with snapshot.open('wb') as target:
            subprocess.run(['git', '-C', str(root), 'archive', '--format=tar',
                            'HEAD'], stdout=target, check=True)
        lock = 'uv.lock' if owner == 'egia' else 'Cargo.lock'
        (snapshots / f'{owner}-{lock}').write_bytes((root / lock).read_bytes())
        receipts[owner] = {'commit': commit, 'tree': tree,
            'snapshot_sha256': sha(snapshot), 'lock_sha256': sha(root / lock)}
    for owner, root, args in (
        ('isos', a.isos_root, ('build', '--locked', '--bin', 'isos')),
        ('eos', a.eos_root, ('build', '--locked', '--bins')),
    ):
        log = out / f'{owner}-selected-build.log'
        with log.open('wb') as target:
            subprocess.run(['cargo', *args], cwd=root,
                           stdout=target, stderr=subprocess.STDOUT, check=True)
        receipts[owner]['build_command'] = ['cargo', *args]
        receipts[owner]['build_log_sha256'] = sha(log)
    launcher = a.egia_root / 'bin/egia'
    (snapshots / 'egia-launcher').write_bytes(launcher.read_bytes())
    receipts['egia']['build_kind'] = 'selected-source-launcher'
    receipts['egia']['launcher_sha256'] = sha(launcher)
    receipts['isos']['binary_sha256'] = sha(a.isos_bin)
    receipts['eos']['binary_sha256'] = sha(a.eos_bin_dir / 'aether-runtime')
    receipts['egia']['binary_sha256'] = sha(launcher)
    binaries = out / 'selected-binaries'
    binaries.mkdir()
    selected = {'isos': a.isos_bin, 'egia': launcher,
        **{name: a.eos_bin_dir / name for name in ('aether-runtime',
            'aether-profile', 'aether-http-bridge', 'aether-config-server')}}
    for name, path in selected.items():
        shutil.copyfile(path, binaries / name)
        assert sha(binaries / name) == sha(path)
    receipts['selected_binary_sha256'] = {name: sha(binaries / name)
                                           for name in selected}
    save(out, 'source-build-receipts.json', receipts)
    return receipts


def main():
    a = args()
    assert bool(a.selection) == bool(a.expect_selection_sha256)
    a.selection_data = (load_selection(a.selection,a.expect_selection_sha256)
                        if a.selection else None)
    selected = a.selection_data
    pins = ({name:selected['owners'][name]['commit'] for name in PINS}
            if selected else PINS)
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    if selected:
        assert a.mujoco_source_root and a.ui_root
        for owner,root in (('isos',a.isos_root),('eos',a.eos_root),
                           ('egia',a.egia_root),('ui',a.ui_root),
                           ('mujoco',a.mujoco_source_root)):
            verify_checkout(selected,owner,root)
        check_harness(selected,Path(__file__).parent)
        assert sha(a.bundle/'release.json')==selected['package']['release_sha256']
        save(out,'selection-sha256.json',
             {'profile':'selected-q03','sha256':a.expect_selection_sha256})
    (out / 'negative').mkdir()
    save(out, 'command-argv.json', sys.argv)
    pin(a.isos_root, pins['isos'])
    pin(a.eos_root, pins['eos'])
    pin(a.egia_root, pins['egia'])
    assert a.ui_dist.is_dir()
    assert a.isos_bin == a.isos_root / 'target/debug/isos'
    assert a.eos_bin_dir == a.eos_root / 'target/debug'
    for binary in (a.isos_bin, *(a.eos_bin_dir / name for name in
                    ('aether-runtime', 'aether-profile', 'aether-http-bridge',
                     'aether-config-server'))):
        assert binary.is_file(), binary
    receipts = selected_build_receipts(a, out)
    sys.path.insert(0, str(a.isos_root / 'tests'))
    from real_home import await_condition, command, port
    home = replay_home(f'q01cam-{os.getpid()}')
    assert not home.exists()
    save(out, 'selected-inputs.json', {
        'source_commits': dict(pins, mujoco=json.loads(
            (a.bundle / 'release.json').read_text())['source_commit']),
        'source_roots': {'isos': str(a.isos_root.resolve()),
                         'eos': str(a.eos_root.resolve()),
                         'egia': str(a.egia_root.resolve())},
        'binary_sha256': {owner: receipts[owner]['binary_sha256']
                          for owner in pins},
        'environment': {'kind': 'selected-isos-mujoco-process',
                        'isos_home': str(home),
                        'mujoco_root': str(a.mujoco_root),
                        'ui_dist': str(a.ui_dist),
                        'egia_q01_stale_test_inherited': True,
                        'package_release_sha256': sha(a.bundle / 'release.json')},
    })
    source = Path(__file__).parent
    with (out / 'egia-stub-run.log').open('w') as log:
        subprocess.run([sys.executable, str(source / 'q01_egia_camera_probe.py'),
                        '--egia-root', str(a.egia_root), '--output',
                        str(out / 'egia-stub.json')], stdout=log,
                       stderr=subprocess.STDOUT, check=True)
    with (out / 'fifo-probe-run.log').open('w') as log:
        subprocess.run([sys.executable, str(source / 'q01_fifo_probe.py'),
                        '--eos-root', str(a.eos_root), '--archive',
                        str(out / 'fifo-probe'), '--expect-eos',pins['eos']], stdout=log,
                       stderr=subprocess.STDOUT, check=True)
    env = dict(os.environ, ISOS_HOME=str(home), ISOS_HTTP_PORT=str(port()),
               ISOSUI_PORT=str(port()), ISOS_AETHER_BIN_DIR=str(a.eos_bin_dir),
               ISOSUI_DIST_DIR=str(a.ui_dist),
               MUJOCO_ROOT=str(a.mujoco_root), ISOS_EGIA_BIN_DIR=str(a.egia_root / 'bin'),
               ISOS_EGIA_DEFAULTS_DIR=str(a.egia_root / 'etc/egia'),
               ISOS_EGIA_FACTORY_DEFAULTS_DIR=str(a.egia_root / 'share/defaults/egia'),
               EGIA_PYTHON=str(a.egia_python), ISLAND_PREFIX=str(a.egia_root),
               EGIA_Q01_STALE_TEST='1')
    api_port = int(env['ISOS_HTTP_PORT'])
    grpc_port, bridge_port, config_port = port(), port(), port()
    for label, argv in (('init', ('init', '--mode', 'dev')),
                        ('pkg-link', ('pkg', 'link', 'island/mujoco-sim', '0.1.1',
                                      str(a.bundle), '--manifest', str(a.bundle / 'release.json')))):
        result = command(a.isos_bin, env, *argv)
        (out / f'{label}.stdout').write_text(result.stdout)
        (out / f'{label}.stderr').write_text(result.stderr)
    cfg = home / 'config/isos.toml'
    assert cfg.parent.is_dir()
    cfg.write_text((a.isos_root / 'config/isos.toml').read_text() +
                   '\n[vision_evidence]\ngate_path = "eos/interface/gate/front-preview"\n')
    env['ISOS_CONFIG'] = str(cfg)
    (out / 'selected-isos.toml').write_bytes(cfg.read_bytes())
    product = home / 'config/selected-island.toml'
    product_source = (a.egia_root / 'etc/island.toml').read_text()
    assert product_source.count('"../Island-eos/share"') == 1
    selected_eos_share = os.path.relpath(a.eos_root.resolve() / 'share',
                                         a.egia_root.resolve())
    assert not Path(selected_eos_share).is_absolute()
    assert (a.egia_root.resolve() / selected_eos_share).resolve() == \
        (a.eos_root.resolve() / 'share').resolve()
    product.write_text(product_source.replace(
        '"../Island-eos/share"', f'"{selected_eos_share}"'))
    env['ISLAND_CONFIG'] = str(product)
    (out / 'selected-island.toml').write_bytes(product.read_bytes())
    save(out, 'selected-config-hashes.json', {
        'isos_config_sha256': sha(cfg), 'island_config_sha256': sha(product),
        'egia_product_source_sha256': sha(a.egia_root / 'etc/island.toml'),
        'resolved_egia_share': str((a.egia_root.resolve() / 'share').resolve()),
        'resolved_eos_share': str((a.egia_root.resolve() / selected_eos_share).resolve())})
    serve_log = (out / 'serve.log').open('w')
    run_log = (out / 'run.log').open('w')
    serve = subprocess.Popen([str(a.isos_bin), 'serve'], env=env,
                             stdout=serve_log, stderr=subprocess.STDOUT)
    running = None
    try:
        await_condition(lambda: (home / 'run/manager.sock').exists(), timeout=20)
        graph = api(api_port, 'node-graph')['graph']
        values = {'scene': 'pick-place', 'socket': str(home / 'run/mujoco.sock'),
                  'camera_device': 'cam-0', 'base_device': 'base-0',
                  'manipulator_device': 'arm-0'}
        preview = api(api_port, 'node-sets/preview', 'POST', {
            'package': {'id': 'island/mujoco-sim', 'version': '0.1.1'},
            'set_id': 'sim-hardware', 'graph': graph, 'values': values})
        graph = preview['graph']
        assert 'EGIA_Q01_STALE_TEST' not in json.dumps(graph)
        graph['cores']['eos']['layers']['process']['groups']['mujoco']['nodes']['simulator']['settings']['env'] = {'MUJOCO_GUI': '0'}
        gate = graph['cores']['eos']['layers']['interface']['groups']['gate']['nodes']['front-preview']
        gate['inputs']['frame']['frequency_hz'] = 1.0
        components = graph['cores']['isos']['layers']['process']['groups']['component']['nodes']
        components['eos']['settings']['args'] += ['--grpc-port', str(grpc_port)]
        for node, chosen in [('bridge', bridge_port), ('config', config_port)]:
            components[node]['settings']['args'][-1] = f'127.0.0.1:{chosen}'
            components[node]['settings']['ready_tcp'] = f'127.0.0.1:{chosen}'
        fixture = json.loads((a.isos_root / 'tests/fixtures/egia_stage_reader/valid_egia.json').read_text())
        graph['cores']['egia'] = fixture['cores']['egia']
        graph['cores']['egia']['layers']['io']['groups']['link']['nodes']['gateway']['settings']['gateway']['port'] = grpc_port
        components['egia'] = {'id': 'egia', 'node_type': 'isos.process.component@v0.1',
            'label': 'EGIA agent', 'description': 'Q01 selected Vision replay',
            'capabilities': ['process'], 'settings': {'role': 'egia', 'args': [],
            'depends_on': ['cores/isos/layers/process/groups/component/nodes/bridge']}}
        saved = api(api_port, 'node-graph')
        validation = api(api_port, 'node-graph/validate', 'POST',
                         {'schema_version': 1, 'graph': graph})
        save(out, 'validation.json', validation)
        assert validation['valid'], validation
        applied = api(api_port, 'node-graph', 'PUT',
                      {'schema_version': 1, 'revision': saved['revision'], 'graph': graph})
        revision = applied['revision']
        raw_graph = (home / 'config/deployments/default.nodes.json').read_bytes()
        assert hashlib.sha256(raw_graph).hexdigest() == revision
        (out / 'saved-graph.raw.json').write_bytes(raw_graph)
        running = subprocess.Popen([str(a.isos_bin), 'run'], env=env,
                                   stdout=run_log, stderr=subprocess.STDOUT)
        status = await_condition(lambda: (s if s['state'] == 'running' and
            {'eos', 'bridge', 'egia'} <= {p['id'] for p in s['processes']} else None)
            if (s := api(api_port, 'process-status')) else None, timeout=180)
        assert status['active_revision'] == revision
        save(out, 'status-running.json', status)
        run_root = home / 'run/control-grants/default' / status['run_id']
        binding = json.loads((run_root / 'egia/vision-binding.json').read_text())
        save(out, 'vision-binding.json', binding)
        eos_socket = home / 'run/default-eos.sock'
        source = binding['source_path']
        gate_path = binding['gate_path']
        attest = owner_request(eos_socket, {'op': 'vision_attest', 'gate_path': gate_path})
        save(out, 'vision-attest.json', attest)
        before = owner_request(eos_socket, {'op': 'route_status'})
        save(out, 'route-status-before.json', before)
        sub = {'op': 'subscribe', 'gate_path': gate_path,
               'revision': binding['projection_revision'], 'target_fps': 30,
               'max_width_px': 1280, 'jpeg_quality': 80}
        owner_grant = (run_root / 'egia-vision.grant').read_text().strip()
        inspection_grant = (run_root / 'vision-inspection.grant').read_text().strip()
        owner_socket, inspection_socket = vision_socket_paths(binding, home, status['run_id'])
        browser_uri = f'ws://127.0.0.1:{bridge_port}/api/eos/v2/vision/ws'
        owner_uri = 'ws://localhost/api/eos/v2/vision/owner-ws'
        async def paired_frames():
            return await asyncio.gather(
                websocket_frame(owner_uri, sub, unix=owner_socket, bearer=owner_grant),
                websocket_frame(browser_uri, sub,
                                browser_origin=f'http://127.0.0.1:{env["ISOSUI_PORT"]}'))
        identity = ('graph_revision', 'projection_revision', 'runtime_stream_id',
                    'source_path', 'source_frame_id', 'gate_path', 'gate_generation',
                    'render', 'jpeg_sha256')
        frame_attempts = []
        deadline = time.monotonic() + 20
        while len(frame_attempts) < 16 and time.monotonic() < deadline:
            (owner_handshake, owner_metadata, owner_jpeg), (
                browser_handshake, browser_metadata, browser_jpeg) = asyncio.run(paired_frames())
            observed = asyncio.run(inspection(inspection_socket, inspection_grant,
                                              generation=owner_metadata['gate_generation']))
            same = all(owner_metadata[key] == frame[key]
                       for frame in (browser_metadata, observed) for key in identity)
            eos_frame = {'changed': True,
                 'generation': owner_metadata['gate_generation'], 'metadata': owner_metadata,
                 'jpeg_length_bytes': len(owner_jpeg),
                 'jpeg_verified_sha256': hashlib.sha256(owner_jpeg).hexdigest(),
                 'handshake': owner_handshake}
            browser_frame = dict(browser_metadata,
                 jpeg_length_bytes=len(browser_jpeg),
                 jpeg_verified_sha256=hashlib.sha256(browser_jpeg).hexdigest(),
                 handshake=browser_handshake)
            comparison = {'owner_generation': owner_metadata['gate_generation'],
                 'browser_generation': browser_metadata['gate_generation'],
                 'egia_generation': observed['gate_generation'],
                 'same_identity': same}
            frame_attempts.append({'eos': eos_frame, 'browser': browser_frame,
                                   'egia': observed, 'comparison': comparison})
            save(out, 'frame-attempts.json', {'attempts': frame_attempts,
                 'selected_index': len(frame_attempts) - 1 if same else None})
            save(out, 'eos-frame.json', eos_frame)
            save(out, 'browser-frame.json', browser_frame)
            save(out, 'egia-observation.json', observed)
            save(out, 'frame-comparison.json', comparison)
            if same:
                break
        else:
            raise AssertionError('no same-generation three-consumer frame in bounded window')
        origin = f'http://127.0.0.1:{env["ISOSUI_PORT"]}'
        stale_test_socket = inspection_socket.with_name('egia-vision-stale-test.sock')
        await_condition(lambda: stale_test_socket.is_socket(), timeout=15)
        # A wrong binding and a prior stream may not consume the one-shot hook.
        # Capture the current accepted identity immediately before each request.
        base = {'schema_version': 1, 'op': 'replay_latest',
                'deployment_run_id': status['run_id'],
                'graph_revision': revision}
        def test_target(snapshot: dict) -> dict:
            return dict(base, **{key: snapshot[key] for key in
                ('runtime_stream_id', 'gate_generation', 'source_frame_id',
                 'jpeg_sha256')})
        current = asyncio.run(inspection(inspection_socket, inspection_grant,
                                         generation=0))
        wrong_binding = stale_test_request(stale_test_socket,
                            dict(test_target(current), graph_revision='f' * 64))
        save(out, 'negative/egia-test-wrong-revision.json',
             {'request': {'graph_revision': 'f' * 64}, 'response': wrong_binding,
              'before': current,
              'after': asyncio.run(inspection(inspection_socket, inspection_grant,
                                               generation=0))})
        current = asyncio.run(inspection(inspection_socket, inspection_grant,
                                         generation=0))
        old_stream = stale_test_request(stale_test_socket,
            dict(test_target(current), runtime_stream_id='00000000-0000-4000-8000-000000000000'))
        save(out, 'negative/egia-test-old-stream.json',
             {'request': {'runtime_stream_id': '00000000-0000-4000-8000-000000000000'},
              'response': old_stream, 'before': current,
              'after': asyncio.run(inspection(inspection_socket, inspection_grant,
                                               generation=0))})
        assert wrong_binding['reason_code'] == 'binding_stale', wrong_binding
        assert old_stream['reason_code'] == 'target_changed', old_stream
        attempts = []
        preflight_mismatches = []
        deadline = time.monotonic() + 20
        while len(attempts) + len(preflight_mismatches) < 12 and time.monotonic() < deadline:
            browser_before = asyncio.run(browser_snapshot(browser_uri, origin, sub))
            observed = asyncio.run(inspection(inspection_socket, inspection_grant,
                                              generation=0))
            browser_keys = ('graph_revision', 'projection_revision',
                            'runtime_stream_id', 'source_path', 'source_frame_id',
                            'gate_path', 'gate_generation', 'render', 'jpeg_sha256',
                            'jpeg_length_bytes')
            if any(browser_before[key] != observed[key] for key in browser_keys):
                preflight_mismatches.append({'browser': browser_before,
                                             'egia': observed})
                save(out, 'negative/egia-live-stale-generation.json',
                     {'preflight_mismatches': preflight_mismatches,
                      'attempts': attempts, 'selected_index': None})
                continue
            response = stale_test_request(stale_test_socket, test_target(observed))
            browser_after = asyncio.run(browser_snapshot(browser_uri, origin, sub))
            attempts.append({'request_identity': {key: observed[key] for key in
                ('runtime_stream_id', 'gate_generation', 'source_frame_id',
                 'jpeg_sha256')}, 'browser_before': browser_before,
                 'browser_after': browser_after, 'response': response})
            stable = (response.get('injected')
                      and all(browser_before[key] == response['before'][key]
                              == response['after'][key] for key in browser_keys)
                      and browser_after_follows(browser_before, browser_after))
            save(out, 'negative/egia-live-stale-generation.json',
                 {'preflight_mismatches': preflight_mismatches,
                  'attempts': attempts, 'selected_index': len(attempts) - 1
                  if stable else None})
            if response.get('injected'):
                assert stable, 'legitimate camera generation changed after one-shot injection'
                break
            assert response['reason_code'] == 'target_changed', response
        else:
            raise AssertionError('EGIA accepted identity changed during bounded duplicate attempts')
        assert response['reason_code'] == 'generation_stale', response
        assert not stale_test_socket.exists()
        wrong_sub = dict(sub, revision='f' * 64)
        save(out, 'negative/live-wrong-revision.json', asyncio.run(live_rejection_case(
            owner_socket, owner_grant, browser_uri, origin, sub,
            inspection_socket, inspection_grant, wrong_sub, 'revision_mismatch',
            out, 'negative/live-wrong-revision.json')))
        stale_sub = dict(sub, after_generation=owner_metadata['gate_generation'],
                         after_runtime_stream_id='00000000-0000-4000-8000-000000000000')
        save(out, 'negative/live-stale-cursor.json', asyncio.run(live_rejection_case(
            owner_socket, owner_grant, browser_uri, origin, sub,
            inspection_socket, inspection_grant, stale_sub, 'cursor_stale',
            out, 'negative/live-stale-cursor.json')))
        wrong_grant = http_denial(owner_socket, '0' * 64)
        save(out, 'negative/wrong-grant.json', wrong_grant)
        wrong_revision = owner_request(eos_socket, {'op': 'vision_frame',
             'gate_path': gate_path, 'revision': 'f' * 64,
             'after_generation': 0, 'max_width_px': 1280, 'jpeg_quality': 80})
        save(out, 'negative/wrong-revision.json', wrong_revision)
        stale = owner_request(eos_socket, {'op': 'vision_frame',
             'gate_path': gate_path, 'revision': binding['projection_revision'],
             'after_generation': 2**64 - 1,
             'max_width_px': 1280, 'jpeg_quality': 80})
        save(out, 'negative/stale-cursor.json', stale)
        time.sleep(3)
        after = owner_request(eos_socket, {'op': 'route_status'})
        save(out, 'route-status-after.json', after)
        stop = api(api_port, 'process-control', 'POST', {'schema_version': 1,
            'operation': 'stop', 'revision': revision})
        save(out, 'stop-response.json', stop)
        stopped = await_condition(lambda: (s if s['state'] == 'stopped' else None)
            if (s := api(api_port, 'process-status')) else None, timeout=30)
        save(out, 'status-stopped.json', stopped)
        save(out, 'negative/revoked-grant.json', {'socket_absent': not owner_socket.exists(),
             'inspection_socket_absent': not inspection_socket.exists(),
             'stale_test_socket_absent': not stale_test_socket.exists()})
    finally:
        cleanup_owned_run(out, api_port, running, serve)
        try:
            save(out, 'final-process-status.json', api(api_port, 'process-status', timeout=5))
        except Exception as error:
            save(out, 'final-process-status-error.json', {'type': type(error).__name__})
        if running is not None and running.poll() is None:
            running.terminate(); running.wait(timeout=10)
        if serve.poll() is None:
            serve.terminate(); serve.wait(timeout=10)
        serve_log.close(); run_log.close()
        raw_events = home / 'data/logs/default/events.log'
        if raw_events.is_file():
            (out / 'events.raw.jsonl').write_bytes(raw_events.read_bytes())
        owner_stderr = home / 'data/logs/default/owner/egia/stderr.log'
        if owner_stderr.is_file():
            (out / 'egia-stderr.raw.log').write_bytes(owner_stderr.read_bytes())
        last_error = home / 'run/default.last-error'
        if last_error.is_file():
            (out / 'owner.last-error.txt').write_bytes(last_error.read_bytes())
    files = {path.relative_to(out).as_posix(): sha(path)
             for path in out.rglob('*') if path.is_file()
             and path.name != 'evidence.sha256.json'}
    save(out, 'evidence.sha256.json', files)
    verify_camera_archive(out, selected, pins)
    print('Q01_CAMERA_SELECTED_PIN_PASS')


def verify_camera_archive(out: Path, selection: dict | None,
                          pins: dict[str, str]) -> None:
    """Bind the final self-check to the external selection when supplied."""
    from verify_q01_camera import verify
    if selection is None:
        verify(out)
    else:
        verify(out, selected_pins=pins, selection=selection)


if __name__ == '__main__':
    main()
