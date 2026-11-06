use serde::{Deserialize, Serialize};
use serde_json::Value;

/// The owner-only socket contract. Each operation carries only the fields it
/// can use; unknown fields and operations fail before reaching the manager.
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct WireRequest {
  #[serde(default)]
  pub(crate) schema_version: Value,
  #[serde(flatten)]
  pub(crate) command: WireCommand,
}

#[derive(Deserialize, Serialize)]
#[serde(tag = "op", content = "payload", rename_all = "snake_case")]
pub(crate) enum WireCommand {
  ManagerIdentity,
  ManagerShutdown,
  DeploymentStart(DeploymentMutation),
  DeploymentRestart(DeploymentMutation),
  DeploymentStop(DeploymentMutation),
  DeploymentStatus(DeploymentTarget),
  DeploymentActivePackages(DeploymentTarget),
  ServicesList,
  ServicesValidate,
  ServicesStatus,
  ServicesStart(ServiceTarget),
  ServicesStop(ServiceTarget),
  ServicesRestart(ServiceTarget),
  ServicesLogs(ServiceLogs),
  LogAppend(LogAppend),
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct DeploymentTarget {
  pub(crate) deployment: String,
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct DeploymentMutation {
  pub(crate) deployment: String,
  #[serde(default, skip_serializing_if = "Option::is_none")]
  pub(crate) revision: Option<String>,
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct ServiceTarget {
  pub(crate) service_id: String,
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct ServiceLogs {
  pub(crate) service_id: String,
  #[serde(default, skip_serializing_if = "Option::is_none")]
  pub(crate) cursor: Option<String>,
  #[serde(default, skip_serializing_if = "Option::is_none")]
  pub(crate) limit: Option<usize>,
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct LogAppend {
  pub(crate) deployment: String,
  pub(crate) log: LogEventInput,
}

impl WireRequest {
  pub(crate) fn new(command: WireCommand) -> Self {
    Self {
      schema_version: serde_json::json!("0.1.0"),
      command,
    }
  }

  pub(crate) fn validate(&self) -> anyhow::Result<()> {
    anyhow::ensure!(
      self.schema_version == "0.1.0",
      "unsupported_schema_version: manager schema_version must be 0.1.0"
    );
    Ok(())
  }
}

impl WireCommand {
  pub(crate) fn op(&self) -> &'static str {
    match self {
      Self::ManagerIdentity => "manager_identity",
      Self::ManagerShutdown => "manager_shutdown",
      Self::DeploymentStart(_) => "deployment_start",
      Self::DeploymentRestart(_) => "deployment_restart",
      Self::DeploymentStop(_) => "deployment_stop",
      Self::DeploymentStatus(_) => "deployment_status",
      Self::DeploymentActivePackages(_) => "deployment_active_packages",
      Self::ServicesList => "services_list",
      Self::ServicesValidate => "services_validate",
      Self::ServicesStatus => "services_status",
      Self::ServicesStart(_) => "services_start",
      Self::ServicesStop(_) => "services_stop",
      Self::ServicesRestart(_) => "services_restart",
      Self::ServicesLogs(_) => "services_logs",
      Self::LogAppend(_) => "log_append",
    }
  }

  pub(crate) fn deployment(&self) -> Option<&str> {
    match self {
      Self::DeploymentStart(v) | Self::DeploymentRestart(v) | Self::DeploymentStop(v) => {
        Some(&v.deployment)
      }
      Self::DeploymentStatus(v) | Self::DeploymentActivePackages(v) => Some(&v.deployment),
      Self::LogAppend(v) => Some(&v.deployment),
      _ => None,
    }
  }

  pub(crate) fn deployment_call(
    op: &str,
    deployment: &str,
    revision: Option<String>,
  ) -> anyhow::Result<Self> {
    anyhow::ensure!(
      revision.is_none()
        || matches!(
          op,
          "deployment_start" | "deployment_restart" | "deployment_stop"
        ),
      "revision is only valid for a deployment mutation"
    );
    let mutation = || DeploymentMutation {
      deployment: deployment.to_owned(),
      revision: revision.clone(),
    };
    let target = || DeploymentTarget {
      deployment: deployment.to_owned(),
    };
    match op {
      "manager_identity" => Ok(Self::ManagerIdentity),
      "manager_shutdown" => Ok(Self::ManagerShutdown),
      "deployment_start" => Ok(Self::DeploymentStart(mutation())),
      "deployment_restart" => Ok(Self::DeploymentRestart(mutation())),
      "deployment_stop" => Ok(Self::DeploymentStop(mutation())),
      "deployment_status" => Ok(Self::DeploymentStatus(target())),
      "deployment_active_packages" => Ok(Self::DeploymentActivePackages(target())),
      _ => anyhow::bail!("unknown manager operation"),
    }
  }

  pub(crate) fn service_call(
    op: &str,
    service_id: Option<String>,
    limit: Option<usize>,
  ) -> anyhow::Result<Self> {
    anyhow::ensure!(
      service_id.is_none()
        || matches!(
          op,
          "services_start" | "services_stop" | "services_restart" | "services_logs"
        ),
      "service_id is only valid for a selected service"
    );
    anyhow::ensure!(
      limit.is_none() || op == "services_logs",
      "limit is only valid for service logs"
    );
    match op {
      "services_list" => Ok(Self::ServicesList),
      "services_validate" => Ok(Self::ServicesValidate),
      "services_status" => Ok(Self::ServicesStatus),
      "services_start" | "services_stop" | "services_restart" | "services_logs" => {
        let service_id = service_id.ok_or_else(|| anyhow::anyhow!("service_id is required"))?;
        Ok(match op {
          "services_start" => Self::ServicesStart(ServiceTarget { service_id }),
          "services_stop" => Self::ServicesStop(ServiceTarget { service_id }),
          "services_restart" => Self::ServicesRestart(ServiceTarget { service_id }),
          _ => Self::ServicesLogs(ServiceLogs {
            service_id,
            cursor: None,
            limit,
          }),
        })
      }
      _ => anyhow::bail!("unknown service operation"),
    }
  }
}

#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct LogEventInput {
  pub(crate) source: String,
  pub(crate) level: String,
  pub(crate) message: String,
  #[serde(default)]
  pub(crate) event_code: Option<String>,
}
#[derive(Deserialize, Serialize)]
pub(crate) struct ApiResponse {
  pub(crate) schema_version: String,
  pub(crate) ok: bool,
  pub(crate) data: Value,
  pub(crate) error: Option<WireError>,
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct WireError {
  pub(crate) code: String,
  pub(crate) message: String,
  pub(crate) correlation_id: String,
}

#[cfg(test)]
mod wire_tests {
  use super::*;

  #[test]
  fn v1_request_rejects_flat_or_cross_operation_fields() {
    for source in [
      r#"{"schema_version":1,"op":"deployment_stop","payload":{"deployment":"default"}}"#,
      r#"{"schema_version":"0.1.0","op":"deployment_stop","deployment":"default"}"#,
      r#"{"schema_version":"0.1.0","op":"deployment_stop","payload":{"deployment":"default","service_id":"x"}}"#,
      r#"{"schema_version":"0.1.0","op":"manager_shutdown","payload":{"deployment":"default"}}"#,
      r#"{"schema_version":"0.1.0","op":"unknown"}"#,
    ] {
      let result = serde_json::from_str::<WireRequest>(source)
        .map_err(anyhow::Error::from)
        .and_then(|request| request.validate());
      assert!(result.is_err(), "accepted invalid request: {source}");
    }
  }

  #[test]
  fn v1_mutation_round_trips_only_its_declared_fields() {
    let source = r#"{"schema_version":"0.1.0","op":"deployment_restart","payload":{"deployment":"alpha","revision":"abcdef"}}"#;
    let decoded = serde_json::from_str::<WireRequest>(source).unwrap();
    decoded.validate().unwrap();
    assert_eq!(decoded.command.op(), "deployment_restart");
    assert_eq!(decoded.command.deployment(), Some("alpha"));
    let encoded = serde_json::to_value(decoded).unwrap();
    assert_eq!(encoded["op"], "deployment_restart");
    assert_eq!(encoded["payload"]["revision"], "abcdef");
    assert!(encoded.get("service_id").is_none());
  }

  #[test]
  fn old_numeric_request_has_version_diagnostic() {
    let decoded: WireRequest =
      serde_json::from_str(r#"{"schema_version":2,"op":"manager_identity"}"#).unwrap();
    let error = decoded.validate().unwrap_err();
    assert!(error.to_string().starts_with("unsupported_schema_version:"));
  }

  #[test]
  fn typed_cli_constructors_reject_irrelevant_fields() {
    assert!(
      WireCommand::deployment_call("deployment_status", "default", Some("old".into())).is_err()
    );
    assert!(WireCommand::service_call("services_status", Some("svc".into()), None).is_err());
    assert!(WireCommand::service_call("services_start", Some("svc".into()), Some(5)).is_err());
  }
}
