"""Validate the selected capture's simulated hosted Origin boundary."""

from urllib.parse import urlsplit


APPROVED = 'https://os-island.com'


def verify_hosted_origin(record, ready):
    assert set(record) == {'label','synthetic_origin','approved_forwarded_origin',
                           'destination_ports','direct_without_translation',
                           'browser_probe','requests'}
    hosted = ready['hosted_urls']
    assert set(hosted) == {'en','cn'}
    parsed_hosted = {language:urlsplit(hosted[language]) for language in ('en','cn')}
    for language, parsed in parsed_hosted.items():
        assert parsed.scheme == 'http' and parsed.hostname == '127.0.0.1'
        assert parsed.path == f'/{language}/isos' and not parsed.query and not parsed.fragment
    ports = {parsed.port for parsed in parsed_hosted.values()}
    assert len(ports) == 1 and None not in ports
    assert all(type(ready[key]) is int and 0 < ready[key] < 65536
               for key in ('api_port','bridge_port'))
    assert ready['api_port'] != ready['bridge_port']
    synthetic = f'http://127.0.0.1:{ports.pop()}'
    assert record['label'] == 'simulated hosted-Origin browser evidence'
    assert record['synthetic_origin'] == synthetic
    assert record['approved_forwarded_origin'] == APPROVED
    assert record['destination_ports'] == {
        'isos':ready['api_port'],'eos':ready['bridge_port']}
    control_url=f"http://127.0.0.1:{ready['bridge_port']}/api/eos/v2/control"
    assert record['browser_probe'] in (
        {'mode':'ui-navigation'},
        {'mode':'explicit-browser-fetch','url':control_url,'status':200})
    direct = record['direct_without_translation']
    assert type(direct) is list and len(direct) == 2
    assert direct == [
        {'owner':'isos','method':'GET','path':'/api/isos/v1/process-status',
         'origin':synthetic,'status':403,'access_control_allow_origin':None},
        {'owner':'eos','method':'OPTIONS','path':'/api/eos/v2/control',
         'origin':synthetic,'status':403,'access_control_allow_origin':None}]
    rows = record['requests']
    assert type(rows) is list and rows, 'missing translated owner API requests'
    seen = set()
    for row in rows:
        assert set(row) == {'method','url','original_origin','forwarded_origin',
                            'raw_status','delivered_status',
                            'raw_access_control_allow_origin',
                            'delivered_access_control_allow_origin',
                            'preflight_request_headers','raw_cors_headers',
                            'delivered_cors_headers','simulated_hosted_origin'}
        parsed = urlsplit(row['url'])
        assert parsed.scheme == 'http' and parsed.hostname == '127.0.0.1'
        assert not parsed.fragment and not parsed.username and not parsed.password
        if parsed.port == ready['api_port']:
            assert parsed.path.startswith('/api/isos/v1/')
            owner='isos'
        else:
            assert parsed.port == ready['bridge_port']
            assert parsed.path.startswith('/api/eos/v2/')
            owner='eos'
        method=row['method']
        assert method in ('GET','POST','OPTIONS')
        assert row['simulated_hosted_origin'] is True
        assert row['original_origin'] == synthetic
        assert row['forwarded_origin'] == APPROVED
        assert type(row['raw_status']) is int and 200 <= row['raw_status'] < 600
        assert row['delivered_status'] == row['raw_status']
        assert row['raw_access_control_allow_origin'] == APPROVED
        assert row['delivered_access_control_allow_origin'] == synthetic
        raw=row['raw_cors_headers']
        delivered=row['delivered_cors_headers']
        assert type(raw) is dict and type(delivered) is dict
        assert raw.get('access-control-allow-origin') == APPROVED
        assert delivered == {**raw,'access-control-allow-origin':synthetic}
        preflight=row['preflight_request_headers']
        assert type(preflight) is dict
        assert all(key.startswith('access-control-request-') for key in preflight)
        if method == 'OPTIONS':
            assert preflight.get('access-control-request-method') in ('GET','POST')
            if owner == 'eos':
                assert parsed.path == '/api/eos/v2/control' and not parsed.query
                assert preflight['access-control-request-method'] == 'POST'
                assert preflight.get('access-control-request-headers', '').lower() == \
                    'content-type'
                assert 'POST' in {part.strip().upper() for part in
                    raw.get('access-control-allow-methods','').split(',')}
                assert 'content-type' in {part.strip().lower() for part in
                    raw.get('access-control-allow-headers','').split(',')}
        else:
            assert preflight == {}
        if row['raw_status'] < 300:
            seen.add((owner,method))
    assert ('isos','GET') in seen, 'selected IsOS GET not observed'
    assert ('eos','OPTIONS') in seen, 'selected EOS preflight not observed'
    return {'synthetic_origin':synthetic,'approved_forwarded_origin':APPROVED,
            'direct_403_owners':['isos','eos'],
            'translated_request_count':len(rows),
            'selected_methods':sorted(f'{owner}.{method}' for owner,method in seen)}
