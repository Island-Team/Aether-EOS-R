"""Regression coverage for bounded IsOS filtered-log cursor scans."""

import json
from pathlib import Path
import sys
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parent))

from q01_full_replay import correlated_logs
from verify_q01 import correlated_log_pages


class CorrelatedLogPagesTest(unittest.TestCase):
    def test_empty_page_advances_before_matching_event(self):
        run_id='run-1'
        revision='a'*64
        mission_id='mission-1'
        mission_command='egia-command'
        browser_command='ui-command'
        with TemporaryDirectory() as temporary:
            root=Path(temporary)
            home=root/'home'
            event_path=home/'data/logs/default/events.log'
            event_path.parent.mkdir(parents=True)
            event_path.write_text(json.dumps({'event_code':'egia_proposal_created',
                'run_id':run_id,'fields':{'mission_id':mission_id,
                'operation_id':mission_command}})+'\n')
            (root/'browser-dom-observation.json').write_text(json.dumps({
                'command_id':browser_command,'active_revision':revision}))
            calls=[]

            def response(_port,path):
                from urllib.parse import parse_qs, urlsplit
                query=parse_qs(urlsplit(path).query)
                command=query['correlation_id'][0]
                cursor=query.get('cursor',[None])[0]
                calls.append((command,cursor))
                # Each request advances over at most one mocked 64 KiB scan.
                assert cursor in (None,'lc1.first','lc1.second')
                following={None:'lc1.first','lc1.first':'lc1.second',
                           'lc1.second':'lc1.second'}[cursor]
                lines=([{'correlation_id':command,
                         'event_code':'egia_proposal_created' if command==mission_command
                                      else 'bridge_primitive_result',
                         'fields':{'operation_id':command}}]
                       if cursor=='lc1.first' else [])
                return {'scope':{'kind':'deployment_run','run_id':run_id,
                                 'active_revision':revision},
                        'anchor':{'state':'found'},'lines':lines,
                        'next_cursor':following,'diagnostics':[],'rotated':False}

            with patch('q01_full_replay.api',side_effect=response):
                correlated_logs(root,home,1234,{'terminal':{'mission_id':mission_id}},run_id)
            self.assertEqual(len(calls),6)
            self.assertEqual(json.loads((root/'q01-log-correlation-gaps.json').read_text()),[])
            for label,command in (('mission',mission_command),('browser',browser_command)):
                pages=correlated_log_pages(root,label,command,run_id,revision)
                self.assertEqual([len(page['lines']) for page in pages],[0,1,0])
                self.assertEqual(json.loads((root/f'q01-logs-{label}-correlated.json').read_text())['lines'],[])
            browser_index=root/'q01-logs-browser-correlated-pages.json'
            altered=json.loads(browser_index.read_text())
            altered[1]['request_cursor']='lc1.wrong'
            browser_index.write_text(json.dumps(altered))
            with self.assertRaises(AssertionError):
                correlated_log_pages(root,'browser',browser_command,run_id,revision)


if __name__=='__main__':
    unittest.main()
