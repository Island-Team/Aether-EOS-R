use crate::{
  app::auth,
  domain::packages,
  app::cli::{
    AuthCommand, Cli, CommandLine, DeploymentCommand, VersionCommand, config_command, library,
    package_command, print_init_next_steps, services_command,
  },
  app::config,
  runtime::owners::core_setup,
  app::foreground::{
    catch_interrupts, ensure_setup, open_browser, print_run_startup, serve_foreground,
    wait_for_interrupt,
  },
  app::server as http,
  runtime::logs as log_store,
  runtime::manager::{bind_daemon, daemon, manager_call, service_call},
  runtime::logs::manager_log,
  app::physical_stop,
  runtime::process,
  runtime::process::readiness,
  domain::graph::starter,
  domain::version,
};
use anyhow::{Context, Result};
use clap::{CommandFactory, Parser};
use clap_complete::generate;
use serde_json::json;
use std::{
  fs,
  os::unix::net::UnixStream,
  process::{Command, Stdio},
  time::Instant,
};

pub(crate) fn run() -> Result<()> {
  config::load_runtime_config()?;
  let cli = Cli::parse();
  if let Some(shell) = cli.generate_completion {
    generate(shell, &mut Cli::command(), "isos", &mut std::io::stdout());
    return Ok(());
  }
  let paths = config::Paths::discover()?;
  let name = cli.deployment;
  let Some(command) = cli.command else {
    ensure_setup(&paths, &name)?;
    if UnixStream::connect(paths.socket()).is_ok() {
      let identity = manager_call(&paths, &name, "manager_identity")?;
      println!("IsOS workbench already running: {}", identity["ui_url"]);
      if let Err(error) = open_browser() {
        eprintln!(
          "Could not open browser: {error}. Open {} manually.",
          http::ui_url()?
        );
      }
      return Ok(());
    }
    return serve_foreground(paths, name, false, true, false);
  };
  match command {
    CommandLine::Version { command } => match command {
      VersionCommand::Check { json } => {
        let active = if UnixStream::connect(paths.socket()).is_ok() {
          manager_call(&paths, &name, "deployment_status").ok()
        } else {
          None
        };
        let mut report = readiness::report(&paths, &name, active.as_ref());
        let support = version::bundled()?;
        let graph_version = report["graph"]["version"].as_str();
        report["version_support"] = json!({
          "status":support.graph_status(graph_version),
          "map":support,
        });
        if json {
          println!("{}", serde_json::to_string_pretty(&report)?);
        } else {
          println!(
            "IsOS {} | graph {}",
            env!("CARGO_PKG_VERSION"),
            report["graph"]["path"].as_str().unwrap_or("unavailable")
          );
          println!(
            "Local version support: {} | target graph {}",
            report["version_support"]["status"]
              .as_str()
              .unwrap_or("unknown"),
            report["version_support"]["map"]["target_graph_format"]
              .as_str()
              .unwrap_or("unknown")
          );
          for core in report["cores"].as_array().into_iter().flatten() {
            let name = core["core"].as_str().unwrap_or("core");
            let version = core["version"].as_str().unwrap_or("unknown");
            let state = if core["enabled"] == false {
              "not enabled"
            } else if core["ready"] == true {
              "ready"
            } else {
              "blocked"
            };
            println!(
              "{name}: {state} | version {version} | {}",
              core["path"].as_str().unwrap_or("not configured")
            );
            println!(
              "  Graph configuration: {} | Running: {}",
              if core["graph_configured"] == true {
                "saved"
              } else {
                "missing"
              },
              if core["running"] == true { "yes" } else { "no" }
            );
            if let Some(reason) = core["reason"].as_str() {
              println!("  Reason: {reason}");
            }
            if let Some(action) = core["action"].as_str() {
              println!("  Next: {action}");
            }
          }
          println!(
            "IsosUI: {} | {}",
            if report["isosui"]["ready"] == true {
              "ready"
            } else {
              "blocked"
            },
            report["isosui"]["path"]
              .as_str()
              .unwrap_or("not configured")
          );
        }
      }
    },
    CommandLine::Init { mode } => {
      paths.prepare()?;
      if let Some(mode) = mode {
        paths.set_mode(&mode)?;
      } else if !paths.config.join("mode").exists() {
        paths.set_mode("dev")?;
      }
      let graph_path = starter::init(&paths, &name)?;
      let refreshed = core_setup::refresh_after_init(&paths, &name)?;
      println!("editable graph: {}", graph_path.display());
      if let Some(revision) = refreshed {
        println!(
          "refreshed core launch inputs for graph {revision}; Run or Restart to use the configured build"
        );
        if let Err(error) = crate::app::cli::clear_saved_setup_error(&paths, &name) {
          eprintln!("core inputs refreshed, but could not clear prior setup error: {error}");
        }
        println!("Run `isos version check` to confirm readiness.");
      } else {
        if packages::list(&paths)?.is_empty() {
          println!(
            "No package release is installed; install or link an exact release before selecting package-contributed nodes."
          );
        }
        print_init_next_steps(paths.mode()?);
      }
    }
    CommandLine::Open => {
      if UnixStream::connect(paths.socket()).is_err() {
        return serve_foreground(paths, name, false, true, false);
      }
      manager_call(&paths, &name, "manager_identity")?;
      let url = http::ui_url()?;
      Command::new("xdg-open")
        .arg(&url)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .context("launch IsosUI with xdg-open")?;
      println!("{url}");
    }
    CommandLine::Run => {
      let setup_started = Instant::now();
      ensure_setup(&paths, &name)?;
      crate::runtime::manager::emit_phase_timing(
        "start",
        "cli_setup_complete",
        setup_started.elapsed().as_millis() as u64,
        setup_started.elapsed().as_millis() as u64,
      );
      if UnixStream::connect(paths.socket()).is_err() {
        return serve_foreground(paths, name, false, true, true);
      }
      catch_interrupts()?;
      print_run_startup(
        &paths,
        &name,
        &manager_call(&paths, &name, "deployment_start")?,
      )?;
      wait_for_interrupt(&paths, &name)?;
    }
    CommandLine::Serve { ui_live } => {
      serve_foreground(paths, name, ui_live, false, false)?;
    }
    CommandLine::Auth { command } => match command {
      AuthCommand::Login => auth::login(&paths)?,
      AuthCommand::Logout => auth::logout(&paths)?,
      AuthCommand::Whoami => auth::status(&paths)?,
    },
    CommandLine::Start => {
      catch_interrupts()?;
      print_run_startup(
        &paths,
        &name,
        &manager_call(&paths, &name, "deployment_start")?,
      )?;
      wait_for_interrupt(&paths, &name)?;
    }
    CommandLine::Stop => match manager_call(&paths, &name, "manager_shutdown") {
      Ok(result) => {
        println!("{}", serde_json::to_string_pretty(&result)?);
        if result["terminal"].as_array().is_some_and(|terminals| {
          terminals
            .iter()
            .any(|terminal| terminal["outcome"] == "unknown")
        }) {
          anyhow::bail!("dependent stop unconfirmed");
        }
      }
      Err(_) if UnixStream::connect(paths.socket()).is_err() => {
        println!("{}", json!({"stopped":[],"manager":"stopped"}))
      }
      Err(error) => return Err(error),
    },
    CommandLine::PhysicalStop { command } => {
      physical_stop::run(&paths, &name, command)?;
    }
    CommandLine::Deployment { command } => match command {
      DeploymentCommand::Stop => {
        let result = manager_call(&paths, &name, "deployment_stop")?;
        println!("{}", serde_json::to_string_pretty(&result)?);
        if result["terminal"]["outcome"] == "unknown" {
          anyhow::bail!("dependent stop unconfirmed");
        }
      }
      DeploymentCommand::Status => println!(
        "{}",
        serde_json::to_string_pretty(&manager_call(&paths, &name, "deployment_status")?)?
      ),
    },
    CommandLine::Restart => {
      catch_interrupts()?;
      print_run_startup(
        &paths,
        &name,
        &manager_call(&paths, &name, "deployment_restart")?,
      )?;
      wait_for_interrupt(&paths, &name)?;
    }
    CommandLine::Status { json: _ } => {
      let mut status = if UnixStream::connect(paths.socket()).is_ok() {
        manager_call(&paths, &name, "deployment_status")?
      } else {
        physical_stop::offline_status(&paths, &name)
      };
      if status.get("run_id_state").is_none() {
        let (run_id, run_id_state) = log_store::LogStore::new(&paths, &name)
          .map(|store| store.run_identity())
          .unwrap_or((None, "unavailable"));
        status["run_id"] = json!(run_id);
        status["run_id_state"] = json!(run_id_state);
      }
      status["manager"] = if UnixStream::connect(paths.socket()).is_ok() {
        manager_call(&paths, &name, "manager_identity")?
      } else {
        json!({"state":"stopped"})
      };
      status["services"] = if UnixStream::connect(paths.socket()).is_ok() {
        service_call(&paths, &name, "services_status", None, None)?["services"].clone()
      } else {
        json!([])
      };
      println!("{}", serde_json::to_string_pretty(&status)?);
    }
    CommandLine::Logs {
      source,
      lines,
      level,
      current,
    } => {
      let store = log_store::LogStore::new(&paths, &name)?;
      if current {
        let active_revision = manager_call(&paths, &name, "deployment_status")
          .ok()
          .and_then(|status| status["active_revision"].as_str().map(str::to_owned));
        let page = store
          .read_current(log_store::CurrentQuery {
            source: &source,
            level: &level,
            node_path: None,
            correlation_id: None,
            cursor: None,
            limit: lines as usize,
            active_revision: active_revision.as_deref(),
          })
          .map_err(|error| anyhow::anyhow!("{}: {}", error.code, error.message))?;
        if let Some(anchor) = page["anchor"]["event"].as_object() {
          println!("{}", serde_json::to_string(anchor)?);
        }
        for entry in page["lines"].as_array().into_iter().flatten() {
          println!("{}", serde_json::to_string(entry)?);
        }
        for diagnostic in page["diagnostics"].as_array().into_iter().flatten() {
          eprintln!("{}", serde_json::to_string(diagnostic)?);
        }
        return Ok(());
      }
      for entry in store.tail_filtered(&source, lines as usize, Some(&level))? {
        println!(
          "{} {} [{}] {}",
          entry["timestamp"].as_str().unwrap_or(""),
          entry["level"].as_str().unwrap_or("").to_ascii_uppercase(),
          entry["source"].as_str().unwrap_or(""),
          entry["message"].as_str().unwrap_or("")
        );
      }
    }
    CommandLine::Doctor => {
      println!("IsOS {}", env!("CARGO_PKG_VERSION"));
      println!("graph: {}", paths.node_graph_path(&name).display());
      println!("data: {}", paths.data.display());
      println!("runtime: {}", paths.runtime.display());
      let graph = paths.node_graph_path(&name);
      match process::validate_full_graph(&graph)
        .and_then(|_| process::project_with_packages(&paths, &fs::read_to_string(&graph)?))
      {
        Ok(specs) => println!("graph and package: ready ({} processes)", specs.len()),
        Err(error) => println!("graph and package: {error}"),
      }
      println!(
        "manager: {}",
        if UnixStream::connect(paths.socket()).is_ok() {
          "running"
        } else {
          "stopped"
        }
      );
    }
    CommandLine::Config { command } => config_command(&paths, &name, command)?,
    CommandLine::Packages { command } => package_command(&paths, command)?,
    CommandLine::Skills { command } => library(&paths, &name, "skills", command)?,
    CommandLine::Services { command } => services_command(&paths, &name, command)?,
    CommandLine::Backends { command } => library(&paths, &name, "backends", command)?,
    CommandLine::Daemon => {
      catch_interrupts()?;
      manager_log::install(log_store::LogStore::new(&paths, &name)?)?;
      let listener = bind_daemon(&paths)?;
      daemon(&paths, &name, listener)?;
    }
  }
  Ok(())
}
