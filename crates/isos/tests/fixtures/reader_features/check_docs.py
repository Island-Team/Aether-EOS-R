#!/usr/bin/env python3
"""Check P05 bilingual links and shared contract identifiers."""

import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
PAIRS = [
    ("docs/isos/interfaces/packages/verification.md", "docs_cn/isos/interfaces/packages/verification_cn.md"),
    ("docs/isos/interfaces/nodes/instance.md", "docs_cn/isos/interfaces/nodes/instance_cn.md"),
    ("docs/isos/develop/http/README.md", "docs_cn/isos/develop/http/README_cn.md"),
    ("docs/isos/design/server.md", "docs_cn/isos/design/server_cn.md"),
    ("docs/isos/usage/README.md", "docs_cn/isos/usage/README_cn.md"),
    ("tests/fixtures/reader_features/README.md", "tests/fixtures/reader_features/README_cn.md"),
]
SHARED = [
    "supported_graph_versions", "supported_node_type_versions", "supported_node_types",
    "package-process-v1", "reader_identity_invalid", "reader_graph_unsupported",
    "reader_type_unsupported", "reader_feature_unsupported", "reader_binary_mismatch",
    "reader_package_pin_mismatch", "compatible_upgrade",
    "managed-config-v1", "eos-gateway-client-v2", "uv sync", "/settings/role",
]


def main() -> None:
    for en, cn in PAIRS:
        for name in (en, cn):
            path = ROOT / name
            text = path.read_text()
            for target in re.findall(r"\[[^]]+\]\(([^)]+)\)", text):
                if target.startswith(("http://", "https://", "/")):
                    continue
                destination = path.parent / target.split("#", 1)[0]
                assert destination.exists(), (name, target)
    en = (ROOT / PAIRS[0][0]).read_text()
    cn = (ROOT / PAIRS[0][1]).read_text()
    for token in SHARED:
        assert token in en and token in cn, token
    examples = []
    for text in (en, cn):
        payloads = [json.loads(raw) for raw in re.findall(r"`(\{[^`]+\})`", text) if raw.startswith('{"diagnostics"')]
        assert len(payloads) == 1
        examples.append(payloads[0])
    assert examples[0] == examples[1]
    diagnostic = examples[0]["diagnostics"][0]
    assert diagnostic["code"] == "reader_feature_unsupported"
    assert diagnostic["reader"]["requested"]["feature"] == "vision-gate"
    print("P05 bilingual links and shared identifiers valid")


if __name__ == "__main__":
    main()
