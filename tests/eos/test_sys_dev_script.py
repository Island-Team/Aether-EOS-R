#!/usr/bin/env python3
"""Unit tests for unified system-device discovery and selection helpers."""

import importlib.util
import sys
import unittest
from pathlib import Path
from unittest.mock import patch


root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "scripts"))
spec = importlib.util.spec_from_file_location("sys_dev", root / "scripts/sys_dev.py")
sys_dev = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(sys_dev)


class SysDevTest(unittest.TestCase):
    def test_preferred_audio_reselects_only_present_devices(self) -> None:
        routes = {
            "mic": [(sys_dev.PREFERRED_AUDIO["mic"], "TAKSTAR")],
            "speaker": [],
        }
        with patch.object(sys_dev, "audio_routes", side_effect=lambda role: routes[role]), \
             patch.object(sys_dev, "audio_default", return_value="other"), \
             patch.object(sys_dev, "_pactl") as pactl:
            state = sys_dev.ensure_preferred_audio()
        pactl.assert_called_once_with(["set-default-source", sys_dev.PREFERRED_AUDIO["mic"]])
        self.assertIn("speaker: waiting", state[1])

    def test_parses_realsense_short_listing(self) -> None:
        routes = sys_dev.camera_routes_from_text(
            "Device Name                   Serial Number       Firmware Version\n"
            "RealSense D435                347622071100        5.17.0.10\n"
        )
        self.assertEqual(routes, [("347622071100", "RealSense D435 - firmware 5.17.0.10")])

    def test_finds_physical_pulse_endpoints(self) -> None:
        output = (
            "Sink #0\n"
            "\tName: alsa_output.platform-sound.analog-stereo\n"
            "\tDescription: Built-in Audio Analog Stereo\n"
            "\tProperties:\n"
            "\t\talsa.card_name = \"NVIDIA Jetson Orin NX APE\"\n"
            "Sink #1\n"
            "\tName: alsa_output.virtual\n"
            "\tDescription: Virtual Sink\n"
        )
        self.assertEqual(
            sys_dev.pulse_endpoints_from_text(output, "sink"),
            [("alsa_output.platform-sound.analog-stereo", "Built-in Audio Analog Stereo")],
        )


if __name__ == "__main__":
    unittest.main()
