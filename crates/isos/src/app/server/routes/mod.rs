//! Domain routes exposed by the loopback API.
pub(super) mod catalog;
pub(super) mod graph;
pub(super) mod nodes;
pub(super) mod package_handlers;
pub(super) mod presentations;
pub(super) mod runtime;
pub(super) mod system;
use system::version;
use nodes::node_event;
use super::*;

pub(super) fn router(app: Arc<App>) -> Router {
  let api = Router::new()
    .route(
      "/api/isos/v1/system/version",
      get(version).options(preflight),
    )
    .route(
      "/api/isos/v1/catalog/node-types",
      get(node_types).options(preflight),
    )
    .route(
      "/api/isos/v1/catalog/node-types/selected",
      get(selected_node_types).options(preflight),
    )
    .route(
      "/api/isos/v1/catalog/node-options",
      get(node_options_current)
        .post(node_options_for_graph)
        .options(preflight),
    )
    .route(
      "/api/isos/v1/catalog/node-sets",
      get(package_node_sets).options(preflight),
    )
    .route(
      "/api/isos/v1/catalog/node-sets/preview",
      post(preview_node_set).options(preflight),
    )
    .route(
      "/api/isos/v1/runtime/process-status",
      get(process_status).options(preflight),
    )
    .route(
      "/api/isos/v1/nodes/events",
      post(node_event).options(preflight),
    )
    .route(
      "/api/isos/v1/presentations/select",
      post(select_presentation).options(preflight),
    )
    .route(
      "/api/isos/v1/packages",
      get(package_list).options(preflight),
    )
    .route(
      "/api/isos/v1/packages/install",
      post(package_install).options(preflight),
    )
    .route(
      "/api/isos/v1/packages/link",
      post(package_link).options(preflight),
    )
    .route(
      "/api/isos/v1/packages/preview-link",
      post(preview_link).options(preflight),
    )
    .route(
      "/api/isos/v1/cores/readiness",
      get(core_readiness).options(preflight),
    )
    .route(
      "/api/isos/v1/cores/setup-status",
      get(setup_status).options(preflight),
    )
    .route(
      "/api/isos/v1/runtime/control",
      post(process_control).options(preflight),
    )
    .route("/api/isos/v1/runtime/logs", get(logs).options(preflight))
    .route(
      "/api/isos/v1/graph",
      get(node_graph).put(apply_node_graph).options(preflight),
    )
    .route(
      "/api/isos/v1/graph/validate",
      post(validate_node_graph).options(preflight),
    )
    .route(
      "/api/isos/v1/graph/migration/preview",
      post(migration_preview).options(preflight),
    )
    .route(
      "/api/isos/v1/graph/migration/apply",
      post(migration_apply).options(preflight),
    )
    .route(
      "/api/isos/v1/cores/setup/{role}/{op}",
      get(core_setup_route)
        .post(core_setup_route)
        .options(preflight),
    )
    .route_layer(middleware::from_fn(cors));
  Router::new().merge(api).with_state(app)
}
