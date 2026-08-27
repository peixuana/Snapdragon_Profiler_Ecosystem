# ============================================================================================================
#
#                  Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
#                              SPDX-License-Identifier: BSD-3-Clause
#
# ============================================================================================================
"""C++ QDC tunnel wrappers.

Subcommands (invoked via ``uv run <cmd> [args]``):

    qdc-tunnel-build        Build C++ binaries for the host platform and/or
                            cross-targets (aarch64-musl device, Windows .exe).
    qdc-tunnel-host         Run the compiled qdc-tunnel-host binary with the
                            same CLI it accepts natively.
    qdc-tunnel-test         Run the loopback smoke-test against the native
                            host+target binaries.

The wrapper locates binaries relative to this file:
    ../src/build/native/    native Linux build
    ../src/build/target/    aarch64-musl device binary
    ../src/build/win/       Windows cross-compiled host
    ../src/bin/             pre-built drop-in directory

Set HOST_BIN / TARGET_BIN env vars to override the search path.
"""

from __future__ import annotations

import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

_HERE = Path(__file__).resolve().parent
_CPP_DIR = _HERE / "websocket-tunnel-QDC" / "src"
_TUNNEL_DIR = _HERE / "websocket-tunnel-QDC"
_BUILD_NATIVE = _CPP_DIR / "build" / "native"
_BUILD_TARGET = _CPP_DIR / "build" / "target"
_BUILD_WIN    = _CPP_DIR / "build" / "win"
_BIN_DIR      = _CPP_DIR / "bin"
_SMOKE_TEST   = _CPP_DIR / "tools" / "loopback_smoke_test.py"
_CONNECT_BAT  = _TUNNEL_DIR / "connect-qdc-sdp-launcher.bat"


def _find_bin(name: str, candidates: list[Path]) -> Path | None:
    """Return the first path in *candidates* that is an executable file."""
    env_key = "HOST_BIN" if "host" in name else "TARGET_BIN"
    if env_override := os.environ.get(env_key):
        p = Path(env_override)
        if p.is_file():
            return p

    for p in candidates:
        if p.is_file() and os.access(p, os.X_OK):
            return p
    return None


def _host_bin() -> Path | None:
    on_windows = platform.system() == "Windows"
    if on_windows:
        candidates = [
            _BUILD_WIN / "qdc-tunnel-host.exe",
            _BUILD_WIN / "Release" / "qdc-tunnel-host.exe",
            _BUILD_WIN / "Debug" / "qdc-tunnel-host.exe",
            _BUILD_NATIVE / "qdc-tunnel-host.exe",
            _BUILD_NATIVE / "Release" / "qdc-tunnel-host.exe",
            _BUILD_NATIVE / "Debug" / "qdc-tunnel-host.exe",
            _BIN_DIR / "qdc-tunnel-host.exe",
        ]
    else:
        candidates = [
            _BUILD_NATIVE / "qdc-tunnel-host",
            _BIN_DIR / "qdc-tunnel-host",
        ]
    return _find_bin("host", candidates)


def _target_bin() -> Path | None:
    candidates = [
        _BUILD_NATIVE / "qdc-tunnel-target",
        _BUILD_TARGET / "qdc-tunnel-target",
        _BIN_DIR / "qdc-tunnel-target",
    ]
    return _find_bin("target", candidates)


# ---------------------------------------------------------------------------
# build subcommand
# ---------------------------------------------------------------------------

def _cmake_available() -> bool:
    return shutil.which("cmake") is not None


def _run_cmake(source: Path, build: Path, extra: list[str]) -> int:
    build.mkdir(parents=True, exist_ok=True)
    cfg = ["cmake", "-S", str(source), "-B", str(build),
           "-DCMAKE_BUILD_TYPE=Release"] + extra
    r = subprocess.run(cfg)
    if r.returncode != 0:
        return r.returncode
    return subprocess.run(["cmake", "--build", str(build)]).returncode


def _win_to_wsl(p: Path) -> str:
    """Convert a Windows absolute path to its /mnt/<drive>/... WSL equivalent."""
    drive = p.drive.rstrip(":")          # e.g. "C"
    rest  = p.as_posix()[len(p.drive):]  # e.g. "/Users/foo/..."
    return f"/mnt/{drive.lower()}{rest}"


def _build_target_via_wsl() -> int:
    """Cross-compile the aarch64-musl device binary inside WSL.

    Installs cmake and the musl.cc toolchain in WSL on first run (no root
    needed for the toolchain - it unpacks into ~/qdc-toolchains/).
    """
    wsl_src   = _win_to_wsl(_CPP_DIR)
    wsl_build = _win_to_wsl(_BUILD_TARGET)
    wsl_tc    = _win_to_wsl(_CPP_DIR / "toolchains" / "aarch64-linux-musl.cmake")
    musl_url  = "https://musl.cc/aarch64-linux-musl-cross.tgz"
    musl_root = "~/qdc-toolchains/aarch64-linux-musl-cross"

    # Single shell script that sets up tools the first time and then builds.
    script = f"""
set -e
if ! command -v cmake >/dev/null 2>&1; then
    echo "ERROR: cmake not found in WSL. Install it with: sudo apt-get install cmake" >&2
    exit 1
fi
if [ ! -x {musl_root}/bin/aarch64-linux-musl-g++ ]; then
    echo "==> Downloading aarch64-linux-musl toolchain (~50 MB)..."
    mkdir -p ~/qdc-toolchains
    curl -L --progress-bar -o /tmp/aarch64-linux-musl-cross.tgz {musl_url}
    echo "==> Extracting toolchain..."
    tar -xzf /tmp/aarch64-linux-musl-cross.tgz -C ~/qdc-toolchains
    rm /tmp/aarch64-linux-musl-cross.tgz
    echo "==> Toolchain ready at {musl_root}"
fi
cmake -S "{wsl_src}" -B "{wsl_build}" \\
      -DCMAKE_BUILD_TYPE=Release \\
      -DCMAKE_TOOLCHAIN_FILE="{wsl_tc}"
cmake --build "{wsl_build}"
"""
    return subprocess.run(["wsl", "bash", "-c", script]).returncode


def build_main() -> None:
    """Entry point for ``uv run qdc-tunnel-build``."""
    import argparse

    parser = argparse.ArgumentParser(
        prog="qdc-tunnel-build",
        description="Build C++ QDC tunnel binaries via CMake.",
    )
    parser.add_argument("--native", action="store_true", default=False,
                        help="Build native Linux host+target (default when no flags given)")
    parser.add_argument("--target-aarch64", action="store_true", default=False,
                        help="Cross-compile static aarch64-musl device binary")
    parser.add_argument("--win", action="store_true", default=False,
                        help="Cross-compile Windows host .exe via llvm-mingw")
    parser.add_argument("--all", action="store_true", default=False,
                        help="Build all three configurations")
    args = parser.parse_args()

    on_windows = platform.system() == "Windows"

    do_native  = args.native or args.all or not (args.target_aarch64 or args.win)
    do_target  = args.target_aarch64 or args.all
    do_win     = args.win or args.all

    rc = 0

    if do_native:
        if not _cmake_available():
            print("ERROR: cmake not found in PATH. Install CMake >= 3.13 and retry.", file=sys.stderr)
            sys.exit(1)
        print("==> Building native...")
        rc = rc or _run_cmake(_CPP_DIR, _BUILD_NATIVE, [])

    if do_target:
        if on_windows:
            print("==> Cross-compiling aarch64-musl device binary via WSL...")
            rc = rc or _build_target_via_wsl()
        else:
            if not _cmake_available():
                print("ERROR: cmake not found in PATH.", file=sys.stderr)
                sys.exit(1)
            tc = _CPP_DIR / "toolchains" / "aarch64-linux-musl.cmake"
            print("==> Cross-compiling aarch64-musl device binary...")
            rc = rc or _run_cmake(_CPP_DIR, _BUILD_TARGET,
                                  [f"-DCMAKE_TOOLCHAIN_FILE={tc}"])

    if do_win:
        if not _cmake_available():
            print("ERROR: cmake not found in PATH.", file=sys.stderr)
            sys.exit(1)
        tc = _CPP_DIR / "toolchains" / "x86_64-w64-mingw32.cmake"
        print("==> Cross-compiling Windows host binary...")
        rc = rc or _run_cmake(_CPP_DIR, _BUILD_WIN,
                              [f"-DCMAKE_TOOLCHAIN_FILE={tc}"])

    sys.exit(rc)


# ---------------------------------------------------------------------------
# host subcommand
# ---------------------------------------------------------------------------

def host_main() -> None:
    """Entry point for ``uv run qdc-tunnel-host``."""
    bin_path = _host_bin()
    if bin_path is None:
        print(
            "ERROR: qdc-tunnel-host binary not found.\n"
            "Build it first:  uv run qdc-tunnel-build\n"
            "Or set HOST_BIN=/path/to/qdc-tunnel-host",
            file=sys.stderr,
        )
        sys.exit(1)

    os.execv(str(bin_path), [str(bin_path)] + sys.argv[1:])


# ---------------------------------------------------------------------------
# test subcommand
# ---------------------------------------------------------------------------

def test_main() -> None:
    """Entry point for ``uv run qdc-tunnel-test``."""
    target = _target_bin()
    host   = _host_bin()

    missing = []
    if target is None:
        missing.append("qdc-tunnel-target - build with: uv run qdc-tunnel-build")
    if host is None:
        missing.append("qdc-tunnel-host - build with: uv run qdc-tunnel-build")
    if missing:
        print("ERROR: binaries not found:\n  " + "\n  ".join(missing), file=sys.stderr)
        sys.exit(1)

    rc = subprocess.run([sys.executable, str(_SMOKE_TEST), str(target), str(host)]).returncode
    sys.exit(rc)


# ---------------------------------------------------------------------------
# connect subcommand
# ---------------------------------------------------------------------------

def connect_main() -> None:
    """Entry point for ``uv run qdc-connect``.

    Resolves the host binary path (handling MSVC Debug/Release subdirs) and
    sets HOST_BIN before invoking connect-qdc-sdp-launcher.bat, so the bat finds
    the binary regardless of which CMake generator was used.
    """
    if platform.system() != "Windows":
        print("ERROR: qdc-connect is Windows-only (launches connect-qdc-sdp-launcher.bat).",
              file=sys.stderr)
        sys.exit(1)

    host = _host_bin()
    if host is None:
        print(
            "ERROR: qdc-tunnel-host.exe not found.\n"
            "Build it first:  uv run qdc-tunnel-build\n"
            "Or set HOST_BIN=/path/to/qdc-tunnel-host.exe",
            file=sys.stderr,
        )
        sys.exit(1)

    env = os.environ.copy()
    env.setdefault("HOST_BIN", str(host))

    rc = subprocess.run(["cmd", "/c", str(_CONNECT_BAT)] + sys.argv[1:], env=env).returncode
    sys.exit(rc)
