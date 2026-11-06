#!/usr/bin/env bash
set -euo pipefail

# Keep the remote identity check adjacent to publication. The build above can
# take long enough for master to move after the workflow's initial preflight.
source_commit="$(git rev-parse HEAD)"
tag_commit="$(git rev-parse "refs/tags/$ISOSUI_TAG^{commit}")"
if [[ "$tag_commit" != "$source_commit" ]]; then
  echo "Release tag $ISOSUI_TAG no longer identifies the prepared source" >&2
  exit 1
fi

remote_master="$(git ls-remote --exit-code origin refs/heads/master)"
if [[ "$remote_master" != "$source_commit"$'\t'refs/heads/master ]]; then
  echo "Remote master moved since $ISOSUI_TAG was prepared" >&2
  exit 1
fi

# Annotated tags expose both the tag object and its peeled commit; lightweight
# tags expose only the direct ref. Require the remote target, not the checkout's
# cached tag, to still identify the source that produced these archives.
remote_tag_refs="$(git ls-remote --exit-code origin "refs/tags/$ISOSUI_TAG" "refs/tags/$ISOSUI_TAG^{}")"
remote_tag_direct=''
remote_tag_peeled=''
while IFS=$'\t' read -r object ref; do
  case "$ref" in
    "refs/tags/$ISOSUI_TAG") remote_tag_direct="$object" ;;
    "refs/tags/$ISOSUI_TAG^{}") remote_tag_peeled="$object" ;;
  esac
done <<< "$remote_tag_refs"
remote_tag_commit="${remote_tag_peeled:-$remote_tag_direct}"
if [[ "$remote_tag_commit" != "$source_commit" ]]; then
  echo "Remote tag $ISOSUI_TAG moved since the archives were prepared" >&2
  exit 1
fi

if gh release view "$ISOSUI_TAG" >/dev/null 2>&1; then
  echo "Release $ISOSUI_TAG already exists" >&2
  exit 1
fi

gh release create "$ISOSUI_TAG" release/* --verify-tag --generate-notes
