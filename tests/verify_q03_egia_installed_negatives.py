#!/usr/bin/env python3
"""Read-only verification of the installed EGIA IsOS preflight negatives."""

import argparse
import hashlib
import json
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load(path):
    return json.loads(path.read_text())


def tree(root):
    return {p.relative_to(root).as_posix():sha(p) for p in root.rglob('*') if p.is_file()}


def strings(value):
    if isinstance(value,dict):
        for key,item in value.items():
            for path,text in strings(item):
                yield '/'+key+path,text
    elif isinstance(value,list):
        for index,item in enumerate(value):
            for path,text in strings(item):
                yield '/'+str(index)+path,text
    elif isinstance(value,str):
        yield '',value


def verify_graph(original, selected, original_home, selected_home):
    original_paths={path:value for path,value in strings(original)
                    if str(original_home) in value}
    assert len(original_paths)==10, original_paths
    expected=json.loads(json.dumps(original).replace(str(original_home),str(selected_home)))
    assert selected==expected
    for path,value in original_paths.items():
        assert value.startswith(str(original_home)+'/'), (path,value)
        assert any(token in path for token in ('socket','args','ready_unix')), path
    return sorted(original_paths)


def verify(root, positive, inputs, *, evidence_sha256, inputs_sha256, positive_sha256):
    assert sha(inputs)==inputs_sha256
    assert sha(positive/'evidence.sha256.json')==positive_sha256
    assert sha(root/'evidence.sha256.json')==evidence_sha256
    manifest=load(root/'evidence.sha256.json')
    actual=tree(root)
    actual.pop('evidence.sha256.json')
    assert actual==manifest, 'negative archive file set or digest changed'
    summary=load(root/'negative-summary.json')
    receipt=load(inputs)
    assert summary['inputs_sha256']==inputs_sha256
    assert summary['positive_evidence_sha256']==positive_sha256
    assert summary['wheel_sha256']==receipt['wheel_sha256']
    assert summary['negative_harness_sha256']==sha(root/'q03-negative-harness.py')
    source_home=Path((positive/'home-path.txt').read_text().strip())
    graph=load(positive/'graph.json')
    paths={}
    for name in ('selected','wrong-interpreter'):
        home=root/name/'home'
        selected=load(home/'config/deployments/default.nodes.json')
        socket=selected['cores']['eos']['layers']['process']['groups']['mujoco'][
            'nodes']['simulator']['settings']['socket']
        declared_home=Path(socket).parent.parent
        assert declared_home.name=='home' and declared_home.parent.name==name
        paths[name]=verify_graph(graph,selected,source_home,declared_home)
        applied=load(root/name/'apply.json')
        assert applied['saved_graph_sha256']==sha(home/'config/deployments/default.nodes.json')
        assert applied['revision']==applied['saved_graph_sha256']
        assert applied['selected_wheel_sha256']==receipt['wheel_sha256']
    assert paths['selected']==paths['wrong-interpreter']
    control=load(root/'selected/control.json')
    assert control['http_status']==200 and control['reply']['ok']
    assert control['after']['state']=='running'
    assert control['stop_http_status']==200 and control['stop_reply']['ok']
    assert control['stopped']['state']=='stopped' and not control['stopped']['processes']
    for name,parent in (('tampered_module','selected'),('changed_overlay','selected'),
                        ('wrong_interpreter','wrong-interpreter')):
        case=load(root/parent/(name+'.json'))
        diagnostic=case['reply']['data']['diagnostics'][0]
        assert case['http_status']!=200 and not case['reply']['ok']
        assert (diagnostic['code'],diagnostic['stage'],diagnostic['path'])==(
            'generated_manifest_mismatch','snapshot','')
        assert not case['before']['processes'] and not case['after']['processes']
        assert case['after']['active_revision']==case['before']['active_revision']
        assert case['after']['run_id']==case['before']['run_id']
    assert summary['failure_cases_no_child_spawn'] is True
    print('Q03_EGIA_INSTALLED_NEGATIVE_VERIFY_PASS',len(paths['selected']),flush=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive',type=Path)
    parser.add_argument('positive',type=Path)
    parser.add_argument('inputs',type=Path)
    parser.add_argument('--expect-evidence-sha256',required=True)
    parser.add_argument('--expect-positive-sha256',required=True)
    parser.add_argument('--expect-inputs-sha256',required=True)
    args=parser.parse_args()
    verify(args.archive,args.positive,args.inputs,
           evidence_sha256=args.expect_evidence_sha256,
           inputs_sha256=args.expect_inputs_sha256,
           positive_sha256=args.expect_positive_sha256)


if __name__=='__main__':
    main()
