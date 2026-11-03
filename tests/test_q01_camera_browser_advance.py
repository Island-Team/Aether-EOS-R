"""The rejected stale ingress is independent of the live browser frame clock."""

import unittest

from verify_q01_camera import browser_after_follows


class BrowserAdvanceTest(unittest.TestCase):
    def setUp(self):
        self.before = {
            'graph_revision': 'a' * 64,
            'projection_revision': 'b' * 64,
            'runtime_stream_id': 'stream-one',
            'source_path': 'eos/hardware/camera/front',
            'source_frame_id': 'src-11',
            'gate_path': 'eos/interface/gate/front-preview',
            'gate_generation': 2,
            'render': {'max_width_px': 1280, 'jpeg_quality': 80},
            'jpeg_sha256': 'c' * 64,
            'jpeg_verified_sha256': 'c' * 64,
            'jpeg_length_bytes': 1024,
        }

    def test_same_frame_and_natural_advance(self):
        self.assertTrue(browser_after_follows(self.before, dict(self.before)))
        advanced = dict(self.before, gate_generation=3,
                        source_frame_id='src-18', jpeg_sha256='d' * 64,
                        jpeg_verified_sha256='d' * 64)
        self.assertTrue(browser_after_follows(self.before, advanced))

    def test_rollback_stream_swap_and_same_generation_mutation_rejected(self):
        changes = (
            {'gate_generation': 1},
            {'runtime_stream_id': 'other-stream', 'gate_generation': 3},
            {'gate_generation': 3, 'source_frame_id': 'src-11'},
            {'gate_generation': 3, 'source_frame_id': 'src-10'},
            {'jpeg_sha256': 'd' * 64},
            {'gate_generation': 3, 'jpeg_sha256': 'd' * 64},
        )
        for change in changes:
            with self.subTest(change=change):
                self.assertFalse(browser_after_follows(
                    self.before, dict(self.before, **change)))


if __name__ == '__main__':
    unittest.main()
