#!/usr/bin/env python3
"""Prepare canonical six-case byte receipt for independent reviewer freezing."""

import argparse
from pathlib import Path

from q01_selection import (CASES, CASE_UI_DIRS, canonical_bytes,
                           load_build_receipt, load_selection, sha256)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive',required=True,type=Path)
    parser.add_argument('--selection',required=True,type=Path)
    parser.add_argument('--expect-selection-sha256',required=True)
    parser.add_argument('--main-build-receipt',required=True,type=Path)
    parser.add_argument('--expect-main-build-receipt-sha256',required=True)
    parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args()
    assert args.output.is_absolute() and not args.output.exists()
    load_selection(args.selection,args.expect_selection_sha256)
    load_build_receipt(args.main_build_receipt,
                       args.expect_main_build_receipt_sha256,
                       args.expect_selection_sha256)
    root=args.archive.resolve()
    cases={}
    for name in CASES:
        case=root/'cases'/name
        assert case.is_dir() and (case/'evidence.sha256.json').is_file(), name
        ui_files={}
        if name in CASE_UI_DIRS:
            ui=case/CASE_UI_DIRS[name]
            assert ui.is_dir(), name
            ui_files={path.relative_to(ui).as_posix():sha256(path.read_bytes())
                      for path in ui.rglob('*') if path.is_file()}
        cases[name]={'manifest_sha256':sha256((case/'evidence.sha256.json').read_bytes()),
                     'ui_files_sha256':ui_files}
    hosted_origin=root/'q03-simulated-hosted-origin.json'
    assert hosted_origin.is_file(), 'selected hosted-Origin evidence required'
    receipt={'schema_version':1,'profile':'selected-q03',
             'selection_sha256':args.expect_selection_sha256,
             'main_build_receipt_sha256':args.expect_main_build_receipt_sha256,
             'hosted_origin_sha256':sha256(hosted_origin.read_bytes()),
             'cases':cases}
    args.output.write_bytes(canonical_bytes(receipt))
    print(sha256(args.output.read_bytes()))


if __name__=='__main__':
    main()
