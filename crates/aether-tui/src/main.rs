//! A deliberately small REPL on the EOS v2 control socket.

use std::env;
use std::io::{self, BufRead, Write};
use std::path::PathBuf;

use serde_json::{json, Value};

fn main() -> Result<(), Box<dyn std::error::Error>> {
  let mut args = env::args().skip(1);
  let socket = match (args.next().as_deref(), args.next(), args.next()) {
    (Some("--socket"), Some(path), None) => PathBuf::from(path),
    _ => return Err("usage: aether-tui --socket <aether-runtime.sock>".into()),
  };
  let stdin = io::stdin();
  let mut stdout = io::stdout();
  println!("EOS v2 REPL. Commands: time, discover, health <path>, capture <path>, observe <path>, stop <path>, result <id>, dispatch <json>, cancel <id>, quit");
  let mut input = stdin.lock();
  loop {
    write!(stdout, "eos> ")?;
    stdout.flush()?;
    let mut line = String::new();
    if input.read_line(&mut line)? == 0 {
      break;
    }
    let line = line.trim();
    if line.is_empty() {
      continue;
    }
    if matches!(line, "quit" | "exit") {
      break;
    }
    let request = match parse(line) {
      Ok(request) => request,
      Err(error) => {
        eprintln!("{error}");
        continue;
      }
    };
    match aether_control::request(&socket, &request) {
      Ok(reply) => println!("{}", serde_json::to_string_pretty(&reply)?),
      Err(error) => eprintln!("{error}"),
    }
  }
  Ok(())
}

fn parse(line: &str) -> Result<Value, String> {
  if matches!(line, "time" | "discover") {
    return Ok(json!({"op":line}));
  }
  let (verb, argument) = line
    .split_once(' ')
    .ok_or("expected command and argument")?;
  let argument = argument.trim();
  if argument.is_empty() {
    return Err("argument is empty".into());
  }
  match verb {
    "health" | "capture" | "observe" | "stop" => Ok(json!({"op":verb,"node_path":argument})),
    "result" | "cancel" => Ok(json!({"op":verb,"command_id":argument})),
    "dispatch" => {
      let value: Value = serde_json::from_str(argument).map_err(|error| error.to_string())?;
      if value.get("op").and_then(Value::as_str) != Some("dispatch") {
        return Err("dispatch JSON must contain op=dispatch".into());
      }
      Ok(value)
    }
    _ => Err(format!("unknown command: {verb}")),
  }
}

#[cfg(test)]
mod tests {
  use super::parse;

  #[test]
  fn maps_repl_commands_to_control_requests() {
    assert_eq!(
      parse("health cores/eos/layers/hardware/groups/base/nodes/drive").unwrap()["op"],
      "health"
    );
    assert!(parse("dispatch {}").is_err());
    assert!(parse("delete anything").is_err());
  }
}
