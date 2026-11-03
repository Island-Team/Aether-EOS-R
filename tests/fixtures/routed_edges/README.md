# R01 routed-edge fixtures and executable coverage

`cases.json` freezes the 28 owner cases. `check_cases.py` validates the machine fixture. The live graph route is only the exact `@v0.1` camera frame to gate frame pair; synthetic state/event queues exercise scheduler limits without adding graph routes. G06 profile admission remains independently covered by `../edge_contract/`.

Run `python3 tests/fixtures/routed_edges/check_cases.py` from the repository root.

| Fixture cases | Executable check |
| --- | --- |
| `exact_camera_gate`, `shared_camera_independent_gates`, `status_has_no_payload_or_health`, `route_status_after_stop` | `process::routed_camera_frames_are_gate_specific_and_report_live_status` checks the exact route, distinct gate generations, 15 Hz versus a near-zero cap, fixed rows, digest, malformed status request, and shutdown with a stalled client. |
| `default_priority`, `fixed_actuator_unthrottled`, `fixed_result_health_stop_unthrottled` | `edge_contract::legacy_one_source_array_and_absent_qos_stay_valid`, `edge_contract::camera_evidence_is_the_only_user_scheduled_eos_route`, and `routes::tests::admission_rejects_excess_routes_and_non_evidence_class`. |
| `similar_legacy_pair`, `legacy_preview_is_not_r01` | `process::legacy_camera_preview_keeps_direct_path_without_live_route`, including the admitted legacy EGIA guard → EOS Dispatcher edge being omitted from EOS status. |
| `contributed_port_no_implicit_route`, `cross_core_designonly_not_in_eos_status` | `edge_contract::exact_type_and_source_identity_are_required`, `edge_contract::contributed_command_shape_cannot_bypass_dispatcher`, and the exact route selection and EOS-local filter in `VisionService::start`; no contributed evidence route is installed. The cross-core status omission is also exercised by the legacy process fixture. |
| `monotonic_cap_no_burst`, `max_finite_frequency`, `min_subnormal_frequency`, `consumer_error` | `routes::tests::cap_handles_extreme_rates_and_backward_time`, `routes::tests::delivery_cap_waits_without_gap_and_consumes_failed_attempt`, and `routes::tests::delivery_timestamp_is_sampled_at_acceptance_commit`. |
| `latest_replacement`, `fifo_saturation`, `fifo_oversized`, `global_byte_pressure` | `routes::tests::latest_replaces_and_fifo_rejects_newest`, `routes::tests::byte_limits_preserve_older_fifo_and_latest_on_pressure`. |
| `low_weight_progress`, `large_low_weight_progress`, `ready_65_cursor`, `ready_256_cursor`, `ready_256_mixed_weights` | `routes::tests::priority_weight_and_large_payload_credit_are_bounded`, `routes::tests::cursor_reaches_sixty_fifth_route_on_second_slice`, `routes::tests::cursor_reaches_every_route_with_256_ready`, and `routes::tests::mixed_weight_cursor_visits_all_continuously_ready_routes`. |
| `stop_with_backlog`, `inflight_accept_before_cancel`, `inflight_deadline_cancel` | `routes::tests::stop_cancels_backlog_and_rejects_new_admission`, `routes::tests::accepted_before_cancel_is_counted_as_delivery`, `routes::tests::stop_cancels_inflight_once_and_fences_late_accept`, `routes::tests::acceptance_after_deadline_is_cancelled_once`, `routes::tests::timed_out_preparation_never_publishes_a_gate_frame`, `routes::tests::sealed_scheduler_fences_paused_consumer_publication_and_producer_admission`, and `vision::tests::blocked_capture_worker_reports_join_timeout_without_waiting_forever`. The process finalizer seals admission before Dispatcher emergency stop and drains afterward. |

The 100 ms consumer acceptance deadline does not bound a synchronous camera adapter capture. Shutdown fences publication and reports a failed 1 s worker join if capture remains blocked; it cannot terminate a blocked OS thread. R06 owns simulator termination and reap evidence.

The admitted positive-subnormal camera sampling rate is exercised by `vision::tests::tiny_positive_camera_rate_has_safe_saturated_cadence` and `process::subnormal_camera_rate_starts_and_stops_without_cadence_panic`.

A saturated fanout regression, `routes::tests::saturated_shared_frame_fanout_does_not_hold_stop_seal_behind_payload_copies`, uses 256 routes and one shared 8 MiB payload. It checks 8 retained routes under the 64 MiB quota, 248 overflow counters, pointer identity for every retained sample, and bounded producer-lock and stop-seal latency under a held-lock handoff.
