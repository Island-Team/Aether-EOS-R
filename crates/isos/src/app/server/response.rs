//! Versioned API envelope and public error mapping.
use super::*;
use crate::interfaces::http::{Envelope, Error};

pub(super) type ApiResult = std::result::Result<Json<Value>, (StatusCode, Json<Value>)>;
pub(super) fn err(status: StatusCode, message: impl ToString) -> (StatusCode, Json<Value>) {
  fail(status, error_code(status), message, Value::Null)
}

pub(super) fn fail(
  status: StatusCode,
  code: impl AsRef<str>,
  message: impl ToString,
  details: Value,
) -> (StatusCode, Json<Value>) {
  let message = message.to_string();
  let code = code.as_ref();
  (
    status,
    Json(json!(Envelope::<Value>::failure(Error {
      code: code.to_owned(),
      message,
      details,
      correlation_id: crate::runtime::manager::next_correlation_id(),
    }))),
  )
}

pub(super) fn diagnostic_failure(
  status: StatusCode,
  message: &'static str,
  diagnostic: Value,
) -> (StatusCode, Json<Value>) {
  let code = diagnostic["code"]
    .as_str()
    .unwrap_or(error_code(status))
    .to_owned();
  fail(status, code, message, json!({"diagnostics":[diagnostic]}))
}

pub(super) fn internal_err(
  status: StatusCode,
  error: &impl std::fmt::Display,
) -> (StatusCode, Json<Value>) {
  tracing::warn!(event_code = "http_request_failed", %error, "IsOS HTTP operation failed");
  let message = match status {
    StatusCode::BAD_REQUEST => "invalid request",
    StatusCode::NOT_FOUND => "requested resource is unavailable",
    StatusCode::CONFLICT => "request conflicts with current state",
    StatusCode::UNPROCESSABLE_ENTITY => "request failed validation",
    StatusCode::SERVICE_UNAVAILABLE => "required owner is unavailable",
    _ => "IsOS operation failed",
  };
  err(status, message)
}

pub(super) fn error_code(status: StatusCode) -> &'static str {
  match status {
    StatusCode::BAD_REQUEST => "invalid_request",
    StatusCode::UNAUTHORIZED => "unauthorized",
    StatusCode::FORBIDDEN => "forbidden",
    StatusCode::NOT_FOUND => "not_found",
    StatusCode::CONFLICT => "revision_conflict",
    StatusCode::UNPROCESSABLE_ENTITY => "admission_rejected",
    StatusCode::SERVICE_UNAVAILABLE => "owner_unavailable",
    _ => "request_failed",
  }
}

pub(super) fn ok(data: Value) -> ApiResult {
  Ok(Json(json!(Envelope::success(data))))
}
