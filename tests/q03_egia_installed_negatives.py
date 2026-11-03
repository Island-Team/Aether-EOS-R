#!/usr/bin/env python3
"""Real IsOS preflight negatives for a pinned installed EGIA prefix."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import time

from m4_affected_replay import api, save, sha, verify_installed_egia


def port():
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        return listener.getsockname()[1]


def environment(home, prefix, args, *, interpreter=None):
    env=dict(os.environ, ISOS_HOME=str(home), ISOS_HTTP_PORT=str(port()),
             ISOSUI_PORT=str(port()), ISOS_AETHER_BIN_DIR=str(args.eos_bin_dir),
             ISOSUI_DIST_DIR=str(args.ui_dist), MUJOCO_ROOT=str(args.mujoco_root),
             ISOS_EGIA_BIN_DIR=str(prefix/'bin'),
             ISOS_EGIA_DEFAULTS_DIR=str(home/'egia-defaults'),
             ISOS_EGIA_FACTORY_DEFAULTS_DIR=str(prefix/'share/defaults/egia'),
             EGIA_PYTHON=str(interpreter or prefix/'.venv/bin/python'),
             ISLAND_PREFIX=str(prefix), ISLAND_MODE='system',
             ISLAND_ETC=str(prefix/'etc/island'),
             ISLAND_SHARE=str(prefix/'share/island'),
             ISLAND_CONFIG=str(prefix/'etc/island/island.toml'),
             EGIA_ETC=str(prefix/'etc/island/egia'),
             ISLAND_CACHE=str(home/'data/cache/default'),
             ISLAND_LOG=str(home/'data/logs/default'),
             ISLAND_RUN=str(home/'run'), ISLAND_TMP=str(home/'tmp'))
    for key in ('PYTHONPATH','ISLAND_LIB','ISLAND_LIBEXEC','ISLAND_PYTHON','ISOS_CONFIG'):
        env.pop(key,None)
    return env


def server(isos, home, env, log_path):
    log=log_path.open('w')
    child=subprocess.Popen([str(isos),'serve'],env=env,cwd=home,
                           stdout=log,stderr=subprocess.STDOUT,text=True)
    deadline=time.monotonic()+60
    try:
        while True:
            assert child.poll() is None, (child.returncode,log_path)
            try:
                api(int(env['ISOS_HTTP_PORT']),'process-status',timeout=2)
                return child,log
            except (OSError,TimeoutError):
                assert time.monotonic()<deadline, log_path
                time.sleep(.2)
    except BaseException:
        child.terminate(); child.wait(timeout=15); log.close()
        raise


def close_server(child, log):
    child.terminate()
    try: child.wait(timeout=15)
    except subprocess.TimeoutExpired:
        child.kill(); child.wait()
    log.close()


def command(isos, home, env, archive, label, *argv):
    result=subprocess.run([str(isos),*argv],cwd=home,env=env,
                          text=True,capture_output=True,timeout=120)
    (archive/f'{label}.stdout').write_text(result.stdout)
    (archive/f'{label}.stderr').write_text(result.stderr)
    assert result.returncode==0, (argv,result.returncode,result.stderr)


def setup(archive, name, args, prefix, source_defaults, graph):
    root=archive/name
    home=root/'home'
    home.mkdir(parents=True)
    shutil.copytree(source_defaults,home/'egia-defaults')
    source_home=source_defaults.parent
    selected_graph=json.loads(json.dumps(graph).replace(str(source_home),str(home)))
    assert str(source_home) not in json.dumps(selected_graph)
    env=environment(home,prefix,args)
    command(args.isos,home,env,root,'pkg-link','pkg','link','island/mujoco-sim',
            '0.1.1',str(args.bundle),'--manifest',str(args.release))
    command(args.isos,home,env,root,'init','init','--mode','dev')
    serve,log=server(args.isos,home,env,root/'apply.serve.log')
    api_port=int(env['ISOS_HTTP_PORT'])
    try:
        saved=api(api_port,'node-graph')
        validation=api(api_port,'node-graph/validate','POST',
                       {'schema_version':1,'graph':selected_graph})
        assert validation['valid'], validation
        applied=api(api_port,'node-graph','PUT',
                    {'schema_version':1,'revision':saved['revision'],'graph':selected_graph})
        revision=applied['revision']
        assert sha(home/'config/deployments/default.nodes.json')==revision
        assert not api(api_port,'process-status')['processes']
        save(root,'apply.json',{'revision':revision,'validation':validation,
             'saved_graph_sha256':sha(home/'config/deployments/default.nodes.json'),
             'selected_wheel_sha256':args.wheel_sha256})
    finally:
        close_server(serve,log)
    return home,revision


def start_case(api_port, revision, label, root, *, expected_failure):
    before=api(api_port,'process-status')
    assert not before['processes'], before
    status,reply=api(api_port,'process-control','POST',
                     {'schema_version':1,'operation':'start','revision':revision},
                     checked=False,timeout=600)
    after=api(api_port,'process-status')
    result={'case':label,'http_status':status,'reply':reply,
            'before':before,'after':after}
    if expected_failure:
        diagnostic=((reply.get('data') or {}).get('diagnostics') or [None])[0]
        assert status!=200 and not reply['ok'], result
        assert diagnostic and diagnostic['code']=='generated_manifest_mismatch', result
        assert diagnostic['path']=='' and diagnostic['stage']=='snapshot', result
        assert not after['processes'], after
        assert after['active_revision']==before['active_revision'], (before,after)
        assert after['run_id']==before['run_id'], (before,after)
    else:
        assert status==200 and reply['ok'] and after['state']=='running', result
        assert {'eos','egia','bridge','config'} <= {p['id'] for p in after['processes']}, after
        stop_status,stop_reply=api(api_port,'process-control','POST',
            {'schema_version':1,'operation':'stop','revision':revision},
            checked=False,timeout=120)
        stopped=api(api_port,'process-status')
        result.update(stop_http_status=stop_status,stop_reply=stop_reply,stopped=stopped)
        assert stop_status==200 and stop_reply['ok'], result
        assert stopped['state']=='stopped' and not stopped['processes'], stopped
        assert all(not Path(f'/proc/{p["pid"]}').exists() for p in after['processes']), after
    save(root,f'{label}.json',result)


def digest_tree(root):
    return {p.relative_to(root).as_posix():sha(p) for p in root.rglob('*') if p.is_file()}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('positive','inputs','isos','eos-bin-dir','ui-dist','mujoco-root',
                 'bundle','release','archive'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--positive-evidence-sha256',required=True)
    parser.add_argument('--expect-inputs-sha256',required=True)
    a=parser.parse_args()
    assert sha(a.inputs)==a.expect_inputs_sha256
    assert sha(a.positive/'evidence.sha256.json')==a.positive_evidence_sha256
    inputs=json.loads(a.inputs.read_text())
    prefix=Path(inputs['prefix'])
    a.wheel_sha256=inputs['wheel_sha256']
    assert sha(a.release)==inputs['package']['release_sha256']
    assert sha(next((prefix/'wheels').glob('egia-0.1.0-*.whl')))==a.wheel_sha256
    source_home=Path((a.positive/'home-path.txt').read_text().strip())
    graph=json.loads((a.positive/'graph.json').read_text())
    a.archive.mkdir(parents=True,exist_ok=False)
    shutil.copy2(Path(__file__),a.archive/'q03-negative-harness.py')
    verify_installed_egia(prefix,prefix/'.venv/bin/python',a.archive,a.archive)
    home,revision=setup(a.archive,'selected',a,prefix,source_home/'egia-defaults',graph)
    module=prefix/'lib/island/egia/api/chat.py'
    selection=json.loads((home/'data/generated/default/selection.json').read_text())
    leaf=home/'data/generated/default/v2'/revision/selection['entries'][revision]['generation']
    overlay=leaf/'egia/stages.json'
    module_bytes,overlay_bytes=module.read_bytes(),overlay.read_bytes()
    env=environment(home,prefix,a)
    serve,log=server(a.isos,home,env,a.archive/'selected/live.serve.log')
    try:
        api_port=int(env['ISOS_HTTP_PORT'])
        start_case(api_port,revision,'control',a.archive/'selected',expected_failure=False)
        module.write_bytes(module_bytes+b'\n# Q03 tamper probe\n')
        try: start_case(api_port,revision,'tampered_module',a.archive/'selected',expected_failure=True)
        finally: module.write_bytes(module_bytes)
        overlay.write_bytes(overlay_bytes+b'\n')
        try: start_case(api_port,revision,'changed_overlay',a.archive/'selected',expected_failure=True)
        finally: overlay.write_bytes(overlay_bytes)
    finally:
        close_server(serve,log)
        assert module.read_bytes()==module_bytes and overlay.read_bytes()==overlay_bytes
    wrong_home,wrong_revision=setup(a.archive,'wrong-interpreter',a,prefix,
                                    source_home/'egia-defaults',graph)
    wrong_env=environment(wrong_home,prefix,a,interpreter=Path('/usr/bin/python3'))
    wrong,wrong_log=server(a.isos,wrong_home,wrong_env,
                           a.archive/'wrong-interpreter/live.serve.log')
    try:
        start_case(int(wrong_env['ISOS_HTTP_PORT']),wrong_revision,'wrong_interpreter',
                   a.archive/'wrong-interpreter',expected_failure=True)
    finally:
        close_server(wrong,wrong_log)
    verify_installed_egia(prefix,prefix/'.venv/bin/python',a.archive,a.archive)
    save(a.archive,'negative-summary.json',{
        'inputs_sha256':a.expect_inputs_sha256,
        'positive_evidence_sha256':a.positive_evidence_sha256,
        'prefix':str(prefix),'wheel_sha256':a.wheel_sha256,
        'negative_harness_sha256':sha(a.archive/'q03-negative-harness.py'),
        'selected_revision':revision,'wrong_interpreter_revision':wrong_revision,
        'cases':['control','tampered_module','changed_overlay','wrong_interpreter'],
        'failure_cases_no_child_spawn':True,
        'module_sha256':hashlib.sha256(module_bytes).hexdigest(),
        'overlay_sha256':hashlib.sha256(overlay_bytes).hexdigest()})
    seal=a.archive/'evidence.sha256.json'
    seal.write_text(json.dumps(digest_tree(a.archive),sort_keys=True,indent=2)+'\n')
    print('Q03_INSTALLED_NEGATIVE_SEAL_SHA256',sha(seal),flush=True)
    print('Q03_INSTALLED_NEGATIVE_PREFLIGHT_PASS',flush=True)


if __name__=='__main__':
    main()
