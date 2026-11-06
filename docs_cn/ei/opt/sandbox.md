# egia/opt/sandbox — local device playground（中文）

> 本文是 [egia/opt/sandbox — local device playground](sandbox.md) 的中文伴随文档。代码、命令、路径、接口名称和字段名保持与英文原文一致，以便直接用于开发与运维。

## 中文说明

本页覆盖 **egia/opt/sandbox — local device playground** 的当前实现、边界和使用约定。请结合英文原文阅读完整的命令示例、配置键、协议字段与代码片段；任何与实现不一致时，以仓库中的代码和规范契约为准。

## 原文

以下内容保留原文，便于逐段对照：

<details>
<summary>展开英文原文</summary>

# egia/opt/sandbox — local device playground

`egia.opt.sandbox` is an optional, out-of-process EOS-compatible backend for the host's default camera, microphone, and speaker. It is a temporary local playground while a configured real fleet is unavailable; it is not a simulator and implements no mirror, reinforcement-learning, or synthetic world behavior.

## Activation and failover

The canonical process switches live in `etc/egia/egia.toml`:

```toml
[launch]
backend = true
memory = true
sandbox = true
```

`backend=false, sandbox=false` starts a ready-empty northbound service and enabled non-model modules only. Sandbox alone starts directly in the local playground. Backend alone waits indefinitely for every mandatory fleet endpoint. With both enabled, EGIA waits `fallback_timeout_s` (five seconds by default), then uses the playground while continuing whole-fleet probes.

When every real endpoint discovers successfully, EGIA pauses the playground, retains pending/running missions and completed barriers, increments the backend session revision, discards unstarted playground horizons, and replans against the real fleet. This promotion is one-way for the process lifetime. A later real-backend heartbeat failure immediately pauses model lanes and new planner barriers; all endpoints must rediscover resources and primitives before work resumes. EGIA never falls back to the playground after promotion.

The child has a readiness handshake, uses the repository-wide structured logger and its normal `egia.opt.sandbox` route at `var/log/egia/opt/sandbox.log`, restarts with bounded backoff, and is terminated and reaped on shutdown. The owning EGIA process remains the sole replay-log writer. Sandbox failure is non-fatal; execution remains paused until either it restarts or the real fleet becomes ready.

Legacy `[memory].enabled` and `[sandbox].enabled` keys are rejected. Move them to `[launch]`.

## Device configuration

Detailed settings are independent of activation and live in `etc/egia/opt/sandbox.toml` (with matching factory defaults). The default EOS gRPC listener is `127.0.0.1:16022`; explicit overrides remain valid. The child carries media over a Unix-domain socket below the runtime root; the main EGIA listener exposes it to clients as `ws://127.0.0.1:16060/v1/sandbox`. No additional sandbox TCP port is opened.

Linux automatic selection prefers PipeWire, then PulseAudio/ALSA for audio; the camera uses GStreamer `autovideosrc`. Per-device `backend` and `device` overrides support hosts where automatic selection is unsuitable. Configuration also bounds recording duration and PCM snapshot size, camera dimensions/JPEG quality/frame count/rate, and speaker WAV bytes/duration. No Python media library is required.

## EOS surface

The child implements the existing `GatewayService` without modifying `gateway.proto`. Discovery advertises every enabled `microphone`, `camera`, and `speaker` device, including unavailable hardware with `alive=false` and `admitted=false`; each descriptor carries the class-specific configuration and operational state. These body values are discovery metadata, not readable resources. Usable cameras provide bounded JPEG capture through `CaptureFrames`; the playground publishes no semantic world resources and provides no speech recognition, synthesis, or `speaker.play` primitive. Its media WebSocket still supports explicit raw WAV capture and playback for local device diagnostics.

The playground is an EOS-compatible backend only. It does not add an Agent-local execution command. Backend selection and failover are launch concerns rather than model actions.

## WebSocket media protocol

Every JSON command contains correlated `id` and `op` fields and receives `accepted`, `complete`, or `error` events:

- `status` returns camera, microphone, and speaker identity, configured backend/device selection, availability, and current capture/playback state.
- `mic.start` begins recording; `mic.stop` returns a media metadata event, followed by one binary WAV frame, then `complete`.
- `mic.cancel` stops and discards the connection's recording, then returns `complete` without a binary frame.
- `camera.capture` accepts bounded `count` and `interval_ms`; each ordered result is a metadata event followed by one binary JPEG frame.
- `speaker.start` claims the uploader; binary messages append WAV chunks and matching `speaker.finish` validates and plays the completed file.

Only one microphone recording and one speaker upload may be active. Conflicts return `busy`, malformed/bounded media returns structured errors, and a client disconnect releases work it owns.

## Boundary

The optional package imports contracts and leaf utilities only; it never imports `egia.core`. The composition root presents it to core as the temporary backend alias `sandbox`, separate from all configured fleet aliases.

Each monitor heartbeat also reads runtime status. Device availability updates the backend ledger, the agent's always-present backend context section, and the northbound `/backend` frame consumed by the TUI.

</details>
