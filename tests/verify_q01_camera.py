#!/usr/bin/env python3
"""Verify a selected-pin Q01 camera archive without reading live owner state.

The replay retains owner responses and computed JPEG digests/lengths, never JPEG
bytes or grants. A checksum manifest makes accidental archive substitutions
visible; the semantic comparisons below are the qualification predicates.
"""

import argparse
import hashlib
import io
import json
import re
import subprocess
import tarfile
import tomllib
from pathlib import Path

from q01_selection import canonical_bytes, sha256
from verify_q01_r06_a03 import archive_tree


PINS = {
    'eos': '642166576a2cddfd97cd703e5bd9e53567745515',
    'isos': '00c1d7aa184de8b969e6e40472bf9b1fa55032ff',
    'egia': 'd3868d2763c35d9f6bfed80d75edb7a1844ef54e',
}
HEX = re.compile(r'[0-9a-f]{64}\Z')
IDENTITY = ('graph_revision', 'projection_revision', 'runtime_stream_id',
            'source_path', 'source_frame_id', 'gate_path', 'gate_generation',
            'render', 'jpeg_sha256')
REQUIRED = {
    'selected-inputs.json', 'saved-graph.raw.json', 'status-running.json',
    'vision-binding.json', 'vision-attest.json', 'eos-frame.json',
    'egia-observation.json',
    'browser-frame.json', 'frame-comparison.json', 'route-status-before.json',
    'frame-attempts.json',
    'route-status-after.json', 'fifo-probe/stdout.json', 'fifo-probe/source.json',
    'fifo-probe/routes.rs', 'fifo-probe/src/main.rs',
    'fifo-probe/Cargo.toml', 'fifo-probe/Cargo.lock', 'fifo-probe/build.log',
    'egia-stub.json', 'egia-vision-source.py',
    'selected-isos.toml', 'selected-island.toml',
    'selected-config-hashes.json',
    'source-build-receipts.json', 'source-snapshot/eos.tar',
    'source-snapshot/isos.tar', 'source-snapshot/egia.tar',
    'source-snapshot/eos-Cargo.lock', 'source-snapshot/isos-Cargo.lock',
    'source-snapshot/egia-uv.lock', 'source-snapshot/egia-launcher',
    'eos-selected-build.log', 'isos-selected-build.log',
    'selected-binaries/isos', 'selected-binaries/egia',
    'selected-binaries/aether-runtime', 'selected-binaries/aether-profile',
    'selected-binaries/aether-http-bridge',
    'selected-binaries/aether-config-server',
    'negative/wrong-grant.json', 'negative/wrong-revision.json',
    'negative/stale-cursor.json',
    'negative/live-wrong-revision.json',
    'negative/live-stale-cursor.json',
    'negative/egia-test-wrong-revision.json',
    'negative/egia-test-old-stream.json',
    'negative/egia-live-stale-generation.json',
    'negative/revoked-grant.json', 'stop-response.json', 'status-stopped.json',
    'evidence.sha256.json',
}


def read(root: Path, name: str):
    return json.loads((root / name).read_text())


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def counter(row: dict, key: str) -> int:
    value = row[key]
    assert type(value) is int and value >= 0, (key, value)
    return value


def selected_route(status: dict, source: str, gate: str) -> dict:
    assert status['ok'] is True
    rows = [row for row in status['routes']
            if row.get('source_node') == source and row.get('target_node') == gate]
    assert len(rows) == 1, rows
    return rows[0]


def assert_handshake(handshake: dict, *, revision: str, projection: str,
                     stream: str, source: str, gate: str, generation: int) -> None:
    assert handshake['type'] == 'subscribed'
    for key, expected in (('graph_revision', revision),
                          ('projection_revision', projection),
                          ('runtime_stream_id', stream), ('source_path', source),
                          ('gate_path', gate)):
        assert handshake[key] == expected, (key, handshake.get(key), expected)
    assert type(handshake['gate_generation']) is int
    assert 0 <= handshake['gate_generation'] <= generation


def consumer_identity(row: dict) -> tuple:
    return tuple(row[key] for key in IDENTITY)


def browser_after_follows(before: dict, after: dict) -> bool:
    """A live browser frame may advance after EGIA rejects the injected duplicate."""
    static = ('graph_revision', 'projection_revision', 'runtime_stream_id',
              'source_path', 'gate_path', 'render')
    if any(after.get(key) != before.get(key) for key in static):
        return False
    old_generation = before.get('gate_generation')
    new_generation = after.get('gate_generation')
    if type(old_generation) is not int or type(new_generation) is not int:
        return False
    if new_generation < old_generation:
        return False
    if new_generation == old_generation:
        return all(after.get(key) == before.get(key)
                   for key in IDENTITY + ('jpeg_length_bytes',))
    old_source = re.fullmatch(r'src-([1-9][0-9]*)',
                              str(before.get('source_frame_id')))
    new_source = re.fullmatch(r'src-([1-9][0-9]*)',
                              str(after.get('source_frame_id')))
    return (old_source is not None and new_source is not None
            and int(new_source.group(1)) > int(old_source.group(1))
            and after.get('jpeg_verified_sha256') == after.get('jpeg_sha256'))


def verify(root: Path, selected_pins: dict | None = None,
           selection: dict | None = None) -> None:
    assert root.is_dir(), root
    expected = ({owner:selection['owners'][owner]['commit'] for owner in PINS}
                if selection else dict(PINS))
    if selected_pins is not None:
        for owner in expected:
            assert selected_pins.get(owner) == expected[owner]
    assert (root / 'evidence.sha256.json').is_file()
    manifest = read(root, 'evidence.sha256.json')
    found = {p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()}
    assert REQUIRED <= found, sorted(REQUIRED - found)
    assert found == set(manifest) | {'evidence.sha256.json'}, sorted(found ^ (set(manifest) | {'evidence.sha256.json'}))
    for name, expected_hash in manifest.items():
        assert HEX.fullmatch(expected_hash), name
        assert digest(root / name) == expected_hash, name
    if selection:
        assert read(root,'selection-sha256.json') == \
            {'profile':'selected-q03','sha256':sha256(canonical_bytes(selection))}

    inputs = read(root, 'selected-inputs.json')
    receipts = read(root, 'source-build-receipts.json')
    for name, archived_hash in receipts['selected_binary_sha256'].items():
        assert archived_hash == digest(root / f'selected-binaries/{name}')
    assert receipts['selected_binary_sha256']['isos'] == inputs['binary_sha256']['isos']
    assert receipts['selected_binary_sha256']['aether-runtime'] == inputs['binary_sha256']['eos']
    assert receipts['selected_binary_sha256']['egia'] == inputs['binary_sha256']['egia']
    for owner, pin in expected.items():
        assert inputs['source_commits'][owner] == pin
        assert HEX.fullmatch(inputs['binary_sha256'][owner])
        receipt = receipts[owner]
        assert receipt['commit'] == pin
        assert re.fullmatch(r'[0-9a-f]{40}', receipt['tree'])
        assert receipt['binary_sha256'] == inputs['binary_sha256'][owner]
        archive = root / f'source-snapshot/{owner}.tar'
        assert receipt['snapshot_sha256'] == digest(archive)
        if selection:
            assert receipt['tree'] == selection['owners'][owner]['tree']
            assert archive_tree(archive) == receipt['tree']
        archived_commit = subprocess.run(['git', 'get-tar-commit-id'],
            input=archive.read_bytes(), capture_output=True, check=True).stdout.decode().strip()
        assert archived_commit == pin
        lock = 'uv.lock' if owner == 'egia' else 'Cargo.lock'
        copied_lock = root / f'source-snapshot/{owner}-{lock}'
        assert receipt['lock_sha256'] == digest(copied_lock)
        if selection:
            assert receipt['lock_sha256'] == selection['owners'][owner]['locks'][lock]
        with tarfile.open(fileobj=io.BytesIO(archive.read_bytes())) as source_tar:
            source_lock = source_tar.extractfile(lock)
            assert source_lock is not None
            assert hashlib.sha256(source_lock.read()).hexdigest() == receipt['lock_sha256']
        if owner != 'egia':
            command = receipt['build_command']
            assert Path(command[0]).name == 'cargo'
            assert command[1:] == ['build', '--locked',
                *(['--bin', 'isos'] if owner == 'isos' else ['--bins'])]
            log = root / f'{owner}-selected-build.log'
            assert receipt['build_log_sha256'] == digest(log)
            assert b'Finished `dev` profile' in log.read_bytes()
    assert receipts['egia']['build_kind'] == 'selected-source-launcher'
    assert receipts['egia']['launcher_sha256'] == digest(root / 'source-snapshot/egia-launcher')
    assert inputs['source_commits']['mujoco'] == (selection['owners']['mujoco']['commit']
        if selection else '4e6b5449ccd25be53ad95b13baa2c89f7ed91e10')
    if selection:
        assert inputs['environment']['package_release_sha256'] == \
            selection['package']['release_sha256']
    roots = inputs['source_roots']
    selected = read(root, 'selected-config-hashes.json')
    assert selected['isos_config_sha256'] == digest(root / 'selected-isos.toml')
    assert selected['island_config_sha256'] == digest(root / 'selected-island.toml')
    isos_config = tomllib.loads((root / 'selected-isos.toml').read_text())
    assert isos_config['vision_evidence']['gate_path'] == 'eos/interface/gate/front-preview'
    product_config = tomllib.loads((root / 'selected-island.toml').read_text())
    assert product_config['share'][0] == 'share'
    assert not Path(product_config['share'][1]).is_absolute()
    assert (Path(roots['egia']) / product_config['share'][1]).resolve() == Path(roots['eos']) / 'share'
    assert selected['resolved_egia_share'] == str(Path(roots['egia']) / 'share')
    assert selected['resolved_eos_share'] == str(Path(roots['eos']) / 'share')
    assert inputs['environment']['kind'] == 'selected-isos-mujoco-process'
    selected_home = Path(inputs['environment']['isos_home'])
    assert selected_home.is_absolute()
    assert inputs['environment']['egia_q01_stale_test_inherited'] is True
    saved = root / 'saved-graph.raw.json'
    revision = digest(saved)
    graph = json.loads(saved.read_bytes())
    assert graph['version'] == '0.1.0'
    assert 'EGIA_Q01_STALE_TEST' not in json.dumps(graph)
    status = read(root, 'status-running.json')
    assert status['state'] == 'running' and status['active_revision'] == revision
    run_id = status['run_id']
    assert run_id and all(p['id'] in {'eos', 'bridge', 'egia', 'config', 'simulator'}
                          for p in status['processes'])
    assert {'eos', 'bridge', 'egia'} <= {p['id'] for p in status['processes']}
    binding = read(root, 'vision-binding.json')
    assert binding['schema_version'] == 1
    assert binding['deployment_run_id'] == run_id
    assert binding['graph_revision'] == revision
    assert binding['eos_endpoint']['transport'] == 'uds'
    endpoint = Path(binding['eos_endpoint']['path'])
    endpoint_name = hashlib.sha256(f'default\0{run_id}'.encode()).hexdigest()[:32]
    assert endpoint.is_absolute()
    assert endpoint == selected_home / 'run/v' / endpoint_name / 'eos-vision-owner.sock'
    assert binding['ingress_type'] == 'egia.vision.ingress@v0.1.frame'
    assert binding['render'] == {'max_width_px': 1280, 'jpeg_quality': 80}
    assert binding['target_fps'] == 30
    assert set(binding) == {'schema_version', 'deployment_run_id', 'graph_revision',
                            'projection_revision', 'eos_endpoint', 'source_path',
                            'gate_path', 'ingress_type', 'render', 'target_fps',
                            'grant_file'}
    projection = binding['projection_revision']
    assert HEX.fullmatch(projection)
    public = status['vision_evidence']
    for key in ('gate_path', 'source_path', 'projection_revision', 'render', 'target_fps'):
        assert public[key] == binding[key], key
    assert 'grant_file' not in public and 'eos_endpoint' not in public
    gate = binding['gate_path']
    source = binding['source_path']
    source_node = graph['cores']['eos']['layers']['hardware']['groups']['camera']['nodes'][source.split('/')[-1]]
    gate_node = graph['cores']['eos']['layers']['interface']['groups']['gate']['nodes'][gate.split('/')[-1]]
    assert source_node['node_type'] == 'eos.hardware.camera@v0.1'
    assert gate_node['node_type'] == 'eos.interface.camera_gate@v0.1'
    assert gate_node['inputs']['frame']['node'] == source
    assert gate_node['inputs']['frame']['output'] == 'frame'
    attest = read(root, 'vision-attest.json')
    assert attest['ok'] is True and attest['ready'] is True
    for key, expected_value in (('run_id', run_id), ('active_revision', revision),
                                ('projection_revision', projection),
                                ('source_path', source), ('gate_path', gate)):
        assert attest[key] == expected_value, key
    stream = attest['runtime_stream_id']
    assert stream
    eos = read(root, 'eos-frame.json')
    egia = read(root, 'egia-observation.json')
    browser = read(root, 'browser-frame.json')
    assert eos['changed'] is True and eos['generation'] > 0
    metadata = eos['metadata']
    assert metadata['gate_generation'] == eos['generation']
    assert metadata['graph_revision'] == revision
    assert metadata['projection_revision'] == projection
    assert metadata['runtime_stream_id'] == stream
    assert metadata['source_path'] == source and metadata['gate_path'] == gate
    assert metadata['render'] == binding['render']
    assert_handshake(eos['handshake'], revision=revision, projection=projection,
                     stream=stream, source=source, gate=gate,
                     generation=metadata['gate_generation'])
    assert HEX.fullmatch(metadata['jpeg_sha256'])
    assert 0 < eos['jpeg_length_bytes'] <= 8 * 1024 * 1024
    assert eos['jpeg_verified_sha256'] == metadata['jpeg_sha256']
    assert egia['type'] == 'vision_evidence' and egia['state'] == 'available'
    assert egia['deployment_run_id'] == run_id
    assert egia['accepted_count'] >= 1
    assert egia['jpeg_length_bytes'] == eos['jpeg_length_bytes']
    assert browser['jpeg_length_bytes'] == eos['jpeg_length_bytes']
    assert browser['jpeg_verified_sha256'] == metadata['jpeg_sha256']
    assert_handshake(browser['handshake'], revision=revision, projection=projection,
                     stream=stream, source=source, gate=gate,
                     generation=browser['gate_generation'])
    for owner in (egia, browser):
        for key in IDENTITY:
            assert owner[key] == metadata[key], (key, owner.get(key), metadata[key])
    assert egia['gate_generation'] > 0
    comparison = read(root, 'frame-comparison.json')
    assert comparison == {
        'owner_generation': eos['generation'],
        'browser_generation': browser['gate_generation'],
        'egia_generation': egia['gate_generation'],
        'same_identity': consumer_identity(browser) == consumer_identity(egia)
                         == consumer_identity(metadata),
    }
    assert comparison['same_identity'] is True
    frame_attempts = read(root, 'frame-attempts.json')
    attempts = frame_attempts['attempts']
    assert 1 <= len(attempts) <= 16
    assert frame_attempts['selected_index'] == len(attempts) - 1
    assert attempts[-1] == {'eos': eos, 'browser': browser,
                            'egia': egia, 'comparison': comparison}
    for attempt in attempts:
        row = attempt['comparison']
        assert row == {
            'owner_generation': attempt['eos']['generation'],
            'browser_generation': attempt['browser']['gate_generation'],
            'egia_generation': attempt['egia']['gate_generation'],
            'same_identity': consumer_identity(attempt['eos']['metadata'])
                             == consumer_identity(attempt['browser'])
                             == consumer_identity(attempt['egia']),
        }

    for filename, reason in (('egia-test-wrong-revision.json', 'binding_stale'),
                             ('egia-test-old-stream.json', 'target_changed')):
        test = read(root, f'negative/{filename}')
        response = test['response']
        assert response == {'schema_version': 1, 'op': 'replay_latest',
                            'injected': False, 'reason_code': reason}
        assert test['before']['deployment_run_id'] == run_id
        assert test['after']['deployment_run_id'] == run_id
        assert test['after']['accepted_count'] >= test['before']['accepted_count']
        assert test['after']['rejected_count'] >= test['before']['rejected_count']
        if reason == 'binding_stale':
            assert test['request']['graph_revision'] != revision
        else:
            assert test['request']['runtime_stream_id'] != stream
    duplicate = read(root, 'negative/egia-live-stale-generation.json')
    duplicate_attempts = duplicate['attempts']
    preflight_mismatches = duplicate['preflight_mismatches']
    selected_duplicate = duplicate['selected_index']
    assert isinstance(preflight_mismatches, list)
    assert 1 <= len(duplicate_attempts) + len(preflight_mismatches) <= 12
    for mismatch in preflight_mismatches:
        assert mismatch['egia']['deployment_run_id'] == run_id
        assert mismatch['browser']['graph_revision'] == revision
        assert mismatch['browser']['projection_revision'] == projection
        assert mismatch['browser']['runtime_stream_id'] == stream
        assert mismatch['browser']['jpeg_verified_sha256'] == mismatch['browser']['jpeg_sha256']
    assert selected_duplicate == len(duplicate_attempts) - 1
    for attempt in duplicate_attempts[:selected_duplicate]:
        assert attempt['response'] == {'schema_version': 1, 'op': 'replay_latest',
            'injected': False, 'reason_code': 'target_changed'}
    selected_attempt = duplicate_attempts[selected_duplicate]
    response = selected_attempt['response']
    assert response['schema_version'] == 1 and response['op'] == 'replay_latest'
    assert response['injected'] is True and response['reason_code'] == 'generation_stale'
    before_duplicate, after_duplicate = response['before'], response['after']
    assert before_duplicate['type'] == after_duplicate['type'] == 'vision_evidence'
    assert before_duplicate['state'] == after_duplicate['state'] == 'available'
    assert before_duplicate['deployment_run_id'] == after_duplicate['deployment_run_id'] == run_id
    for snapshot in (before_duplicate, after_duplicate):
        assert snapshot['graph_revision'] == revision
        assert snapshot['projection_revision'] == projection
        assert snapshot['source_path'] == source
        assert snapshot['gate_path'] == gate
        assert snapshot['runtime_stream_id'] == stream
        assert type(snapshot['gate_generation']) is int and snapshot['gate_generation'] > 0
    for key in IDENTITY + ('jpeg_length_bytes',):
        assert before_duplicate[key] == after_duplicate[key], key
    for key in ('runtime_stream_id', 'gate_generation', 'source_frame_id', 'jpeg_sha256'):
        assert selected_attempt['request_identity'][key] == before_duplicate[key], key
    assert after_duplicate['accepted_count'] == before_duplicate['accepted_count']
    assert after_duplicate['rejected_count'] == before_duplicate['rejected_count'] + 1
    assert after_duplicate['rejection_counts']['generation_stale'] == \
           before_duplicate['rejection_counts'].get('generation_stale', 0) + 1
    browser_before = selected_attempt['browser_before']
    browser_after = selected_attempt['browser_after']
    for name, browser_snapshot in (('browser_before', browser_before),
                                   ('browser_after', browser_after)):
        assert_handshake(browser_snapshot['handshake'], revision=revision,
                         projection=projection, stream=stream, source=source,
                         gate=gate, generation=browser_snapshot['gate_generation'])
        assert browser_snapshot['jpeg_verified_sha256'] == browser_snapshot['jpeg_sha256']
        assert 0 < browser_snapshot['jpeg_length_bytes'] <= 8 * 1024 * 1024
    for key in IDENTITY + ('jpeg_length_bytes',):
        assert browser_before[key] == before_duplicate[key], key
    assert browser_after_follows(browser_before, browser_after)

    for filename, reason in (('live-wrong-revision.json', 'revision_mismatch'),
                             ('live-stale-cursor.json', 'cursor_stale')):
        live = read(root, f'negative/{filename}')
        assert live['expected_reason'] == reason
        attempts = live['attempts']
        assert 1 <= len(attempts) <= 12
        stable_index = live['stable_attempt_index']
        assert type(stable_index) is int and 0 <= stable_index < len(attempts)
        for index, attempt in enumerate(attempts):
            rejected = attempt['rejection']
            request = rejected['request']
            assert request['op'] == 'subscribe' and request['gate_path'] == gate
            assert rejected['reply']['type'] == 'error'
            assert rejected['reply']['reason_code'] == reason
            assert rejected['close_code'] == 1008
            assert rejected['binary_frames'] == 0
            if reason == 'revision_mismatch':
                assert request['revision'] != projection
            else:
                assert request['revision'] == projection
                assert request['after_runtime_stream_id'] != stream
                assert type(request['after_generation']) is int
                assert request['after_generation'] > 0
            for state in ('before', 'after'):
                snapshot = attempt[state]
                assert snapshot['egia']['type'] == 'vision_evidence'
                assert snapshot['egia']['state'] == 'available'
                assert snapshot['egia']['deployment_run_id'] == run_id
                frame = snapshot['browser']
                assert_handshake(frame['handshake'], revision=revision,
                                 projection=projection, stream=stream,
                                 source=source, gate=gate,
                                 generation=frame['gate_generation'])
                assert frame['jpeg_verified_sha256'] == frame['jpeg_sha256']
                assert 0 < frame['jpeg_length_bytes'] <= 8 * 1024 * 1024
                for consumer in (frame, snapshot['egia']):
                    assert consumer['graph_revision'] == revision
                    assert consumer['projection_revision'] == projection
                    assert consumer['runtime_stream_id'] == stream
                    assert consumer['source_path'] == source
                    assert consumer['gate_path'] == gate
            before = attempt['before']
            after = attempt['after']
            unchanged = (consumer_identity(before['browser'])
                         == consumer_identity(after['browser'])
                         == consumer_identity(before['egia'])
                         == consumer_identity(after['egia'])
                         and before['egia']['accepted_count']
                         == after['egia']['accepted_count'])
            assert attempt['consumers_unchanged'] is unchanged
            if index == stable_index:
                assert unchanged

    before = selected_route(read(root, 'route-status-before.json'), source, gate)
    after = selected_route(read(root, 'route-status-after.json'), source, gate)
    assert after['frequency_hz'] > 0
    for key in ('delivered_count', 'cap_wait_count', 'replaced_count', 'gap_count'):
        assert counter(after, key) >= counter(before, key), key
    assert counter(after, 'cap_wait_count') > counter(before, 'cap_wait_count')
    assert counter(after, 'replaced_count') > counter(before, 'replaced_count')
    assert counter(after, 'gap_count') > counter(before, 'gap_count')
    assert counter(after, 'overflow_count') == counter(before, 'overflow_count') == 0
    probe = read(root, 'fifo-probe/source.json')
    assert probe['eos_commit'] == expected['eos'] and probe['exit_code'] == 0
    assert probe['routes_sha256'] == digest(root / 'fifo-probe/routes.rs')
    assert probe['probe_sha256'] == digest(root / 'fifo-probe/src/main.rs')
    if selection:
        assert probe['routes_sha256'] == selection['owners']['eos']['sources'][
            'crates/aether-runtime/src/bin/aether-runtime/routes.rs']
        assert probe['probe_sha256'] == selection['harness']['tests/q01_fifo_probe.rs']
    fifo = read(root, 'fifo-probe/stdout.json')
    assert fifo['source_kind'] == 'event'
    assert fifo['bound_records'] == 64 and fifo['bound_bytes'] == 1024 * 1024
    assert fifo['actuator_route_affected'] is False
    assert fifo['before']['queue_class'] == fifo['after']['queue_class'] == 'fifo'
    assert counter(fifo['before'], 'overflow_count') == 0
    assert counter(fifo['after'], 'overflow_count') == 1
    assert counter(fifo['before'], 'gap_count') == 0
    assert counter(fifo['after'], 'gap_count') == 1
    assert fifo['after']['queue_depth'] == 64

    stub = read(root, 'egia-stub.json')
    assert stub['source_commit'] == expected['egia']
    assert stub['selected_implementation_sha256'] == digest(root / 'egia-vision-source.py')
    if selection:
        assert stub['selected_implementation_sha256'] == \
            selection['owners']['egia']['sources']['src/egia/vision_evidence.py']
    assert stub['wrong_run_reason'] == 'run_mismatch'
    baseline = stub['baseline']
    assert baseline['state'] == 'available' and baseline['accepted_count'] == 1
    for name, index, reason in (('stale_generation', 1, 'generation_stale'),
                                ('wrong_revision', 2, 'revision_mismatch'),
                                ('digest_mismatch', 3, 'digest_mismatch')):
        snapshot = stub[name]
        assert snapshot['state'] == 'available'
        assert snapshot['accepted_count'] == baseline['accepted_count']
        assert snapshot['rejected_count'] == index
        assert snapshot['rejection_counts'][reason] == 1
        for field in ('gate_generation', 'source_frame_id', 'jpeg_sha256',
                      'jpeg_length_bytes'):
            assert snapshot[field] == baseline[field], (name, field)

    denial = read(root, 'negative/wrong-grant.json')
    assert denial['http_status'] == 403
    assert denial['body'] == {'ok': False, 'error': 'owner_grant_denied'}
    wrong_revision = read(root, 'negative/wrong-revision.json')
    assert wrong_revision['ok'] is False
    assert wrong_revision['error'] == 'graph revision changed'
    stale_cursor = read(root, 'negative/stale-cursor.json')
    assert stale_cursor['ok'] is True and stale_cursor['changed'] is False
    assert read(root, 'stop-response.json')['terminal']['outcome'] == 'confirmed'
    stopped = read(root, 'status-stopped.json')
    assert stopped['state'] == 'stopped' and stopped['processes'] == []
    assert stopped['vision_evidence'] is None
    revoked = read(root, 'negative/revoked-grant.json')
    assert revoked == {'socket_absent': True, 'inspection_socket_absent': True,
                       'stale_test_socket_absent': True}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    args = parser.parse_args()
    verify(args.archive)
    print('Q01_CAMERA_ARCHIVE_PASS')
