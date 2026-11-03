use std::{env, fs, os::unix::fs::MetadataExt, time::UNIX_EPOCH};

fn main() {
  let path = env::args().nth(1).expect("segment path");
  let meta = fs::metadata(path).expect("segment metadata");
  let birth = meta
    .created()
    .ok()
    .and_then(|time| time.duration_since(UNIX_EPOCH).ok())
    .map_or(0, |duration| duration.as_nanos());
  println!(
    "{} {} {} {:x}-{:x}-{birth:x}",
    meta.dev(),
    meta.ino(),
    birth,
    meta.dev(),
    meta.ino()
  );
}
