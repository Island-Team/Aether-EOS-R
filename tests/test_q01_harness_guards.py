"""Focused regression checks for Q01 archive and cleanup fail-closed rules."""

import json
from pathlib import Path
import tempfile
import tomllib
from unittest import TestCase, main
from unittest.mock import patch

from m4_affected_replay import cleanup_owned_run, write_selected_isos_config
from verify_q01_cases import verify_case
from verify_q01 import files, seal, verify


class Alive:
    def poll(self):
        return None


class HarnessGuards(TestCase):
    def test_selected_isos_config_starts_from_owner_source(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            isos = root / 'isos'
            (isos / 'config').mkdir(parents=True)
            source = '[paths]\nui_dist = "../IsosUI/dist/local"\n'
            (isos / 'config/isos.toml').write_text(source)
            home = root / 'home'
            (home / 'config').mkdir(parents=True)
            config = write_selected_isos_config(home, isos)
            self.assertTrue(config.read_text().startswith(source))
            self.assertEqual(tomllib.loads(config.read_text())['vision_evidence'],
                             {'gate_path': 'eos/interface/gate/front-preview'})

    def test_nested_manifest_name_is_an_archive_member(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'nested').mkdir()
            (root / 'nested/evidence.sha256.json').write_text('{}\n')
            self.assertIn('nested/evidence.sha256.json', files(root))

    def test_archive_seal_rejects_external_symlink(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'external.txt').write_text('outside archive')
            archive = root / 'archive'
            archive.mkdir()
            (archive / 'linked.txt').symlink_to(root / 'external.txt')
            with self.assertRaisesRegex(AssertionError, 'nonregular archive member'):
                seal(archive)

    def test_resealed_archive_cannot_choose_owner_pins(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            pins = {owner: digit * 40 for owner, digit in
                    (('isos', 'a'), ('eos', 'b'), ('egia', 'c'), ('ui', 'd'))}
            (out / 'selected-inputs.json').write_text(json.dumps(pins))
            (out / 'q01-package-input.json').write_text(
                json.dumps({'source_commit': 'e' * 40}))
            seal(out)
            expected = dict(pins, mujoco='e' * 40, eos='f' * 40)
            with self.assertRaisesRegex(AssertionError, 'owner pins differ'):
                verify(out, expected_pins=expected)
            expected = dict(pins, mujoco='f' * 40)
            with self.assertRaisesRegex(AssertionError, 'MuJoCo package differs'):
                verify(out, expected_pins=expected)

    def test_programmatic_full_gate_requires_external_pins(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(AssertionError, 'requires five external owner pins'):
                verify(Path(directory))

    def test_self_declared_matrix_case_cannot_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            case=Path(directory)
            (case/'passed.json').write_text('{"passed":true}\n')
            pins = {
                'eos': '642166576a2cddfd97cd703e5bd9e53567745515',
                'isos': '00c1d7aa184de8b969e6e40472bf9b1fa55032ff',
                'egia': '694c58c9ff40fc2e10678d5247222a821b1d9756',
            }
            with self.assertRaises(AssertionError):
                verify_case('camera',case,pins)

    def test_failure_finalizer_requests_owner_stop(self):
        with tempfile.TemporaryDirectory() as directory:
            out=Path(directory)
            requests=[]

            def owner(_port,path,method='GET',data=None,**_kwargs):
                requests.append((path,method,data))
                if path=='process-status':
                    if len([r for r in requests if r[0]==path])==1:
                        return {'state':'running','active_revision':'a'*64,
                                'processes':[]}
                    return {'state':'stopped','processes':[],
                            'terminal':{'outcome':'confirmed'}}
                return 200,{'ok':True,'data':{'terminal':{'outcome':'confirmed'}}}

            with patch('m4_affected_replay.api',side_effect=owner):
                result=cleanup_owned_run(out,16000,Alive(),Alive())
            self.assertEqual(requests[1],('process-control','POST',
                {'schema_version':1,'operation':'stop','revision':'a'*64}))
            self.assertTrue(result['stop_confirmed'])
            self.assertEqual(result['errors'],[])
            self.assertEqual(json.loads((out/'q01-cleanup-proof.json').read_text()),result)

    def test_uncertain_stop_is_retained_as_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            out=Path(directory)
            calls=0

            def owner(_port,path,method='GET',data=None,**_kwargs):
                nonlocal calls
                if path=='process-status':
                    calls+=1
                    return {'state':'running' if calls==1 else 'stopped',
                            'active_revision':'b'*64,'processes':[]}
                return 200,{'ok':True,'data':{'terminal':{'outcome':'unknown'}}}

            with patch('m4_affected_replay.api',side_effect=owner):
                result=cleanup_owned_run(out,16000,Alive(),Alive())
            self.assertFalse(result['stop_confirmed'])
            self.assertIn('owner_stop_not_confirmed',result['errors'])


if __name__=='__main__':
    main()
