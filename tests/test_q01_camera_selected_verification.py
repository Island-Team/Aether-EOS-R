"""The camera replay's own final check must use the reviewer selection."""

import tempfile
from pathlib import Path
from unittest import TestCase, main
from unittest.mock import patch

from q01_camera_replay import verify_camera_archive
from verify_q01_camera import PINS


class CameraSelectedVerification(TestCase):
    def test_selected_self_check_receives_external_pins_and_selection(self):
        selected = {owner: digit * 40 for owner, digit in
                    (('eos', 'a'), ('isos', 'b'), ('egia', 'c'))}
        self.assertNotEqual(selected, PINS)
        selection = {'owners': {owner: {'commit': commit}
                                for owner, commit in selected.items()}}
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory)
            with patch('verify_q01_camera.verify') as verify:
                verify_camera_archive(archive, selection, selected)
            verify.assert_called_once_with(archive, selected_pins=selected,
                                           selection=selection)

    def test_legacy_self_check_keeps_historical_default(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory)
            with patch('verify_q01_camera.verify') as verify:
                verify_camera_archive(archive, None, dict(PINS))
            verify.assert_called_once_with(archive)


if __name__ == '__main__':
    main()
