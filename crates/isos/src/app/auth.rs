//! GitHub Device Flow login and short-lived Island-port credentials.

use crate::{app::config, domain::packages::CATALOG};
use anyhow::{Context, Result, ensure};
use reqwest::blocking::{Client, Response};
use serde::{Deserialize, Serialize, de::DeserializeOwned};
use std::{
  fs,
  path::PathBuf,
  thread,
  time::{Duration, Instant},
};

const GITHUB_DEVICE_URL: &str = "https://github.com/login/device/code";
const GITHUB_TOKEN_URL: &str = "https://github.com/login/oauth/access_token";
const GITHUB_DEVICE_GRANT: &str = "urn:ietf:params:oauth:grant-type:device_code";

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct User {
  pub id: String,
  pub username: Option<String>,
  pub email: Option<String>,
  pub role: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Credentials {
  access_token: String,
  refresh_token: String,
  pub user: User,
}

#[derive(Debug, Deserialize)]
struct Envelope<T> {
  success: bool,
  data: Option<T>,
  error: Option<ApiError>,
}

#[derive(Debug, Deserialize)]
struct ApiError {
  code: String,
  message: String,
}

#[derive(Debug, Deserialize)]
struct GithubConfig {
  client_id: String,
  #[serde(default)]
  device_flow_enabled: bool,
}

#[derive(Debug, Deserialize)]
struct DeviceCode {
  device_code: String,
  user_code: String,
  verification_uri: String,
  expires_in: u64,
  interval: Option<u64>,
}

#[derive(Debug, Deserialize)]
struct DeviceToken {
  access_token: Option<String>,
  error: Option<String>,
}

fn credentials_path(paths: &config::Paths) -> PathBuf {
  paths.config.join("auth.json")
}

fn client() -> Result<Client> {
  Ok(
    Client::builder()
      .timeout(Duration::from_secs(15))
      .user_agent(format!("IsOS/{}", env!("CARGO_PKG_VERSION")))
      .build()?,
  )
}

fn decode<T: DeserializeOwned>(response: Response) -> Result<T> {
  let status = response.status();
  let bytes = response.bytes()?;
  ensure!(
    status.is_success(),
    "request failed with HTTP {status}: {}",
    String::from_utf8_lossy(&bytes)
  );
  Ok(serde_json::from_slice(&bytes)?)
}

fn api_data<T: DeserializeOwned>(response: Response) -> Result<T> {
  let status = response.status();
  let bytes = response.bytes()?;
  let envelope: Envelope<T> = serde_json::from_slice(&bytes)
    .with_context(|| format!("Island-port returned an invalid response (HTTP {status})"))?;
  ensure!(
    status.is_success() && envelope.success,
    "Island-port: {}",
    envelope
      .error
      .map(|error| format!("{} ({})", error.message, error.code))
      .unwrap_or_else(|| format!("request failed (HTTP {status})"))
  );
  envelope
    .data
    .ok_or_else(|| anyhow::anyhow!("Island-port returned no response data"))
}

fn save(paths: &config::Paths, credentials: &Credentials) -> Result<()> {
  let bytes = serde_json::to_vec_pretty(credentials)?;
  config::atomic_write(&credentials_path(paths), &bytes)
}

pub fn read(paths: &config::Paths) -> Result<Credentials> {
  let path = credentials_path(paths);
  let metadata = fs::metadata(&path).with_context(|| {
    format!(
      "not logged in; run `isos auth login` ({} missing)",
      path.display()
    )
  })?;
  use std::os::unix::fs::PermissionsExt;
  ensure!(
    metadata.permissions().mode() & 0o077 == 0,
    "IsOS credential file permissions are too broad; run `chmod 600 {}`",
    path.display()
  );
  Ok(serde_json::from_slice(&fs::read(path)?)?)
}

fn post_refresh(
  paths: &config::Paths,
  client: &Client,
  refresh_token: &str,
) -> Result<Credentials> {
  let response = client
    .post(format!("{CATALOG}/api/auth/refresh"))
    .json(&serde_json::json!({"refresh_token": refresh_token}))
    .send()?;
  let credentials: Credentials = api_data(response)?;
  save(paths, &credentials)?;
  Ok(credentials)
}

pub fn authorized_post<T: Serialize>(
  paths: &config::Paths,
  url: &str,
  body: &T,
) -> Result<Response> {
  ensure!(
    url.starts_with("https://"),
    "Island-port URLs require HTTPS"
  );
  let client = client()?;
  let mut credentials = read(paths)?;
  let response = client
    .post(url)
    .bearer_auth(&credentials.access_token)
    .json(body)
    .send()?;
  if response.status() != reqwest::StatusCode::UNAUTHORIZED {
    return Ok(response);
  }
  credentials = post_refresh(paths, &client, &credentials.refresh_token)
    .context("Island-port session expired; run `isos auth login` again")?;
  Ok(
    client
      .post(url)
      .bearer_auth(credentials.access_token)
      .json(body)
      .send()?,
  )
}

pub fn login(paths: &config::Paths) -> Result<()> {
  let client = client()?;
  let config: GithubConfig = api_data(
    client
      .get(format!("{CATALOG}/api/auth/github/config"))
      .send()?,
  )?;
  ensure!(
    config.device_flow_enabled,
    "GitHub CLI login is disabled on Island-port; enable Device Flow for the IslandOS GitHub OAuth app and set ISLAND_GITHUB_DEVICE_FLOW_ENABLED=true"
  );

  let device: DeviceCode = decode(
    client
      .post(GITHUB_DEVICE_URL)
      .header("Accept", "application/json")
      .form(&[
        ("client_id", config.client_id.as_str()),
        ("scope", "user:email"),
      ])
      .send()?,
  )?;
  ensure!(
    device.device_code.len() <= 512
      && device.user_code.len() <= 32
      && device.verification_uri == "https://github.com/login/device"
      && (60..=1800).contains(&device.expires_in),
    "GitHub returned invalid device authorization data"
  );
  println!(
    "Open {} and enter code {}",
    device.verification_uri, device.user_code
  );
  println!("Waiting for GitHub authorization…");

  let started = Instant::now();
  let mut interval = Duration::from_secs(device.interval.unwrap_or(5).clamp(1, 30));
  let github_token = loop {
    if started.elapsed() >= Duration::from_secs(device.expires_in) {
      anyhow::bail!("GitHub device code expired; run `isos auth login` again");
    }
    thread::sleep(interval);
    let response = client
      .post(GITHUB_TOKEN_URL)
      .header("Accept", "application/json")
      .form(&[
        ("client_id", config.client_id.as_str()),
        ("device_code", device.device_code.as_str()),
        ("grant_type", GITHUB_DEVICE_GRANT),
      ])
      .send()?;
    let token: DeviceToken = decode(response)?;
    if let Some(access_token) = token.access_token {
      break access_token;
    }
    match token.error.as_deref() {
      Some("authorization_pending") => continue,
      Some("slow_down") => {
        interval += Duration::from_secs(5);
        continue;
      }
      Some("access_denied") => anyhow::bail!("GitHub authorization was denied"),
      Some("expired_token") => {
        anyhow::bail!("GitHub device code expired; run `isos auth login` again")
      }
      Some(code) => anyhow::bail!("GitHub device authorization failed: {code}"),
      None => anyhow::bail!("GitHub returned neither an access token nor an error"),
    }
  };

  let response = client
    .post(format!("{CATALOG}/api/auth/github/device-exchange"))
    .json(&serde_json::json!({"access_token": github_token}))
    .send()?;
  let credentials: Credentials = api_data(response)?;
  save(paths, &credentials)?;
  println!(
    "Signed in to Island-port as {} ({})",
    credentials.user.username.as_deref().unwrap_or("user"),
    credentials.user.role
  );
  Ok(())
}

pub fn logout(paths: &config::Paths) -> Result<()> {
  let path = credentials_path(paths);
  if !path.exists() {
    println!("not logged in");
    return Ok(());
  }
  let credentials = read(paths)?;
  let client = client()?;
  let response = client
    .post(format!("{CATALOG}/api/auth/logout"))
    .json(&serde_json::json!({"refresh_token": credentials.refresh_token}))
    .send()?;
  let _: serde_json::Value = api_data(response)?;
  fs::remove_file(path)?;
  println!("logged out of Island-port");
  Ok(())
}

pub fn status(paths: &config::Paths) -> Result<()> {
  match read(paths) {
    Ok(credentials) => println!(
      "signed in as {} ({})",
      credentials.user.username.as_deref().unwrap_or("user"),
      credentials.user.role
    ),
    Err(_error) if !credentials_path(paths).exists() => println!("not logged in"),
    Err(error) => return Err(error),
  }
  Ok(())
}

#[cfg(test)]
mod tests {
  use super::{Credentials, DeviceCode, Envelope, User, credentials_path, read, save};
  use crate::app::config::Paths;
  use std::{fs, os::unix::fs::PermissionsExt, path::PathBuf};

  #[test]
  fn parses_device_code_and_auth_envelope() {
    let device: DeviceCode = serde_json::from_value(serde_json::json!({
        "device_code": "opaque-device-code",
        "user_code": "ABCD-EFGH",
        "verification_uri": "https://github.com/login/device",
        "expires_in": 900,
        "interval": 5
    }))
    .unwrap();
    assert_eq!(device.user_code, "ABCD-EFGH");
    let envelope: Envelope<User> = serde_json::from_value(serde_json::json!({
        "success": true,
        "data": {"id":"user-id","username":"dev","email":null,"role":"developer"}
    }))
    .unwrap();
    assert!(envelope.success);
    assert_eq!(envelope.data.unwrap().role, "developer");
  }

  #[test]
  fn credentials_are_saved_with_owner_only_permissions() {
    let root = std::env::temp_dir().join(format!("isos-auth-test-{}", std::process::id()));
    let paths = Paths {
      config: root.join("config"),
      data: root.join("data"),
      runtime: root.join("run"),
    };
    paths.prepare().unwrap();
    let credentials = Credentials {
      access_token: "short-lived-access-token".into(),
      refresh_token: "rotating-refresh-token".into(),
      user: User {
        id: "user-id".into(),
        username: Some("developer".into()),
        email: None,
        role: "developer".into(),
      },
    };
    save(&paths, &credentials).unwrap();
    let path: PathBuf = credentials_path(&paths);
    assert_eq!(
      fs::metadata(&path).unwrap().permissions().mode() & 0o777,
      0o600
    );
    assert_eq!(read(&paths).unwrap().user.role, "developer");
    fs::remove_dir_all(root).unwrap();
  }
}
