mod app;
mod domain;
mod interfaces;
mod runtime;

fn main() {
  if let Err(error) = app::entry::run() {
    eprintln!("isos: {error:#}");
    std::process::exit(1);
  }
}
