#!/usr/bin/env python3
"""Exercise selected EGIA Vision ingress rejections with its owner test frames.

This is a controlled stub case. It imports the exact pinned owner module and
test frame helper, then records only metadata snapshots, never JPEG or grants.
"""

import argparse
import asyncio
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import uuid


JPEG = (b'\xff\xd8\xff\xc0\x00\x0b\x08\x00\x01\x00\x01\x01\x01\x11\x00'
        b'\xff\xda\x00\x08\x01\x01\x00\x00\x3f\x00\x00\xff\xd9')


def frame(binding: dict, stream: str, *, generation: int = 1, **changes) -> bytes:
    metadata = {'graph_revision': binding['graph_revision'],
                'projection_revision': binding['projection_revision'],
                'runtime_stream_id': stream, 'source_path': binding['source_path'],
                'source_frame_id': 'frame-1', 'gate_path': binding['gate_path'],
                'gate_generation': generation, 'width_px': 1, 'height_px': 1,
                'source_rate_hz': 30.0, 'render': binding['render'],
                'jpeg_sha256': hashlib.sha256(JPEG).hexdigest()}
    metadata.update(changes)
    raw = json.dumps(metadata, separators=(',', ':')).encode()
    return struct.pack('<I', len(raw)) + raw + JPEG


def setup(directory: Path) -> tuple[dict, str, Path, Path]:
    run = directory / 'run'
    run.mkdir(mode=0o700)
    egia = run / 'egia'
    egia.mkdir(mode=0o700)
    grant_path = run / 'egia-vision.grant'
    grant = '1' * 64
    grant_path.write_text(grant + '\n')
    grant_path.chmod(0o600)
    binding = {'schema_version': 1, 'deployment_run_id': 'run-one',
               'graph_revision': '0' * 64, 'projection_revision': 'a' * 64,
               'eos_endpoint': {'transport': 'uds',
                                'path': str(run / 'eos-vision-owner.sock')},
               'source_path': 'eos/hardware/camera/front',
               'gate_path': 'eos/interface/gate/front',
               'ingress_type': 'egia.vision.ingress@v0.1.frame',
               'render': {'max_width_px': 1280, 'jpeg_quality': 80},
               'target_fps': 30, 'grant_file': str(grant_path)}
    binding_path = egia / 'vision-binding.json'
    binding_path.write_text(json.dumps(binding))
    binding_path.chmod(0o600)
    return binding, grant, binding_path, grant_path


async def probe(root: Path) -> dict:
    sys.path.insert(0, str(root / 'src'))
    from egia.vision_evidence import VisionEvidence, load_binding

    with tempfile.TemporaryDirectory(prefix='q01-egia-vision-') as directory:
        binding, grant, binding_path, grant_path = setup(Path(directory))
        stream = str(uuid.uuid4())
        wrong_run = None
        try:
            load_binding(binding_path, grant_path, 'wrong-run', binding['graph_revision'])
        except ValueError as error:
            wrong_run = str(error)
        assert wrong_run == 'run_mismatch'
        evidence = VisionEvidence(binding, grant)
        await evidence.attest({'type': 'subscribed',
            'graph_revision': binding['graph_revision'],
            'projection_revision': binding['projection_revision'],
            'source_path': binding['source_path'], 'gate_path': binding['gate_path'],
            'runtime_stream_id': stream, 'gate_generation': 0})
        await evidence.accept(frame(binding, stream))
        baseline = evidence.snapshot()
        await evidence.accept(frame(binding, stream, generation=1))
        stale = evidence.snapshot()
        await evidence.accept(frame(binding, stream, generation=2,
                                    graph_revision='f' * 64))
        revision = evidence.snapshot()
        await evidence.accept(frame(binding, stream, generation=2,
                                    jpeg_sha256='f' * 64))
        bad_digest = evidence.snapshot()
        return {'source_commit': subprocess.check_output(
                    ['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip(),
                'selected_implementation_sha256': hashlib.sha256(
                    (root / 'src/egia/vision_evidence.py').read_bytes()).hexdigest(),
                'wrong_run_reason': wrong_run,
                'baseline': baseline, 'stale_generation': stale,
                'wrong_revision': revision, 'digest_mismatch': bad_digest}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--egia-root', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    result = asyncio.run(probe(args.egia_root))
    selected_source = args.egia_root / 'src/egia/vision_evidence.py'
    source_copy = args.output.with_name('egia-vision-source.py')
    source_copy.write_bytes(selected_source.read_bytes())
    assert hashlib.sha256(source_copy.read_bytes()).hexdigest() == result['selected_implementation_sha256']
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
    print('Q01_EGIA_CAMERA_STUB_PASS')
