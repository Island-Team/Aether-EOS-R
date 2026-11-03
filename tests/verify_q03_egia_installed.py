#!/usr/bin/env python3
"""Seal and independently verify the supplemental installed EGIA process replay."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load(path):
    return json.loads(path.read_text())


def files_under(root):
    result = {}
    for path in root.rglob('*'):
        assert not path.is_symlink(), path
        if path.is_file():
            result[path.relative_to(root).as_posix()] = digest(path)
    return result


def copy_selected(root, inputs):
    prefix = Path(inputs['prefix'])
    snapshot = root/'installed-prefix-snapshot'
    assert not snapshot.exists()
    snapshot.mkdir()
    names = set(inputs['cmake_installed_files'])
    payload = load(prefix/'q03-wheel-payload.json')
    names.update('lib/island/'+name for name in payload['files'])
    wheel = next((prefix/'wheels').glob('egia-0.1.0-*.whl'))
    names.update(('q03-wheel-payload.json', 'wheels/'+wheel.name))
    for name in sorted(names):
        source = prefix/name
        assert source.is_file() and not source.is_symlink(), source
        target = snapshot/name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source,target)
    harness = Path(__file__).with_name('m4_affected_replay.py')
    assert digest(harness) == inputs['harness_sha256']
    (root/'harness').mkdir()
    shutil.copy2(harness,root/'harness/m4_affected_replay.py')


def verify(root, inputs, *, sealed, expected_evidence_sha256=None):
    assert inputs['schema_version'] == 1 and inputs['profile'] == 'q03-egia-installed-process'
    prefix = Path(inputs['prefix'])
    assert load(root/'q03-egia-installed-inputs.json') == inputs
    if sealed:
        assert expected_evidence_sha256 and digest(root/'evidence.sha256.json') == expected_evidence_sha256
        manifest = load(root/'evidence.sha256.json')
        actual = files_under(root)
        actual.pop('evidence.sha256.json')
        assert actual == manifest, 'archive file set or digest differs'
    chain = load(root/'q03-egia-installed-chain.json')
    child = load(root/'q03-egia-installed-child.json')
    wheel_receipt = load(root/'q03-egia-installed-wheel.json')
    assert chain['mode'] == 'installed' and chain['prefix'] == str(prefix)
    assert chain['binding']['PYTHONPATH'] == ''
    assert chain['binding']['ISLAND_PREFIX'] == str(prefix)
    assert chain['binding']['ISLAND_LIB'] == str(prefix/'lib/island')
    assert chain['interpreter'] == str(prefix/'.venv/bin/python')
    assert chain['code'] and all(Path(item['root']).is_relative_to(prefix)
                                 for item in chain['code'])
    assert digest(root/'harness/m4_affected_replay.py') == inputs['harness_sha256']
    assert child['binding']['PYTHONPATH'] == (
        str(prefix/'lib/island')+':'+str(prefix/'libexec/island'))
    assert child['binding']['EGIA_PYTHON'] == chain['interpreter']
    assert str(prefix/'.venv/bin/python') in child['argv']
    assert child['binding']['ISLAND_PREFIX'] == str(prefix)
    assert child['binding']['ISLAND_LIB'] == str(prefix/'lib/island')
    assert child['binding']['ISLAND_LIBEXEC'] == str(prefix/'libexec/island')
    home = Path((root/'home-path.txt').read_text().strip())
    assert Path(child['cwd']) == home.resolve()
    assert child['binding']['ISLAND_CACHE'] == str(home/'data/cache/default')
    assert child['binding']['ISLAND_LOG'] == str(home/'data/logs/default')
    assert child['binding']['ISLAND_RUN'] == str(home/'run')
    assert child['binding']['ISLAND_TMP'] == str(home/'tmp')
    assert all(not Path(child['cwd']).is_relative_to(Path(owner['root']))
               for owner in inputs['owners'].values())
    assert all(not str(Path(owner['root'])) in argument
               for owner in inputs['owners'].values() for argument in child['argv'])
    assert all(Path(path).is_relative_to(prefix/'lib/island')
               for path in wheel_receipt['imports'].values())
    assert wheel_receipt['wheel_sha256'] == inputs['wheel_sha256']
    selected = load(root/'selected-inputs.json')
    for owner in ('isos','eos','egia','ui'):
        assert selected[owner] == inputs['owners'][owner]['commit']
    assert selected['egia_launcher_sha256'] == inputs['cmake_installed_files']['bin/egia']
    assert selected['egia_config_sha256'] == inputs['cmake_installed_files']['bin/egia-config']
    assert selected['mujoco_release_sha256'] == inputs['package']['release_sha256']
    snapshot = root/'installed-prefix-snapshot'
    snap = files_under(snapshot)
    assert {name:snap[name] for name in inputs['cmake_installed_files']} == inputs['cmake_installed_files']
    assert snap['wheels/egia-0.1.0-py3-none-any.whl'] == inputs['wheel_sha256']
    assert snap['q03-wheel-payload.json'] == inputs['wheel_payload_manifest_sha256']
    payload = load(snapshot/'q03-wheel-payload.json')
    assert payload['wheel_sha256'] == inputs['wheel_sha256']
    assert payload['files'] == wheel_receipt['files']
    for name, expected in payload['files'].items():
        assert snap['lib/island/'+name] == expected
    for item in chain['code']:
        root_path = Path(item['root'])
        relative = Path(item['path'])
        assert not relative.is_absolute() and '..' not in relative.parts
        selected_path = (root_path.relative_to(prefix)/relative).as_posix()
        assert snap[selected_path] == item['sha256'], item
    assert set(snap) == (set(inputs['cmake_installed_files']) |
                         {'lib/island/'+name for name in payload['files']} |
                         {'q03-wheel-payload.json','wheels/egia-0.1.0-py3-none-any.whl'})
    subprocess.run([sys.executable,str(Path(__file__).with_name('verify_q01_restart.py')),
                    str(root)],check=True)
    assert load(root/'reap-proof.json')['all_absent'] is True
    assert load(root/'status-stopped.json')['state'] == 'stopped'
    print('Q03_EGIA_INSTALLED_PROCESS_VERIFY_PASS',flush=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive',type=Path)
    parser.add_argument('inputs',type=Path)
    parser.add_argument('--expect-inputs-sha256',required=True)
    parser.add_argument('--expect-evidence-sha256')
    parser.add_argument('--seal',action='store_true')
    a=parser.parse_args()
    root=a.archive.resolve()
    assert root.is_dir()
    assert digest(a.inputs) == a.expect_inputs_sha256
    inputs=load(a.inputs)
    if a.seal:
        assert not (root/'evidence.sha256.json').exists()
        shutil.copy2(a.inputs,root/'q03-egia-installed-inputs.json')
        copy_selected(root,inputs)
        verify(root,inputs,sealed=False)
        manifest=files_under(root)
        (root/'evidence.sha256.json').write_text(json.dumps(manifest,sort_keys=True,indent=2)+'\n')
        print('Q03_EGIA_INSTALLED_SEAL_SHA256',digest(root/'evidence.sha256.json'),flush=True)
    else:
        verify(root,inputs,sealed=True,expected_evidence_sha256=a.expect_evidence_sha256)


if __name__=='__main__':
    main()
