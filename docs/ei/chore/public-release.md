# Public release procedure

[中文](../../../docs_cn/ei/chore/public-release.md) · [Release checklist](release.md) · [EOS compatibility](../../island/eos-contracts.md)

The first planned public version is v0.1.0, matching the Python package and CMake project version. Do not publish until every gate below passes. This development branch ports EGIA to EOS 2 commit c43e5a32eafd9bf22b2dba2940662dcded60111c. It is not an EOS public release tag, and no Aether-EGIA-R release or tag is created by the compatibility port.

## Compatibility gates

- Keep the EOS protobuf and action-schema pin exact and regenerate checked bindings from that source.
- Preserve EGIA safety admission, exact capability joins, durable operation IDs, terminal acknowledgement validation, explicit cancellation, and the no-retry rule for lost or OUTCOME_UNKNOWN results.
- Keep matching English and Chinese operation wire examples and the release checklist current.
- Pass the repository lint, type, import-boundary, schema, and full test suites.
- Run a real-process integration test against an exact, published EOS release. The current development pin alone is not release qualification.
- Verify Python 3.12 operation with the documented selectable interpreter, Conda, and uv environments; do not bind the package to one system Python installation.

## Snapshot release steps

After all gates pass, create the public release from a clean checkout and record the source commit:

1. Run the release checklist and confirm package, CMake, and documentation versions agree.
2. Build and inspect the source archive and wheel; ensure generated protobuf modules and EGIA-owned northbound schemas are present and EOS schemas are not duplicated.
3. Create the reviewed version commit and tag only after all compatibility evidence is recorded.
4. Rebuild from the tagged snapshot in a clean Python 3.12 environment and verify installation, configuration validation, and documented startup.

Do not publish an unqualified development snapshot to make the checklist appear complete.
