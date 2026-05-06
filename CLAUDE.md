# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Environment

- Python 3.13+ required. Dependencies managed by [`uv`](https://docs.astral.sh/uv/); `uv sync` creates `.venv` and installs dev deps (pytest).
- Runtime dependencies list is empty (stdlib only); pytest is the sole dev dependency.

## Common commands

```bash
uv sync                                   # create venv, install deps
uv run main.py --help                     # list registered tools
uv run main.py converter <csv> [-o out.json | --output-dir dir/]
uv run python -m tools.converter.convert_sdp_csv_to_perfetto_json <csv>   # bypass dispatcher

uv run pytest                             # full suite
uv run pytest -v
uv run pytest tests/test_convert_sdp_csv_to_perfetto_json.py::TestExampleCSV -v
```

Commits must be DCO-signed (`git commit -s`). External PRs are scanned by Semgrep in CI.

## Architecture

**Dispatcher pattern.** `main.py` is a thin CLI dispatcher. `TOOLS_REGISTRY` maps a tool name (e.g. `converter`) to a dotted module path; the dispatcher imports the module, rewrites `sys.argv` to hide its own name, then calls the module's `main()`. To add a new tool: create `tools/<name>/<module>.py` exposing `main()` that uses argparse, then add one line to `TOOLS_REGISTRY`. Each tool is independently invokable via `python -m ...` and is also registered as a console script in `pyproject.toml` (`[project.scripts]`).

**Converter (`tools/converter/convert_sdp_csv_to_perfetto_json.py`).** The SDP CSV → Perfetto JSON pipeline auto-detects the CSV variant by inspecting the header row:

- **Realtime** header has `Category`, `Metric`, `Timestamp` → `convert_realtime()`. Emits only counter events (`ph: "C"`). Synthesizes PIDs starting at `800000`, one per `(Process, Category)` pair.
- **Trace** header has `Group`, `Track`, `TimestampStart` → `convert_trace()`. Emits duration slices (`ph: "X"`) and counters (`ph: "C"`). Uses *real* PIDs from the CSV for `Process` rows and synthesizes PIDs starting at `900000` for `System` rows (one per `Group`). Trace-mode conversion does a two-pass read: pass 1 finds main-thread names (rows where `TID == PID`) to label processes; pass 2 emits events.

Counter vs slice disambiguation in trace mode is driven by the `Track` column: tracks containing `CounterValue` are explicit counters; rows with both `TimestampStart` and `TimestampEnd` are slices; rows with only one timestamp are treated as implicit counters.

Output always wraps events as `{"traceEvents": [...]}` with metadata events (`ph: "M"`, `process_name`/`thread_name`) prepended. `_write_perfetto()` is the single write path; `_metadata_events()` is the single metadata builder — reuse these when adding new event sources rather than open-coding JSON structure.

**Batch / glob handling.** `resolve_input_files()` expands glob patterns and deduplicates by normalized path. When multiple inputs are given, `-o` is ignored (warned) and each input is written next to itself (or into `--output-dir`) with the basename + `.json`.

## Tests

- Live in `tests/test_convert_sdp_csv_to_perfetto_json.py`. `test_main.py` is a backward-compat shim that re-exports the same tests so `pytest test_main.py` still works.
- `tests/__init__.py` and `tools/__init__.py` exist so tests can import `tools.converter...` directly — keep them.
