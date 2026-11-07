#!/usr/bin/env python3
"""Check the current bilingual IsOS documentation tree and local links."""

import json
import re
import sys
import urllib.parse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EN = ROOT / "docs/isos"
CN = ROOT / "docs_cn/isos"


def slug(heading: str) -> str:
    text = re.sub(r"<[^>]*>", "", heading.lower().strip())
    text = re.sub(r"[^\w\- ]", "", text)
    return re.sub(r" +", "-", text)


def main() -> int:
    errors = []
    english = {str(path.relative_to(EN)) for path in EN.rglob("*.md")}
    chinese = {
        str(path.relative_to(CN)).replace("_cn.md", ".md")
        for path in CN.rglob("*.md")
    }
    if english != chinese:
        errors.append(f"tree parity: EN-only={sorted(english - chinese)} CN-only={sorted(chinese - english)}")

    for relative in sorted(english & chinese):
        left = EN / relative
        right = CN / (relative[:-3] + "_cn.md")
        left_levels = [len(line.split(" ")[0]) for line in left.read_text().splitlines() if line.startswith("#")]
        right_levels = [len(line.split(" ")[0]) for line in right.read_text().splitlines() if line.startswith("#")]
        if left_levels != right_levels:
            errors.append(f"heading levels: {relative}")

    pages = [ROOT / "README.md", ROOT / "README_cn.md", ROOT / "AGENTS.md"]
    pages += list(EN.rglob("*.md")) + list(CN.rglob("*.md"))
    pages += list((ROOT / "tests/fixtures").rglob("*.md"))
    for page in pages:
        content = page.read_text()
        for url in re.findall(r"\[[^\]]+\]\(([^)]+)\)", content):
            if url.startswith(("http:", "https:", "mailto:")):
                continue
            name, _, fragment = url.partition("#")
            target = (page.parent / name).resolve() if name else page
            if not target.exists():
                errors.append(f"missing link: {page.relative_to(ROOT)} {url}")
                continue
            if fragment and target.suffix == ".md":
                headings = [
                    slug(line.lstrip("#").strip())
                    for line in target.read_text().splitlines()
                    if line.startswith("#")
                ]
                if urllib.parse.unquote(fragment).lower() not in headings:
                    errors.append(f"missing fragment: {page.relative_to(ROOT)} {url}")
        for index, raw in enumerate(re.findall(r"```json\s*\n(.*?)\n```", content, re.S), 1):
            try:
                json.loads(raw)
            except json.JSONDecodeError as exc:
                errors.append(f"invalid JSON: {page.relative_to(ROOT)} #{index}: {exc}")

    print(f"{len(english)} English / {len(chinese)} Chinese IsOS pages; {len(errors)} issues")
    for error in errors:
        print(error)
    return int(bool(errors))


if __name__ == "__main__":
    sys.exit(main())
