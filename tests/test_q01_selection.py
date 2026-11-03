"""Focused tests for the reviewer-owned Q03 trust root."""

import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from unittest import TestCase, main
from unittest.mock import patch

from q01_selection import (CASE_PROGRAMS, CASES, CASE_UI_DIRS, OWNERS,
                           UI_BUILD_FILES, canonical_bytes, check_harness,
                           load_build_receipt, load_case_receipt, load_selection,
                           replay_home, sha256, verify_checkout,
                           verify_ui_build_inputs)


def selection_fixture():
    owner = {
        'repository': 'https://example.invalid/Island.git',
        'ref': 'refs/heads/refactor(v2)/q03',
        'commit': 'a' * 40,
        'tree': 'b' * 40,
        'subtrees': {},
        'locks': {'Cargo.lock': 'c' * 64},
        'sources': {'src/main.rs': 'd' * 64},
    }
    owners = {name: copy.deepcopy(owner) for name in OWNERS}
    owners['eos']['subtrees']['crates'] = 'e' * 40
    owners['egia']['locks'] = {}
    owners['mujoco']['locks'] = {}
    return {
        'schema_version': 1,
        'profile': 'selected-q03',
        'owners': owners,
        'harness': {name: 'f' * 64 for name in CASE_PROGRAMS},
        'build_inputs': {profile:{name:'1'*64 for name in UI_BUILD_FILES}
                         for profile in ('ui_local','ui_hosted')},
        'package': {'release_sha256': '2' * 64, 'asset_sha256': '3' * 64},
    }


class SelectionGuards(TestCase):
    def test_replay_home_uses_configured_temp_root_in_fresh_process(self):
        with tempfile.TemporaryDirectory(dir=Path.home()) as directory:
            root = Path(directory)
            code = ("from q01_selection import replay_home; "
                    "from pathlib import Path; "
                    "import socket; "
                    "home = replay_home('m4e-test'); "
                    "assert home.parent == Path.cwd(); "
                    "sock = socket.socket(socket.AF_UNIX); "
                    "sock.bind(str(home) + '.sock'); sock.close(); "
                    "print(home)")
            result = subprocess.run([sys.executable, '-c', code], cwd=root,
                                    env=dict(os.environ, TMPDIR=str(root),
                                             PYTHONPATH=str(Path(__file__).parent)),
                                    capture_output=True, text=True, check=True)
            self.assertEqual(result.stdout.strip(), str(root/'m4e-test'))

    def test_replay_home_rejects_path_escape(self):
        for name in ('', '.', '..', '../outside', '/outside'):
            with self.subTest(name=name), self.assertRaises(AssertionError):
                replay_home(name)

    def load(self, value, *, digest=None, raw=None):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'selection.json'
            raw = canonical_bytes(value) if raw is None else raw
            path.write_bytes(raw)
            return load_selection(path, sha256(raw) if digest is None else digest)

    def test_complete_selection_accepts_canonical_bytes(self):
        value = selection_fixture()
        self.assertEqual(self.load(value), value)

    def test_wrong_external_digest_rejects(self):
        with self.assertRaisesRegex(AssertionError, 'SHA-256 mismatch'):
            self.load(selection_fixture(), digest='0' * 64)

    def test_missing_external_harness_rejects(self):
        value = selection_fixture()
        del value['harness']['tests/q01_migration_replay.py']
        with self.assertRaisesRegex(AssertionError, 'harness'):
            self.load(value)

    def test_missing_p07_ui_overlay_hash_rejects_before_replay(self):
        for name in ('q01_p07_u03_mounted.test.tsx',
                     'q01_p07_workspace_stop.test.tsx'):
            with self.subTest(name=name):
                value = selection_fixture()
                del value['harness'][f'tests/{name}']
                with self.assertRaisesRegex(AssertionError,
                                            'selection lacks executed harness hashes'):
                    self.load(value)

    def test_missing_tree_lock_source_and_build_input_reject(self):
        for key in ('tree', 'locks', 'sources'):
            value = selection_fixture()
            del value['owners']['eos'][key]
            with self.subTest(key=key), self.assertRaises(AssertionError):
                self.load(value)
        value = selection_fixture()
        del value['build_inputs']['ui_hosted']['src/index.tsx']
        with self.assertRaises(AssertionError):
            self.load(value)

    def test_noncanonical_and_unknown_profile_reject(self):
        value = selection_fixture()
        with self.assertRaisesRegex(AssertionError, 'not canonical'):
            self.load(value, raw=(json.dumps(value, indent=2) + '\n').encode())
        value['profile'] = 'legacy-q01'
        with self.assertRaisesRegex(AssertionError, 'unknown'):
            self.load(value)

    def test_external_build_receipt_digest_and_selection_binding(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'receipt.json'
            value = {'schema_version':1,'profile':'selected-q03',
                     'selection_sha256':'a'*64,
                     'binaries_sha256':{'isos':'b'*64},
                     'ui_local_sha256':{'index.html':'c'*64},
                     'ui_hosted_sha256':{'index.html':'d'*64},
                     'build_logs_sha256':{'build.log':'e'*64},
                     'package':{'release_sha256':'f'*64,'asset_sha256':'0'*64}}
            path.write_bytes(canonical_bytes(value))
            self.assertEqual(load_build_receipt(path,sha256(path.read_bytes()),'a'*64),value)
            with self.assertRaises(AssertionError):
                load_build_receipt(path,sha256(path.read_bytes()),'1'*64)
            with self.assertRaisesRegex(AssertionError,'SHA-256 mismatch'):
                load_build_receipt(path,'1'*64,'a'*64)

    def test_external_case_receipt_binds_main_receipt_and_all_six(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'cases.json'
            value={'schema_version':1,'profile':'selected-q03',
                   'selection_sha256':'a'*64,
                   'main_build_receipt_sha256':'b'*64,
                   'hosted_origin_sha256':'e'*64,
                   'cases':{name:{'manifest_sha256':'c'*64,
                       'ui_files_sha256':({'index.html':'d'*64}
                           if name in CASE_UI_DIRS else {})} for name in CASES}}
            path.write_bytes(canonical_bytes(value))
            self.assertEqual(load_case_receipt(path,sha256(path.read_bytes()),
                                               'a'*64,'b'*64),value)
            with self.assertRaises(AssertionError):
                load_case_receipt(path,sha256(path.read_bytes()),'a'*64,'e'*64)
            value['hosted_origin_sha256']='invalid'
            path.write_bytes(canonical_bytes(value))
            with self.assertRaises(AssertionError):
                load_case_receipt(path,sha256(path.read_bytes()),'a'*64,'b'*64)
            value['hosted_origin_sha256']='e'*64
            del value['cases']['camera']
            path.write_bytes(canonical_bytes(value))
            with self.assertRaisesRegex(AssertionError,'exact six cases'):
                load_case_receipt(path,sha256(path.read_bytes()),'a'*64,'b'*64)

    def test_checkout_rejects_external_tree_lock_source_and_ref_mutations(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            checkout = base/'checkout'
            remote = base/'remote.git'
            checkout.mkdir()
            def git(*args):
                return subprocess.check_output(['git','-C',str(checkout),*args],
                                               text=True).strip()
            git('init','-q')
            git('config','user.name','Q03 Test')
            git('config','user.email','q03@example.invalid')
            (checkout/'src').mkdir()
            (checkout/'src/main.rs').write_text('fn main() {}\n')
            (checkout/'Cargo.lock').write_text('lock\n')
            git('add','.')
            git('commit','-qm','test')
            subprocess.run(['git','clone','--bare','-q',str(checkout),str(remote)],
                           check=True)
            owner = {'repository':str(remote),
                     'ref':'refs/heads/'+git('branch','--show-current'),
                     'commit':git('rev-parse','HEAD'),
                     'tree':git('rev-parse','HEAD^{tree}'),
                     'subtrees':{},
                     'locks':{'Cargo.lock':sha256(b'lock\n')},
                     'sources':{'src/main.rs':sha256(b'fn main() {}\n')}}
            verify_checkout({'owners':{'isos':owner}},'isos',checkout)
            for field, member, wrong in (('tree',None,'0'*40),
                                         ('locks','Cargo.lock','0'*64),
                                         ('sources','src/main.rs','0'*64),
                                         ('commit',None,'0'*40),
                                         ('ref',None,'refs/heads/missing')):
                changed = copy.deepcopy(owner)
                if member:
                    changed[field][member] = wrong
                else:
                    changed[field] = wrong
                with self.subTest(field=field), self.assertRaises(AssertionError):
                    verify_checkout({'owners':{'isos':changed}},'isos',checkout)

    def test_local_harness_mismatch_rejects(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'replay.py').write_text('original\n')
            selection = {'harness':{'tests/replay.py':sha256(b'other\n')}}
            with self.assertRaisesRegex(AssertionError,'harness digest differs'):
                check_harness(selection,root)

    def test_complete_ui_manifest_rejects_omitted_tracked_source(self):
        value=selection_fixture()
        tracked=('\0'.join(sorted(UI_BUILD_FILES | {'src/deep/component.tsx'}))+'\0').encode()
        with tempfile.TemporaryDirectory() as directory:
            with patch('q01_selection.subprocess.check_output',return_value=tracked):
                with self.assertRaisesRegex(AssertionError,'every tracked UI build input'):
                    verify_ui_build_inputs(value,Path(directory))


if __name__ == '__main__':
    main()
