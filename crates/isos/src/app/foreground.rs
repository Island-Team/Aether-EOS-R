use crate::{
  app::config,
  app::server as http,
  app::isosui,
  runtime::logs as log_store,
  runtime::manager::{bind_daemon, daemon, manager_call},
  runtime::logs::manager_log,
  runtime::process::preflight,
  domain::graph::starter,
};
use anyhow::{Context, Result, bail, ensure};
use serde_json::Value;
use std::{
  net::TcpStream,
  os::unix::{net::UnixStream, process::CommandExt},
  path::{Path, PathBuf},
  process::{Child, Command, Stdio},
  sync::atomic::{AtomicBool, Ordering},
  time::{Duration, Instant},
};

pub(crate) static INTERRUPTED: AtomicBool = AtomicBool::new(false);

extern "C" fn interrupt_handler(_: libc::c_int) {
  INTERRUPTED.store(true, Ordering::SeqCst);
}

pub(crate) fn catch_interrupts() -> Result<()> {
  INTERRUPTED.store(false, Ordering::SeqCst);
  unsafe {
    let handler = interrupt_handler as *const () as libc::sighandler_t;
    ensure!(
      libc::signal(libc::SIGINT, handler) != libc::SIG_ERR,
      "install SIGINT handler"
    );
    ensure!(
      libc::signal(libc::SIGTERM, handler) != libc::SIG_ERR,
      "install SIGTERM handler"
    );
  }
  Ok(())
}

pub(crate) fn ui_source() -> Result<PathBuf> {
  let source = isosui::source_root()?;
  ensure!(
    source.join("node_modules").is_dir(),
    "IsosUI dependencies missing at {}; run npm ci there",
    source.display()
  );
  Ok(source)
}

pub(crate) struct Frontend(Child);

impl Frontend {
  fn spawn(source: &Path, logs: log_store::LogStore) -> Result<Self> {
    let port = http::ui_port()?;
    ensure!(
      TcpStream::connect(("127.0.0.1", port)).is_err(),
      "IsosUI port 127.0.0.1:{port} is already in use"
    );
    let mut command = Command::new("npm");
    command
      .arg("start")
      .current_dir(source)
      .env("ISOSUI_HOST", "127.0.0.1")
      .env("ISOSUI_PORT", port.to_string())
      .env("ISOSUI_API_ORIGIN", http::local_origin()?)
      .stdin(Stdio::null())
      .stdout(Stdio::piped())
      .stderr(Stdio::piped());
    let parent_pid = std::process::id();
    unsafe {
      command.pre_exec(move || {
        if libc::setsid() < 0 {
          return Err(std::io::Error::last_os_error());
        }
        if libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGTERM) < 0 {
          return Err(std::io::Error::last_os_error());
        }
        if libc::getppid() as u32 != parent_pid {
          return Err(std::io::Error::from_raw_os_error(libc::ESRCH));
        }
        Ok(())
      });
    }
    let mut child = command.spawn()?;
    let pid = child.id();
    if let Some(stdout) = child.stdout.take() {
      log_store::capture(
        stdout,
        logs.clone(),
        "ui".into(),
        "stdout",
        crate::runtime::manager::manager_session_id().into(),
        pid,
        None,
      );
    }
    if let Some(stderr) = child.stderr.take() {
      log_store::capture(
        stderr,
        logs,
        "ui".into(),
        "stderr",
        crate::runtime::manager::manager_session_id().into(),
        pid,
        None,
      );
    }
    Ok(Self(child))
  }

  fn ready(&mut self, port: u16) -> Result<()> {
    let deadline = Instant::now() + Duration::from_secs(30);
    while Instant::now() < deadline {
      if let Some(status) = self.0.try_wait()? {
        bail!("IsosUI npm server exited before ready: {status}");
      }
      if TcpStream::connect(("127.0.0.1", port)).is_ok() {
        return Ok(());
      }
      if INTERRUPTED.load(Ordering::SeqCst) {
        return Ok(());
      }
      std::thread::sleep(Duration::from_millis(50));
    }
    bail!("IsosUI npm server did not listen on 127.0.0.1:{port} within 30 seconds")
  }
}

impl Drop for Frontend {
  fn drop(&mut self) {
    let group = -(self.0.id() as i32);
    unsafe { libc::kill(group, libc::SIGTERM) };
    let deadline = Instant::now() + Duration::from_secs(2);
    while Instant::now() < deadline {
      if self.0.try_wait().ok().flatten().is_some() {
        break;
      }
      std::thread::sleep(Duration::from_millis(20));
    }
    if unsafe { libc::kill(group, 0) } == 0 {
      unsafe { libc::kill(group, libc::SIGKILL) };
    }
    let _ = self.0.wait();
  }
}

pub(crate) fn open_browser() -> Result<()> {
  let url = http::ui_url()?;
  Command::new("xdg-open")
    .arg(&url)
    .stdin(Stdio::null())
    .stdout(Stdio::null())
    .stderr(Stdio::null())
    .spawn()
    .context("open IsosUI in browser")?;
  Ok(())
}

pub(crate) fn ensure_setup(paths: &config::Paths, name: &str) -> Result<()> {
  let old_server = Command::new("systemctl")
    .args(["--user", "is-active", "--quiet", "isos-server.service"])
    .stdout(Stdio::null())
    .stderr(Stdio::null())
    .status();
  ensure!(
    !old_server.is_ok_and(|status| status.success()),
    "obsolete isos-server.service is active; stop it with `systemctl --user stop isos-server.service`, then use `isos serve`"
  );
  paths.prepare()?;
  if !paths.config.join("mode").exists() && std::env::var_os("ISOS_MODE").is_none() {
    paths.set_mode("dev")?;
  }
  starter::init(paths, name)?;
  Ok(())
}

pub(crate) fn serve_foreground(
  paths: config::Paths,
  name: String,
  ui_live: bool,
  browser: bool,
  run_graph: bool,
) -> Result<()> {
  ensure_setup(&paths, &name)?;
  preflight::verify_active_isos(&paths)?;
  ensure!(
    !ui_live || paths.mode()? == "dev",
    "--ui-live is available only in dev mode"
  );
  if UnixStream::connect(paths.socket()).is_ok() {
    let identity = manager_call(&paths, &name, "manager_identity")?;
    bail!("IsOS manager is already running: {identity}");
  }
  let port = http::ui_port()?;
  ensure!(
    TcpStream::connect(("127.0.0.1", http::local_port()?)).is_err(),
    "IsOS API port belongs to another listener; stop it or configure a different port"
  );
  catch_interrupts()?;
  let logs = log_store::LogStore::new(&paths, &name)?;
  logs.begin_manager_session()?;
  manager_log::install(logs.clone())?;
  logs.append("isos", "info", "Starting IsOS workbench")?;
  let main_log = logs.root().join("events.log");
  let mut frontend = None;
  let mut ui_issue = None;
  let static_ui = if ui_live {
    match isosui::bundle_for_serve(&paths) {
      Ok(_) => frontend = Some(Frontend::spawn(&ui_source()?, logs.clone())?),
      Err(error) => {
        ui_issue = Some(format!(
          "IsosUI bundle unavailable or incompatible: {error}"
        ));
      }
    }
    None
  } else {
    match isosui::bundle_for_serve(&paths) {
      Ok(directory) => {
        ensure!(
          TcpStream::connect(("127.0.0.1", port)).is_err(),
          "IsosUI port belongs to another listener; stop it or configure a different port"
        );
        Some(std::thread::spawn(move || http::serve_ui(directory)))
      }
      Err(error) => {
        ui_issue = Some(format!(
          "IsosUI bundle unavailable or incompatible: {error}"
        ));
        None
      }
    }
  };
  let listener = bind_daemon(&paths)?;
  let api_paths = paths.clone();
  let manager_name = name.clone();
  let manager = std::thread::spawn(move || daemon(&api_paths, &manager_name, listener));
  let http_paths = paths.clone();
  let http_name = name.clone();
  let http_server = std::thread::spawn(move || http::serve(http_paths, http_name));
  let mut result = if let Some(frontend) = frontend.as_mut() {
    frontend.ready(port)
  } else if static_ui.is_some() {
    let deadline = Instant::now() + Duration::from_secs(30);
    loop {
      if TcpStream::connect(("127.0.0.1", port)).is_ok() {
        break Ok(());
      }
      if static_ui.as_ref().is_some_and(|ui| ui.is_finished()) {
        break Err(anyhow::anyhow!("IsosUI static server exited before ready"));
      }
      if Instant::now() >= deadline {
        break Err(anyhow::anyhow!("IsosUI static server did not become ready"));
      }
      std::thread::sleep(Duration::from_millis(50));
    }
  } else {
    let deadline = Instant::now() + Duration::from_secs(30);
    loop {
      if TcpStream::connect(("127.0.0.1", http::local_port()?)).is_ok()
        && UnixStream::connect(paths.socket()).is_ok()
      {
        break Ok(());
      }
      if http_server.is_finished() || manager.is_finished() {
        break Err(anyhow::anyhow!("IsOS API or manager exited before ready"));
      }
      if Instant::now() >= deadline {
        break Err(anyhow::anyhow!("IsOS API or manager did not become ready"));
      }
      std::thread::sleep(Duration::from_millis(50));
    }
  };
  if result.is_ok() && !INTERRUPTED.load(Ordering::SeqCst) {
    if static_ui.is_some() || frontend.is_some() {
      println!("{}", http::ui_url()?);
    } else if let Some(issue) = &ui_issue {
      eprintln!("{issue}; IsOS API and manager are available");
    }
    println!("IsOS API: {}", http::local_origin()?);
    println!("Unified log: {}", main_log.display());
    println!("IsOS workbench is running; press Ctrl+C to stop it.");
    if browser
      && (static_ui.is_some() || frontend.is_some())
      && let Err(error) = open_browser()
    {
      eprintln!(
        "Could not open browser: {error}. Open {} manually.",
        http::ui_url()?
      );
    }
    if run_graph {
      match manager_call(&paths, &name, "deployment_start") {
        Ok(started) => {
          if let Err(error) = print_run_startup(&paths, &name, &started) {
            eprintln!("Could not display graph status: {error}");
          }
        }
        Err(error) => {
          eprintln!("Graph did not start: {error}. Use the workbench to finish setup.")
        }
      }
    }
    while !INTERRUPTED.load(Ordering::SeqCst) {
      if let Some(frontend) = frontend.as_mut()
        && let Some(status) = frontend.0.try_wait()?
      {
        result = Err(anyhow::anyhow!("IsosUI npm server exited: {status}"));
        break;
      }
      if static_ui.as_ref().is_some_and(|ui| ui.is_finished()) {
        result = Err(anyhow::anyhow!("IsosUI static server exited"));
        break;
      }
      if http_server.is_finished() || manager.is_finished() {
        break;
      }
      std::thread::sleep(Duration::from_millis(100));
    }
  }
  INTERRUPTED.store(true, Ordering::SeqCst);
  drop(frontend);
  if let Some(ui) = static_ui {
    ui.join()
      .map_err(|_| anyhow::anyhow!("IsosUI server thread panicked"))??;
  }
  let http_result = http_server
    .join()
    .map_err(|_| anyhow::anyhow!("IsOS HTTP thread panicked"))?;
  let manager_result = manager
    .join()
    .map_err(|_| anyhow::anyhow!("IsOS manager thread panicked"))?;
  manager_result?;
  http_result?;
  if let Err(error) = &result {
    tracing::error!(
      event_code = "workbench_failed",
      "IsOS workbench failed: {error:#}"
    );
  }
  result
}

pub(crate) fn print_run_startup(paths: &config::Paths, name: &str, result: &Value) -> Result<()> {
  println!("{}", serde_json::to_string_pretty(result)?);
  println!("IsosUI: {}", http::ui_url()?);
  println!("IsOS API: {}", http::local_origin()?);
  println!(
    "Active revision: {}",
    result["active_revision"]
      .as_str()
      .context("start result lacks active revision")?
  );
  println!(
    "Unified log: {}",
    paths
      .data
      .join("logs")
      .join(name)
      .join("events.log")
      .display()
  );
  Ok(())
}

pub(crate) fn wait_for_interrupt(paths: &config::Paths, name: &str) -> Result<()> {
  println!("Deployment is running; press Ctrl+C to stop it.");
  loop {
    if INTERRUPTED.load(Ordering::SeqCst) {
      let result = manager_call(paths, name, "deployment_stop")?;
      println!("{}", serde_json::to_string_pretty(&result)?);
      if result["terminal"]["outcome"] == "unknown" {
        anyhow::bail!("dependent stop unconfirmed");
      }
      return Ok(());
    }
    std::thread::sleep(Duration::from_millis(200));
    let status = manager_call(paths, name, "deployment_status")?;
    if status["terminal"]["outcome"] == "unknown" {
      anyhow::bail!("dependent stop unconfirmed");
    }
    if status["state"] == "stopped" {
      return Ok(());
    }
  }
}
