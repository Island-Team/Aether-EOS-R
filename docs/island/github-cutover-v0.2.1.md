# GitHub cutover checklist for unified IsOS

**Blocked until the private integration, disclosure review, legacy mirrors, public snapshot, Island-web source pin, and clean-home replay are complete. Do not create `v0.2.1` or publish its product release during this cutover.**

1. Preserve all original repository heads and tags in verified bundles, mirror old public IsOS and IsosUI v0.1.0 assets and checksums as clearly labeled legacy preservation releases, and verify the historical public EOS 0.2.0 release remains unchanged.
2. Prepare an independent-history public branch from `Island-Team/Aether-EOS-R` and compare every tracked path, mode, and SHA-256 against the reviewed private tree with `scripts/check_public_snapshot.py`. Review source and history for credentials, device/site identities, captures, local endpoints, and private skill content. Record every exclusion and public overlay; preserve public license and contributor attribution. Publish the reviewed four-component source snapshot, record its exact commit, and activate Island-web's hosted UI build from that immutable commit. Verify the asset digest and `/en/isos`, `/cn/isos`, `/isosui/` routes.
3. In GitHub web, rename the **old** public `Island-Team/isos` repository to a temporary legacy name. Record the new URL and confirm its v0.1.0 release and archive remain accessible.
4. Rename the private `Island-Team/Island-eos` repository to `Island-Team/isos-p`; keep its visibility private. Set description: “Private IsOS development history for the unified EI, EOS, manager, and workbench source.”
5. Rename the public `Island-Team/Aether-EOS-R` repository to `Island-Team/isos`; keep its visibility public. Set description: “IsOS: one Island OS core combining EI embodied intelligence, EOS embodied execution, and the IsosUI workbench. MuJoCo simulation is supplied by an external verified package.”
6. Verify both final URLs, public source commit, historical EOS 0.2.0 release, mirrored legacy assets and SHA-256 checksums, and Island-web pin and routes. Update remotes and references to the final URLs.
7. Only after those receipts pass, delete the retired old IsOS, IsosUI, and EGIA GitHub repositories and remove their local checkout folders. Before local removal, copy ignored evidence needed for review; the current EOS logs tree is large and requires an explicit retention decision. Leave the external MuJoCo repository and package untouched.

The release draft remains a draft. A later, separate release decision may create a v0.2.1 tag and product release after all product gates pass.
