"""Selected Q03 proxy origin simulation is narrow and records raw upstream CORS."""

import http.client
from copy import deepcopy
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from threading import Thread
import unittest

from q03_browser_capture import direct_synthetic_origin_denials
from q03_browser_capture import eos_post_preflight_seen
from q03_hosted_origin import verify_hosted_origin
from q03_recording_proxy import RecordingProxy


class Owner(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass

    def do_GET(self):
        self.answer()

    def do_OPTIONS(self):
        self.answer()

    def do_POST(self):
        self.answer()

    def answer(self):
        origin=self.headers.get('Origin')
        approved=origin == 'https://os-island.com'
        self.send_response((204 if self.command=='OPTIONS' else 200) if approved else 403)
        if approved and self.path != '/api/isos/v1/no-acao':
            self.send_header('Access-Control-Allow-Origin',origin)
            if self.command == 'OPTIONS':
                self.send_header('Access-Control-Allow-Methods','GET, POST, OPTIONS')
                self.send_header('Access-Control-Allow-Headers','content-type')
        self.send_header('Content-Length','0')
        self.end_headers()


def request(port, method, url, origin):
    connection=http.client.HTTPConnection('127.0.0.1',port,timeout=5)
    headers={'Origin':origin}
    if method == 'OPTIONS':
        headers['Access-Control-Request-Method']='POST'
        headers['Access-Control-Request-Headers']='content-type'
    connection.request(method,url,headers=headers)
    response=connection.getresponse()
    result=(response.status,response.getheader('Access-Control-Allow-Origin'))
    response.read()
    connection.close()
    return result


class HostedOriginProxyTest(unittest.TestCase):
    def test_selected_evidence_rejects_missing_and_contradictory_rows(self):
        synthetic='http://127.0.0.1:18081'
        ready={'hosted_urls':{'en':synthetic+'/en/isos',
                              'cn':synthetic+'/cn/isos'},
               'api_port':16000,'bridge_port':16020}
        def row(owner,method):
            port,path=(16000,'/api/isos/v1/process-status') if owner=='isos' else (
                16020,'/api/eos/v2/control')
            preflight=({'access-control-request-method':'POST',
                        'access-control-request-headers':'content-type'}
                       if method=='OPTIONS' else {})
            raw_cors={'access-control-allow-origin':'https://os-island.com'}
            if method == 'OPTIONS':
                raw_cors.update({'access-control-allow-methods':'GET, POST, OPTIONS',
                                 'access-control-allow-headers':'content-type'})
            return {'method':method,'url':f'http://127.0.0.1:{port}{path}',
                'original_origin':synthetic,'forwarded_origin':'https://os-island.com',
                'raw_status':204 if method=='OPTIONS' else 200,
                'delivered_status':204 if method=='OPTIONS' else 200,
                'raw_access_control_allow_origin':'https://os-island.com',
                'delivered_access_control_allow_origin':synthetic,
                'preflight_request_headers':preflight,
                'raw_cors_headers':raw_cors,
                'delivered_cors_headers':{**raw_cors,
                    'access-control-allow-origin':synthetic},
                'simulated_hosted_origin':True}
        record={'label':'simulated hosted-Origin browser evidence',
            'synthetic_origin':synthetic,
            'approved_forwarded_origin':'https://os-island.com',
            'destination_ports':{'isos':16000,'eos':16020},
            'browser_probe':{'mode':'ui-navigation'},
            'direct_without_translation':[
                {'owner':'isos','method':'GET','path':'/api/isos/v1/process-status',
                 'origin':synthetic,'status':403,'access_control_allow_origin':None},
                {'owner':'eos','method':'OPTIONS','path':'/api/eos/v2/control',
                 'origin':synthetic,'status':403,'access_control_allow_origin':None}],
            'requests':[row('isos','GET'),row('eos','OPTIONS')]}
        self.assertEqual(verify_hosted_origin(record,ready)['translated_request_count'],2)
        self.assertTrue(eos_post_preflight_seen(record['requests'],
            'http://127.0.0.1:16020/api/eos/v2/control'))
        explicit=deepcopy(record)
        explicit['browser_probe']={'mode':'explicit-browser-fetch',
            'url':'http://127.0.0.1:16020/api/eos/v2/control','status':200}
        self.assertEqual(verify_hosted_origin(explicit,ready)['translated_request_count'],2)
        mutations=[
            lambda value:value.pop('requests'),
            lambda value:value['requests'].pop(),
            lambda value:value['requests'][0].update(raw_status=403),
            lambda value:value['requests'][1].update(raw_status=403,
                                                       delivered_status=403),
            lambda value:value['requests'][0].update(delivered_status=204),
            lambda value:value['requests'][0].update(
                raw_access_control_allow_origin=synthetic),
            lambda value:value['requests'][0]['raw_cors_headers'].update(
                {'access-control-allow-origin':synthetic}),
            lambda value:value['requests'][1].update(
                delivered_access_control_allow_origin='https://os-island.com'),
            lambda value:value['requests'][1].update(url='http://127.0.0.1:1/api/eos/v2/control'),
            lambda value:value['requests'][1].update(method='GET'),
            lambda value:value.update(browser_probe={'mode':'explicit-browser-fetch',
                'url':'http://127.0.0.1:1/api/eos/v2/control','status':200}),
            lambda value:value['requests'][1]['preflight_request_headers'].update(
                {'access-control-request-method':'GET'}),
            lambda value:value['requests'][1]['preflight_request_headers'].pop(
                'access-control-request-headers'),
            lambda value:(value['requests'][1]['raw_cors_headers'].pop(
                'access-control-allow-methods'),
                value['requests'][1]['delivered_cors_headers'].pop(
                    'access-control-allow-methods')),
            lambda value:(value['requests'][1]['raw_cors_headers'].pop(
                'access-control-allow-headers'),
                value['requests'][1]['delivered_cors_headers'].pop(
                    'access-control-allow-headers')),
            lambda value:value['direct_without_translation'][0].update(status=200),
            lambda value:value['direct_without_translation'][1].update(
                access_control_allow_origin=synthetic),
            lambda value:value.update(synthetic_origin='http://127.0.0.1:1'),
        ]
        for mutate in mutations:
            bad=deepcopy(record)
            mutate(bad)
            with self.subTest(mutate=mutate),self.assertRaises((AssertionError,KeyError)):
                verify_hosted_origin(bad,ready)

    def test_selected_api_only_and_raw_denial(self):
        isos=ThreadingHTTPServer(('127.0.0.1',0),Owner)
        eos=ThreadingHTTPServer(('127.0.0.1',0),Owner)
        threads=[Thread(target=server.serve_forever,daemon=True)
                 for server in (isos,eos)]
        for thread in threads:
            thread.start()
        synthetic='http://127.0.0.1:18081'
        proxy=RecordingProxy(hosted_origin=synthetic,isos_port=isos.server_port,
                             eos_port=eos.server_port).start()
        try:
            isos_api=f'http://127.0.0.1:{isos.server_port}/api/isos/v1/node-graph'
            eos_api=f'http://127.0.0.1:{eos.server_port}/api/eos/v2/control'
            for method,url in [('GET',isos_api),('OPTIONS',isos_api),
                               ('POST',eos_api),('OPTIONS',eos_api)]:
                self.assertEqual(request(proxy.port,method,url,synthetic),
                                 (204 if method=='OPTIONS' else 200,synthetic))
            self.assertEqual(len(proxy.cors_rows),4)
            for row in proxy.cors_rows:
                self.assertEqual(row['original_origin'],synthetic)
                self.assertEqual(row['forwarded_origin'],'https://os-island.com')
                self.assertEqual(row['raw_access_control_allow_origin'],
                                 'https://os-island.com')
                self.assertEqual(row['delivered_access_control_allow_origin'],synthetic)
                self.assertEqual(row['raw_status'],
                                 204 if row['method']=='OPTIONS' else 200)
            record={'label':'simulated hosted-Origin browser evidence',
                'synthetic_origin':synthetic,
                'approved_forwarded_origin':'https://os-island.com',
                'destination_ports':{'isos':isos.server_port,'eos':eos.server_port},
                'direct_without_translation':direct_synthetic_origin_denials(
                    {'api_port':isos.server_port,'bridge_port':eos.server_port},synthetic),
                'browser_probe':{'mode':'ui-navigation'},
                'requests':list(proxy.cors_rows)}
            ready={'hosted_urls':{'en':synthetic+'/en/isos',
                                  'cn':synthetic+'/cn/isos'},
                   'api_port':isos.server_port,'bridge_port':eos.server_port}
            self.assertEqual(verify_hosted_origin(record,ready)['translated_request_count'],4)
            self.assertEqual(request(isos.server_port,'GET',
                             '/api/isos/v1/node-graph',synthetic),(403,None))
            self.assertEqual(request(eos.server_port,'OPTIONS',
                             '/api/eos/v2/control',synthetic),(403,None))
            self.assertEqual([row['status'] for row in direct_synthetic_origin_denials(
                {'api_port':isos.server_port,'bridge_port':eos.server_port},synthetic)],
                [403,403])
            self.assertEqual(request(proxy.port,'GET',
                f'http://127.0.0.1:{isos.server_port}/api/isos/v1/no-acao',synthetic),
                (200,None))
            self.assertEqual(request(proxy.port,'GET',
                f'http://127.0.0.1:{isos.server_port}/private',synthetic),
                (403,None))
            self.assertEqual(request(proxy.port,'GET',isos_api,
                'http://127.0.0.1:18082'),(403,None))
            self.assertEqual(len(proxy.cors_rows),5)
        finally:
            proxy.close()
            for server in (isos,eos):
                server.shutdown()
                server.server_close()
            for thread in threads:
                thread.join(timeout=5)


if __name__=='__main__':
    unittest.main()
