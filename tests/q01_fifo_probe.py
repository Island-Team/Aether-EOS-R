#!/usr/bin/env python3
"""Compile a synthetic FIFO probe against exact selected EOS route source."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


PIN = '642166576a2cddfd97cd703e5bd9e53567745515'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--eos-root', required=True, type=Path)
    parser.add_argument('--archive', required=True, type=Path)
    parser.add_argument('--expect-eos', default=PIN)
    args = parser.parse_args()
    root = args.eos_root.resolve()
    actual = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'],
                                     text=True).strip()
    assert actual == args.expect_eos, actual
    archive = args.archive.resolve()
    archive.mkdir(parents=True, exist_ok=False)
    source = root / 'crates/aether-runtime/src/bin/aether-runtime/routes.rs'
    copied = archive / 'routes.rs'
    shutil.copy2(source, copied)
    assert copied.read_bytes() == source.read_bytes()
    (archive / 'src').mkdir()
    shutil.copy2(Path(__file__).with_suffix('.rs'), archive / 'src/main.rs')
    manifest = (f'[package]\nname = "q01-selected-fifo-probe"\nversion = "0.1.0"\n'
                'edition = "2024"\n\n[dependencies]\n'
                f'aether-hal = {{ path = "{root}/crates/aether-hal" }}\n'
                'serde_json = "1"\n\n[workspace]\n')
    (archive / 'Cargo.toml').write_text(manifest)
    cargo = shutil.which('cargo') or str(Path.home() / '.cargo/bin/cargo')
    assert Path(cargo).is_file()
    with tempfile.TemporaryDirectory(prefix='q01-fifo-build-') as target:
        with (archive / 'build.log').open('w') as log:
            result = subprocess.run([cargo, 'run', '--offline', '--quiet',
                                     '--manifest-path', str(archive / 'Cargo.toml')],
                                    env=dict(os.environ, CARGO_TARGET_DIR=target),
                                    stdout=subprocess.PIPE, stderr=log, text=True)
    (archive / 'stdout.json').write_text(result.stdout)
    assert result.returncode == 0, (archive / 'build.log').read_text()[-2000:]
    observed = json.loads(result.stdout)
    assert observed['after']['overflow_count'] == 1
    assert observed['after']['gap_count'] == 1
    (archive / 'source.json').write_text(json.dumps({
        'eos_commit': args.expect_eos,
        'routes_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
        'probe_sha256': hashlib.sha256((archive / 'src/main.rs').read_bytes()).hexdigest(),
        'command': 'cargo run --offline --quiet --manifest-path Cargo.toml',
        'exit_code': result.returncode,
    }, indent=2, sort_keys=True) + '\n')
    print('Q01_FIFO_SELECTED_SOURCE_PASS')


if __name__ == '__main__':
    main()
