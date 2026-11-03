#!/usr/bin/env python3
"""Require semantic rejection of resealed selected-pin G06/R03 case copies."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile

from verify_q01_g06_r03 import verify


def replace(path: Path, data: bytes) -> None:
    temporary = path.with_name(path.name + '.changed')
    temporary.write_bytes(data)
    os.replace(temporary, path)


def save(path: Path, value: object) -> None:
    replace(path, (json.dumps(value, indent=2, sort_keys=True) + '\n').encode())


def read(path: Path):
    return json.loads(path.read_text())


def reseal(root: Path) -> None:
    save(root / 'evidence.sha256.json', {
        path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in root.rglob('*') if path.is_file() and path.name != 'evidence.sha256.json'
    })


def mutate(name: str, root: Path) -> None:
    if name == 'owner-pin':
        pins = read(root / 'selected-pins.json')
        pins['isos_source'] = 'ac984bcf998e9252cc4e5c222620dca3ae6d3c37'
        save(root / 'selected-pins.json', pins)
    elif name == 'unrelated-candidate-edit':
        candidate = read(root / 'actuator-bypass-candidate.json')
        candidate['robot']['name'] = 'mutated outside edge under test'
        save(root / 'actuator-bypass-candidate.json', candidate)
    elif name == 'journal-identity':
        record = root / 'journal-probe' / ('q01-uncertain'.encode().hex() + '.rec')
        payload = read(record)
        payload['body']['operation']['identity']['target']['hardware']['node_path'] = 'eos/hardware/base/other'
        wire = json.dumps(payload['body'], separators=(',', ':'), ensure_ascii=False).encode()
        checksum = 0xcbf29ce484222325
        for byte in wire:
            checksum = ((checksum ^ byte) * 0x100000001b3) & 0xffffffffffffffff
        payload['checksum'] = checksum
        replace(record, (json.dumps(payload, indent=2, ensure_ascii=False) + '\n').encode())
    else:
        raise AssertionError(name)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    args = parser.parse_args()
    source = args.archive.resolve()
    verify(source)
    with tempfile.TemporaryDirectory(prefix='q01-g06-r03-tamper-', dir=source.parent) as temporary:
        for name in ('owner-pin', 'unrelated-candidate-edit', 'journal-identity'):
            root = Path(temporary) / name
            shutil.copytree(source, root, copy_function=os.link)
            mutate(name, root)
            reseal(root)
            try:
                verify(root)
            except (AssertionError, KeyError, ValueError) as error:
                print(name, 'REJECTED', type(error).__name__, str(error)[:160])
            else:
                raise AssertionError(f'{name}: resealed semantic mutation accepted')


if __name__ == '__main__':
    main()
