#!/usr/bin/env python3
"""Reject a one-byte tampered selected MuJoCo archive before graph Apply."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(binary,env,*arguments):
    return subprocess.run([str(binary),*arguments],env=env,capture_output=True,text=True,
                          timeout=120)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--isos-bin',type=Path,required=True)
    parser.add_argument('--manifest',type=Path,required=True)
    parser.add_argument('--asset',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    home=Path(tempfile.mkdtemp(prefix='q01-package-negative-'))
    env=dict(os.environ,ISOS_HOME=str(home))
    try:
        initialized=command(args.isos_bin,env,'init','--mode','dev')
        assert initialized.returncode==0
        graph=home/'config/deployments/default.nodes.json'
        before=graph.read_bytes()
        before_status=json.loads(command(args.isos_bin,env,'status','--json').stdout)
        bad=out/'tampered-asset.tar.gz'
        shutil.copy2(args.asset,bad)
        with bad.open('r+b') as stream:
            first=stream.read(1)
            stream.seek(0)
            stream.write(bytes([first[0]^1]))
        assert sha(bad)!=sha(args.asset)
        result=command(args.isos_bin,env,'pkg','install','--manifest',str(args.manifest),
                       '--asset',str(bad),'--allow-unapproved')
        assert result.returncode!=0
        after_status=json.loads(command(args.isos_bin,env,'status','--json').stdout)
        assert graph.read_bytes()==before
        for key in ('active_revision','state','processes','run_id','run_id_state'):
            assert after_status[key]==before_status[key]
        (out/'saved-graph.raw.json').write_bytes(before)
        (out/'install.stdout').write_text(result.stdout)
        (out/'install.stderr').write_text(result.stderr)
        (out/'result.json').write_text(json.dumps({
            'returncode':result.returncode,'asset_sha256':sha(args.asset),
            'tampered_sha256':sha(bad),'saved_revision':sha(graph),
            'status_before':before_status,'status_after':after_status,
            'installed':(home/'data/packages/island/mujoco-sim/0.1.1').exists()},
            indent=2,sort_keys=True)+'\n')
        assert not (home/'data/packages/island/mujoco-sim/0.1.1').exists()
        print('Q01_PACKAGE_TAMPER_REJECTED')
    finally:
        # The home contains no live child and is retained until evidence is sealed.
        (out/'home-path.txt').write_text(str(home)+'\n')


if __name__=='__main__':
    main()
