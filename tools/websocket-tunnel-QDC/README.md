# QDC WebSocket Tunnel (C++)

Native C++ reverse WebSocket tunnel that connects **Snapdragon Profiler (SDP)** to
QDC-hosted **Android** and **Qualcomm Linux (IoT)** devices through the QDC SSH gateway.
Two small self-contained static binaries — no JRE, Gradle, or Android SDK required.

| Binary | Runs on | Role |
|--------|---------|------|
| `qdc-tunnel-target` | Android / Qualcomm Linux (aarch64) | WebSocket server + TCP multiplexer |
| `qdc-tunnel-host` | Windows / Linux host (x86-64 or ARM64) | WebSocket client + TCP forwarder |

## Architecture

```
SDPCore ──TCP:6500──► qdc-tunnel-host ──WS:8900──► [ssh -L] ──► qdc-tunnel-target ──TCP:6500──► profiler clients
```

Both binaries multiplex concurrent TCP streams over a single WebSocket connection using a
length-prefixed MuxProtocol frame format (16-byte UUIDs per stream). Heartbeats every 500 ms
keep the QDC-forwarded stream alive across the gateway's idle-reset window.

## Pre-built Releases

Download the latest release package from the [GitHub Releases](../../releases) page.
Each package bundles host binaries for **both** x86-64 and ARM64 — the launcher
auto-selects the right binary for your machine:

| Package | Contents |
|---------|----------|
| `QDC-CPP-Tunnel-Windows-<tag>.zip` | Windows host (x86-64 + ARM64) + aarch64 device binary |
| `QDC-CPP-Tunnel-Linux-<tag>.tar.gz` | Linux host (x86-64 + ARM64) + aarch64 device binary |

## One-Click Launcher

Copy the config template, fill in your credentials, then run the launcher:

**Windows:**
```cmd
copy config.local.bat.template config.local.bat
rem  Edit config.local.bat — set QDC_API_KEY and SSH_KEY
connect-qdc-sdp-launcher.bat
```

**Linux / macOS:**
```bash
cp config.local.sh.template config.local.sh
# Edit config.local.sh — set QDC_API_KEY and SSH_KEY
chmod +x connect-qdc-sdp-launcher.sh
./connect-qdc-sdp-launcher.sh
```

### Configuration

| Variable | Required | Description |
|----------|----------|-------------|
| `QDC_API_KEY` | **Yes** | Your QDC API key for session discovery |
| `SSH_KEY` | **Yes** | Path to your SSH private key (e.g. `~/.ssh/id_ed25519`) |
| `SSH_USER_HOST` | No | SSH `user@host` for the QDC tunnel endpoint (auto-detected if not set) |
| `QDC_SESSIONS_URL` | No | QDC sessions API endpoint (built-in default) |

If `SSH_USER_HOST` is not set, the launcher auto-detects the correct endpoint by inspecting
DNS entries — on-network (`sshtunnel@ssh.qdc-internal.qualcomm.com`) vs.
off-network (`sshtunnel@ssh.qdc.qualcomm.com`).

### What the launcher does

1. Kills any stale ADB server and clears port `5037`.
2. Opens an SSH tunnel for ADB discovery (`-L 5037:<device>:5037`).
3. Runs `adb forward` for the WebSocket ports (`8900`, `8902`).
4. Pushes `qdc-tunnel-target` to the device (`/data/local/tmp/` or `/tmp/qdc-sdp/`).
5. Starts `qdc-tunnel-target` on the device with `--port-map 6500:8900 --port-map 6502:8902`.
6. Opens SSH tunnels for the WebSocket ports (`8900`, `8902`).
7. Starts `qdc-tunnel-host` on the host machine.

### Prerequisites (checked at runtime)

- `ssh`, `adb` (or `scp`/`ssh` for IoT targets)

## Build Locally

See [`src/README.md`](src/README.md) for full CMake instructions. Quick reference:

```bash
# Native Linux (host + device — also works for the smoke test)
cmake -S src -B src/build/native -DCMAKE_BUILD_TYPE=Release
cmake --build src/build/native

# Or via the Python wrapper:
uv run qdc-tunnel-build                    # native only
uv run qdc-tunnel-build --target-aarch64   # aarch64 musl cross-compile (device)
uv run qdc-tunnel-build --win              # Windows cross-compile
uv run qdc-tunnel-build --all              # all three
```

On Windows, cross-compiling the device binary requires WSL with CMake installed;
`uv run qdc-tunnel-build --target-aarch64` handles this automatically.

## Manual Run

```bash
# 1. Build and push device binary
adb push src/build/target/qdc-tunnel-target /data/local/tmp/qdc-tunnel-target
adb shell "chmod 755 /data/local/tmp/qdc-tunnel-target && \
           /data/local/tmp/qdc-tunnel-target --port-map 6500:8900 --port-map 6502:8902"

# 2. Set up port forwarding
adb forward tcp:8900 tcp:8900
adb forward tcp:8902 tcp:8902

# 3. Run host binary
./src/build/native/qdc-tunnel-host --remote-host 127.0.0.1 --port-map 8900:6500 --port-map 8902:6502
# Or via the Python wrapper:
uv run qdc-tunnel-host --remote-host 127.0.0.1 --port-map 8900:6500 --port-map 8902:6502
```

## Testing

```bash
# Build native binaries first, then:
python src/tools/loopback_smoke_test.py src/build/native/qdc-tunnel-target src/build/native/qdc-tunnel-host
# Or:
uv run qdc-tunnel-test
```

## CI/CD

The workflow `.github/workflows/build-qdc-cpp-tunnel.yml` builds five configurations on every
push/PR touching `tools/websocket-tunnel-QDC/src/`:

| Config | Output |
|--------|--------|
| `linux-native` | `qdc-tunnel-host` (x86-64) + `qdc-tunnel-target` (x86-64, for smoke test) |
| `aarch64-cross` | `qdc-tunnel-target` (aarch64 musl static — Android & Qualcomm Linux) |
| `linux-host-aarch64` | `qdc-tunnel-host` (ARM64 Linux, musl static) |
| `windows-cross` | `qdc-tunnel-host.exe` (Windows x86-64, via llvm-mingw) |
| `windows-cross-arm64` | `qdc-tunnel-host.exe` (Windows ARM64, via llvm-mingw) |

A smoke test runs a loopback echo through both native binaries, then the two fat release
archives are assembled and published to GitHub Releases on a `v*` tag push.

## Common Pitfalls

| Error | Cause | Fix |
|-------|-------|-----|
| `EACCES` on device | Binary not marked executable | `adb shell chmod 755 /data/local/tmp/qdc-tunnel-target` |
| SSH tunnel failed after 3 attempts | SSH key auth rejected, wrong device ID, or network unreachable | Check `SSH_KEY`, `SSH_USER_HOST`, device ID, and that the QDC session is running |
| `ERROR: Required tool "ssh" was not found` | Missing CLI prerequisite | Install OpenSSH / add `ssh` and `adb` to `PATH` |
