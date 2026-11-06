#!/usr/bin/env python3
"""Validate R03 IsOS handoff design fixtures and bilingual links."""

import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
PAIRS = [
    ("docs/isos/interfaces/cores/control-grants.md", "docs_cn/isos/interfaces/cores/control-grants_cn.md"),
    ("docs/isos/design/server.md", "docs_cn/isos/design/server_cn.md"),
    ("tests/fixtures/control_grants/README.md", "tests/fixtures/control_grants/README_cn.md"),
]
SHARED = (
    "EOS_CONTROL_GRANTS_FILE",
    "EGIA_EOS_CONTROL_GRANT_FILE",
    "AETHER_BRIDGE_CONTROL_GRANT_FILE",
    "egia_control_sha256",
    "operator_control_sha256",
    "control_grant_unsafe_path",
    "control_grant_invalid_file",
    "control_grant_mismatch",
    "control_grant_issue_failed",
    "control_grant_delivery_failed",
    "control_grant_reserved_arg",
    "managed-control-grants",
)


def main() -> None:
    cases = json.loads((HERE / "cases.json").read_text())
    assert len(cases) == 26
    assert [case["id"] for case in cases] == [f"R03-I{i:02}" for i in range(1, 27)]
    assert all(set(case) == {"id", "operation", "mutation", "expected"} for case in cases)
    assert len({case["mutation"] for case in cases}) == len(cases)
    for english, chinese in PAIRS:
        texts = [(ROOT / name).read_text() for name in (english, chinese)]
        for name, content in zip((english, chinese), texts):
            for link in re.findall(r"\[[^]]+\]\(([^)]+)\)", content):
                if link.startswith(("http://", "https://")):
                    continue
                target = (ROOT / name).parent / link.split("#", 1)[0]
                assert target.exists(), (name, link)
        if english.endswith("cores/control-grants.md"):
            for token in SHARED:
                assert all(token in text for text in texts), token
    print("R03 IsOS control-grant cases and bilingual links valid")


if __name__ == "__main__":
    main()
