//! Sanitized browser command diagnostics. The Dispatcher journal owns outcomes.

use std::collections::HashMap;
use std::io::Write;
use std::sync::Mutex;

use aether_common::events::{Event, EventSink, valid_run_id};
use aether_common::time::Clock;
use serde_json::Value;
use sha2::{Digest, Sha256};

const MAX_COMMANDS: usize = 1024;
const HASH_DOMAIN: &[u8] = b"eos-browser-command-v1\0";

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum Phase {
  Submit,
  Followup,
}

impl Phase {
  fn as_str(self) -> &'static str {
    match self {
      Self::Submit => "submit",
      Self::Followup => "followup",
    }
  }
}

#[derive(Clone)]
struct Seen {
  submit_seen: bool,
  terminal_status: Option<&'static str>,
  order: u64,
}

#[derive(Default)]
struct State {
  seen: HashMap<String, Seen>,
  next_order: u64,
}

#[derive(Default)]
pub(crate) struct ResultEvents(Mutex<State>);

pub(crate) fn public_command_id(request: &Value, phase: Phase) -> Option<&str> {
  let id = match phase {
    Phase::Submit => request.get("header")?.get("command_id")?.as_str()?,
    Phase::Followup => request.get("command_id")?.as_str()?,
  };
  (!id.is_empty() && id.len() <= 128).then_some(id)
}

/// A successful private response must match the admitted request exactly.
/// A failed private operation is forwarded without inventing a result event.
pub(crate) fn validated_status(
  response: &Value,
  command_id: Option<&str>,
) -> Result<Option<&'static str>, ()> {
  match response.get("ok").and_then(Value::as_bool) {
    Some(false) => return Ok(None),
    Some(true) => {}
    None => return Err(()),
  }
  let command_id = command_id.ok_or(())?;
  let result = response
    .get("result")
    .and_then(Value::as_object)
    .ok_or(())?;
  if result.get("command_id").and_then(Value::as_str) != Some(command_id) {
    return Err(());
  }
  let status = match result.get("status").and_then(Value::as_str) {
    Some("Completed") => "Completed",
    Some("Failed") => "Failed",
    Some("Cancelled") => "Cancelled",
    Some("Rejected") => "Rejected",
    Some("InProgress") => "InProgress",
    Some("OutcomeUnknown") => "OutcomeUnknown",
    _ => return Err(()),
  };
  Ok(Some(status))
}

pub(crate) fn correlation_id(command_id: &str) -> String {
  if valid_run_id(command_id) && !command_id.starts_with("h_") {
    return command_id.to_owned();
  }
  let digest = Sha256::new()
    .chain_update(HASH_DOMAIN)
    .chain_update(command_id.as_bytes())
    .finalize();
  let mut token = String::with_capacity(64);
  token.push_str("h_");
  for byte in digest.iter().take(31) {
    use std::fmt::Write as _;
    write!(token, "{byte:02x}").expect("string write");
  }
  token
}

fn terminal(status: &str) -> bool {
  status != "InProgress"
}

impl ResultEvents {
  pub(crate) fn observe<W: Write, C: Clock>(
    &self,
    sink: &EventSink<W, C>,
    command_id: &str,
    status: &'static str,
    phase: Phase,
  ) {
    let Ok(mut state) = self.0.lock() else { return };
    let prior = state.seen.get(command_id);
    let emit = match phase {
      Phase::Submit => !prior.is_some_and(|seen| seen.submit_seen),
      Phase::Followup => {
        terminal(status) && !prior.is_some_and(|seen| seen.terminal_status.is_some())
      }
    };
    if !emit {
      return;
    }
    if !state.seen.contains_key(command_id) && state.seen.len() >= MAX_COMMANDS {
      if let Some(oldest) = state
        .seen
        .iter()
        .min_by_key(|(_, seen)| (seen.terminal_status.is_none(), seen.order))
        .map(|(id, _)| id.clone())
      {
        state.seen.remove(&oldest);
      }
    }
    state.next_order = state.next_order.wrapping_add(1);
    let order = state.next_order;
    let mut seen = state.seen.remove(command_id).unwrap_or(Seen {
      submit_seen: false,
      terminal_status: None,
      order,
    });
    seen.order = order;
    if phase == Phase::Submit {
      seen.submit_seen = true;
    }
    if terminal(status) && seen.terminal_status.is_none() {
      seen.terminal_status = Some(status);
    }
    state.seen.insert(command_id.to_owned(), seen);
    let token = correlation_id(command_id);
    // A failed nonblocking sink write permanently disables that process's
    // stream. Diagnostic loss must never turn a command into a false failure.
    let _ = sink.emit_correlated(
      Event::BridgePrimitiveResult {
        status,
        phase: phase.as_str(),
      },
      None,
      Some(&token),
    );
  }
}

#[cfg(test)]
mod tests {
  use super::*;
  use aether_common::events::Source;
  use aether_common::time::SystemClock;
  use std::io;
  use std::io::Read;
  use std::net::{TcpListener, TcpStream};
  use std::sync::Arc;
  use std::sync::atomic::{AtomicUsize, Ordering};

  #[derive(Clone)]
  struct Shared(Arc<Mutex<Vec<u8>>>);
  impl Write for Shared {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
      self.0.lock().unwrap().extend_from_slice(bytes);
      Ok(bytes.len())
    }
    fn flush(&mut self) -> io::Result<()> {
      Ok(())
    }
  }

  fn sink() -> (EventSink<Shared, SystemClock>, Shared) {
    let writer = Shared(Arc::new(Mutex::new(Vec::new())));
    (
      EventSink::new(Source::Bridge, "r_test".into(), writer.clone(), SystemClock).unwrap(),
      writer,
    )
  }

  #[test]
  fn correlation_preserves_safe_ids_and_derives_unsafe_ids() {
    assert_eq!(correlation_id("ui-123"), "ui-123");
    for id in ["h_claim", "é", &"x".repeat(128)] {
      let token = correlation_id(id);
      assert_eq!(token.len(), 64);
      assert!(token.starts_with("h_"));
      assert!(valid_run_id(&token));
      assert_ne!(token, id);
      assert_eq!(token, correlation_id(id));
    }
  }

  #[test]
  fn validates_matching_result_and_closed_status_set() {
    assert_eq!(
      validated_status(
        &serde_json::json!({"ok":true,"result":{"command_id":"x","status":"Completed"}}),
        Some("x")
      ),
      Ok(Some("Completed"))
    );
    for result in [
      serde_json::json!({"command_id":"y","status":"Completed"}),
      serde_json::json!({"command_id":"x","status":"Unknown"}),
      serde_json::json!({"status":"Completed"}),
    ] {
      assert_eq!(
        validated_status(&serde_json::json!({"ok":true,"result":result}), Some("x")),
        Err(())
      );
    }
    assert_eq!(
      validated_status(&serde_json::json!({"ok":false,"error":"denied"}), Some("x")),
      Ok(None)
    );
    assert_eq!(
      validated_status(&serde_json::json!({"result":{}}), Some("x")),
      Err(())
    );
  }

  #[test]
  fn submit_and_first_terminal_only_and_bounded_eviction() {
    let events = ResultEvents::default();
    let (sink, writer) = sink();
    events.observe(&sink, "ui-1", "InProgress", Phase::Submit);
    for _ in 0..100 {
      events.observe(&sink, "ui-1", "InProgress", Phase::Followup);
    }
    events.observe(&sink, "ui-1", "Completed", Phase::Followup);
    events.observe(&sink, "ui-1", "Completed", Phase::Followup);
    for n in 0..1100 {
      events.observe(&sink, &format!("id-{n}"), "Completed", Phase::Submit);
    }
    assert_eq!(events.0.lock().unwrap().seen.len(), MAX_COMMANDS);
    assert!(!events.0.lock().unwrap().seen.contains_key("ui-1"));
    let lines: Vec<Value> = String::from_utf8(writer.0.lock().unwrap().clone())
      .unwrap()
      .lines()
      .map(|line| serde_json::from_str(line).unwrap())
      .collect();
    assert_eq!(
      lines
        .iter()
        .filter(|v| v["correlation_id"] == "ui-1")
        .count(),
      2
    );
  }

  #[test]
  fn terminal_followup_before_submit_still_emits_both_phases_once() {
    let events = ResultEvents::default();
    let (sink, writer) = sink();
    events.observe(&sink, "ui-race", "Completed", Phase::Followup);
    events.observe(&sink, "ui-race", "InProgress", Phase::Submit);
    events.observe(&sink, "ui-race", "Completed", Phase::Followup);
    events.observe(&sink, "ui-race", "InProgress", Phase::Submit);
    let bytes = writer.0.lock().unwrap().clone();
    let lines: Vec<Value> = String::from_utf8(bytes)
      .unwrap()
      .lines()
      .map(|line| serde_json::from_str(line).unwrap())
      .collect();
    assert_eq!(lines.len(), 2);
    assert_eq!(
      lines[0]["fields"],
      serde_json::json!({"status":"Completed","phase":"followup"})
    );
    assert_eq!(
      lines[1]["fields"],
      serde_json::json!({"status":"InProgress","phase":"submit"})
    );
    let seen = events.0.lock().unwrap();
    assert!(seen.seen["ui-race"].submit_seen);
    assert_eq!(seen.seen["ui-race"].terminal_status, Some("Completed"));
  }

  struct CountingBroken(Arc<AtomicUsize>);
  impl Write for CountingBroken {
    fn write(&mut self, _: &[u8]) -> io::Result<usize> {
      self.0.fetch_add(1, Ordering::SeqCst);
      Err(io::ErrorKind::BrokenPipe.into())
    }
    fn flush(&mut self) -> io::Result<()> {
      Ok(())
    }
  }

  #[test]
  fn sink_failure_never_retries_and_http_success_is_unchanged() {
    let writes = Arc::new(AtomicUsize::new(0));
    let sink = EventSink::new(
      Source::Bridge,
      "r_test".into(),
      CountingBroken(Arc::clone(&writes)),
      SystemClock,
    )
    .unwrap();
    let events = ResultEvents::default();
    for id in ["ui-1", "ui-2"] {
      let listener = TcpListener::bind("127.0.0.1:0").unwrap();
      let mut client = TcpStream::connect(listener.local_addr().unwrap()).unwrap();
      client
        .set_read_timeout(Some(std::time::Duration::from_secs(1)))
        .unwrap();
      let (mut server, _) = listener.accept().unwrap();
      let response = serde_json::json!({"ok":true,"result":{"command_id":id,"status":"Completed"}});
      crate::send_control_response(
        &mut server,
        response,
        None,
        Some(id),
        Some(Phase::Submit),
        &sink,
        &events,
      )
      .unwrap();
      drop(server);
      let mut body = String::new();
      client.read_to_string(&mut body).unwrap();
      assert!(body.starts_with("HTTP/1.1 200 OK"), "{body}");
      assert!(body.contains(id));
    }
    assert_eq!(writes.load(Ordering::SeqCst), 1);
    assert_eq!(events.0.lock().unwrap().seen.len(), 2);
  }

  #[test]
  fn parallel_same_id_emits_only_one_submit_and_one_terminal() {
    let events = Arc::new(ResultEvents::default());
    let (sink, writer) = sink();
    let sink = Arc::new(sink);
    std::thread::scope(|scope| {
      for _ in 0..16 {
        let events = Arc::clone(&events);
        let sink = Arc::clone(&sink);
        scope.spawn(move || events.observe(&sink, "ui-parallel", "InProgress", Phase::Submit));
      }
    });
    std::thread::scope(|scope| {
      for _ in 0..16 {
        let events = Arc::clone(&events);
        let sink = Arc::clone(&sink);
        scope.spawn(move || events.observe(&sink, "ui-parallel", "Completed", Phase::Followup));
      }
    });
    let bytes = writer.0.lock().unwrap().clone();
    let lines: Vec<Value> = String::from_utf8(bytes)
      .unwrap()
      .lines()
      .map(|line| serde_json::from_str(line).unwrap())
      .collect();
    assert_eq!(lines.len(), 2);
    assert_eq!(
      lines[0]["fields"],
      serde_json::json!({"status":"InProgress","phase":"submit"})
    );
    assert_eq!(
      lines[1]["fields"],
      serde_json::json!({"status":"Completed","phase":"followup"})
    );
  }
}
