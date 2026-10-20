# `register_host`

## Version and purpose

`register_host@1.0.0` is a reentrant native skill (`island.register_host@1.0.0`) that registers exactly three hosts in order. Each host states their name before the skill stores the latest camera frame with `face.register`.

## Input contract

`robot_name`, `camera_device_id`, and `speaker_device_id` are required non-empty strings. `introduction_timeout_ms` optionally bounds each host's introduction wait from `1000` to `300000` milliseconds and defaults to `60000`. Additional properties are rejected. `host_ready_delay_ms` optionally adds `0` to `30000` milliseconds after each `请主人N就位` prompt and defaults to `0`. The host count is an implementation constant.

## Resources, primitives, and claims

- Reads `robots/{robot_name}/world/asr/state` for fresh recognized speech.
- Calls `speaker.speak@1.0.0` with Chinese prompts.
- Calls `face.register@1.0.0` with the name extracted from the introduction.
- Exclusively claims `/camera_device_id` and `/speaker_device_id` for the full run.

`face.register` sends the current validated JPEG directly to the configured registration model. It logs only frame metadata and does not persist an image attachment.

## Execution flow

1. Emit `registration.started` with stage count `3`.
2. For each host, speak `请主人N就位`, wait the configured preparation delay, and then speak `请自我介绍`.
3. Before the introduction prompt starts, record the latest ASR publication as the baseline. This preserves a host's answer even if they speak during the prompt.
4. Wait for newer ASR publications. Speech that does not contain a supported introduction is ignored, including recognition of the robot's prompt.
5. Extract names from common forms such as `我是张三`, `我叫张三`, `我的名字是张三`, or `My name is Alice`. Terminal punctuation is removed.
6. Speak `开始注册`, call `face.register` with the extracted name, then speak `注册完成`.
7. A host-specific prompt, introduction timeout, or face-registration failure records `registration.stage.failed` and advances to the next host.
8. After all three attempts, report whether all registrations succeeded and return the successfully registered names.

## Pause, cancellation, and failure

`skill.command` accepts `pause` and `resume`. Pause is cooperative at resource waits and primitive boundaries. Manager cancellation cancels and drains an active primitive where supported. Successful earlier registrations are not rolled back.

An introduction timeout or host-specific primitive failure skips that host without preventing the remaining attempts. Subscription failures use `asr.subscription_failed`, `asr.read_failed`, `asr.wait_failed`, or `asr.closed` and still terminate the skill because subsequent introductions cannot be read. Cancellation also terminates immediately. The completed result contains every successfully registered name in `registered_hosts`, which may contain fewer than three entries.
