//! Transport-neutral admission and routing for EOS hardware nodes.
//! Each command ID is reserved before a single adapter call. Ambiguous outcomes
//! remain terminal and are never retried by this dispatcher.

mod journal;
mod registry;
mod gateway_stop;

#[cfg(test)]
extern crate self as aether_dispatcher;
#[cfg(test)]
#[path = "../tests/gateway_stop.rs"]
mod gateway_stop_unit_tests;

#[cfg(test)]
mod release_boundary_tests {
  use super::*;
  use std::sync::mpsc::Sender;
  use std::thread::JoinHandle;
  use std::time::Duration;
  use aether_hal::{CommandOutcome, ControlledStopOutcome, NodeAdapter};
  use crate::gateway_stop_unit_tests::{
    ProofBase, directory, fixture, managed_context, managed_fixture, move_a, stop,
  };

  struct BoundaryThreads {
    release: Option<Sender<()>>,
    stop_release: Option<Sender<()>>,
    moving: Option<JoinHandle<DispatchResult>>,
    stopping: Option<JoinHandle<DispatchResult>>,
  }

  impl BoundaryThreads {
    fn release(&mut self) {
      if let Some(release) = self.release.take() {
        let _ = release.send(());
      }
      if let Some(release) = self.stop_release.take() {
        let _ = release.send(());
      }
    }
  }

  impl Drop for BoundaryThreads {
    fn drop(&mut self) {
      self.release();
      if let Some(moving) = self.moving.take() {
        let _ = moving.join();
      }
      if let Some(stopping) = self.stopping.take() {
        let _ = stopping.join();
      }
    }
  }

  fn paused_releasing(ambiguous: bool) {
    let path = directory();
    let a = Arc::new(ProofBase::new());
    let b = Arc::new(ProofBase::new());
    if ambiguous {
      a.movement_outcome(CommandOutcome::Ambiguous);
      a.controlled_outcome(ControlledStopOutcome::OutcomeUnknown);
    }
    let dispatcher = Arc::new(fixture(&path, a.clone(), b.clone()));
    let (entered_tx, entered_rx) = std::sync::mpsc::channel();
    let (release_tx, release_rx) = std::sync::mpsc::channel();
    *dispatcher.release_pause.lock().unwrap() = Some((entered_tx, release_rx));
    let mut threads = BoundaryThreads {
      release: Some(release_tx),
      stop_release: None,
      moving: None,
      stopping: None,
    };
    threads.moving = Some({
      let dispatcher = dispatcher.clone();
      std::thread::spawn(move || {
        dispatcher.dispatch_gateway_movement(move_a("pre-adapter"), "base.velocity")
      })
    });
    entered_rx.recv_timeout(Duration::from_secs(1)).unwrap();
    assert_eq!(
      dispatcher
        .journal
        .lock()
        .unwrap()
        .get("pre-adapter")
        .unwrap()
        .phase,
      CommandPhase::Releasing,
    );
    let (stop_entered_tx, stop_entered_rx) = std::sync::mpsc::channel();
    let (stop_release_tx, stop_release_rx) = std::sync::mpsc::channel();
    threads.stop_release = Some(stop_release_tx);
    *dispatcher.stop_inhibit_pause.lock().unwrap() = Some((stop_entered_tx, stop_release_rx));
    threads.stopping = Some({
      let dispatcher = dispatcher.clone();
      std::thread::spawn(move || {
        dispatcher.dispatch_gateway_stop(stop(
          "ordered-boundary",
          StopKind::RobotStop {
            robot_id: "r".into(),
          },
          false,
        ))
      })
    });
    stop_entered_rx
      .recv_timeout(Duration::from_secs(5))
      .unwrap();
    assert!(dispatcher.movement_inhibited());
    assert_eq!((a.calls(), b.calls()), (0, 0));
    threads.release();
    assert_eq!(
      threads.moving.take().unwrap().join().unwrap().status,
      if ambiguous {
        OutcomeStatus::OutcomeUnknown
      } else {
        OutcomeStatus::Completed
      },
    );
    assert_eq!(
      threads.stopping.take().unwrap().join().unwrap().status,
      if ambiguous {
        OutcomeStatus::OutcomeUnknown
      } else {
        OutcomeStatus::Completed
      },
    );
    drop(dispatcher);
    let recovered = fixture(&path, a.clone(), b.clone());
    if ambiguous {
      assert!(recovered.movement_inhibited());
      let before = (a.calls(), b.calls());
      assert_eq!(
        recovered
          .dispatch_gateway_stop(stop(
            "ordered-boundary",
            StopKind::RobotStop {
              robot_id: "r".into()
            },
            false,
          ))
          .status,
        OutcomeStatus::OutcomeUnknown,
      );
      assert_eq!((a.calls(), b.calls()), before);
      a.controlled_outcome(ControlledStopOutcome::Stopped);
      assert_eq!(
        recovered
          .dispatch_gateway_stop(stop(
            "fenced-recovery",
            StopKind::RobotStop {
              robot_id: "r".into()
            },
            true,
          ))
          .status,
        OutcomeStatus::Completed,
      );
      assert!(!recovered.movement_inhibited());
    } else {
      assert!(!recovered.movement_inhibited());
    }
    drop(recovered);
    std::fs::remove_dir_all(path).unwrap();
  }

  #[test]
  fn synced_releasing_pause_precedes_adapter_and_robot_stop() {
    paused_releasing(false);
  }

  #[test]
  fn ambiguous_release_at_exact_boundary_stays_inhibited_across_restart() {
    paused_releasing(true);
  }

  #[test]
  fn routine_marker_cannot_resolve_while_admitted_public_stop_is_active() {
    let path = directory();
    let a = Arc::new(ProofBase::new());
    let b = Arc::new(ProofBase::new());
    let dispatcher = managed_fixture(
      &path,
      a,
      b,
      Arc::new(aether_common::time::ManualClock::new(0, None)),
      "run-a",
    );
    dispatcher
      .journal
      .lock()
      .unwrap()
      .set_shutdown_pending(vec!["cores/eos/layers/hardware/groups/base/nodes/a".into()])
      .unwrap();
    dispatcher
      .shutdown_marker_owned
      .store(true, Ordering::SeqCst);
    let admitted_stop = dispatcher.begin_public_stop().unwrap();
    dispatcher.quiesce();
    assert!(!dispatcher.resolve_managed_shutdown());
    drop(admitted_stop);
    assert!(dispatcher.resolve_managed_shutdown());
    drop(dispatcher);
    std::fs::remove_dir_all(path).unwrap();
  }

  #[test]
  fn stop_record_overflow_before_release_calls_no_adapter_and_keeps_inhibit_clear() {
    let path = directory();
    let a = Arc::new(ProofBase::new());
    let b = Arc::new(ProofBase::new());
    let dispatcher = fixture(&path, a.clone(), b.clone());
    dispatcher
      .journal
      .lock()
      .unwrap()
      .set_record_limit_for_test(128);
    let outcome = dispatcher.dispatch_gateway_stop_checked(stop(
      "overflow-pre",
      StopKind::RobotStop {
        robot_id: "r".into(),
      },
      false,
    ));
    assert!(
      matches!(outcome, StopDispatchOutcome::PreReleaseCapacity(result) if result.status == OutcomeStatus::Rejected)
    );
    assert!(dispatcher
      .journal
      .lock()
      .unwrap()
      .get("overflow-pre")
      .is_none());
    assert_eq!((a.calls(), b.calls()), (0, 0));
    assert!(!dispatcher.movement_inhibited());
    drop(dispatcher);
    std::fs::remove_dir_all(path).unwrap();
  }

  #[test]
  fn pre_release_control_write_failure_is_internal_and_never_calls_adapter() {
    let path = directory();
    let a = Arc::new(ProofBase::new());
    let b = Arc::new(ProofBase::new());
    let dispatcher = fixture(&path, a.clone(), b.clone());
    std::fs::remove_file(path.join(".control")).unwrap();
    std::fs::create_dir(path.join(".control")).unwrap();
    let outcome = dispatcher.dispatch_gateway_stop_checked(stop(
      "durability-pre",
      StopKind::RobotStop {
        robot_id: "r".into(),
      },
      false,
    ));
    assert!(
      matches!(outcome, StopDispatchOutcome::PreReleaseDurability(result) if result.status == OutcomeStatus::Rejected)
    );
    assert_eq!((a.calls(), b.calls()), (0, 0));
    assert!(!dispatcher.movement_inhibited());
    assert_eq!(
      dispatcher
        .gateway_operation("durability-pre")
        .unwrap()
        .unwrap()
        .status,
      OutcomeStatus::Rejected,
    );
    assert_eq!(
      dispatcher
        .gateway_operation("durability-pre")
        .unwrap()
        .unwrap()
        .rejection_code,
      Some(GatewayRejectionCode::Internal),
    );
    std::fs::remove_dir(path.join(".control")).unwrap();
    drop(dispatcher);
    std::fs::remove_dir_all(path).unwrap();
  }

  #[test]
  fn stop_result_overflow_after_call_is_durable_unknown_and_never_replays() {
    let path = directory();
    let a = Arc::new(ProofBase::new());
    let b = Arc::new(ProofBase::new());
    let (entered_tx, entered_rx) = std::sync::mpsc::channel();
    let (release_tx, release_rx) = std::sync::mpsc::channel();
    a.pause_controlled(entered_tx, release_rx);
    let dispatcher = Arc::new(fixture(&path, a.clone(), b.clone()));
    let request = stop(
      "overflow-post",
      StopKind::RobotStop {
        robot_id: "r".into(),
      },
      false,
    );
    let stopping = {
      let dispatcher = dispatcher.clone();
      let request = request.clone();
      std::thread::spawn(move || dispatcher.dispatch_gateway_stop(request))
    };
    entered_rx.recv_timeout(Duration::from_secs(1)).unwrap();
    let record = std::fs::read_dir(&path)
      .unwrap()
      .map(|entry| entry.unwrap().path())
      .find(|path| path.extension().is_some_and(|ext| ext == "rec"))
      .unwrap();
    let before = std::fs::metadata(&record).unwrap().len() as usize;
    dispatcher
      .journal
      .lock()
      .unwrap()
      .set_record_limit_for_test(before + 1);
    release_tx.send(()).unwrap();
    assert_eq!(
      stopping.join().unwrap().status,
      OutcomeStatus::OutcomeUnknown
    );
    assert_eq!((a.calls(), b.calls()), (1, 0));
    assert!(dispatcher.movement_inhibited());
    assert_eq!(
      dispatcher.dispatch_gateway_stop(request.clone()).status,
      OutcomeStatus::OutcomeUnknown
    );
    assert_eq!((a.calls(), b.calls()), (1, 0));
    drop(dispatcher);
    let recovered = fixture(&path, a.clone(), b.clone());
    assert!(recovered.movement_inhibited());
    assert_eq!(
      recovered.dispatch_gateway_stop(request).status,
      OutcomeStatus::OutcomeUnknown
    );
    assert_eq!((a.calls(), b.calls()), (1, 0));
    drop(recovered);
    std::fs::remove_dir_all(path).unwrap();
  }

  #[test]
  fn crash_after_first_confirmed_part_leaves_second_unattempted_and_query_only() {
    let path = directory();
    let a = Arc::new(ProofBase::new());
    let b = Arc::new(ProofBase::new());
    let dispatcher = fixture(&path, a.clone(), b.clone());
    let request = stop(
      "partial-abandon",
      StopKind::RobotStop {
        robot_id: "r".into(),
      },
      false,
    );
    *dispatcher.stop_crash_after_part.lock().unwrap() = Some(0);
    let crashed = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
      dispatcher.dispatch_gateway_stop(request.clone())
    }));
    assert!(crashed.is_err());
    assert_eq!((a.calls(), b.calls()), (1, 0));
    {
      let journal = dispatcher.journal.lock().unwrap();
      let entry = journal.get("partial-abandon").unwrap();
      assert_eq!(entry.phase, CommandPhase::Releasing);
      let parts = &entry.operation.as_ref().unwrap().parts;
      assert_eq!(parts.len(), 2);
      assert!(parts[0].attempting);
      assert_eq!(parts[0].status, Some(OutcomeStatus::Completed));
      assert!(!parts[1].attempting);
      assert_eq!(parts[1].status, None);
    }
    drop(dispatcher);

    let recovered = fixture(&path, a.clone(), b.clone());
    assert!(recovered.movement_inhibited());
    let operation = recovered
      .gateway_operation("partial-abandon")
      .unwrap()
      .unwrap();
    assert_eq!(operation.status, OutcomeStatus::OutcomeUnknown);
    assert_eq!(operation.parts[0].status, OutcomeStatus::Completed);
    assert_eq!(operation.parts[1].status, OutcomeStatus::OutcomeUnknown);
    {
      let journal = recovered.journal.lock().unwrap();
      let parts = &journal
        .get("partial-abandon")
        .unwrap()
        .operation
        .as_ref()
        .unwrap()
        .parts;
      assert!(!parts[1].attempting);
    }
    assert_eq!(
      recovered.dispatch_gateway_stop(request).status,
      OutcomeStatus::OutcomeUnknown,
    );
    assert_eq!((a.calls(), b.calls()), (1, 0));
    drop(recovered);
    std::fs::remove_dir_all(path).unwrap();
  }

  #[test]
  fn queued_movement_permits_expire_and_cancel_without_releasing() {
    let path = directory();
    let a = Arc::new(ProofBase::new());
    let b = Arc::new(ProofBase::new());
    let (entered_tx, entered_rx) = std::sync::mpsc::channel();
    let (release_tx, release_rx) = std::sync::mpsc::channel();
    a.pause_controlled(entered_tx, release_rx);
    let dispatcher = Arc::new(fixture(&path, a.clone(), b));
    let (reserved_tx, reserved_rx) = std::sync::mpsc::channel();
    let (reserved_release_tx, reserved_release_rx) = std::sync::mpsc::channel();
    *dispatcher.reservation_pause.lock().unwrap() = Some(ReservationPause {
      command_id: "queued-0".into(),
      entered: reserved_tx,
      release: reserved_release_rx,
    });
    let stopping = {
      let dispatcher = dispatcher.clone();
      std::thread::spawn(move || {
        let mut request = stop(
          "gate-held",
          StopKind::MotionStop {
            node_path: "cores/eos/layers/hardware/groups/base/nodes/a".into(),
          },
          false,
        );
        request.deadline_monotonic_ns = 10_000_000_000;
        dispatcher.dispatch_gateway_stop(request)
      })
    };
    entered_rx.recv_timeout(Duration::from_secs(10)).unwrap();
    let queued: Vec<_> = (0..16)
      .map(|index| {
        let dispatcher = dispatcher.clone();
        std::thread::spawn(move || {
          let mut request = move_a(&format!("queued-{index}"));
          request.meta.deadline_monotonic_ns = 1_000_000_000;
          dispatcher.dispatch_gateway_movement(request, "base.velocity")
        })
      })
      .collect();
    reserved_rx.recv_timeout(Duration::from_secs(10)).unwrap();
    assert!(dispatcher.gateway_operation("queued-0").unwrap().is_some());
    let cancellation = dispatcher.cancel("queued-0");
    reserved_release_tx.send(()).unwrap();
    assert_eq!(cancellation.unwrap().status, OutcomeStatus::Cancelled);
    let outcomes: Vec<_> = queued
      .into_iter()
      .map(|worker| worker.join().unwrap())
      .collect();
    assert_eq!(outcomes[0].status, OutcomeStatus::Cancelled);
    assert!(outcomes[1..]
      .iter()
      .all(|result| result.status == OutcomeStatus::Rejected
        && result.reason.as_deref() == Some("command deadline expired waiting for release gate")));
    assert_eq!(a.movement_calls(), 0);
    release_tx.send(()).unwrap();
    assert_eq!(stopping.join().unwrap().status, OutcomeStatus::Completed);
    assert_eq!(a.movement_calls(), 0);
    drop(dispatcher);
    std::fs::remove_dir_all(path).unwrap();
  }

  #[test]
  fn browser_terminal_write_fault_hides_stale_result_and_recovers_unknown() {
    let path = directory();
    let a = Arc::new(ProofBase::new());
    let b = Arc::new(ProofBase::new());
    let dispatcher = Arc::new(managed_fixture(
      &path,
      a.clone(),
      b.clone(),
      Arc::new(aether_common::time::ManualClock::new(0, None)),
      "run-a",
    ));
    let (entered_tx, entered_rx) = std::sync::mpsc::channel();
    let (release_tx, release_rx) = std::sync::mpsc::channel();
    a.pause_movement(entered_tx, release_rx);
    let context = managed_context(
      AuthorityRole::OperatorControl,
      AdmissionIngress::PrivateBridge,
    );
    let request = move_a("browser-write-fault");
    assert_eq!(
      dispatcher
        .submit_browser_movement(context.clone(), request.clone())
        .unwrap()
        .status,
      OutcomeStatus::InProgress
    );
    entered_rx.recv_timeout(Duration::from_secs(1)).unwrap();
    dispatcher
      .journal
      .lock()
      .unwrap()
      .set_record_limit_for_test(1);
    release_tx.send(()).unwrap();
    for _ in 0..1000 {
      if dispatcher
        .browser_lane
        .lock()
        .unwrap()
        .unavailable
        .contains("browser-write-fault")
      {
        break;
      }
      std::thread::sleep(Duration::from_millis(1));
    }
    assert!(dispatcher
      .browser_result_authorized_checked("browser-write-fault", "run-a")
      .is_err());
    assert_eq!(
      dispatcher
        .submit_browser_movement(context, request)
        .unwrap_err(),
      "result unavailable"
    );
    assert_eq!(a.movement_calls(), 1);
    assert!(dispatcher.movement_inhibited());
    assert!(dispatcher
      .browser_lane
      .lock()
      .unwrap()
      .active
      .contains("browser-write-fault"));
    for _ in 0..1000 {
      if Arc::strong_count(&dispatcher) == 1 {
        break;
      }
      std::thread::sleep(Duration::from_millis(1));
    }
    assert_eq!(Arc::strong_count(&dispatcher), 1);
    drop(dispatcher);
    let recovered = managed_fixture(
      &path,
      a.clone(),
      b,
      Arc::new(aether_common::time::ManualClock::new(0, None)),
      "run-a",
    );
    assert_eq!(
      recovered
        .browser_result_authorized("browser-write-fault", "run-a")
        .unwrap()
        .status,
      OutcomeStatus::OutcomeUnknown
    );
    assert!(recovered.movement_inhibited());
    assert_eq!(a.movement_calls(), 1);
    drop(recovered);
    std::fs::remove_dir_all(path).unwrap();
  }

  #[test]
  fn browser_pre_release_stop_write_fault_never_calls_adapter_and_recovers_cancelled() {
    let path = directory();
    let a = Arc::new(ProofBase::new());
    let b = Arc::new(ProofBase::new());
    let dispatcher = Arc::new(managed_fixture(
      &path,
      a.clone(),
      b.clone(),
      Arc::new(aether_common::time::ManualClock::new(0, None)),
      "run-a",
    ));
    let (entered_tx, entered_rx) = std::sync::mpsc::channel();
    let (release_tx, release_rx) = std::sync::mpsc::channel();
    a.pause_movement(entered_tx, release_rx);
    let context = managed_context(
      AuthorityRole::OperatorControl,
      AdmissionIngress::PrivateBridge,
    );
    assert_eq!(
      dispatcher
        .submit_browser_movement(context.clone(), move_a("browser-first"))
        .unwrap()
        .status,
      OutcomeStatus::InProgress
    );
    entered_rx.recv_timeout(Duration::from_secs(1)).unwrap();
    let queued = move_a("browser-queued");
    assert_eq!(
      dispatcher
        .submit_browser_movement(context.clone(), queued.clone())
        .unwrap()
        .status,
      OutcomeStatus::InProgress
    );
    dispatcher.quiesce();
    dispatcher
      .journal
      .lock()
      .unwrap()
      .set_record_limit_for_test(1);
    dispatcher.cancel_prepared_browser_after_stop();
    assert!(dispatcher
      .browser_result_authorized_checked("browser-queued", "run-a")
      .is_err());
    assert_eq!(
      dispatcher
        .submit_browser_movement(context, queued)
        .unwrap_err(),
      "result unavailable"
    );
    assert!(dispatcher
      .browser_lane
      .lock()
      .unwrap()
      .active
      .contains("browser-queued"));
    release_tx.send(()).unwrap();
    for _ in 0..1000 {
      if a.movement_calls() == 1
        && dispatcher
          .browser_lane
          .lock()
          .unwrap()
          .unavailable
          .contains("browser-first")
      {
        break;
      }
      std::thread::sleep(Duration::from_millis(1));
    }
    assert_eq!(a.movement_calls(), 1);
    for _ in 0..1000 {
      if Arc::strong_count(&dispatcher) == 1 {
        break;
      }
      std::thread::sleep(Duration::from_millis(1));
    }
    assert_eq!(Arc::strong_count(&dispatcher), 1);
    drop(dispatcher);
    let recovered = managed_fixture(
      &path,
      a.clone(),
      b,
      Arc::new(aether_common::time::ManualClock::new(0, None)),
      "run-a",
    );
    assert_eq!(
      recovered
        .browser_result_authorized("browser-queued", "run-a")
        .unwrap()
        .status,
      OutcomeStatus::Cancelled
    );
    assert_eq!(
      recovered
        .browser_result_authorized("browser-first", "run-a")
        .unwrap()
        .status,
      OutcomeStatus::OutcomeUnknown
    );
    assert_eq!(a.movement_calls(), 1);
    drop(recovered);
    std::fs::remove_dir_all(path).unwrap();
  }

  #[test]
  fn browser_full_lane_rejection_write_failure_stays_unreserved() {
    let path = directory();
    let a = Arc::new(ProofBase::new());
    let b = Arc::new(ProofBase::new());
    let dispatcher = Arc::new(managed_fixture(
      &path,
      a.clone(),
      b,
      Arc::new(aether_common::time::ManualClock::new(0, None)),
      "run-a",
    ));
    let (entered_tx, entered_rx) = std::sync::mpsc::channel();
    let (release_tx, release_rx) = std::sync::mpsc::channel();
    a.pause_movement(entered_tx, release_rx);
    let context = managed_context(
      AuthorityRole::OperatorControl,
      AdmissionIngress::PrivateBridge,
    );
    assert_eq!(
      dispatcher
        .submit_browser_movement(context.clone(), move_a("slot-0"))
        .unwrap()
        .status,
      OutcomeStatus::InProgress
    );
    entered_rx.recv_timeout(Duration::from_secs(1)).unwrap();
    for index in 1..8 {
      assert_eq!(
        dispatcher
          .submit_browser_movement(context.clone(), move_a(&format!("slot-{index}")))
          .unwrap()
          .status,
        OutcomeStatus::InProgress
      );
    }
    dispatcher
      .journal
      .lock()
      .unwrap()
      .set_record_limit_for_test(1);
    assert_eq!(
      dispatcher
        .submit_browser_movement(context, move_a("capacity-write-fault"))
        .unwrap_err(),
      "movement capacity unavailable"
    );
    assert!(dispatcher
      .journal
      .lock()
      .unwrap()
      .get("capacity-write-fault")
      .is_none());
    assert_eq!(a.movement_calls(), 0);
    dispatcher.quiesce();
    dispatcher.cancel_prepared_browser_after_stop();
    release_tx.send(()).unwrap();
    for _ in 0..1000 {
      if Arc::strong_count(&dispatcher) == 1 {
        break;
      }
      std::thread::sleep(Duration::from_millis(1));
    }
    assert_eq!(Arc::strong_count(&dispatcher), 1);
    drop(dispatcher);
    std::fs::remove_dir_all(path).unwrap();
  }

  #[test]
  fn browser_worker_spawn_failure_seals_prepared_without_adapter_call() {
    let path = directory();
    let a = Arc::new(ProofBase::new());
    let b = Arc::new(ProofBase::new());
    let dispatcher = Arc::new(managed_fixture(
      &path,
      a.clone(),
      b.clone(),
      Arc::new(aether_common::time::ManualClock::new(0, None)),
      "run-a",
    ));
    dispatcher.browser_spawn_fail.store(true, Ordering::SeqCst);
    let context = managed_context(
      AuthorityRole::OperatorControl,
      AdmissionIngress::PrivateBridge,
    );
    let result = dispatcher
      .submit_browser_movement(context, move_a("spawn-fault"))
      .unwrap();
    assert_eq!(result.status, OutcomeStatus::Rejected);
    assert_eq!(
      dispatcher
        .browser_result_authorized("spawn-fault", "run-a")
        .unwrap()
        .status,
      OutcomeStatus::Rejected
    );
    assert!(!dispatcher
      .browser_lane
      .lock()
      .unwrap()
      .active
      .contains("spawn-fault"));
    assert_eq!(a.movement_calls(), 0);
    drop(dispatcher);
    let recovered = managed_fixture(
      &path,
      a.clone(),
      b,
      Arc::new(aether_common::time::ManualClock::new(0, None)),
      "run-a",
    );
    assert_eq!(
      recovered
        .browser_result_authorized("spawn-fault", "run-a")
        .unwrap()
        .status,
      OutcomeStatus::Rejected
    );
    assert_eq!(a.movement_calls(), 0);
    drop(recovered);
    std::fs::remove_dir_all(path).unwrap();
  }

  fn browser_transient_pre_release_fault(expire: bool) {
    let path = directory();
    let a = Arc::new(ProofBase::new());
    let b = Arc::new(ProofBase::new());
    let clock = Arc::new(aether_common::time::ManualClock::new(0, None));
    let dispatcher = Arc::new(managed_fixture(&path, a.clone(), b, clock.clone(), "run-a"));
    let (entered_tx, entered_rx) = std::sync::mpsc::channel();
    let (release_tx, release_rx) = std::sync::mpsc::channel();
    a.pause_movement(entered_tx, release_rx);
    let first = {
      let dispatcher = dispatcher.clone();
      std::thread::spawn(move || {
        dispatcher.dispatch_managed_movement(
          managed_context(AuthorityRole::EgiaControl, AdmissionIngress::Grpc),
          move_a("prior-release"),
        )
      })
    };
    entered_rx.recv_timeout(Duration::from_secs(1)).unwrap();
    let context = managed_context(
      AuthorityRole::OperatorControl,
      AdmissionIngress::PrivateBridge,
    );
    let mut queued = move_a("queued-fault");
    if expire {
      queued.meta.deadline_monotonic_ns = 300_000_000;
    }
    assert_eq!(
      dispatcher
        .submit_browser_movement(context.clone(), queued.clone())
        .unwrap()
        .status,
      OutcomeStatus::InProgress
    );
    dispatcher
      .journal
      .lock()
      .unwrap()
      .fail_next_record_writes_for_test("queued-fault", 1);
    if expire {
      clock.set_monotonic_ns(400_000_000);
    } else {
      a.set_state(NodeState::Stopped);
    }
    release_tx.send(()).unwrap();
    assert_eq!(first.join().unwrap().status, OutcomeStatus::Completed);
    for _ in 0..1000 {
      if dispatcher
        .browser_result_authorized("queued-fault", "run-a")
        .is_some_and(|result| result.status == OutcomeStatus::Rejected)
      {
        break;
      }
      std::thread::sleep(Duration::from_millis(1));
    }
    assert_eq!(
      dispatcher
        .browser_result_authorized("queued-fault", "run-a")
        .unwrap()
        .status,
      OutcomeStatus::Rejected
    );
    assert_eq!(
      dispatcher
        .submit_browser_movement(context, queued)
        .unwrap()
        .status,
      OutcomeStatus::Rejected
    );
    assert!(dispatcher.movement_inhibited());
    assert!(!dispatcher
      .browser_lane
      .lock()
      .unwrap()
      .active
      .contains("queued-fault"));
    assert_eq!(a.movement_calls(), 1);
    for _ in 0..1000 {
      if Arc::strong_count(&dispatcher) == 1 {
        break;
      }
      std::thread::sleep(Duration::from_millis(1));
    }
    assert_eq!(Arc::strong_count(&dispatcher), 1);
    drop(dispatcher);
    std::fs::remove_dir_all(path).unwrap();
  }

  #[test]
  fn browser_queued_expiry_terminal_write_retries_durably() {
    browser_transient_pre_release_fault(true);
  }

  #[test]
  fn browser_queued_health_terminal_write_retries_durably() {
    browser_transient_pre_release_fault(false);
  }

  #[test]
  fn browser_queued_stop_terminal_write_retries_cancelled() {
    let path = directory();
    let a = Arc::new(ProofBase::new());
    let b = Arc::new(ProofBase::new());
    let dispatcher = Arc::new(managed_fixture(
      &path,
      a.clone(),
      b,
      Arc::new(aether_common::time::ManualClock::new(0, None)),
      "run-a",
    ));
    let (entered_tx, entered_rx) = std::sync::mpsc::channel();
    let (release_tx, release_rx) = std::sync::mpsc::channel();
    a.pause_movement(entered_tx, release_rx);
    let first = {
      let dispatcher = dispatcher.clone();
      std::thread::spawn(move || {
        dispatcher.dispatch_managed_movement(
          managed_context(AuthorityRole::EgiaControl, AdmissionIngress::Grpc),
          move_a("prior-stop"),
        )
      })
    };
    entered_rx.recv_timeout(Duration::from_secs(1)).unwrap();
    let context = managed_context(
      AuthorityRole::OperatorControl,
      AdmissionIngress::PrivateBridge,
    );
    let queued = move_a("queued-stop-retry");
    assert_eq!(
      dispatcher
        .submit_browser_movement(context.clone(), queued.clone())
        .unwrap()
        .status,
      OutcomeStatus::InProgress
    );
    dispatcher
      .journal
      .lock()
      .unwrap()
      .fail_next_record_writes_for_test("queued-stop-retry", 1);
    dispatcher.quiesce();
    dispatcher.cancel_prepared_browser_after_stop();
    assert!(dispatcher
      .browser_result_authorized_checked("queued-stop-retry", "run-a")
      .is_err());
    assert_eq!(
      dispatcher
        .submit_browser_movement(context, queued)
        .unwrap_err(),
      "result unavailable"
    );
    release_tx.send(()).unwrap();
    let _ = first.join().unwrap();
    for _ in 0..1000 {
      if dispatcher
        .browser_result_authorized("queued-stop-retry", "run-a")
        .is_some_and(|result| result.status == OutcomeStatus::Cancelled)
      {
        break;
      }
      std::thread::sleep(Duration::from_millis(1));
    }
    assert_eq!(
      dispatcher
        .browser_result_authorized("queued-stop-retry", "run-a")
        .unwrap()
        .status,
      OutcomeStatus::Cancelled
    );
    assert!(!dispatcher
      .browser_lane
      .lock()
      .unwrap()
      .active
      .contains("queued-stop-retry"));
    assert_eq!(a.movement_calls(), 1);
    for _ in 0..1000 {
      if Arc::strong_count(&dispatcher) == 1 {
        break;
      }
      std::thread::sleep(Duration::from_millis(1));
    }
    assert_eq!(Arc::strong_count(&dispatcher), 1);
    drop(dispatcher);
    std::fs::remove_dir_all(path).unwrap();
  }

  #[test]
  fn browser_stop_wins_after_failed_queued_rejection_write() {
    let path = directory();
    let a = Arc::new(ProofBase::new());
    let b = Arc::new(ProofBase::new());
    let clock = Arc::new(aether_common::time::ManualClock::new(0, None));
    let dispatcher = Arc::new(managed_fixture(
      &path,
      a.clone(),
      b.clone(),
      clock.clone(),
      "run-a",
    ));
    let (entered_tx, entered_rx) = std::sync::mpsc::channel();
    let (release_tx, release_rx) = std::sync::mpsc::channel();
    a.pause_movement(entered_tx, release_rx);
    let first = {
      let dispatcher = dispatcher.clone();
      std::thread::spawn(move || {
        dispatcher.dispatch_managed_movement(
          managed_context(AuthorityRole::EgiaControl, AdmissionIngress::Grpc),
          move_a("prior-stop-race"),
        )
      })
    };
    entered_rx.recv_timeout(Duration::from_secs(1)).unwrap();
    let mut queued = move_a("queued-stop-race");
    queued.meta.deadline_monotonic_ns = 300_000_000;
    assert_eq!(
      dispatcher
        .submit_browser_movement(
          managed_context(
            AuthorityRole::OperatorControl,
            AdmissionIngress::PrivateBridge
          ),
          queued,
        )
        .unwrap()
        .status,
      OutcomeStatus::InProgress
    );
    dispatcher
      .journal
      .lock()
      .unwrap()
      .fail_next_record_writes_for_test("queued-stop-race", 1);
    clock.set_monotonic_ns(400_000_000);

    // Keep the fault path between its failed terminal write and durable retry.
    // browser_fault latches inhibit first, then waits for this lane lock.
    let lane = dispatcher.browser_lane.lock().unwrap();
    release_tx.send(()).unwrap();
    for _ in 0..1000 {
      if dispatcher.movement_inhibited() {
        break;
      }
      std::thread::sleep(Duration::from_millis(1));
    }
    assert!(dispatcher.movement_inhibited());
    assert!(lane.active.contains("queued-stop-race"));
    assert!(!lane.unavailable.contains("queued-stop-race"));
    dispatcher.quiesce();
    drop(lane);
    let _ = first.join().unwrap();
    for _ in 0..1000 {
      if dispatcher
        .browser_result_authorized("queued-stop-race", "run-a")
        .is_some_and(|result| result.status == OutcomeStatus::Cancelled)
      {
        break;
      }
      std::thread::sleep(Duration::from_millis(1));
    }
    assert_eq!(
      dispatcher
        .browser_result_authorized("queued-stop-race", "run-a")
        .unwrap()
        .status,
      OutcomeStatus::Cancelled
    );
    assert_eq!(
      dispatcher
        .journal
        .lock()
        .unwrap()
        .get("queued-stop-race")
        .unwrap()
        .phase,
      CommandPhase::Terminal
    );
    assert!(!dispatcher
      .browser_lane
      .lock()
      .unwrap()
      .active
      .contains("queued-stop-race"));
    assert_eq!(a.movement_calls(), 1);
    for _ in 0..1000 {
      if Arc::strong_count(&dispatcher) == 1 {
        break;
      }
      std::thread::sleep(Duration::from_millis(1));
    }
    assert_eq!(Arc::strong_count(&dispatcher), 1);
    drop(dispatcher);
    let recovered = managed_fixture(
      &path,
      a.clone(),
      b,
      Arc::new(aether_common::time::ManualClock::new(0, None)),
      "run-a",
    );
    assert_eq!(
      recovered
        .browser_result_authorized("queued-stop-race", "run-a")
        .unwrap()
        .status,
      OutcomeStatus::Cancelled
    );
    assert_eq!(a.movement_calls(), 1);
    drop(recovered);
    std::fs::remove_dir_all(path).unwrap();
  }

  #[test]
  fn browser_late_adapter_reply_after_stop_remains_unknown() {
    let path = directory();
    let a = Arc::new(ProofBase::new());
    let b = Arc::new(ProofBase::new());
    let dispatcher = Arc::new(managed_fixture(
      &path,
      a.clone(),
      b,
      Arc::new(aether_common::time::ManualClock::new(0, None)),
      "run-a",
    ));
    let (entered_tx, entered_rx) = std::sync::mpsc::channel();
    let (release_tx, release_rx) = std::sync::mpsc::channel();
    a.pause_movement(entered_tx, release_rx);
    let context = managed_context(
      AuthorityRole::OperatorControl,
      AdmissionIngress::PrivateBridge,
    );
    assert_eq!(
      dispatcher
        .submit_browser_movement(context, move_a("late-after-stop"))
        .unwrap()
        .status,
      OutcomeStatus::InProgress
    );
    entered_rx.recv_timeout(Duration::from_secs(1)).unwrap();
    dispatcher.quiesce();
    a.stop().unwrap();
    dispatcher.cancel_prepared_browser_after_stop();
    release_tx.send(()).unwrap();
    for _ in 0..1000 {
      if dispatcher
        .browser_result_authorized("late-after-stop", "run-a")
        .unwrap()
        .status
        == OutcomeStatus::OutcomeUnknown
      {
        break;
      }
      std::thread::sleep(Duration::from_millis(1));
    }
    assert_eq!(
      dispatcher
        .browser_result_authorized("late-after-stop", "run-a")
        .unwrap()
        .status,
      OutcomeStatus::OutcomeUnknown
    );
    assert!(dispatcher.movement_inhibited());
    assert_eq!(a.movement_calls(), 1);
    for _ in 0..1000 {
      if Arc::strong_count(&dispatcher) == 1 {
        break;
      }
      std::thread::sleep(Duration::from_millis(1));
    }
    assert_eq!(Arc::strong_count(&dispatcher), 1);
    drop(dispatcher);
    std::fs::remove_dir_all(path).unwrap();
  }
}

use std::collections::{BTreeMap, HashSet};
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use aether_common::io::BoundedWriter;
use aether_common::time::Clock;
use aether_hal::{
  canonical_node_path, BaseMovement, CameraFrame, CommandMeta, CommandOutcome, Health, HealthState,
  ManipulatorMovement, NodeState, NodeStatus, Profile,
};
use serde::{Deserialize, Serialize};
use thiserror::Error;

use journal::{
  CommandPhase, GatewayIdentity, GatewayOperation, Journal, JournalEntry, JournalTarget,
  OperationKind,
};
use registry::Registry;
pub use registry::{AdapterRegistration, NodeDescriptor, NodeFacet};
pub use gateway_stop::StopDispatchOutcome;

#[derive(Debug, Error)]
pub enum DispatchError {
  #[error("configuration: {0}")]
  Configuration(String),
  #[error("command journal: {0}")]
  Journal(#[from] std::io::Error),
}

#[derive(Clone, Debug, Serialize)]
pub enum Movement {
  Base(BaseMovement),
  Manipulator(ManipulatorMovement),
}

#[derive(Clone, Debug, Serialize)]
pub struct CommandRequest {
  pub node_path: String,
  pub meta: CommandMeta,
  pub movement: Movement,
}

/// Roles are produced by the local grant verifier, never parsed from a client
/// controlled command body.
#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum AuthorityRole {
  EgiaControl,
  OperatorControl,
}

/// The serving transport attests its own ingress before invoking Dispatcher.
#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum AdmissionIngress {
  Grpc,
  PrivateBridge,
  /// Server-attested invocation from the confined native skill broker.
  SkillWorker,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum MovementDelivery {
  #[default]
  OneShot,
  Stream,
}

/// Stable, credential-free identity stored with a governed operation. An
/// authenticated estop alone may omit the revision.
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct AdmissionContext {
  pub run_id: String,
  pub active_revision: Option<String>,
  pub authority_role: AuthorityRole,
  pub ingress: AdmissionIngress,
  pub binding_path: String,
  pub primitive: String,
  pub primitive_version: String,
  #[serde(default)]
  pub delivery_mode: MovementDelivery,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct AdmittedBinding {
  pub binding_path: String,
  pub node_path: String,
  pub primitive: String,
  pub primitive_version: String,
}

/// One immutable, IsOS-verified deployment generation per Dispatcher.
#[derive(Clone, Debug)]
pub struct AdmissionGeneration {
  pub run_id: String,
  pub active_revision: String,
  pub bindings: Vec<AdmittedBinding>,
}

struct ManagedGeneration {
  run_id: String,
  active_revision: String,
  bindings: BTreeMap<String, AdmittedBinding>,
}

impl ManagedGeneration {
  fn new(generation: AdmissionGeneration) -> Result<Self, DispatchError> {
    if generation.run_id.is_empty()
      || generation.run_id.len() > 128
      || !valid_revision(&generation.active_revision)
    {
      return Err(DispatchError::Configuration(
        "invalid managed generation identity".into(),
      ));
    }
    let mut bindings = BTreeMap::new();
    for mut binding in generation.bindings {
      if !canonical_node_path(&binding.binding_path).is_some_and(|path| {
        path.starts_with("eos/dispatcher/") || path.starts_with("isos/eos_dispatcher/")
      }) {
        return Err(DispatchError::Configuration(
          "binding source is not a canonical EOS dispatcher node".into(),
        ));
      }
      let Some(node_path) = canonical_node_path(&binding.node_path) else {
        return Err(DispatchError::Configuration(
          "invalid admitted binding target".into(),
        ));
      };
      binding.node_path = node_path;
      if binding.binding_path.is_empty()
        || binding.binding_path.len() > 512
        || binding.primitive.is_empty()
        || binding.primitive_version != "1.0.0"
        || bindings
          .insert(binding.binding_path.clone(), binding)
          .is_some()
      {
        return Err(DispatchError::Configuration(
          "invalid or duplicate admitted binding".into(),
        ));
      }
    }
    Ok(Self {
      run_id: generation.run_id,
      active_revision: generation.active_revision,
      bindings,
    })
  }
}

fn valid_revision(value: &str) -> bool {
  value.len() == 64
    && value
      .bytes()
      .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub enum OutcomeStatus {
  Completed,
  Failed,
  Cancelled,
  Rejected,
  InProgress,
  OutcomeUnknown,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct DispatchResult {
  pub command_id: String,
  pub status: OutcomeStatus,
  pub reason: Option<String>,
}

/// Durable public-operation view. Legacy journal records deliberately have no
/// typed identity and cannot be reconstructed as a primitive acknowledgement.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct GatewayOperationInfo {
  pub command_id: String,
  pub primitive: String,
  pub version: String,
  pub node_path: String,
  pub status: OutcomeStatus,
  pub reason: Option<String>,
  pub accepted: Option<bool>,
  pub rejection_code: Option<GatewayRejectionCode>,
  pub kind: GatewayOperationKind,
  pub target: GatewayTarget,
  pub parts: Vec<StopPartInfo>,
}

/// Durable machine code for a terminal admission rejection. The enum keeps
/// stable wire strings out of free-form diagnostic text.
#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum GatewayRejectionCode {
  Unauthorized,
  InvalidState,
  Internal,
}

impl GatewayRejectionCode {
  pub const fn as_str(self) -> &'static str {
    match self {
      Self::Unauthorized => "rejected.unauthorized",
      Self::InvalidState => "rejected.invalid_state",
      Self::Internal => "rejected.internal",
    }
  }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum GatewayOperationKind {
  Movement,
  MotionStop,
  RobotStop,
  SafetyEstop,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum GatewayTarget {
  Hardware { node_path: String },
  Robot { robot_id: String },
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct StopPartInfo {
  pub node_path: String,
  pub status: OutcomeStatus,
  pub reason: Option<String>,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
pub enum StopKind {
  MotionStop { node_path: String },
  RobotStop { robot_id: String },
  SafetyEstop { robot_id: String },
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct StopRequest {
  pub command_id: String,
  pub deadline_monotonic_ns: u64,
  pub kind: StopKind,
  /// Set only by a local owner authority when recovering a prior uncertain
  /// RobotStop. R03 supplies the broader authority admission policy.
  pub authorized_recovery: bool,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum GatewayLookupError {
  LegacyRecord,
  Unauthorized,
  WrongRun,
  NotFound,
  Internal,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum BrowserLookupError {
  Unavailable,
}

impl DispatchResult {
  fn new(id: &str, status: OutcomeStatus, reason: impl Into<Option<String>>) -> Self {
    Self {
      command_id: id.into(),
      status,
      reason: reason.into(),
    }
  }
}

pub type StopResults = Vec<(String, Result<(), String>)>;

#[cfg(test)]
struct ReservationPause {
  command_id: String,
  entered: std::sync::mpsc::Sender<()>,
  release: std::sync::mpsc::Receiver<()>,
}

pub struct Dispatcher {
  registry: Registry,
  robot_id: String,
  managed: Option<ManagedGeneration>,
  clock: Arc<dyn Clock>,
  emergency_stopped: AtomicBool,
  movement_inhibited: AtomicBool,
  release_gate: gateway_stop::ReleaseGate,
  stop_serial: Mutex<()>,
  public_stop_gate: Mutex<PublicStopGate>,
  public_estop_durability_failed: AtomicBool,
  shutdown_marker_owned: AtomicBool,
  stop_inflight: Arc<Mutex<HashSet<String>>>,
  managed_stop_kicks: Mutex<gateway_stop::ManagedStopKicks>,
  stop_owner_lock: Option<Arc<std::fs::File>>,
  #[cfg(test)]
  release_pause: Mutex<Option<(std::sync::mpsc::Sender<()>, std::sync::mpsc::Receiver<()>)>>,
  #[cfg(test)]
  reservation_pause: Mutex<Option<ReservationPause>>,
  #[cfg(test)]
  stop_inhibit_pause: Mutex<Option<(std::sync::mpsc::Sender<()>, std::sync::mpsc::Receiver<()>)>>,
  #[cfg(any(test, feature = "test-hooks"))]
  journal_release_pause:
    Mutex<Option<(std::sync::mpsc::Sender<()>, std::sync::mpsc::Receiver<()>)>>,
  #[cfg(test)]
  stop_crash_after_part: Mutex<Option<usize>>,
  #[cfg(test)]
  browser_spawn_fail: AtomicBool,
  quiescing: AtomicBool,
  journal: Arc<Mutex<Journal>>,
  browser_lane: Mutex<BrowserLane>,
}

const MAX_BROWSER_MOVEMENTS: usize = 8;

#[derive(Default)]
struct BrowserLane {
  active: HashSet<String>,
  unavailable: HashSet<String>,
}

#[derive(Default)]
struct PublicStopGate {
  closed: bool,
  active: usize,
}

struct PublicStopGuard<'a>(&'a Mutex<PublicStopGate>);

impl Drop for PublicStopGuard<'_> {
  fn drop(&mut self) {
    self.0.lock().expect("public stop gate poisoned").active -= 1;
  }
}

impl Dispatcher {
  pub fn new(
    profile: &Profile,
    registrations: Vec<AdapterRegistration>,
    clock: Arc<dyn Clock>,
  ) -> Result<Self, DispatchError> {
    Self::with_journal(profile, registrations, clock, Journal::memory(), None)
  }

  /// Open a single-owner durable journal before accepting physical commands.
  pub fn new_durable(
    profile: &Profile,
    registrations: Vec<AdapterRegistration>,
    clock: Arc<dyn Clock>,
    journal_directory: impl AsRef<Path>,
  ) -> Result<Self, DispatchError> {
    let journal = Journal::open(journal_directory)?;
    Self::with_journal(profile, registrations, clock, journal, None)
  }

  /// Managed write admission is fixed at construction from an IsOS-verified
  /// saved generation. It cannot be hot-swapped under a live listener.
  pub fn new_durable_managed(
    profile: &Profile,
    registrations: Vec<AdapterRegistration>,
    clock: Arc<dyn Clock>,
    journal_directory: impl AsRef<Path>,
    generation: AdmissionGeneration,
  ) -> Result<Self, DispatchError> {
    let managed = ManagedGeneration::new(generation)?;
    let journal = Journal::open(journal_directory)?;
    Self::with_journal(profile, registrations, clock, journal, Some(managed))
  }

  fn with_journal(
    profile: &Profile,
    registrations: Vec<AdapterRegistration>,
    clock: Arc<dyn Clock>,
    mut journal: Journal,
    managed: Option<ManagedGeneration>,
  ) -> Result<Self, DispatchError> {
    let registry = Registry::new(profile, registrations)?;
    if let Some(managed) = &managed {
      for binding in managed.bindings.values() {
        let Some(node) = registry.nodes.get(&binding.node_path) else {
          return Err(DispatchError::Configuration(format!(
            "binding has no admitted target: {}",
            binding.binding_path
          )));
        };
        if node.profile.movement.is_none() {
          return Err(DispatchError::Configuration(format!(
            "binding target has no movement: {}",
            binding.binding_path
          )));
        }
      }
    }
    journal.bind_robot_id(&profile.robot.id)?;
    let moving_targets: Vec<String> = registry
      .nodes
      .iter()
      .filter(|(_, entry)| entry.profile.movement.is_some())
      .map(|(path, _)| path.clone())
      .collect();
    journal.bind_shutdown_targets(&moving_targets)?;
    let movement_inhibited =
      journal.has_uncertain_movement() || journal.robot_inhibited() || journal.shutdown_pending();
    let emergency_stopped = journal.estop_latched();
    let known_stop_ids = journal.reserved_ids().cloned().collect();
    let stop_owner_lock = journal.retain_owner_lock()?.map(Arc::new);
    Ok(Self {
      registry,
      robot_id: profile.robot.id.clone(),
      managed,
      clock,
      emergency_stopped: AtomicBool::new(emergency_stopped),
      movement_inhibited: AtomicBool::new(movement_inhibited),
      release_gate: gateway_stop::ReleaseGate::new(),
      stop_serial: Mutex::new(()),
      public_stop_gate: Mutex::new(PublicStopGate::default()),
      public_estop_durability_failed: AtomicBool::new(false),
      shutdown_marker_owned: AtomicBool::new(false),
      stop_inflight: Arc::new(Mutex::new(HashSet::new())),
      managed_stop_kicks: Mutex::new(gateway_stop::ManagedStopKicks::new(known_stop_ids)),
      stop_owner_lock,
      #[cfg(test)]
      release_pause: Mutex::new(None),
      #[cfg(test)]
      reservation_pause: Mutex::new(None),
      #[cfg(test)]
      stop_inhibit_pause: Mutex::new(None),
      #[cfg(any(test, feature = "test-hooks"))]
      journal_release_pause: Mutex::new(None),
      #[cfg(test)]
      stop_crash_after_part: Mutex::new(None),
      #[cfg(test)]
      browser_spawn_fail: AtomicBool::new(false),
      quiescing: AtomicBool::new(false),
      journal: Arc::new(Mutex::new(journal)),
      browser_lane: Mutex::new(BrowserLane::default()),
    })
  }

  pub fn discover(&self) -> Vec<NodeDescriptor> {
    self.registry.discover()
  }

  /// Effective node health, including Rust-side observation failures.
  pub fn health(&self, node_path: &str) -> Result<Health, String> {
    self
      .registry
      .nodes
      .get(&canonical_node_path(node_path).unwrap_or_default())
      .map(|entry| entry.health())
      .ok_or_else(|| format!("unknown node: {node_path}"))
  }

  /// Adapter lifecycle state is independent of health and is safe to read after a stop.
  pub fn status(&self, node_path: &str) -> Result<NodeStatus, String> {
    let entry = self
      .registry
      .nodes
      .get(&canonical_node_path(node_path).unwrap_or_default())
      .ok_or_else(|| format!("unknown node: {node_path}"))?;
    Ok(match &entry.adapter {
      AdapterRegistration::Camera { adapter, .. } => adapter.status(),
      AdapterRegistration::Base { adapter, .. } => adapter.status(),
      AdapterRegistration::Manipulator { adapter, .. } => adapter.status(),
    })
  }

  /// A typed adapter promise, never inferred from a package or adapter name.
  pub fn has_terminal_stop_fence(&self, node_path: &str) -> bool {
    self
      .registry
      .nodes
      .get(&canonical_node_path(node_path).unwrap_or_default())
      .is_some_and(|entry| match &entry.adapter {
        AdapterRegistration::Camera { adapter, .. } => adapter.has_terminal_stop_fence(),
        AdapterRegistration::Base { adapter, .. } => adapter.has_terminal_stop_fence(),
        AdapterRegistration::Manipulator { adapter, .. } => adapter.has_terminal_stop_fence(),
      })
  }

  /// An uncertain physical release remains sealed until a separately reviewed
  /// recovery proves the adapter generation quiescent. Restart does not clear it.
  pub fn movement_inhibited(&self) -> bool {
    self.movement_inhibited.load(Ordering::SeqCst)
  }

  /// The durable safety-estop latch, independent of RobotStop's recoverable
  /// movement inhibit.
  pub fn emergency_stopped(&self) -> bool {
    self.emergency_stopped.load(Ordering::SeqCst)
  }

  /// Legacy durable records cannot be projected into a typed public ack.
  pub fn has_legacy_journal_records(&self) -> bool {
    self
      .journal
      .lock()
      .expect("journal poisoned")
      .has_legacy_records()
  }

  /// Deterministic capacity injection for cross-crate transport tests. The
  /// feature is enabled only by test dependencies and absent from release
  /// builds; it changes no normal journal limit.
  #[cfg(feature = "test-hooks")]
  pub fn set_journal_record_limit_for_test(&self, bytes: usize) {
    self
      .journal
      .lock()
      .expect("journal poisoned")
      .set_record_limit_for_test(bytes);
  }

  #[cfg(feature = "test-hooks")]
  pub fn pause_journal_release_for_test(
    &self,
    entered: std::sync::mpsc::Sender<()>,
    release: std::sync::mpsc::Receiver<()>,
  ) {
    *self
      .journal_release_pause
      .lock()
      .expect("journal pause poisoned") = Some((entered, release));
  }

  /// Close new movement release admission before process safety stop begins.
  pub fn quiesce(&self) {
    self.quiescing.store(true, Ordering::SeqCst);
    self
      .public_stop_gate
      .lock()
      .expect("public stop gate poisoned")
      .closed = true;
  }

  fn begin_public_stop(&self) -> Option<PublicStopGuard<'_>> {
    let mut gate = self
      .public_stop_gate
      .lock()
      .expect("public stop gate poisoned");
    if gate.closed {
      return None;
    }
    gate.active += 1;
    Some(PublicStopGuard(&self.public_stop_gate))
  }

  /// Probe the adapter after a read failure before clearing its degraded state.
  pub fn refresh_node(&self, node_path: &str) -> Result<(), String> {
    let entry = self
      .registry
      .nodes
      .get(&canonical_node_path(node_path).unwrap_or_default())
      .ok_or_else(|| format!("unknown node: {node_path}"))?;
    let reported = match &entry.adapter {
      AdapterRegistration::Camera { adapter, .. } => (adapter.status(), adapter.health()),
      AdapterRegistration::Base { adapter, .. } => (adapter.status(), adapter.health()),
      AdapterRegistration::Manipulator { adapter, .. } => (adapter.status(), adapter.health()),
    };
    let readable = reported.0.state == NodeState::Ready
      || (reported.0.state == NodeState::Stopped
        && !matches!(&entry.adapter, AdapterRegistration::Camera { .. }));
    if !readable || reported.1.state != HealthState::Healthy {
      return Err("adapter is not readable and healthy".into());
    }
    let stale_ns = entry
      .profile
      .health_timing
      .stale_after_ms
      .saturating_mul(1_000_000);
    if reported
      .1
      .last_ok_monotonic_ns
      .is_none_or(|last| !fresh(self.clock.monotonic_ns(), last, stale_ns))
    {
      return Err("adapter health is stale".into());
    }
    let checked = match &entry.adapter {
      AdapterRegistration::Camera { adapter, .. } => adapter
        .capture()
        .map_err(|error| error.to_string())
        .and_then(|frame| {
          frame
            .validate(
              entry
                .profile
                .observation
                .as_ref()
                .expect("camera bounds")
                .max_payload_bytes,
            )
            .map_err(|error| error.to_string())?;
          if frame.meta.frame_id != entry.profile.frame_id
            || !fresh(
              self.clock.monotonic_ns(),
              frame.meta.received_monotonic_ns,
              stale_ns,
            )
          {
            return Err("camera frame is invalid or stale".into());
          }
          Ok(())
        }),
      AdapterRegistration::Base { adapter, .. } => adapter
        .observe()
        .map_err(|error| error.to_string())
        .and_then(|observation| {
          if !valid_base_observation(&observation, &entry.profile.frame_id)
            || !fresh(
              self.clock.monotonic_ns(),
              observation.meta.received_monotonic_ns,
              stale_ns,
            )
          {
            return Err("base observation is invalid or stale".into());
          }
          Ok(())
        }),
      AdapterRegistration::Manipulator { adapter, .. } => adapter
        .observe()
        .map_err(|error| error.to_string())
        .and_then(|observation| {
          if !valid_manipulator_observation(
            &observation,
            &entry.profile.frame_id,
            entry
              .profile
              .joint_names
              .as_deref()
              .expect("manipulator joint names"),
          ) || !fresh(
            self.clock.monotonic_ns(),
            observation.meta.received_monotonic_ns,
            stale_ns,
          ) {
            return Err("manipulator observation is invalid or stale".into());
          }
          Ok(())
        }),
    };
    match checked {
      Ok(()) => {
        entry.clear_read_fault();
        Ok(())
      }
      Err(reason) => {
        entry.record_read_fault(reason.clone());
        Err(reason)
      }
    }
  }

  /// Read and validate a camera sample within the node's configured payload bound.
  pub fn capture_camera(&self, node_path: &str) -> Result<CameraFrame, String> {
    let entry = self
      .registry
      .nodes
      .get(&canonical_node_path(node_path).unwrap_or_default())
      .ok_or_else(|| format!("unknown node: {node_path}"))?;
    let AdapterRegistration::Camera { adapter, .. } = &entry.adapter else {
      return Err(format!("node {node_path} is not a camera"));
    };
    if adapter.status().state != NodeState::Ready {
      return Err("camera is not ready".into());
    }
    let health = entry.health();
    let now = self.clock.monotonic_ns();
    let stale_ns = entry
      .profile
      .health_timing
      .stale_after_ms
      .saturating_mul(1_000_000);
    if health.state != HealthState::Healthy
      || health
        .last_ok_monotonic_ns
        .is_none_or(|last| !fresh(now, last, stale_ns))
    {
      return Err("camera health is unavailable or stale".into());
    }
    let frame = adapter.capture().map_err(|error| {
      let reason = error.to_string();
      entry.record_read_fault(reason.clone());
      reason
    })?;
    let observation = entry
      .profile
      .observation
      .as_ref()
      .expect("camera profile requires observation bounds");
    frame
      .validate(observation.max_payload_bytes)
      .map_err(|error| {
        let reason = error.to_string();
        entry.record_read_fault(reason.clone());
        reason
      })?;
    if frame.meta.frame_id != entry.profile.frame_id {
      entry.record_read_fault("camera frame ID disagrees with profile".into());
      return Err("camera frame ID disagrees with profile".into());
    }
    if !fresh(
      self.clock.monotonic_ns(),
      frame.meta.received_monotonic_ns,
      stale_ns,
    ) {
      entry.record_read_fault("camera frame is stale".into());
      return Err("camera frame is stale".into());
    }
    Ok(frame)
  }

  /// Read a typed base sample after a fresh health and sample check.
  pub fn observe_base(&self, node_path: &str) -> Result<aether_hal::BaseObservation, String> {
    self.refresh_node(node_path)?;
    let entry = self
      .registry
      .nodes
      .get(&canonical_node_path(node_path).unwrap_or_default())
      .ok_or_else(|| format!("unknown node: {node_path}"))?;
    let AdapterRegistration::Base { adapter, .. } = &entry.adapter else {
      return Err(format!("node {node_path} is not a base"));
    };
    let observation = adapter.observe().map_err(|error| {
      let reason = error.to_string();
      entry.record_read_fault(reason.clone());
      reason
    })?;
    let stale_ns = entry
      .profile
      .health_timing
      .stale_after_ms
      .saturating_mul(1_000_000);
    if !valid_base_observation(&observation, &entry.profile.frame_id)
      || !fresh(
        self.clock.monotonic_ns(),
        observation.meta.received_monotonic_ns,
        stale_ns,
      )
    {
      entry.record_read_fault("base observation is invalid or stale".into());
      return Err("base observation is invalid or stale".into());
    }
    Ok(observation)
  }

  /// Read a typed manipulator sample after a fresh health and sample check.
  pub fn observe_manipulator(
    &self,
    node_path: &str,
  ) -> Result<aether_hal::ManipulatorObservation, String> {
    self.refresh_node(node_path)?;
    let entry = self
      .registry
      .nodes
      .get(&canonical_node_path(node_path).unwrap_or_default())
      .ok_or_else(|| format!("unknown node: {node_path}"))?;
    let AdapterRegistration::Manipulator { adapter, .. } = &entry.adapter else {
      return Err(format!("node {node_path} is not a manipulator"));
    };
    let observation = adapter.observe().map_err(|error| {
      let reason = error.to_string();
      entry.record_read_fault(reason.clone());
      reason
    })?;
    let stale_ns = entry
      .profile
      .health_timing
      .stale_after_ms
      .saturating_mul(1_000_000);
    if !valid_manipulator_observation(
      &observation,
      &entry.profile.frame_id,
      entry
        .profile
        .joint_names
        .as_deref()
        .expect("manipulator joint names"),
    ) || !fresh(
      self.clock.monotonic_ns(),
      observation.meta.received_monotonic_ns,
      stale_ns,
    ) {
      entry.record_read_fault("manipulator observation is invalid or stale".into());
      return Err("manipulator observation is invalid or stale".into());
    }
    Ok(observation)
  }

  pub fn result(&self, command_id: &str) -> Option<DispatchResult> {
    if self.managed.is_some() {
      return None;
    }
    self.result_inner(command_id)
  }

  fn result_inner(&self, command_id: &str) -> Option<DispatchResult> {
    self
      .journal
      .lock()
      .expect("journal poisoned")
      .get(command_id)
      .map(|entry| entry.result.clone())
  }

  /// Read one browser-admitted movement without exposing another ingress or
  /// generation. The identity check and result clone share the journal lock.
  pub fn browser_result_authorized(
    &self,
    command_id: &str,
    run_id: &str,
  ) -> Option<DispatchResult> {
    self
      .browser_result_authorized_checked(command_id, run_id)
      .ok()
      .flatten()
  }

  /// An unavailable live record must not expose a stale durable InProgress
  /// result after a terminal write fault.
  pub fn browser_result_authorized_checked(
    &self,
    command_id: &str,
    run_id: &str,
  ) -> Result<Option<DispatchResult>, BrowserLookupError> {
    let Some(managed) = self.managed.as_ref() else {
      return Ok(None);
    };
    if managed.run_id != run_id {
      return Ok(None);
    }
    let journal = self.journal.lock().expect("journal poisoned");
    let Some(entry) = journal.get(command_id) else {
      return Ok(None);
    };
    let Some(identity) = entry.operation.as_ref().map(|op| &op.identity) else {
      return Ok(None);
    };
    let Some(admission) = identity.admission.as_ref() else {
      return Ok(None);
    };
    if identity.kind != OperationKind::Movement
      || admission.run_id != run_id
      || admission.active_revision.as_deref() != Some(managed.active_revision.as_str())
      || admission.authority_role != AuthorityRole::OperatorControl
      || admission.ingress != AdmissionIngress::PrivateBridge
    {
      return Ok(None);
    }
    if self
      .browser_lane
      .lock()
      .expect("browser lane poisoned")
      .unavailable
      .contains(command_id)
    {
      return Err(BrowserLookupError::Unavailable);
    }
    Ok(Some(entry.result.clone()))
  }

  pub fn gateway_operation(
    &self,
    command_id: &str,
  ) -> Result<Option<GatewayOperationInfo>, GatewayLookupError> {
    if self.managed.is_some() {
      return Err(GatewayLookupError::Unauthorized);
    }
    self.gateway_operation_inner(command_id, None)
  }

  pub fn gateway_operation_authorized(
    &self,
    command_id: &str,
    run_id: &str,
    role: AuthorityRole,
  ) -> Result<Option<GatewayOperationInfo>, GatewayLookupError> {
    self.gateway_operation_inner(command_id, Some((run_id, role)))
  }

  fn gateway_operation_inner(
    &self,
    command_id: &str,
    authority: Option<(&str, AuthorityRole)>,
  ) -> Result<Option<GatewayOperationInfo>, GatewayLookupError> {
    let journal = self.journal.lock().expect("journal poisoned");
    let Some(entry) = journal.get(command_id) else {
      return Ok(None);
    };
    let Some(operation) = &entry.operation else {
      return Err(GatewayLookupError::LegacyRecord);
    };
    if let Some((run_id, role)) = authority {
      let Some(identity) = &operation.identity.admission else {
        return Err(GatewayLookupError::LegacyRecord);
      };
      if identity.run_id != run_id {
        return Err(GatewayLookupError::WrongRun);
      }
      if identity.authority_role != role {
        return Err(GatewayLookupError::Unauthorized);
      }
    }
    let target = match &operation.identity.target {
      JournalTarget::Hardware { node_path } => GatewayTarget::Hardware {
        node_path: node_path.clone(),
      },
      JournalTarget::Robot { robot_id } => GatewayTarget::Robot {
        robot_id: robot_id.clone(),
      },
    };
    let kind = match operation.identity.kind {
      OperationKind::Movement => GatewayOperationKind::Movement,
      OperationKind::MotionStop => GatewayOperationKind::MotionStop,
      OperationKind::RobotStop => GatewayOperationKind::RobotStop,
      OperationKind::SafetyEstop => GatewayOperationKind::SafetyEstop,
    };
    let parts = operation
      .parts
      .iter()
      .map(|part| StopPartInfo {
        node_path: part.node_path.clone(),
        status: part.status.unwrap_or(OutcomeStatus::OutcomeUnknown),
        reason: part.reason.clone(),
      })
      .collect();
    Ok(Some(GatewayOperationInfo {
      command_id: command_id.into(),
      primitive: operation.identity.primitive.clone(),
      version: operation.identity.version.clone(),
      node_path: match &target {
        GatewayTarget::Hardware { node_path } => node_path.clone(),
        GatewayTarget::Robot { .. } => String::new(),
      },
      status: entry.result.status,
      reason: entry.result.reason.clone(),
      accepted: operation.accepted,
      rejection_code: operation.rejection_code,
      kind,
      target,
      parts,
    }))
  }

  /// The only public movement reservation path. The typed identity is stored
  /// in the same durable record before an adapter can be called.
  pub fn dispatch_gateway_movement(
    &self,
    request: CommandRequest,
    primitive: &str,
  ) -> DispatchResult {
    if !matches!(
      primitive,
      "base.velocity" | "base.navigate" | "arm.movej" | "arm.movel"
    ) {
      return DispatchResult::new(
        &request.meta.command_id,
        OutcomeStatus::Rejected,
        Some("unsupported primitive".into()),
      );
    }
    let operation = GatewayOperation {
      identity: GatewayIdentity {
        primitive: primitive.into(),
        version: "1.0.0".into(),
        target: JournalTarget::Hardware {
          node_path: canonical_node_path(&request.node_path).unwrap_or_default(),
        },
        kind: OperationKind::Movement,
        admission: None,
        recovery: false,
      },
      accepted: None,
      rejection_code: None,
      parts: Vec::new(),
      fenced: false,
    };
    if self.managed.is_some() {
      return DispatchResult::new(
        &request.meta.command_id,
        OutcomeStatus::Rejected,
        Some("managed generation requires governed admission".into()),
      );
    }
    self.dispatch_inner(request, Some(operation), None)
  }

  pub fn dispatch(&self, request: CommandRequest) -> DispatchResult {
    if self.managed.is_some() {
      return DispatchResult::new(
        &request.meta.command_id,
        OutcomeStatus::Rejected,
        Some("managed generation requires governed admission".into()),
      );
    }
    self.dispatch_inner(request, None, None)
  }

  pub fn dispatch_managed_movement(
    &self,
    admission: AdmissionContext,
    request: CommandRequest,
  ) -> DispatchResult {
    self.dispatch_managed_movement_inner(admission, request, None)
  }

  /// A native skill's invocation revocation is checked under the durable
  /// journal lock at the last pre-release boundary. Cancellation can race
  /// reservation safely without holding a manager lock over adapter I/O.
  pub fn dispatch_skill_movement(
    &self,
    admission: AdmissionContext,
    request: CommandRequest,
    revoked: &AtomicBool,
  ) -> DispatchResult {
    if admission.ingress != AdmissionIngress::SkillWorker {
      return DispatchResult::new(
        &request.meta.command_id,
        OutcomeStatus::Rejected,
        Some("skill ingress required".into()),
      );
    }
    self.dispatch_managed_movement_inner(admission, request, Some(revoked))
  }

  fn dispatch_managed_movement_inner(
    &self,
    admission: AdmissionContext,
    request: CommandRequest,
    revoked: Option<&AtomicBool>,
  ) -> DispatchResult {
    let id = request.meta.command_id.clone();
    let Some(_) = &self.managed else {
      return DispatchResult::new(
        &id,
        OutcomeStatus::Rejected,
        Some("managed generation unavailable".into()),
      );
    };
    let operation = GatewayOperation {
      identity: GatewayIdentity {
        primitive: admission.primitive.clone(),
        version: admission.primitive_version.clone(),
        target: JournalTarget::Hardware {
          node_path: canonical_node_path(&request.node_path).unwrap_or_default(),
        },
        kind: OperationKind::Movement,
        admission: Some(admission),
        recovery: false,
      },
      accepted: None,
      rejection_code: None,
      parts: Vec::new(),
      fenced: false,
    };
    self.dispatch_inner(request, Some(operation), revoked)
  }

  /// Browser submission acknowledges a synced Prepared record and never waits
  /// for the adapter. The worker retains this Dispatcher and its journal lock.
  pub fn submit_browser_movement(
    self: &Arc<Self>,
    admission: AdmissionContext,
    mut request: CommandRequest,
  ) -> Result<DispatchResult, String> {
    if admission.authority_role != AuthorityRole::OperatorControl
      || admission.ingress != AdmissionIngress::PrivateBridge
    {
      return Err("operator bridge admission required".into());
    }
    if admission.delivery_mode == MovementDelivery::Stream
      && !matches!(
        &request.movement,
        Movement::Base(BaseMovement::BodyVelocity { .. })
      )
    {
      return Err("stream delivery requires body velocity".into());
    }
    if let Some(path) = canonical_node_path(&request.node_path) {
      request.node_path = path;
    }
    let id = request.meta.command_id.clone();
    if id.trim().is_empty() || id.len() > 128 {
      return Err("invalid command ID".into());
    }
    let operation = GatewayOperation {
      identity: GatewayIdentity {
        primitive: admission.primitive.clone(),
        version: admission.primitive_version.clone(),
        target: JournalTarget::Hardware {
          node_path: request.node_path.clone(),
        },
        kind: OperationKind::Movement,
        admission: Some(admission.clone()),
        recovery: false,
      },
      accepted: None,
      rejection_code: None,
      parts: Vec::new(),
      fenced: false,
    };
    let signature = browser_movement_signature(&request, &operation)?;
    {
      let mut journal = self.journal.lock().expect("journal poisoned");
      if let Some(entry) = journal.get(&id) {
        if entry.signature != signature {
          return Ok(DispatchResult::new(
            &id,
            OutcomeStatus::Rejected,
            Some("conflicting command ID reuse".into()),
          ));
        }
        self.validate_managed_movement(&admission, &request)?;
        if self
          .browser_lane
          .lock()
          .expect("browser lane poisoned")
          .unavailable
          .contains(&id)
        {
          return Err("result unavailable".into());
        }
        return Ok(entry.result.clone());
      }
      self.validate_managed_movement(&admission, &request)?;
      if self.quiescing.load(Ordering::SeqCst) {
        return Err("runtime is quiescing".into());
      }
      if self.clock.monotonic_ns() >= request.meta.deadline_monotonic_ns {
        return Err("command deadline expired".into());
      }
      self.validate_browser_payload(&request)?;
      let mut lane = self.browser_lane.lock().expect("browser lane poisoned");
      if lane.active.len() >= MAX_BROWSER_MOVEMENTS {
        let result = DispatchResult::new(
          &id,
          OutcomeStatus::Rejected,
          Some("browser movement capacity exhausted".into()),
        );
        let mut rejected = operation;
        rejected.accepted = Some(false);
        rejected.rejection_code = Some(GatewayRejectionCode::InvalidState);
        journal
          .insert_terminal_rejection(
            id.clone(),
            JournalEntry {
              node_path: request.node_path.clone(),
              signature,
              phase: CommandPhase::Terminal,
              result: result.clone(),
              operation: Some(rejected),
            },
          )
          .map_err(|_| "movement capacity unavailable".to_string())?;
        return Ok(result);
      }
      lane.active.insert(id.clone());
      if journal
        .insert(
          id.clone(),
          JournalEntry {
            node_path: request.node_path.clone(),
            signature,
            phase: CommandPhase::Prepared,
            result: DispatchResult::new(&id, OutcomeStatus::InProgress, None),
            operation: Some(operation),
          },
        )
        .is_err()
      {
        lane.active.remove(&id);
        return Err("result unavailable".into());
      }
    }
    let worker = self.clone();
    let worker_id = id.clone();
    #[cfg(test)]
    let fail_spawn = self.browser_spawn_fail.swap(false, Ordering::SeqCst);
    #[cfg(not(test))]
    let fail_spawn = false;
    let spawned = if fail_spawn {
      false
    } else {
      std::thread::Builder::new()
        .name("aether-browser-movement".into())
        .spawn(move || {
          worker.run_prepared_browser(request, admission);
        })
        .is_ok()
    };
    if !spawned {
      let mut journal = self.journal.lock().expect("journal poisoned");
      let phase = journal.get(&id).map(|entry| entry.phase);
      if phase == Some(CommandPhase::Prepared) {
        let sealed = journal.update(&id, |entry| {
          entry.phase = CommandPhase::Terminal;
          entry.result = DispatchResult::new(
            &worker_id,
            OutcomeStatus::Rejected,
            Some("browser worker unavailable".into()),
          );
          if let Some(operation) = &mut entry.operation {
            operation.accepted = Some(false);
            operation.rejection_code = Some(GatewayRejectionCode::Internal);
          }
        });
        if sealed.is_err() {
          self.browser_fault(&worker_id);
          return Err("result unavailable".into());
        }
      }
      self
        .browser_lane
        .lock()
        .expect("browser lane poisoned")
        .active
        .remove(&id);
      return Ok(
        journal
          .get(&id)
          .expect("reserved browser ID")
          .result
          .clone(),
      );
    }
    Ok(DispatchResult::new(&id, OutcomeStatus::InProgress, None))
  }

  fn validate_browser_payload(&self, request: &CommandRequest) -> Result<(), String> {
    let entry = self
      .registry
      .nodes
      .get(&request.node_path)
      .ok_or("unknown node")?;
    let profile = entry
      .profile
      .movement
      .as_ref()
      .ok_or("node has no movement interface")?;
    match (&request.movement, &entry.adapter) {
      (Movement::Base(movement), AdapterRegistration::Base { .. }) => {
        validate_base(movement, profile, &entry.profile.frame_id)?;
        if let BaseMovement::BodyVelocity { valid_for_ms, .. } = movement {
          let remaining_ns = request
            .meta
            .deadline_monotonic_ns
            .saturating_sub(self.clock.monotonic_ns());
          if *valid_for_ms > remaining_ns / 1_000_000 {
            return Err("velocity lease exceeds command deadline".into());
          }
        }
      }
      (
        Movement::Manipulator(ManipulatorMovement::JointPositions {
          joint_names,
          joint_positions_rad,
          max_speed_rad_s,
        }),
        AdapterRegistration::Manipulator { .. },
      ) => {
        let limits = &profile.limits;
        if !profile.modes.iter().any(|mode| mode == "joint_positions")
          || entry.profile.joint_names.as_deref() != Some(joint_names.as_slice())
          || joint_names.is_empty()
          || joint_positions_rad.len() != joint_names.len()
          || !joint_positions_rad
            .iter()
            .all(|value| bounded(*value, limits.max_joint_position_rad))
          || max_speed_rad_s.is_some_and(|speed| {
            !speed.is_finite()
              || speed <= 0.0
              || limits.max_joint_speed_rad_s.is_some_and(|max| speed > max)
          })
        {
          return Err("joint targets are invalid or exceed limits".into());
        }
      }
      (
        Movement::Manipulator(ManipulatorMovement::EndEffectorPose {
          pose,
          max_speed_m_s,
        }),
        AdapterRegistration::Manipulator { .. },
      ) => {
        let limits = &profile.limits;
        let qnorm = pose
          .orientation_xyzw
          .iter()
          .map(|value| value * value)
          .sum::<f64>();
        if !profile.modes.iter().any(|mode| mode == "end_effector_pose")
          || pose.frame_id != entry.profile.frame_id
          || !pose.position_m.iter().all(|value| value.is_finite())
          || !pose.orientation_xyzw.iter().all(|value| value.is_finite())
          || !(0.999..=1.001).contains(&qnorm)
          || limits.max_end_effector_translation_m.is_some_and(|max| {
            pose
              .position_m
              .iter()
              .map(|value| value * value)
              .sum::<f64>()
              .sqrt()
              > max
          })
          || max_speed_m_s.is_some_and(|speed| {
            !speed.is_finite()
              || speed <= 0.0
              || limits
                .max_end_effector_speed_m_s
                .is_some_and(|max| speed > max)
          })
        {
          return Err("end-effector target is invalid or exceeds limits".into());
        }
      }
      _ => return Err("movement does not match node kind".into()),
    }
    Ok(())
  }

  fn browser_fault(&self, id: &str) {
    self.movement_inhibited.store(true, Ordering::SeqCst);
    self
      .browser_lane
      .lock()
      .expect("browser lane poisoned")
      .unavailable
      .insert(id.into());
  }

  fn run_prepared_browser(&self, request: CommandRequest, admission: AdmissionContext) {
    let id = request.meta.command_id.clone();
    let result = self.admit_and_release(&request, Some(&admission), None);
    let mut journal = self.journal.lock().expect("journal poisoned");
    let Some(phase) = journal.get(&id).map(|entry| entry.phase) else {
      return;
    };
    if phase == CommandPhase::Terminal {
      self
        .browser_lane
        .lock()
        .expect("browser lane poisoned")
        .active
        .remove(&id);
      return;
    }
    let accepted = phase == CommandPhase::Releasing;
    let result = if accepted && self.quiescing.load(Ordering::SeqCst) {
      self.movement_inhibited.store(true, Ordering::SeqCst);
      DispatchResult::new(
        &id,
        OutcomeStatus::OutcomeUnknown,
        Some("movement crossed a stop fence; physical outcome unconfirmed".into()),
      )
    } else if !accepted && self.quiescing.load(Ordering::SeqCst) {
      DispatchResult::new(
        &id,
        OutcomeStatus::Cancelled,
        Some("runtime stopped before release".into()),
      )
    } else {
      result
    };
    let write = journal.update(&id, |entry| {
      entry.phase = CommandPhase::Terminal;
      entry.result = result.clone();
      if let Some(operation) = &mut entry.operation {
        operation.accepted = Some(accepted);
        operation.rejection_code =
          (result.status == OutcomeStatus::Rejected).then_some(GatewayRejectionCode::InvalidState);
      }
    });
    if write.is_err() {
      self.browser_fault(&id);
      let fallback = if accepted {
        DispatchResult::new(
          &id,
          OutcomeStatus::OutcomeUnknown,
          Some("command outcome could not be persisted".into()),
        )
      } else if self.quiescing.load(Ordering::SeqCst) {
        DispatchResult::new(
          &id,
          OutcomeStatus::Cancelled,
          Some("runtime stopped before release".into()),
        )
      } else {
        result
      };
      if journal
        .update(&id, |entry| {
          entry.phase = CommandPhase::Terminal;
          entry.result = fallback;
          if let Some(operation) = &mut entry.operation {
            operation.accepted = Some(accepted);
            operation.rejection_code = (!accepted
              && entry.result.status == OutcomeStatus::Rejected)
              .then_some(GatewayRejectionCode::Internal);
          }
        })
        .is_err()
      {
        return;
      }
    }
    let mut lane = self.browser_lane.lock().expect("browser lane poisoned");
    lane.unavailable.remove(&id);
    lane.active.remove(&id);
  }

  /// Called after the physical stop has been scheduled. Prepared records can
  /// still be cancelled; Releasing records remain owned by their worker.
  pub fn cancel_prepared_browser_after_stop(&self) {
    let mut journal = self.journal.lock().expect("journal poisoned");
    let ids: Vec<_> = self
      .browser_lane
      .lock()
      .expect("browser lane poisoned")
      .active
      .iter()
      .cloned()
      .collect();
    for id in ids {
      if journal
        .get(&id)
        .is_some_and(|entry| entry.phase == CommandPhase::Prepared)
      {
        if journal
          .update(&id, |entry| {
            entry.phase = CommandPhase::Terminal;
            entry.result = DispatchResult::new(
              &id,
              OutcomeStatus::Cancelled,
              Some("runtime stopped before release".into()),
            );
            if let Some(operation) = &mut entry.operation {
              operation.accepted = Some(false);
            }
          })
          .is_ok()
        {
          let mut lane = self.browser_lane.lock().expect("browser lane poisoned");
          lane.unavailable.remove(&id);
          lane.active.remove(&id);
        } else {
          self.browser_fault(&id);
        }
      }
    }
  }

  fn validate_managed_movement(
    &self,
    context: &AdmissionContext,
    request: &CommandRequest,
  ) -> Result<(), String> {
    let generation = self
      .managed
      .as_ref()
      .ok_or("managed generation unavailable")?;
    if context.run_id != generation.run_id {
      return Err("deployment run differs from active generation".into());
    }
    if context.active_revision.as_deref() != Some(generation.active_revision.as_str()) {
      return Err("saved active revision differs from admitted generation".into());
    }
    if !matches!(
      (context.authority_role, context.ingress),
      (AuthorityRole::EgiaControl, AdmissionIngress::Grpc)
        | (AuthorityRole::EgiaControl, AdmissionIngress::SkillWorker)
        | (
          AuthorityRole::OperatorControl,
          AdmissionIngress::PrivateBridge
        )
    ) {
      return Err("role cannot submit movement over this ingress".into());
    }
    let binding = generation
      .bindings
      .get(&context.binding_path)
      .ok_or("unknown primitive binding")?;
    if binding.node_path != request.node_path
      || binding.primitive != context.primitive
      || binding.primitive_version != context.primitive_version
    {
      return Err("command differs from exact admitted primitive binding".into());
    }
    match (&request.movement, context.primitive.as_str()) {
      (Movement::Base(BaseMovement::BodyVelocity { .. }), "base.velocity")
      | (Movement::Base(BaseMovement::RelativePose { .. }), "base.navigate")
      | (Movement::Manipulator(ManipulatorMovement::JointPositions { .. }), "arm.movej")
      | (Movement::Manipulator(ManipulatorMovement::EndEffectorPose { .. }), "arm.movel") => Ok(()),
      _ => Err("movement mode differs from primitive binding".into()),
    }
  }

  fn dispatch_inner(
    &self,
    mut request: CommandRequest,
    operation: Option<GatewayOperation>,
    revoked: Option<&AtomicBool>,
  ) -> DispatchResult {
    if let Some(path) = canonical_node_path(&request.node_path) {
      request.node_path = path;
    }
    let id = request.meta.command_id.clone();
    if id.trim().is_empty() || id.len() > 128 {
      return DispatchResult::new(
        &id,
        OutcomeStatus::Rejected,
        Some("command ID is empty or exceeds 128 bytes".into()),
      );
    }
    let signature = match movement_signature(&request, operation.as_ref()) {
      Ok(signature) => signature,
      Err(error) => {
        return DispatchResult::new(
          &id,
          OutcomeStatus::Rejected,
          Some(format!("command signature could not be encoded: {error}")),
        )
      }
    };
    let context = operation
      .as_ref()
      .and_then(|op| op.identity.admission.clone());
    // Reserve before any validation or hardware release; duplicate IDs cannot release twice.
    {
      let mut journal = self.journal.lock().expect("journal poisoned");
      if let Some(entry) = journal.get(&id) {
        if entry.signature != signature {
          return DispatchResult::new(
            &id,
            OutcomeStatus::Rejected,
            Some("conflicting command ID reuse".into()),
          );
        }
        if let Some(context) = context.as_ref() {
          if let Err(reason) = self.validate_managed_movement(context, &request) {
            return DispatchResult::new(&id, OutcomeStatus::Rejected, Some(reason));
          }
        }
        return entry.result.clone();
      }
      if let Some(context) = context.as_ref() {
        if let Err(reason) = self.validate_managed_movement(context, &request) {
          return DispatchResult::new(&id, OutcomeStatus::Rejected, Some(reason));
        }
      }
      if let Err(error) = journal.insert(
        id.clone(),
        JournalEntry {
          node_path: request.node_path.clone(),
          signature,
          phase: CommandPhase::Prepared,
          result: DispatchResult::new(&id, OutcomeStatus::InProgress, None),
          operation,
        },
      ) {
        return DispatchResult::new(
          &id,
          OutcomeStatus::Rejected,
          Some(format!(
            "command reservation could not be persisted: {error}"
          )),
        );
      }
      self
        .managed_stop_kicks
        .lock()
        .expect("managed stop kicks poisoned")
        .reserve_existing_id(id.clone());
    }
    #[cfg(test)]
    {
      let pause = {
        let mut hook = self
          .reservation_pause
          .lock()
          .expect("test reservation pause poisoned");
        if hook.as_ref().is_some_and(|pause| pause.command_id == id) {
          hook.take()
        } else {
          None
        }
      };
      if let Some(pause) = pause {
        let _ = pause.entered.send(());
        let _ = pause.release.recv();
      }
    }
    let result = self.admit_and_release(&request, context.as_ref(), revoked);
    let mut journal = self.journal.lock().expect("journal poisoned");
    if journal.get(&id).expect("reserved command ID missing").phase != CommandPhase::Terminal {
      let accepted = journal
        .get(&id)
        .is_some_and(|entry| entry.phase == CommandPhase::Releasing);
      if let Err(error) = journal.update(&id, |entry| {
        entry.result = result;
        entry.phase = CommandPhase::Terminal;
        if let Some(operation) = &mut entry.operation {
          operation.accepted = Some(accepted);
          operation.rejection_code = (entry.result.status == OutcomeStatus::Rejected)
            .then_some(GatewayRejectionCode::InvalidState);
        }
      }) {
        let reason = format!("command outcome could not be persisted: {error}");
        journal.mark_uncertain_in_memory(&id, reason);
      }
    }
    let final_result = journal
      .get(&id)
      .expect("reserved command ID missing")
      .result
      .clone();
    if final_result.status == OutcomeStatus::OutcomeUnknown {
      self.movement_inhibited.store(true, Ordering::SeqCst);
    }
    final_result
  }

  fn admit_and_release(
    &self,
    request: &CommandRequest,
    context: Option<&AdmissionContext>,
    revoked: Option<&AtomicBool>,
  ) -> DispatchResult {
    // This permit spans admission, the physical call, and the uncertain-result
    // latch. A second release cannot observe the gap after an adapter returns.
    let id = &request.meta.command_id;
    let reject = |reason: String| DispatchResult::new(id, OutcomeStatus::Rejected, Some(reason));
    let remaining_ns = request
      .meta
      .deadline_monotonic_ns
      .saturating_sub(self.clock.monotonic_ns());
    let gate_deadline = Instant::now() + Duration::from_nanos(remaining_ns.min(5_000_000_000));
    let Some(_permit) = self.release_gate.movement_permit(gate_deadline) else {
      return reject("command deadline expired waiting for release gate".into());
    };
    if self.emergency_stopped.load(Ordering::SeqCst) {
      return reject("emergency stop is latched".into());
    }
    if self.quiescing.load(Ordering::SeqCst) {
      return reject("runtime is quiescing".into());
    }
    if self.movement_inhibited() {
      return reject("movement is inhibited after an uncertain physical release".into());
    }
    if self.clock.monotonic_ns() >= request.meta.deadline_monotonic_ns {
      return reject("command deadline expired".into());
    }
    let Some(entry) = self.registry.nodes.get(&request.node_path) else {
      return reject("unknown node".into());
    };
    let _operation = entry.operation_lock.lock().expect("node lock poisoned");
    if let Some(context) = context {
      if let Err(reason) = self.validate_managed_movement(context, request) {
        return reject(reason);
      }
    }
    if self.emergency_stopped.load(Ordering::SeqCst) {
      return reject("emergency stop is latched".into());
    }
    if self.quiescing.load(Ordering::SeqCst) {
      return reject("runtime is quiescing".into());
    }
    if self.movement_inhibited() {
      return reject("movement is inhibited after an uncertain physical release".into());
    }
    if self.clock.monotonic_ns() >= request.meta.deadline_monotonic_ns {
      return reject("command deadline expired while queued".into());
    }
    let status = match &entry.adapter {
      AdapterRegistration::Camera { adapter, .. } => adapter.status(),
      AdapterRegistration::Base { adapter, .. } => adapter.status(),
      AdapterRegistration::Manipulator { adapter, .. } => adapter.status(),
    };
    let health = entry.health();
    if status.state != NodeState::Ready {
      return reject("node is not ready".into());
    }
    if health.state != HealthState::Healthy {
      return reject("node health is not healthy".into());
    }
    let stale_ns = entry
      .profile
      .health_timing
      .stale_after_ms
      .saturating_mul(1_000_000);
    let now = self.clock.monotonic_ns();
    if health
      .last_ok_monotonic_ns
      .is_none_or(|last| last > now || now.saturating_sub(last) > stale_ns)
    {
      return reject("node health is stale".into());
    }
    let Some(movement_profile) = &entry.profile.movement else {
      return reject("node has no movement interface".into());
    };
    // Capture before the durable Releasing write. A stop can advance the
    // child epoch while that write or an ordinary socket request is stalled.
    let release_token = match &entry.adapter {
      AdapterRegistration::Base { adapter, .. } => adapter.movement_fence_token(),
      AdapterRegistration::Manipulator { adapter, .. } => adapter.movement_fence_token(),
      AdapterRegistration::Camera { .. } => None,
    };
    let release = match (&entry.adapter, &request.movement) {
      (AdapterRegistration::Base { adapter, .. }, Movement::Base(movement)) => {
        let observation = match adapter.observe() {
          Ok(obs) => obs,
          Err(error) => {
            let reason = format!("base observation failed: {error}");
            entry.record_read_fault(reason.clone());
            return reject(reason);
          }
        };
        if !valid_base_observation(&observation, &entry.profile.frame_id)
          || !fresh(
            self.clock.monotonic_ns(),
            observation.meta.received_monotonic_ns,
            stale_ns,
          )
        {
          entry.record_read_fault("base observation is invalid or stale".into());
          return reject("base observation is invalid or stale".into());
        }
        if let Err(reason) = validate_base(movement, movement_profile, &entry.profile.frame_id) {
          return reject(reason);
        }
        if let BaseMovement::BodyVelocity { valid_for_ms, .. } = movement {
          let remaining_ns = request
            .meta
            .deadline_monotonic_ns
            .saturating_sub(self.clock.monotonic_ns());
          if *valid_for_ms > remaining_ns / 1_000_000 {
            return reject("velocity lease exceeds command deadline".into());
          }
        }
        if let Err(reason) = self.begin_release(
          id,
          request.meta.deadline_monotonic_ns,
          context,
          request,
          revoked,
        ) {
          return reject(reason);
        }
        if context.is_some_and(|admission| admission.delivery_mode == MovementDelivery::Stream) {
          adapter.publish_base_stream_fenced(
            request.meta.clone(),
            movement.clone(),
            release_token.clone(),
          )
        } else {
          adapter.move_base_fenced(
            request.meta.clone(),
            movement.clone(),
            release_token.clone(),
          )
        }
      }
      (AdapterRegistration::Manipulator { adapter, .. }, Movement::Manipulator(movement)) => {
        let observation = match adapter.observe() {
          Ok(obs) => obs,
          Err(error) => {
            let reason = format!("manipulator observation failed: {error}");
            entry.record_read_fault(reason.clone());
            return reject(reason);
          }
        };
        if !fresh(
          self.clock.monotonic_ns(),
          observation.meta.received_monotonic_ns,
          stale_ns,
        ) {
          entry.record_read_fault("manipulator observation is stale".into());
          return reject("manipulator observation is stale".into());
        }
        if !valid_manipulator_observation(
          &observation,
          &entry.profile.frame_id,
          entry
            .profile
            .joint_names
            .as_deref()
            .expect("manipulator profile requires joint names"),
        ) {
          entry.record_read_fault("manipulator observation is malformed".into());
          return reject("manipulator observation is malformed".into());
        }
        if let Err(reason) = validate_manipulator(
          movement,
          movement_profile,
          &observation,
          &entry.profile.frame_id,
        ) {
          return reject(reason);
        }
        if let Err(reason) = self.begin_release(
          id,
          request.meta.deadline_monotonic_ns,
          context,
          request,
          revoked,
        ) {
          return reject(reason);
        }
        adapter.move_manipulator_fenced(
          request.meta.clone(),
          movement.clone(),
          release_token.clone(),
        )
      }
      _ => return reject("movement does not match node kind".into()),
    };
    let fence_changed = release_token.as_ref().is_some_and(|captured| {
      let current = match &entry.adapter {
        AdapterRegistration::Base { adapter, .. } => adapter.movement_fence_token(),
        AdapterRegistration::Manipulator { adapter, .. } => adapter.movement_fence_token(),
        AdapterRegistration::Camera { .. } => None,
      };
      current.as_ref() != Some(captured)
    });
    let result = if fence_changed
      || self.quiescing.load(Ordering::SeqCst)
      || self.emergency_stopped.load(Ordering::SeqCst)
    {
      DispatchResult::new(
        id,
        OutcomeStatus::OutcomeUnknown,
        Some("movement crossed a stop fence; physical outcome unconfirmed".into()),
      )
    } else {
      match release {
        Ok(CommandOutcome::Completed) => DispatchResult::new(id, OutcomeStatus::Completed, None),
        Ok(CommandOutcome::Accepted)
          if context
            .is_some_and(|admission| admission.delivery_mode == MovementDelivery::Stream) =>
        {
          DispatchResult::new(
            id,
            OutcomeStatus::Completed,
            Some("stream lease published".into()),
          )
        }
        Ok(CommandOutcome::Cancelled) => DispatchResult::new(id, OutcomeStatus::Cancelled, None),
        Ok(CommandOutcome::Accepted | CommandOutcome::Ambiguous) => DispatchResult::new(
          id,
          OutcomeStatus::OutcomeUnknown,
          Some("adapter did not confirm a terminal outcome".into()),
        ),
        Err(error) => DispatchResult::new(
          id,
          OutcomeStatus::OutcomeUnknown,
          Some(format!("adapter release failed: {error}")),
        ),
      }
    };
    if result.status == OutcomeStatus::OutcomeUnknown {
      self.movement_inhibited.store(true, Ordering::SeqCst);
    }
    result
  }

  fn begin_release(
    &self,
    command_id: &str,
    deadline_ns: u64,
    context: Option<&AdmissionContext>,
    request: &CommandRequest,
    revoked: Option<&AtomicBool>,
  ) -> Result<(), String> {
    let mut journal = self.journal.lock().expect("journal poisoned");
    let entry = journal
      .get(command_id)
      .expect("reserved command ID missing");
    if entry.phase != CommandPhase::Prepared {
      return Err("command cancelled before release".into());
    }
    if revoked.is_some_and(|token| token.load(Ordering::Acquire)) {
      return Err("skill invocation revoked before release".into());
    }
    if self
      .browser_lane
      .lock()
      .expect("browser lane poisoned")
      .unavailable
      .contains(command_id)
    {
      return Err("result unavailable".into());
    }
    if self.emergency_stopped.load(Ordering::SeqCst) {
      return Err("emergency stop is latched".into());
    }
    if self.quiescing.load(Ordering::SeqCst) {
      return Err("runtime is quiescing".into());
    }
    if self.movement_inhibited() {
      return Err("movement is inhibited after an uncertain physical release".into());
    }
    if self.clock.monotonic_ns() >= deadline_ns {
      return Err("command deadline expired before release".into());
    }
    if let Some(context) = context {
      self.validate_managed_movement(context, request)?;
    }
    #[cfg(any(test, feature = "test-hooks"))]
    if let Some((entered, release)) = self
      .journal_release_pause
      .lock()
      .expect("journal pause poisoned")
      .take()
    {
      let _ = entered.send(());
      let _ = release.recv();
    }
    journal
      .update(command_id, |entry| entry.phase = CommandPhase::Releasing)
      .map_err(|error| format!("command release could not be persisted: {error}"))?;
    drop(journal);
    #[cfg(test)]
    if let Some((entered, release)) = self
      .release_pause
      .lock()
      .expect("test release pause poisoned")
      .take()
    {
      let _ = entered.send(());
      let _ = release.recv();
    }
    Ok(())
  }

  pub fn stop_node(&self, node_path: &str) -> Result<(), String> {
    if self.managed.is_some() {
      return Err("managed generation requires governed stop".into());
    }
    self.stop_node_internal(node_path)
  }

  fn stop_node_internal(&self, node_path: &str) -> Result<(), String> {
    let entry = self
      .registry
      .nodes
      .get(&canonical_node_path(node_path).unwrap_or_default())
      .ok_or_else(|| format!("unknown node: {node_path}"))?;
    match &entry.adapter {
      AdapterRegistration::Camera { .. } => Err("camera has no safety stop".into()),
      AdapterRegistration::Base { adapter, .. } => {
        adapter.stop().map_err(|error| error.to_string())
      }
      AdapterRegistration::Manipulator { adapter, .. } => {
        adapter.stop().map_err(|error| error.to_string())
      }
    }
  }

  /// A cancellation requests a physical stop and keeps uncertain outcomes uncertain.
  pub fn cancel(&self, command_id: &str) -> Result<DispatchResult, String> {
    if self.managed.is_some() {
      return Err("managed generation requires governed cancellation".into());
    }
    self.cancel_inner(command_id)
  }

  pub fn cancel_authorized(
    &self,
    command_id: &str,
    run_id: &str,
    role: AuthorityRole,
  ) -> Result<DispatchResult, GatewayLookupError> {
    let operation = self
      .gateway_operation_authorized(command_id, run_id, role)?
      .ok_or(GatewayLookupError::NotFound)?;
    if operation.kind != GatewayOperationKind::Movement {
      return self
        .result_inner(command_id)
        .ok_or(GatewayLookupError::NotFound);
    }
    self
      .cancel_inner(command_id)
      .map_err(|_| GatewayLookupError::Internal)
  }

  fn cancel_inner(&self, command_id: &str) -> Result<DispatchResult, String> {
    let node_path = {
      let mut journal = self.journal.lock().expect("journal poisoned");
      let entry = journal
        .get(command_id)
        .ok_or_else(|| format!("unknown command: {command_id}"))?;
      match entry.phase {
        CommandPhase::Terminal => return Ok(entry.result.clone()),
        CommandPhase::Prepared => {
          journal
            .update(command_id, |entry| {
              entry.phase = CommandPhase::Terminal;
              entry.result = DispatchResult::new(command_id, OutcomeStatus::Cancelled, None);
              if let Some(operation) = &mut entry.operation {
                operation.accepted = Some(false);
              }
            })
            .map_err(|error| format!("cancellation could not be persisted: {error}"))?;
          return Ok(
            journal
              .get(command_id)
              .expect("reserved ID missing")
              .result
              .clone(),
          );
        }
        CommandPhase::Releasing => {
          self.movement_inhibited.store(true, Ordering::SeqCst);
          let node_path = entry.node_path.clone();
          if let Err(error) = journal.update(command_id, |entry| {
            entry.phase = CommandPhase::Terminal;
            entry.result = DispatchResult::new(
              command_id,
              OutcomeStatus::OutcomeUnknown,
              Some("stop requested during release; physical outcome unconfirmed".into()),
            );
            if let Some(operation) = &mut entry.operation {
              operation.accepted = Some(true);
            }
          }) {
            journal.mark_uncertain_in_memory(
              command_id,
              format!("cancellation could not be persisted: {error}"),
            );
          }
          node_path
        }
      }
    };
    let stop = self.stop_node_internal(&node_path);
    if let Err(reason) = stop {
      let mut journal = self.journal.lock().expect("journal poisoned");
      if journal
        .update(command_id, |entry| {
          entry.result.reason = Some(format!("stop failed: {reason}"));
        })
        .is_err()
      {
        journal.mark_uncertain_in_memory(command_id, format!("stop failed: {reason}"));
      }
    }
    Ok(
      self
        .result_inner(command_id)
        .expect("reserved command ID missing"),
    )
  }

  /// Latch process safety before stopping every actuator. Restart is required to clear it.
  pub fn emergency_stop(&self) -> Vec<(String, Result<(), String>)> {
    self.emergency_stopped.store(true, Ordering::SeqCst);
    self.movement_inhibited.store(true, Ordering::SeqCst);
    let targets: Vec<_> = self
      .registry
      .nodes
      .keys()
      .filter(|id| self.registry.nodes[*id].profile.movement.is_some())
      .cloned()
      .collect();
    if self.is_managed_mujoco_stop(&targets) {
      let deadline = Instant::now() + Duration::from_secs(3);
      let kick = self.start_managed_physical_stop(&targets);
      let journal = self.journal.clone();
      let owner_lock = self.stop_owner_lock.clone();
      let (sender, receiver) = std::sync::mpsc::sync_channel(1);
      let worker = std::thread::Builder::new()
        .name("aether-estop-journal".into())
        .spawn(move || {
          let _owner_lock = owner_lock;
          let durable = journal
            .lock()
            .expect("journal poisoned")
            .latch_estop()
            .is_ok();
          let _ = sender.send(durable);
        });
      let durability = worker.is_ok()
        && receiver
          .recv_timeout(deadline.saturating_duration_since(Instant::now()))
          .unwrap_or(false);
      let proof = kick.wait(deadline);
      return targets
        .into_iter()
        .map(|path| {
          let part =
            proof
              .as_ref()
              .and_then(|proof| match self.registry.nodes[&path].profile.kind {
                aether_hal::NodeKind::Base => Some(&proof.base),
                aether_hal::NodeKind::Manipulator => Some(&proof.arm),
                _ => None,
              });
          let result = if durability && part == Some(&aether_hal::ControlledStopOutcome::Stopped) {
            Ok(())
          } else {
            Err("whole-robot stop did not durably confirm".into())
          };
          (path, result)
        })
        .collect();
    }
    let _ = self.journal.lock().expect("journal poisoned").latch_estop();
    targets
      .into_iter()
      .map(|id| {
        let result = self.stop_node_internal(&id);
        (id, result)
      })
      .collect()
  }

  /// Managed routine shutdown uses the R06 preemptive physical lane but owns
  /// a separate durable fence. The caller may resolve it only after every
  /// selected receipt predicate, including backend reap and cleanup, holds.
  pub fn managed_shutdown_stop(&self) -> Option<(StopResults, bool)> {
    let mut targets: Vec<_> = self
      .registry
      .nodes
      .keys()
      .filter(|id| self.registry.nodes[*id].profile.movement.is_some())
      .cloned()
      .collect();
    targets.sort();
    if !self.is_managed_mujoco_stop(&targets) {
      return None;
    }
    self.movement_inhibited.store(true, Ordering::SeqCst);
    let deadline = Instant::now() + Duration::from_secs(3);
    let kick = self.start_managed_physical_stop(&targets);
    // A stalled journal mutex cannot delay the companion physical lane or
    // extend the selected R06 stop deadline. The worker retains journal
    // ownership even if it outlives this bounded wait.
    let journal = self.journal.clone();
    let owner_lock = self.stop_owner_lock.clone();
    let journal_targets = targets.clone();
    let (sender, receiver) = std::sync::mpsc::sync_channel(1);
    let worker = std::thread::Builder::new()
      .name("aether-shutdown-journal".into())
      .spawn(move || {
        let _owner_lock = owner_lock;
        let durable = journal
          .lock()
          .expect("journal poisoned")
          .set_shutdown_pending(journal_targets)
          .is_ok();
        let _ = sender.send(durable);
      });
    let marker_persisted = worker.is_ok()
      && receiver
        .recv_timeout(deadline.saturating_duration_since(Instant::now()))
        .unwrap_or(false);
    if marker_persisted {
      self.shutdown_marker_owned.store(true, Ordering::SeqCst);
    }
    let proof = kick.wait(deadline);
    let results = targets
      .into_iter()
      .map(|path| {
        let part = proof
          .as_ref()
          .and_then(|proof| match self.registry.nodes[&path].profile.kind {
            aether_hal::NodeKind::Base => Some(&proof.base),
            aether_hal::NodeKind::Manipulator => Some(&proof.arm),
            _ => None,
          });
        let result =
          if marker_persisted && part == Some(&aether_hal::ControlledStopOutcome::Stopped) {
            Ok(())
          } else {
            Err("managed shutdown stop did not durably confirm".into())
          };
        (path, result)
      })
      .collect();
    Some((results, marker_persisted))
  }

  /// Only the selected runtime calls this after full R06 terminal proof. It
  /// never changes a genuine estop or a preexisting RobotStop inhibit.
  pub fn resolve_managed_shutdown(&self) -> bool {
    let gate = self
      .public_stop_gate
      .lock()
      .expect("public stop gate poisoned");
    if !gate.closed
      || gate.active != 0
      || self.public_estop_durability_failed.load(Ordering::SeqCst)
      || !self.shutdown_marker_owned.load(Ordering::SeqCst)
    {
      return false;
    }
    let resolved = self
      .journal
      .lock()
      .expect("journal poisoned")
      .resolve_shutdown_pending()
      .is_ok();
    if resolved {
      self.shutdown_marker_owned.store(false, Ordering::SeqCst);
    }
    resolved
  }
}

fn movement_signature(
  request: &CommandRequest,
  operation: Option<&GatewayOperation>,
) -> Result<String, String> {
  let mut encoded = BoundedWriter::new(64 * 1024);
  match operation {
    Some(operation) => serde_json::to_writer(
      &mut encoded,
      &(
        if operation.identity.admission.is_some() {
          "4.0.0"
        } else {
          "3.0.0"
        },
        &request.node_path,
        &request.movement,
        &operation.identity,
      ),
    ),
    None => serde_json::to_writer(&mut encoded, &("2.0.0", request)),
  }
  .map_err(|error| format!("command signature could not be encoded: {error}"))?;
  String::from_utf8(encoded.into_inner()).map_err(|_| "command signature is not UTF-8".into())
}

/// Browser retries use the entire submitted deadline as part of the managed
/// command identity. A caller cannot turn a retained ID into a new lease.
fn browser_movement_signature(
  request: &CommandRequest,
  operation: &GatewayOperation,
) -> Result<String, String> {
  let mut encoded = BoundedWriter::new(64 * 1024);
  serde_json::to_writer(
    &mut encoded,
    &(
      "5.0.0",
      &request.node_path,
      &request.movement,
      request.meta.deadline_monotonic_ns,
      &operation.identity,
    ),
  )
  .map_err(|error| format!("command signature could not be encoded: {error}"))?;
  String::from_utf8(encoded.into_inner()).map_err(|_| "command signature is not UTF-8".into())
}

fn fresh(now: u64, received: u64, stale_ns: u64) -> bool {
  received <= now && now - received <= stale_ns
}

fn bounded(value: f64, max: Option<f64>) -> bool {
  value.is_finite() && max.is_none_or(|limit| value.abs() <= limit)
}

fn validate_base(
  command: &BaseMovement,
  profile: &aether_hal::MovementProfile,
  expected_frame: &str,
) -> Result<(), String> {
  let limits = &profile.limits;
  match command {
    BaseMovement::BodyVelocity {
      x_m_s,
      y_m_s,
      yaw_rad_s,
      valid_for_ms,
    } => {
      if !profile.modes.iter().any(|m| m == "body_velocity") {
        return Err("body_velocity mode is not advertised".into());
      }
      if *valid_for_ms == 0
        || limits
          .max_velocity_lease_ms
          .is_none_or(|max| *valid_for_ms > max)
        || !bounded(*x_m_s, limits.max_linear_m_s)
        || !bounded(*y_m_s, limits.max_linear_m_s)
        || !bounded(*yaw_rad_s, limits.max_angular_rad_s)
      {
        return Err("body velocity exceeds limits or validity is zero".into());
      }
    }
    BaseMovement::RelativePose {
      x_m,
      y_m,
      yaw_rad,
      frame_id,
    } => {
      if !profile.modes.iter().any(|m| m == "relative_pose") {
        return Err("relative_pose mode is not advertised".into());
      }
      if frame_id != expected_frame
        || !bounded(*x_m, limits.max_translation_m)
        || !bounded(*y_m, limits.max_translation_m)
        || !bounded(*yaw_rad, limits.max_rotation_rad)
      {
        return Err("relative pose exceeds limits or frame is empty".into());
      }
    }
  }
  Ok(())
}

fn validate_manipulator(
  command: &ManipulatorMovement,
  profile: &aether_hal::MovementProfile,
  observation: &aether_hal::ManipulatorObservation,
  expected_frame: &str,
) -> Result<(), String> {
  let limits = &profile.limits;
  match command {
    ManipulatorMovement::JointPositions {
      joint_names,
      joint_positions_rad,
      max_speed_rad_s,
    } => {
      if !profile.modes.iter().any(|m| m == "joint_positions") {
        return Err("joint_positions mode is not advertised".into());
      }
      if !observation.joints_valid
        || joint_names.is_empty()
        || joint_names != &observation.joint_names
        || joint_positions_rad.len() != joint_names.len()
        || !joint_positions_rad
          .iter()
          .all(|value| bounded(*value, limits.max_joint_position_rad))
        || max_speed_rad_s.is_some_and(|speed| {
          !speed.is_finite()
            || speed <= 0.0
            || limits.max_joint_speed_rad_s.is_some_and(|max| speed > max)
        })
      {
        return Err("joint targets are invalid or exceed limits".into());
      }
    }
    ManipulatorMovement::EndEffectorPose {
      pose,
      max_speed_m_s,
    } => {
      if !profile.modes.iter().any(|m| m == "end_effector_pose") {
        return Err("end_effector_pose mode is not advertised".into());
      }
      let qnorm = pose.orientation_xyzw.iter().map(|v| v * v).sum::<f64>();
      if !observation.end_effector_valid
        || pose.frame_id != expected_frame
        || !pose.position_m.iter().all(|v| v.is_finite())
        || !pose.orientation_xyzw.iter().all(|v| v.is_finite())
        || !(0.999..=1.001).contains(&qnorm)
        || limits.max_end_effector_translation_m.is_some_and(|max| {
          pose
            .position_m
            .iter()
            .map(|value| value * value)
            .sum::<f64>()
            .sqrt()
            > max
        })
        || max_speed_m_s.is_some_and(|speed| {
          !speed.is_finite()
            || speed <= 0.0
            || limits
              .max_end_effector_speed_m_s
              .is_some_and(|max| speed > max)
        })
      {
        return Err("end-effector target is invalid or exceeds limits".into());
      }
    }
  }
  Ok(())
}

fn valid_base_observation(obs: &aether_hal::BaseObservation, frame_id: &str) -> bool {
  obs.valid
    && obs.meta.frame_id == frame_id
    && [
      obs.x_m,
      obs.y_m,
      obs.yaw_rad,
      obs.x_m_s,
      obs.y_m_s,
      obs.yaw_rad_s,
    ]
    .iter()
    .all(|value| value.is_finite())
}

fn valid_pose(pose: &aether_hal::EndEffectorPose) -> bool {
  let qnorm = pose
    .orientation_xyzw
    .iter()
    .map(|value| value * value)
    .sum::<f64>();
  !pose.frame_id.trim().is_empty()
    && pose.position_m.iter().all(|value| value.is_finite())
    && pose.orientation_xyzw.iter().all(|value| value.is_finite())
    && (0.999..=1.001).contains(&qnorm)
}

fn valid_manipulator_observation(
  obs: &aether_hal::ManipulatorObservation,
  frame_id: &str,
  expected_joint_names: &[String],
) -> bool {
  if obs.meta.frame_id != frame_id {
    return false;
  }
  if obs.joints_valid {
    let mut names = HashSet::new();
    if obs.joint_names.is_empty()
      || obs.joint_names != expected_joint_names
      || !obs
        .joint_names
        .iter()
        .all(|name| !name.trim().is_empty() && names.insert(name))
      || obs.joint_positions_rad.len() != obs.joint_names.len()
      || obs.joint_velocities_rad_s.len() != obs.joint_names.len()
      || !obs
        .joint_positions_rad
        .iter()
        .chain(&obs.joint_velocities_rad_s)
        .all(|value| value.is_finite())
    {
      return false;
    }
  }
  if obs.end_effector_valid && !obs.end_effector_pose.as_ref().is_some_and(valid_pose) {
    return false;
  }
  true
}

#[cfg(test)]
mod tests {
  use super::*;
  use aether_hal::{
    ClockKind, EndEffectorPose, ManipulatorObservation, MovementLimits, MovementProfile, SampleMeta,
  };

  fn meta(frame: &str) -> SampleMeta {
    SampleMeta {
      source_timestamp_ns: None,
      clock: ClockKind::Unknown,
      received_monotonic_ns: 0,
      sequence: None,
      frame_id: frame.into(),
    }
  }

  #[test]
  fn test_base_observation_rejects_nonfinite_measurement() {
    let obs = aether_hal::BaseObservation {
      meta: meta("base"),
      valid: true,
      x_m: f64::NAN,
      y_m: 0.0,
      yaw_rad: 0.0,
      x_m_s: 0.0,
      y_m_s: 0.0,
      yaw_rad_s: 0.0,
    };
    assert!(!valid_base_observation(&obs, "base"));
  }

  #[test]
  fn test_manipulator_observation_rejects_wrong_joint_order_and_length() {
    let mut obs = ManipulatorObservation {
      meta: meta("arm"),
      joint_names: vec!["j2".into(), "j1".into()],
      joint_positions_rad: vec![0.0, 0.0],
      joint_velocities_rad_s: vec![0.0, 0.0],
      joints_valid: true,
      end_effector_pose: None,
      end_effector_valid: false,
    };
    let names = vec!["j1".into(), "j2".into()];
    assert!(!valid_manipulator_observation(&obs, "arm", &names));
    obs.joint_names = names.clone();
    obs.joint_velocities_rad_s.pop();
    assert!(!valid_manipulator_observation(&obs, "arm", &names));
  }

  #[test]
  fn test_end_effector_target_uses_euclidean_translation_bound() {
    let obs = ManipulatorObservation {
      meta: meta("arm"),
      joint_names: vec!["j1".into()],
      joint_positions_rad: vec![0.0],
      joint_velocities_rad_s: vec![0.0],
      joints_valid: true,
      end_effector_pose: Some(EndEffectorPose {
        position_m: [0.0; 3],
        orientation_xyzw: [0.0, 0.0, 0.0, 1.0],
        frame_id: "arm".into(),
      }),
      end_effector_valid: true,
    };
    let movement = ManipulatorMovement::EndEffectorPose {
      pose: EndEffectorPose {
        position_m: [0.8, 0.8, 0.0],
        orientation_xyzw: [0.0, 0.0, 0.0, 1.0],
        frame_id: "arm".into(),
      },
      max_speed_m_s: Some(0.1),
    };
    let profile = MovementProfile {
      modes: vec!["end_effector_pose".into()],
      limits: MovementLimits {
        max_end_effector_translation_m: Some(1.0),
        max_end_effector_speed_m_s: Some(0.2),
        ..Default::default()
      },
    };
    assert!(validate_manipulator(&movement, &profile, &obs, "arm").is_err());
  }
}
