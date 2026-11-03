#!/usr/bin/env python3
"""A03 selected negative recovery replay within one live IsOS manager session.

Call after the baseline primitive and confirmed Stop, before `isos serve` exits.
An explicit new Run rotates the EOS grant and run ID, so its old operation ID
must stay opaque. IsOS R06 treats a managed EGIA child exit as an uncertain
dependent stop, and deliberately does not restart it within the same Run.
Owner checkpoints and child termination below are labeled fault injections.
EGIA owner tests cover receipt lookup without resubmit and durable checkpoint
storage separately; they do not establish a process-restart boundary.
"""

from __future__ import annotations

import asyncio
import json
import os
from pathlib import Path
import shutil
import signal
import stat
import subprocess
import sys
import time
import tomllib

from m4_affected_replay import api, save, sha

PINS = {
    'isos': '2bdf6ea0ce8dbf72ee73e563d0151364c93ff0c0',
    'eos': 'c3aec9247634ff3cbbf3b4ede687c508f2c0f645',
    'egia': '475aa567247d6288c40160a2baafc04c8db65856',
}


def wait_for(predicate, *, timeout=120):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.2)
    raise AssertionError('selected process state did not converge before deadline')


def selected_state_root(home, graph):
    settings = graph['cores']['isos']['layers']['process']['groups']['component']['nodes']['egia']['settings']
    assert not any(arg == '--state-root' or arg.startswith('--state-root=')
                   for arg in settings['args']), 'graph authored the owner-reserved state root'
    assert 'ISLAND_STATE' not in settings.get('env', {}), 'graph authored ISLAND_STATE'
    assert 'ISLAND_STATE' not in settings.get('env_from', {}), 'graph authored ISLAND_STATE target'
    root = home / 'data/state/default'
    for path in (home / 'data', home / 'data/state', root, root / 'egia'):
        info = path.lstat()
        assert stat.S_ISDIR(info.st_mode) and info.st_uid == os.getuid(), (
            'selected state path is not an owner directory', path)
        assert stat.S_IMODE(info.st_mode) == 0o700, (
            'selected state path is not private', path, oct(stat.S_IMODE(info.st_mode)))
    return root / 'egia'


def check_selected_child_state(pid, home):
    expected = str(home / 'data/state/default')
    argv = [part.decode('utf-8') for part in
            Path(f'/proc/{pid}/cmdline').read_bytes().rstrip(b'\0').split(b'\0')]
    assert argv.count('--state-root') == 1, ('selected EGIA state root missing or duplicated', argv)
    position = argv.index('--state-root')
    assert position + 1 < len(argv) and argv[position + 1] == expected, (
        'selected EGIA state root differs from IsOS deployment root', argv, expected)
    inherited = Path(f'/proc/{pid}/environ').read_bytes().split(b'\0')
    assert not any(value.startswith(b'ISLAND_STATE=') for value in inherited), (
        'selected EGIA inherited ISLAND_STATE', pid)
    return argv


def events(home, run_id):
    raw = home / 'data/logs/default/events.log'
    if not raw.exists():
        return []
    parsed = [json.loads(line) for line in raw.read_text().splitlines() if line]
    return [event for event in parsed if event.get('run_id') == run_id]


def journal_hashes(home):
    root = home / 'data/journal/default'
    return {path.name: sha(path) for path in sorted(root.glob('*.rec'))}


def assert_no_motion_replay(home, before, record_name, *, missing_id=None):
    """Preserve the old receipt and permit only nonmovement new records."""
    current = journal_hashes(home)
    assert all(current.get(name) == digest for name, digest in before.items()), (
        'baseline EOS journal record changed', record_name)
    if missing_id is not None:
        missing_record = missing_id.encode('utf-8').hex() + '.rec'
        assert missing_record not in current, ('unresolved ID reached EOS journal', missing_id)
    root = home / 'data/journal/default'
    for name in current.keys() - before.keys():
        body = json.loads((root / name).read_text())['body']
        kind = body.get('operation', {}).get('identity', {}).get('kind')
        assert kind != 'movement', ('new movement was admitted during recovery', name)
    return current


def process_pids(status):
    return {item['id']: item['pid'] for item in status['processes']}


def running_status(api_port):
    status = api(api_port, 'process-status')
    return status if status['state'] == 'running' and {'egia', 'eos'} <= set(process_pids(status)) else None


def stopped(api_port, revision, prior_pids):
    response = api(api_port, 'process-control', 'POST',
                   {'schema_version': 1, 'operation': 'stop', 'revision': revision})
    assert response['terminal']['outcome'] == 'confirmed', response
    state = wait_for(lambda: (s if s['state'] != 'running' else None)
                     if (s := api(api_port, 'process-status')) else None, timeout=45)
    for pid in prior_pids.values():
        assert not Path(f'/proc/{pid}').exists(), ('child was not reaped', pid)
    return response, state


async def inspect_eos(egia_root, grpc_port, grant, revision, operation_id, alias):
    sys.path.insert(0, str(egia_root / 'src'))
    from egia.core.link.client import EosClient, LinkPrimitiveError
    from egia.lib.config import GatewayConfig

    os.environ['EGIA_EOS_CONTROL_GRANT_FILE'] = str(grant)
    client = EosClient(GatewayConfig(host='127.0.0.1', grpc_port=grpc_port),
                       backend=alias, expected_revision=revision)
    try:
        discovery = await client.initialize()
        assert client.active_revision == revision and discovery.health in {'ready', 'degraded'}, (
            client.active_revision, discovery.health)
        assert discovery.robot_id and any(
            item.name == 'base.velocity' and item.device_id == 'drive'
            for item in client.primitives), 'complete EOS movement binding is absent'
        try:
            receipt = await client.recover_operation(
                operation_id, 'base.velocity', 'drive',
                eos_robot_id=discovery.robot_id, graph_revision=revision)
        except LinkPrimitiveError as error:
            return discovery.robot_id, {'error': type(error).__name__,
                                        'status': error.status, 'detail': str(error),
                                        'discovery_health': discovery.health}
        return discovery.robot_id, {'status': receipt.status.value,
                                    'discovery_health': discovery.health}
    finally:
        await client.aclose()


def launch(bin_path, env, log):
    handle = log.open('w')
    process = subprocess.Popen([str(bin_path), 'run'], env=env,
                               stdout=handle, stderr=subprocess.STDOUT, text=True)
    return process, handle


def terminate(process):
    if process is None or process.poll() is not None:
        return
    process.send_signal(signal.SIGINT)
    try:
        process.wait(timeout=15)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def selected_alias(home, revision):
    selection = json.loads((home / 'data/generated/default/selection.json').read_text())
    generation = selection['entries'][revision]['generation']
    profile = home / 'data/generated/default/v2' / revision / generation / 'egia/profiles/isos.toml'
    backends = tomllib.loads(profile.read_text())['modes']['isos']['backends']
    assert len(backends) == 1 and isinstance(backends[0]['id'], str), backends
    return backends[0]['id']


def run_in_session(*, out, home, graph, revision, prior_operation_id,
                   api_port, grpc_port, isos_bin, env, egia_root, baseline_run_id):
    """Qualify selected old-ID opacity, durable latch and dependent fault stop.

    The baseline Stop must be confirmed and the same IsOS manager must remain
    alive. Selected same-run process recovery is unqualified because this IsOS
    pin has no EGIA-only restart operation.
    """
    out = Path(out).resolve()
    home = Path(home).resolve()
    egia_root = Path(egia_root).resolve()
    isos_bin = Path(isos_bin).resolve()
    out.mkdir(parents=True, exist_ok=False)
    assert env['ISOS_HOME'] == str(home)
    assert subprocess.check_output(['git', '-C', str(egia_root), 'rev-parse', 'HEAD'], text=True).strip() == PINS['egia']
    assert api(api_port, 'node-graph')['revision'] == revision
    assert api(api_port, 'process-status')['state'] != 'running', 'baseline Stop is incomplete'
    state_root = selected_state_root(home, graph)
    alias = selected_alias(home, revision)
    before_journal = journal_hashes(home)
    old_record = prior_operation_id.encode('utf-8').hex() + '.rec'
    assert old_record in before_journal, ('baseline EOS journal ID is absent', prior_operation_id)
    owner_correlation = [event for event in events(home, baseline_run_id)
                         if event.get('source') == 'egia'
                         and event.get('event_code') in {
                             'egia_operation_submitted', 'egia_operation_terminal'}
                         and event.get('fields', {}).get('operation_id') == prior_operation_id
                         and event.get('correlation_id') == prior_operation_id]
    assert {event['event_code'] for event in owner_correlation} == {
        'egia_operation_submitted', 'egia_operation_terminal'}, (
            'prior ID is not a supervised EGIA operation', prior_operation_id)
    prior_journal = json.loads((home / 'data/journal/default' / old_record).read_text())['body']
    identity = prior_journal['operation']['identity']
    admission = identity['admission']
    assert prior_journal['command_id'] == prior_operation_id
    assert prior_journal['phase'] == 'terminal'
    assert identity['primitive'] == 'base.velocity' and identity['kind'] == 'movement'
    assert admission['run_id'] == baseline_run_id and admission['ingress'] == 'grpc'
    assert admission['authority_role'] == 'egia_control'
    assert admission['active_revision'] == revision
    save(out, 'inputs.json', {'pins': PINS, 'home': str(home), 'state_root': str(state_root),
                              'revision': revision, 'prior_operation_id': prior_operation_id,
                              'alias': alias, 'baseline_run_id': baseline_run_id,
                              'isos_bin_sha256': sha(isos_bin), 'api_port': api_port,
                              'grpc_port': grpc_port,
                              'fault_injections': ['owner pending checkpoint', 'owner safety latch',
                                                   'managed EGIA child SIGKILL'],
                              'selected_same_run_recovery': 'unavailable: IsOS R06 does not restart one child'})
    save(out, 'journal-before.json', before_journal)
    save(out, 'prior-owner-correlation.json', owner_correlation)
    save(out, 'prior-journal-record.json', prior_journal)
    running = None
    run_handle = None
    last_status = None
    try:
        # New run: the old EOS record is durable, but the new grant cannot see it.
        running, run_handle = launch(isos_bin, env, out / 'opacity-run.log')
        first = wait_for(lambda: running_status(api_port))
        last_status = first
        assert first['run_id'] != baseline_run_id
        save(out, 'opacity-status.json', first)
        save(out, 'opacity-egia-cmdline.json', check_selected_child_state(process_pids(first)['egia'], home))
        grant = home / 'run/control-grants/default' / first['run_id'] / 'egia-control.grant'
        robot_id, lookup = asyncio.run(inspect_eos(egia_root, grpc_port, grant,
                                                   revision, prior_operation_id, alias))
        assert lookup.get('status') == 404, ('old-run operation was exposed', lookup)
        save(out, 'old-id-new-run-lookup.json', {'operation_id': prior_operation_id,
                                                 'old_run_id': baseline_run_id,
                                                 'new_run_id': first['run_id'],
                                                 'lookup': lookup, 'robot_id': robot_id})
        save(out, 'journal-after-old-id-lookup.json',
             assert_no_motion_replay(home, before_journal, old_record))
        first_stop, first_stopped = stopped(api_port, revision, process_pids(first))
        save(out, 'opacity-stop.json', first_stop)
        save(out, 'opacity-stopped.json', first_stopped)
        running.wait(timeout=15)
        run_handle.close()
        running = None
        run_handle = None
        last_status = None

        # Owner-controlled fault injection, only while EGIA is stopped.
        sys.path.insert(0, str(egia_root / 'src'))
        from egia.lib.operation_ledger import OperationLedger, PendingOperation
        from egia.lib.safety_state import SafetyState, SafetyStateError
        ledger = OperationLedger(state_root)
        assert ledger.get(prior_operation_id) is not None, 'mission checkpoint was not retained'
        mission_checkpoint = json.loads((ledger.root / f'{prior_operation_id}.json').read_text())
        assert mission_checkpoint['state'] == 'terminal'
        assert mission_checkpoint['outcome'] == 'succeeded'
        assert mission_checkpoint['alias'] == alias
        assert mission_checkpoint['eos_robot_id'] == robot_id
        save(out, 'prior-mission-checkpoint.json', mission_checkpoint)
        missing_id = f'm4-missing-{time.time_ns()}'
        assert ledger.get(missing_id) is None
        ledger.reserve(PendingOperation(missing_id, alias, 'base.velocity',
                                        'drive', None, robot_id, revision))
        pending_path = ledger.root / f'{missing_id}.json'
        assert stat.S_IMODE(pending_path.stat().st_mode) == 0o600
        shutil.copy2(pending_path, out / 'injected-pending.json')
        safety = SafetyState(state_root / 'safety')
        assert not safety.aliases(), 'selected home already has a safety latch'
        safety.engage({alias: robot_id}, revision)
        episode = safety.episode()
        assert episode is not None
        for name in ('safety-history.json', 'safety-latch.json'):
            owner_file = safety.root / name
            assert stat.S_IMODE(owner_file.stat().st_mode) == 0o600
            shutil.copy2(owner_file, out / ('injected-' + name))
        try:
            safety.authorize_reset(frozenset({alias}), episode[0], 'f' * 64)
        except SafetyStateError:
            pass
        else:
            raise AssertionError('wrong-revision reset was accepted')
        safety.authorize_reset(frozenset({alias}), *episode)
        assert stat.S_IMODE((safety.root / 'safety-reset.json').stat().st_mode) == 0o600
        save(out, 'fault-injection.json', {'kind': 'owner_offline_pending_and_latch',
                                           'missing_operation_id': missing_id,
                                           'prior_mission_operation_id': prior_operation_id,
                                           'episode_id': episode[0], 'revision': revision,
                                           'old_run_id': baseline_run_id})

        # The selected child consumes a reset request only after complete EOS
        # discovery; the injected absent receipt stays unresolved and reset is refused.
        running, run_handle = launch(isos_bin, env, out / 'latched-run.log')
        second = wait_for(lambda: running_status(api_port))
        last_status = second
        assert second['run_id'] not in {baseline_run_id, first['run_id']}
        save(out, 'latched-status.json', second)
        save(out, 'latched-egia-cmdline.json', check_selected_child_state(process_pids(second)['egia'], home))
        wait_for(lambda: not (safety.root / 'safety-reset.json').exists(), timeout=45)
        assert safety.episode() == episode and safety.identities() == {alias: robot_id}
        assert missing_id in {item.operation_id for item in ledger.pending()}
        observed = wait_for(lambda: (found if (
            any(event['event_code'] == 'egia_operation_unknown' and
                event.get('fields', {}).get('operation_id') == missing_id for event in found)
            and any(event['event_code'] == 'egia_safety_reset_refused' for event in found)
        ) else None) if (found := events(home, second['run_id'])) else None, timeout=45)
        save(out, 'latched-events.json', observed)
        assert not any(event['event_code'] == 'egia_operation_submitted' and
                       event.get('fields', {}).get('operation_id') == missing_id
                       for event in observed), 'unresolved ID was resubmitted'
        save(out, 'journal-after-unresolved-lookup.json',
             assert_no_motion_replay(home, before_journal, old_record,
                                     missing_id=missing_id))

        # An EGIA child fault is a dependent-stop event, not an automatic child
        # replacement. Trigger only after the latch has blocked movement.
        egia_pid = process_pids(second)['egia']
        os.kill(egia_pid, signal.SIGKILL)
        save(out, 'child-fault-injection.json', {'signal': 'SIGKILL', 'role': 'egia',
                                                 'pid': egia_pid, 'run_id': second['run_id']})
        failed = wait_for(lambda: (s if s['state'] != 'running' else None)
                          if (s := api(api_port, 'process-status')) else None, timeout=45)
        save(out, 'fault-status.json', failed)
        assert failed['run_id'] == second['run_id']
        assert failed['state'] != 'running'
        assert failed['terminal']['outcome'] == 'unknown', failed['terminal']
        assert not Path(f'/proc/{egia_pid}').exists(), 'faulted EGIA child was not reaped'
        for pid in process_pids(second).values():
            assert not Path(f'/proc/{pid}').exists(), ('dependent child was not reaped', pid)
        assert_no_motion_replay(home, before_journal, old_record, missing_id=missing_id)
        assert safety.episode() == episode
        assert missing_id in {item.operation_id for item in ledger.pending()}
        save(out, 'journal-after.json', journal_hashes(home))
        print('M4_A03_SELECTED_NEGATIVE_PASS', prior_operation_id, missing_id, episode[0], flush=True)
        return {'old_id_lookup_status': 404, 'unresolved_operation_id': missing_id,
                'prior_mission_operation_id': prior_operation_id,
                'latch_episode_id': episode[0], 'faulted_run_id': second['run_id'],
                'selected_same_run_positive': 'unqualified: no supervised single-child restart'}
    finally:
        if last_status is not None:
            try:
                current = api(api_port, 'process-status')
                if current['state'] == 'running':
                    cleanup, cleanup_state = stopped(api_port, revision, process_pids(current))
                    save(out, 'cleanup-stop.json', cleanup)
                    save(out, 'cleanup-stopped.json', cleanup_state)
            except Exception as error:
                save(out, 'cleanup-error.json', {'error': type(error).__name__,
                                                  'detail': str(error)[:1000]})
        terminate(running)
        if run_handle is not None:
            run_handle.close()
        raw = home / 'data/logs/default/events.log'
        if raw.is_file():
            shutil.copy2(raw, out / 'events.raw.jsonl')
        owner = state_root / 'egia.jsonl'
        if owner.is_file():
            shutil.copy2(owner, out / 'egia-owner.raw.jsonl')
