#!/usr/bin/env python3
"""Exercise the documentation link checker with an isolated doc tree."""

import subprocess
import sys
import tempfile
from pathlib import Path


repo = Path(__file__).resolve().parents[2]
tool = repo / "scripts/check_doc_links.py"


def check(*paths: Path) -> subprocess.CompletedProcess[str]:
  return subprocess.run(
      [sys.executable, str(tool), *(str(path) for path in paths)],
      capture_output=True,
      text=True,
      check=False,
  )


with tempfile.TemporaryDirectory(prefix="link-test-", dir=repo / "docs") as directory:
  root = Path(directory)
  target = root / "target page.md"
  target.write_text("# Hello World\n## Hello World\n## 中文标题\n", encoding="utf-8")
  asset = root / "image.png"
  asset.write_bytes(b"image")
  source = root / "source.md"
  source.write_text(
      "[first](<target page.md#hello-world>)\n"
      "[second](<target page.md#hello-world-1>)\n"
      "[unicode](<target page.md#中文标题>)\n"
      "![image](image.png)\n"
      "[external](https://example.com/missing.md)\n"
      "```md\n[ignored](missing-in-fence.md)\n```\n",
      encoding="utf-8",
  )
  valid = check(source)
  assert valid.returncode == 0, valid.stderr
  source.write_text(
      source.read_text(encoding="utf-8")
      + "[broken](missing.md)\n"
      + "[anchor](<target page.md#no-such-heading>)\n",
      encoding="utf-8",
  )
  broken = check(source)
  assert broken.returncode == 1
  assert "missing.md: missing target" in broken.stderr
  assert "missing anchor #no-such-heading" in broken.stderr
  assert "missing-in-fence" not in broken.stderr
