//! Graph API handlers.
use super::*;

fn graph_slots() -> &'static Arc<Semaphore> {
  static SLOTS: OnceLock<Arc<Semaphore>> = OnceLock::new();
  SLOTS.get_or_init(|| Arc::new(Semaphore::new(2)))
}

async fn graph_work<T, F>(work: F) -> std::result::Result<T, (StatusCode, Json<Value>)>
where
  T: Send + 'static,
  F: FnOnce() -> std::result::Result<T, (StatusCode, Json<Value>)> + Send + 'static,
{
  let permit = graph_slots()
    .clone()
    .acquire_owned()
    .await
    .map_err(|error| internal_err(StatusCode::SERVICE_UNAVAILABLE, &error))?;
  tokio::task::spawn_blocking(move || {
    let _permit = permit;
    work()
  })
  .await
  .map_err(|error| internal_err(StatusCode::INTERNAL_SERVER_ERROR, &error))?
}

pub(crate) fn edge_diagnostics(graph: &Value, catalog: &Value) -> Vec<Value> {
  if graph["version"] != "1.0.0" {
    return Vec::new();
  }
  let Some(types) = catalog["types"].as_array() else {
    return Vec::new();
  };
  let definitions: std::collections::BTreeMap<&str, &Value> = types
    .iter()
    .filter_map(|definition| Some((definition["id"].as_str()?, definition)))
    .collect();
  let mut nodes = std::collections::BTreeMap::new();
  let Some(cores) = graph["cores"].as_object() else {
    return Vec::new();
  };
  for (core_id, core) in cores {
    let Some(layers) = core["layers"].as_object() else {
      continue;
    };
    for (layer_id, layer) in layers {
      let Some(groups) = layer["groups"].as_object() else {
        continue;
      };
      for (group_id, group) in groups {
        let Some(entries) = group["nodes"].as_object() else {
          continue;
        };
        for (node_id, node) in entries {
          nodes.insert(format!("{core_id}/{layer_id}/{group_id}/{node_id}"), node);
        }
      }
    }
  }
  let mut diagnostics = Vec::new();
  for (path, node) in &nodes {
    let Some(definition) = node["node_type"]
      .as_str()
      .and_then(|id| definitions.get(id))
    else {
      continue;
    };
    let Some(inputs) = node["inputs"].as_object() else {
      continue;
    };
    for (input_name, source_value) in inputs {
      let Some(input_type) = definition["inputs"]
        .as_array()
        .and_then(|ports| {
          ports
            .iter()
            .find(|port| port["name"] == input_name.as_str())
        })
        .and_then(|port| port["data_type"].as_str())
      else {
        continue;
      };
      let sources: Vec<&Value> = source_value
        .as_array()
        .map_or_else(|| vec![source_value], |values| values.iter().collect());
      for (index, source) in sources.iter().enumerate() {
        let Some(source_path) = source["node"].as_str() else {
          continue;
        };
        let Some(source_node) = nodes.get(source_path) else {
          continue;
        };
        let Some(source_definition) = source_node["node_type"]
          .as_str()
          .and_then(|id| definitions.get(id))
        else {
          continue;
        };
        let Some(output_name) = source["output"].as_str() else {
          continue;
        };
        let Some(output_type) = source_definition["outputs"]
          .as_array()
          .and_then(|ports| ports.iter().find(|port| port["name"] == output_name))
          .and_then(|port| port["data_type"].as_str())
        else {
          continue;
        };
        if input_type != output_type {
          diagnostics.push(json!({
                        "path":format!("/cores/{}/layers/{}/groups/{}/nodes/{}/inputs/{input_name}/{index}",
                            path.split('/').next().unwrap_or(""), path.split('/').nth(1).unwrap_or(""),
                            path.split('/').nth(2).unwrap_or(""), path.split('/').nth(3).unwrap_or("")),
                        "code":"edge_type_mismatch",
                        "message":format!("{source_path}.{output_name} ({output_type}) cannot feed {path}.{input_name} ({input_type}); compare the registered data type and its unit schema"),
                        "source":source_path,"target":path,"input":input_name
                    }));
        }
      }
    }
  }
  diagnostics
}
pub(crate) async fn node_graph(State(app): State<Arc<App>>, headers: HeaderMap) -> ApiResult {
  authorized(&app, &headers)?;
  process::validate_deployment_name(&app.deployment)
    .map_err(|error| internal_err(StatusCode::BAD_REQUEST, &error))?;
  let bytes = match fs::read(app.paths.node_graph_path(&app.deployment)) {
    Ok(bytes) => bytes,
    Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
      return ok(json!({
          "revision":"",
          "graph":crate::domain::graph::starter::draft(&app.paths, &app.deployment)
              .map_err(|error| internal_err(StatusCode::UNPROCESSABLE_ENTITY, &error))?,
          "read_only":false,"diagnostics":[]
      }));
    }
    Err(error) => return Err(internal_err(StatusCode::INTERNAL_SERVER_ERROR, &error)),
  };
  let saved = graph_compat::parse(&bytes)
    .map_err(|error| internal_err(StatusCode::UNPROCESSABLE_ENTITY, &error))?;
  let applied = app
    .paths
    .generated(&app.deployment)
    .join("core-setup/launch-inputs.json")
    .is_file();
  let selected_catalog = if saved.value["version"] == "0.1.0" {
    Some(selected_catalog_read(app.paths.clone(), saved.value.clone()).await)
  } else {
    None
  };
  let catalog = match &selected_catalog {
    Some(Ok(catalog)) => Some(catalog.clone()),
    Some(Err(_)) => None,
    None => Some(
      bounded_catalog_read(process::catalog)
        .await
        .map_err(|error| internal_err(StatusCode::SERVICE_UNAVAILABLE, &error))?,
    ),
  };
  let projection = graph_compat::legacy_projection(&saved.value, &saved.revision, catalog.as_ref());
  let editable_starter =
    !applied && saved.value["version"] == "1.0.0" && projection.as_ref().is_ok_and(Option::is_some);
  let editable_new = saved.value["version"] == "0.1.0"
    && catalog.is_some()
    && graph_compat::admission_diagnostic(&saved.value, catalog.as_ref()).is_none();
  let mut diagnostics = saved.diagnostics;
  if let Err(problem) = &projection
    && diagnostics.is_empty()
  {
    diagnostics.push(problem.clone());
  }
  if let Some(Err(error)) = selected_catalog {
    tracing::warn!("selected catalog unavailable: {error:#}");
    diagnostics.push(selected_catalog_diagnostic());
  }
  if saved.value["version"] == "0.1.0"
    && let Some(problem) = graph_compat::admission_diagnostic(&saved.value, catalog.as_ref())
    && !diagnostics
      .iter()
      .any(|item| item.path == problem.path && item.code == problem.code)
  {
    diagnostics.push(problem);
  }
  let read_only = if saved.value["version"] == "0.1.0" {
    !editable_new
  } else {
    saved.read_only && !editable_starter
  };
  let mut data = json!({"revision":saved.revision,"graph":saved.value,
        "read_only":read_only,
        "diagnostics":diagnostics});
  if let Ok(Some(projection)) = projection {
    data["legacy_projection"] = json!(projection);
  }
  ok(data)
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct MigrationRequest {
  #[serde(default)]
  schema_version: Value,
  revision: String,
  #[serde(default)]
  proposed_revision: Option<String>,
}

pub(crate) fn migration_error(error: graph_migration::MigrationError) -> (StatusCode, Json<Value>) {
  match error {
    graph_migration::MigrationError::Conflict(_) => {
      err(StatusCode::CONFLICT, "graph revision changed")
    }
    graph_migration::MigrationError::Diagnostic(diagnostic) => diagnostic_failure(
      StatusCode::UNPROCESSABLE_ENTITY,
      "migration rejected",
      json!(graph_migration::public_diagnostic(&diagnostic)),
    ),
    graph_migration::MigrationError::Generated(diagnostic) => {
      let status = if diagnostic.code == "generated_commit_uncertain" {
        StatusCode::INTERNAL_SERVER_ERROR
      } else {
        StatusCode::UNPROCESSABLE_ENTITY
      };
      diagnostic_failure(
        status,
        "generated configuration rejected",
        json!(diagnostic),
      )
    }
    graph_migration::MigrationError::Filesystem(error) => {
      internal_err(StatusCode::INTERNAL_SERVER_ERROR, &error)
    }
    graph_migration::MigrationError::OutcomeUncertain => diagnostic_failure(
      StatusCode::INTERNAL_SERVER_ERROR,
      "generated configuration publication is uncertain",
      json!(crate::domain::graph::generated_v2::commit_uncertain()),
    ),
  }
}

pub(crate) async fn migration_preview(
  State(app): State<Arc<App>>,
  headers: HeaderMap,
  Json(request): Json<MigrationRequest>,
) -> ApiResult {
  authorized(&app, &headers)?;
  if request.schema_version != "0.1.0" {
    if request.schema_version == "0.2.0" {
      if request.proposed_revision.is_some() {
        return Err(err(
          StatusCode::BAD_REQUEST,
          "preview requires revision only",
        ));
      }
      return graph_work(move || {
        let preview = graph_migration::preview_unified(&app.paths, &app.deployment, &request.revision)
          .map_err(migration_error)?;
        let diagnostics: Vec<_> = preview.diagnostic.into_iter().collect();
        ok(json!({"revision":request.revision,"proposed_revision":preview.proposal.revision,
          "graph":preview.proposal.graph,"changes":preview.proposal.changes,
          "can_apply":graph_migration::unified_apply_ready() && diagnostics.is_empty(),
          "diagnostics":if diagnostics.is_empty() && !graph_migration::unified_apply_ready() {
            json!([{"path":"/cores/isos","code":"runtime_replay_required",
              "message":"camera capture and admitted Dispatcher control replay is required before graph 0.2.0 Apply"}])
          } else { json!(diagnostics) }}))
      }).await;
    }
    return Err(fail(
      StatusCode::BAD_REQUEST,
      "unsupported_schema_version",
      "API schema_version must be 0.1.0 or 0.2.0",
      Value::Null,
    ));
  }
  if request.proposed_revision.is_some() {
    return Err(err(
      StatusCode::BAD_REQUEST,
      "preview requires revision only",
    ));
  }
  graph_work(move || {
    let preview = graph_migration::preview(&app.paths, &app.deployment, &request.revision)
      .map_err(migration_error)?;
    let diagnostics: Vec<_> = preview.diagnostic.into_iter().collect();
    ok(
      json!({"revision":request.revision,"proposed_revision":preview.proposal.revision,
            "graph":preview.proposal.graph,"changes":preview.proposal.changes,
            "can_apply":diagnostics.is_empty(),"diagnostics":diagnostics}),
    )
  })
  .await
}

pub(crate) async fn migration_apply(
  State(app): State<Arc<App>>,
  headers: HeaderMap,
  Json(request): Json<MigrationRequest>,
) -> ApiResult {
  authorized(&app, &headers)?;
  if request.schema_version != "0.1.0" && request.schema_version != "0.2.0" {
    return Err(fail(
      StatusCode::BAD_REQUEST,
      "unsupported_schema_version",
      "API schema_version must be 0.1.0 or 0.2.0",
      Value::Null,
    ));
  }
  let proposed_revision = request
    .proposed_revision
    .ok_or_else(|| err(StatusCode::BAD_REQUEST, "proposed_revision is required"))?;
  let work_app = app.clone();
  let unified = request.schema_version == "0.2.0";
  let proposal = graph_work(move || {
    (if unified {
      graph_migration::commit_unified
    } else {
      graph_migration::commit
    })(
      &work_app.paths,
      &work_app.deployment,
      &request.revision,
      &proposed_revision,
    )
    .map_err(migration_error)
  })
  .await?;
  let paths = app.paths.clone();
  let deployment = app.deployment.clone();
  let active_revision = match tokio::task::spawn_blocking(move || {
    crate::runtime::manager::manager_status_for_http(&paths, &deployment)
  })
  .await
  {
    Ok(Ok(status)) => status.get("active_revision").cloned(),
    Ok(Err(error)) => {
      tracing::warn!(%error, "manager status unavailable after graph migration");
      None
    }
    Err(error) => {
      tracing::warn!(%error, "manager status join failed after graph migration");
      None
    }
  };
  record(&app, "graph", "Migrated saved node graph revision");
  ok(json!({"revision":proposal.revision,"graph":proposal.graph,
        "active_revision":active_revision}))
}

#[derive(Deserialize)]
pub(crate) struct NodeGraphEdit {
  #[serde(default)]
  pub(crate) schema_version: Value,
  pub(crate) graph: Value,
  #[serde(default)]
  pub(crate) revision: Option<String>,
}

pub(crate) async fn validate_node_graph(
  State(app): State<Arc<App>>,
  headers: HeaderMap,
  Json(edit): Json<NodeGraphEdit>,
) -> ApiResult {
  let result = validate_node_graph_inner(State(app.clone()), headers, Json(edit)).await;
  record_graph_failure(&app, &result, false);
  result
}

pub(crate) async fn validate_node_graph_inner(
  State(app): State<Arc<App>>,
  headers: HeaderMap,
  Json(edit): Json<NodeGraphEdit>,
) -> ApiResult {
  authorized(&app, &headers)?;
  if edit.schema_version != "0.1.0" && edit.schema_version != "0.2.0" {
    return Err(fail(
      StatusCode::BAD_REQUEST,
      "unsupported_schema_version",
      "API schema_version must be 0.1.0 or 0.2.0",
      Value::Null,
    ));
  }
  graph_work(move || validate_node_graph_sync(&app, edit)).await
}

fn validate_node_graph_sync(app: &App, edit: NodeGraphEdit) -> ApiResult {
  if edit.graph["version"] == "0.1.0" || edit.graph["version"] == "0.2.0" {
    let bytes = serde_json::to_vec_pretty(&edit.graph)
      .map_err(|error| internal_err(StatusCode::UNPROCESSABLE_ENTITY, &error))?;
    let proposal = graph_migration::Proposal {
      revision: graph_compat::revision(&bytes),
      graph: edit.graph,
      bytes,
      changes: Vec::new(),
    };
    return match graph_migration::prepare(&app.paths, &app.deployment, &proposal) {
      Ok(prepared) => ok(json!({"valid":true,"diagnostics":[],"edges":prepared.edges})),
      Err(diagnostic) => {
        ok(json!({"valid":false,"diagnostics":[graph_migration::public_diagnostic(&diagnostic)]}))
      }
    };
  }
  if let Err(diagnostic) = skill_selection::check_reserved(&edit.graph)
    .and_then(|()| skill_selection::entries(&edit.graph).map(|_| ()))
  {
    return ok(json!({"valid":false,"diagnostics":[diagnostic]}));
  }
  if let Err(error) = packages::verify_graph_packages(&app.paths, &edit.graph) {
    return ok(json!({"valid":false,"diagnostics":[graph_migration::package_diagnostic(&error)]}));
  }
  let catalog =
    process::catalog().map_err(|error| internal_err(StatusCode::SERVICE_UNAVAILABLE, &error))?;
  if let Some(diagnostic) = graph_compat::legacy_mapping_diagnostic(&edit.graph, Some(&catalog)) {
    return ok(json!({"valid":false,"diagnostics":[diagnostic]}));
  }
  let edge_diagnostics = edge_diagnostics(&edit.graph, &catalog);
  let diagnostics: Vec<Value> = if edge_diagnostics.is_empty() {
    (|| -> Result<()> {
      process::validate_value(&app.paths, &edit.graph)?;
      let specs = process::project_with_packages(&app.paths, &serde_json::to_string(&edit.graph)?)?;
      let catalog = process::catalog_for_graph(&app.paths, &edit.graph)?;
      reader_features::check(&app.paths, &edit.graph, &specs, &catalog)?;
      Ok(())
    })()
    .err()
    .map(|error| admission_diagnostic(&error))
    .into_iter()
    .collect()
  } else {
    edge_diagnostics
  };
  ok(json!({"valid":diagnostics.is_empty(),"diagnostics":diagnostics}))
}

pub(crate) async fn apply_node_graph(
  State(app): State<Arc<App>>,
  headers: HeaderMap,
  Json(edit): Json<NodeGraphEdit>,
) -> ApiResult {
  let result = apply_node_graph_inner(State(app.clone()), headers, Json(edit)).await;
  record_graph_failure(&app, &result, true);
  result
}

pub(crate) async fn apply_node_graph_inner(
  State(app): State<Arc<App>>,
  headers: HeaderMap,
  Json(edit): Json<NodeGraphEdit>,
) -> ApiResult {
  authorized(&app, &headers)?;
  process::validate_deployment_name(&app.deployment)
    .map_err(|error| internal_err(StatusCode::BAD_REQUEST, &error))?;
  if edit.schema_version != "0.1.0" && edit.schema_version != "0.2.0" {
    return Err(fail(
      StatusCode::BAD_REQUEST,
      "unsupported_schema_version",
      "API schema_version must be 0.1.0 or 0.2.0",
      Value::Null,
    ));
  }
  graph_work(move || apply_node_graph_sync(&app, edit)).await
}

fn apply_node_graph_sync(app: &App, edit: NodeGraphEdit) -> ApiResult {
  let graph = edit.graph;
  if graph["version"] == "0.1.0" || graph["version"] == "0.2.0" {
    let expected = edit.revision.as_deref().ok_or_else(|| {
      err(
        StatusCode::BAD_REQUEST,
        "Apply requires an explicit revision, including an empty revision for a new graph",
      )
    })?;
    let (proposal, _) = graph_migration::apply_new(&app.paths, &app.deployment, graph, expected)
      .map_err(migration_error)?;
    record(app, "graph", "Applied node graph revision");
    return ok(json!({"revision":proposal.revision,"graph":proposal.graph}));
  }
  if let Err(error) = packages::verify_graph_packages(&app.paths, &graph) {
    let diagnostic = graph_migration::package_diagnostic(&error);
    return Err(diagnostic_failure(
      StatusCode::UNPROCESSABLE_ENTITY,
      "package verification rejected",
      json!(diagnostic),
    ));
  }
  let catalog =
    process::catalog().map_err(|error| internal_err(StatusCode::SERVICE_UNAVAILABLE, &error))?;
  if let Some(diagnostic) = graph_compat::legacy_mapping_diagnostic(&graph, Some(&catalog)) {
    return Err(diagnostic_failure(
      StatusCode::UNPROCESSABLE_ENTITY,
      "graph admission rejected",
      json!(diagnostic),
    ));
  }
  let path = app.paths.node_graph_path(&app.deployment);
  {
    let _lock = lock::Lock::take(&app.paths)
      .map_err(|error| internal_err(StatusCode::INTERNAL_SERVER_ERROR, &error))?;
    if path.exists() {
      let saved = graph_compat::read(&path)
        .map_err(|error| internal_err(StatusCode::UNPROCESSABLE_ENTITY, &error))?;
      if edit.revision.as_deref() != Some(saved.revision.as_str()) {
        return Err(err(
          StatusCode::CONFLICT,
          "node graph revision changed; reload before applying",
        ));
      }
      if saved.value["version"] == "1.0.0" {
        if let Some(diagnostic) =
          graph_compat::projected_input_diagnostic(&saved, &graph, Some(&catalog))
        {
          return Err(diagnostic_failure(
            StatusCode::UNPROCESSABLE_ENTITY,
            "projection is read-only",
            json!(diagnostic),
          ));
        }
        if graph_compat::changed_legacy_diagnostic(&saved, &graph).is_none() {
          process::validate_value(&app.paths, &saved.value)
            .map_err(|error| admission_err(&error))?;
          let source =
            serde_json::to_string(&saved.value).map_err(|error| admission_err(&error.into()))?;
          let specs = process::project_with_packages(&app.paths, &source)
            .map_err(|error| admission_err(&error))?;
          let catalog = process::catalog_for_graph(&app.paths, &saved.value)
            .map_err(|error| admission_err(&error))?;
          reader_features::check(&app.paths, &saved.value, &specs, &catalog)
            .map_err(|error| admission_err(&error.into()))?;
          let rendered =
            core_setup::render_graph(&app.paths, &app.deployment, &saved.value, &saved.revision)
              .map_err(|error| internal_err(StatusCode::UNPROCESSABLE_ENTITY, &error))?;
          core_setup::write_rendered(&app.paths, &app.deployment, rendered)
            .map_err(|error| internal_err(StatusCode::INTERNAL_SERVER_ERROR, &error))?;
          return ok(json!({"revision":saved.revision,"graph":saved.value}));
        }
        if graph_compat::requires_migration(
          &saved,
          &graph,
          app
            .paths
            .generated(&app.deployment)
            .join("core-setup/launch-inputs.json")
            .is_file(),
        ) {
          let diagnostic = graph_compat::changed_legacy_diagnostic(&saved, &graph)
            .expect("changed legacy graph has migration diagnostic");
          return Err(diagnostic_failure(
            StatusCode::UNPROCESSABLE_ENTITY,
            "migration required",
            json!(diagnostic),
          ));
        }
      }
      if saved.value["version"] != "1.0.0" {
        let diagnostic = saved
          .diagnostics
          .into_iter()
          .next()
          .unwrap_or(graph_compat::Diagnostic {
            path: "/version".into(),
            code: "migration_required",
            message: "saved graph requires a compatible migration writer".into(),
            context: std::collections::BTreeMap::new(),
          });
        return Err(diagnostic_failure(
          StatusCode::UNPROCESSABLE_ENTITY,
          "saved graph is read-only",
          json!(diagnostic),
        ));
      }
    }
  }
  if graph["version"] != "1.0.0" {
    return Err(err(
      StatusCode::UNPROCESSABLE_ENTITY,
      "0.1.0 writer is unavailable until core readers advertise support",
    ));
  }
  if let Ok(catalog) = process::catalog()
    && let Some(diagnostic) = edge_diagnostics(&graph, &catalog).into_iter().next()
  {
    return Err(err(
      StatusCode::UNPROCESSABLE_ENTITY,
      diagnostic["message"]
        .as_str()
        .unwrap_or("edge type mismatch"),
    ));
  }
  process::validate_value(&app.paths, &graph).map_err(|error| admission_err(&error))?;
  let source = serde_json::to_string(&graph).map_err(|error| admission_err(&error.into()))?;
  let specs =
    process::project_with_packages(&app.paths, &source).map_err(|error| admission_err(&error))?;
  let catalog =
    process::catalog_for_graph(&app.paths, &graph).map_err(|error| admission_err(&error))?;
  reader_features::check(&app.paths, &graph, &specs, &catalog)
    .map_err(|error| admission_err(&error.into()))?;
  let bytes = serde_json::to_vec_pretty(&graph)
    .map_err(|error| internal_err(StatusCode::UNPROCESSABLE_ENTITY, &error))?;
  let next_revision = revision(&bytes);
  let rendered = core_setup::render_graph(&app.paths, &app.deployment, &graph, &next_revision)
    .map_err(|error| internal_err(StatusCode::UNPROCESSABLE_ENTITY, &error))?;
  let _lock = lock::Lock::take(&app.paths)
    .map_err(|error| internal_err(StatusCode::INTERNAL_SERVER_ERROR, &error))?;
  if let Err(error) = packages::verify_graph_packages(&app.paths, &graph) {
    let diagnostic = graph_migration::package_diagnostic(&error);
    return Err(diagnostic_failure(
      StatusCode::UNPROCESSABLE_ENTITY,
      "package verification rejected",
      json!(diagnostic),
    ));
  }
  reader_features::check(&app.paths, &graph, &specs, &catalog)
    .map_err(|error| admission_err(&error.into()))?;
  let path = app.paths.node_graph_path(&app.deployment);
  let previous = match fs::read(&path) {
    Ok(bytes) => Some(bytes),
    Err(error) if error.kind() == std::io::ErrorKind::NotFound => None,
    Err(error) => return Err(internal_err(StatusCode::INTERNAL_SERVER_ERROR, &error)),
  };
  if let Some(bytes) = &previous {
    if edit.revision.as_deref() != Some(revision(bytes).as_str()) {
      return Err(err(
        StatusCode::CONFLICT,
        "node graph revision changed; reload before applying",
      ));
    }
    if app
      .paths
      .generated(&app.deployment)
      .join("core-setup/launch-inputs.json")
      .is_file()
    {
      let saved = graph_compat::parse(bytes)
        .map_err(|error| internal_err(StatusCode::UNPROCESSABLE_ENTITY, &error))?;
      if graph_compat::requires_migration(&saved, &graph, true) {
        return Err(err(
          StatusCode::CONFLICT,
          "graph became applied during editing; reload before migration",
        ));
      }
    }
  } else if edit
    .revision
    .as_deref()
    .is_some_and(|revision| !revision.is_empty())
  {
    return Err(err(
      StatusCode::CONFLICT,
      "node graph was removed; reload before applying",
    ));
  }
  config::atomic_write(&path, &bytes)
    .map_err(|error| internal_err(StatusCode::INTERNAL_SERVER_ERROR, &error))?;
  if let Err(error) = core_setup::write_rendered(&app.paths, &app.deployment, rendered) {
    if let Some(previous) = previous {
      let _ = config::atomic_write(&path, &previous);
    } else {
      let _ = fs::remove_file(&path);
    }
    return Err(internal_err(StatusCode::INTERNAL_SERVER_ERROR, &error));
  }
  if let Err(error) = crate::app::cli::clear_saved_setup_error(&app.paths, &app.deployment) {
    tracing::warn!("saved graph applied, but could not clear prior setup error: {error}");
  }
  record(app, "graph", "Applied node graph revision");
  ok(json!({"revision":next_revision,"graph":graph}))
}

#[cfg(test)]
mod blocking_tests {
  use super::*;

  #[tokio::test(flavor = "current_thread")]
  async fn graph_work_does_not_stall_http_worker() {
    let work = tokio::spawn(graph_work(|| {
      std::thread::sleep(std::time::Duration::from_millis(150));
      ok(json!({"complete":true}))
    }));
    tokio::time::timeout(
      std::time::Duration::from_millis(50),
      tokio::time::sleep(std::time::Duration::from_millis(20)),
    )
    .await
    .expect("Tokio worker stalled on graph operation");
    assert!(work.await.unwrap().is_ok());
  }
}
