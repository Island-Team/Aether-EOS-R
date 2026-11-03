# Hello package example

This tiny package demonstrates archive creation, digest verification, and the
`island-pkg install` and `island-pkg activate` commands without MuJoCo or a
compiler. It installs a shell greeting at `bin/island-hello`. Activation reports
the package path; it does not generate EOS configuration or start a simulator.

```sh
python3 examples/packages/hello/build-release.py \
  --version 0.1.0 \
  --source-url https://github.com/YOUR_ORG/island-hello \
  --output /tmp/island-hello-release
```

Replace the placeholder with a publicly readable GitHub repository containing
the example source. The command writes a `.tar.gz` and a **draft** manifest
containing the measured archive size and SHA-256 digest. The GitHub URL is the
expected location for a future release; the script does not verify repository
visibility, upload, or publish anything. Reviewers must check public access to
the source and uploaded asset before approval. The greeting can
be run directly after installation from the installed package's `bin` directory.

This is an **install-only teaching example**, not an EOS simulator. Its package
kind is `example`, its EOS contract is empty, and it has no activation templates.
The v1 client can install and list it; `activate` reports its installed path.
