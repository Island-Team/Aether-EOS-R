#!/usr/bin/env python3
"""Reject one-byte tampering of every critical retained Q01 evidence class."""

import argparse
import json
from pathlib import Path
import shutil
import tempfile

from verify_q01 import verify


def flip(path):
    with path.open('r+b') as stream:
        original=stream.read(1)
        assert original
        stream.seek(0)
        stream.write(bytes([original[0]^1]))
    return original


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive',type=Path)
    args=parser.parse_args()
    source=args.archive.resolve()
    with tempfile.TemporaryDirectory(prefix='q01-tamper-') as temporary:
        root=Path(temporary)/'archive'
        shutil.copytree(source,root)
        verify(root,main_only=True)
        targets=[root/'package-input/asset.tar.gz',root/'saved-graph.raw.json',
                 root/'ui-dist/index.html',next((root/'journal').glob('*.rec')),
                 root/'q01-logs-old-unfiltered.json']
        migration=next((root/'cases/migration').rglob('source.graph.json'),None) \
            if (root/'cases/migration').is_dir() else None
        if migration is not None:
            targets.append(migration)
        for path in targets:
            original=flip(path)
            try:
                try:
                    verify(root,main_only=True)
                except (AssertionError,ValueError,KeyError,json.JSONDecodeError):
                    pass
                else:
                    raise AssertionError(f'tamper accepted: {path.relative_to(root)}')
            finally:
                with path.open('r+b') as stream:
                    stream.write(original)
        verify(root,main_only=True)
        print(f'Q01_TAMPER_REJECTED {len(targets)}')


if __name__=='__main__':
    main()
