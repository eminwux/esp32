#!/usr/bin/env python3
"""Run pure protocol and real transport code against a fake timer/GPIO boundary."""
import configparser
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
compiler = shlex.split(os.environ.get("CXX", "c++"))
common = compiler + ["-std=c++11", "-Wall", "-Wextra", "-Werror", "-I", str(project / "include")]
config = configparser.ConfigParser(interpolation=None)
config.read(project / "platformio.ini")
with tempfile.TemporaryDirectory(prefix="async-host-") as directory:
    target = str(Path(directory) / "protocol")
    subprocess.run(common + [str(project / "test/protocol.cpp"), "-o", target], check=True)
    subprocess.run([target], check=True)
    for role in ("esp32_a", "esp32_b"):
        # Use the actual project flags so role/frequency/timing cannot drift
        # between this harness and PlatformIO's normal environments.
        flags = config[f"env:{role}"]["build_flags"].replace(
            "${env.build_flags}", config["env"]["build_flags"]
        )
        target = str(Path(directory) / role)
        subprocess.run(common + shlex.split(flags) + [
            "-I", str(project / "test/fakes"),
            str(project / "test/transport.cpp"), str(project / "src/bit_io.cpp"),
            "-o", target,
        ], check=True)
        subprocess.run([target], check=True)
