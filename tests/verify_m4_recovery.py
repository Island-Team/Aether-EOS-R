#!/usr/bin/env python3
"""Verify archived A03 selected negative recovery evidence without a live home.

This verifies old-run receipt opacity, owner latch persistence, and dependent
stop. It does not claim a selected EGIA-only restart or same-run recovery.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import stat

PINS = {
    'isos': '2bdf6ea0ce8dbf72ee73e563d0151364c93ff0c0',
    'eos': 'c3aec9247634ff3cbbf3b4ede687c508f2c0f645',
    'egia': '475aa567247d6288c40160a2baafc04c8db65856',
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def read(root: Path, name: str):
    path = root / name
    require(path.is_file(), f'missing evidence: {name}')
    return json.loads(path.read_text())


def pid_map(status: dict) -> dict[str, int]:
    return {item['id']: item['pid'] for item in status['processes']}


def verify(root: Path) -> dict:
    if not (root / 'inputs.json').is_file():
        root = root / 'a03-recovery'
    parent = root.parent
    require((parent / 'mission.json').is_file() and (parent / 'journal').is_dir(),
            'A03 recovery must be verified with its parent baseline archive')
    inputs = read(root, 'inputs.json')
    prior_id = inputs['prior_operation_id']
    baseline = inputs['baseline_run_id']
    revision = inputs['revision']
    alias = inputs['alias']
    require(inputs['pins'] == PINS, 'archived replay pins differ from exact merged boundary')
    require(re.fullmatch(r'[0-9a-f]{64}', revision) is not None, 'invalid revision')
    require(inputs['selected_same_run_recovery'].startswith('unavailable:'),
            'selected same-run boundary was not disclosed')
    require(inputs['state_root'] == inputs['home'] + '/data/state/default/egia',
            'owner state root differs from selected IsOS deployment root')
    require(inputs['fault_injections'] == ['owner pending checkpoint',
                                          'owner safety latch',
                                          'managed EGIA child SIGKILL'],
            'fault injections were not labeled')

    correlation = read(root, 'prior-owner-correlation.json')
    require(len(correlation) == 2, 'prior mission must have one submit and one terminal event')
    require([event['event_code'] for event in correlation] ==
            ['egia_operation_submitted', 'egia_operation_terminal'],
            'prior mission event order differs')
    for event in correlation:
        require(event['source'] == 'egia' and event['run_id'] == baseline and
                event['correlation_id'] == prior_id and
                event['fields']['operation_id'] == prior_id,
                'prior mission event identity differs')
    require(correlation[-1]['fields']['outcome'] == 'succeeded',
            'prior mission did not succeed')

    record = read(root, 'prior-journal-record.json')
    identity = record['operation']['identity']
    admission = identity['admission']
    require(record['command_id'] == prior_id and record['phase'] == 'terminal' and
            record['result']['command_id'] == prior_id and
            record['result']['status'] == 'Completed', 'prior journal result differs')
    require(identity['kind'] == 'movement' and identity['primitive'] == 'base.velocity' and
            admission['primitive'] == 'base.velocity' and
            admission['run_id'] == baseline and admission['active_revision'] == revision and
            admission['ingress'] == 'grpc' and admission['authority_role'] == 'egia_control',
            'prior journal admission does not prove supervised EGIA movement')
    checkpoint = read(root, 'prior-mission-checkpoint.json')
    require(checkpoint['operation_id'] == prior_id and checkpoint['state'] == 'terminal' and
            checkpoint['outcome'] == 'succeeded' and checkpoint['mission_id'] and
            checkpoint['alias'] == alias and checkpoint['graph_revision'] == revision and
            checkpoint['primitive'] == 'base.velocity',
            'prior mission checkpoint differs from owner and journal evidence')
    mission = read(parent, 'mission.json')
    mission_id = checkpoint['mission_id']
    require(mission['command']['ok'] is True and
            mission['command']['mission']['mission_id'] == mission_id and
            mission['terminal']['mission_id'] == mission_id and
            mission['terminal']['state'] == 'succeeded',
            'prior checkpoint does not belong to the archived supervised mission')
    owner_path = root / 'egia-owner.raw.jsonl'
    require(owner_path.is_file(), 'missing raw EGIA owner event archive')
    owner_events = [json.loads(line) for line in owner_path.read_text().splitlines() if line]
    started = [event for event in owner_events
               if event.get('event') == 'mission_started' and
               event.get('mission_id') == mission_id and event.get('run_id') == baseline]
    require(len(started) == 1, 'archived owner stream lacks unique mission start')
    mission_correlation = started[0]['correlation_id']
    require(any(event.get('event') == 'primitive_command_reserved' and
                event.get('command_id') == prior_id and
                event.get('correlation_id') == mission_correlation and
                event.get('run_id') == baseline
                for event in owner_events),
            'archived mission did not reserve the prior EOS operation ID')
    require(any(event.get('event') == 'mission_finished' and
                event.get('mission_id') == mission_id and
                event.get('state') == 'succeeded' and
                event.get('correlation_id') == mission_correlation
                for event in owner_events),
            'archived supervised mission did not finish')

    opacity = read(root, 'opacity-status.json')
    opacity_run = opacity['run_id']
    require(opacity_run != baseline and opacity['state'] == 'running' and
            opacity['active_revision'] == revision and
            {'egia', 'eos'} <= pid_map(opacity).keys(),
            'new selected run was not active at old-ID lookup')
    lookup = read(root, 'old-id-new-run-lookup.json')
    require(lookup['operation_id'] == prior_id and lookup['old_run_id'] == baseline and
            lookup['new_run_id'] == opacity_run and
            lookup['lookup']['error'] == 'LinkPrimitiveError' and
            lookup['lookup']['status'] == 404 and
            lookup['lookup']['detail'] == 'unknown operation' and
            lookup['lookup']['discovery_health'] in {'ready', 'degraded'} and
            lookup['robot_id'] == checkpoint['eos_robot_id'],
            'old ID was not opaque under the new EOS run grant')
    expected_root = inputs['home'] + '/data/state/default'
    for name in ('opacity-egia-cmdline.json', 'latched-egia-cmdline.json'):
        argv = read(root, name)
        require(argv.count('--state-root') == 1 and
                argv[argv.index('--state-root') + 1] == expected_root and
                '--managed' in argv, f'{name} does not show selected managed owner root')
    stop = read(root, 'opacity-stop.json')
    stopped = read(root, 'opacity-stopped.json')
    require(stop['terminal']['run_id'] == opacity_run and
            stop['terminal']['outcome'] == 'confirmed' and
            stopped['run_id'] == opacity_run and stopped['state'] == 'stopped' and
            stopped['processes'] == [] and stopped['terminal'] == stop['terminal'],
            'opacity run was not confirmed stopped and reaped')

    injection = read(root, 'fault-injection.json')
    missing_id = injection['missing_operation_id']
    episode = injection['episode_id']
    require(injection['kind'] == 'owner_offline_pending_and_latch' and
            injection['prior_mission_operation_id'] == prior_id and
            injection['old_run_id'] == baseline and injection['revision'] == revision and
            missing_id != prior_id and episode, 'owner fault provenance differs')
    for name in ('injected-pending.json', 'injected-safety-history.json',
                 'injected-safety-latch.json'):
        mode = stat.S_IMODE((root / name).stat().st_mode)
        require(mode == 0o600, f'{name} was not archived as owner-private 0600')
    pending = read(root, 'injected-pending.json')
    history = read(root, 'injected-safety-history.json')
    latch = read(root, 'injected-safety-latch.json')
    require(pending['operation_id'] == missing_id and pending['state'] == 'pending' and
            pending['alias'] == alias and pending['primitive'] == 'base.velocity' and
            pending['graph_revision'] == revision and
            pending['eos_robot_id'] == checkpoint['eos_robot_id'],
            'injected owner pending checkpoint differs')
    require(history['episode_id'] == episode and history['graph_revision'] == revision and
            history['state'] == 'engaged' and latch['episode_id'] == episode and
            latch['graph_revision'] == revision and
            latch['identities'] == {alias: checkpoint['eos_robot_id']},
            'durable safety history/latch identity differs')

    latched = read(root, 'latched-status.json')
    latched_run = latched['run_id']
    require(latched_run not in {baseline, opacity_run} and
            latched['state'] == 'running' and latched['active_revision'] == revision and
            {'egia', 'eos'} <= pid_map(latched).keys(),
            'latched selected run did not rotate run/grant')
    events = read(root, 'latched-events.json')
    require(all(event['run_id'] == latched_run for event in events),
            'latched events contain another run')
    event_codes = [event['event_code'] for event in events]
    unknown = [i for i, event in enumerate(events)
               if event['event_code'] == 'egia_operation_unknown' and
               event['correlation_id'] == missing_id and
               event['fields'] == {'operation_id': missing_id, 'reason_code': 'receipt_missing'}]
    recovery = [i for i, event in enumerate(events)
                if event['event_code'] == 'egia_recovery_required' and
                event['correlation_id'] == missing_id and
                event['fields']['reason_code'] == 'unknown_outcome']
    refusal = [i for i, event in enumerate(events)
               if event['event_code'] == 'egia_safety_reset_refused' and
               event['fields']['reason_code'] == 'lookup_unresolved']
    require(len(unknown) == len(recovery) == len(refusal) == 1 and
            unknown[0] < recovery[0] < refusal[0] and
            'egia_safety_reset_requested' in event_codes,
            'startup did not preserve unknown receipt and refuse reset')
    require(not any(event['event_code'] == 'egia_operation_submitted' and
                    event.get('fields', {}).get('operation_id') == missing_id
                    for event in events), 'unresolved operation was resubmitted')

    raw_path = root / 'events.raw.jsonl'
    require(raw_path.is_file(), 'missing raw IsOS event archive')
    raw_events = [json.loads(line) for line in raw_path.read_text().splitlines() if line]
    require(all(event in raw_events for event in correlation + events),
            'curated owner events differ from raw IsOS event stream')
    require(any(event['event_code'] == 'deployment_stop_unconfirmed' and
                event['run_id'] == latched_run and
                event['fields'] == {'outcome': 'unknown',
                                    'reason_code': 'child_exit_unexpected'}
                for event in raw_events),
            'raw event stream lacks dependent unknown stop')

    fault = read(root, 'child-fault-injection.json')
    final = read(root, 'fault-status.json')
    require(fault == {'signal': 'SIGKILL', 'role': 'egia',
                      'pid': pid_map(latched)['egia'], 'run_id': latched_run},
            'fault injection was not the selected EGIA child')
    require(final['run_id'] == latched_run and final['state'] == 'degraded' and
            final['active_revision'] == revision and final['processes'] == [] and
            final['terminal']['run_id'] == latched_run and
            final['terminal']['outcome'] == 'unknown' and
            final['terminal']['reason_code'] == 'child_exit_unexpected' and
            {item['id'] for item in final['terminal']['processes']} ==
            set(pid_map(latched)) and
            all(item['outcome'] == 'unknown' and
                item['reason_code'] == 'child_exit_unexpected'
                for item in final['terminal']['processes']),
            'child fault did not yield unknown dependent stop and empty process set')

    before = read(root, 'journal-before.json')
    prior_name = prior_id.encode().hex() + '.rec'
    require(prior_name in before,
            'prior mission ID is absent from archived journal manifest')
    require(missing_id.encode().hex() + '.rec' not in before,
            'injected absent ID already existed in baseline journal')
    require(read(parent, 'journal-sha256.json') == before,
            'parent baseline journal manifest differs from recovery snapshot')
    archived_records = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                        for path in (parent / 'journal').glob('*.rec')}
    require(archived_records == before,
            'archived raw EOS records differ from baseline SHA-256 manifest')
    require(read(parent / 'journal', prior_name)['body'] == record,
            'prior decoded journal record differs from archived raw EOS bytes')
    for name in ('journal-after-old-id-lookup.json',
                 'journal-after-unresolved-lookup.json', 'journal-after.json'):
        require(read(root, name) == before,
                f'{name} added or changed an EOS journal record; no-replay unproven')
    baseline_reap = read(parent, 'reap-proof.json')
    baseline_status = read(parent, 'status-running.json')
    require(baseline_reap['run_id'] == baseline and
            baseline_reap['all_absent'] is True and
            baseline_reap['pids'] == pid_map(baseline_status),
            'baseline selected children lack archived reap proof')
    require(all(item['restarts'] == 0 for status in
                (baseline_status, opacity, latched) for item in status['processes']),
            'selected child restarted within one run')

    return {'result': 'M4_A03_SELECTED_NEGATIVE_VERIFIED_PASS',
            'prior_mission_operation_id': prior_id,
            'missing_operation_id': missing_id,
            'latch_episode_id': episode,
            'run_ids': [baseline, opacity_run, latched_run],
            'journal_records': len(before),
            'selected_same_run_positive': 'unqualified'}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path,
                        help='A03 recovery directory or its containing replay output')
    args = parser.parse_args()
    print(json.dumps(verify(args.archive), sort_keys=True))


if __name__ == '__main__':
    main()
