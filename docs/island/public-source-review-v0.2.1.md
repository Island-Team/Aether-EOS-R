# Public source review for the 0.2.1 candidate

**Candidate only.** This public tree is a one-commit snapshot of private source commit `7c2eb8b8` above the existing public EOS `0.2.0` history. It is not a product release and creates no `v0.2.1` tag.

The snapshot review compared every tracked file with the private source: five site-specific example files are excluded, and 42 files have explicit public edits. Those edits replace machine home paths in 32 historical English/Chinese documents, sanitize one prompt example and its test, remove the excluded example from the optional CMake target, and adjust public documentation and ignore rules. The archived private source and redaction receipt preserve the exact originals and per-file hashes. Generic interfaces and loopback-only examples remain available; real hardware setup and debug evidence stays private.

The independent disclosure pass found no tracked credentials, real hardware endpoints, calibration data, or captures. The Q03 browser fixture rejects any `/home/<user>/` path in response data. Relative-link validation checked 640 Markdown files with zero broken links; Q03 and P07 fixture checks passed. The source tree still needs final public build and branch review before publication.

The original source repositories and imported commits are listed in [source provenance](source-provenance-v0.2.1.md). Historical private repository links are retained only as dated evidence; current guidance links to files in this tree.
