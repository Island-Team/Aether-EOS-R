fn main() {
  cxx_build::bridge("src/cxx_bridge.rs")
    .file("cxx/src/mock_hardware.cc")
    .include("cxx/include")
    .flag_if_supported("-std=c++17")
    .compile("aether_hal_cxx");
  println!("cargo:rerun-if-changed=src/cxx_bridge.rs");
  println!("cargo:rerun-if-changed=cxx/src/mock_hardware.cc");
  println!("cargo:rerun-if-changed=cxx/include/mock_hardware.h");
}
