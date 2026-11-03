"""Exercise package target selection and atomic local installation."""

import hashlib
import importlib.machinery
import io
import json
import contextlib
from pathlib import Path
import platform
import tarfile
import tempfile
import tomllib
from unittest import mock


repo = Path(__file__).resolve().parents[2]
pkg = importlib.machinery.SourceFileLoader("island_pkg", str(repo / "bin/island-pkg")).load_module()


def manifest(size, digest):
  return {
    "schema_version": 1, "id": "island/hello-extension", "kind": "example", "version": "1.0.0",
    "description": "Hello extension", "source_url": "https://github.com/Island-Team/hello-extension",
    "release_url": "https://github.com/Island-Team/hello-extension/releases/tag/v1.0.0", "license": "MIT",
    "assets": [{"name": "hello.tar.gz", "url": "https://github.com/Island-Team/hello-extension/releases/download/v1.0.0/hello.tar.gz",
                "sha256": digest, "size_bytes": size, "os": "linux", "arch": "x86_64", "glibc_min": "2.35",
                "eos_contract": "", "required_libraries": [], "tested_ubuntu": ["22.04"]}],
    "config_inputs": [], "activation": None,
  }


def archive_bytes(name="bin/hello", data=b"#!/bin/sh\necho hello\n"):
  output = io.BytesIO()
  with tarfile.open(fileobj=output, mode="w:gz") as tar:
    entry = tarfile.TarInfo(name)
    entry.size = len(data)
    entry.mode = 0o755
    tar.addfile(entry, io.BytesIO(data))
  return output.getvalue()


def test_public_skill_download_is_not_activation():
  archive = archive_bytes("lib/shared.skill.so", b"native skill")
  release = {
    "schema_version": 3, "id": "island/shared-skill", "kind": "skill", "version": "1.0.0",
    "assets": [{"name": "shared.tar.gz", "url": "https://assets.os-island.com/shared.tar.gz",
                "sha256": hashlib.sha256(archive).hexdigest(), "size_bytes": len(archive),
                "os": "linux", "arch": {"AMD64": "x86_64", "arm64": "aarch64"}.get(platform.machine(), platform.machine()),
                "eos_contract": pkg.SKILL_CONTRACT, "required_libraries": []}],
  }
  with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    with mock.patch.object(pkg, "read_api", return_value=release) as api:
      selected = pkg.release(pkg.REGISTRY, "island/shared-skill")
    assert api.call_args.args[1].endswith("/island/shared-skill/releases/latest")
    with mock.patch.object(pkg.urllib.request, "urlopen", return_value=io.BytesIO(archive)):
      pkg.install(selected, root)
    skill = root / "installed/island/shared-skill/1.0.0/lib/shared.skill.so"
    assert skill.is_file() and not skill.is_symlink() and skill.read_bytes() == b"native skill"
    assert not (root / "skills/current").exists()
    assert not (root / "isos.lock").exists()


def test_public_catalog_rejects_private_backend():
  release = {"schema_version": 3, "id": "private/driver", "kind": "backend", "version": "1.0.0",
             "source_build": {"system": "cmake"}}
  with mock.patch.object(pkg, "read_api", return_value=release):
    try:
      pkg.release(pkg.REGISTRY, "private/driver")
    except pkg.PackageError as error:
      assert "private" in str(error)
    else:
      assert False, "public backend release was accepted"


def test_source_manifest_does_not_skip_asset_validation():
  release = {"schema_version": 3, "id": "private/driver", "kind": "backend", "version": "1.0.0",
             "source_build": {"system": "cmake"}, "assets": [{"name": "driver.tar.gz", "sha256": "bad"}]}
  try:
    pkg.validate_manifest(release)
  except pkg.PackageError as error:
    assert "SHA-256" in str(error)
  else:
    assert False, "unverified asset was accepted beside a source build"


def test_search_hides_private_backends():
  data = {"items": [
    {"id": "island/shared-skill", "version": "1.0.0", "kind": "skill", "description": "Shared"},
    {"id": "private/driver", "version": "1.0.0", "kind": "backend", "description": "Private"},
  ]}
  output = io.StringIO()
  with mock.patch.object(pkg, "read_api", return_value=data), mock.patch.object(pkg.sys, "argv", ["island-pkg", "search", "skill"]), mock.patch.dict(pkg.os.environ, {"ISLAND_PKG_LEGACY": "1"}):
    with contextlib.redirect_stdout(output):
      assert pkg.main() == 0
  assert "island/shared-skill" in output.getvalue()
  assert "private/driver" not in output.getvalue()


def test_install_example_needs_no_activation():
  archive = archive_bytes()
  release = manifest(len(archive), hashlib.sha256(archive).hexdigest())
  with tempfile.TemporaryDirectory() as tmp:
    with mock.patch.object(pkg, "compatibility", return_value=[]), mock.patch.object(pkg.urllib.request, "urlopen", return_value=io.BytesIO(archive)):
      pkg.install(release, Path(tmp))
    path, installed = pkg.installed(Path(tmp), "hello-extension")
    assert installed["version"] == "1.0.0"
    assert (path / "bin/hello").is_file()
    assert pkg.activate(Path(tmp), "hello-extension", None, {})["bundle"] is None


def test_digest_failure_leaves_no_install():
  archive = archive_bytes()
  release = manifest(len(archive), "a" * 64)
  with tempfile.TemporaryDirectory() as tmp:
    with mock.patch.object(pkg, "compatibility", return_value=[]), mock.patch.object(pkg.urllib.request, "urlopen", return_value=io.BytesIO(archive)):
      try:
        pkg.install(release, Path(tmp))
      except pkg.PackageError as error:
        assert "SHA-256" in str(error)
      else:
        assert False, "checksum mismatch was accepted"
    assert not list(Path(tmp).glob("installed/*/*/*/release.json"))


def test_download_accepts_approved_https_asset_on_another_host():
  archive = archive_bytes()
  asset = manifest(len(archive), hashlib.sha256(archive).hexdigest())["assets"][0]
  asset["url"] = "https://assets.os-island.com/packages/hello.tar.gz"
  with tempfile.TemporaryDirectory() as tmp:
    target = Path(tmp) / "hello.tar.gz"
    with mock.patch.object(pkg.urllib.request, "urlopen", return_value=io.BytesIO(archive)) as get:
      pkg.download(asset, target)
    get.assert_called_once_with(asset["url"], timeout=60)
    assert target.read_bytes() == archive


def test_download_rejects_non_https_asset():
  archive = archive_bytes()
  asset = manifest(len(archive), hashlib.sha256(archive).hexdigest())["assets"][0]
  asset["url"] = "http://assets.os-island.com/packages/hello.tar.gz"
  with tempfile.TemporaryDirectory() as tmp:
    with mock.patch.object(pkg.urllib.request, "urlopen") as get:
      try:
        pkg.download(asset, Path(tmp) / "hello.tar.gz")
      except pkg.PackageError as error:
        assert "HTTPS" in str(error)
      else:
        assert False, "non-HTTPS asset was accepted"
      get.assert_not_called()


def test_interrupted_download_leaves_no_install():
  archive = archive_bytes()
  release = manifest(len(archive), hashlib.sha256(archive).hexdigest())

  class InterruptedStream(io.BytesIO):
    def read(self, size=-1):
      if self.tell():
        raise OSError("connection interrupted")
      return super().read(8)

  with tempfile.TemporaryDirectory() as tmp:
    with mock.patch.object(pkg, "compatibility", return_value=[]), mock.patch.object(pkg.urllib.request, "urlopen", return_value=InterruptedStream(archive)):
      try:
        pkg.install(release, Path(tmp))
      except OSError as error:
        assert "interrupted" in str(error)
      else:
        assert False, "interrupted download was accepted"
    assert not list(Path(tmp).glob("installed/*/*/*/release.json"))


def test_archive_rejects_path_escape():
  archive = archive_bytes("../outside")
  with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "asset.tar.gz"
    source.write_bytes(archive)
    try:
      pkg.unpack(source, Path(tmp) / "files")
    except pkg.PackageError as error:
      assert "unsafe archive" in str(error)
    else:
      assert False, "path escape was accepted"


def test_package_paths_reject_ambiguous_or_non_string_components():
  for value in ("a//b", "a/./b", "a/../b", "a/", 42, None):
    assert not pkg.safe_relative(value)
  assert pkg.safe_relative("config/module.toml")


def test_unsupported_target_explains_source_build():
  release = manifest(1, "a" * 64)
  release["assets"][0]["arch"] = "aarch64"
  with mock.patch.object(pkg.platform, "machine", return_value="x86_64"):
    try:
      pkg.choose_asset(release)
    except pkg.PackageError as error:
      assert "requires aarch64" in str(error)
      assert release["source_url"] in str(error)
    else:
      assert False, "unsupported target was accepted"


def test_legacy_activation_requires_migration():
  archive = archive_bytes()
  release = manifest(len(archive), hashlib.sha256(archive).hexdigest())
  release["kind"] = "extension"
  release["assets"][0]["eos_contract"] = "eos.simulator.v1"
  release["activation"] = {"entrypoint": "bin/hello", "scene": "assets/scene.xml",
                           "eos_template": "config/eos.toml", "simulator_template": "config/sim.toml",
                           "backend_template": "config/backends/mujoco.toml"}
  with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    source = root / "installed/island/hello-extension/1.0.0"
    (source / "bin").mkdir(parents=True)
    (source / "bin/hello").write_text("#!/bin/sh\n")
    (source / "bin/hello").chmod(0o755)
    (source / "assets").mkdir()
    (source / "assets/scene.xml").write_text("<xml/>")
    (source / "config/backends").mkdir(parents=True)
    (source / "config/eos.toml").write_text('executable = {{package_executable}}\n')
    (source / "config/sim.toml").write_text('scene = {{package_scene}}\n')
    (source / "config/backends/mujoco.toml").write_text('root = {{package_root}}\n')
    (source / "release.json").write_text(json.dumps(release))
    try:
      pkg.activate(root, "hello-extension", None, {})
    except pkg.PackageError as error:
      assert "legacy activation" in str(error)
    else:
      assert False, "legacy layout was accepted"


def test_v2_activation_requires_migration():
  release = manifest(1, "a" * 64)
  release["schema_version"] = 2
  release["kind"] = "extension"
  release["activation"] = {"entrypoint": "bin/hello", "scene": "assets/scene.xml",
                           "eos_template": "config/eos.toml", "simulator_template": "config/sim.toml",
                           "backend_template": "config/backends/mujoco.toml",
                           "launch_template": "config/launch.toml"}
  with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    source = root / "installed/island/hello-extension/1.0.0"
    for directory in ("bin", "assets", "config/backends"):
      (source / directory).mkdir(parents=True, exist_ok=True)
    (source / "bin/hello").write_text("#!/bin/sh\n")
    (source / "bin/hello").chmod(0o755)
    (source / "assets/scene.xml").write_text("<xml/>")
    for template in ("config/eos.toml", "config/sim.toml", "config/backends/mujoco.toml"):
      (source / template).write_text('path = {{package_root}}\n')
    (source / "config/launch.toml").write_text(
      'version = 1\n[[service]]\nname = "simulator"\nrole = "managed"\n'
      'command = [{{package_executable}}, "--socket", "{run}/simulator.sock"]\n'
      'ready_unix = "{run}/simulator.sock"\n'
      '[[service]]\nname = "vendor-driver"\nrole = "driver"\n'
      'command = [{{package_executable}}]\n')
    (source / "release.json").write_text(json.dumps(release))
    try:
      pkg.activate(root, "hello-extension", None, {})
    except pkg.PackageError as error:
      assert "legacy activation" in str(error)
    else:
      assert False, "legacy layout was accepted"


def test_native_backend_build_and_activation():
  with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    source = root / "source"
    source.mkdir()
    cmake_source = root / "eos-cmake"
    cmake_source.mkdir()
    (cmake_source / "CMakeLists.txt").write_text("cmake_minimum_required(VERSION 3.22)\n")
    (source / "config").mkdir()
    (source / "config/module.toml").write_text(
      'version = 1\n[module]\nid = "private--device-backend"\nenabled = true\n'
      'package = "private/device-backend"\nversion = "1.0.0"\n'
      '[[service]]\nname = "driver"\nrole = "driver"\n'
      'command = [{{package_file:bin/driver}}]\n')
    (source / "island-package.toml").write_text(
      'schema_version = 3\nid = "private/device-backend"\nkind = "backend"\nversion = "1.0.0"\n'
      '[source_build]\nsystem = "cmake"\ntarget = "device_bridge"\n'
      'install_component = "device-runtime"\nresources = ["config/module.toml"]\n'
      '[source_build.cmake_options]\nWITH_ROS2 = false\n'
      '[[activation.files]]\nsource = "config/module.toml"\ndestination = "module.toml"\n')
    def run(command, check):
      assert check
      if command[1] == "--install":
        assert command[-2:] == ["--component", "device-runtime"]
        stage = next(Path(part.split("=", 1)[1]) for part in configure[0] if part.startswith("-DCMAKE_INSTALL_PREFIX="))
        (stage / "bin").mkdir()
        (stage / "bin/driver").write_text("driver")
      elif command[1] == "-S":
        configure.append(command)
      elif command[1] == "--build":
        assert command[-2:] == ["--target", "device_bridge"]
      return mock.Mock(returncode=0)
    configure = []
    with mock.patch.object(pkg.subprocess, "run", side_effect=run):
      destination = pkg.build_source(source, root / "packages", ["WITH_ROS2=ON"], cmake_source)
    assert (destination / "bin/driver").is_file()
    assert configure[0][2] == str(cmake_source)
    assert "-DWITH_ROS2=ON" in configure[0]
    result = pkg.activate(root / "packages", "private/device-backend", None, {})
    assert str(destination) in Path(result["module"]).read_text()
    assert Path(result["module"]).parts[-4:] == ("etc", "ext", "private--device-backend", "module.toml")


def test_native_skill_build_does_not_populate_loader_files():
  with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    source = root / "source"
    source.mkdir()
    (source / "CMakeLists.txt").write_text("cmake_minimum_required(VERSION 3.22)\n")
    (source / "island-package.toml").write_text(
      'schema_version = 3\nid = "private/grasp"\nkind = "skill"\nversion = "1.0.0"\n'
      '[source_build]\nsystem = "cmake"\n')
    def run(command, check):
      if command[1] == "-S":
        configure.append(command)
      elif command[1] == "--install":
        stage = next(Path(part.split("=", 1)[1]) for part in configure[0] if part.startswith("-DCMAKE_INSTALL_PREFIX="))
        (stage / "lib").mkdir()
        (stage / "lib/grasp.skill.so").write_bytes(b"native")
      return mock.Mock(returncode=0)
    configure = []
    with mock.patch.object(pkg.subprocess, "run", side_effect=run):
      pkg.build_source(source, root / "packages", [])
    installed_skill = root / "packages/installed/private/grasp/1.0.0/lib/grasp.skill.so"
    assert installed_skill.is_file() and not installed_skill.is_symlink() and installed_skill.read_bytes() == b"native"
    assert not (root / "packages/skills/current").exists()


def test_failed_native_build_is_atomic():
  with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    source = root / "source"
    source.mkdir()
    (source / "CMakeLists.txt").write_text("cmake_minimum_required(VERSION 3.22)\n")
    (source / "island-package.toml").write_text(
      'schema_version = 3\nid = "private/skill"\nkind = "skill"\nversion = "1.0.0"\n'
      '[source_build]\nsystem = "cmake"\n')
    with mock.patch.object(pkg.subprocess, "run", side_effect=__import__("subprocess").CalledProcessError(1, "cmake")):
      try:
        pkg.build_source(source, root / "packages", [])
      except __import__("subprocess").CalledProcessError:
        pass
      else:
        assert False, "failed native build was accepted"
    assert not list((root / "packages").glob("installed/*/*/*/release.json"))


def test_lock_selection_is_deterministic_and_detects_tampering():
  archive = archive_bytes()
  release = manifest(len(archive), hashlib.sha256(archive).hexdigest())
  with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    with mock.patch.object(pkg, "compatibility", return_value=[]), mock.patch.object(pkg.urllib.request, "urlopen", return_value=io.BytesIO(archive)):
      pkg.install(release, root)
    assert not (root / "isos.lock").exists()
    first = pkg.select_release(root, "hello-extension")
    lock_contents = (root / "isos.lock").read_text()
    pkg.select_release(root, "island/hello-extension", "1.0.0")
    assert (root / "isos.lock").read_text() == lock_contents
    assert tomllib.loads(lock_contents)["package"] == [first]
    pkg.verify_lock(root)
    (root / "installed/island/hello-extension/1.0.0/bin/hello").write_text("tampered")
    try:
      pkg.verify_lock(root)
    except pkg.PackageError as error:
      assert "lock mismatch" in str(error)
    else:
      assert False, "tampered release was accepted"


def test_remove_rejects_locked_release():
  archive = archive_bytes()
  release = manifest(len(archive), hashlib.sha256(archive).hexdigest())
  with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    with mock.patch.object(pkg, "compatibility", return_value=[]), mock.patch.object(pkg.urllib.request, "urlopen", return_value=io.BytesIO(archive)):
      pkg.install(release, root)
    pkg.select_release(root, "hello-extension")
    with mock.patch.object(pkg.sys, "argv", ["island-pkg", "--root", str(root), "remove", "hello-extension"]):
      assert pkg.main() == 1
    assert (root / "installed/island/hello-extension/1.0.0/release.json").is_file()
    pkg.deselect_release(root, "hello-extension")
    with mock.patch.object(pkg.sys, "argv", ["island-pkg", "--root", str(root), "remove", "hello-extension"]):
      assert pkg.main() == 0


if __name__ == "__main__":
  for name, test in sorted(globals().items()):
    if name.startswith("test_") and callable(test):
      test()
      print(f"PASS {name}")
