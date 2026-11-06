//! Packages API handlers.
use super::*;

pub(crate) async fn package_list(State(app): State<Arc<App>>, headers: HeaderMap) -> ApiResult {
  authorized(&app, &headers)?;
  let installed = packages::listing(&app.paths)
    .map_err(|error| internal_err(StatusCode::INTERNAL_SERVER_ERROR, &error))?;
  ok(json!({"installed":installed}))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct PackageRequest {
  id: String,
  version: String,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct PackageLinkRequest {
  id: String,
  version: String,
  path: PathBuf,
  #[serde(default)]
  manifest: Option<PathBuf>,
}

pub(crate) async fn package_install(
  State(app): State<Arc<App>>,
  headers: HeaderMap,
  Json(request): Json<PackageRequest>,
) -> ApiResult {
  authorized(&app, &headers)?;
  packages::package_path(&app.paths, &request.id, &request.version)
    .map_err(|error| internal_err(StatusCode::BAD_REQUEST, &error))?;
  let id = request.id.clone();
  let version = request.version.clone();
  let paths = app.paths.clone();
  let result = tokio::task::spawn_blocking(move || -> Result<PathBuf> {
    let release = packages::catalog_release(&request.id, &request.version)?;
    anyhow::ensure!(
      release.id == request.id && release.version == request.version,
      "catalog returned a different package identity"
    );
    packages::install(&paths, &release, false, None)
  })
  .await
  .map_err(|error| internal_err(StatusCode::INTERNAL_SERVER_ERROR, &error))?;
  let installed = match result {
    Ok(path) => path,
    Err(error) => {
      let code = error
        .chain()
        .find_map(|cause| cause.downcast_ref::<packages::PackageFailure>())
        .map_or("package_invalid_manifest", |failure| failure.code);
      record_package(&app, "error", code, &id, &version, "approved_catalog", None);
      return Err(package_err(&error, &id, &version));
    }
  };
  record_package(
    &app,
    "info",
    "package_verified",
    &id,
    &version,
    "approved_catalog",
    Some(&installed),
  );
  ok(
    json!({"path":installed,"verified":true,"source_kind":"approved_catalog","installed":packages::listing(&app.paths).map_err(|error| internal_err(StatusCode::INTERNAL_SERVER_ERROR, &error))?}),
  )
}

pub(crate) async fn package_link(
  State(app): State<Arc<App>>,
  headers: HeaderMap,
  Json(request): Json<PackageLinkRequest>,
) -> ApiResult {
  authorized(&app, &headers)?;
  packages::package_path(&app.paths, &request.id, &request.version)
    .map_err(|error| internal_err(StatusCode::BAD_REQUEST, &error))?;
  if !request.path.is_absolute() {
    return Err(err(
      StatusCode::BAD_REQUEST,
      "package link path must be absolute",
    ));
  }
  if request
    .manifest
    .as_ref()
    .is_some_and(|path| !path.is_absolute())
  {
    return Err(err(
      StatusCode::BAD_REQUEST,
      "package manifest path must be absolute",
    ));
  }
  let id = request.id.clone();
  let version = request.version.clone();
  let paths = app.paths.clone();
  let result = tokio::task::spawn_blocking(move || {
    packages::link_with_manifest(
      &paths,
      &request.id,
      &request.version,
      &request.path,
      request.manifest.as_deref(),
    )
  })
  .await
  .map_err(|error| internal_err(StatusCode::INTERNAL_SERVER_ERROR, &error))?;
  let linked = match result {
    Ok(path) => path,
    Err(error) => {
      let code = error
        .chain()
        .find_map(|cause| cause.downcast_ref::<packages::PackageFailure>())
        .map_or("package_invalid_manifest", |failure| failure.code);
      record_package(&app, "error", code, &id, &version, "local_link", None);
      return Err(package_err(&error, &id, &version));
    }
  };
  record_package(
    &app,
    "info",
    if linked.join("release.json").is_file() {
      "package_verified"
    } else {
      "package_linked"
    },
    &id,
    &version,
    "local_link",
    Some(&linked),
  );
  ok(
    json!({"path":linked,"verified":linked.join("release.json").is_file(),"source_kind":"local_link","installed":packages::listing(&app.paths).map_err(|error| internal_err(StatusCode::INTERNAL_SERVER_ERROR, &error))?}),
  )
}
