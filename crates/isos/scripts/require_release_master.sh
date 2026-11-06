#!/usr/bin/env bash
set -euo pipefail

expected="${1:?selected master SHA required}"
[[ "$expected" =~ ^[0-9a-f]{40}$ ]] || { echo 'Invalid selected master SHA' >&2; exit 1; }
remote="$(git ls-remote origin refs/heads/master)"
[[ "$remote" == "$expected"$'\t'refs/heads/master ]] || {
  echo 'Remote master moved since publish dispatch' >&2
  exit 1
}
