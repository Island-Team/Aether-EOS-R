use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::path::PathBuf;
use std::process::{Command, Stdio};
use std::time::{SystemTime, UNIX_EPOCH};

use serde_json::Value;

fn unique_dir() -> PathBuf {
  let stamp = SystemTime::now()
    .duration_since(UNIX_EPOCH)
    .unwrap()
    .as_nanos();
  let dir = std::env::temp_dir().join(format!(
    "aether-jsonl-runtime-{}-{stamp}",
    std::process::id()
  ));
  std::fs::create_dir(&dir).unwrap();
  dir
}

fn events(stderr: &[u8]) -> Vec<Value> {
  stderr
    .split(|byte| *byte == b'\n')
    .filter(|line| !line.is_empty())
    .map(|line| {
      assert!(line.len() < 8064);
      let event: Value = serde_json::from_slice(line).unwrap();
      assert_eq!(event.as_object().unwrap().len(), 10);
      assert!(event.get("pid").is_none());
      event
    })
    .collect()
}

#[test]
fn run_id_lifecycle_preserves_stdout_and_jsonl_stderr() {
  let dir = unique_dir();
  let socket = dir.join("eos.sock");
  let journal = dir.join("journal");
  let profile =
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../etc/eos/backends/mock-cxx.toml");
  let mut child = Command::new(env!("CARGO_BIN_EXE_aether-runtime"))
    .arg("--grpc-port")
    .arg("0")
    .args([
      "--profile",
      profile.to_str().unwrap(),
      "--journal-dir",
      journal.to_str().unwrap(),
      "--socket",
      socket.to_str().unwrap(),
      "--run-id",
      "run_7",
    ])
    .stdout(Stdio::piped())
    .stderr(Stdio::piped())
    .spawn()
    .unwrap();
  let mut ready_line = String::new();
  BufReader::new(child.stdout.take().unwrap())
    .read_line(&mut ready_line)
    .unwrap();
  assert_eq!(
    serde_json::from_str::<Value>(&ready_line).unwrap()["status"],
    "ready"
  );
  let mut stream = UnixStream::connect(&socket).unwrap();
  stream
    .write_all(b"{\"op\":\"shutdown\",\"correlation_id\":\"op_42\"}\n")
    .unwrap();
  let output = child.wait_with_output().unwrap();
  assert!(output.status.success());
  let lines = events(&output.stderr);
  let codes: Vec<_> = lines
    .iter()
    .map(|line| line["event_code"].as_str().unwrap())
    .collect();
  assert_eq!(
    codes,
    ["eos_starting", "eos_ready", "eos_stopping", "eos_stopped"]
  );
  assert!(lines
    .iter()
    .all(|line| line["run_id"] == "run_7" && line["source"] == "eos" && line["core"] == "eos"));
  assert!(lines[1]["fields"]["node_count"].as_u64().unwrap() >= 3);
  assert_eq!(lines[0]["correlation_id"], Value::Null);
  assert_eq!(lines[1]["correlation_id"], Value::Null);
  assert_eq!(lines[2]["correlation_id"], "op_42");
  assert_eq!(lines[3]["correlation_id"], "op_42");
  let _ = std::fs::remove_dir_all(dir);
}

#[test]
fn invalid_arguments_have_no_event_and_profile_failure_has_no_ready() {
  let program = env!("CARGO_BIN_EXE_aether-runtime");
  for args in [
    vec!["--run-id".into(), "bad/path".into()],
    vec!["--run-id".into(), "a".into(), "--run-id".into(), "b".into()],
    vec!["--backend-release-sha256".into(), "a".into()],
    vec!["--backend-payload-sha256".into(), "a".repeat(64)],
  ] {
    let output = Command::new(program).args(args).output().unwrap();
    assert!(!output.status.success());
    assert!(!output
      .stderr
      .windows(13)
      .any(|bytes| bytes == b"\"event_code\""));
  }
  let dir = unique_dir();
  let socket = dir.join("bad.sock");
  let output = Command::new(program)
    .args([
      "--profile",
      "/home/private/TOP_SECRET_9F.toml",
      "--journal-dir",
      dir.to_str().unwrap(),
      "--socket",
      socket.to_str().unwrap(),
      "--run-id",
      "run_7",
    ])
    .output()
    .unwrap();
  assert!(!output.status.success());
  let lines = events(&output.stderr);
  assert_eq!(lines.len(), 2);
  assert_eq!(lines[0]["event_code"], "eos_starting");
  assert_eq!(lines[1]["event_code"], "eos_start_failed");
  assert_eq!(lines[1]["fields"]["stage"], "profile");
  assert!(!String::from_utf8_lossy(&output.stderr).contains("TOP_SECRET_9F"));
  let _ = std::fs::remove_dir_all(dir);
}
