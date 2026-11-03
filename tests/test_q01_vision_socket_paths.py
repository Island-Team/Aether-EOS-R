"""The selected Vision endpoint may differ from the control-grant run root."""

from pathlib import Path
import hashlib
import unittest

from q01_camera_replay import vision_socket_paths


class VisionSocketPathsTest(unittest.TestCase):
    def test_uses_selected_short_endpoint_for_both_owner_observations(self):
        home = Path('/home/test-operator/long-private-home')
        run_id = 'run-id-with-a-long-name'
        digest = hashlib.sha256(f'default\0{run_id}'.encode()).hexdigest()[:32]
        binding = {'deployment_run_id': run_id, 'eos_endpoint': {'transport': 'uds',
            'path': str(home / 'run/v' / digest / 'eos-vision-owner.sock')}}
        owner, inspection = vision_socket_paths(binding, home, run_id)
        self.assertEqual(owner, Path(binding['eos_endpoint']['path']))
        self.assertEqual(inspection, owner.with_name('egia-vision-inspection.sock'))
        self.assertNotIn('control-grants', str(owner))

    def test_rejects_wrong_transport_and_non_owner_path(self):
        home = Path('/home/test-operator/long-private-home')
        run_id = 'run-one'
        for endpoint in (
            {'transport': 'tcp', 'path': '/tmp/v/eos-vision-owner.sock'},
            {'transport': 'uds', 'path': 'relative/eos-vision-owner.sock'},
            {'transport': 'uds', 'path': '/tmp/v/wrong.sock'},
        ):
            with self.subTest(endpoint=endpoint), self.assertRaises(AssertionError):
                vision_socket_paths({'deployment_run_id': run_id,
                                     'eos_endpoint': endpoint}, home, run_id)

    def test_rejects_path_for_another_run_or_outside_selected_home(self):
        home = Path('/home/test-operator/long-private-home')
        run_id = 'run-one'
        digest = hashlib.sha256(f'default\0{run_id}'.encode()).hexdigest()[:32]
        expected = home / 'run/v' / digest / 'eos-vision-owner.sock'
        for selected_run, selected_path in (
            ('run-two', expected),
            (run_id, Path('/tmp') / 'run/v' / digest / expected.name),
        ):
            with self.subTest(run=selected_run, path=selected_path), \
                    self.assertRaises(AssertionError):
                vision_socket_paths({'deployment_run_id': selected_run,
                    'eos_endpoint': {'transport': 'uds', 'path': str(selected_path)}},
                    home, run_id)


if __name__ == '__main__':
    unittest.main()
