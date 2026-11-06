# Control-grant boundary

[中文](../../../../docs_cn/isos/interfaces/cores/control-grants_cn.md) · [Design](../../design/server.md)

The implemented IsOS grant bundle consists of private per-run files. The two control grant files are random, distinct secrets delivered only through reserved owner environment paths. The private `verifier.json` has the exact fields below. IsOS checks owner-only directory/file permissions, no symlink ancestors, the selected run and revision, and grant hashes. Stop and replacement revoke the files. EOS owns actuator command admission; a package contribution cannot create another route.

| `verifier.json` field | Type / unit | Purpose and validation | Absence/default |
| --- | --- | --- | --- |
| `schema_version` | integer / version | Current private verifier value `1`. | Required. |
| `run_id` | string / run identity | Exact bundle generation. | Required. |
| `active_revision` | lowercase 64-hex / SHA-256 | Binds saved graph bytes. | Required. |
| `egia_control_sha256` | lowercase 64-hex / grant bytes | Digest of first private grant. | Required. |
| `operator_control_sha256` | lowercase 64-hex / grant bytes | Digest of second private grant. | Required. |

Recipient/capability/expiry fields are **not** in this implemented verifier. A redesigned `@v1` grant envelope would require an owner migration and separate contract review; it is not active.

## Delivery paths and failure codes

`EOS_CONTROL_GRANTS_FILE`, `EGIA_EOS_CONTROL_GRANT_FILE`, and `AETHER_BRIDGE_CONTROL_GRANT_FILE` are reserved private delivery environment keys; graph arguments cannot override them. The selected owner's `managed-control-grants` capability is checked before issuance. Stable failures are `control_grant_unsafe_path`, `control_grant_invalid_file`, `control_grant_mismatch`, `control_grant_issue_failed`, `control_grant_delivery_failed`, and `control_grant_reserved_arg`. They cover permissions/path, file contents, run or digest mismatch, issuance, delivery, and reserved-argument collision respectively. Each failure closes authority before spawn or replacement.
