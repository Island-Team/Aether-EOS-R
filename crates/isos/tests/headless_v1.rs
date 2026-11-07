use std::{
  io::{Read, Write},
  net::{TcpListener, TcpStream},
  os::unix::net::UnixStream,
  process::{Command, Stdio},
  time::{Duration, Instant},
};

fn free_port() -> u16 {
  TcpListener::bind("127.0.0.1:0")
    .unwrap()
    .local_addr()
    .unwrap()
    .port()
}

#[test]
fn incompatible_ui_keeps_v1_api_and_manager_available() {
  let home = tempfile::tempdir().unwrap();
  let api_port = free_port();
  let occupied_ui = TcpListener::bind("127.0.0.1:0").unwrap();
  let ui_port = occupied_ui.local_addr().unwrap().port();
  let mut child = Command::new(env!("CARGO_BIN_EXE_isos"))
    .arg("serve")
    .env("ISOS_HOME", home.path())
    .env("HOME", home.path())
    .env("XDG_CONFIG_HOME", home.path().join("config"))
    .env("ISOS_HTTP_PORT", api_port.to_string())
    .env("ISOSUI_PORT", ui_port.to_string())
    .env("ISOSUI_DIST_DIR", home.path().join("missing-ui"))
    .env_remove("ISOS_CONFIG")
    .stdout(Stdio::piped())
    .stderr(Stdio::piped())
    .spawn()
    .unwrap();
  let deadline = Instant::now() + Duration::from_secs(10);
  let mut response = String::new();
  while Instant::now() < deadline {
    if child.try_wait().unwrap().is_some() {
      break;
    }
    if UnixStream::connect(home.path().join("run/manager.sock")).is_ok()
      && let Ok(mut connection) = TcpStream::connect(("127.0.0.1", api_port))
    {
      connection
        .set_read_timeout(Some(Duration::from_secs(2)))
        .unwrap();
      write!(
                connection,
                "GET /api/isos/v1/system/version HTTP/1.1\r\nHost: 127.0.0.1:{api_port}\r\nConnection: close\r\n\r\n"
            )
            .unwrap();
      let _ = connection.read_to_string(&mut response);
      if response.contains("api_schema_version") {
        break;
      }
    }
    std::thread::sleep(Duration::from_millis(50));
  }
  unsafe { libc::kill(child.id() as i32, libc::SIGTERM) };
  let output = child.wait_with_output().unwrap();
  assert!(
    response.starts_with("HTTP/1.1 200") && response.contains("\"api_schema_version\":\"0.1.0\""),
    "response={response}; stdout={}; stderr={}",
    String::from_utf8_lossy(&output.stdout),
    String::from_utf8_lossy(&output.stderr)
  );
  assert!(output.status.success(), "{output:?}");
}
