//! Presentations API handlers.
use super::*;

pub(crate) async fn process_status(State(app): State<Arc<App>>, headers: HeaderMap) -> ApiResult {
  authorized(&app, &headers)?;
  let paths = app.paths.clone();
  let deployment = app.deployment.clone();
  let status = tokio::task::spawn_blocking(move || {
    crate::runtime::manager::manager_status_for_http(&paths, &deployment)
  })
  .await
  .map_err(|_| {
    err(
      StatusCode::SERVICE_UNAVAILABLE,
      "process status is unavailable",
    )
  })?
  .map_err(|_| {
    err(
      StatusCode::SERVICE_UNAVAILABLE,
      "process status is unavailable",
    )
  })?;
  ok(status)
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct PresentationSelectRequest {
  #[serde(default)]
  schema_version: Value,
  node_path: String,
  port: String,
  accept: Vec<crate::interfaces::data::Method>,
  run_id: String,
  graph_revision: String,
}

pub(crate) async fn select_presentation(
  State(app): State<Arc<App>>,
  headers: HeaderMap,
  Json(request): Json<PresentationSelectRequest>,
) -> ApiResult {
  authorized(&app, &headers)?;
  if request.schema_version != "0.1.0" {
    return Err(fail(
      StatusCode::BAD_REQUEST,
      "unsupported_schema_version",
      "API schema_version must be 0.1.0",
      Value::Null,
    ));
  }
  // Only the owner may register public delivery descriptors. Until that
  // contract is available, never use client-supplied media or endpoints.
  let descriptors: Vec<crate::interfaces::data::Descriptor> = Vec::new();
  let selection = crate::interfaces::data::select(
    &descriptors,
    &request.accept,
    &request.node_path,
    &request.port,
    &request.run_id,
    &request.graph_revision,
  );
  if matches!(selection, crate::interfaces::data::Selection::Invalid) {
    return Err(err(
      StatusCode::BAD_REQUEST,
      "invalid presentation selection",
    ));
  }
  let paths = app.paths.clone();
  let deployment = app.deployment.clone();
  let status = tokio::task::spawn_blocking(move || {
    crate::runtime::manager::manager_status_for_http(&paths, &deployment)
  })
  .await
  .map_err(|_| {
    err(
      StatusCode::SERVICE_UNAVAILABLE,
      "process status is unavailable",
    )
  })?
  .map_err(|_| {
    err(
      StatusCode::SERVICE_UNAVAILABLE,
      "process status is unavailable",
    )
  })?;
  if status["run_id"] != request.run_id || status["active_revision"] != request.graph_revision {
    return Err(err(
      StatusCode::CONFLICT,
      "presentation run or graph revision is stale",
    ));
  }
  Err(err(
    StatusCode::SERVICE_UNAVAILABLE,
    "presentation owner has not registered a public delivery descriptor",
  ))
}
