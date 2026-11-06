# U04 implementation design and evidence

[中文](../../docs_cn/isosui/design-primitive-control_cn.md) · [Owner interface](interfaces/primitive-control.md)

The R07 result client remains the only browser command path. U04 generalizes its one mock base card to a card per exact active `@v0.1` base/manipulator binding. The pure binding resolver verifies one target and its declared mode before rendering. The component rechecks IsOS graph/run identity around EOS time and submission and uses the existing correlated result state machine. `Failed` is an owner terminal result, while browser transport loss is unavailable. Stop still calls IsOS independently and fences every card for the current run before awaiting its response, aborts sibling result reads and discards late replies. No legacy graph-authored skill sequence is exposed.

Graph nodes render their information card in a portal, keeping the card above overlapping React Flow nodes. Hover/focus/touch info opens it; Escape and close dismiss. The card is nonmodal, clamped on opening and resize, and does not claim live health when owner reads are unavailable. Port units come from the node catalog data schema. The portal's info controls use `nodrag` and `nowheel`. A visible open card polls EOS every two seconds and labels failed or five-second-old samples stale.

Verification: `npm ci --no-audit --no-fund`; `npm test` (144 passing tests, including exact base/arm binding, ambiguity, stale run, result follow-up, Stop failure and viewport clamp); `npm run typecheck`; `npm run build`; `git diff --check`. Complete logs are local in `logs/test/20261102-u04-primitive-control/`. The Node 22.23.4/npm 10.9.9 environment is the supported toolchain. Browser visual QA and M5 clean-home replay remain separate qualification gates.

## Workbench refresh qualification fix

The selected owner catalog may take longer than the five-second workbench interval. Previously every tick invalidated the in-flight response, so no graph, status or control could appear. The workbench now permits one refresh at a time and ignores timer ticks during that flight. A manual Refresh immediately closes Invoke, invalidates the older flight and coalesces repeated clicks into one follow-up read. The follow-up publishes only matching graph/catalog/status and its EOS discovery; failure and revision mismatch stay read-only. A new revision or run clears old discovery/health, and the hardware poll is restarted so late old-run samples cannot refill it. A mount generation and latest callback refs discard replies after unmount without restarting on parent callback changes.

Node 22.23.4/npm 10.9.9 verification: `npm ci --no-audit --no-fund`; `npm test` (39 files, 179 tests, including eight focused slow-timer, manual queue, mismatch, new-run health, unmount/discovery, callback identity and StrictMode cases); `npm run build` (typecheck, local and hosted bundles and manifests); `git diff --check`. The complete test and build output plus independent design review are in ignored `logs/test/20261102-workbench-refresh/`. The final browser replay is an M4 qualification gate.

## Q01 command deadline order

A selected first-use run exposed a 13.9-second IsOS process-status read between the browser's EOS monotonic time sample and its four-second typed-command deadline. The request reached admission after expiry and the UI correctly showed it as unavailable. The sender now completes its final IsOS status fence before sampling EOS time, then immediately checks the latest local run/revision, binding, edit, Stop and abort state before one send. A late local change cancels the send; an expired or uncertain command ID is never retried. The owner-facing contract and mounted deferred-read tests cover this order.
