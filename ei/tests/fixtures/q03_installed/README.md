# Q03 installed setup fixture

`stages.json` is the byte-exact EGIA stage overlay from the selected Q01
archive `20261103-q01-full-current/live-3`, graph revision
`1964bdae4a0291b17306f9861bc971e158cebac59a115a946658e94fa6a88a89`.
Its SHA-256 is
`1e239085ec48ac988cdc30732e7843141a184ecfd60ca6e3b49459a971a2a702`.
The CI probe derives a minimal graph with the same EGIA core and runs the
**installed** `egia-config setup` catalog, validate and render operations.
It requires exact overlay bytes, stage IDs, route count and prefix-contained
imports/resources. This fixture does not replace the selected Q03 live
IsOS/EOS/MuJoCo/EGIA process replay.
