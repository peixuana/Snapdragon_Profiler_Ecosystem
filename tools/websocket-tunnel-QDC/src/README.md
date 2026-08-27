# QDC Tunnel — C++ WebSocket Reverse Tunnel for QDC

Native C++ replacement for the Java WebSocket tunnel used to connect
Snapdragon Profiler (SDP) to QDC-hosted Android and Qualcomm Linux (IoT)
devices. A single small static ELF (~200 KB) replaces a multi-megabyte JVM/JRE
bundle, so device-side setup is one quick push with no runtime dependencies.

## Binaries

| Binary | Platform | Role |
|--------|----------|------|
| `qdc-tunnel-target` | Device (aarch64 Android or Qualcomm Linux) | WebSocket server + TCP multiplexer |
| `qdc-tunnel-host` | Host (Linux x86-64 or Windows x86-64) | WebSocket client + TCP forwarder |

## Data Path

```
SDPCore ──TCP:6500──► qdc-tunnel-host ──WS:8900──► [ssh -L] ──► qdc-tunnel-target ──TCP:6500──► profiler clients
```

Both binaries multiplex many concurrent TCP streams over a single WebSocket
connection using the MuxProtocol wire format (length-prefixed frames with 16-byte
UUIDs per stream). 500 ms heartbeats keep the QDC-forwarded WebSocket stream
alive across the gateway's ~1 s idle-reset window.

## Build

### Prerequisites

- CMake ≥ 3.13, Ninja (or any CMake generator)
- For aarch64 cross-compile: [musl.cc aarch64-linux-musl-cross toolchain](https://musl.cc/)
- For Windows cross-compile: [llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases)

Both toolchains are downloaded automatically by the CMake toolchain files if
`QDC_MUSL_ROOT` / `QDC_MINGW_ROOT` are unset and the default paths
(`~/qdc-toolchains/`) are empty.

### Native Linux (host + device binaries)

```bash
cmake -S . -B build/native -DCMAKE_BUILD_TYPE=Release
cmake --build build/native
# outputs: build/native/qdc-tunnel-target  build/native/qdc-tunnel-host
```

### aarch64-musl (static device binary — runs on Android & Qualcomm Linux)

```bash
cmake -S . -B build/target \
      -DCMAKE_TOOLCHAIN_FILE=toolchains/aarch64-linux-musl.cmake \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build/target
# output: build/target/qdc-tunnel-target
```

### Windows x86-64 (host binary via llvm-mingw cross-compile)

```bash
cmake -S . -B build/win \
      -DCMAKE_TOOLCHAIN_FILE=toolchains/x86_64-w64-mingw32.cmake \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build/win
# output: build/win/qdc-tunnel-host.exe
```

### Windows ARM64 (host binary via llvm-mingw cross-compile)

For Windows-on-ARM hosts (Snapdragon X / Copilot+ PCs). Uses the same
llvm-mingw toolchain, targeting the `aarch64-w64-mingw32` triple.

```bash
cmake -S . -B build/win-arm64 \
      -DCMAKE_TOOLCHAIN_FILE=toolchains/aarch64-w64-mingw32.cmake \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build/win-arm64
# output: build/win-arm64/qdc-tunnel-host.exe
```

### Linux ARM64 (static host binary via musl cross-compile)

For aarch64 Linux hosts (ARM64 workstations, Graviton/Ampere servers). Uses
the same musl cross-toolchain as the device build, but builds the host binary.

```bash
cmake -S . -B build/host-arm64 \
      -DCMAKE_TOOLCHAIN_FILE=toolchains/aarch64-linux-musl-host.cmake \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build/host-arm64
# output: build/host-arm64/qdc-tunnel-host
```

### Via Python wrapper (`uv run`)

```bash
uv run qdc-tunnel-build                # native only
uv run qdc-tunnel-build --target-aarch64   # aarch64 musl cross-compile
uv run qdc-tunnel-build --win              # Windows cross-compile
uv run qdc-tunnel-build --all              # all three
```

## Running

### Device side (`qdc-tunnel-target`)

```bash
# Deploy to Android
adb push build/target/qdc-tunnel-target /data/local/tmp/qdc-tunnel-target
adb shell "chmod 755 /data/local/tmp/qdc-tunnel-target"
adb shell "/data/local/tmp/qdc-tunnel-target --port-map 6500:8900 --port-map 6502:8902"

# Deploy to Qualcomm Linux (IoT) via SSH tunnel
scp -P 2222 build/target/qdc-tunnel-target root@localhost:/tmp/qdc-sdp/qdc-tunnel-target
ssh -p 2222 root@localhost "chmod 755 /tmp/qdc-sdp/qdc-tunnel-target && /tmp/qdc-sdp/qdc-tunnel-target --port-map 6500:8900 --port-map 6502:8902"
```

### Host side (`qdc-tunnel-host`)

```bash
# After adb forward tcp:8900 tcp:8900 or ssh -L 8900:...:8900:
./build/native/qdc-tunnel-host --remote-host 127.0.0.1 --port-map 8900:6500 --port-map 8902:6502
```

Or via Python wrapper:

```bash
uv run qdc-tunnel-host --remote-host 127.0.0.1 --port-map 8900:6500 --port-map 8902:6502
```

## One-Click Launchers

The parent directory (`tools/websocket-tunnel-QDC/`) contains full connection
launchers that automate all SSH, ADB/SCP, and tunnel steps:

- `connect-qdc-sdp-launcher.sh` — Linux / macOS
- `connect-qdc-sdp-launcher.bat` — Windows

Copy `config.local.sh.template` → `config.local.sh` (or `.bat`) and set your
`QDC_API_KEY` and `SSH_KEY` before running.

## Testing

```bash
# Build native binaries first, then:
python tools/loopback_smoke_test.py build/native/qdc-tunnel-target build/native/qdc-tunnel-host

# Or via uv:
uv run qdc-tunnel-test
```

The smoke test runs two scenarios:
1. 5 concurrent 1 KB echo streams
2. Single 4 MB round-trip echo

## Layout

```
src/
  CMakeLists.txt
  common/          # Shared: Logging, UuidUtil, MuxProtocol, WebSocket, NetCompat
  target/          # qdc-tunnel-target: POSIX poll() event loop, TunnelPort
  host/            # qdc-tunnel-host: cross-platform, HostTunnelPort
  toolchains/
    aarch64-linux-musl.cmake        # musl cross-compiler → device qdc-tunnel-target
    aarch64-linux-musl-host.cmake   # musl cross-compiler → ARM64 Linux host
    x86_64-w64-mingw32.cmake        # llvm-mingw → x86-64 Windows host
    aarch64-w64-mingw32.cmake       # llvm-mingw → ARM64 Windows host
  tools/
    loopback_smoke_test.py
```
