# Snapdragon Profiler Ecosystem

A collection of tools for working with [Snapdragon Profiler (SDP)](https://developer.qualcomm.com/software/snapdragon-profiler).

## Branches

**main**: Primary development branch. Contributors should develop submissions based on this branch, and submit pull requests to this branch.

## Available Tools

### Converter

Convert SDP exported CSV trace files into [Perfetto](https://ui.perfetto.dev/) compatible JSON format for visualization and analysis.
This converter supports CSV files exported from both SDP capture modes:

- **Realtime Capture Mode** — Streaming/live capture sessions where metrics are recorded continuously in real time
- **Trace Capture Mode** — Full trace sessions with detailed scheduling, slice, and counter data captured over a defined time window

#### Supported Event Types

| CSV Row Type | Perfetto Phase | Description |
|---|---|---|
| System counters | `"ph": "C"` | System-level metrics (CPU frequency, GPU metrics, DSP metrics) |
| Process counters | `"ph": "C"` | Per-process counter values (e.g., `aaRdy`, GPU Rendering Pipe Metrics) |
| Process slices | `"ph": "X"` | Duration events with start/end timestamps |
| System scheduling slices | `"ph": "X"` | Kernel scheduling events (e.g., Sched CPU) |
| Metadata | `"ph": "M"` | Auto-generated `process_name` and `thread_name` labels |

### RenderStage Splitter

Split GPU RenderStage events in a Perfetto `.pftrace` by process. The tool reads a Perfetto trace, extracts `GpuRenderStageEvent` packets, resolves each event to a process when possible, and writes an augmented `.pftrace` with separate RenderStage tracks per process for easier analysis in [Perfetto UI](https://ui.perfetto.dev/).

The tool also includes a debug mode that scans the input trace and prints packet, process, track, and RenderStage diagnostics.

### QDC WebSocket Tunnel (C++)

A zero-dependency C++ reverse WebSocket tunnel for connecting Snapdragon Profiler (SDP) to QDC-hosted Android and Qualcomm Linux (IoT) devices. Two small self-contained static binaries — no JRE, DEX bundle, or Gradle required on either side.

| Binary | Runs on | Role |
|--------|---------|------|
| `qdc-tunnel-target` | Android / Qualcomm Linux (aarch64) | WebSocket server + TCP multiplexer |
| `qdc-tunnel-host` | Windows / Linux host (x86-64 or ARM64) | WebSocket client + TCP forwarder |

Pre-built release packages (Windows ZIP and Linux tarball) are published to the [GitHub Releases](../../releases) page on each `v*` tag.

#### Quick Start

```bash
# Build C++ binaries for this machine
uv run qdc-tunnel-build

# Run the host tunnel (after building)
uv run qdc-tunnel-host --remote-host 127.0.0.1 --port-map 8900:6500 --port-map 8902:6502

# Run the loopback smoke test
uv run qdc-tunnel-test

# One-click connection launcher (Windows)
tools\websocket-tunnel-QDC\connect-qdc-sdp-launcher.bat
```

See [`tools/websocket-tunnel-QDC/README.md`](tools/websocket-tunnel-QDC/README.md) for the full connection guide and [`tools/websocket-tunnel-QDC/src/README.md`](tools/websocket-tunnel-QDC/src/README.md) for build details.

## Installation Instructions

```bash
# Install uv (if not already installed)
# Windows
powershell -ExecutionPolicy ByPass -c "irm https://astral.sh/uv/install.ps1 | iex"

# macOS / Linux
curl -LsSf https://astral.sh/uv/install.sh | sh

# Sync the project (creates virtual environment and installs dependencies)
uv sync
```

## Usage

All tools are accessible through the unified `main.py` entry point:

```bash
# Show available tools
uv run main.py --help

# Run a specific tool (e.g., converter)
uv run main.py converter <tool-specific arguments ...>

# Run the RenderStage splitter
uv run main.py split-renderstage <input.pftrace> <output.pftrace>
```

### Converter Examples

```bash
# Single file (output: trace1.json)
uv run main.py converter trace1.csv

# Single file with custom output name
uv run main.py converter trace1.csv -o custom_output.json

# Multiple files (each gets its own .json: trace1.json, trace2.json, trace3.json)
uv run main.py converter trace1.csv trace2.csv trace3.csv

# All CSV files in the current directory
uv run main.py converter *.csv

# Output to a specific directory
uv run main.py converter *.csv --output-dir ./output/
```

You can also invoke the converter directly (bypassing the dispatcher):

```bash
uv run python -m tools.converter.convert_sdp_csv_to_perfetto_json trace1.csv
```

### RenderStage Splitter Examples

```bash
# Split GPU RenderStage events into process-specific tracks
uv run main.py split-renderstage input.pftrace output.pftrace

# Inspect trace contents without writing an output file
uv run main.py split-renderstage --debug input.pftrace
```

You can also invoke the RenderStage splitter directly (bypassing the dispatcher):

```bash
uv run python -m tools.converter.split_renderstage_by_process input.pftrace output.pftrace

# Or use the uv-installed console script
uv run split-renderstage-by-process input.pftrace output.pftrace
```

## Development

To contribute new features or fixes, please see [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines on branching, submitting pull requests, and coding standards.

```bash
# Clone the repository
git clone https://github.com/SnapdragonGameStudios/Snapdragon_Profiler_Ecosystem.git
cd Snapdragon_Profiler_Ecosystem

# Sync the project (creates virtual environment and installs dev dependencies)
uv sync
```

## Testing

The project includes a comprehensive test suite using pytest.

```bash
# Run all tests
uv run pytest

# Run tests with verbose output
uv run pytest -v

# Run a specific test class
uv run pytest tests/test_convert_sdp_csv_to_perfetto_json.py::TestExampleCSV -v
```

## Getting in Contact

* [Report an Issue on GitHub](../../issues)
* [Open a Discussion on GitHub](../../discussions)

## License

*Snapdragon Profiler Ecosystem* is licensed under the [BSD-3-clause License](https://spdx.org/licenses/BSD-3-Clause.html). See [LICENSE.txt](LICENSE.txt) for the full license text.
