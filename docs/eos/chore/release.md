# Release 1.0.0 qualification

[中文](../../../docs_cn/eos/chore/release_cn.md)

The public release candidate uses the hardware-free `demo` profile in `etc/eos/eos.example.toml`. Site bindings and vendor adapters are deployment-owned. This page records qualification evidence; do not infer hardware support from an untested build.

## 1.0.0 changes

- This is the first public EOS source snapshot. The private deployment adapter, site profiles, and site workflows are excluded from this history. The public ALSA, aplay, and V4L2 backends remain available.
- Product, built-in skill, and backend descriptor versions are `1.0.0`. `observe_human@1.0.0` uses the newer lying-person selection; the former behavior is `observe_human_legacy@1.0.0`.
- Owner interaction now requires deployment-provided waypoint IDs and arm poses. `observe_human` requires an explicit gimbal reacquire tilt. Existing callers must supply those arguments after upgrading.
- The demo config uses loopback devices. Its face and speech model entries are endpoint examples; model services are not bundled.

## Release checks

- [x] Confirm the public tree contains no private configuration, credentials, captures, calibration, or site endpoints.
- [x] Build and run the full test suite from a standalone source copy without sibling repositories.
- [x] Verify version reporting, installation, documentation links, and English/Chinese mirrors.
- [x] Review the release source and record qualified platforms below.

## Qualification record

| Target | Build and tests | Hardware exercise | Evidence |
| --- | --- | --- | --- |
| Ubuntu 24.04.4 x86, RTX 2080 Ti | Debug build and 102/102 CTest; RelWithDebInfo build and staged `/usr` install; demo runtime start/stop | Hardware-free | Local release branch, 2026-10-20 |
| Jetson Orin, Ubuntu 22.04.5 ARM64 | Debug build and 102/102 CTest | No physical device exercise | Native ARM64 build, 2026-10-20 |
| ROS 2 Humble on Jetson Orin | RelWithDebInfo build with `EOS_BACKEND_ROS2`; ROS backend and `eosd` compiled | No ROS graph or physical device exercise | Native ARM64 build, 2026-10-20 |

Release tag: `v1.0.0`. Hardware support requires deployment-specific qualification.
