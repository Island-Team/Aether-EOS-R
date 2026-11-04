# Publish EOS source

<!-- docs-tree:start -->
**Location:** [Docs](../../README.md) / [EOS](../README.md) / [Maintenance](README.md) / **publish**

<details>
<summary>Docs tree</summary>

- [docs/](../../README.md)
  - [eos/](../README.md)
    - [maintenance/](README.md)
      - **publish.md**
      - [release.md](release.md)

</details>
<!-- docs-tree:end -->

[中文](../../../docs_cn/eos/maintenance/publish_cn.md) · [Release checks](release.md)

Develop and review changes in the private `Island-eos` repository. The public `Aether-EOS-R` repository has an independent history: create a new public branch from its current target branch and copy only the reviewed source snapshot intended for release. Do not push, merge, cherry-pick, or mirror private commits or tags into the public repository.

Before opening the public PR, compare the snapshot with the private tree and perform the [release checks](release.md). Confirm that deployment configuration, credentials, device identities, and ignored runtime state are absent. Build and test from the public snapshot, review the complete public diff, and verify documentation links.

Merge the public PR after review and passing checks. Create a public tag from the merged public commit only; the product version comes from the root `CMakeLists.txt`. Keep primitive, schema, and skill ABI versions independent of the product release number.

## Prepare the 0.2.0 source candidate

Use a reviewed private EOS commit and its exact tree as the input. Start a new branch from the current target of `Aether-EOS-R`; copy the reviewed tracked source while omitting the private-only optional skill subtree named in the private release receipt. Remove tracked files left over from the previous public tree when absent from the selected private tree. The public repository keeps its own commits and tags. Keep the private commit ID, public base ID, selected tree ID, and review decisions in the release receipt outside both source trees.

Stage the complete copied public tree with `git add -A` before comparing; the checker rejects untracked, nonignored files. Run `python3 scripts/check_public_snapshot.py "$EOS_PRIVATE" "$EOS_PUBLIC" --output "$RELEASE_LOG/eos-tracked-manifest.json"` after the copy. The receipt lists every tracked path, Git mode, and SHA-256 of its current bytes in both checkouts; the command fails for every unreviewed path, mode, or byte difference. Pass only the reviewed exceptions listed below; the receipt records every omitted and overlaid path with both available digests. Set `RELEASE_LOG` to a directory on a filesystem with enough free space, outside both checkouts. This comparison covers the snapshot contents, not provenance or disclosure safety, so complete the [release review](release.md) before opening the public PR.

The private release policy excludes an optional site-specific skill subtree while retaining its callable ABI in private source. In the public overlay, replace the named backend ignore rule in `.gitignore` with `/etc/eos/backends/*.toml`, remove the optional skill reference from `share/eos/skills/CMakeLists.txt`, and replace private-specific instructions in the English and Chinese publish guides. Run the checker with the exclusion prefix and four overlay paths recorded in the private release receipt. Its receipt must list exact omitted and overlaid paths and hashes; the clean public build and link check must pass.

The public PR review covers the complete diff from the public target, including removed files. Build and test from a clean public checkout. After reviewed merge, verify the remote public target and create `0.2.0` only from that merged public commit. Record the public commit and tag object IDs in the release receipt. The private `refactor(v2)/runtime-main` integration history is not copied into the public repository.

The reviewed public snapshot was merged at `a4b66d3dff75d0d4b738a265b160bcd75bb7f34a` and published as annotated tag `0.2.0`, which peels to the same commit. The public history remains independent of private `Island-eos`; see the [release evidence](release.md#published-020-evidence--2026-11-04).
