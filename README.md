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
