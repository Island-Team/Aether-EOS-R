//! Loopback listener ports, host/origin admission, and CORS policy.

use super::*;

pub fn local_port() -> Result<u16> {
  let port = std::env::var("ISOS_HTTP_PORT")
    .unwrap_or_else(|_| "16000".into())
    .parse::<u16>()?;
  anyhow::ensure!(port >= 1024, "ISOS_HTTP_PORT must be 1024..65535");
  Ok(port)
}
pub fn local_origin() -> Result<String> {
  Ok(format!("http://127.0.0.1:{}", local_port()?))
}
pub fn ui_port() -> Result<u16> {
  let port = std::env::var("ISOSUI_PORT")
    .unwrap_or_else(|_| "16001".into())
    .parse::<u16>()?;
  anyhow::ensure!(port >= 1024, "ISOSUI_PORT must be 1024..65535");
  anyhow::ensure!(
    port != local_port()?,
    "ISOSUI_PORT must differ from ISOS_HTTP_PORT"
  );
  Ok(port)
}
pub fn ui_url() -> Result<String> {
  Ok(format!("http://127.0.0.1:{}/en/isos", ui_port()?))
}
const HOSTED_ORIGINS: [&str; 2] = ["https://www.os-island.com", "https://os-island.com"];

pub(super) fn authorized(
  _state: &App,
  headers: &HeaderMap,
) -> std::result::Result<(), (StatusCode, Json<Value>)> {
  if headers
    .get("sec-fetch-site")
    .and_then(|value| value.to_str().ok())
    == Some("cross-site")
    && !headers.contains_key(header::ORIGIN)
  {
    return Err(err(
      StatusCode::FORBIDDEN,
      "cross-site request requires Origin",
    ));
  }
  Ok(())
}
fn allowed_origin(origin: &str) -> bool {
  local_origin().is_ok_and(|approved| origin == approved)
    || ui_port().is_ok_and(|port| origin == format!("http://127.0.0.1:{port}"))
    || HOSTED_ORIGINS.contains(&origin)
}

pub(super) async fn cors(request: Request<Body>, next: Next) -> Response {
  let port = match local_port() {
    Ok(port) => port,
    Err(_) => {
      return err(
        StatusCode::INTERNAL_SERVER_ERROR,
        "invalid API port configuration",
      )
      .into_response();
    }
  };
  let headers = request.headers();
  let host = headers
    .get(header::HOST)
    .and_then(|h| h.to_str().ok())
    .unwrap_or("");
  if host != format!("127.0.0.1:{port}") {
    return err(StatusCode::FORBIDDEN, "invalid Host").into_response();
  }
  let origin = headers
    .get(header::ORIGIN)
    .and_then(|h| h.to_str().ok())
    .map(str::to_owned);
  if headers.contains_key(header::ORIGIN) && !origin.as_deref().is_some_and(allowed_origin) {
    return err(StatusCode::FORBIDDEN, "invalid Origin").into_response();
  }
  let preflight = request.method() == Method::OPTIONS;
  if preflight {
    if origin.is_none() {
      return err(StatusCode::FORBIDDEN, "preflight requires Origin").into_response();
    }
    let method = headers
      .get(header::ACCESS_CONTROL_REQUEST_METHOD)
      .and_then(|h| h.to_str().ok());
    if !matches!(method, Some("GET" | "POST" | "PUT")) {
      return err(StatusCode::FORBIDDEN, "invalid preflight method").into_response();
    }
    if let Some(names) = headers
      .get(header::ACCESS_CONTROL_REQUEST_HEADERS)
      .and_then(|h| h.to_str().ok())
      && names
        .split(',')
        .any(|name| !matches!(name.trim().to_ascii_lowercase().as_str(), "content-type"))
    {
      return err(StatusCode::FORBIDDEN, "invalid preflight headers").into_response();
    }
  }
  let mut response = if preflight {
    StatusCode::NO_CONTENT.into_response()
  } else {
    next.run(request).await
  };
  if let Some(origin) = origin {
    response.headers_mut().insert(
      header::ACCESS_CONTROL_ALLOW_ORIGIN,
      HeaderValue::from_str(&origin).expect("approved Origin is a header value"),
    );
    response
      .headers_mut()
      .insert(header::VARY, HeaderValue::from_static("Origin"));
  }
  if preflight {
    response.headers_mut().insert(
      header::ACCESS_CONTROL_ALLOW_METHODS,
      HeaderValue::from_static("GET, POST, PUT"),
    );
    response.headers_mut().insert(
      header::ACCESS_CONTROL_ALLOW_HEADERS,
      HeaderValue::from_static("Content-Type"),
    );
    response.headers_mut().insert(
      header::ACCESS_CONTROL_MAX_AGE,
      HeaderValue::from_static("600"),
    );
  }
  response
}

pub(super) async fn preflight() -> StatusCode {
  StatusCode::NO_CONTENT
}
