//! Runtime API handlers.
use super::*;

pub(crate) async fn setup_status(State(app): State<Arc<App>>, headers: HeaderMap) -> ApiResult {
  authorized(&app, &headers)?;
  let graph_path = app.paths.node_graph_path(&app.deployment);
  let packages = packages::list(&app.paths)
    .map_err(|error| internal_err(StatusCode::INTERNAL_SERVER_ERROR, &error))?;
  let mut issues: Vec<String> = Vec::new();
  let revision = match fs::read(&graph_path) {
    Ok(bytes) => match serde_json::from_slice::<Value>(&bytes) {
      Ok(graph) => {
        if let Err(error) = process::validate_value(&app.paths, &graph) {
          tracing::warn!(%error, "graph setup validation failed");
          let diagnostic = admission_diagnostic(&error);
          issues.push(format!(
            "{}: {}",
            diagnostic["path"].as_str().unwrap_or("/"),
            diagnostic["code"].as_str().unwrap_or("invalid_graph")
          ));
        }
        Some(revision(&bytes))
      }
      Err(error) => {
        tracing::warn!(%error, "graph setup parse failed");
        issues.push("graph is invalid".into());
        Some(revision(&bytes))
      }
    },
    Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
      issues.push("graph is missing".into());
      None
    }
    Err(error) => return Err(internal_err(StatusCode::INTERNAL_SERVER_ERROR, &error)),
  };
  let paths = app.paths.clone();
  let deployment = app.deployment.clone();
  let process = match tokio::task::spawn_blocking(move || {
    crate::runtime::manager::manager_status_for_http(&paths, &deployment)
  })
  .await
  {
    Ok(Ok(process)) => Some(process),
    Ok(Err(error)) => {
      tracing::warn!(%error, "manager setup status unavailable");
      issues.push("IsOS process manager is unavailable".into());
      None
    }
    Err(error) => return Err(internal_err(StatusCode::SERVICE_UNAVAILABLE, &error)),
  };
  ok(
    json!({"ready":issues.is_empty(),"issues":issues,"graph_revision":revision,"packages":packages,"process":process,"graph_path":graph_path}),
  )
}

#[derive(Deserialize)]
pub(crate) struct ProcessControl {
  #[serde(default)]
  schema_version: Value,
  operation: String,
  #[serde(default)]
  revision: Option<String>,
}

pub(crate) async fn process_control(
  State(app): State<Arc<App>>,
  headers: HeaderMap,
  Json(control): Json<ProcessControl>,
) -> ApiResult {
  authorized(&app, &headers)?;
  if control.schema_version != "0.1.0" {
    return Err(fail(
      StatusCode::BAD_REQUEST,
      "unsupported_schema_version",
      "API schema_version must be 0.1.0",
      Value::Null,
    ));
  }
  let op = match control.operation.as_str() {
    "run" | "start" => "deployment_start",
    "stop" => "deployment_stop",
    "restart" => "deployment_restart",
    _ => {
      return Err(err(
        StatusCode::BAD_REQUEST,
        "operation must be run, start, stop, or restart",
      ));
    }
  };
  let result = crate::runtime::manager::manager_call_revision(
    &app.paths,
    &app.deployment,
    op,
    control.revision,
  )
  .map_err(|error| {
    if let Some(remote) = error.downcast_ref::<crate::runtime::manager::RemoteDiagnosticError>()
      && remote.data["diagnostics"].is_array()
    {
      let status = if matches!(
        remote.data["diagnostics"][0]["code"].as_str(),
        Some("generated_commit_uncertain" | "log_scope_publish_failed")
      ) {
        StatusCode::INTERNAL_SERVER_ERROR
      } else {
        StatusCode::UNPROCESSABLE_ENTITY
      };
      let code = remote.data["diagnostics"][0]["code"]
        .as_str()
        .unwrap_or("manager_operation_failed")
        .to_owned();
      fail(
        status,
        code,
        "manager operation rejected",
        remote.data.clone(),
      )
    } else {
      internal_err(StatusCode::UNPROCESSABLE_ENTITY, &error)
    }
  })?;
  ok(result)
}

pub(crate) async fn logs(
  State(app): State<Arc<App>>,
  headers: HeaderMap,
  axum::extract::Query(query): axum::extract::Query<std::collections::BTreeMap<String, String>>,
) -> ApiResult {
  authorized(&app, &headers)?;
  if let Some(view) = query.get("view") {
    if view != "current" {
      return Err(diagnostic_failure(
        StatusCode::BAD_REQUEST,
        "log query rejected",
        json!({"path":"/query/view","code":"log_view_invalid","message":"Log view invalid"}),
      ));
    }
    let limit = query
      .get("limit")
      .map(|value| value.parse::<usize>())
      .transpose()
      .map_err(|_| {
        diagnostic_failure(
          StatusCode::BAD_REQUEST,
          "log query rejected",
          json!({"path":"/query/limit","code":"log_limit_invalid","message":"Log limit invalid"}),
        )
      })?
      .unwrap_or(100);
    let store = LogStore::new(&app.paths, &app.deployment)
            .map_err(|_| diagnostic_failure(StatusCode::SERVICE_UNAVAILABLE, "log scope unavailable",
                json!({"path":"/scope","code":"log_scope_unavailable","message":"Current log scope unavailable"})))?;
    let paths = app.paths.clone();
    let deployment = app.deployment.clone();
    let status = tokio::task::spawn_blocking(move || {
      crate::runtime::manager::manager_status_for_http(&paths, &deployment)
    })
    .await
    .map_err(|error| internal_err(StatusCode::SERVICE_UNAVAILABLE, &error))?;
    let active_revision = match &status {
      Ok(status) => status["active_revision"].as_str(),
      Err(error) => {
        tracing::warn!(%error, "manager unavailable while reading local log scope");
        None
      }
    };
    let data = store
      .read_current(crate::runtime::logs::CurrentQuery {
        source: query.get("source").map_or("all", String::as_str),
        level: query.get("level").map_or("info", String::as_str),
        node_path: query.get("node_path").map(String::as_str),
        correlation_id: query.get("correlation_id").map(String::as_str),
        cursor: query.get("cursor").map(String::as_str),
        limit,
        active_revision,
      })
      .map_err(|error| {
        diagnostic_failure(
          StatusCode::from_u16(error.status).unwrap_or(StatusCode::BAD_REQUEST),
          error.public_error(),
          error.diagnostic(),
        )
      })?;
    return ok(data);
  }
  let default_source = std::env::var("ISOS_LOG_DEFAULT_SOURCE").unwrap_or_else(|_| "all".into());
  let source = query.get("source").unwrap_or(&default_source);
  let limit = query
    .get("limit")
    .map(|value| value.parse::<usize>())
    .transpose()
    .map_err(|_| err(StatusCode::BAD_REQUEST, "invalid log limit"))?
    .unwrap_or(100);
  let store = LogStore::new(&app.paths, &app.deployment)
    .map_err(|error| internal_err(StatusCode::INTERNAL_SERVER_ERROR, &error))?;
  let data = store
    .read_filtered(
      source,
      query.get("cursor").map(String::as_str),
      limit,
      query.get("level").map(String::as_str),
    )
    .map_err(|error| {
      let message = error.to_string();
      let status = if message.contains("unavailable") {
        StatusCode::NOT_FOUND
      } else {
        StatusCode::BAD_REQUEST
      };
      internal_err(status, &error)
    })?;
  ok(data)
}
#[derive(Deserialize)]
pub(crate) struct CoreSetupEdit {
  #[serde(default)]
  schema_version: Value,
  graph: Value,
  #[serde(default)]
  revision: Option<String>,
}

pub(crate) async fn core_setup_route(
  State(app): State<Arc<App>>,
  headers: HeaderMap,
  UrlPath((role, op)): UrlPath<(String, String)>,
  body: Option<Json<CoreSetupEdit>>,
) -> ApiResult {
  authorized(&app, &headers)?;
  if !matches!(role.as_str(), "eos" | "egia")
    || !matches!(op.as_str(), "catalog" | "validate" | "render")
  {
    return Err(err(StatusCode::NOT_FOUND, "unknown core setup operation"));
  }
  if op != "catalog" && body.is_none() {
    return Err(err(StatusCode::BAD_REQUEST, "operation requires a graph"));
  }
  let (graph, revision) = if let Some(Json(edit)) = body {
    if edit.schema_version != "0.1.0" {
      return Err(fail(
        StatusCode::BAD_REQUEST,
        "unsupported_schema_version",
        "API schema_version must be 0.1.0",
        Value::Null,
      ));
    }
    let graph = edit.graph;
    let revision = edit.revision.unwrap_or_else(|| {
      serde_json::to_vec(&graph)
        .map(|bytes| revision(&bytes))
        .expect("JSON value serialization cannot fail")
    });
    (graph, revision)
  } else {
    let bytes = fs::read(app.paths.node_graph_path(&app.deployment))
      .map_err(|error| internal_err(StatusCode::NOT_FOUND, &error))?;
    let graph = serde_json::from_slice(&bytes)
      .map_err(|error| internal_err(StatusCode::UNPROCESSABLE_ENTITY, &error))?;
    (graph, revision(&bytes))
  };
  let reply = core_setup::call(&app.paths, &app.deployment, &role, &op, &graph, &revision)
    .map_err(|error| internal_err(StatusCode::SERVICE_UNAVAILABLE, &error))?;
  ok(reply)
}
