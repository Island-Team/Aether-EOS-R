use std::io::{Read, Write};
use std::os::unix::net::{UnixListener, UnixStream};
use std::sync::Arc;

use aether_common::time::ManualClock;
use aether_hal::{
  BaseMovement, BaseNode, CommandMeta, CommandOutcome, HealthState, ManipulatorMovement,
  ManipulatorNode, NodeAdapter,
};
use aether_mujoco::{Endpoint, MujocoBase, MujocoManipulator};
use serde_json::{json, Value};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::Duration;

fn read_frame(stream: &mut UnixStream) -> Value {
  let mut lengths = [0u8; 8];
  stream.read_exact(&mut lengths).unwrap();
  let metadata_len = u32::from_le_bytes(lengths[..4].try_into().unwrap()) as usize;
  let payload_len = u32::from_le_bytes(lengths[4..].try_into().unwrap()) as usize;
  assert!(metadata_len <= aether_mujoco::MAX_METADATA_BYTES);
  assert_eq!(payload_len, 0);
  let mut body = vec![0; metadata_len];
  stream.read_exact(&mut body).unwrap();
  serde_json::from_slice(&body).unwrap()
}

fn write_frame(stream: &mut UnixStream, reply: Value) {
  let body = serde_json::to_vec(&reply).unwrap();
  stream
    .write_all(&(body.len() as u32).to_le_bytes())
    .unwrap();
  stream.write_all(&0u32.to_le_bytes()).unwrap();
  stream.write_all(&body).unwrap();
}

#[test]
fn managed_stop_uses_one_companion_frame_and_advances_only_dual_proof() {
  let directory = tempfile::tempdir().unwrap();
  let main = directory.path().join("m.sock");
  let stop = directory.path().join("m.sock.stop");
  let listener = UnixListener::bind(&stop).unwrap();
  let calls = Arc::new(AtomicUsize::new(0));
  let count = calls.clone();
  let generation = "00112233445566778899aabbccddeeff";
  let server = std::thread::spawn(move || {
    for (index, expected_epoch) in ["1", "2"].into_iter().enumerate() {
      let (mut stream, _) = listener.accept().unwrap();
      let request = read_frame(&mut stream);
      assert_eq!(request["op"], "stop");
      assert_eq!(request["generation_id"], generation);
      count.fetch_add(1, Ordering::SeqCst);
      // Keep the first request in flight so a second logical caller joins it.
      if index == 0 {
        std::thread::sleep(Duration::from_millis(80));
      }
      write_frame(
        &mut stream,
        json!({"version":"2.0.0","id":request["id"],
        "ok":index == 0,"accepted":index == 0,"generation_id":generation,
        "stop_epoch":expected_epoch,"base_stop":"stopped",
        "arm_stop":if index == 0 {"stopped"} else {"unknown"},
        "error":if index == 0 {Value::Null} else {json!({"code":"stop_outcome_unknown","message":"arm unknown"})}}),
      );
    }
  });
  let endpoint = Endpoint::new(
    main,
    "base-0".into(),
    String::new(),
    Vec::new(),
    Arc::new(ManualClock::new(0, None)),
  );
  let session = endpoint.session();
  session.install_generation(generation).unwrap();
  let original = session.token().unwrap();
  let first = {
    let session = session.clone();
    std::thread::spawn(move || session.stop(Duration::from_secs(1)))
  };
  while calls.load(Ordering::SeqCst) == 0 {
    std::thread::sleep(Duration::from_millis(1));
  }
  let joined = session.stop(Duration::from_secs(1));
  let first = first.join().unwrap();
  assert_eq!(first, joined);
  assert_eq!(first.stop_epoch.as_deref(), Some("1"));
  assert_eq!(session.token().unwrap().stop_epoch, "1");
  let base = MujocoBase(endpoint);
  let _ = base.move_base_fenced(
    CommandMeta {
      command_id: "attempt".into(),
      deadline_monotonic_ns: 100,
    },
    BaseMovement::BodyVelocity {
      x_m_s: 0.1,
      y_m_s: 0.0,
      yaw_rad_s: 0.0,
      valid_for_ms: 10,
    },
    session.token(),
  );
  let partial = session.stop(Duration::from_secs(1));
  assert_eq!(partial.base, aether_hal::ControlledStopOutcome::Stopped);
  assert_eq!(
    partial.arm,
    aether_hal::ControlledStopOutcome::OutcomeUnknown
  );
  assert_eq!(session.token().unwrap().stop_epoch, "1");
  assert_ne!(original, session.token().unwrap());
  assert_eq!(calls.load(Ordering::SeqCst), 2);
  server.join().unwrap();
}

#[test]
fn delayed_move_keeps_epoch_captured_before_stop() {
  let directory = tempfile::tempdir().unwrap();
  let main = directory.path().join("m.sock");
  let stop = directory.path().join("m.sock.stop");
  let main_listener = UnixListener::bind(&main).unwrap();
  let stop_listener = UnixListener::bind(&stop).unwrap();
  let generation = "00112233445566778899aabbccddeeff";
  let stopper = std::thread::spawn(move || {
    let (mut stream, _) = stop_listener.accept().unwrap();
    let request = read_frame(&mut stream);
    write_frame(
      &mut stream,
      json!({"version":"2.0.0","id":request["id"],
      "ok":true,"accepted":true,"generation_id":generation,"stop_epoch":"1",
      "base_stop":"stopped","arm_stop":"stopped"}),
    );
  });
  let mover = std::thread::spawn(move || {
    let (mut stream, _) = main_listener.accept().unwrap();
    let request = read_frame(&mut stream);
    assert_eq!(request["op"], "base_move");
    assert_eq!(request["generation_id"], generation);
    assert_eq!(request["stop_epoch"], "0");
    write_frame(
      &mut stream,
      json!({"version":"2.0.0","id":request["id"],
      "ok":false,"accepted":false,"error":{"code":"stop_fenced","message":"stale epoch"}}),
    );
  });
  let endpoint = Endpoint::new(
    main,
    "base-0".into(),
    "base_link".into(),
    Vec::new(),
    Arc::new(ManualClock::new(0, None)),
  );
  let session = endpoint.session();
  session.install_generation(generation).unwrap();
  let old = session.token();
  let proof = session.stop(Duration::from_secs(1));
  assert_eq!(proof.stop_epoch.as_deref(), Some("1"));
  let base = MujocoBase(endpoint);
  assert!(base
    .move_base_fenced(
      CommandMeta {
        command_id: "late".into(),
        deadline_monotonic_ns: 100
      },
      BaseMovement::BodyVelocity {
        x_m_s: 0.1,
        y_m_s: 0.0,
        yaw_rad_s: 0.0,
        valid_for_ms: 10
      },
      old
    )
    .is_err());
  stopper.join().unwrap();
  mover.join().unwrap();
}

#[test]
fn base_observe_command_and_stop_use_typed_bounded_frames() {
  let path = std::env::temp_dir().join(format!(
    "aether-mujoco-wire-{}-{}.sock",
    std::process::id(),
    std::thread::current().name().unwrap_or("test")
  ));
  let _ = std::fs::remove_file(&path);
  let listener = UnixListener::bind(&path).unwrap();
  let server = std::thread::spawn(move || {
    for (index, expected) in ["base_observe", "base_move", "base_observe", "stop"]
      .into_iter()
      .enumerate()
    {
      let (mut stream, _) = listener.accept().unwrap();
      let request = read_frame(&mut stream);
      assert_eq!(request["version"], "2.0.0");
      assert_eq!(request["op"], expected);
      assert_eq!(request["node"], "base-0");
      if expected == "base_move" {
        assert_eq!(request["command_id"], "cmd-1");
        assert_eq!(request["valid_for_ms"], 100);
      }
      if expected == "stop" {
        assert!(request["command_id"].as_str().is_some());
      }
      let mut reply = json!({"version":"2.0.0","id":request["id"],"ok":true});
      if expected == "base_observe" {
        reply["position_m"] = json!([1.0, 2.0]);
        reply["yaw_rad"] = json!(0.25);
        reply["linear_velocity_m_s"] = json!(if index == 0 { [0.1, 0.2] } else { [0.0, 0.0] });
        reply["angular_velocity_rad_s"] = json!(if index == 0 { 0.3 } else { 0.0 });
        reply["sequence"] = json!(if index == 0 { 7 } else { 8 });
        reply["timestamp_ns"] = json!(if index == 0 { 0.0 } else { 200_000_000.0 });
      } else {
        reply["accepted"] = json!(true);
        if expected == "base_move" {
          reply["timestamp_ns"] = json!(0.0);
          reply["sequence"] = json!(7);
        }
      }
      write_frame(&mut stream, reply);
    }
  });
  let endpoint = Endpoint::new(
    path.clone(),
    "base-0".into(),
    "base_link".into(),
    vec![],
    Arc::new(ManualClock::new(10, Some(20))),
  );
  let base = MujocoBase(endpoint);
  let observation = base.observe().unwrap();
  assert_eq!(observation.x_m, 1.0);
  assert_eq!(observation.meta.sequence, Some(7));
  assert_eq!(observation.meta.frame_id, "base_link");
  assert_eq!(
    base
      .move_base(
        CommandMeta {
          command_id: "cmd-1".into(),
          deadline_monotonic_ns: 100,
        },
        BaseMovement::BodyVelocity {
          x_m_s: 0.1,
          y_m_s: 0.0,
          yaw_rad_s: 0.0,
          valid_for_ms: 100,
        },
      )
      .unwrap(),
    aether_hal::CommandOutcome::Completed
  );
  base.stop().unwrap();
  assert!(base
    .move_base(
      CommandMeta {
        command_id: "cmd-2".into(),
        deadline_monotonic_ns: 100,
      },
      BaseMovement::BodyVelocity {
        x_m_s: 0.1,
        y_m_s: 0.0,
        yaw_rad_s: 0.0,
        valid_for_ms: 100,
      },
    )
    .is_err());
  server.join().unwrap();
  std::fs::remove_file(path).unwrap();
}

#[test]
fn response_identity_mismatch_is_rejected() {
  let path = std::env::temp_dir().join(format!("aether-mujoco-bad-{}.sock", std::process::id()));
  let _ = std::fs::remove_file(&path);
  let listener = UnixListener::bind(&path).unwrap();
  let server = std::thread::spawn(move || {
    let (mut stream, _) = listener.accept().unwrap();
    let _ = read_frame(&mut stream);
    write_frame(
      &mut stream,
      json!({"version":"2.0.0","id":"wrong","ok":true}),
    );
  });
  let base = MujocoBase(Endpoint::new(
    path.clone(),
    "base-0".into(),
    "base_link".into(),
    vec![],
    Arc::new(ManualClock::new(0, None)),
  ));
  assert!(base.observe().is_err());
  server.join().unwrap();
  std::fs::remove_file(path).unwrap();
}

#[test]
fn accepted_base_without_lease_evidence_is_uncertain() {
  let path = std::env::temp_dir().join(format!(
    "aether-mujoco-unconfirmed-{}.sock",
    std::process::id()
  ));
  let _ = std::fs::remove_file(&path);
  let listener = UnixListener::bind(&path).unwrap();
  let server = std::thread::spawn(move || {
    let (mut stream, _) = listener.accept().unwrap();
    let request = read_frame(&mut stream);
    assert_eq!(request["op"], "base_move");
    write_frame(
      &mut stream,
      json!({"version":"2.0.0","id":request["id"],"ok":true,"accepted":true}),
    );
  });
  let base = MujocoBase(Endpoint::new(
    path.clone(),
    "base-0".into(),
    "base_link".into(),
    vec![],
    Arc::new(ManualClock::new(10, None)),
  ));
  let outcome = base
    .move_base(
      CommandMeta {
        command_id: "unconfirmed".into(),
        deadline_monotonic_ns: 100,
      },
      BaseMovement::BodyVelocity {
        x_m_s: 0.1,
        y_m_s: 0.0,
        yaw_rad_s: 0.0,
        valid_for_ms: 100,
      },
    )
    .unwrap();
  assert_eq!(outcome, CommandOutcome::Ambiguous);
  server.join().unwrap();
  std::fs::remove_file(path).unwrap();
}

#[test]
fn arm_completion_requires_post_command_joint_convergence() {
  for (index, actual) in [0.5, 0.1].into_iter().enumerate() {
    let path = std::env::temp_dir().join(format!(
      "aether-mujoco-arm-confirm-{}-{index}.sock",
      std::process::id()
    ));
    let _ = std::fs::remove_file(&path);
    let listener = UnixListener::bind(&path).unwrap();
    let server = std::thread::spawn(move || {
      for expected in ["manipulator_move", "manipulator_observe"] {
        let (mut stream, _) = listener.accept().unwrap();
        let request = read_frame(&mut stream);
        assert_eq!(request["op"], expected);
        let mut reply = json!({"version":"2.0.0","id":request["id"],"ok":true});
        if expected == "manipulator_move" {
          reply["accepted"] = json!(true);
        } else {
          reply["joint_positions_rad"] = json!(vec![actual; 6]);
          reply["joint_velocities_rad_s"] = json!(vec![0.0; 6]);
          reply["end_position_m"] = json!(vec![0.0; 3]);
          reply["end_orientation_xyzw"] = json!([0.0, 0.0, 0.0, 1.0]);
        }
        write_frame(&mut stream, reply);
      }
    });
    let arm = MujocoManipulator(Endpoint::new(
      path.clone(),
      "arm-0".into(),
      "arm_base".into(),
      (1..=6).map(|i| format!("j{i}")).collect(),
      Arc::new(ManualClock::new(10, None)),
    ));
    let outcome = arm
      .move_manipulator(
        CommandMeta {
          command_id: format!("arm-{index}"),
          deadline_monotonic_ns: 100,
        },
        ManipulatorMovement::JointPositions {
          joint_names: (1..=6).map(|i| format!("j{i}")).collect(),
          joint_positions_rad: vec![0.5; 6],
          max_speed_rad_s: Some(1.0),
        },
      )
      .unwrap();
    assert_eq!(
      outcome,
      if index == 0 {
        CommandOutcome::Completed
      } else {
        CommandOutcome::Ambiguous
      }
    );
    server.join().unwrap();
    std::fs::remove_file(path).unwrap();
  }
}

#[test]
fn base_confirmation_respects_command_deadline() {
  let path = std::env::temp_dir().join(format!(
    "aether-mujoco-deadline-{}.sock",
    std::process::id()
  ));
  let _ = std::fs::remove_file(&path);
  let listener = UnixListener::bind(&path).unwrap();
  let server = std::thread::spawn(move || {
    let (mut stream, _) = listener.accept().unwrap();
    let request = read_frame(&mut stream);
    write_frame(
      &mut stream,
      json!({"version":"2.0.0","id":request["id"],"ok":true,
                "accepted":true,"timestamp_ns":0.0,"sequence":1}),
    );
  });
  let base = MujocoBase(Endpoint::new(
    path.clone(),
    "base-0".into(),
    "base_link".into(),
    vec![],
    Arc::new(ManualClock::new(101, None)),
  ));
  let result = base
    .move_base(
      CommandMeta {
        command_id: "expired".into(),
        deadline_monotonic_ns: 100,
      },
      BaseMovement::BodyVelocity {
        x_m_s: 0.1,
        y_m_s: 0.0,
        yaw_rad_s: 0.0,
        valid_for_ms: 100,
      },
    )
    .unwrap();
  assert_eq!(result, CommandOutcome::Ambiguous);
  server.join().unwrap();
  std::fs::remove_file(path).unwrap();
}

#[test]
fn stop_during_base_confirmation_cannot_be_reported_complete() {
  let path = std::env::temp_dir().join(format!("aether-mujoco-cancel-{}.sock", std::process::id()));
  let _ = std::fs::remove_file(&path);
  let listener = UnixListener::bind(&path).unwrap();
  let (observing, observed) = std::sync::mpsc::channel();
  let server = std::thread::spawn(move || {
    let (mut move_stream, _) = listener.accept().unwrap();
    let move_request = read_frame(&mut move_stream);
    write_frame(
      &mut move_stream,
      json!({"version":"2.0.0","id":move_request["id"],"ok":true,
            "accepted":true,"timestamp_ns":0.0,"sequence":1}),
    );
    let (mut observe_stream, _) = listener.accept().unwrap();
    let observe_request = read_frame(&mut observe_stream);
    assert_eq!(observe_request["op"], "base_observe");
    observing.send(()).unwrap();
    let (mut stop_stream, _) = listener.accept().unwrap();
    let stop_request = read_frame(&mut stop_stream);
    assert_eq!(stop_request["op"], "stop");
    write_frame(
      &mut stop_stream,
      json!({"version":"2.0.0","id":stop_request["id"],"ok":true,"accepted":true}),
    );
    write_frame(
      &mut observe_stream,
      json!({"version":"2.0.0","id":observe_request["id"],"ok":true,
            "timestamp_ns":200_000_000.0,"sequence":2,"linear_velocity_m_s":[0.0,0.0],"angular_velocity_rad_s":0.0}),
    );
  });
  let base = Arc::new(MujocoBase(Endpoint::new(
    path.clone(),
    "base-0".into(),
    "base_link".into(),
    vec![],
    Arc::new(ManualClock::new(10, None)),
  )));
  let moving = base.clone();
  let movement = std::thread::spawn(move || {
    moving.move_base(
      CommandMeta {
        command_id: "cancel-me".into(),
        deadline_monotonic_ns: 1_000_000_000,
      },
      BaseMovement::BodyVelocity {
        x_m_s: 0.1,
        y_m_s: 0.0,
        yaw_rad_s: 0.0,
        valid_for_ms: 100,
      },
    )
  });
  observed
    .recv_timeout(std::time::Duration::from_secs(2))
    .unwrap();
  base.stop().unwrap();
  assert_eq!(movement.join().unwrap().unwrap(), CommandOutcome::Ambiguous);
  server.join().unwrap();
  std::fs::remove_file(path).unwrap();
}

#[test]
fn health_refresh_requires_strict_proof_each_time() {
  let path =
    std::env::temp_dir().join(format!("aether-mujoco-refresh-{}.sock", std::process::id()));
  let _ = std::fs::remove_file(&path);
  let listener = UnixListener::bind(&path).unwrap();
  let server = std::thread::spawn(move || {
    for strict in [true, false] {
      let (mut stream, _) = listener.accept().unwrap();
      let request = read_frame(&mut stream);
      assert_eq!(request["op"], "health");
      assert_eq!(request["node"], "base-0");
      let mut reply = json!({"version":"2.0.0","id":request["id"],
                             "ok":true,"accepted":true});
      if strict {
        reply["features"] = json!(["strict-socket-v1"]);
      }
      write_frame(&mut stream, reply);
    }
  });
  let base = MujocoBase(Endpoint::new(
    path.clone(),
    "base-0".into(),
    "base_link".into(),
    vec![],
    Arc::new(ManualClock::new(10, None)),
  ));
  assert_eq!(base.health().state, HealthState::Healthy);
  assert_eq!(base.health().state, HealthState::Unavailable);
  server.join().unwrap();
  std::fs::remove_file(path).unwrap();
}

#[test]
fn slow_drip_response_uses_one_total_health_deadline() {
  let path = std::env::temp_dir().join(format!("aether-mujoco-drip-{}.sock", std::process::id()));
  let _ = std::fs::remove_file(&path);
  let listener = UnixListener::bind(&path).unwrap();
  let server = std::thread::spawn(move || {
    let (mut stream, _) = listener.accept().unwrap();
    let request = read_frame(&mut stream);
    let reply = serde_json::to_vec(&json!({"version":"2.0.0","id":request["id"],
      "ok":true,"accepted":true,"features":["strict-socket-v1"]}))
    .unwrap();
    let mut frame = Vec::from((reply.len() as u32).to_le_bytes());
    frame.extend_from_slice(&0u32.to_le_bytes());
    frame.extend_from_slice(&reply);
    for byte in frame {
      if stream.write_all(&[byte]).is_err() {
        break;
      }
      std::thread::sleep(std::time::Duration::from_millis(20));
    }
  });
  let endpoint = Endpoint::new(
    path.clone(),
    "base-0".into(),
    "base_link".into(),
    vec![],
    Arc::new(ManualClock::new(0, None)),
  );
  let started = std::time::Instant::now();
  let error = endpoint
    .strict_health(std::time::Duration::from_millis(120))
    .unwrap_err();
  assert!(error.is_transient(), "{error}");
  assert!(started.elapsed() < std::time::Duration::from_millis(300));
  server.join().unwrap();
  std::fs::remove_file(path).unwrap();
}
