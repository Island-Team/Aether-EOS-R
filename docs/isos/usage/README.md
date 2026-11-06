# First use

This sequence is for an operator starting a local IsOS deployment. An **exact pin** names one package ID and version; a **graph revision** identifies saved graph bytes. IsOS verifies both before changing the active run. The selected **core** owns execution and reports readiness and stop.

1. Run `isos init --mode dev` to create a local development home.
   [Configure IsOS](configuration.md) explains the example defaults, local overrides, production activation, and how to repair startup errors.
2. Run `isos status --json` and `isos logs all --current` to inspect state before changing it.
3. [Install or link a package](packages.md). That page shows how to inspect a verified release, select a registered type, and reference stable resource IDs in the draft graph.
4. Validate the draft, Apply against the current revision, and [run the deployment](run.md). That page explains Run, Restart, Stop, and state checks; a missing owner or lifecycle adapter must be resolved before retrying.

Publishers use [Island-port sign-in](authentication.md) to authenticate publication through the CLI; it is separate from the local IsOS HTTP API.

The [saved instance contract](../interfaces/nodes/instance.md) explains graph fields; [local HTTP routes](../develop/http/README.md) explain server requests. Package-specific setup is documented by the owning core, not in IsOS.

[中文](../../../docs_cn/isos/usage/README_cn.md)
