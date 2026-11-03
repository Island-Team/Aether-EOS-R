#!/usr/bin/env python3
"""Run frozen graph compatibility cases in disposable IsOS homes for Q01."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

import m1_cross_repo_replay as m1
from q01_selection import load_selection


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--isos-root',type=Path,required=True)
    parser.add_argument('--isos-bin',type=Path,required=True)
    parser.add_argument('--eos-bin-dir',type=Path,required=True)
    parser.add_argument('--ui-dist',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--selection',type=Path,
                        help='externally frozen Q03 owner selection')
    parser.add_argument('--expect-selection-sha256',
                        help='reviewer-supplied digest of the selection file')
    args = parser.parse_args()
    assert bool(args.selection) == bool(args.expect_selection_sha256), \
        'selection and its external SHA-256 must be supplied together'
    selection = (load_selection(args.selection, args.expect_selection_sha256)
                 if args.selection else None)
    m1.ISOS=args.isos_root
    m1.FIXTURES=args.isos_root/'tests/fixtures/graph_compat'
    m1.ARTIFACTS=args.output.resolve()
    m1.ARTIFACTS.mkdir(parents=True,exist_ok=False)
    source = subprocess.check_output(['git','-C',str(args.isos_root),'rev-parse','HEAD'],
                                     text=True).strip()
    tree = subprocess.check_output(['git','-C',str(args.isos_root),'rev-parse',
                                    'HEAD^{tree}'],text=True).strip()
    if selection is None:
        assert source == '00c1d7aa184de8b969e6e40472bf9b1fa55032ff'
    else:
        owner = selection['owners']['isos']
        assert source == owner['commit'] and tree == owner['tree']
        for name, path in (('tests/q01_migration_replay.py', Path(__file__)),
                           ('tests/m1_cross_repo_replay.py', Path(m1.__file__))):
            assert hashlib.sha256(path.read_bytes()).hexdigest() == selection['harness'][name]
        lock = subprocess.check_output(
            ['git','-C',str(args.isos_root),'show',f'{source}:Cargo.lock'])
        assert hashlib.sha256(lock).hexdigest() == owner['locks']['Cargo.lock']
        assert not subprocess.check_output(
            ['git','-C',str(args.isos_root),'status','--porcelain',
             '--untracked-files=no'], text=True), 'selected IsOS checkout is dirty'
    shutil.copy2(args.isos_bin,m1.ARTIFACTS/'selected-isos.bin')
    pins = {
        'isos_source':source,'isos_tree':tree,
        'isos_binary_sha256':hashlib.sha256((m1.ARTIFACTS/'selected-isos.bin').read_bytes()).hexdigest(),
    }
    if selection is not None:
        pins['selection_sha256'] = args.expect_selection_sha256
    (m1.ARTIFACTS/'selected-pins.json').write_text(json.dumps(pins,indent=2,sort_keys=True)+'\n')
    m1.legacy_and_negative(args.isos_bin,args.eos_bin_dir,args.ui_dist)
    case=m1.Case('explicit-stock-migration',args.isos_bin,args.eos_bin_dir,args.ui_dist)
    try:
        source=(m1.FIXTURES/'legacy_1_0_0.json').read_bytes()
        assert m1.digest(source)==m1.EXPECTED['legacy_1_0_0.json']
        case.graph.write_bytes(source)
        case.serve()
        old=case.snapshot('source')
        code,preview=case.api('node-graph/migration/preview','POST',
                              {'schema_version':1,'revision':old})
        assert code==200 and preview['data']['can_apply']
        proposed=preview['data']['proposed_revision']
        assert case.snapshot('after-preview')==old
        code,stale=case.api('node-graph/migration/apply','POST',
                            {'schema_version':1,'revision':'0'*64,'proposed_revision':proposed})
        assert code==409 and not stale['ok']
        assert case.snapshot('after-stale')==old
        code,applied=case.api('node-graph/migration/apply','POST',
                              {'schema_version':1,'revision':old,'proposed_revision':proposed})
        assert code==200 and applied['data']['revision']==proposed
        assert case.snapshot('migrated')==proposed
        actual=json.loads(case.graph.read_text())
        assert actual['version']=='0.1.0'
        assert actual==preview['data']['graph']
        assert not actual.get('packages'), 'stock fixture must not invent a package pin'
    finally:
        case.close()
    print('Q01_MIGRATION_CASES_PASS')


if __name__=='__main__':
    main()
