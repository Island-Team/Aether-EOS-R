"""External trust root for the selected Q03 replay profile."""

import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile


OWNERS = ('isos', 'eos', 'egia', 'ui', 'mujoco')


def replay_home(name: str) -> Path:
    """Place a named clean replay home beneath the configured temp directory."""
    assert name and Path(name).name == name and name not in ('.', '..'), name
    return Path(tempfile.gettempdir()) / name
HISTORICAL = {
    'isos': '00c1d7aa184de8b969e6e40472bf9b1fa55032ff',
    'eos': '642166576a2cddfd97cd703e5bd9e53567745515',
    'egia': 'd3868d2763c35d9f6bfed80d75edb7a1844ef54e',
    'ui': '920995788d8e29dd6ab594d31278033bb791cfeb',
    'mujoco': '4e6b5449ccd25be53ad95b13baa2c89f7ed91e10',
}
CASE_PROGRAMS = tuple('tests/' + name for name in (
    'm4_affected_replay.py', 'q01_full_replay.py', 'verify_q01.py',
    'verify_q01_cases.py', 'q01_selection.py', 'q01_browser_raw.py',
    'q01_camera_replay.py', 'verify_q01_camera.py',
    'q01_g06_r03_replay.py', 'verify_q01_g06_r03.py',
    'q01_p01_p03_u01_replay.py', 'verify_q01_p01_p03_u01.py',
    'q01_p07_u03_replay.py', 'verify_q01_p07_u03.py',
    'q01_r06_a03_replay.py', 'verify_q01_r06_a03.py',
    'q01_migration_replay.py', 'verify_q01_migration.py',
    'q01_restart_replay.py', 'verify_q01_restart.py',
    'q01_egia_camera_probe.py', 'q01_fifo_probe.py',
    'q01_g06_r03_tamper.py', 'q01_p07_u03_tamper.py',
    'q01_package_negative.py', 'q01_tamper.py',
    'm4_model_stub.py', 'q01_p07_segment_id.rs',
    'q01_p07_u03_mounted.test.tsx', 'q01_p07_workspace_stop.test.tsx',
    'q01_g06_r03_journal_probe.rs', 'q01_fifo_probe.rs',
    'm1_cross_repo_replay.py', 'q03_browser_capture.py',
    'q03_case_receipt.py', 'q03_recording_proxy.py', 'q03_hosted_origin.py',
))
HEX40 = re.compile(r'[0-9a-f]{40}\Z')
HEX64 = re.compile(r'[0-9a-f]{64}\Z')
UI_BUILD_FILES = frozenset(('package.json', 'package-lock.json',
                            'webpack.config.js', 'tsconfig.json',
                            'public/index.html', 'scripts/write-manifest.mjs',
                            'src/index.tsx'))
UI_BUILD_DIRS = ('src/', 'public/', 'scripts/')
CASES = ('camera', 'g06-r03', 'p01-p03-u01', 'p07-u03', 'r06-a03',
         'migration')
CASE_BINARIES = {
    'camera': {'selected-binaries/isos':'isos',
               'selected-binaries/egia':'egia',
               'selected-binaries/aether-runtime':'aether-runtime',
               'selected-binaries/aether-profile':'aether-profile',
               'selected-binaries/aether-http-bridge':'aether-http-bridge',
               'selected-binaries/aether-config-server':'aether-config-server'},
    'g06-r03': {'bin/isos':'isos','bin/aether-profile':'aether-profile'},
    'p01-p03-u01': {'bin/isos':'isos'},
    'p07-u03': {'bin/isos':'isos'},
    'r06-a03': {'selected-binaries/isos':'isos',
                'selected-binaries/egia':'egia',
                'selected-binaries/aether-runtime':'aether-runtime',
                'selected-binaries/aether-profile':'aether-profile',
                'selected-binaries/aether-http-bridge':'aether-http-bridge',
                'selected-binaries/aether-config-server':'aether-config-server'},
    'migration': {'selected-isos.bin':'isos'},
}
CASE_UI_DIRS = {'p01-p03-u01':'ui-dist', 'p07-u03':'ui-dist',
                'r06-a03':'selected-ui-dist'}
CASE_ASSET_PATHS = {'p01-p03-u01':'package-input/asset.tar.gz',
                    'r06-a03':'package-asset.tar.gz'}


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def canonical_bytes(value):
    return (json.dumps(value, sort_keys=True, separators=(',', ':'),
                       ensure_ascii=False) + '\n').encode()


def _relative(name):
    path = Path(name)
    return bool(name and not path.is_absolute() and '..' not in path.parts
                and path.as_posix() == name and not name.startswith('./'))


def _digest_map(value, label, *, nonempty=True):
    assert isinstance(value, dict) and (value or not nonempty), label
    for name, digest in value.items():
        assert isinstance(name, str) and _relative(name), (label, name)
        assert isinstance(digest, str) and HEX64.fullmatch(digest), (label, name)


def load_selection(path: Path, expected_sha256: str) -> dict:
    assert path.is_absolute() and path.is_file(), 'external selection path required'
    assert isinstance(expected_sha256, str) and HEX64.fullmatch(expected_sha256), \
        'reviewer-recorded selection SHA-256 required'
    raw = path.read_bytes()
    assert sha256(raw) == expected_sha256, 'external selection SHA-256 mismatch'
    value = json.loads(raw)
    assert raw == canonical_bytes(value), 'selection JSON is not canonical'
    assert set(value) == {'schema_version', 'profile', 'owners', 'harness',
                          'build_inputs', 'package'}, 'selection schema keys differ'
    assert value['schema_version'] == 1 and value['profile'] == 'selected-q03', \
        'unknown selection schema or profile'
    owners = value['owners']
    assert isinstance(owners, dict) and set(owners) == set(OWNERS), \
        'selection requires five exact owners'
    for name, owner in owners.items():
        assert set(owner) == {'repository', 'ref', 'commit', 'tree', 'subtrees',
                              'locks', 'sources'}, name
        assert isinstance(owner['repository'], str) and owner['repository'] and \
            not any(c.isspace() for c in owner['repository']), name
        assert re.fullmatch(r'refs/heads/[A-Za-z0-9._/()\-]+', owner['ref']) and \
            '..' not in owner['ref'] and not owner['ref'].endswith('/'), name
        assert HEX40.fullmatch(owner['commit']) and HEX40.fullmatch(owner['tree']), name
        assert isinstance(owner['subtrees'], dict), name
        for path, subtree in owner['subtrees'].items():
            assert _relative(path) and HEX40.fullmatch(subtree), (name, path)
        if name == 'eos':
            assert 'crates' in owner['subtrees'], 'EOS crates subtree required'
        _digest_map(owner['locks'], f'{name} locks', nonempty=name in ('isos', 'eos', 'ui'))
        _digest_map(owner['sources'], f'{name} sources')
    harness = value['harness']
    _digest_map(harness, 'harness')
    assert set(CASE_PROGRAMS) <= set(harness), 'selection lacks executed harness hashes'
    build = value['build_inputs']
    assert set(build) == {'ui_local', 'ui_hosted'}, 'both UI build inputs required'
    for name in build:
        _digest_map(build[name], name)
        assert UI_BUILD_FILES <= set(build[name]), \
            f'{name} missing required UI build inputs'
    package = value['package']
    assert set(package) == {'release_sha256', 'asset_sha256'}, \
        'MuJoCo package digests required'
    assert all(isinstance(digest, str) and HEX64.fullmatch(digest)
               for digest in package.values()), 'MuJoCo package digest invalid'
    return value


def load_build_receipt(path: Path, expected_sha256: str, selection_sha256: str) -> dict:
    assert path.is_absolute() and path.is_file(), 'external build receipt required'
    assert isinstance(expected_sha256, str) and HEX64.fullmatch(expected_sha256), \
        'reviewer-recorded build receipt SHA-256 required'
    raw = path.read_bytes()
    assert sha256(raw) == expected_sha256, 'external build receipt SHA-256 mismatch'
    value = json.loads(raw)
    assert raw == canonical_bytes(value), 'build receipt JSON is not canonical'
    assert set(value) == {'schema_version', 'profile', 'selection_sha256',
                          'binaries_sha256', 'ui_local_sha256', 'ui_hosted_sha256',
                          'build_logs_sha256', 'package'}, 'build receipt schema differs'
    assert value['schema_version'] == 1 and value['profile'] == 'selected-q03'
    assert value['selection_sha256'] == selection_sha256
    for name in ('binaries_sha256', 'ui_local_sha256', 'ui_hosted_sha256',
                 'build_logs_sha256'):
        _digest_map(value[name], name)
    assert set(value['package']) == {'release_sha256', 'asset_sha256'}
    return value


def load_case_receipt(path: Path, expected_sha256: str, selection_sha256: str,
                      main_build_sha256: str) -> dict:
    assert path.is_absolute() and path.is_file(), 'external case receipt required'
    assert isinstance(expected_sha256, str) and HEX64.fullmatch(expected_sha256), \
        'reviewer-recorded case receipt SHA-256 required'
    raw = path.read_bytes()
    assert sha256(raw) == expected_sha256, 'external case receipt SHA-256 mismatch'
    value = json.loads(raw)
    assert raw == canonical_bytes(value), 'case receipt JSON is not canonical'
    assert set(value) == {'schema_version','profile','selection_sha256',
                          'main_build_receipt_sha256','hosted_origin_sha256','cases'}
    assert value['schema_version'] == 1 and value['profile'] == 'selected-q03'
    assert value['selection_sha256'] == selection_sha256
    assert value['main_build_receipt_sha256'] == main_build_sha256
    assert HEX64.fullmatch(value['hosted_origin_sha256'])
    assert set(value['cases']) == set(CASES), 'case receipt lacks exact six cases'
    for name, entry in value['cases'].items():
        assert set(entry) == {'manifest_sha256','ui_files_sha256'}, name
        assert HEX64.fullmatch(entry['manifest_sha256']), name
        _digest_map(entry['ui_files_sha256'], f'{name} UI',
                    nonempty=name in CASE_UI_DIRS)
        if name not in CASE_UI_DIRS:
            assert entry['ui_files_sha256'] == {}, name
    return value


def verify_case_build_bytes(name: str, case: Path, entry: dict,
                            build_receipt: dict, case_receipt: dict):
    assert name in CASES
    frozen = case_receipt['cases'][name]
    assert frozen['manifest_sha256'] == entry['manifest_sha256'] == \
        sha256((case/'evidence.sha256.json').read_bytes()), name
    for relative, binary in CASE_BINARIES[name].items():
        assert sha256((case/relative).read_bytes()) == \
            build_receipt['binaries_sha256'][binary], (name, relative)
    if name in CASE_ASSET_PATHS:
        assert sha256((case/CASE_ASSET_PATHS[name]).read_bytes()) == \
            build_receipt['package']['asset_sha256'], name
    if name == 'camera':
        selected = json.loads((case/'selected-inputs.json').read_text())
        assert selected['environment']['package_release_sha256'] == \
            build_receipt['package']['release_sha256']
    if name in CASE_UI_DIRS:
        ui = case/CASE_UI_DIRS[name]
        actual = {path.relative_to(ui).as_posix():sha256(path.read_bytes())
                  for path in ui.rglob('*') if path.is_file()}
        assert actual == frozen['ui_files_sha256'], f'{name} UI bytes differ'


def selected_pins(selection):
    return {name: selection['owners'][name]['commit'] for name in OWNERS}


def check_harness(selection, directory: Path):
    for name, expected in selection['harness'].items():
        source = directory / Path(name).name
        assert source.is_file() and sha256(source.read_bytes()) == expected, \
            f'harness digest differs: {name}'


def _git(root, *arguments):
    return subprocess.check_output(['git', '-C', str(root), *arguments],
                                   text=True).strip()


def verify_checkout(selection, name: str, root: Path):
    owner = selection['owners'][name]
    assert _git(root, 'status', '--porcelain', '--untracked-files=no') == '', \
        f'{name} checkout is dirty'
    assert _git(root, 'rev-parse', 'HEAD') == owner['commit'], f'{name} HEAD differs'
    assert _git(root, 'rev-parse', 'HEAD^{tree}') == owner['tree'], \
        f'{name} tree differs'
    remote = subprocess.check_output(['git', 'ls-remote', owner['repository'],
                                      owner['ref']], text=True).strip().splitlines()
    assert remote == [f"{owner['commit']}\t{owner['ref']}"], \
        f'{name} remote ref moved'
    for path, expected in owner['locks'].items():
        actual = subprocess.check_output(['git', '-C', str(root), 'show',
                                          f"{owner['commit']}:{path}"])
        assert sha256(actual) == expected, f'{name} lock differs: {path}'
    for path, expected in owner['subtrees'].items():
        assert _git(root, 'rev-parse', f"{owner['commit']}:{path}") == expected, \
            f'{name} subtree differs: {path}'
    for path, expected in owner['sources'].items():
        actual = subprocess.check_output(['git', '-C', str(root), 'show',
                                          f"{owner['commit']}:{path}"])
        assert sha256(actual) == expected, f'{name} source differs: {path}'


def verify_ui_build_inputs(selection, root: Path):
    owner = selection['owners']['ui']
    tracked = subprocess.check_output(['git', '-C', str(root), 'ls-tree', '-r',
                                      '-z', '--name-only', owner['commit']])
    paths = {item.decode() for item in tracked.split(b'\0') if item}
    required = {name for name in paths if name in UI_BUILD_FILES or
                name.startswith(UI_BUILD_DIRS)}
    assert UI_BUILD_FILES <= required, 'selected UI checkout lacks required build files'
    for profile in ('ui_local', 'ui_hosted'):
        expected = selection['build_inputs'][profile]
        assert set(expected) == required, \
            f'{profile} must hash every tracked UI build input'
        for name, digest in expected.items():
            content = subprocess.check_output(['git', '-C', str(root), 'show',
                                               f"{owner['commit']}:{name}"])
            assert sha256(content) == digest, (profile, name)
            assert sha256((root/name).read_bytes()) == digest, (profile, name)
