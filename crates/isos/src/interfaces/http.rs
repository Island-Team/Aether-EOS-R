//! The v1 HTTP response envelope shared by all route domains.

use serde::Serialize;
use serde_json::Value;

#[derive(Serialize)]
pub(crate) struct Error {
  pub code: String,
  pub message: String,
  pub details: Value,
  pub correlation_id: String,
}

#[derive(Serialize)]
pub(crate) struct Envelope<T: Serialize> {
  schema_version: &'static str,
  ok: bool,
  data: Option<T>,
  error: Option<Error>,
}

impl<T: Serialize> Envelope<T> {
  pub(crate) fn success(data: T) -> Self {
    Self {
      schema_version: "0.1.0",
      ok: true,
      data: Some(data),
      error: None,
    }
  }

  pub(crate) fn failure(error: Error) -> Self {
    Self {
      schema_version: "0.1.0",
      ok: false,
      data: None,
      error: Some(error),
    }
  }
}

#[cfg(test)]
mod tests {
  use super::*;
  use serde_json::json;

  #[test]
  fn success_and_failure_have_exclusive_data_and_error() {
    let success = serde_json::to_value(Envelope::success(json!({"state":"ready"}))).unwrap();
    assert_eq!(
      success,
      json!({"schema_version":"0.1.0","ok":true,"data":{"state":"ready"},"error":null})
    );
    let failure = serde_json::to_value(Envelope::<Value>::failure(Error {
      code: "invalid_request".into(),
      message: "invalid request".into(),
      details: Value::Null,
      correlation_id: "req-1".into(),
    }))
    .unwrap();
    assert_eq!(failure["data"], Value::Null);
    assert_eq!(failure["ok"], false);
  }
}
