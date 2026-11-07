//! System version route.

use super::*;

pub(crate) async fn version(State(app): State<Arc<App>>, headers: HeaderMap) -> ApiResult {
  authorized(&app, &headers)?;
  let support = crate::domain::version::bundled()
    .map_err(|error| internal_err(StatusCode::INTERNAL_SERVER_ERROR, &error))?;
  ok(
    json!({"api_schema_version":"0.1.0","isos_version":env!("CARGO_PKG_VERSION"),"version_support":support}),
  )
}
