use std::io::{Read, Write};
use std::os::unix::net::UnixListener;

use serde_json::json;

#[test]
fn control_client_exchanges_one_bounded_json_request() {
  let path = std::env::temp_dir().join(format!("aether-control-{}.sock", std::process::id()));
  let _ = std::fs::remove_file(&path);
  let listener = UnixListener::bind(&path).unwrap();
  let server = std::thread::spawn(move || {
    let (mut stream, _) = listener.accept().unwrap();
    let mut bytes = [0u8; 128];
    let count = stream.read(&mut bytes).unwrap();
    assert_eq!(&bytes[..count], b"{\"op\":\"discover\"}\n");
    stream.write_all(b"{\"ok\":true,\"nodes\":[]}\n").unwrap();
  });
  let result = aether_control::request(&path, &json!({"op":"discover"})).unwrap();
  assert_eq!(result["ok"], true);
  server.join().unwrap();
  std::fs::remove_file(path).unwrap();
}
