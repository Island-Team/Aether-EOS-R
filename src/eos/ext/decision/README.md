# EOS external decision layer

`eos_decision` is an in-tree CMake library used to put a product-owned decision policy outside the EOS runtime. It talks only to the generated `SkillService` gRPC API; it does not link the skill manager, a robot backend, a deployment-specific adapter or the demo runtime. It is not currently installed as a standalone SDK or package.

The library is split into four deliberately narrow pieces:

- `types.h` contains normalized catalog, execution, journal, monitor, and action values.
- `client.h` implements the SkillService catalog and execution-management RPCs.
- `policy.h` is the injected decision-policy interface.
- `runner.h` follows one execution journal, remembers its in-process cursor across reconnects, and submits policy actions in order.

Policy code supplies every start and signal `request_id`; the library never generates one. Deterministic action output and stable IDs make a caller retry explicit and safe through EOS idempotency.

```cpp
auto channel = grpc::CreateChannel("127.0.0.1:16003",
                                   grpc::InsecureChannelCredentials());
eos::ext::decision::SkillClient client(channel);
MyDecisionPolicy policy;
eos::ext::decision::DecisionRunner runner(client, policy);
auto error = runner.Run(execution_id);
```

`SkillClient` applies a five-second deadline to unary calls by default. `DecisionRunner::Run` watches one execution and submits the vector returned by the policy in order. It reconnects after 250 ms by default for gRPC status codes `CANCELLED`, `UNKNOWN`, `DEADLINE_EXCEEDED`, `RESOURCE_EXHAUSTED`, `ABORTED`, `INTERNAL`, `UNAVAILABLE`, and `DATA_LOSS`. The cursor advances only after all actions for a journal entry succeed. If one fails, `Run` returns the error without advancing, so calling it again replays the entry and relies on deterministic policy output plus stable request IDs.

`Stop` cancels all active client RPCs and provides graceful shutdown. Cursors survive reconnects only within the current `DecisionRunner` process; the API does not claim durable journal delivery or automatic routing between skill executions.
