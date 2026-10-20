# Native skill runtime

This directory implements the reviewed in-process native profile:

```text
native/
├── runtime.h        copied metadata and developer-facing runtime API
├── detail.h         loader-owned process-lifetime state
├── loader.cpp       regular-file discovery, ABI and schema admission
├── registry.cpp     immutable exact-identity lookup
├── executor.cpp     invocation-scoped host callbacks and result validation
└── tests/           ABI fixtures and runtime boundary tests
```

Developers author each public-ABI skill as a separate source directory and CMake `MODULE` target linked only to `eos_skill_api`. The installed public contract is `<eos/skill/abi.h>`; runtime C++ headers are private and must not be included by such a skill. Product skills that intentionally use private EOS interfaces live separately under `share/skills/native/`.

EOS loads regular, non-symlink `*.skill.so` files once before opening the gateway. It requires the ABI v2 query symbol and rejects legacy-only modules. It copies and validates metadata, compiles schemas, resolves exact primitive identities and claim templates, and keeps admitted libraries mapped for process lifetime. The bounded `SkillManager` worker pool owns native execution. An invocation may have up to 64 asynchronous primitive operations outstanding; EOS requests cancellation when needed and drains all operations before the invocation returns.

The host exposes only declared primitive calls, allowlisted resource reads, cooperative cancellation, and bounded receive and validated emit callbacks for declared SIGNALs. Resource reads use the normal perception Reader; a concrete URI must match the complete template segment by segment, with each `{name}` replacing exactly one non-empty segment. Every physical effect still crosses Dispatcher validation and gets a derived command ID.

The executor accepts only canonical terminal primitive combinations: accepted calls may finish as `succeeded`, `failed`, `cancelled`, `timed_out`, or `outcome_unknown`; non-admitted calls may finish as `rejected` or `cancelled`. Missing, legacy, or contradictory terminal results are protocol failures and latch further primitive release for that invocation.

This is capability isolation, not process isolation: a native library can still block or corrupt `eosd`, so only reviewed deployment-owned code belongs in this profile.

See [`docs/eos/runtime/skills/native.md`](../../../../../docs/eos/design/runtime/skills/native.md) for the complete runtime contract.
