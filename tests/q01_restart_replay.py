"""Scoped Q01 selected model fault followed by explicit new-run IsOS Restart."""
import asyncio
import hashlib
import json
import os
from pathlib import Path
import time

from m4_affected_replay import api, exercise_mission, save, sha


def events(home):
    path = home/'data/logs/default/events.log'
    return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []


def records(home):
    root = home/'data/journal/default'
    return {path.name: sha(path) for path in root.glob('*.rec')}


def archive_control(out, home, phase):
    source = home/'data/journal/default/.control'
    raw = source.read_bytes()
    body = json.loads(raw)['body']
    raw_name = f'q01-control-{phase}.raw.json'
    (out/raw_name).write_bytes(raw)
    save(out, f'q01-control-{phase}.json', {
        'raw_file':raw_name,
        'sha256':hashlib.sha256(raw).hexdigest(),
        'body':body,
    })
    return body


def proposal(items, mission, run_id):
    found = [event['fields']['operation_id'] for event in items
             if event.get('event_code') == 'egia_proposal_created'
             and event.get('run_id') == run_id
             and event.get('fields', {}).get('mission_id') == mission['terminal']['mission_id']]
    assert len(found) == 1, (mission['terminal']['mission_id'], found)
    return found[0]


async def owner_client(grpc_port, grant, revision):
    from egia.core.link.client import EosClient
    from egia.lib.config import GatewayConfig
    os.environ['EGIA_EOS_CONTROL_GRANT_FILE'] = str(grant)
    client = EosClient(GatewayConfig(host='127.0.0.1', grpc_port=grpc_port),
                       backend='default', expected_revision=revision)
    await client.initialize(force=True)
    assert client.active_revision == revision
    assert any(p.name == 'base.velocity' and p.device_id == 'drive' for p in client.primitives)
    return client


async def stop_robot(grpc_port, grant, revision, label):
    client = await owner_client(grpc_port, grant, revision)
    try:
        ack = await client.call_primitive('robot.stop', '', {}, command_id=f'q01-{label}-{time.time_ns()}')
        assert ack.accepted and ack.status.value == 'succeeded', ack
        return {'id': ack.command_id, 'accepted': ack.accepted, 'status': ack.status.value}
    finally:
        await client.aclose()


async def old_access(grpc_port, new_grant, old_grant_copy, revision, old_id):
    from egia.core.link.client import EosClient, LinkPrimitiveError
    from egia.lib.config import GatewayConfig
    client = await owner_client(grpc_port, new_grant, revision)
    try:
        try:
            await client.recover_operation(old_id, 'base.velocity', 'drive',
                                           eos_robot_id=client.discovery.robot_id, graph_revision=revision)
            raise AssertionError('old-run operation unexpectedly visible')
        except LinkPrimitiveError as exc:
            assert exc.status == 404, (exc.status, str(exc))
            lookup = {'status': exc.status, 'operation_id': old_id}
    finally:
        await client.aclose()
    os.environ['EGIA_EOS_CONTROL_GRANT_FILE'] = str(old_grant_copy)
    stale = EosClient(GatewayConfig(host='127.0.0.1', grpc_port=grpc_port),
                      backend='default', expected_revision=revision)
    try:
        try:
            await stale.initialize(force=True)
            await stale.recover_operation(old_id, 'base.velocity', 'drive',
                                          eos_robot_id=stale.discovery.robot_id, graph_revision=revision)
            raise AssertionError('retired old grant accepted')
        except LinkPrimitiveError as exc:
            assert exc.status == 401, (exc.status, str(exc))
            stale_result = {'status': exc.status}
    finally:
        await stale.aclose()
    return lookup, stale_result


async def await_planner_catalog(port):
    import websockets
    deadline = time.monotonic()+90
    async with websockets.connect(f'ws://127.0.0.1:{port}/api/aether/egia/primitives') as stream:
        while True:
            remaining = deadline-time.monotonic()
            assert remaining>0, 'new EGIA planner catalog lacks ready base.velocity robot-a/drive'
            snapshot = json.loads(await asyncio.wait_for(stream.recv(),timeout=remaining))
            if any(item.get('name')=='base.velocity' and item.get('robot_id')=='robot-a'
                   and item.get('device_id')=='drive' and item.get('health')=='ready'
                   for item in snapshot.get('primitives',[])):
                return snapshot


def run_restart(*, out, home, api_port, grpc_port, mission_port, revision,
                first_status, first_mission, model_stub, first_grant, selected_egia_config,
                factory_egia_defaults):
    assert first_mission['terminal']['state'] == 'succeeded'
    old_run = first_status['run_id']
    old_id = proposal(events(home), first_mission, old_run)
    graph_path = home/'config/deployments/default.nodes.json'
    selection_path = home/'data/generated/default/selection.json'
    graph_digest, selection_digest = sha(graph_path), sha(selection_path)
    assert graph_digest == revision
    (out/'q01-selection.raw.json').write_bytes(selection_path.read_bytes())
    before_fault = records(home)
    owner_before_fault = events(home)
    submitted_before_fault = {e.get('fields',{}).get('operation_id') for e in owner_before_fault
                              if e.get('event_code')=='egia_operation_submitted'}
    assert any(path == old_id.encode().hex()+'.rec' for path in before_fault), old_id
    save(out, 'q01-journal-before-fault.json', before_fault)
    old_grant_bytes = first_grant.read_bytes()
    old_grant_hash = hashlib.sha256(old_grant_bytes).hexdigest()
    old_pids = {p['id']:p['pid'] for p in first_status['processes']}
    # A successful but invalid completion exercises EGIA's bounded controller parser.
    model_stub.set_mode('invalid')
    fault = asyncio.run(exercise_mission(mission_port, f'q01-fault-{old_run}'))
    save(out, 'q01-fault-mission.json', fault)
    assert fault['terminal']['state'] == 'failed', fault['terminal']
    assert not any(step.get('actions') for step in fault['terminal']['document']['plan']['steps'])
    from egia.lib.config import load_launch_config
    resolved = load_launch_config(str(selected_egia_config), mode='isos',
                                  defaults=factory_egia_defaults)
    limit = resolved.decision.max_invalid_replies
    (out/'q01-selected-egia.toml').write_bytes((selected_egia_config/'egia.toml').read_bytes())
    (out/'q01-factory-egia.toml').write_bytes((factory_egia_defaults/'egia.toml').read_bytes())
    save(out,'q01-config-hashes.json',{'selected_sha256':sha(selected_egia_config/'egia.toml'),
         'factory_sha256':sha(factory_egia_defaults/'egia.toml')})
    assert isinstance(limit, int) and 1 <= limit <= 20
    requests = model_stub.snapshot()
    fault_requests = [entry for entry in requests if entry.get('mode') == 'invalid' and entry.get('reply') == 'INVALID_Q01_REPLY']
    assert len(fault_requests) >= limit, (limit, fault_requests)
    owner_path = home/'data/state/default/egia/egia.jsonl'
    fault_id = fault['terminal']['mission_id']
    selected_owner_lines = [line for line in owner_path.read_text().splitlines()
        if line.startswith('{') and (entry:=json.loads(line)).get('mission_id') == fault_id
        and entry.get('run_id') == old_run and entry.get('event') in
        {'step_parse_failed','invalid_reply_limit_reached'}]
    owner_bytes = ('\n'.join(selected_owner_lines)+'\n').encode()
    (out/'q01-egia-owner-fault.raw.jsonl').write_bytes(owner_bytes)
    save(out,'q01-egia-owner-fault-sha256.json',{'sha256':hashlib.sha256(owner_bytes).hexdigest()})
    owner_lines = [json.loads(line) for line in selected_owner_lines]
    parse_failures = [line for line in owner_lines if line.get('event') == 'step_parse_failed'
                      and line.get('mission_id') == fault_id]
    limit_events = [line for line in owner_lines if line.get('event') == 'invalid_reply_limit_reached'
                    and line.get('mission_id') == fault_id]
    assert [line['attempt'] for line in parse_failures] == list(range(1,limit+1)), parse_failures
    assert all(line['reply_preview']=='INVALID_Q01_REPLY' for line in parse_failures)
    assert len(limit_events) == 1 and limit_events[0]['attempts'] == limit, limit_events
    save(out,'q01-planner-fault-events.json',parse_failures+limit_events)
    save(out, 'q01-model-fault.json', {'limit':limit, 'requests':fault_requests,
          'transitions':model_stub.snapshot_transitions()})
    old_events = events(home)
    submitted_after_fault = {e.get('fields',{}).get('operation_id') for e in old_events
                             if e.get('event_code')=='egia_operation_submitted'}
    assert submitted_after_fault == submitted_before_fault, (submitted_before_fault,submitted_after_fault)
    save(out,'q01-submitted-before-fault.json',sorted(submitted_before_fault))
    assert records(home) == before_fault, 'fault mission created an EOS journal record'
    save(out,'q01-events-before-restart.json',old_events)
    save(out,'q01-logs-old-current.json',api(api_port,'logs?view=current&source=all&level=info'))
    old_stop = asyncio.run(stop_robot(grpc_port, first_grant, revision, 'old-stop'))
    old_stop_ack_ns = time.time_ns()
    save(out,'q01-old-robot-stop.json',old_stop)
    before_restart = records(home)
    save(out,'q01-journal-before-restart.json',before_restart)
    archive_control(out,home,'before-restart')
    model_stub.set_mode('normal')
    save(out,'q01-model-repair.json',{'transitions':model_stub.snapshot_transitions(),
                                      'requests_before_repair':len(model_stub.snapshot())})
    restart_request_ns = time.time_ns()
    restart = api(api_port,'process-control','POST',
                  {'schema_version':1,'operation':'restart','revision':revision},timeout=600)
    restart_reply_ns = time.time_ns()
    save(out,'q01-restart-response.json',restart)
    assert set(restart['started']) == {'eos','config','bridge','egia'}, restart
    from real_home import await_condition
    fresh = await_condition(lambda: (s if s['state']=='running' and s['run_id'] != old_run and
                            {'eos','egia','config','bridge'} <= {p['id'] for p in s['processes']} else None)
                            if (s:=api(api_port,'process-status')) else None, timeout=180)
    assert fresh['active_revision'] == revision
    assert sha(graph_path) == graph_digest and sha(selection_path) == selection_digest
    save(out,'q01-revision-pins.json',{'saved_graph_sha256':graph_digest,
                                      'selection_sha256':selection_digest,'active_revision':revision})
    new_run = fresh['run_id']
    new_pids = {p['id']:p['pid'] for p in fresh['processes']}
    assert all(pid not in new_pids.values() for pid in old_pids.values()), (old_pids,new_pids)
    assert all(not Path(f'/proc/{pid}').exists() for pid in old_pids.values()), old_pids
    save(out,'q01-status-new-running.json',fresh)
    old_cursor_file = out/'q01-logs-old-unfiltered.json'
    if old_cursor_file.is_file():
        from urllib.parse import urlencode
        cursor = json.loads(old_cursor_file.read_text())['next_cursor']
        changed = api(api_port,'logs?'+urlencode({'view':'current','source':'all',
                                                  'cursor':cursor}))
        assert changed['scope']['run_id']==new_run
        assert any(item['code']=='log_run_changed' for item in changed['diagnostics'])
        save(out,'q01-logs-run-changed.json',changed)
    archive_control(out,home,'after-restart')
    save(out,'q01-old-reap-proof.json',{'run_id':old_run,'pids':old_pids,'all_absent':True,
                                      'capture_phase':'after_restart_before_new_mission','observed_unix_s':time.time()})
    new_grant = home/'run/control-grants/default'/new_run/'egia-control.grant'
    assert new_grant.exists() and new_grant.stat().st_mode & 0o777 == 0o600
    new_grant_hash = sha(new_grant)
    assert old_grant_hash != new_grant_hash
    save(out,'q01-grant-digests.json',{'old_run':old_run,'new_run':new_run,
         'old_sha256':old_grant_hash,'new_sha256':new_grant_hash})
    old_grant_copy = home/'run/q01-retired-grant.probe'
    old_grant_copy.write_bytes(old_grant_bytes); old_grant_copy.chmod(0o600)
    try:
        lookup, stale = asyncio.run(old_access(grpc_port,new_grant,old_grant_copy,revision,old_id))
    finally:
        old_grant_copy.unlink()
    save(out,'q01-old-id-lookup.json',lookup)
    save(out,'q01-retired-grant.json',stale)
    after_lookup = records(home)
    for name, digest in before_restart.items():
        assert after_lookup.get(name) == digest, name
    assert old_id.encode().hex()+'.rec' not in set(after_lookup)-set(before_restart)
    for name in set(after_lookup)-set(before_restart):
        body = json.loads((home/'data/journal/default'/name).read_text())['body']
        assert body['operation']['identity']['kind'] != 'movement', name
    save(out,'q01-journal-after-lookup.json',after_lookup)
    new_events = events(home)
    stopped = [i for i,e in enumerate(new_events) if e.get('event_code')=='deployment_stopped' and
               e.get('run_id')==old_run and e.get('fields',{}).get('outcome')=='confirmed']
    starting = [i for i,e in enumerate(new_events) if e.get('event_code')=='deployment_starting' and e.get('run_id')==new_run]
    assert stopped and starting and max(stopped) < min(starting)
    assert any(e.get('event_code')=='egia_ready' and e.get('run_id')==new_run for e in new_events)
    owner_log = home/'data/state/default/egia/egia.jsonl'
    discovered = [json.loads(line) for line in owner_log.read_text().splitlines()
                  if line.startswith('{') and (e:=json.loads(line)).get('run_id')==new_run
                  and e.get('event')=='eos_discovered' and e.get('robot')=='robot-a'
                  and e.get('primitives',0)>=4]
    assert discovered, 'new EGIA process did not publish complete EOS discovery'
    catalog = asyncio.run(await_planner_catalog(mission_port))
    catalog_observed_unix_ns = time.time_ns()
    save(out,'q01-new-hardware-ready.json',{'eos_discovery':discovered[-1],
                                            'planner_catalog':catalog,
                                            'catalog_observed_unix_ns':catalog_observed_unix_ns,
                                            'observed_run_id':new_run,
                                            'observed_egia_pid':new_pids['egia']})
    save(out,'q01-events-after-restart.json',new_events)
    save(out,'q01-logs-new-current.json',api(api_port,'logs?view=current&source=all&level=info'))
    fresh_mission = asyncio.run(exercise_mission(mission_port, f'q01-fresh-{new_run}'))
    fresh_mission_terminal_ns = time.time_ns()
    save(out,'q01-fresh-mission.json',fresh_mission)
    assert fresh_mission['terminal']['state']=='succeeded'
    assert any(step.get('actions') for step in fresh_mission['terminal']['document']['plan']['steps'])
    new_id = proposal(events(home),fresh_mission,new_run)
    assert new_id != old_id
    fresh_stop = asyncio.run(stop_robot(grpc_port,new_grant,revision,'fresh-stop'))
    fresh_stop_ack_ns = time.time_ns()
    save(out,'q01-fresh-robot-stop.json',fresh_stop)
    stop = api(api_port,'process-control','POST',
               {'schema_version':1,'operation':'stop','revision':revision},timeout=600)
    final_stop_reply_ns = time.time_ns()
    assert stop['terminal']['outcome']=='confirmed', stop
    final = api(api_port,'process-status')
    assert final['state']!='running' and not final['processes']
    save(out,'q01-final-status.json',final)
    archive_control(out,home,'after-final-stop')
    save(out,'q01-summary.json',{'old_run':old_run,'new_run':new_run,'old_operation_id':old_id,
         'new_operation_id':new_id,'fault_mission_id':fault['terminal']['mission_id'],
         'old_grant_sha256':old_grant_hash,'new_grant_sha256':new_grant_hash})
    save(out,'q01-step-order.json',{'old_robot_stop_ack_ns':old_stop_ack_ns,
         'restart_request_ns':restart_request_ns,'restart_reply_ns':restart_reply_ns,
         'fresh_mission_terminal_ns':fresh_mission_terminal_ns,
         'fresh_robot_stop_ack_ns':fresh_stop_ack_ns,'final_stop_reply_ns':final_stop_reply_ns})
    return {'movement':{'id':new_id,'status':'succeeded','accepted':True},'stop':fresh_stop},stop,fresh
