//! Send structured manager events to the owner-only deployment log stream.

use crate::runtime::logs::LogStore;
use anyhow::Result;
use tracing::{
  Event, Subscriber,
  field::{Field, Visit},
};
use tracing_subscriber::{
  Layer,
  layer::{Context, SubscriberExt},
};

struct EventFields {
  code: String,
  message: String,
}

impl Visit for EventFields {
  fn record_debug(&mut self, field: &Field, value: &dyn std::fmt::Debug) {
    match field.name() {
      "message" => self.message = format!("{value:?}"),
      "event_code" => self.code = format!("{value:?}"),
      _ => {}
    }
  }

  fn record_str(&mut self, field: &Field, value: &str) {
    match field.name() {
      "message" => self.message = value.to_owned(),
      "event_code" => self.code = value.to_owned(),
      _ => {}
    }
  }
}

struct ManagerLayer(LogStore);

impl<S: Subscriber> Layer<S> for ManagerLayer {
  fn on_event(&self, event: &Event<'_>, _context: Context<'_, S>) {
    let mut fields = EventFields {
      code: "manager_event".into(),
      message: String::new(),
    };
    event.record(&mut fields);
    let level = event.metadata().level().as_str().to_ascii_lowercase();
    // Tracing messages may contain error chains and paths. Only the
    // internal code selects a reviewed, fixed shared-stream template.
    let message = match fields.code.as_str() {
      "manager_shutdown" => "IsOS manager shutdown requested",
      "manager_stopped" => "IsOS manager stopped",
      "manager_request_failed" => "IsOS manager request failed",
      "manager_client_failed" => "IsOS manager client failed",
      "manager_accept_failed" => "IsOS manager accept failed",
      "deployment_supervisor_failed" => "Deployment supervisor failed",
      "service_supervisor_failed" => "Service supervisor failed",
      "workbench_failed" => "IsOS workbench failed",
      _ => "IsOS manager event",
    };
    let _ = self.0.append_event("isos", &level, &fields.code, message);
  }
}

pub fn install(store: LogStore) -> Result<()> {
  tracing::subscriber::set_global_default(
    tracing_subscriber::registry().with(ManagerLayer(store)),
  )?;
  Ok(())
}
