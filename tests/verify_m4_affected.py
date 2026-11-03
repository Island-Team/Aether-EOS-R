#!/usr/bin/env python3
"""Verify a completed M4 affected-boundary replay from preserved raw outputs."""

import argparse
import hashlib
import json
from pathlib import Path


def load(root, name):
    return json.loads((root / name).read_text())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--scope', choices=('full', 'a03'), default='full',
                        help='a03 verifies the selected mission/Stop baseline without browser evidence')
    args = parser.parse_args()
    root = args.output.resolve()
    inputs = load(root, 'selected-inputs.json')
    graph = load(root, 'graph.json')
    validation = load(root, 'validation.json')
    running = load(root, 'status-running.json')
    stopped = load(root, 'status-stopped.json')
    stop = load(root, 'stop-response.json')
    results = load(root, 'primitive-result.json')
    selection = load(root, 'selection.json')
    negative = load(root, 'negative-edges.json')
    current_running = load(root, 'logs-current-running.json')
    current_stopped = load(root, 'logs-current-stopped.json')
    revision = running['saved_revision']
    run_id = running['run_id']
    assert len(revision) == 64 and len(run_id) >= 2
    saved_bytes = (root / 'saved-graph.raw.json').read_bytes()
    assert hashlib.sha256(saved_bytes).hexdigest() == revision
    assert json.loads(saved_bytes) == graph
    assert running['active_revision'] == stopped['active_revision'] == stopped['saved_revision'] == revision
    generation = selection['entries'][revision]['generation']
    assert selection['schema_version'] == 2 and selection['entries'][revision]['kind'] == 'v2'
    assert len(generation) == 64 and all(char in '0123456789abcdef' for char in generation)
    overlay_bytes = (root / 'egia-stages.overlay.json').read_bytes()
    expected_overlay = json.dumps({'revision':revision,'version':'0.1.0',
                                   'cores':{'egia':graph['cores']['egia']}},
                                  sort_keys=True,separators=(',',':')).encode() + b'\n'
    assert overlay_bytes == expected_overlay
    assert validation['valid'] is True
    routes = [edge for edge in validation['edges'] if edge.get('route_class') == 'egia_local' and edge.get('routed')]
    assert len(routes) == 2 and all(edge['qos_eligible'] for edge in routes)
    assert {item['case'] for item in negative} == {'third-route-qos','fixed-safety-route'}
    assert any(item['code'] == 'edge_qos_forbidden' and item['path'].endswith('/inputs/snapshot/priority')
               for case in negative if case['case'] == 'third-route-qos' for item in case['diagnostics'])
    assert any(item['code'] == 'edge_fixed_route' and item['path'].endswith('/inputs/admitted')
               for case in negative if case['case'] == 'fixed-safety-route' for item in case['diagnostics'])
    assert running['state'] == 'running' and running['run_id_state'] == 'known'
    assert {item['id'] for item in running['processes']} == {'eos','egia','config','bridge'}
    for current in (current_running,current_stopped):
        assert current['scope']['kind'] == 'deployment_run'
        assert current['scope']['run_id'] == run_id
        assert current['anchor']['state'] == 'found'
        assert current['anchor']['event']['run_id'] == run_id
    assert stop['terminal']['outcome'] == 'confirmed'
    assert stop['terminal']['run_id'] == run_id
    assert stop['stopped'] == ['egia','config','bridge','eos']
    assert stopped['state'] == 'stopped' and stopped['processes'] == []
    assert stopped['run_id'] == run_id and stopped['terminal'] == stop['terminal']
    reaped = load(root, 'reap-proof.json')
    assert reaped['run_id'] == run_id and reaped['all_absent'] is True
    assert reaped['capture_phase'] in {'immediately_after_stop', 'post_run_archive'}
    assert isinstance(reaped['observed_unix_s'], (int, float))
    assert reaped['pids'] == {item['id']: item['pid'] for item in running['processes']}
    command_ids = []
    for key in ('movement','stop'):
        value = results[key]
        assert value['status'] == 'succeeded' and value['accepted'] is True
        command_ids.append(value['id'])
    assert len(set(command_ids)) == 2
    events = [json.loads(line) for line in (root / 'events.raw.jsonl').read_text().splitlines()]
    codes = {item['event_code'] for item in events if item['run_id'] == run_id}
    assert {'eos_ready','egia_ready','deployment_ready','deployment_stopped'} <= codes
    mission = load(root, 'mission.json')
    assert mission['terminal']['state'] == 'succeeded'
    mission_id = mission['terminal']['mission_id']
    actions = [action for step in mission['terminal']['document']['plan']['steps']
               for action in step['actions']]
    assert len(actions) == 1, 'supervised mission needs exactly one governed action'
    assert actions[0]['robot_id'] == 'robot-a'
    assert actions[0]['device_id'] == 'drive'
    assert actions[0]['action'] == 'base.velocity'
    assert actions[0]['state'] == 'done'
    assert 'succeeded' in actions[0]['result']
    owner_events = [item for item in events if item.get('source') == 'egia' and item.get('run_id') == run_id]
    stage_indices = [i for i, item in enumerate(owner_events)
                     if item['event_code'] == 'egia_stage_transition']
    assert stage_indices, 'selected typed stage must accept input'
    proposals = [item for item in owner_events if item['event_code'] == 'egia_proposal_created'
                 and item.get('fields', {}).get('mission_id') == mission_id]
    assert len(proposals) == 1, 'mission must create one correlated owner proposal'
    operation_id = proposals[0]['fields']['operation_id']
    sequence = ['egia_proposal_created','egia_admission_decided',
                'egia_operation_submitted','egia_operation_terminal']
    operation_events = [(i, item) for i, item in enumerate(owner_events)
                        if item.get('fields', {}).get('operation_id') == operation_id
                        and item['event_code'] in sequence]
    assert [item['event_code'] for _, item in operation_events] == sequence
    assert min(stage_indices) < operation_events[0][0]
    assert all(item['correlation_id'] == operation_id for _, item in operation_events)
    assert operation_events[1][1]['fields']['outcome'] == 'admitted'
    assert operation_events[-1][1]['fields']['outcome'] == 'succeeded'
    journal = root / 'journal'
    journal_manifest = load(root, 'journal-sha256.json')
    record_paths = sorted(journal.glob('*.rec'))
    assert {path.name for path in record_paths} == set(journal_manifest)
    assert all(hashlib.sha256(path.read_bytes()).hexdigest() == journal_manifest[path.name]
               for path in record_paths)
    records = [json.loads(path.read_text())['body'] for path in record_paths]
    matching = [record for record in records if record['command_id'] == operation_id]
    assert len(matching) == 1, 'mission operation must have one EOS journal record'
    record = matching[0]
    assert record['phase'] == 'terminal' and record['result']['status'] == 'Completed'
    assert record['operation']['accepted'] is True
    identity = record['operation']['identity']
    assert identity['primitive'] == 'base.velocity'
    assert identity['admission']['binding_path'] == 'eos/dispatcher/base/base-velocity'
    assert identity['admission']['ingress'] == 'grpc'
    assert identity['admission']['run_id'] == run_id
    assert identity['admission']['active_revision'] == revision
    assert record['node_path'] == 'eos/hardware/base/drive'
    browser_id = None
    if args.scope == 'full':
        served = root / 'ui-served-dist'
        index_hash = hashlib.sha256((served / 'index.html').read_bytes()).hexdigest()
        assert (root / 'ui-dynamic-index.sha256').read_text().split()[0] == index_hash
        assert load(served, 'manifest.json')['source_commit'] == inputs['ui']
        asset_hashes = (root / 'ui-served-dist-sha256.txt').read_text().splitlines()
        assert len(asset_hashes) >= 3
        seen = set()
        for item in asset_hashes:
            expected, path = item.split(maxsplit=1)
            relative = Path(path)
            assert not relative.is_absolute() and relative.parts and '..' not in relative.parts
            asset = served / relative
            assert not asset.is_symlink() and asset.resolve().is_relative_to(served)
            assert relative not in seen and asset.is_file()
            seen.add(relative)
            assert hashlib.sha256(asset.read_bytes()).hexdigest() == expected
        assert seen == {path.relative_to(served) for path in served.rglob('*') if path.is_file()}
        browser = load(root, 'browser-dom-observation.json')
        browser_result = load(root, 'browser-result.json')
        browser_id = browser['command_id']
        assert len({operation_id, browser_id, *command_ids}) == 4
        assert browser['active_revision'] == revision
        assert browser['binding'] == 'eos/dispatcher/base/base-velocity'
        assert browser['target'] == 'eos/hardware/base/drive'
        assert browser['invoke_enabled_before_click'] is True
        assert browser['owner_status_after_click'] == 'Completed'
        assert browser_result['result']['command_id'] == browser_id
        assert browser_result['result']['status'] == 'Completed'
        ui_records = [item for item in records if item['command_id'] == browser_id]
        assert len(ui_records) == 1 and ui_records[0]['operation']['accepted'] is True
        assert ui_records[0]['operation']['identity']['admission']['run_id'] == run_id
        assert ui_records[0]['operation']['identity']['admission']['active_revision'] == revision
        assert ui_records[0]['operation']['identity']['admission']['ingress'] == 'private_bridge'
    else:
        assert not (root/'browser-result.json').exists()
        assert len({operation_id, *command_ids}) == 3
    print(json.dumps({'result':'M4_AFFECTED_VERIFIED_PASS' if args.scope == 'full' else 'M4_A03_BASELINE_VERIFIED_PASS',
                      'run_id':run_id,'revision':revision,
                      'command_ids':command_ids,'mission_operation_id':operation_id,
                      'browser_command_id':browser_id,'routes':len(routes),'events':len(events),
                      'terminal':stop['terminal']['outcome'],'pins':{key:inputs[key] for key in ('isos','eos','egia')}},
                     sort_keys=True))


if __name__ == '__main__':
    main()
