//! Loopback configuration API and verified static IsosUI server.
use crate::{
  app::config::{self, Paths},
  runtime::owners::core_setup,
  domain::graph::graph_compat,
  domain::graph::graph_migration,
  runtime::lock,
  runtime::logs::LogStore,
  domain::graph::node_options,
  domain::graph::node_sets,
  domain::packages,
  runtime::process,
  runtime::process::reader_features,
  runtime::process::readiness,
  runtime::process::skill_selection,
};
use anyhow::{Context, Result};
use axum::{
  Json, Router,
  body::Body,
  extract::{Path as UrlPath, State},
  http::{HeaderMap, HeaderValue, Method, Request, StatusCode, header},
  middleware::{self, Next},
  response::{IntoResponse, Response},
  routing::{get, post},
};
use serde::Deserialize;
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::{
  fs,
  net::SocketAddr,
  path::PathBuf,
  sync::{Arc, OnceLock},
};
use tokio::sync::Semaphore;
use tower_http::services::{ServeDir, ServeFile};

struct App {
  paths: Paths,
  deployment: String,
}

// Keep expensive verification and owner catalog calls off Tokio workers and
// bound simultaneous hashing when several workbench panels refresh together.
fn catalog_slots() -> &'static Arc<Semaphore> {
  static SLOTS: OnceLock<Arc<Semaphore>> = OnceLock::new();
  SLOTS.get_or_init(|| Arc::new(Semaphore::new(2)))
}

async fn selected_catalog_read(paths: Paths, graph: Value) -> Result<Value> {
  bounded_catalog_read(move || process::catalog_for_graph(&paths, &graph)).await
}

async fn bounded_catalog_read<F>(work: F) -> Result<Value>
where
  F: FnOnce() -> Result<Value> + Send + 'static,
{
  let slots = catalog_slots().clone();
  let permit = slots.acquire_owned().await?;
  tokio::task::spawn_blocking(move || {
    let _permit = permit;
    work()
  })
  .await?
}

mod transport;
pub(crate) use transport::{local_origin, local_port, ui_port, ui_url};
use transport::{authorized, cors, preflight};
mod response;
use response::*;
fn package_err(error: &anyhow::Error, id: &str, version: &str) -> (StatusCode, Json<Value>) {
  let failure = error
    .chain()
    .find_map(|cause| cause.downcast_ref::<packages::PackageFailure>());
  let diagnostic = failure.map_or_else(|| json!({"code":"package_invalid_manifest","path":"",
        "message":format!("Reinstall or relink {id}@{version}"),"id":id,"version":version,"stage":"manifest"}),
        packages::PackageFailure::diagnostic);
  let status = if diagnostic["code"] == "package_version_exists" {
    StatusCode::CONFLICT
  } else {
    StatusCode::UNPROCESSABLE_ENTITY
  };
  let code = diagnostic["code"]
    .as_str()
    .unwrap_or("package_invalid_manifest")
    .to_owned();
  fail(
    status,
    code,
    format!("Reinstall or relink {id}@{version}"),
    json!({"diagnostics":[diagnostic]}),
  )
}
fn admission_err(error: &anyhow::Error) -> (StatusCode, Json<Value>) {
  if let Some(diagnostic) = error
    .chain()
    .find_map(|cause| cause.downcast_ref::<crate::runtime::owners::egia_state::Diagnostic>())
  {
    return fail(
      StatusCode::UNPROCESSABLE_ENTITY,
      diagnostic.code,
      "EGIA test environment rejected",
      json!({"diagnostics":[diagnostic]}),
    );
  }
  if let Some(diagnostic) = error
    .chain()
    .find_map(|cause| cause.downcast_ref::<crate::runtime::process::skill_selection::Diagnostic>())
  {
    return fail(
      StatusCode::UNPROCESSABLE_ENTITY,
      diagnostic.code,
      "skill selection rejected",
      json!({"diagnostics":[diagnostic]}),
    );
  }
  if let Some(failure) = error
    .chain()
    .find_map(|cause| cause.downcast_ref::<packages::PackageFailure>())
  {
    let diagnostic = failure.diagnostic();
    let code = diagnostic["code"]
      .as_str()
      .unwrap_or("package_invalid")
      .to_owned();
    return fail(
      StatusCode::UNPROCESSABLE_ENTITY,
      code,
      "package admission rejected",
      json!({"diagnostics":[diagnostic]}),
    );
  }
  internal_err(StatusCode::UNPROCESSABLE_ENTITY, error)
}
fn admission_diagnostic(error: &anyhow::Error) -> Value {
  if let Some(diagnostic) = error
    .chain()
    .find_map(|cause| cause.downcast_ref::<crate::runtime::owners::egia_state::Diagnostic>())
  {
    return json!(diagnostic);
  }
  if let Some(diagnostic) = error
    .chain()
    .find_map(|cause| cause.downcast_ref::<crate::runtime::process::skill_selection::Diagnostic>())
  {
    return json!(diagnostic);
  }
  error
    .chain()
    .find_map(|cause| cause.downcast_ref::<graph_compat::Diagnostic>())
    .map(|diagnostic| json!(diagnostic))
    .or_else(|| {
      error
        .chain()
        .find_map(|cause| cause.downcast_ref::<packages::PackageFailure>())
        .map(packages::PackageFailure::diagnostic)
    })
    .unwrap_or_else(
      || json!({"path":"/","code":"invalid_graph","message":"graph admission rejected"}),
    )
}
fn node_option_err(error: &anyhow::Error) -> (StatusCode, Json<Value>) {
  if let Some(diagnostic) = error
    .chain()
    .find_map(|cause| cause.downcast_ref::<graph_compat::Diagnostic>())
  {
    let code = diagnostic.code;
    return fail(
      StatusCode::UNPROCESSABLE_ENTITY,
      code,
      "graph admission rejected",
      json!({"diagnostics":[diagnostic]}),
    );
  }
  admission_err(error)
}
fn record(app: &App, source: &str, message: &str) {
  if let Ok(logs) = LogStore::new(&app.paths, &app.deployment) {
    let _ = logs.append(source, "info", message);
  }
}

fn record_graph_failure(app: &App, result: &ApiResult, apply: bool) {
  let body = match result {
    Ok(Json(body)) if body["data"]["valid"] == false => body,
    Err((StatusCode::FORBIDDEN | StatusCode::UNAUTHORIZED, _)) => return,
    Err((_, Json(body))) => body,
    _ => return,
  };
  if let Ok(logs) = LogStore::new(&app.paths, &app.deployment) {
    let diagnostic = if body["ok"] == false {
      &body["error"]["details"]["diagnostics"][0]
    } else {
      &body["data"]["diagnostics"][0]
    };
    let mut fields = serde_json::Map::new();
    if let Some(code) = diagnostic["code"].as_str().filter(|s| s.len() <= 256) {
      fields.insert("diagnostic_code".into(), json!(code));
    }
    if let Some(path) = diagnostic["path"]
      .as_str()
      .filter(|s| s.len() <= 256 && !s.chars().any(char::is_control))
    {
      fields.insert("diagnostic_path".into(), json!(path));
    }
    let correlation = crate::runtime::manager::next_correlation_id();
    let manager_scope = logs
      .manager_scope_id()
      .unwrap_or_else(|| crate::runtime::manager::manager_session_id().to_owned());
    let append = if apply {
      LogStore::append_apply_failure_context
    } else {
      LogStore::append_event_context
    };
    let _ = append(
      &logs,
      "isos",
      "error",
      "deployment_preflight_failed",
      "Deployment preflight failed before launch",
      crate::runtime::logs::LogContext {
        run_id: Some(&manager_scope),
        correlation_id: Some(&correlation),
        node_path: None,
        fields: Value::Object(fields),
      },
    );
  }
}
fn record_package(
  app: &App,
  level: &str,
  code: &str,
  id: &str,
  version: &str,
  _source_kind: &str,
  path: Option<&std::path::Path>,
) {
  if let Ok(logs) = LogStore::new(&app.paths, &app.deployment) {
    let receipt = if path.is_some() {
      match packages::verify(&app.paths, id, version) {
        Ok(verified) => Some(verified.receipt),
        Err(error) => {
          tracing::warn!(%error, "package receipt unavailable while recording event");
          None
        }
      }
    } else {
      None
    };
    let correlation = crate::runtime::manager::next_correlation_id();
    let manager_scope = logs
      .manager_scope_id()
      .unwrap_or_else(|| crate::runtime::manager::manager_session_id().to_owned());
    let _ = logs.append_event_context("packages", level, code,
            if level == "error" { "Package verification or installation failed" } else if code == "package_verified" { "Verified package release recorded" } else { "Local package link recorded without a release manifest" },
            crate::runtime::logs::LogContext { run_id: Some(&manager_scope), correlation_id: Some(&correlation), node_path: None,
            fields: json!({"package_id":id,"package_version":version,
                "source_commit":receipt.as_ref().and_then(|receipt| receipt.source_commit.as_deref()),
                "asset_sha256":receipt.as_ref().and_then(|receipt| receipt.asset_sha256.as_deref()),
                "release_sha256":receipt.as_ref().and_then(|receipt| receipt.release_sha256.as_deref()),
                "payload_sha256":receipt.as_ref().and_then(|receipt| receipt.payload_sha256.as_deref()),
                "source_kind":receipt.as_ref().and_then(|receipt| receipt.source_kind.as_deref())}) });
  }
}
fn revision(bytes: &[u8]) -> String {
  hex::encode(Sha256::digest(bytes))
}

use routes::graph::*;

use routes::catalog::*;

use routes::presentations::*;

use routes::package_handlers::*;

use routes::runtime::*;

pub fn serve(paths: Paths, deployment: String) -> Result<()> {
  paths.prepare()?;
  let app = Arc::new(App { paths, deployment });
  let router = router(app);
  let runtime = tokio::runtime::Builder::new_multi_thread()
    .enable_all()
    .build()?;
  runtime.block_on(async {
    let address: SocketAddr = format!("127.0.0.1:{}", local_port()?).parse()?;
    let listener = tokio::net::TcpListener::bind(address)
      .await
      .context("bind IsOS loopback API")?;
    axum::serve(listener, router)
      .with_graceful_shutdown(async {
        while !crate::app::foreground::INTERRUPTED.load(std::sync::atomic::Ordering::SeqCst) {
          tokio::time::sleep(std::time::Duration::from_millis(50)).await;
        }
      })
      .await?;
    Ok(())
  })
}

pub fn serve_ui(directory: PathBuf) -> Result<()> {
  let index = directory.join("index.html");
  anyhow::ensure!(index.is_file(), "selected IsosUI index.html is missing");
  let runtime = tokio::runtime::Builder::new_multi_thread()
    .enable_all()
    .build()?;
  runtime.block_on(async {
    let address: SocketAddr = format!("127.0.0.1:{}", ui_port()?).parse()?;
    let listener = tokio::net::TcpListener::bind(address)
      .await
      .context("bind IsosUI loopback server")?;
    let files = ServeDir::new(directory).fallback(ServeFile::new(index));
    axum::serve(listener, Router::new().fallback_service(files))
      .with_graceful_shutdown(async {
        while !crate::app::foreground::INTERRUPTED.load(std::sync::atomic::Ordering::SeqCst) {
          tokio::time::sleep(std::time::Duration::from_millis(50)).await;
        }
      })
      .await?;
    Ok(())
  })
}
mod routes;
use routes::router;

#[cfg(test)]
mod tests {
  use super::*;
  use axum::body::to_bytes;
  use tower::ServiceExt;

  use std::io::{BufRead, BufReader};
  use std::os::unix::net::UnixListener;
  use std::time::{Duration, Instant};

  #[tokio::test]
  async fn presentation_request_rejects_unregistered_or_ambiguous_method() {
    let app = Arc::new(App {
      paths: Paths {
        config: PathBuf::from("/tmp/isos-presentation-test/config"),
        data: PathBuf::from("/tmp/isos-presentation-test/data"),
        runtime: PathBuf::from("/tmp/isos-presentation-test/run"),
      },
      deployment: "default".into(),
    });
    let request = Request::builder()
      .method(Method::POST)
      .uri("/api/isos/v1/presentations/select")
      .header(header::HOST, "127.0.0.1:16000")
      .header(header::CONTENT_TYPE, "application/json")
      .body(Body::from(
        json!({"schema_version":"0.1.0","node_path":"eos/interface/camera/front",
                    "port":"frame","accept":["image","image"],"run_id":"r_test",
                    "graph_revision":"a".repeat(64)})
        .to_string(),
      ))
      .unwrap();
    let response = router(app).oneshot(request).await.unwrap();
    assert_eq!(response.status(), StatusCode::BAD_REQUEST);
    let body: Value =
      serde_json::from_slice(&to_bytes(response.into_body(), 1_000_000).await.unwrap()).unwrap();
    assert_eq!(body["schema_version"], "0.1.0");
    assert_eq!(body["error"]["code"], "invalid_request");
  }

  #[tokio::test(flavor = "current_thread")]
  async fn stalled_manager_status_returns_503_without_blocking_http_worker() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    let listener = UnixListener::bind(paths.socket()).unwrap();
    let server = std::thread::spawn(move || {
      let (stream, _) = listener.accept().unwrap();
      let mut reader = BufReader::new(stream);
      let mut line = String::new();
      reader.read_line(&mut line).unwrap();
      assert!(line.contains("deployment_status"));
      std::thread::sleep(Duration::from_millis(2300));
    });
    let app = Arc::new(App {
      paths,
      deployment: "default".into(),
    });
    let request = Request::builder()
      .uri("/api/isos/v1/runtime/process-status")
      .header(header::HOST, "127.0.0.1:16000")
      .body(Body::empty())
      .unwrap();
    let started = Instant::now();
    let response = router(app).oneshot(request);
    tokio::pin!(response);
    assert!(
      tokio::time::timeout(Duration::from_millis(50), &mut response)
        .await
        .is_err()
    );
    assert!(started.elapsed() < Duration::from_millis(500));
    let response = response.await;
    assert!(started.elapsed() >= Duration::from_secs(2));
    let response = response.unwrap();
    assert_eq!(response.status(), StatusCode::SERVICE_UNAVAILABLE);
    let body: Value =
      serde_json::from_slice(&to_bytes(response.into_body(), 1_000_000).await.unwrap()).unwrap();
    assert_eq!(body["error"]["message"], "process status is unavailable");
    server.join().unwrap();
  }

  #[tokio::test(flavor = "current_thread")]
  async fn live_manager_status_does_not_repeat_slow_selected_verification() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    let listener = UnixListener::bind(paths.socket()).unwrap();
    let manager_paths = paths.clone();
    let server = std::thread::spawn(move || {
      let spec = process::ProcessSpec {
        id: "stub".into(),
        path: "isos/process/component/stub".into(),
        settings: serde_json::from_value(json!({"command":["/bin/sleep","30"]})).unwrap(),
        package: None,
        fingerprint: None,
      };
      let mut deployments = crate::runtime::manager::Deployments::new();
      crate::runtime::manager::restart_processes(
        &manager_paths,
        &mut deployments,
        "test",
        "revision",
        &[spec],
      )
      .unwrap();
      let projection = json!({"gate_path":"eos/interface/gate/front","source_path":"eos/hardware/camera/front","projection_revision":"pinned","render":{"max_width_px":1280,"jpeg_quality":80},"target_fps":30});
      let run_id =
        crate::runtime::manager::test_set_vision_projection(&mut deployments, "test", projection);
      let mut services =
        crate::domain::services::ServiceManager::new(&manager_paths, "test").unwrap();
      crate::runtime::process::vision_binding::test_selected_template_delay(3000);
      let (stream, _) = listener.accept().unwrap();
      crate::runtime::manager::handle_client(
        &manager_paths,
        &mut deployments,
        &mut services,
        stream,
      )
      .unwrap();
      crate::runtime::process::vision_binding::test_selected_template_delay(0);
      run_id
    });
    let app = Arc::new(App {
      paths,
      deployment: "test".into(),
    });
    let request = Request::builder()
      .uri("/api/isos/v1/runtime/process-status")
      .header(header::HOST, "127.0.0.1:16000")
      .body(Body::empty())
      .unwrap();
    let started = Instant::now();
    let response = router(app).oneshot(request).await.unwrap();
    assert!(started.elapsed() < Duration::from_secs(2));
    assert_eq!(response.status(), StatusCode::OK);
    let body: Value =
      serde_json::from_slice(&to_bytes(response.into_body(), 1_000_000).await.unwrap()).unwrap();
    assert_eq!(body["data"]["state"], "running");
    let run_id = server.join().unwrap();
    assert_eq!(body["data"]["run_id"], run_id);
    assert_eq!(body["data"]["active_revision"], "revision");
    assert_eq!(
      body["data"]["vision_evidence"]["projection_revision"],
      "pinned"
    );
  }

  #[tokio::test(flavor = "current_thread")]
  #[ignore = "requires a selected EOS 0.2.0 build via ISOS_AETHER_BIN_DIR; run explicitly as a cross-owner integration test"]
  async fn vision_status_http_retains_run_pair_across_saved_edit_restart_and_stop() {
    use std::sync::{
      Arc as Shared, Mutex,
      atomic::{AtomicBool, Ordering},
    };

    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    let graph = crate::domain::graph::starter::draft(&paths, "test").unwrap();
    let graph_path = paths.node_graph_path("test");
    let source = serde_json::to_vec_pretty(&graph).unwrap();
    config::atomic_write(&graph_path, &source).unwrap();
    let old_revision = graph_compat::revision(&source);
    let spec = process::ProcessSpec {
      id: "stub".into(),
      path: "isos/process/component/stub".into(),
      settings: serde_json::from_value(json!({"command":["/bin/sleep","30"]})).unwrap(),
      package: None,
      fingerprint: None,
    };
    let deployments = Shared::new(Mutex::new(crate::runtime::manager::Deployments::new()));
    {
      let mut active = deployments.lock().unwrap();
      crate::runtime::manager::restart_processes(
        &paths,
        &mut active,
        "test",
        &old_revision,
        std::slice::from_ref(&spec),
      )
      .unwrap();
    }
    let template = |revision: &str, projection_revision: &str| {
      crate::runtime::process::vision_binding::Template {
        schema_version: 2,
        graph_revision: revision.to_owned(),
        projection_revision: projection_revision.to_owned(),
        source_path: "eos/hardware/camera/front".into(),
        gate_path: "eos/interface/gate/front".into(),
        ingress_type: "egia.vision.ingress@v0.1.frame".into(),
        render: crate::runtime::process::vision_binding::Render {
          max_width_px: 1280,
          jpeg_quality: 80,
        },
        target_fps: 30,
        endpoint_kind: "uds".into(),
        endpoint_name: "eos-vision-owner.sock".into(),
      }
    };
    let old_template = template(&old_revision, &"b".repeat(64));
    let old_projection = crate::runtime::process::vision_binding::projection(&old_template);
    let old_run = crate::runtime::manager::test_admit_vision_template(
      &mut deployments.lock().unwrap(),
      "test",
      old_template,
    );

    let listener = UnixListener::bind(paths.socket()).unwrap();
    listener.set_nonblocking(true).unwrap();
    let serving = Shared::new(AtomicBool::new(true));
    let (calibrated_tx, calibrated_rx) = std::sync::mpsc::sync_channel(1);
    let server = {
      let serving = serving.clone();
      let deployments = deployments.clone();
      let manager_paths = paths.clone();
      let old_revision = old_revision.clone();
      std::thread::spawn(move || {
        struct ResetDelay;
        impl Drop for ResetDelay {
          fn drop(&mut self) {
            crate::runtime::process::vision_binding::test_selected_template_delay(0);
          }
        }
        let _reset = ResetDelay;
        let mut services =
          crate::domain::services::ServiceManager::new(&manager_paths, "test").unwrap();
        crate::runtime::process::vision_binding::test_selected_template_delay(3_000);
        let calibration_started = Instant::now();
        let _ = crate::runtime::process::vision_binding::selected_template(
          &manager_paths,
          "test",
          &old_revision,
        );
        calibrated_tx.send(calibration_started.elapsed()).unwrap();
        while serving.load(Ordering::Relaxed) {
          match listener.accept() {
            Ok((stream, _)) => {
              crate::runtime::manager::handle_client(
                &manager_paths,
                &mut deployments.lock().unwrap(),
                &mut services,
                stream,
              )
              .unwrap();
            }
            Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {
              std::thread::sleep(Duration::from_millis(1));
            }
            Err(error) => panic!("manager socket accept failed: {error}"),
          }
        }
      })
    };
    let selected_read_elapsed = calibrated_rx.recv_timeout(Duration::from_secs(10)).unwrap();
    eprintln!(
      "Q03 selected_template calibrated_ms={}",
      selected_read_elapsed.as_millis()
    );
    assert!(
      selected_read_elapsed >= Duration::from_secs(3),
      "selected-template delay did not run on the manager thread"
    );
    let app = Arc::new(App {
      paths: paths.clone(),
      deployment: "test".into(),
    });
    let status = |app: Arc<App>| async move {
      let request = Request::builder()
        .uri("/api/isos/v1/runtime/process-status")
        .header(header::HOST, "127.0.0.1:16000")
        .body(Body::empty())
        .unwrap();
      let started = Instant::now();
      let response = router(app).oneshot(request).await.unwrap();
      let elapsed = started.elapsed();
      eprintln!("Q03 process_status elapsed_ms={}", elapsed.as_millis());
      assert!(elapsed < Duration::from_secs(2));
      assert_eq!(response.status(), StatusCode::OK);
      serde_json::from_slice::<Value>(&to_bytes(response.into_body(), 1_000_000).await.unwrap())
        .unwrap()["data"]
        .clone()
    };
    for _ in 0..5 {
      let body = status(app.clone()).await;
      assert_eq!(body["state"], "running");
      assert_eq!(body["saved_revision"], old_revision);
      assert_eq!(body["active_revision"], old_revision);
      assert_eq!(body["run_id"], old_run);
      assert_eq!(body["vision_evidence"], old_projection);
    }

    let mut edited_graph = graph;
    edited_graph["robot"]["name"] = json!("edited");
    let apply = Request::builder()
      .method(Method::PUT)
      .uri("/api/isos/v1/graph")
      .header(header::HOST, "127.0.0.1:16000")
      .header(header::CONTENT_TYPE, "application/json")
      .body(Body::from(
        serde_json::to_vec(
          &json!({"schema_version":"0.1.0","revision":old_revision,"graph":edited_graph}),
        )
        .unwrap(),
      ))
      .unwrap();
    let response = router(app.clone()).oneshot(apply).await.unwrap();
    let apply_status = response.status();
    let applied: Value =
      serde_json::from_slice(&to_bytes(response.into_body(), 1_000_000).await.unwrap()).unwrap();
    assert_eq!(apply_status, StatusCode::OK, "{applied}");
    let new_revision = applied["data"]["revision"].as_str().unwrap().to_owned();
    assert_ne!(new_revision, old_revision);
    let body = status(app.clone()).await;
    assert_eq!(body["saved_revision"], new_revision);
    assert_eq!(body["active_revision"], old_revision);
    assert_eq!(body["run_id"], old_run);
    assert_eq!(body["vision_evidence"], old_projection);

    // A stale revision rejects Restart before the running child is replaced.
    let failed_restart = Request::builder()
      .method(Method::POST)
      .uri("/api/isos/v1/runtime/control")
      .header(header::HOST, "127.0.0.1:16000")
      .header(header::CONTENT_TYPE, "application/json")
      .body(Body::from(
        serde_json::to_vec(
          &json!({"schema_version":"0.1.0","operation":"restart","revision":old_revision}),
        )
        .unwrap(),
      ))
      .unwrap();
    let response = router(app.clone()).oneshot(failed_restart).await.unwrap();
    assert_eq!(response.status(), StatusCode::UNPROCESSABLE_ENTITY);
    let body = status(app.clone()).await;
    assert_eq!(body["saved_revision"], new_revision);
    assert_eq!(body["active_revision"], old_revision);
    assert_eq!(body["run_id"], old_run);
    assert_eq!(body["vision_evidence"], old_projection);
    let old_pid = body["processes"][0]["pid"].as_i64().unwrap();

    {
      let mut active = deployments.lock().unwrap();
      crate::runtime::manager::restart_processes(
        &paths,
        &mut active,
        "test",
        &new_revision,
        &[spec],
      )
      .unwrap();
    }
    let new_template = template(&new_revision, &"c".repeat(64));
    let new_projection = crate::runtime::process::vision_binding::projection(&new_template);
    let new_run = crate::runtime::manager::test_admit_vision_template(
      &mut deployments.lock().unwrap(),
      "test",
      new_template,
    );
    assert_ne!(new_run, old_run);
    let body = status(app.clone()).await;
    assert_eq!(body["saved_revision"], new_revision);
    assert_eq!(body["active_revision"], new_revision);
    assert_eq!(body["run_id"], new_run);
    assert_eq!(body["vision_evidence"], new_projection);
    assert_ne!(body["processes"][0]["pid"], old_pid);
    assert_eq!(unsafe { libc::kill(old_pid as libc::pid_t, 0) }, -1);
    let new_pid = body["processes"][0]["pid"].as_i64().unwrap();

    let stop = Request::builder()
      .method(Method::POST)
      .uri("/api/isos/v1/runtime/control")
      .header(header::HOST, "127.0.0.1:16000")
      .header(header::CONTENT_TYPE, "application/json")
      .body(Body::from(
        serde_json::to_vec(&json!({"schema_version":"0.1.0","operation":"stop"})).unwrap(),
      ))
      .unwrap();
    let response = router(app.clone()).oneshot(stop).await.unwrap();
    assert_eq!(response.status(), StatusCode::OK);
    let body = status(app).await;
    assert_eq!(body["state"], "stopped");
    assert_eq!(body["run_id"], new_run);
    assert!(body["vision_evidence"].is_null());
    assert!(body["processes"].as_array().unwrap().is_empty());
    assert_eq!(unsafe { libc::kill(new_pid as libc::pid_t, 0) }, -1);
    serving.store(false, Ordering::Relaxed);
    server.join().unwrap();
  }

  #[tokio::test(flavor = "current_thread")]
  async fn selected_catalog_queue_is_bounded_and_does_not_block_runtime() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    let graph = crate::domain::graph::starter::draft(&paths, "default").unwrap();
    let first = catalog_slots().clone().acquire_owned().await.unwrap();
    let second = catalog_slots().clone().acquire_owned().await.unwrap();
    let pending = selected_catalog_read(paths, graph);
    tokio::pin!(pending);
    assert!(
      tokio::time::timeout(Duration::from_millis(25), &mut pending)
        .await
        .is_err()
    );
    drop(first);
    drop(second);
    assert!(
      tokio::time::timeout(Duration::from_secs(5), pending)
        .await
        .unwrap()
        .is_ok()
    );
  }

  #[tokio::test(flavor = "current_thread")]
  async fn global_type_and_node_set_reads_share_bounded_workers() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    let app = Arc::new(App {
      paths,
      deployment: "default".into(),
    });
    let first = catalog_slots().clone().acquire_owned().await.unwrap();
    let second = catalog_slots().clone().acquire_owned().await.unwrap();
    for route in ["node-types", "node-sets"] {
      let request = Request::builder()
        .uri(format!("/api/isos/v1/catalog/{route}"))
        .header(header::HOST, "127.0.0.1:16000")
        .body(Body::empty())
        .unwrap();
      let response = router(app.clone()).oneshot(request);
      tokio::pin!(response);
      assert!(
        tokio::time::timeout(Duration::from_millis(25), &mut response)
          .await
          .is_err()
      );
    }
    drop(first);
    drop(second);
    for route in ["node-types", "node-sets"] {
      let request = Request::builder()
        .uri(format!("/api/isos/v1/catalog/{route}"))
        .header(header::HOST, "127.0.0.1:16000")
        .body(Body::empty())
        .unwrap();
      let response = router(app.clone()).oneshot(request).await.unwrap();
      assert_eq!(response.status(), StatusCode::OK);
    }
  }

  #[tokio::test]
  async fn selected_catalog_is_bound_to_saved_bytes_and_keeps_global_discovery_separate() {
    let temp = tempfile::tempdir().unwrap();
    let app = Arc::new(App {
      paths: Paths {
        config: temp.path().join("config"),
        data: temp.path().join("data"),
        runtime: temp.path().join("run"),
      },
      deployment: "default".into(),
    });
    let request = Request::builder()
      .uri("/api/isos/v1/catalog/node-types/selected")
      .header(header::HOST, "127.0.0.1:16000")
      .body(Body::empty())
      .unwrap();
    let fresh = router(app.clone()).oneshot(request).await.unwrap();
    assert_eq!(fresh.status(), StatusCode::OK);
    let fresh_body: Value =
      serde_json::from_slice(&to_bytes(fresh.into_body(), 1_000_000).await.unwrap()).unwrap();
    assert_eq!(fresh_body["data"]["graph_revision"], "");
    assert!(fresh_body["data"]["catalog"]["types"].is_array());
    let graph = crate::domain::graph::starter::draft(&app.paths, "default").unwrap();
    let source = serde_json::to_vec_pretty(&graph).unwrap();
    let path = app.paths.node_graph_path("default");
    config::atomic_write(&path, &source).unwrap();
    let request = Request::builder()
      .uri("/api/isos/v1/catalog/node-types/selected")
      .header(header::HOST, "127.0.0.1:16000")
      .body(Body::empty())
      .unwrap();
    let response = router(app.clone()).oneshot(request).await.unwrap();
    assert_eq!(response.status(), StatusCode::OK);
    let body: Value =
      serde_json::from_slice(&to_bytes(response.into_body(), 1_000_000).await.unwrap()).unwrap();
    assert_eq!(
      body["data"]["graph_revision"],
      graph_compat::revision(&source)
    );
    assert!(body["data"]["catalog"]["types"].as_array().is_some());
    assert_eq!(fs::read(&path).unwrap(), source);

    // A client must reject this prior response after the exact saved bytes change.
    let changed = serde_json::to_vec(&graph).unwrap();
    config::atomic_write(&path, &changed).unwrap();
    let request = Request::builder()
      .uri("/api/isos/v1/catalog/node-types/selected")
      .header(header::HOST, "127.0.0.1:16000")
      .body(Body::empty())
      .unwrap();
    let response = router(app.clone()).oneshot(request).await.unwrap();
    let new_body: Value =
      serde_json::from_slice(&to_bytes(response.into_body(), 1_000_000).await.unwrap()).unwrap();
    assert_eq!(
      new_body["data"]["graph_revision"],
      graph_compat::revision(&changed)
    );
    assert_ne!(
      new_body["data"]["graph_revision"],
      body["data"]["graph_revision"]
    );
    assert_eq!(fs::read(&path).unwrap(), changed);

    let request = Request::builder()
      .uri("/api/isos/v1/catalog/node-types")
      .header(header::HOST, "127.0.0.1:16000")
      .body(Body::empty())
      .unwrap();
    let response = router(app).oneshot(request).await.unwrap();
    let global: Value =
      serde_json::from_slice(&to_bytes(response.into_body(), 1_000_000).await.unwrap()).unwrap();
    assert!(global["data"].get("graph_revision").is_none());
  }

  #[tokio::test]
  async fn selected_catalog_owner_failure_returns_raw_read_only_graph() {
    let temp = tempfile::tempdir().unwrap();
    let app = Arc::new(App {
      paths: Paths {
        config: temp.path().join("config"),
        data: temp.path().join("data"),
        runtime: temp.path().join("run"),
      },
      deployment: "default".into(),
    });
    let mut graph = crate::domain::graph::starter::draft(&app.paths, "default").unwrap();
    graph["cores"]["eos"]["layers"]["dispatcher"]["groups"]["control"]["nodes"]["command"]["package"] =
      json!({"id":"test/unavailable","version":"0.1.0"});
    let source = serde_json::to_vec_pretty(&graph).unwrap();
    let path = app.paths.node_graph_path("default");
    config::atomic_write(&path, &source).unwrap();
    let request = Request::builder()
      .uri("/api/isos/v1/catalog/node-types/selected")
      .header(header::HOST, "127.0.0.1:16000")
      .body(Body::empty())
      .unwrap();
    let response = router(app.clone()).oneshot(request).await.unwrap();
    let status = response.status();
    let failed: Value =
      serde_json::from_slice(&to_bytes(response.into_body(), 1_000_000).await.unwrap()).unwrap();
    assert_eq!(status, StatusCode::SERVICE_UNAVAILABLE, "{failed}");
    assert_eq!(
      failed["error"]["details"]["graph_revision"],
      graph_compat::revision(&source)
    );
    assert_eq!(
      failed["error"]["details"]["diagnostics"][0]["code"],
      "selected_catalog_unavailable"
    );
    assert!(!failed.to_string().contains(temp.path().to_str().unwrap()));
    assert!(!failed.to_string().contains("No such file"));
    let request = Request::builder()
      .uri("/api/isos/v1/graph")
      .header(header::HOST, "127.0.0.1:16000")
      .body(Body::empty())
      .unwrap();
    let response = router(app).oneshot(request).await.unwrap();
    assert_eq!(response.status(), StatusCode::OK);
    let body: Value =
      serde_json::from_slice(&to_bytes(response.into_body(), 1_000_000).await.unwrap()).unwrap();
    assert_eq!(body["data"]["read_only"], true);
    assert_eq!(body["data"]["graph"], graph);
    assert_eq!(
      body["data"]["diagnostics"][0]["code"],
      "selected_catalog_unavailable"
    );
    assert_eq!(fs::read(&path).unwrap(), source);
  }

  #[tokio::test]
  async fn unknown_saved_type_is_not_granted_editor_access() {
    let temp = tempfile::tempdir().unwrap();
    let app = Arc::new(App {
      paths: Paths {
        config: temp.path().join("config"),
        data: temp.path().join("data"),
        runtime: temp.path().join("run"),
      },
      deployment: "default".into(),
    });
    let mut graph = crate::domain::graph::starter::draft(&app.paths, "default").unwrap();
    graph["cores"]["eos"]["layers"]["dispatcher"]["groups"]["control"]["nodes"]["command"]["node_type"] =
      json!("eos.future.command@v9");

    let bytes = serde_json::to_vec_pretty(&graph).unwrap();
    let path = app.paths.node_graph_path("default");
    config::atomic_write(&path, &bytes).unwrap();
    let request = Request::builder()
      .uri("/api/isos/v1/graph")
      .header(header::HOST, "127.0.0.1:16000")
      .body(Body::empty())
      .unwrap();
    let response = router(app.clone()).oneshot(request).await.unwrap();
    assert_eq!(response.status(), StatusCode::OK);
    let body: Value =
      serde_json::from_slice(&to_bytes(response.into_body(), 1_000_000).await.unwrap()).unwrap();
    assert_eq!(body["data"]["read_only"], true);
    assert_eq!(body["data"]["revision"], graph_compat::revision(&bytes));
    assert_eq!(body["data"]["graph"], graph);
    assert!(
      body["data"]["diagnostics"]
        .as_array()
        .is_some_and(|issues| issues.iter().any(|item| item["path"]
          .as_str()
          .is_some_and(|path| path.ends_with("/node_type"))))
    );
    let request = Request::builder()
      .uri("/api/isos/v1/catalog/node-types/selected")
      .header(header::HOST, "127.0.0.1:16000")
      .body(Body::empty())
      .unwrap();
    let response = router(app).oneshot(request).await.unwrap();
    assert_eq!(response.status(), StatusCode::UNPROCESSABLE_ENTITY);
    let body: Value =
      serde_json::from_slice(&to_bytes(response.into_body(), 1_000_000).await.unwrap()).unwrap();
    assert_eq!(
      body["error"]["details"]["diagnostics"][0]["code"],
      "unsupported_node_type"
    );
    assert_eq!(
      body["error"]["details"]["diagnostics"][0]["path"],
      "/cores/eos/layers/dispatcher/groups/control/nodes/command/node_type"
    );
    assert_eq!(fs::read(path).unwrap(), bytes);
  }

  #[tokio::test]
  async fn future_graph_selected_catalog_reports_version_path_without_writing() {
    let temp = tempfile::tempdir().unwrap();
    let app = Arc::new(App {
      paths: Paths {
        config: temp.path().join("config"),
        data: temp.path().join("data"),
        runtime: temp.path().join("run"),
      },
      deployment: "default".into(),
    });
    let bytes = include_bytes!("../../../tests/fixtures/graph_compat/future_2_0_0.json");
    let path = app.paths.node_graph_path("default");
    config::atomic_write(&path, bytes).unwrap();
    let request = Request::builder()
      .uri("/api/isos/v1/catalog/node-types/selected")
      .header(header::HOST, "127.0.0.1:16000")
      .body(Body::empty())
      .unwrap();
    let response = router(app).oneshot(request).await.unwrap();
    assert_eq!(response.status(), StatusCode::UNPROCESSABLE_ENTITY);
    let body: Value =
      serde_json::from_slice(&to_bytes(response.into_body(), 1_000_000).await.unwrap()).unwrap();
    assert_eq!(
      body["error"]["details"]["graph_revision"],
      graph_compat::revision(bytes)
    );
    assert_eq!(
      body["error"]["details"]["diagnostics"][0]["path"],
      "/version"
    );
    assert_eq!(
      body["error"]["details"]["diagnostics"][0]["code"],
      "unsupported_graph_version"
    );
    assert_eq!(fs::read(path).unwrap(), bytes);
  }

  #[tokio::test]
  async fn legacy_validate_reports_skill_field_before_missing_package() {
    let temp = tempfile::tempdir().unwrap();
    let app = Arc::new(App {
      paths: Paths {
        config: temp.path().join("config"),
        data: temp.path().join("data"),
        runtime: temp.path().join("run"),
      },
      deployment: "default".into(),
    });
    let graph = json!({"version":"1.0.0","robot":{},"cores":{"isos":{"layers":{"process":{"groups":{"component":{"nodes":{"eos":{
        "id":"eos","node_type":"isos.process.component@v0.1","settings":{"role":"eos","skill_libraries":[{"package":{"id":"missing/skill","version":"0.1.0"},"library":"lib/example.skill.so"}]}
    }}}}}}}}});
    let Json(reply) = validate_node_graph_inner(
      State(app),
      HeaderMap::new(),
      Json(NodeGraphEdit {
        schema_version: json!("0.1.0"),
        graph,
        revision: None,
      }),
    )
    .await
    .unwrap();
    assert_eq!(
      reply["data"]["diagnostics"][0]["code"],
      "skill_selection_invalid"
    );
    assert_eq!(
      reply["data"]["diagnostics"][0]["path"],
      "/cores/isos/layers/process/groups/component/nodes/eos/settings/skill_libraries"
    );
  }

  #[test]
  fn graph_failure_event_keeps_structured_path_and_manager_session() {
    let temp = tempfile::tempdir().unwrap();
    let app = App {
      paths: Paths {
        config: temp.path().join("config"),
        data: temp.path().join("data"),
        runtime: temp.path().join("run"),
      },
      deployment: "default".into(),
    };
    let result = Ok(Json(json!({"data":{"valid":false,"diagnostics":[
        {"code":"unsupported_core","path":"/cores/evil"}
    ]}})));
    record_graph_failure(&app, &result, false);
    let lines = LogStore::new(&app.paths, "default")
      .unwrap()
      .read("isos", None, 10)
      .unwrap();
    let event = &lines["lines"][0];
    assert_eq!(event["event_code"], "deployment_preflight_failed");
    assert_eq!(
      event["run_id"],
      crate::runtime::manager::manager_session_id()
    );
    assert_eq!(event["fields"]["diagnostic_code"], "unsupported_core");
    assert_eq!(event["fields"]["diagnostic_path"], "/cores/evil");
    record_graph_failure(
      &app,
      &Ok(Json(json!({"data":{"valid":false,"diagnostics":[
          {"code":"unsupported_core","path":"/cores/bad\nkey"}
      ]}}))),
      false,
    );
    let lines = LogStore::new(&app.paths, "default")
      .unwrap()
      .read("isos", None, 10)
      .unwrap();
    assert_eq!(
      lines["lines"][1]["event_code"],
      "deployment_preflight_failed"
    );
    assert!(lines["lines"][1]["fields"]["diagnostic_path"].is_null());
    record_graph_failure(
      &app,
      &Err(err(
        StatusCode::FORBIDDEN,
        "cross-site request requires Origin",
      )),
      false,
    );
    assert_eq!(
      LogStore::new(&app.paths, "default")
        .unwrap()
        .read("isos", None, 10)
        .unwrap()["lines"]
        .as_array()
        .unwrap()
        .len(),
      2
    );
  }

  #[test]
  fn edge_diagnostic_names_the_wire_and_registered_data_types() {
    let graph = json!({"version":"1.0.0","cores":{"eos":{"layers":{"control":{"groups":{"manual":{"nodes":{"target":{"node_type":"target","inputs":{"status":{"node":"eos/control/manual/source","output":"out"}}},"source":{"node_type":"source"}}}}}}}}});
    let catalog = json!({"types":[
        {"id":"target","inputs":[{"name":"status","data_type":"status"}]},
        {"id":"source","outputs":[{"name":"out","data_type":"base_observation"}]}
    ]});
    let diagnostics = edge_diagnostics(&graph, &catalog);
    assert_eq!(diagnostics.len(), 1);
    assert_eq!(diagnostics[0]["code"], "edge_type_mismatch");
    assert_eq!(
      diagnostics[0]["path"],
      "/cores/eos/layers/control/groups/manual/nodes/target/inputs/status/0"
    );
    assert_eq!(diagnostics[0]["source"], "eos/control/manual/source");
    assert!(
      diagnostics[0]["message"]
        .as_str()
        .unwrap()
        .contains("base_observation")
    );
    let mut future = graph;
    future["version"] = json!("9.0.0");
    assert!(edge_diagnostics(&future, &catalog).is_empty());
  }

  #[tokio::test]
  async fn legacy_get_projects_separately_and_unmapped_validate_apply_keep_raw_bytes() {
    let temp = tempfile::tempdir().unwrap();
    let app = Arc::new(App {
      paths: Paths {
        config: temp.path().join("config"),
        data: temp.path().join("data"),
        runtime: temp.path().join("run"),
      },
      deployment: "default".into(),
    });
    let path = app.paths.node_graph_path("default");
    let mapped =
      include_bytes!("../../../tests/fixtures/graph_compat/legacy_optional_defaults_1_0_0.json");
    config::atomic_write(&path, mapped).unwrap();
    let get = Request::builder()
      .uri("/api/isos/v1/graph")
      .header(header::HOST, "127.0.0.1:16000")
      .body(Body::empty())
      .unwrap();
    let response = router(app.clone()).oneshot(get).await.unwrap();
    assert_eq!(response.status(), StatusCode::OK);
    let body: Value =
      serde_json::from_slice(&to_bytes(response.into_body(), 1_000_000).await.unwrap()).unwrap();
    assert_eq!(body["data"]["revision"], graph_compat::revision(mapped));
    assert_eq!(
      body["data"]["graph"],
      serde_json::from_slice::<Value>(mapped).unwrap()
    );
    assert_eq!(
      body["data"]["legacy_projection"]["graph"]["version"],
      "1.0.0"
    );
    assert_eq!(
      body["data"]["legacy_projection"]["source_revision"],
      graph_compat::revision(mapped)
    );
    assert!(body["data"]["legacy_projection"]["display_descriptions"].is_object());
    assert_eq!(fs::read(&path).unwrap(), mapped);

    let projection_input = body["data"]["legacy_projection"]["graph"].clone();
    let apply = Request::builder()
      .method(Method::PUT)
      .uri("/api/isos/v1/graph")
      .header(header::HOST, "127.0.0.1:16000")
      .header(header::CONTENT_TYPE, "application/json")
      .body(Body::from(
        serde_json::to_vec(&json!({"schema_version":"0.1.0",
                "revision":graph_compat::revision(mapped),"graph":projection_input}))
        .unwrap(),
      ))
      .unwrap();
    let response = router(app.clone()).oneshot(apply).await.unwrap();
    assert_eq!(response.status(), StatusCode::UNPROCESSABLE_ENTITY);
    let reply: Value =
      serde_json::from_slice(&to_bytes(response.into_body(), 1_000_000).await.unwrap()).unwrap();
    assert_eq!(
      reply["error"]["details"]["diagnostics"][0]["code"],
      "projection_read_only"
    );
    assert_eq!(fs::read(&path).unwrap(), mapped);

    let unmapped =
      include_bytes!("../../../tests/fixtures/graph_compat/legacy_unmapped_1_0_0.json");
    config::atomic_write(&path, unmapped).unwrap();
    let graph: Value = serde_json::from_slice(unmapped).unwrap();
    let get = Request::builder()
      .uri("/api/isos/v1/graph")
      .header(header::HOST, "127.0.0.1:16000")
      .body(Body::empty())
      .unwrap();
    let response = router(app.clone()).oneshot(get).await.unwrap();
    let body: Value =
      serde_json::from_slice(&to_bytes(response.into_body(), 1_000_000).await.unwrap()).unwrap();
    assert_eq!(body["data"]["read_only"], true);
    assert!(body["data"].get("legacy_projection").is_none());
    let expected = "/cores/eos/layers/control/groups/manual/nodes/manual_0/node_type";
    assert_eq!(body["data"]["diagnostics"][0]["path"], expected);
    assert_eq!(body["data"]["diagnostics"][0]["code"], "unmapped_node_type");
    let validate = Request::builder()
      .method(Method::POST)
      .uri("/api/isos/v1/graph/validate")
      .header(header::HOST, "127.0.0.1:16000")
      .header(header::CONTENT_TYPE, "application/json")
      .body(Body::from(
        serde_json::to_vec(&json!({"schema_version":"0.1.0","graph":graph})).unwrap(),
      ))
      .unwrap();
    let response = router(app.clone()).oneshot(validate).await.unwrap();
    let body: Value =
      serde_json::from_slice(&to_bytes(response.into_body(), 1_000_000).await.unwrap()).unwrap();
    assert_eq!(body["data"]["valid"], false);
    assert_eq!(body["data"]["diagnostics"][0]["path"], expected);
    let apply = Request::builder()
      .method(Method::PUT)
      .uri("/api/isos/v1/graph")
      .header(header::HOST, "127.0.0.1:16000")
      .header(header::CONTENT_TYPE, "application/json")
      .body(Body::from(
        serde_json::to_vec(&json!({"schema_version":"0.1.0",
                "revision":graph_compat::revision(unmapped),"graph":graph}))
        .unwrap(),
      ))
      .unwrap();
    let response = router(app.clone()).oneshot(apply).await.unwrap();
    assert_eq!(response.status(), StatusCode::UNPROCESSABLE_ENTITY);
    let body: Value =
      serde_json::from_slice(&to_bytes(response.into_body(), 1_000_000).await.unwrap()).unwrap();
    assert_eq!(body["error"]["details"]["diagnostics"][0]["path"], expected);
    let status = Request::builder()
      .uri("/api/isos/v1/cores/setup-status")
      .header(header::HOST, "127.0.0.1:16000")
      .body(Body::empty())
      .unwrap();
    let response = router(app.clone()).oneshot(status).await.unwrap();
    assert_eq!(response.status(), StatusCode::OK);
    let body: Value =
      serde_json::from_slice(&to_bytes(response.into_body(), 1_000_000).await.unwrap()).unwrap();
    assert_eq!(body["data"]["ready"], false);
    assert!(
      body["data"]["issues"]
        .as_array()
        .unwrap()
        .iter()
        .any(|issue| issue.as_str().is_some_and(
          |message| message.contains(expected) && message.contains("unmapped_node_type")
        ))
    );
    assert_eq!(fs::read(path).unwrap(), unmapped);
  }

  #[tokio::test]
  async fn core_setup_api_rejects_unknown_role_and_missing_graph() {
    for (route, expected) in [
      (
        "/api/isos/v1/cores/setup/other/catalog",
        StatusCode::NOT_FOUND,
      ),
      (
        "/api/isos/v1/cores/setup/eos/validate",
        StatusCode::BAD_REQUEST,
      ),
    ] {
      let request = Request::builder()
        .uri(route)
        .header(header::HOST, "127.0.0.1:16000")
        .body(Body::empty())
        .unwrap();
      let response = test_router().oneshot(request).await.unwrap();
      assert_eq!(response.status(), expected);
    }
  }

  #[tokio::test]
  async fn saved_future_graph_is_inspectable_but_read_only() {
    let temp = tempfile::tempdir().unwrap();
    let app = Arc::new(App {
      paths: Paths {
        config: temp.path().join("config"),
        data: temp.path().join("data"),
        runtime: temp.path().join("run"),
      },
      deployment: "default".into(),
    });
    let path = app.paths.node_graph_path("default");
    let source = include_bytes!("../../../tests/fixtures/graph_compat/future_2_0_0.json");
    config::atomic_write(&path, source).unwrap();
    let request = Request::builder()
      .uri("/api/isos/v1/graph")
      .header(header::HOST, "127.0.0.1:16000")
      .body(Body::empty())
      .unwrap();
    let response = router(app).oneshot(request).await.unwrap();
    assert_eq!(response.status(), StatusCode::OK);
    let body: Value =
      serde_json::from_slice(&to_bytes(response.into_body(), 1_000_000).await.unwrap()).unwrap();
    assert_eq!(body["data"]["revision"], graph_compat::revision(source));
    assert_eq!(body["data"]["graph"]["version"], "2.0.0");
    assert_eq!(body["data"]["read_only"], true);
    assert_eq!(body["data"]["diagnostics"][0]["path"], "/version");
    assert_eq!(fs::read(path).unwrap(), source);
  }

  #[tokio::test]
  async fn saved_new_graph_is_editable_and_missing_revision_apply_is_rejected() {
    let temp = tempfile::tempdir().unwrap();
    let app = Arc::new(App {
      paths: Paths {
        config: temp.path().join("config"),
        data: temp.path().join("data"),
        runtime: temp.path().join("run"),
      },
      deployment: "default".into(),
    });
    let graph = crate::domain::graph::starter::draft(&app.paths, "default").unwrap();
    let source = serde_json::to_vec_pretty(&graph).unwrap();
    config::atomic_write(&app.paths.node_graph_path("default"), &source).unwrap();
    let request = Request::builder()
      .uri("/api/isos/v1/graph")
      .header(header::HOST, "127.0.0.1:16000")
      .body(Body::empty())
      .unwrap();
    let response = router(app.clone()).oneshot(request).await.unwrap();
    assert_eq!(response.status(), StatusCode::OK);
    let body: Value =
      serde_json::from_slice(&to_bytes(response.into_body(), 1_000_000).await.unwrap()).unwrap();
    assert_eq!(body["data"]["graph"]["version"], "0.1.0");
    assert_eq!(body["data"]["read_only"], false, "{body}");
    let request = Request::builder()
      .method(Method::PUT)
      .uri("/api/isos/v1/graph")
      .header(header::HOST, "127.0.0.1:16000")
      .header(header::CONTENT_TYPE, "application/json")
      .body(Body::from(
        serde_json::to_vec(&json!({"schema_version":"0.1.0","graph":graph})).unwrap(),
      ))
      .unwrap();
    let response = router(app).oneshot(request).await.unwrap();
    assert_eq!(response.status(), StatusCode::BAD_REQUEST);
  }

  #[tokio::test]
  async fn unified_migration_http_checks_revisions_and_never_writes_on_owner_reject() {
    let temp = tempfile::tempdir().unwrap();
    let app = Arc::new(App {
      paths: Paths {
        config: temp.path().join("config"),
        data: temp.path().join("data"),
        runtime: temp.path().join("run"),
      },
      deployment: "default".into(),
    });
    let source =
      include_bytes!("../../../tests/fixtures/graph_compat/migration_expected_0_1_0.json");
    let path = app.paths.node_graph_path("default");
    config::atomic_write(&path, source).unwrap();
    let revision = graph_compat::revision(source);
    let saved = graph_compat::read(&path).unwrap();
    let proposed = crate::domain::graph::single_core::propose(&saved)
      .unwrap()
      .revision;
    for (old, candidate, expected) in [
      ("0".repeat(64), proposed.clone(), StatusCode::CONFLICT),
      (revision.clone(), "0".repeat(64), StatusCode::CONFLICT),
      (
        revision.clone(),
        proposed.clone(),
        StatusCode::UNPROCESSABLE_ENTITY,
      ),
    ] {
      let request = Request::builder()
        .method(Method::POST)
        .uri("/api/isos/v1/graph/migration/apply")
        .header(header::HOST, "127.0.0.1:16000")
        .header(header::CONTENT_TYPE, "application/json")
        .body(Body::from(
          serde_json::to_vec(
            &json!({"schema_version":"0.2.0","revision":old,"proposed_revision":candidate}),
          )
          .unwrap(),
        ))
        .unwrap();
      let response = router(app.clone()).oneshot(request).await.unwrap();
      assert_eq!(response.status(), expected);
      assert_eq!(fs::read(&path).unwrap(), source);
      assert!(!app.paths.generated("default").exists());
    }
  }

  /// Run with a prepared, isolated 0.1.0 home and real EOS/EI readers.
  #[tokio::test]
  #[ignore = "requires ISOS_TEST_UNIFIED_HOME and real owner readers"]
  async fn unified_migration_http_publishes_revisioned_snapshot_once() {
    let home = PathBuf::from(std::env::var("ISOS_TEST_UNIFIED_HOME").unwrap());
    assert!(home.is_absolute());
    let app = Arc::new(App {
      paths: Paths {
        config: home.join("config"),
        data: home.join("data"),
        runtime: home.join("run"),
      },
      deployment: "default".into(),
    });
    let path = app.paths.node_graph_path("default");
    let source = fs::read(&path).unwrap();
    let saved = graph_compat::parse(&source).unwrap();
    assert_eq!(saved.value["version"], "0.1.0");
    let preview_request = Request::builder()
      .method(Method::POST)
      .uri("/api/isos/v1/graph/migration/preview")
      .header(header::HOST, "127.0.0.1:16000")
      .header(header::CONTENT_TYPE, "application/json")
      .body(Body::from(
        serde_json::to_vec(&json!({"schema_version":"0.2.0","revision":saved.revision})).unwrap(),
      ))
      .unwrap();
    let preview_response = router(app.clone()).oneshot(preview_request).await.unwrap();
    assert_eq!(preview_response.status(), StatusCode::OK);
    let preview: Value = serde_json::from_slice(
      &to_bytes(preview_response.into_body(), 1_000_000)
        .await
        .unwrap(),
    )
    .unwrap();
    assert_eq!(preview["data"]["can_apply"], true, "{preview}");
    let proposed = preview["data"]["proposed_revision"].as_str().unwrap();
    let apply_request = Request::builder()
      .method(Method::POST)
      .uri("/api/isos/v1/graph/migration/apply")
      .header(header::HOST, "127.0.0.1:16000")
      .header(header::CONTENT_TYPE, "application/json")
      .body(Body::from(
        serde_json::to_vec(
          &json!({"schema_version":"0.2.0","revision":saved.revision,"proposed_revision":proposed}),
        )
        .unwrap(),
      ))
      .unwrap();
    let apply_response = router(app.clone()).oneshot(apply_request).await.unwrap();
    assert_eq!(apply_response.status(), StatusCode::OK);
    let applied = fs::read(&path).unwrap();
    assert_eq!(graph_compat::revision(&applied), proposed);
    assert_eq!(
      serde_json::from_slice::<Value>(&applied).unwrap()["version"],
      "0.2.0"
    );
    assert!(
      crate::domain::graph::generated_v2::selected_root(
        &app.paths,
        "default",
        &serde_json::from_slice(&applied).unwrap(),
        proposed
      )
      .is_ok()
    );
  }

  /// Run with a fresh isolated home containing verified packages and owner readers.
  #[tokio::test]
  #[ignore = "requires ISOS_TEST_UNIFIED_HOME and ISOS_TEST_UNIFIED_GRAPH_PATH"]
  async fn fresh_unified_http_apply_publishes_and_rejects_stale_revision() {
    let home = PathBuf::from(std::env::var("ISOS_TEST_UNIFIED_HOME").unwrap());
    let graph_path = PathBuf::from(std::env::var("ISOS_TEST_UNIFIED_GRAPH_PATH").unwrap());
    assert!(home.is_absolute() && graph_path.is_absolute());
    let app = Arc::new(App {
      paths: Paths {
        config: home.join("config"),
        data: home.join("data"),
        runtime: home.join("run"),
      },
      deployment: "default".into(),
    });
    let saved_path = app.paths.node_graph_path("default");
    assert!(
      !saved_path.exists(),
      "fresh writer test needs no saved graph"
    );
    let graph: Value = serde_json::from_slice(&fs::read(graph_path).unwrap()).unwrap();
    assert_eq!(graph["version"], "0.2.0");
    let request = Request::builder()
      .method(Method::PUT)
      .uri("/api/isos/v1/graph")
      .header(header::HOST, "127.0.0.1:16000")
      .header(header::CONTENT_TYPE, "application/json")
      .body(Body::from(
        serde_json::to_vec(&json!({"schema_version":"0.2.0","revision":"","graph":graph})).unwrap(),
      ))
      .unwrap();
    let response = router(app.clone()).oneshot(request).await.unwrap();
    let status = response.status();
    let body = to_bytes(response.into_body(), 1_000_000).await.unwrap();
    assert_eq!(status, StatusCode::OK, "{}", String::from_utf8_lossy(&body));
    let published = fs::read(&saved_path).unwrap();
    let revision = graph_compat::revision(&published);
    assert!(
      crate::domain::graph::generated_v2::selected_root(
        &app.paths,
        "default",
        &serde_json::from_slice(&published).unwrap(),
        &revision
      )
      .is_ok()
    );
    let request = Request::builder().method(Method::PUT).uri("/api/isos/v1/graph")
      .header(header::HOST, "127.0.0.1:16000")
      .header(header::CONTENT_TYPE, "application/json")
      .body(Body::from(serde_json::to_vec(&json!({"schema_version":"0.2.0","revision":"","graph":serde_json::from_slice::<Value>(&published).unwrap()})).unwrap())).unwrap();
    let response = router(app).oneshot(request).await.unwrap();
    assert_eq!(response.status(), StatusCode::CONFLICT);
    assert_eq!(fs::read(saved_path).unwrap(), published);
  }

  #[tokio::test]
  async fn changed_legacy_apply_reports_migration_without_writing() {
    let temp = tempfile::tempdir().unwrap();
    let app = Arc::new(App {
      paths: Paths {
        config: temp.path().join("config"),
        data: temp.path().join("data"),
        runtime: temp.path().join("run"),
      },
      deployment: "default".into(),
    });
    let path = app.paths.node_graph_path("default");
    let source =
      include_bytes!("../../../tests/fixtures/graph_compat/legacy_1_0_0_whitespace.json");
    config::atomic_write(&path, source).unwrap();
    let inspection = Request::builder()
      .uri("/api/isos/v1/graph")
      .header(header::HOST, "127.0.0.1:16000")
      .body(Body::empty())
      .unwrap();
    let inspected = router(app.clone()).oneshot(inspection).await.unwrap();
    let body: Value =
      serde_json::from_slice(&to_bytes(inspected.into_body(), 1_000_000).await.unwrap()).unwrap();
    assert_eq!(body["data"]["read_only"], false);
    config::atomic_write(
      &app
        .paths
        .generated("default")
        .join("core-setup/launch-inputs.json"),
      b"[]",
    )
    .unwrap();
    let inspection = Request::builder()
      .uri("/api/isos/v1/graph")
      .header(header::HOST, "127.0.0.1:16000")
      .body(Body::empty())
      .unwrap();
    let inspected = router(app.clone()).oneshot(inspection).await.unwrap();
    let body: Value =
      serde_json::from_slice(&to_bytes(inspected.into_body(), 1_000_000).await.unwrap()).unwrap();
    assert_eq!(body["data"]["read_only"], true);
    let mut changed: Value = serde_json::from_slice(source).unwrap();
    changed["robot"]["name"] = json!("changed");
    let request = Request::builder()
      .method(Method::PUT)
      .uri("/api/isos/v1/graph")
      .header(header::HOST, "127.0.0.1:16000")
      .header(header::CONTENT_TYPE, "application/json")
      .body(Body::from(
        serde_json::to_vec(&json!({"schema_version":"0.1.0",
                "revision":graph_compat::revision(source),"graph":changed}))
        .unwrap(),
      ))
      .unwrap();
    let response = router(app).oneshot(request).await.unwrap();
    assert_eq!(response.status(), StatusCode::UNPROCESSABLE_ENTITY);
    let body: Value =
      serde_json::from_slice(&to_bytes(response.into_body(), 1_000_000).await.unwrap()).unwrap();
    assert_eq!(
      body["error"]["details"]["diagnostics"][0]["code"],
      "migration_required"
    );
    assert_eq!(
      body["error"]["details"]["diagnostics"][0]["path"],
      "/version"
    );
    assert_eq!(fs::read(path).unwrap(), source);
  }

  #[tokio::test]
  async fn validation_rejects_future_version_and_unsafe_process_at_exact_paths() {
    for (source, path, code) in [
      (
        include_bytes!("../../../tests/fixtures/graph_compat/future_2_0_0.json").as_slice(),
        "/version",
        "unsupported_graph_version",
      ),
      (
        include_bytes!("../../../tests/fixtures/graph_compat/unsafe_component_1_0_0.json")
          .as_slice(),
        "/cores/isos/layers/process/groups/component/nodes/eos/node_type",
        "unsafe_process_node",
      ),
      (
        include_bytes!("../../../tests/fixtures/graph_compat/unknown_type_1_0_0.json").as_slice(),
        "/cores/eos/layers/dispatcher/groups/control/nodes/command/node_type",
        "unsupported_node_type",
      ),
    ] {
      let graph: Value = serde_json::from_slice(source).unwrap();
      let request = Request::builder()
        .method(Method::POST)
        .uri("/api/isos/v1/graph/validate")
        .header(header::HOST, "127.0.0.1:16000")
        .header(header::CONTENT_TYPE, "application/json")
        .body(Body::from(
          serde_json::to_vec(&json!({"schema_version":"0.1.0","graph":graph})).unwrap(),
        ))
        .unwrap();
      let response = test_router().oneshot(request).await.unwrap();
      assert_eq!(response.status(), StatusCode::OK);
      let body: Value =
        serde_json::from_slice(&to_bytes(response.into_body(), 1_000_000).await.unwrap()).unwrap();
      assert_eq!(body["data"]["valid"], false);
      assert_eq!(body["data"]["diagnostics"][0]["path"], path);
      assert_eq!(body["data"]["diagnostics"][0]["code"], code);
    }
  }

  #[tokio::test]
  async fn q01_egia_test_flag_validate_and_apply_reject_exact_graph_paths() {
    let fixture: Value = serde_json::from_str(include_str!(
      "../../../tests/fixtures/q01_egia_stale_env/cases.json"
    ))
    .unwrap();
    for case in fixture["cases"].as_array().unwrap() {
      let temp = tempfile::tempdir().unwrap();
      let app = Arc::new(App {
        paths: Paths {
          config: temp.path().join("config"),
          data: temp.path().join("data"),
          runtime: temp.path().join("run"),
        },
        deployment: "default".into(),
      });
      let mut graph = crate::domain::graph::starter::draft(&app.paths, "default").unwrap();
      graph["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]["egia"] = json!({"id":"egia",
                    "node_type":"isos.process.component@v0.1",
                    "settings":case["settings"]});
      let path = app.paths.node_graph_path("default");
      let saved = serde_json::to_vec_pretty(
        &crate::domain::graph::starter::draft(&app.paths, "default").unwrap(),
      )
      .unwrap();
      config::atomic_write(&path, &saved).unwrap();
      let revision = graph_compat::revision(&saved);
      let validate = Request::builder()
        .method(Method::POST)
        .uri("/api/isos/v1/graph/validate")
        .header(header::HOST, "127.0.0.1:16000")
        .header(header::CONTENT_TYPE, "application/json")
        .body(Body::from(
          serde_json::to_vec(&json!({"schema_version":"0.1.0",
                    "graph":graph}))
          .unwrap(),
        ))
        .unwrap();
      let response = router(app.clone()).oneshot(validate).await.unwrap();
      assert_eq!(response.status(), StatusCode::OK);
      let body: Value =
        serde_json::from_slice(&to_bytes(response.into_body(), 1_000_000).await.unwrap()).unwrap();
      assert_eq!(body["data"]["valid"], false);
      assert_eq!(body["data"]["diagnostics"][0]["code"], fixture["code"]);
      assert_eq!(body["data"]["diagnostics"][0]["path"], case["path"]);
      assert_eq!(body["data"]["diagnostics"][0]["stage"], fixture["stage"]);
      let apply = Request::builder()
        .method(Method::PUT)
        .uri("/api/isos/v1/graph")
        .header(header::HOST, "127.0.0.1:16000")
        .header(header::CONTENT_TYPE, "application/json")
        .body(Body::from(
          serde_json::to_vec(&json!({"schema_version":"0.1.0",
                    "revision":revision,"graph":graph}))
          .unwrap(),
        ))
        .unwrap();
      let response = router(app.clone()).oneshot(apply).await.unwrap();
      assert_eq!(response.status(), StatusCode::UNPROCESSABLE_ENTITY);
      let body: Value =
        serde_json::from_slice(&to_bytes(response.into_body(), 1_000_000).await.unwrap()).unwrap();
      assert_eq!(
        body["error"]["details"]["diagnostics"][0]["code"],
        fixture["code"]
      );
      assert_eq!(
        body["error"]["details"]["diagnostics"][0]["path"],
        case["path"]
      );
      assert_eq!(
        body["error"]["details"]["diagnostics"][0]["stage"],
        fixture["stage"]
      );
      assert_eq!(fs::read(path).unwrap(), saved);
      assert!(!app.paths.runtime.join("control-grants").exists());
    }
  }

  fn test_router() -> Router {
    router(Arc::new(App {
      paths: Paths {
        config: PathBuf::from("/tmp/isos-test/config"),
        data: PathBuf::from("/tmp/isos-test/data"),
        runtime: PathBuf::from("/tmp/isos-test/run"),
      },
      deployment: "default".into(),
    }))
  }

  #[tokio::test]
  async fn obsolete_component_and_build_routes_are_absent() {
    for route in [
      "/api/isos/v2/graph",
      "/api/isos/v2/graph/validate",
      "/api/isos/v1/profiles",
      "/api/isos/v1/config/eos/describe",
      "/api/isos/v1/build-binaries",
      "/api/isos/v1/build-binaries/select",
    ] {
      let request = Request::builder()
        .uri(route)
        .header(header::HOST, "127.0.0.1:16000")
        .body(Body::empty())
        .unwrap();
      let response = test_router().oneshot(request).await.unwrap();
      assert_eq!(response.status(), StatusCode::NOT_FOUND, "{route}");
    }
  }

  #[tokio::test]
  async fn categorized_logs_return_bounded_cursor_envelope() {
    let temp = tempfile::tempdir().unwrap();
    let app = Arc::new(App {
      paths: Paths {
        config: temp.path().join("config"),
        data: temp.path().join("data"),
        runtime: temp.path().join("run"),
      },
      deployment: "default".into(),
    });
    let store = LogStore::new(&app.paths, "default").unwrap();
    store.append("isos", "info", "ready").unwrap();
    for (route, expected) in [
      (
        "/api/isos/v1/runtime/logs?source=isos&limit=1",
        StatusCode::OK,
      ),
      (
        "/api/isos/v1/runtime/logs?source=isos&limit=201",
        StatusCode::BAD_REQUEST,
      ),
      (
        "/api/isos/v1/runtime/logs?source=..%2Fsecret",
        StatusCode::BAD_REQUEST,
      ),
    ] {
      let request = Request::builder()
        .uri(route)
        .header(header::HOST, "127.0.0.1:16000")
        .body(Body::empty())
        .unwrap();
      let response = router(app.clone()).oneshot(request).await.unwrap();
      assert_eq!(response.status(), expected);
      if expected == StatusCode::OK {
        let bytes = to_bytes(response.into_body(), 1_000_000).await.unwrap();
        let body: Value = serde_json::from_slice(&bytes).unwrap();
        assert_eq!(body["data"]["lines"][0]["message"], "ready");
        assert_eq!(body["data"]["lines"][0]["source"], "isos");
        assert!(body["data"]["next_cursor"].is_string());
      }
    }
  }

  #[tokio::test]
  async fn current_logs_preserve_typed_errors_and_legacy_route() {
    let temp = tempfile::tempdir().unwrap();
    let app = Arc::new(App {
      paths: Paths {
        config: temp.path().join("config"),
        data: temp.path().join("data"),
        runtime: temp.path().join("run"),
      },
      deployment: "default".into(),
    });
    let store = LogStore::new(&app.paths, "default").unwrap();
    let run = "rhttp1";
    store.publish_run(run).unwrap();
    store
      .append_event_context(
        "isos",
        "info",
        "deployment_ready",
        "ready",
        crate::runtime::logs::LogContext {
          run_id: Some(run),
          correlation_id: None,
          node_path: None,
          fields: json!({}),
        },
      )
      .unwrap();
    for (route, status, code) in [
      ("/api/isos/v1/runtime/logs?view=current", StatusCode::OK, ""),
      (
        "/api/isos/v1/runtime/logs?view=current&cursor=123:456",
        StatusCode::BAD_REQUEST,
        "log_cursor_invalid",
      ),
      (
        "/api/isos/v1/runtime/logs?view=current&node_path=bad",
        StatusCode::BAD_REQUEST,
        "log_node_path_invalid",
      ),
      (
        "/api/isos/v1/runtime/logs?view=other",
        StatusCode::BAD_REQUEST,
        "log_view_invalid",
      ),
      ("/api/isos/v1/runtime/logs?source=isos", StatusCode::OK, ""),
    ] {
      let request = Request::builder()
        .uri(route)
        .header(header::HOST, "127.0.0.1:16000")
        .body(Body::empty())
        .unwrap();
      let response = router(app.clone()).oneshot(request).await.unwrap();
      assert_eq!(response.status(), status, "{route}");
      let body: Value =
        serde_json::from_slice(&to_bytes(response.into_body(), 1_000_000).await.unwrap()).unwrap();
      if code.is_empty() && route.contains("view=current") {
        assert_eq!(
          body["data"]["anchor"]["event"]["event_code"],
          "deployment_ready"
        );
        assert!(
          body["data"]["next_cursor"]
            .as_str()
            .unwrap()
            .starts_with("lc1.")
        );
      } else if !code.is_empty() {
        assert_eq!(body["error"]["details"]["diagnostics"][0]["code"], code);
      }
    }
  }

  #[tokio::test]
  async fn logs_filter_level_and_default_to_unified_stream() {
    let temp = tempfile::tempdir().unwrap();
    let app = Arc::new(App {
      paths: Paths {
        config: temp.path().join("config"),
        data: temp.path().join("data"),
        runtime: temp.path().join("run"),
      },
      deployment: "default".into(),
    });
    let store = LogStore::new(&app.paths, "default").unwrap();
    store.append("eos", "info", "ready").unwrap();
    store.append("eos", "error", "failed").unwrap();
    for (route, status, expected) in [
      (
        "/api/isos/v1/runtime/logs?source=eos&level=warn",
        StatusCode::OK,
        "failed",
      ),
      (
        "/api/isos/v1/runtime/logs?source=eos&level=verbose",
        StatusCode::BAD_REQUEST,
        "",
      ),
    ] {
      let request = Request::builder()
        .uri(route)
        .header(header::HOST, "127.0.0.1:16000")
        .body(Body::empty())
        .unwrap();
      let response = router(app.clone()).oneshot(request).await.unwrap();
      assert_eq!(response.status(), status);
      if status == StatusCode::OK {
        let bytes = to_bytes(response.into_body(), 1_000_000).await.unwrap();
        let body: Value = serde_json::from_slice(&bytes).unwrap();
        assert_eq!(body["data"]["lines"].as_array().unwrap().len(), 1);
        let line = &body["data"]["lines"][0];
        assert_eq!(
          line["message"].as_str().or_else(|| line.as_str()),
          Some(expected)
        );
      }
    }
    let request = Request::builder()
      .uri("/api/isos/v1/runtime/logs?limit=2")
      .header(header::HOST, "127.0.0.1:16000")
      .body(Body::empty())
      .unwrap();
    let response = router(app).oneshot(request).await.unwrap();
    assert_eq!(response.status(), StatusCode::OK);
    let bytes = to_bytes(response.into_body(), 1_000_000).await.unwrap();
    let body: Value = serde_json::from_slice(&bytes).unwrap();
    assert_eq!(body["data"]["lines"].as_array().unwrap().len(), 2);
  }

  #[tokio::test]
  async fn first_node_graph_read_returns_editable_v1_draft() {
    let temp = tempfile::tempdir().unwrap();
    let app = Arc::new(App {
      paths: Paths {
        config: temp.path().join("config"),
        data: temp.path().join("data"),
        runtime: temp.path().join("run"),
      },
      deployment: "default".into(),
    });
    let request = Request::builder()
      .uri("/api/isos/v1/graph")
      .header(header::HOST, "127.0.0.1:16000")
      .header(header::ORIGIN, "https://www.os-island.com")
      .body(Body::empty())
      .unwrap();
    let response = router(app).oneshot(request).await.unwrap();
    assert_eq!(response.status(), StatusCode::OK);
    let bytes = to_bytes(response.into_body(), 1_000_000).await.unwrap();
    let body: Value = serde_json::from_slice(&bytes).unwrap();
    assert_eq!(body["data"]["revision"], "");
    assert_eq!(body["data"]["graph"]["version"], "0.1.0");
  }

  #[tokio::test]
  async fn home_reports_missing_package_and_graph() {
    let temp = tempfile::tempdir().unwrap();
    let app = Arc::new(App {
      paths: Paths {
        config: temp.path().join("config"),
        data: temp.path().join("data"),
        runtime: temp.path().join("run"),
      },
      deployment: "default".into(),
    });
    for route in ["/api/isos/v1/packages", "/api/isos/v1/cores/setup-status"] {
      let request = Request::builder()
        .uri(route)
        .header(header::HOST, "127.0.0.1:16000")
        .header(header::ORIGIN, "https://www.os-island.com")
        .body(Body::empty())
        .unwrap();
      let response = router(app.clone()).oneshot(request).await.unwrap();
      assert_eq!(response.status(), StatusCode::OK);
      let bytes = to_bytes(response.into_body(), 1_000_000).await.unwrap();
      let body: Value = serde_json::from_slice(&bytes).unwrap();
      if route.ends_with("packages") {
        assert_eq!(body["data"]["installed"], json!([]));
      } else {
        assert_eq!(body["data"]["ready"], false);
        assert!(body["data"]["issues"].as_array().unwrap().len() >= 2);
      }
    }
  }

  #[tokio::test]
  async fn legacy_bundle_link_is_rejected_without_store_change() {
    let temp = tempfile::tempdir().unwrap();
    let bundle = temp.path().join("bundle");
    fs::create_dir(&bundle).unwrap();
    let app = Arc::new(App {
      paths: Paths {
        config: temp.path().join("config"),
        data: temp.path().join("data"),
        runtime: temp.path().join("run"),
      },
      deployment: "default".into(),
    });
    app.paths.prepare().unwrap();
    let request = Request::builder()
      .method(Method::POST)
      .uri("/api/isos/v1/packages/link")
      .header(header::HOST, "127.0.0.1:16000")
      .header(header::CONTENT_TYPE, "application/json")
      .body(Body::from(
        json!({"id":"test/sample","version":"1.0.0","path":bundle}).to_string(),
      ))
      .unwrap();
    let response = router(app.clone()).oneshot(request).await.unwrap();
    assert_eq!(response.status(), StatusCode::UNPROCESSABLE_ENTITY);
    let body: Value =
      serde_json::from_slice(&to_bytes(response.into_body(), 1_000_000).await.unwrap()).unwrap();
    assert_eq!(body["error"]["code"], "unsupported_package_contract");
    assert!(packages::list(&app.paths).unwrap().is_empty());
    assert!(
      !packages::package_path(&app.paths, "test/sample", "1.0.0")
        .unwrap()
        .exists()
    );
  }

  #[tokio::test]
  async fn loopback_and_trusted_browser_origins_connect_without_token() {
    let request = Request::builder()
      .method(Method::OPTIONS)
      .uri("/api/isos/v1/system/version")
      .header(header::HOST, "127.0.0.1:16000")
      .header(header::ORIGIN, "https://www.os-island.com")
      .header(header::ACCESS_CONTROL_REQUEST_METHOD, "GET")
      .header(header::ACCESS_CONTROL_REQUEST_HEADERS, "content-type")
      .body(Body::empty())
      .unwrap();
    let response = test_router().oneshot(request).await.unwrap();
    assert_eq!(response.status(), StatusCode::NO_CONTENT);
    assert_eq!(
      response.headers()[header::ACCESS_CONTROL_ALLOW_ORIGIN],
      "https://www.os-island.com"
    );

    let request = Request::builder()
      .uri("/api/isos/v1/system/version")
      .header(header::HOST, "127.0.0.1:16000")
      .header(header::ORIGIN, "https://www.os-island.com")
      .body(Body::empty())
      .unwrap();
    let response = test_router().oneshot(request).await.unwrap();
    assert_eq!(response.status(), StatusCode::OK);
    assert_eq!(
      response.headers()[header::ACCESS_CONTROL_ALLOW_ORIGIN],
      "https://www.os-island.com"
    );

    let request = Request::builder()
      .uri("/api/isos/v1/system/version")
      .header(header::HOST, "127.0.0.1:16000")
      .header(header::ORIGIN, "https://www.os-island.com")
      .body(Body::empty())
      .unwrap();
    let response = test_router().oneshot(request).await.unwrap();
    assert_eq!(response.status(), StatusCode::OK);
    assert_eq!(
      response.headers()[header::ACCESS_CONTROL_ALLOW_ORIGIN],
      "https://www.os-island.com"
    );

    for headers in [
      [("origin", "http://127.0.0.1:16000")],
      [("origin", "http://127.0.0.1:16001")],
      [("sec-fetch-site", "same-origin")],
    ] {
      let mut request = Request::builder()
        .uri("/api/isos/v1/system/version")
        .header(header::HOST, "127.0.0.1:16000");
      for (name, value) in headers {
        request = request.header(name, value);
      }
      let response = test_router()
        .oneshot(request.body(Body::empty()).unwrap())
        .await
        .unwrap();
      assert_eq!(response.status(), StatusCode::OK);
    }

    let request = Request::builder()
      .uri("/api/isos/v1/system/version")
      .header(header::HOST, "127.0.0.1:16000")
      .body(Body::empty())
      .unwrap();
    assert_eq!(
      test_router().oneshot(request).await.unwrap().status(),
      StatusCode::OK
    );
    let request = Request::builder()
      .uri("/api/isos/v1/system/version")
      .header(header::HOST, "127.0.0.1:16000")
      .header("sec-fetch-site", "cross-site")
      .body(Body::empty())
      .unwrap();
    assert_eq!(
      test_router().oneshot(request).await.unwrap().status(),
      StatusCode::FORBIDDEN
    );
  }

  #[tokio::test]
  async fn old_route_is_absent_and_numeric_schema_is_typed_error() {
    let old = Request::builder()
      .uri("/api/isos/v2/system/version")
      .header(header::HOST, "127.0.0.1:16000")
      .body(Body::empty())
      .unwrap();
    assert_eq!(
      test_router().oneshot(old).await.unwrap().status(),
      StatusCode::NOT_FOUND
    );

    let request = Request::builder()
      .method(Method::POST)
      .uri("/api/isos/v1/presentations/select")
      .header(header::HOST, "127.0.0.1:16000")
      .header(header::CONTENT_TYPE, "application/json")
      .body(Body::from(r#"{"schema_version":2,"node_path":"eos/interface/camera/front","port":"frame","accept":[],"run_id":"run","graph_revision":"rev"}"#))
      .unwrap();
    let response = test_router().oneshot(request).await.unwrap();
    assert_eq!(response.status(), StatusCode::BAD_REQUEST);
    let body = axum::body::to_bytes(response.into_body(), 4096)
      .await
      .unwrap();
    let body: Value = serde_json::from_slice(&body).unwrap();
    assert_eq!(body["schema_version"], "0.1.0");
    assert_eq!(body["error"]["code"], "unsupported_schema_version");
  }

  #[tokio::test]
  async fn rejects_other_origins_and_hosts() {
    for (host, origin) in [
      ("127.0.0.1:16000", "https://evil.example"),
      ("127.0.0.1:16000", "http://127.0.0.1:16002"),
      ("localhost:16000", "https://www.os-island.com"),
    ] {
      let request = Request::builder()
        .uri("/api/isos/v1/system/version")
        .header(header::HOST, host)
        .header(header::ORIGIN, origin)
        .body(Body::empty())
        .unwrap();
      let response = test_router().oneshot(request).await.unwrap();
      assert_eq!(response.status(), StatusCode::FORBIDDEN);
    }
  }
  #[tokio::test]
  async fn api_does_not_serve_frontend_pages() {
    for route in ["/", "/en/isos", "/assets/app.js"] {
      let response = test_router()
        .oneshot(
          Request::builder()
            .uri(route)
            .header(header::HOST, "127.0.0.1:16000")
            .body(Body::empty())
            .unwrap(),
        )
        .await
        .unwrap();
      assert_eq!(response.status(), StatusCode::NOT_FOUND);
    }
  }
}
