use std::net::{TcpListener, TcpStream};
use std::path::Path;
use std::process::Command;
use std::time::Duration;

use aether_runtime::grpc::v1;
use serde_json::Value;

fn run_rejected_start(profile: &Path, corrupt_journal: bool, expected_stage: &str) {
  let directory = std::env::temp_dir().join(format!(
    "r02-admission-{}-{}",
    std::process::id(),
    std::time::SystemTime::now()
      .duration_since(std::time::UNIX_EPOCH)
      .unwrap()
      .as_nanos()
  ));
  std::fs::create_dir(&directory).unwrap();
  let journal = directory.join("journal");
  if corrupt_journal {
    std::fs::create_dir(&journal).unwrap();
    std::fs::write(journal.join("corrupt.rec"), b"not a journal record").unwrap();
  }
  let probe = TcpListener::bind("127.0.0.1:0").unwrap();
  let address = probe.local_addr().unwrap();
  drop(probe);
  let socket = directory.join("control.sock");
  let output = Command::new(env!("CARGO_BIN_EXE_aether-runtime"))
    .args(["--profile", profile.to_str().unwrap()])
    .args(["--journal-dir", journal.to_str().unwrap()])
    .args(["--socket", socket.to_str().unwrap()])
    .args(["--grpc-port", &address.port().to_string()])
    .args(["--run-id", "rR0201"])
    .output()
    .unwrap();
  assert!(!output.status.success());
  assert!(output.stdout.is_empty(), "no ready line may be published");
  let events: Vec<Value> = output
    .stderr
    .split(|byte| *byte == b'\n')
    .filter(|line| !line.is_empty())
    .map(|line| serde_json::from_slice(line).unwrap())
    .collect();
  assert!(events.iter().any(|event| {
    event["event_code"] == "eos_start_failed" && event["fields"]["stage"] == expected_stage
  }));
  assert!(!events
    .iter()
    .any(|event| event["event_code"] == "eos_ready"));
  assert!(
    !socket.exists(),
    "private listener must not survive failed admission"
  );
  assert!(TcpStream::connect_timeout(&address, Duration::from_millis(100)).is_err());

  // This implementation binds gRPC only after admission. A failed startup
  // therefore has no service that could report NOT_SERVING; both required
  // health and Gateway RPCs must be unavailable on the selected endpoint.
  tokio::runtime::Builder::new_current_thread()
    .enable_all()
    .build()
    .unwrap()
    .block_on(async {
      let channel = tonic::transport::Endpoint::from_shared(format!("http://{address}"))
        .unwrap()
        .connect_timeout(Duration::from_millis(100))
        .connect_lazy();
      let mut health = tonic_health::pb::health_client::HealthClient::new(channel.clone());
      assert_eq!(
        health
          .check(tonic_health::pb::HealthCheckRequest {
            service: String::new(),
          })
          .await
          .unwrap_err()
          .code(),
        tonic::Code::Unavailable
      );
      let mut gateway = v1::gateway_service_client::GatewayServiceClient::new(channel);
      assert_eq!(
        gateway.discover(()).await.unwrap_err().code(),
        tonic::Code::Unavailable
      );
    });
  std::fs::remove_dir_all(directory).unwrap();
}

#[test]
fn malformed_profile_never_publishes_ready_or_a_grpc_listener() {
  let directory = std::env::temp_dir().join(format!(
    "r02-bad-profile-{}-{}",
    std::process::id(),
    std::time::SystemTime::now()
      .duration_since(std::time::UNIX_EPOCH)
      .unwrap()
      .as_nanos()
  ));
  std::fs::create_dir(&directory).unwrap();
  let profile = directory.join("bad.toml");
  std::fs::write(&profile, b"version = [").unwrap();
  run_rejected_start(&profile, false, "profile");
  std::fs::remove_dir_all(directory).unwrap();
}

#[test]
fn corrupt_journal_never_publishes_ready_or_a_grpc_listener() {
  let profile = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../etc/eos/backends/mock-cxx.toml");
  run_rejected_start(&profile, true, "journal");
}
