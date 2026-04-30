# ============================================================================================================
# 
#                  Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
#                              SPDX-License-Identifier: BSD-3-Clause
# 
# ============================================================================================================ 

"""
Unit tests for SDP CSV to Perfetto JSON converter.

Tests cover:
- Helper function correctness
- JSON structure and schema validation
- Data quality checks (types, ranges, completeness)
- End-to-end conversion with the example CSV
"""

import csv
import json
import os
import tempfile

import pytest

from tools.converter.convert_sdp_csv_to_perfetto_json import (
    convert,
    derive_output_path,
    extract_counter_name,
    extract_process_name_from_group,
    extract_thread_name_from_track,
    extract_tid_from_track,
    resolve_input_files,
)


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

@pytest.fixture
def sample_csv(tmp_path):
    """Create a minimal SDP CSV file for testing."""
    csv_path = tmp_path / "sample.csv"
    rows = [
        {
            "Process": "System",
            "Group": "CPU Metrics",
            "Process ID": "",
            "Thread ID": "",
            "Track": "CPU 0 Frequency",
            "Block Name": "",
            "TimestampStart": "1000",
            "TimestampEnd": "",
            "Value": "1800",
        },
        {
            "Process": "System",
            "Group": "GPU Metrics",
            "Process ID": "",
            "Thread ID": "",
            "Track": "GPU Frequency",
            "Block Name": "",
            "TimestampStart": "1000",
            "TimestampEnd": "",
            "Value": "500",
        },
        {
            "Process": "Process",
            "Group": "Trace myapp-1234",
            "Process ID": "1234",
            "Thread ID": "",
            "Track": "myapp-1234",
            "Block Name": "doWork",
            "TimestampStart": "2000",
            "TimestampEnd": "3000",
            "Value": "",
        },
        {
            "Process": "Process",
            "Group": "Trace myapp-1234",
            "Process ID": "1234",
            "Thread ID": "",
            "Track": "worker-1235",
            "Block Name": "processData",
            "TimestampStart": "2500",
            "TimestampEnd": "2800",
            "Value": "",
        },
        {
            "Process": "Process",
            "Group": "Trace myapp-1234",
            "Process ID": "1234",
            "Thread ID": "",
            "Track": "aaRdy - CounterValue:",
            "Block Name": "",
            "TimestampStart": "2000",
            "TimestampEnd": "",
            "Value": "42",
        },
        # Process-level counter WITHOUT 'CounterValue' in Track name
        # (e.g., GPU Rendering Pipe Metrics like "Avg Bytes / Fragment")
        {
            "Process": "Process",
            "Group": "GPU Rendering Pipe Metrics",
            "Process ID": "1234",
            "Thread ID": "",
            "Track": "Avg Bytes / Fragment",
            "Block Name": "",
            "TimestampStart": "3000",
            "TimestampEnd": "",
            "Value": "128.5",
        },
        {
            "Process": "Process",
            "Group": "GPU Rendering Pipe Metrics",
            "Process ID": "1234",
            "Thread ID": "",
            "Track": "% LRZ Busy",
            "Block Name": "",
            "TimestampStart": "3100",
            "TimestampEnd": "",
            "Value": "0",
        },
        # System scheduling slice (e.g., Trace Kernel - Sched CPU)
        {
            "Process": "System",
            "Group": "Trace Kernel - Sched CPU",
            "Process ID": "",
            "Thread ID": "",
            "Track": "Sched CPU 0",
            "Block Name": "APM-TickThread",
            "TimestampStart": "7699",
            "TimestampEnd": "9560",
            "Value": "",
        },
        {
            "Process": "System",
            "Group": "Trace Kernel - Sched CPU",
            "Process ID": "",
            "Thread ID": "",
            "Track": "Sched CPU 4",
            "Block Name": "GameThread",
            "TimestampStart": "10000",
            "TimestampEnd": "12000",
            "Value": "",
        },
    ]
    fieldnames = [
        "Process", "Group", "Process ID", "Thread ID",
        "Track", "Block Name", "TimestampStart", "TimestampEnd", "Value",
    ]
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)
    return csv_path


@pytest.fixture
def converted_json(sample_csv, tmp_path):
    """Convert sample CSV and return parsed JSON data + output path."""
    output_path = tmp_path / "output.json"
    stats = convert(str(sample_csv), str(output_path))
    assert stats is not None, "Conversion returned None (file not found?)"
    with open(output_path, "r", encoding="utf-8") as f:
        data = json.load(f)
    return data, stats


@pytest.fixture
def example_csv_json(tmp_path):
    """Convert the real trace1.csv from input/ if it exists."""
    csv_path = os.path.join(os.path.dirname(__file__), "..", "input", "trace1.csv")
    csv_path = os.path.normpath(csv_path)
    if not os.path.exists(csv_path):
        pytest.skip("input/trace1.csv not found")
    output_path = tmp_path / "example_output.json"
    stats = convert(csv_path, str(output_path))
    assert stats is not None
    with open(output_path, "r", encoding="utf-8") as f:
        data = json.load(f)
    return data, stats


# ===========================================================================
# Helper Function Tests
# ===========================================================================

class TestExtractTidFromTrack:
    def test_basic(self):
        assert extract_tid_from_track("binder:30114_3-30127") == 30127

    def test_simple_name(self):
        assert extract_tid_from_track("surfaceflinger-2109") == 2109

    def test_dotted_name(self):
        assert extract_tid_from_track("cent.tmgp.sgame-30114") == 30114

    def test_no_tid(self):
        assert extract_tid_from_track("CPU 0 Frequency") is None

    def test_empty(self):
        assert extract_tid_from_track("") is None

    def test_counter_value_track(self):
        assert extract_tid_from_track("aaRdy - CounterValue:") is None


class TestExtractThreadNameFromTrack:
    def test_binder(self):
        assert extract_thread_name_from_track("binder:30114_3-30127") == "binder:30114_3"

    def test_simple(self):
        assert extract_thread_name_from_track("surfaceflinger-2109") == "surfaceflinger"

    def test_dotted(self):
        assert extract_thread_name_from_track("cent.tmgp.sgame-30114") == "cent.tmgp.sgame"

    def test_no_tid(self):
        assert extract_thread_name_from_track("CPU 0 Frequency") == "CPU 0 Frequency"

    def test_empty(self):
        assert extract_thread_name_from_track("") == ""


class TestExtractCounterName:
    def test_basic(self):
        assert extract_counter_name("aaRdy - CounterValue:") == "aaRdy"

    def test_with_spaces(self):
        assert extract_counter_name("some counter - CounterValue:") == "some counter"

    def test_no_counter(self):
        assert extract_counter_name("surfaceflinger-2109") == "surfaceflinger-2109"

    def test_empty(self):
        assert extract_counter_name("") == ""


class TestExtractProcessNameFromGroup:
    def test_trace_prefix(self):
        assert extract_process_name_from_group("Trace surfaceflinger-2109") == "surfaceflinger-2109"

    def test_no_trace_prefix(self):
        assert extract_process_name_from_group("CPU Metrics") == "CPU Metrics"

    def test_empty(self):
        assert extract_process_name_from_group("") == ""


# ===========================================================================
# JSON Structure Tests
# ===========================================================================

class TestJsonStructure:
    def test_has_trace_events_wrapper(self, converted_json):
        data, _ = converted_json
        assert "traceEvents" in data
        assert isinstance(data["traceEvents"], list)

    def test_events_not_empty(self, converted_json):
        data, _ = converted_json
        assert len(data["traceEvents"]) > 0

    def test_all_events_have_required_fields(self, converted_json):
        data, _ = converted_json
        required_fields = {"name", "ph", "ts", "pid", "tid"}
        for i, event in enumerate(data["traceEvents"]):
            missing = required_fields - set(event.keys())
            assert not missing, f"Event {i} missing fields: {missing}. Event: {event}"


# ===========================================================================
# Event Type Tests
# ===========================================================================

class TestEventTypes:
    def test_slice_events_have_dur(self, converted_json):
        data, _ = converted_json
        slices = [e for e in data["traceEvents"] if e["ph"] == "X"]
        assert len(slices) > 0, "No slice events found"
        for i, event in enumerate(slices):
            assert "dur" in event, f"Slice event {i} missing 'dur': {event}"
            assert event["dur"] >= 0, f"Slice event {i} has negative duration: {event['dur']}"

    def test_counter_events_have_args(self, converted_json):
        data, _ = converted_json
        counters = [e for e in data["traceEvents"] if e["ph"] == "C"]
        assert len(counters) > 0, "No counter events found"
        for i, event in enumerate(counters):
            assert "args" in event, f"Counter event {i} missing 'args': {event}"
            assert isinstance(event["args"], dict), f"Counter event {i} 'args' is not dict"
            for k, v in event["args"].items():
                assert isinstance(v, (int, float)), (
                    f"Counter event {i} arg '{k}' is not numeric: {v}"
                )

    def test_metadata_events_have_name_arg(self, converted_json):
        data, _ = converted_json
        metadata = [e for e in data["traceEvents"] if e["ph"] == "M"]
        assert len(metadata) > 0, "No metadata events found"
        for i, event in enumerate(metadata):
            assert "args" in event, f"Metadata event {i} missing 'args': {event}"
            assert "name" in event["args"], (
                f"Metadata event {i} missing 'args.name': {event}"
            )

    def test_only_valid_phases(self, converted_json):
        data, _ = converted_json
        valid_phases = {"X", "C", "M"}
        for i, event in enumerate(data["traceEvents"]):
            assert event["ph"] in valid_phases, (
                f"Event {i} has unexpected phase '{event['ph']}'"
            )


# ===========================================================================
# Data Quality Tests
# ===========================================================================

class TestDataQuality:
    def test_pids_are_integers(self, converted_json):
        data, _ = converted_json
        for i, event in enumerate(data["traceEvents"]):
            assert isinstance(event["pid"], int), (
                f"Event {i} pid is not int: {type(event['pid'])}"
            )

    def test_tids_are_integers(self, converted_json):
        data, _ = converted_json
        for i, event in enumerate(data["traceEvents"]):
            assert isinstance(event["tid"], int), (
                f"Event {i} tid is not int: {type(event['tid'])}"
            )

    def test_timestamps_are_non_negative(self, converted_json):
        data, _ = converted_json
        for i, event in enumerate(data["traceEvents"]):
            assert isinstance(event["ts"], int), (
                f"Event {i} ts is not int: {type(event['ts'])}"
            )
            assert event["ts"] >= 0, f"Event {i} has negative timestamp: {event['ts']}"

    def test_no_zero_duration_slices(self, converted_json):
        """Slices with zero duration are technically valid but may indicate issues."""
        data, _ = converted_json
        slices = [e for e in data["traceEvents"] if e["ph"] == "X"]
        zero_dur = [e for e in slices if e["dur"] == 0]
        # Warn but don't fail — zero duration can be valid
        if zero_dur:
            print(f"Warning: {len(zero_dur)} slice(s) with zero duration")

    def test_no_negative_duration_slices(self, converted_json):
        data, _ = converted_json
        slices = [e for e in data["traceEvents"] if e["ph"] == "X"]
        for i, event in enumerate(slices):
            assert event["dur"] >= 0, (
                f"Slice {i} has negative duration: {event['dur']}"
            )


# ===========================================================================
# Metadata Completeness Tests
# ===========================================================================

class TestMetadataCompleteness:
    def test_process_metadata_covers_all_pids(self, converted_json):
        """Every PID used in trace events should have a process_name metadata entry."""
        data, _ = converted_json
        events = data["traceEvents"]

        trace_pids = {e["pid"] for e in events if e["ph"] in ("X", "C")}
        metadata_pids = {
            e["pid"] for e in events
            if e["ph"] == "M" and e["name"] == "process_name"
        }
        missing = trace_pids - metadata_pids
        assert not missing, f"PIDs without process_name metadata: {missing}"

    def test_thread_metadata_covers_slice_threads(self, converted_json):
        """Every (PID, TID) used in slice events should have thread_name metadata."""
        data, _ = converted_json
        events = data["traceEvents"]

        slice_threads = {
            (e["pid"], e["tid"]) for e in events if e["ph"] == "X"
        }
        metadata_threads = {
            (e["pid"], e["tid"]) for e in events
            if e["ph"] == "M" and e["name"] == "thread_name"
        }
        missing = slice_threads - metadata_threads
        assert not missing, f"(PID, TID) pairs without thread_name metadata: {missing}"

    def test_no_duplicate_process_metadata(self, converted_json):
        data, _ = converted_json
        events = data["traceEvents"]
        process_meta = [
            e["pid"] for e in events
            if e["ph"] == "M" and e["name"] == "process_name"
        ]
        assert len(process_meta) == len(set(process_meta)), (
            "Duplicate process_name metadata entries found"
        )

    def test_no_duplicate_thread_metadata(self, converted_json):
        data, _ = converted_json
        events = data["traceEvents"]
        thread_meta = [
            (e["pid"], e["tid"]) for e in events
            if e["ph"] == "M" and e["name"] == "thread_name"
        ]
        assert len(thread_meta) == len(set(thread_meta)), (
            "Duplicate thread_name metadata entries found"
        )


# ===========================================================================
# Conversion Stats Tests
# ===========================================================================

class TestConversionStats:
    def test_stats_returned(self, converted_json):
        _, stats = converted_json
        assert "total" in stats
        assert "metadata" in stats
        assert "trace" in stats
        assert "csv_rows" in stats

    def test_total_equals_metadata_plus_trace(self, converted_json):
        _, stats = converted_json
        assert stats["total"] == stats["metadata"] + stats["trace"]

    def test_csv_rows_positive(self, converted_json):
        _, stats = converted_json
        assert stats["csv_rows"] > 0


# ===========================================================================
# File Handling Tests
# ===========================================================================

class TestFileHandling:
    def test_missing_file_returns_none(self, tmp_path):
        result = convert(str(tmp_path / "nonexistent.csv"), str(tmp_path / "out.json"))
        assert result is None

    def test_output_file_is_valid_json(self, sample_csv, tmp_path):
        output_path = tmp_path / "output.json"
        convert(str(sample_csv), str(output_path))
        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)  # Should not raise
        assert isinstance(data, dict)

    def test_empty_csv(self, tmp_path):
        """An empty CSV (header only) should produce valid JSON with no trace events."""
        csv_path = tmp_path / "empty.csv"
        fieldnames = [
            "Process", "Group", "Process ID", "Thread ID",
            "Track", "Block Name", "TimestampStart", "TimestampEnd", "Value",
        ]
        with open(csv_path, "w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()

        output_path = tmp_path / "empty.json"
        stats = convert(str(csv_path), str(output_path))
        assert stats is not None
        assert stats["trace"] == 0

        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
        assert data["traceEvents"] == []


# ===========================================================================
# Multi-file Support Tests
# ===========================================================================

class TestResolveInputFiles:
    def test_single_file(self, sample_csv):
        result = resolve_input_files([str(sample_csv)])
        assert len(result) == 1

    def test_nonexistent_passthrough(self):
        result = resolve_input_files(["nonexistent_xyz.csv"])
        assert result == ["nonexistent_xyz.csv"]

    def test_deduplication(self, sample_csv):
        result = resolve_input_files([str(sample_csv), str(sample_csv)])
        assert len(result) == 1


class TestDeriveOutputPath:
    def test_csv_to_json(self):
        assert derive_output_path("trace1.csv") == "trace1.json"

    def test_with_directory(self):
        result = derive_output_path("data/trace.csv")
        assert result.endswith("trace.json")
        assert "data" in result

    def test_with_output_dir(self, tmp_path):
        out_dir = tmp_path / "output"
        result = derive_output_path("trace.csv", str(out_dir))
        assert result == os.path.join(str(out_dir), "trace.json")
        assert os.path.exists(out_dir)


# ===========================================================================
# End-to-End Test with Real Example CSV
# ===========================================================================

class TestExampleCSV:
    """Tests that run against the real SDP_CSV_example.csv file."""

    def test_conversion_succeeds(self, example_csv_json):
        data, stats = example_csv_json
        assert stats["total"] > 0
        assert stats["trace"] > 0

    def test_has_slice_events(self, example_csv_json):
        data, _ = example_csv_json
        slices = [e for e in data["traceEvents"] if e["ph"] == "X"]
        assert len(slices) > 0

    def test_has_counter_events(self, example_csv_json):
        data, _ = example_csv_json
        counters = [e for e in data["traceEvents"] if e["ph"] == "C"]
        assert len(counters) > 0

    def test_has_metadata_events(self, example_csv_json):
        data, _ = example_csv_json
        metadata = [e for e in data["traceEvents"] if e["ph"] == "M"]
        assert len(metadata) > 0

    def test_all_events_valid_structure(self, example_csv_json):
        data, _ = example_csv_json
        required_fields = {"name", "ph", "ts", "pid", "tid"}
        for i, event in enumerate(data["traceEvents"]):
            missing = required_fields - set(event.keys())
            assert not missing, f"Event {i} missing: {missing}"

    def test_process_metadata_complete(self, example_csv_json):
        data, _ = example_csv_json
        events = data["traceEvents"]
        trace_pids = {e["pid"] for e in events if e["ph"] in ("X", "C")}
        metadata_pids = {
            e["pid"] for e in events
            if e["ph"] == "M" and e["name"] == "process_name"
        }
        missing = trace_pids - metadata_pids
        assert not missing, f"PIDs without process_name metadata: {missing}"

    def test_thread_metadata_complete(self, example_csv_json):
        data, _ = example_csv_json
        events = data["traceEvents"]
        slice_threads = {
            (e["pid"], e["tid"]) for e in events if e["ph"] == "X"
        }
        metadata_threads = {
            (e["pid"], e["tid"]) for e in events
            if e["ph"] == "M" and e["name"] == "thread_name"
        }
        missing = slice_threads - metadata_threads
        assert not missing, f"(PID, TID) pairs without thread_name metadata: {missing}"


# ===========================================================================
# Regression: Process Counter Without "CounterValue" Marker
# ===========================================================================

class TestProcessCounterWithoutCounterValueMarker:
    """Regression tests for process-level counter rows that do NOT contain
    'CounterValue' in their Track column (e.g., GPU Rendering Pipe Metrics
    like 'Avg Bytes / Fragment', '% LRZ Busy', 'Clocks', etc.).

    These rows have only TimestampStart (no TimestampEnd) and a Value.
    Previously they were silently dropped because they matched neither
    the CounterValue branch nor the slice branch.
    """

    def test_process_counter_rows_produce_events(self, converted_json):
        """Rows like 'Avg Bytes / Fragment' must produce counter events."""
        data, _ = converted_json
        counters = [e for e in data["traceEvents"] if e["ph"] == "C"]
        counter_names = {e["name"] for e in counters}
        assert "Avg Bytes / Fragment" in counter_names, (
            f"'Avg Bytes / Fragment' not found in counter events. "
            f"Counter names: {counter_names}"
        )
        assert "% LRZ Busy" in counter_names, (
            f"'% LRZ Busy' not found in counter events. "
            f"Counter names: {counter_names}"
        )

    def test_counter_has_correct_args(self, converted_json):
        """Process counter args should contain the track name as key with numeric value."""
        data, _ = converted_json
        avg_bytes = [
            e for e in data["traceEvents"]
            if e["ph"] == "C" and e["name"] == "Avg Bytes / Fragment"
        ]
        assert len(avg_bytes) > 0
        event = avg_bytes[0]
        assert "Avg Bytes / Fragment" in event["args"]
        assert event["args"]["Avg Bytes / Fragment"] == 128.5

    def test_counter_has_correct_timestamp(self, converted_json):
        data, _ = converted_json
        avg_bytes = [
            e for e in data["traceEvents"]
            if e["ph"] == "C" and e["name"] == "Avg Bytes / Fragment"
        ]
        assert avg_bytes[0]["ts"] == 3000

    def test_counter_phase_is_C(self, converted_json):
        data, _ = converted_json
        avg_bytes = [
            e for e in data["traceEvents"]
            if e["name"] == "Avg Bytes / Fragment"
        ]
        assert all(e["ph"] == "C" for e in avg_bytes)

    def test_dedicated_csv_with_only_process_counters(self, tmp_path):
        """Standalone test with a CSV containing ONLY process counter rows
        without 'CounterValue' in Track — ensures they are never silently dropped."""
        csv_path = tmp_path / "gpu_counters.csv"
        rows = [
            {
                "Process": "Process",
                "Group": "GPU Rendering Pipe Metrics",
                "Process ID": "5000",
                "Thread ID": "",
                "Track": "Avg Preemption Delay",
                "Block Name": "",
                "TimestampStart": "100",
                "TimestampEnd": "",
                "Value": "3.14",
            },
            {
                "Process": "Process",
                "Group": "GPU Rendering Pipe Metrics",
                "Process ID": "5000",
                "Thread ID": "",
                "Track": "Clocks",
                "Block Name": "",
                "TimestampStart": "200",
                "TimestampEnd": "",
                "Value": "999",
            },
            {
                "Process": "Process",
                "Group": "GPU Rendering Pipe Metrics",
                "Process ID": "5000",
                "Thread ID": "",
                "Track": "% Anisotropic Filtered",
                "Block Name": "",
                "TimestampStart": "300",
                "TimestampEnd": "",
                "Value": "0",
            },
        ]
        fieldnames = [
            "Process", "Group", "Process ID", "Thread ID",
            "Track", "Block Name", "TimestampStart", "TimestampEnd", "Value",
        ]
        with open(csv_path, "w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            writer.writerows(rows)

        output_path = tmp_path / "gpu_counters.json"
        stats = convert(str(csv_path), str(output_path))
        assert stats is not None
        # All 3 CSV rows should produce trace events (no silent drops)
        assert stats["trace"] == 3, (
            f"Expected 3 trace events from 3 CSV rows, got {stats['trace']}"
        )

        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)

        counters = [e for e in data["traceEvents"] if e["ph"] == "C"]
        counter_names = {e["name"] for e in counters}
        assert counter_names == {"Avg Preemption Delay", "Clocks", "% Anisotropic Filtered"}


# ===========================================================================
# Regression: System Scheduling Slices
# ===========================================================================

class TestSystemSchedulingSlices:
    """Regression tests for System rows with both TimestampStart AND TimestampEnd
    (e.g., Trace Kernel - Sched CPU). These are scheduling slices, not counters.
    Previously they were treated as counter events and the Block Name was lost.
    """

    def test_system_slice_events_exist(self, converted_json):
        data, _ = converted_json
        slices = [e for e in data["traceEvents"]
                  if e["ph"] == "X" and e.get("cat") == "Trace Kernel - Sched CPU"]
        assert len(slices) == 2, f"Expected 2 system scheduling slices, got {len(slices)}"

    def test_system_slice_has_correct_name(self, converted_json):
        data, _ = converted_json
        slices = [e for e in data["traceEvents"]
                  if e["ph"] == "X" and e.get("cat") == "Trace Kernel - Sched CPU"]
        names = {e["name"] for e in slices}
        assert "APM-TickThread" in names, f"'APM-TickThread' not in slice names: {names}"
        assert "GameThread" in names, f"'GameThread' not in slice names: {names}"

    def test_system_slice_has_correct_duration(self, converted_json):
        data, _ = converted_json
        apm = [e for e in data["traceEvents"]
               if e["ph"] == "X" and e["name"] == "APM-TickThread"]
        assert len(apm) == 1
        assert apm[0]["ts"] == 7699
        assert apm[0]["dur"] == 9560 - 7699

    def test_system_slice_threads_are_per_cpu(self, converted_json):
        """Different CPU tracks should get different TIDs."""
        data, _ = converted_json
        slices = [e for e in data["traceEvents"]
                  if e["ph"] == "X" and e.get("cat") == "Trace Kernel - Sched CPU"]
        tids = {e["tid"] for e in slices}
        assert len(tids) == 2, f"Expected 2 unique TIDs (one per CPU), got {tids}"

    def test_system_slice_thread_metadata(self, converted_json):
        """Each system CPU track should have thread_name metadata."""
        data, _ = converted_json
        slices = [e for e in data["traceEvents"]
                  if e["ph"] == "X" and e.get("cat") == "Trace Kernel - Sched CPU"]
        slice_threads = {(e["pid"], e["tid"]) for e in slices}
        metadata_threads = {
            (e["pid"], e["tid"]) for e in data["traceEvents"]
            if e["ph"] == "M" and e["name"] == "thread_name"
        }
        missing = slice_threads - metadata_threads
        assert not missing, f"System slice threads without metadata: {missing}"


# ===========================================================================
# Regression: No Silently Dropped Rows
# ===========================================================================

class TestNoSilentlyDroppedRows:
    """Catch-all regression test ensuring that every data row in the CSV
    produces at least one trace event. If a new CSV row pattern is added
    that the converter doesn't handle, this test will fail.
    """

    def test_sample_csv_no_dropped_rows(self, converted_json):
        """Every row in the sample CSV should produce exactly one trace event."""
        _, stats = converted_json
        assert stats["trace"] == stats["csv_rows"], (
            f"Expected {stats['csv_rows']} trace events (one per CSV row), "
            f"but got {stats['trace']}. Some rows were silently dropped!"
        )

    def test_dedicated_all_row_types_covered(self, tmp_path):
        """A CSV with one row of each type must produce one event per row."""
        csv_path = tmp_path / "all_types.csv"
        rows = [
            # System counter
            {
                "Process": "System", "Group": "CPU Metrics",
                "Process ID": "", "Thread ID": "",
                "Track": "CPU Freq", "Block Name": "",
                "TimestampStart": "100", "TimestampEnd": "", "Value": "1800",
            },
            # Process slice (both timestamps)
            {
                "Process": "Process", "Group": "Trace app-100",
                "Process ID": "100", "Thread ID": "",
                "Track": "app-100", "Block Name": "render",
                "TimestampStart": "200", "TimestampEnd": "300", "Value": "",
            },
            # Process counter WITH CounterValue marker
            {
                "Process": "Process", "Group": "Trace app-100",
                "Process ID": "100", "Thread ID": "",
                "Track": "fps - CounterValue:", "Block Name": "",
                "TimestampStart": "200", "TimestampEnd": "", "Value": "60",
            },
            # Process counter WITHOUT CounterValue marker (only TimestampStart)
            {
                "Process": "Process", "Group": "GPU Rendering Pipe Metrics",
                "Process ID": "100", "Thread ID": "",
                "Track": "Avg Bytes / Fragment", "Block Name": "",
                "TimestampStart": "200", "TimestampEnd": "", "Value": "42",
            },
            # System scheduling slice (both timestamps, no value)
            {
                "Process": "System", "Group": "Trace Kernel - Sched CPU",
                "Process ID": "", "Thread ID": "",
                "Track": "Sched CPU 0", "Block Name": "mythread",
                "TimestampStart": "500", "TimestampEnd": "600", "Value": "",
            },
        ]
        fieldnames = [
            "Process", "Group", "Process ID", "Thread ID",
            "Track", "Block Name", "TimestampStart", "TimestampEnd", "Value",
        ]
        with open(csv_path, "w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            writer.writerows(rows)

        output_path = tmp_path / "all_types.json"
        stats = convert(str(csv_path), str(output_path))
        assert stats is not None
        assert stats["trace"] == len(rows), (
            f"Expected {len(rows)} trace events (one per CSV row), "
            f"but got {stats['trace']}. Some row types are not handled!"
        )
