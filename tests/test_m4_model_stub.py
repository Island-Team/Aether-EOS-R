"""Focused wire and bounded-log checks for the M4 model stub."""

import json
from urllib.error import HTTPError
from urllib.request import Request, urlopen
import unittest

from m4_model_stub import FINAL, MOVE, PLAN, PROPOSER_EMPTY, INVALID_Q01_REPLY, ModelStub


def complete(endpoint: str, system: str, *, stream: bool = False) -> dict:
    request = Request(endpoint + '/chat/completions',
                      data=json.dumps({'model': 'm4-test',
                                       'messages': [{'role': 'system', 'content': system}],
                                       'stream': stream}).encode(),
                      headers={'Content-Type': 'application/json',
                               'Authorization': 'Bearer must-not-be-logged'})
    with urlopen(request, timeout=2) as response:
        return json.load(response)


class ModelStubTest(unittest.TestCase):
    def test_controller_sentinel_and_bounded_log(self) -> None:
        with ModelStub(max_requests=4) as stub:
            assert complete(stub.endpoint, 'controller', stream=True)['choices'][0]['message']['content'] == PLAN
            assert complete(stub.endpoint, 'You are a fast safety sentinel')['choices'][0]['message']['content'] == 'continue'
            assert complete(stub.endpoint, 'controller')['choices'][0]['message']['content'] == MOVE
            final = complete(stub.endpoint, 'controller')
            self.assertEqual(final['choices'][0]['message']['content'], FINAL)
            self.assertEqual(final['choices'][0]['finish_reason'], 'stop')
            self.assertEqual(final['usage']['total_tokens'], 20)
            with self.assertRaises(HTTPError) as error:
                complete(stub.endpoint, 'controller')
            self.assertEqual(error.exception.code, 429)
            requests = stub.snapshot()
        self.assertEqual([r['lane'] for r in requests],
                         ['controller', 'sentinel', 'controller', 'controller'])
        self.assertNotIn('must-not-be-logged', json.dumps(requests))
        self.assertNotIn('You are a fast safety sentinel', json.dumps(requests))

    def test_q01_invalid_controller_fault_and_repair(self) -> None:
        with ModelStub(max_requests=8) as stub:
            self.assertEqual(complete(stub.endpoint, 'controller')['choices'][0]['message']['content'], PLAN)
            stub.set_mode('invalid')
            for _ in range(3):
                self.assertEqual(complete(stub.endpoint, 'controller')['choices'][0]['message']['content'], INVALID_Q01_REPLY)
            self.assertEqual(complete(stub.endpoint, 'fast safety sentinel')['choices'][0]['message']['content'], 'continue')
            stub.set_mode('normal')
            self.assertEqual(complete(stub.endpoint, 'controller')['choices'][0]['message']['content'], PLAN)
            requests, transitions = stub.snapshot(), stub.snapshot_transitions()
        self.assertEqual([item['mode'] for item in transitions], ['normal', 'invalid', 'normal'])
        self.assertEqual([item['reply'] for item in requests if item['mode'] == 'invalid' and item['lane'] == 'controller'],
                         [INVALID_Q01_REPLY] * 3)
        self.assertNotIn('fast safety sentinel', json.dumps(requests))

    def test_proposer_does_not_consume_repaired_controller_plan(self) -> None:
        with ModelStub(max_requests=5) as stub:
            stub.set_mode('invalid')
            self.assertEqual(complete(stub.endpoint, 'controller')['choices'][0]['message']['content'],
                             INVALID_Q01_REPLY)
            stub.set_mode('normal')
            proposer = complete(stub.endpoint, 'You propose grounded missions for Mewna')
            self.assertEqual(proposer['choices'][0]['message']['content'], PROPOSER_EMPTY)
            self.assertEqual(complete(stub.endpoint, 'controller')['choices'][0]['message']['content'], PLAN)
            self.assertEqual(complete(stub.endpoint, 'controller')['choices'][0]['message']['content'], MOVE)
            self.assertEqual(complete(stub.endpoint, 'controller')['choices'][0]['message']['content'], FINAL)
            requests = stub.snapshot()
        self.assertEqual([item['lane'] for item in requests],
                         ['controller', 'proposer', 'controller', 'controller', 'controller'])
        self.assertEqual([item['reply'] for item in requests[2:]],
                         [PLAN, 'move', FINAL])


if __name__ == '__main__':
    unittest.main()
