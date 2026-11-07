# Q03 browser benchmark fixture v1

This graph preserves the 19 nodes and 10 input wires from the selected Q01 clean-home `saved-graph.raw.json` at `logs/test/20261103-q01-full-current/live-3/` in EOS. Run-specific socket paths were replaced with `/tmp/q03-fixture/*`, and the robot identity is synthetic. No grant, prompt, or media is included.

`manifest.json` pins every response template by SHA-256 and byte length. The browser runner wraps IsOS bodies in the v1 API envelope and substitutes a unique command ID into the two EOS result templates. It records the actual response size and hash for each timed interaction. The synthetic `primitive_typed` request is accepted only by the isolated EOS stub on port 16020; the stub checks source path, target path, revision and movement type. The runner refuses to start if any required loopback port is occupied.

Run `python3 tests/fixtures/q03_browser_benchmark/v1/check.py` before using the fixture. The selected bundle should be built with Node 22/npm 10 and its manifest source commit checked before measurement.
