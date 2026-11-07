# Q03 EOS source-layout fixture

This test-only archive contains `share/`, the executable `bin/eosd` launcher,
and `adapters/eos-cxx/CMakeLists.txt` from Island-Team/Island-eos commit
`c43e5a32eafd9bf22b2dba2940662dcded60111c`, the exact EGIA contract pin
in `scripts/generate_eos_proto.py`. CI checks `SHA256SUMS` before extracting the
archive beside the EGIA checkout. The source tests read EOS contracts from this
sibling tree; `islandctl validate` also requires the launcher to exist and be
executable. The CMake file preserves the source-checkout marker used by the
launcher environment test. The launcher is never started by the Q03 owner job.
The fixture is not part of the EGIA wheel.

To regenerate from an authorized EOS checkout and review the update:

```sh
git -C ../Island-eos archive c43e5a32eafd9bf22b2dba2940662dcded60111c share bin/eosd adapters/eos-cxx/CMakeLists.txt \
  | gzip -n -9 > tests/fixtures/q03_eos_share/eos-share-c43e5a32.tar.gz
sha256sum tests/fixtures/q03_eos_share/eos-share-c43e5a32.tar.gz \
  > tests/fixtures/q03_eos_share/SHA256SUMS
sha256sum -c tests/fixtures/q03_eos_share/SHA256SUMS
```

A contract-pin change must update this archive, its checksum, the protobuf
bindings, the typed contracts, and the compatibility documentation together.
