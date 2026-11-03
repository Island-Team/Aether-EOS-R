#!/usr/bin/env python3
"""Check relative Markdown links in docs/ and docs_cn/.

Pass changed Markdown paths, or use --all to scan both documentation trees.
Only local file links and GitHub-style heading fragments are checked.
"""

from __future__ import annotations

import argparse
import re
import sys
import unicodedata
from pathlib import Path
from urllib.parse import unquote, urlsplit


ROOT = Path(__file__).resolve().parents[1]
DOC_TREES = (ROOT / "docs", ROOT / "docs_cn")
FENCE = re.compile(r"^\s{0,3}(`{3,}|~{3,})")
HEADING = re.compile(r"^\s{0,3}#{1,6}\s+(.+?)\s*#*\s*$")
HTML_ANCHOR = re.compile(r'<(?:a\s+(?:id|name)|[^>]+\s+id)=["\']([^"\']+)["\']', re.I)


def visible_lines(content: str):
  """Return non-fenced lines with their original line numbers."""
  marker = ""
  width = 0
  for number, line in enumerate(content.splitlines(), 1):
    fence = FENCE.match(line)
    if fence:
      candidate = fence.group(1)
      if not marker:
        marker, width = candidate[0], len(candidate)
        continue
      if candidate[0] == marker and len(candidate) >= width:
        marker = ""
        continue
    if not marker:
      yield number, line


def slug(heading: str) -> str:
  heading = re.sub(r"<[^>]*>", "", heading)
  heading = re.sub(r"!?\[([^]]+)\]\([^)]*\)", r"\1", heading)
  heading = re.sub(r"[`*_~]", "", heading).lower().strip()
  heading = "".join(
      char for char in heading
      if char in " -_" or unicodedata.category(char)[0] in ("L", "N")
  )
  return heading.replace(" ", "-")


def anchors(path: Path) -> set[str]:
  found: set[str] = set()
  duplicates: dict[str, int] = {}
  for _, line in visible_lines(path.read_text(encoding="utf-8")):
    match = HEADING.match(line)
    if match:
      base = slug(match.group(1))
      count = duplicates.get(base, 0)
      found.add(f"{base}-{count}" if count else base)
      duplicates[base] = count + 1
    found.update(HTML_ANCHOR.findall(line))
  return found


def destinations(line: str):
  """Yield inline Markdown destinations, allowing balanced URL parentheses."""
  position = 0
  while position < len(line):
    start = line.find("](", position)
    if start < 0:
      return
    position = start + 2
    depth = 1
    escaped = False
    end = position
    while end < len(line):
      char = line[end]
      if escaped:
        escaped = False
      elif char == "\\":
        escaped = True
      elif char == "(":
        depth += 1
      elif char == ")":
        depth -= 1
        if depth == 0:
          raw = line[position:end].strip()
          if raw.startswith("<"):
            raw = raw[1:raw.find(">", 1)] if ">" in raw else raw
          else:
            raw = re.split(r"\s+[\"']", raw, maxsplit=1)[0]
          yield raw
          position = end + 1
          break
      end += 1
    else:
      return


def check_file(path: Path, anchor_cache: dict[Path, set[str]]) -> list[str]:
  errors = []
  for number, line in visible_lines(path.read_text(encoding="utf-8")):
    for destination in destinations(line):
      if not destination or destination.startswith("/") or re.match(r"^[a-z][a-z0-9+.-]*:", destination, re.I):
        continue
      parts = urlsplit(destination)
      if parts.scheme or parts.netloc:
        continue
      target = path if not parts.path else (path.parent / unquote(parts.path)).resolve()
      label = f"{path.relative_to(ROOT)}:{number}: {destination}"
      if not target.exists():
        errors.append(f"{label}: missing target")
      elif target.is_file() and target.suffix.lower() == ".md" and parts.fragment:
        if target not in anchor_cache:
          anchor_cache[target] = anchors(target)
        fragment = unquote(parts.fragment)
        if fragment not in anchor_cache[target]:
          errors.append(f"{label}: missing anchor #{fragment}")
  return errors


def main() -> int:
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("paths", nargs="*", type=Path, help="Markdown files or directories to check")
  parser.add_argument("--all", action="store_true", help="check every Markdown file under docs/ and docs_cn/")
  args = parser.parse_args()
  if not args.all and not args.paths:
    parser.error("pass Markdown paths or --all")
  sources = list(DOC_TREES) if args.all else args.paths
  files: set[Path] = set()
  for source in sources:
    source = source.resolve()
    if not any(source == tree or source.is_relative_to(tree) for tree in DOC_TREES):
      parser.error(f"outside docs/ and docs_cn/: {source}")
    if not source.exists():
      parser.error(f"missing path: {source}")
    files.update(source.rglob("*.md") if source.is_dir() else [source])
  errors = []
  cache: dict[Path, set[str]] = {}
  for path in sorted(files):
    errors.extend(check_file(path, cache))
  for error in errors:
    print(error, file=sys.stderr)
  print(f"Checked {len(files)} Markdown files; {len(errors)} broken relative links.")
  return 1 if errors else 0


if __name__ == "__main__":
  sys.exit(main())
