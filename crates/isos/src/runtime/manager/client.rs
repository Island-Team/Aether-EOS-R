use super::*;

pub(crate) fn manager_call(paths: &config::Paths, name: &str, op: &str) -> Result<Value> {
  manager_call_revision(paths, name, op, None)
}

/// Read-only HTTP status has a short socket deadline. Stop and restart continue
/// through the unrestricted manager call so their safety budget is unchanged.
pub(crate) fn manager_status_for_http(paths: &config::Paths, name: &str) -> Result<Value> {
  let deadline = Instant::now() + Duration::from_secs(2);
  let request = WireRequest::new(WireCommand::DeploymentStatus(DeploymentTarget {
    deployment: name.to_owned(),
  }));
  let mut stream = UnixStream::connect(paths.socket()).map_err(|error| {
    anyhow::anyhow!("IsOS manager is not running; run `isos serve` first: {error}")
  })?;
  let mut request_bytes = serde_json::to_vec(&request)?;
  request_bytes.push(b'\n');
  let remaining = deadline.saturating_duration_since(Instant::now());
  ensure!(!remaining.is_zero(), "manager status deadline expired");
  stream.set_write_timeout(Some(remaining))?;
  stream.write_all(&request_bytes)?;
  ensure!(
    Instant::now() <= deadline,
    "manager status deadline expired"
  );
  let mut line = Vec::new();
  loop {
    let remaining = deadline.saturating_duration_since(Instant::now());
    ensure!(!remaining.is_zero(), "manager status deadline expired");
    stream.set_read_timeout(Some(remaining))?;
    let mut chunk = [0u8; 4096];
    let count = stream.read(&mut chunk)?;
    ensure!(count > 0, "manager status response ended early");
    line.extend_from_slice(&chunk[..count]);
    ensure!(
      line.len() <= 1024 * 1024,
      "manager status response is too large"
    );
    if let Some(end) = line.iter().position(|byte| *byte == b'\n') {
      line.truncate(end);
      break;
    }
  }
  ensure!(
    Instant::now() <= deadline,
    "manager status deadline expired"
  );
  let response: ApiResponse = serde_json::from_slice(&line)?;
  ensure!(
    response.schema_version == "0.1.0",
    "manager response schema_version must be 0.1.0"
  );
  if !response.ok {
    return Err(anyhow::Error::new(RemoteDiagnosticError {
      message: response
        .error
        .map_or_else(|| "manager error".into(), |error| error.message),
      data: response.data,
    }));
  }
  Ok(response.data)
}

pub(crate) fn manager_call_revision(
  paths: &config::Paths,
  name: &str,
  op: &str,
  revision: Option<String>,
) -> Result<Value> {
  let started_at = Instant::now();
  if matches!(
    op,
    "deployment_start" | "deployment_restart" | "services_start" | "services_restart"
  ) {
    check_manager_binary(paths, name)?;
    emit_phase_timing(
      op,
      "cli_manager_identity",
      started_at.elapsed().as_millis() as u64,
      started_at.elapsed().as_millis() as u64,
    );
  }
  let request = WireCommand::deployment_call(op, name, revision)?;
  manager_request(paths, request)
}

pub(crate) fn manager_request(paths: &config::Paths, request: WireCommand) -> Result<Value> {
  let op = request.op();
  let timed = matches!(request.op(), "deployment_start" | "deployment_restart");
  let started_at = Instant::now();
  let mut stream = UnixStream::connect(paths.socket()).map_err(|error| {
    anyhow::anyhow!("IsOS manager is not running; run `isos serve` first: {error}")
  })?;
  stream.set_write_timeout(Some(Duration::from_secs(5)))?;
  stream.set_read_timeout(Some(if timed {
    Duration::from_secs(120)
  } else {
    Duration::from_secs(15)
  }))?;
  serde_json::to_writer(&mut stream, &WireRequest::new(request))?;
  stream.write_all(b"\n")?;
  if timed {
    emit_phase_timing(
      op,
      "cli_socket_send",
      started_at.elapsed().as_millis() as u64,
      started_at.elapsed().as_millis() as u64,
    );
  }
  let mut line = String::new();
  BufReader::new(stream)
    .take(1_048_577)
    .read_line(&mut line)?;
  ensure!(line.len() <= 1_048_576, "manager response exceeds 1 MB");
  ensure!(line.ends_with('\n'), "manager response ended early");
  if timed {
    emit_phase_timing(
      op,
      "cli_response",
      started_at.elapsed().as_millis() as u64,
      started_at.elapsed().as_millis() as u64,
    );
  }
  let response: ApiResponse = serde_json::from_str(&line)?;
  ensure!(
    response.schema_version == "0.1.0",
    "manager response schema_version must be 0.1.0"
  );
  if !response.ok {
    return Err(anyhow::Error::new(RemoteDiagnosticError {
      message: response
        .error
        .map_or_else(|| "manager error".into(), |error| error.message),
      data: response.data,
    }));
  }
  Ok(response.data)
}

#[derive(Debug)]
pub(crate) struct RemoteDiagnosticError {
  pub message: String,
  pub data: Value,
}
impl std::fmt::Display for RemoteDiagnosticError {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(f, "{}", self.message)
  }
}
impl std::error::Error for RemoteDiagnosticError {}

pub(crate) fn check_manager_binary(paths: &config::Paths, name: &str) -> Result<()> {
  let active = manager_call(paths, name, "manager_identity")?;
  let own = hex::encode(Sha256::digest(fs::read(std::env::current_exe()?)?));
  ensure!(
    active["executable_sha256"] == own,
    "running IsOS manager uses a different binary; run `isos stop`, then `isos serve` with this version"
  );
  Ok(())
}

pub(crate) fn service_call(
  paths: &config::Paths,
  name: &str,
  op: &str,
  id: Option<String>,
  limit: Option<usize>,
) -> Result<Value> {
  if matches!(op, "services_start" | "services_restart") {
    check_manager_binary(paths, name)?;
  }
  manager_request(paths, WireCommand::service_call(op, id, limit)?)
}
