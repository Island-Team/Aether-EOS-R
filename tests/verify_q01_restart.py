#!/usr/bin/env python3
"""Archive-only verification for the scoped Q01 new-run restart replay."""
import argparse
import hashlib
import json
from pathlib import Path
import tomllib
from datetime import datetime


def load(root, name):
    return json.loads((root/name).read_text())


def journal_checksum(body):
    encoded = json.dumps(body, separators=(',', ':'), ensure_ascii=False).encode()
    value = 0xcbf29ce484222325
    for byte in encoded:
        value = ((value ^ byte) * 0x100000001b3) & ((1 << 64) - 1)
    return value


def correlated_mission(events, mission, run_id, operation_id):
    assert mission['terminal']['state']=='succeeded'
    mission_id = mission['terminal']['mission_id']
    owner = [e for e in events if e.get('source')=='egia' and e.get('run_id')==run_id]
    stage = [i for i,e in enumerate(owner) if e['event_code']=='egia_stage_transition']
    sequence = ['egia_proposal_created','egia_admission_decided',
                'egia_operation_submitted','egia_operation_terminal']
    selected = [(i,e) for i,e in enumerate(owner) if e['event_code'] in sequence and
                e.get('fields',{}).get('operation_id')==operation_id]
    assert [e['event_code'] for _,e in selected] == sequence
    assert stage and min(stage)<selected[0][0]
    assert selected[0][1]['fields']['mission_id']==mission_id
    assert all(e['correlation_id']==operation_id for _,e in selected)
    assert selected[1][1]['fields']['outcome']=='admitted'
    assert selected[-1][1]['fields']['outcome']=='succeeded'
    actions = [action for step in mission['terminal']['document']['plan']['steps']
               for action in step['actions']]
    assert len(actions)==1
    assert actions[0]['robot_id']=='robot-a' and actions[0]['device_id']=='drive'
    assert actions[0]['action']=='base.velocity' and actions[0]['state']=='done'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    a = parser.parse_args()
    root = a.output.resolve()
    inputs = load(root,'selected-inputs.json')
    for owner in ('eos', 'egia', 'isos', 'ui'):
        pin = inputs[owner]
        assert len(pin) == 40 and all(c in '0123456789abcdef' for c in pin), owner
    for key in ('isos_bin_sha256','eos_runtime_sha256','egia_launcher_sha256',
                'egia_config_sha256','mujoco_release_sha256','ui_index_sha256'):
        digest = inputs[key]
        assert len(digest)==64 and all(c in '0123456789abcdef' for c in digest), key
    old = load(root,'status-running.json')
    new = load(root,'q01-status-new-running.json')
    final = load(root,'status-stopped.json')
    summary = load(root,'q01-summary.json')
    rev = old['saved_revision']
    assert hashlib.sha256((root/'saved-graph.raw.json').read_bytes()).hexdigest() == rev
    assert load(root,'q01-revision-pins.json')['saved_graph_sha256']==rev
    assert hashlib.sha256((root/'q01-selection.raw.json').read_bytes()).hexdigest() == load(root,'q01-revision-pins.json')['selection_sha256']
    graph = load(root,'graph.json')
    assert json.loads((root/'saved-graph.raw.json').read_bytes()) == graph
    for phase in ('before-restart','after-restart','after-final-stop'):
        proof = load(root,f'q01-control-{phase}.json')
        raw = (root/proof['raw_file']).read_bytes()
        assert hashlib.sha256(raw).hexdigest() == proof['sha256']
        record = json.loads(raw)
        body = record['body']
        assert record['checksum'] == journal_checksum(body)
        assert body == proof['body']
        assert body['version'] == '1.1.0'
        assert body['robot_id'] == graph['robot']['id']
        assert body['shutdown_pending'] is False and body['shutdown_targets'] == []
        assert body['estop_latched'] is False
    expected_overlay = json.dumps({'revision':rev,'version':'0.1.0','cores':{'egia':graph['cores']['egia']}},
                                  sort_keys=True,separators=(',',':')).encode()+b'\n'
    assert (root/'egia-stages.overlay.json').read_bytes() == expected_overlay
    assert old['run_id'] != new['run_id']
    assert old['active_revision'] == new['active_revision'] == rev
    assert summary['old_run'] == old['run_id'] and summary['new_run'] == new['run_id']
    assert summary['old_operation_id'] != summary['new_operation_id']
    assert set(p['pid'] for p in old['processes']).isdisjoint(p['pid'] for p in new['processes'])
    grants = load(root,'q01-grant-digests.json')
    assert grants['old_sha256'] != grants['new_sha256']
    assert grants['old_run'] == old['run_id'] and grants['new_run'] == new['run_id']
    assert load(root,'q01-old-reap-proof.json')['all_absent'] is True
    assert load(root,'q01-old-id-lookup.json') == {'status':404,'operation_id':summary['old_operation_id']}
    assert load(root,'q01-retired-grant.json')['status'] == 401
    fault = load(root,'q01-fault-mission.json')
    fresh = load(root,'q01-fresh-mission.json')
    assert fault['terminal']['state']=='failed' and fresh['terminal']['state']=='succeeded'
    assert fault['terminal']['mission_id'] == summary['fault_mission_id']
    assert not any(step['actions'] for step in fault['terminal']['document']['plan']['steps'])
    actions = [a for step in fresh['terminal']['document']['plan']['steps'] for a in step['actions']]
    assert len(actions)==1 and actions[0]['action']=='base.velocity' and actions[0]['state']=='done'
    model = load(root,'q01-model-fault.json')
    config_hashes = load(root,'q01-config-hashes.json')
    assert hashlib.sha256((root/'q01-selected-egia.toml').read_bytes()).hexdigest()==config_hashes['selected_sha256']
    assert hashlib.sha256((root/'q01-factory-egia.toml').read_bytes()).hexdigest()==config_hashes['factory_sha256']
    selected_config = tomllib.loads((root/'q01-selected-egia.toml').read_text())
    factory_config = tomllib.loads((root/'q01-factory-egia.toml').read_text())
    effective_limit = selected_config.get('decision',{}).get('max_invalid_replies',
                      factory_config.get('decision',{}).get('max_invalid_replies',3))
    assert model['limit']==effective_limit
    assert len(model['requests']) >= model['limit'] >= 1
    assert all(q['mode']=='invalid' and q['lane']=='controller' and q['reply']=='INVALID_Q01_REPLY'
               for q in model['requests'])
    planner = load(root,'q01-planner-fault-events.json')
    owner_bytes = (root/'q01-egia-owner-fault.raw.jsonl').read_bytes()
    assert hashlib.sha256(owner_bytes).hexdigest()==load(root,'q01-egia-owner-fault-sha256.json')['sha256']
    owner_lines = [json.loads(line) for line in owner_bytes.splitlines()]
    assert owner_lines == planner
    full_owner_bytes = (root/'egia-owner-full.raw.jsonl').read_bytes()
    assert hashlib.sha256(full_owner_bytes).hexdigest()==load(root,'egia-owner-full-sha256.json')['sha256']
    provenance = load(root,'egia-owner-archive-provenance.json')
    assert provenance['source_sha256']==hashlib.sha256(full_owner_bytes).hexdigest()
    assert provenance['bytes']==len(full_owner_bytes)
    assert provenance['source'].endswith('/data/state/default/egia/egia.jsonl')
    assert provenance['capture'] in {'run finalizer','post-run retained-home copy'}
    full_owner = [json.loads(line) for line in full_owner_bytes.splitlines()]
    assert owner_lines == [entry for entry in full_owner
                           if entry.get('run_id')==old['run_id']
                           and entry.get('mission_id')==summary['fault_mission_id']
                           and entry.get('event') in {'step_parse_failed','invalid_reply_limit_reached'}]
    assert all(e['mission_id']==summary['fault_mission_id'] and e['run_id']==old['run_id']
               for e in owner_lines)
    assert all(e['reply_preview']=='INVALID_Q01_REPLY' for e in planner[:-1])
    assert [e['attempt'] for e in planner[:-1]] == list(range(1,model['limit']+1))
    assert planner[-1]['event']=='invalid_reply_limit_reached'
    assert planner[-1]['attempts']==model['limit']
    assert [t['mode'] for t in model['transitions']] == ['normal','invalid']
    repair = load(root,'q01-model-repair.json')
    assert [t['mode'] for t in repair['transitions']] == ['normal','invalid','normal']
    assert repair['requests_before_repair'] == repair['transitions'][-1]['after_request_index']
    repaired = [q for q in load(root,'model-requests-final.json')
                if q['index']>repair['requests_before_repair'] and q['lane']=='controller']
    assert [q['reply'] for q in repaired] == [
        'plan add move "Robot has moved."','move','final movement complete']
    assert all(q['mode']=='normal' for q in repaired)
    before = load(root,'q01-journal-before-fault.json')
    before_restart = load(root,'q01-journal-before-restart.json')
    after_lookup = load(root,'q01-journal-after-lookup.json')
    assert set(before).issubset(before_restart) and set(before_restart).issubset(after_lookup)
    assert all(after_lookup[name] == digest for name,digest in before_restart.items())
    assert all(before_restart[name] == digest for name,digest in before.items())
    events = [json.loads(line) for line in (root/'events.raw.jsonl').read_text().splitlines()]
    old_stop = [i for i,e in enumerate(events) if e['event_code']=='deployment_stopped' and
                e['run_id']==old['run_id'] and e.get('fields',{}).get('outcome')=='confirmed']
    new_start = [i for i,e in enumerate(events) if e['event_code']=='deployment_starting' and e['run_id']==new['run_id']]
    assert old_stop and new_start and max(old_stop)<min(new_start)
    assert any(e['event_code']=='egia_ready' and e['run_id']==new['run_id'] for e in events)
    hardware = load(root,'q01-new-hardware-ready.json')
    assert hardware['observed_run_id']==new['run_id']
    assert hardware['observed_egia_pid']==next(p['pid'] for p in new['processes'] if p['id']=='egia')
    assert hardware['eos_discovery']['run_id']==new['run_id']
    assert hardware['eos_discovery']['robot']=='robot-a' and hardware['eos_discovery']['primitives']>=4
    assert datetime.fromisoformat(hardware['eos_discovery']['timestamp']).timestamp()*1e9 <= hardware['catalog_observed_unix_ns']
    assert hardware['catalog_observed_unix_ns'] < datetime.fromisoformat(fresh['terminal']['started_at']).timestamp()*1e9
    assert any(item.get('name')=='base.velocity' and item.get('robot_id')=='robot-a'
               and item.get('device_id')=='drive' and item.get('health')=='ready'
               for item in hardware['planner_catalog']['primitives'])
    before_fault_submitted = set(load(root,'q01-submitted-before-fault.json'))
    before_restart_events = load(root,'q01-events-before-restart.json')
    assert before_restart_events == events[:len(before_restart_events)]
    after_fault_submitted = {e.get('fields',{}).get('operation_id') for e in before_restart_events
                             if e.get('event_code')=='egia_operation_submitted'}
    assert after_fault_submitted == before_fault_submitted
    proposals = [e for e in events if e['event_code']=='egia_proposal_created']
    assert any(e.get('fields',{}).get('operation_id')==summary['new_operation_id'] and
               e['run_id']==new['run_id'] for e in proposals)
    journal = root/'journal'
    manifest = load(root,'journal-sha256.json')
    paths = list(journal.glob('*.rec'))
    assert {p.name for p in paths} == set(manifest)
    assert all(hashlib.sha256(p.read_bytes()).hexdigest()==manifest[p.name] for p in paths)
    records = [json.loads(p.read_text())['body'] for p in paths]
    correlated_mission(events,load(root,'mission.json'),old['run_id'],summary['old_operation_id'])
    correlated_mission(events,fresh,new['run_id'],summary['new_operation_id'])
    assert sum(e['event_code']=='egia_operation_submitted' and
               e.get('fields',{}).get('operation_id')==summary['old_operation_id'] for e in events)==1
    assert not any(e['event_code']=='egia_proposal_created' and
                   e.get('fields',{}).get('mission_id')==summary['fault_mission_id'] for e in events)
    for name in set(after_lookup)-set(before_restart):
        record = json.loads((journal/name).read_text())['body']
        assert record['operation']['identity']['kind'] != 'movement'
    for op_id, run_id in ((summary['old_operation_id'],old['run_id']),
                          (summary['new_operation_id'],new['run_id'])):
        matches = [r for r in records if r['command_id']==op_id]
        assert len(matches)==1 and matches[0]['operation']['identity']['admission']['run_id']==run_id
        assert matches[0]['phase']=='terminal' and matches[0]['result']['status']=='Completed'
        assert matches[0]['operation']['identity']['admission']['ingress']=='grpc'
        assert matches[0]['operation']['identity']['admission']['active_revision']==rev
        assert matches[0]['operation']['identity']['admission']['authority_role']=='egia_control'
        identity = matches[0]['operation']['identity']
        assert identity['kind']=='movement' and identity['primitive']=='base.velocity'
        assert identity['version']=='1.0.0' and matches[0]['node_path']=='eos/hardware/base/drive'
        assert identity['target']=={'hardware':{'node_path':'eos/hardware/base/drive'}}
        assert identity['admission']['binding_path']=='eos/dispatcher/base/base-velocity'
        assert identity['admission']['primitive_version']=='1.0.0'
        signature = json.loads(matches[0]['signature'])
        assert signature[1]=='eos/hardware/base/drive'
        assert signature[2]['Base']['BodyVelocity']=={
            'x_m_s':0.1,'y_m_s':0.0,'yaw_rad_s':0.0,'valid_for_ms':100}
        assert matches[0]['operation']['accepted'] is True
    for label, run_id in (('q01-old-robot-stop.json',old['run_id']),
                          ('q01-fresh-robot-stop.json',new['run_id'])):
        receipt = load(root,label)
        assert receipt['accepted'] and receipt['status']=='succeeded'
        match = [r for r in records if r['command_id']==receipt['id']]
        assert len(match)==1 and match[0]['phase']=='terminal'
        assert match[0]['result']['status']=='Completed'
        identity = match[0]['operation']['identity']
        assert identity['primitive']=='robot.stop' and identity['kind']=='robot_stop'
        assert identity['target']=={'robot':{'robot_id':'default'}} and match[0]['operation']['accepted'] is True
        assert identity['admission']['run_id']==run_id
        assert identity['admission']['active_revision']==rev
    assert set(load(root,'q01-restart-response.json')['started']) == {'eos','config','bridge','egia'}
    order = load(root,'q01-step-order.json')
    times = [order[name] for name in ('old_robot_stop_ack_ns','restart_request_ns',
        'restart_reply_ns','fresh_mission_terminal_ns','fresh_robot_stop_ack_ns','final_stop_reply_ns')]
    assert times == sorted(times) and len(set(times))==len(times)
    assert load(root,'stop-response.json')['terminal']['outcome']=='confirmed'
    assert final['state']=='stopped' and final['processes']==[]
    assert load(root,'reap-proof.json')['all_absent'] is True
    reaped = load(root,'reap-proof.json')
    assert reaped['run_id']==new['run_id']
    assert reaped['pids']=={p['id']:p['pid'] for p in new['processes']}
    assert load(root,'q01-old-reap-proof.json')['pids']=={p['id']:p['pid'] for p in old['processes']}
    assert load(root,'logs-current-running.json')['scope']['run_id']==old['run_id']
    assert load(root,'q01-logs-old-current.json')['scope']['run_id']==old['run_id']
    assert load(root,'q01-logs-new-current.json')['scope']['run_id']==new['run_id']
    print('Q01_RESTART_SCOPED_VERIFIED_PASS', summary)


if __name__ == '__main__':
    main()
