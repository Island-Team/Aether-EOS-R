fn main() -> Result<(), Box<dyn std::error::Error>> {
  // The checked-in Google API annotations are resolved from root; the
  // well-known types and compiler come from the pinned vendored protoc.
  let protoc = protoc_bin_vendored::protoc_bin_path()?;
  let include = protoc_bin_vendored::include_path()?;
  std::env::set_var("PROTOC", protoc);
  let descriptor = std::path::PathBuf::from(std::env::var("OUT_DIR")?).join("eos-descriptor.bin");
  let root = "../../share/eos/contracts";
  let protos = [
    "../../share/eos/contracts/eos/gateway.proto",
    "../../share/eos/contracts/eos/capabilities.proto",
    "../../share/eos/contracts/eos/skills.proto",
  ];
  for proto in &protos {
    println!("cargo:rerun-if-changed={proto}");
  }
  tonic_prost_build::configure()
    .file_descriptor_set_path(descriptor)
    .compile_protos(
      &protos,
      &[root, include.to_str().ok_or("non-UTF-8 protoc include")?],
    )?;
  Ok(())
}
