#!/usr/bin/env python3
"""Check that package service selection stays outside eosd and the demo wrapper."""

from __future__ import annotations

import importlib.machinery
import importlib.util
from pathlib import Path
import platform
import sys
import tempfile


def main() -> None:
    root = Path(sys.argv[1])
    loader = importlib.machinery.SourceFileLoader("island_launch_test", str(root / "bin/island-launch"))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    assert spec is not None
    launch = importlib.util.module_from_spec(spec)
    loader.exec_module(launch)
    with tempfile.TemporaryDirectory(prefix="eos-simulator-launch-") as temporary:
        bundle = Path(temporary)
        (bundle / "eos.toml").write_text(
            "[gateway]\nhost='127.0.0.1'\ngrpc_port=16003\n"
            "[[mode.binding]]\nbackend='mujoco.toml'\nprofile='benchmark'\n"
        )
        (bundle / "mujoco.toml").write_text("[profile.benchmark]\n")
        module = bundle / "ext/mujoco"
        module.mkdir(parents=True)
        release = bundle / "packages/installed/island/mujoco/1.0.0"
        release.mkdir(parents=True)
        (release / "release.json").write_text('{"id":"island/mujoco","version":"1.0.0","kind":"extension"}\n')
        (release / "simulator.bin").write_bytes(b"installed simulator")
        (release / "lib/island/skills").mkdir(parents=True)
        digest = launch.release_digest(release)
        lock = bundle / "packages/isos.lock"
        target = "linux-" + {"AMD64": "x86_64", "arm64": "aarch64"}.get(platform.machine(), platform.machine())
        lock_text = ("version=1\n[[package]]\nid='island/mujoco'\nversion='1.0.0'\n"
                     "kind='extension'\nsource='local:unknown'\n"
                     f"digest='{digest}'\ntarget='{target}'\ncontract='eos.simulator.v1'\n")
        lock.write_text(lock_text)
        (module / "module.toml").write_text(
            "version=1\nskill_directory='lib/island/skills'\n"
            "[module]\nid='mujoco'\nenabled=true\npackage='island/mujoco'\nversion='1.0.0'\n"
            "[[service]]\nname='simulator'\nrole='managed'\n"
            "command=['/package/eos-mujoco-sim','--socket','{run}/simulator.sock']\n"
            "ready_unix='{run}/simulator.sock'\n"
            "[[binding]]\nbackend='mujoco.toml'\nprofile='benchmark'\n"
            "requires=['simulator']\neos_after=['simulator']\n"
            "[[skill_capability]]\nskill='move'\nskill_version='1.0.0'\n"
            "slot='base'\nargument_pointer='/target/base'\nrobot_id='mujoco-0'\n"
            "device_class='base'\nprimitive='base.navigate'\nprimitive_version='1.0.0'\n"
        )
        (bundle / "stack.toml").write_text(
            "version=1\neos_config='eos.toml'\npackage_root='packages'\n"
            "modules=['ext/mujoco/module.toml']\n"
            "[[service]]\nname='eosd'\nrole='managed'\ncommand=['/bin/true']\n"
        )
        services = {item["name"]: item for item in launch.graph(str(bundle))}
        assert services["simulator"]["command"][0] == "/package/eos-mujoco-sim"
        assert services["simulator"]["role"] == "managed"
        assert "simulator" in services["eosd"]["after"]
        module_file = module / "module.toml"
        valid_module = module_file.read_text()
        module_file.write_text(valid_module.replace("/target/base", "/target/~2base"))
        try:
            launch.graph(str(bundle))
        except launch.LaunchError as error:
            assert "JSON pointer" in str(error)
        else:
            assert False, "invalid skill capability argument pointer was admitted"
        module_file.write_text(valid_module)
        module_file.write_text(valid_module.replace("lib/island/skills", "../outside"))
        try:
            launch.graph(str(bundle))
        except launch.LaunchError as error:
            assert "skill_directory" in str(error)
        else:
            assert False, "escaping skill directory was admitted"
        module_file.write_text(valid_module)
        (release / "simulator.bin").write_bytes(b"changed simulator")
        try:
            launch.graph(str(bundle))
        except launch.LaunchError as error:
            assert "digest differs" in str(error)
        else:
            assert False, "modified installed release was admitted"
        (release / "simulator.bin").write_bytes(b"installed simulator")
        lock.write_text("version=1\n")
        try:
            launch.graph(str(bundle))
        except launch.LaunchError as error:
            assert "not selected" in str(error)
        else:
            assert False, "unlocked module was admitted"
        lock.unlink()
        try:
            launch.graph(str(bundle))
        except launch.LaunchError as error:
            assert "cannot read" in str(error) and "isos.lock" in str(error)
        else:
            assert False, "module was admitted without isos.lock"
        lock.write_text(lock_text)
        with (bundle / "stack.toml").open("a") as stream:
            stream.write("[[service]]\nname='collision'\nrole='managed'\n"
                         "command=['/bin/true']\nready_unix='{run}/simulator.sock'\n")
        try:
            launch.graph(str(bundle))
        except launch.LaunchError as error:
            assert "share readiness endpoint" in str(error)
        else:
            assert False, "duplicate endpoint was admitted"
        (bundle / "stack.toml").write_text(
            "version=1\neos_config='eos.toml'\nmodules=[]\n"
            "[[service]]\nname='eosd'\nrole='managed'\ncommand=['/bin/true']\n")
        try:
            launch.graph(str(bundle))
        except launch.LaunchError as error:
            assert "no runtime declaration" in str(error)
        else:
            assert False, "MuJoCo binding started without its module declaration"


if __name__ == "__main__":
    main()
