# ============================================================================================================
# 
#                  Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
#                              SPDX-License-Identifier: BSD-3-Clause
# 
# ============================================================================================================ 

import csv, json, argparse, os, re, glob


def extract_tid_from_track(track):
    """Extract thread ID from Track column (e.g., 'binder:30114_3-30127' -> 30127)."""
    m = re.search(r'-(\d+)$', track)
    return int(m.group(1)) if m else None

def extract_thread_name_from_track(track):
    """Extract human-readable thread name from Track column."""
    m = re.match(r'^(.+)-\d+$', track)
    return m.group(1) if m else track

def extract_counter_name(track):
    """Extract counter name from Track column (e.g., 'aaRdy - CounterValue:' -> 'aaRdy')."""
    m = re.match(r'^(.+?)\s*-\s*CounterValue:?\s*$', track)
    return m.group(1).strip() if m else track

def extract_process_name_from_group(group):
    """Extract process name from Group column (e.g., 'Trace surfaceflinger-2109' -> 'surfaceflinger-2109')."""
    m = re.match(r'^Trace\s+(.+)$', group)
    return m.group(1) if m else group

def detect_csv_format(input_path):
    """Detect whether a CSV file is 'trace', 'realtime', or None."""
    with open(input_path, 'r', encoding='utf-8') as f:
        header = [h.strip() for h in next(csv.reader(f))]
    if 'Category' in header and 'Metric' in header and 'Timestamp' in header:
        return 'realtime'
    if 'Group' in header and 'Track' in header and 'TimestampStart' in header:
        return 'trace'
    return None

def _metadata_events(process_names, thread_names=None):
    """Build metadata event list from process_names dict and optional thread_names dict."""
    events = [{"args": {"name": n}, "cat": "__metadata", "name": "process_name",
                "ph": "M", "pid": p, "tid": 0, "ts": 0} for p, n in process_names.items()]
    if thread_names:
        events += [{"args": {"name": n}, "cat": "__metadata", "name": "thread_name",
                     "ph": "M", "pid": p, "tid": t, "ts": 0} for (p, t), n in thread_names.items()]
    return events

def _write_perfetto(events, meta, output_path, input_path, fmt_label):
    """Write Perfetto JSON, print summary, return stats dict."""
    all_events = meta + events
    with open(output_path, 'w', encoding='utf-8') as f:
        json.dump({"traceEvents": all_events}, f, indent=2)
    stats = {"total": len(all_events), "metadata": len(meta), "trace": len(events)}
    print(f"Successfully converted {stats['total']} events "
          f"({stats['metadata']} metadata + {stats['trace']} trace) "
          f"from {input_path} to {output_path} [{fmt_label} format]")
    return stats

def convert_realtime(input_path, output_path):
    """Convert a realtime SDP CSV file to Perfetto JSON format."""
    events, process_names = [], {}
    group_pids, next_pid = {}, 800000

    with open(input_path, 'r', encoding='utf-8') as f:
        rows = list(csv.DictReader(f))

    for row in rows:
        process, category = row.get('Process', '').strip(), row.get('Category', '').strip()
        metric, ts_str = row.get('Metric', '').strip(), row.get('Timestamp', '').strip()
        value_str = row.get('Value', '').strip()
        if not ts_str or not metric:
            continue
        key = (process, category)
        if key not in group_pids:
            group_pids[key] = next_pid
            process_names[next_pid] = f"Global – {category}" if process == 'Global' else f"{process} – {category}"
            next_pid += 1
        try:
            ts, val = int(ts_str), float(value_str) if value_str else 0.0
        except ValueError:
            continue
        events.append({"name": metric, "ph": "C", "ts": ts, "pid": group_pids[key],
                        "tid": 0, "cat": category, "args": {metric: val}})

    meta = _metadata_events(process_names)
    stats = _write_perfetto(events, meta, output_path, input_path, "realtime")
    stats["csv_rows"] = len(rows)
    return stats

def convert_trace(input_path, output_path):
    """Convert a trace-mode SDP CSV file to Perfetto JSON format."""
    events, process_names, thread_names = [], {}, {}
    sys_group_pids, next_sys_pid = {}, 900000
    sys_track_tids, next_sys_tid = {}, 1

    with open(input_path, 'r', encoding='utf-8') as f:
        rows = list(csv.DictReader(f))

    # First pass: find main-thread names (TID == PID) for process naming
    pid_names = {}
    for row in rows:
        if row.get('Process', '').strip() != 'Process':
            continue
        pid_str = row.get('Process ID', '').strip()
        if not pid_str or not pid_str.isdigit():
            continue
        pid, track = int(pid_str), row.get('Track', '').strip()
        if 'CounterValue' not in track and extract_tid_from_track(track) == pid and pid not in pid_names:
            pid_names[pid] = extract_thread_name_from_track(track)

    def _pname(pid, group):
        if pid not in process_names:
            process_names[pid] = pid_names.get(pid, extract_process_name_from_group(group))

    for row in rows:
        rtype = row.get('Process', '').strip()
        group, track = row.get('Group', '').strip(), row.get('Track', '').strip()
        pid_str, tid_str = row.get('Process ID', '').strip(), row.get('Thread ID', '').strip()
        block = row.get('Block Name', '').strip()
        ts_s, ts_e = row.get('TimestampStart', '').strip(), row.get('TimestampEnd', '').strip()
        value = row.get('Value', '').strip()

        try:
            if rtype == 'System':
                if group not in sys_group_pids:
                    sys_group_pids[group] = next_sys_pid
                    next_sys_pid += 1
                spid = sys_group_pids[group]
                process_names[spid] = group

                if ts_s and ts_e:  # scheduling slice
                    name = block or track or 'Unknown'
                    tk = (spid, track)
                    if tk not in sys_track_tids:
                        sys_track_tids[tk] = next_sys_tid
                        next_sys_tid += 1
                    stid = sys_track_tids[tk]
                    thread_names.setdefault((spid, stid), track or 'Unknown')
                    events.append({"name": name, "ph": "X", "ts": int(ts_s),
                                   "dur": int(ts_e) - int(ts_s), "pid": spid, "tid": stid, "cat": group})
                else:  # system counter
                    ts = ts_s or ts_e
                    if not ts:
                        continue
                    cname = track or block or 'Unknown'
                    events.append({"name": cname, "ph": "C", "ts": int(ts),
                                   "pid": spid, "tid": 0, "args": {cname: float(value) if value else 0}})

            elif rtype == 'Process':
                pid = int(pid_str) if pid_str.isdigit() else 0
                is_cv = 'CounterValue' in track

                if is_cv:  # explicit counter track
                    ts = ts_s or ts_e
                    if not ts:
                        continue
                    _pname(pid, group)
                    cname = extract_counter_name(track)
                    events.append({"name": cname, "ph": "C", "ts": int(ts),
                                   "pid": pid, "tid": 0, "args": {cname: float(value) if value else 0}})
                elif ts_s and ts_e:  # slice
                    tid = int(tid_str) if tid_str and tid_str.isdigit() else (extract_tid_from_track(track) or pid)
                    _pname(pid, group)
                    thread_names.setdefault((pid, tid), extract_thread_name_from_track(track))
                    events.append({"name": block or track or 'Unknown', "ph": "X", "ts": int(ts_s),
                                   "dur": int(ts_e) - int(ts_s), "pid": pid, "tid": tid, "cat": group})
                elif ts_s or ts_e:  # implicit counter (no CounterValue marker)
                    ts = ts_s or ts_e
                    _pname(pid, group)
                    cname = track or block or 'Unknown'
                    events.append({"name": cname, "ph": "C", "ts": int(ts),
                                   "pid": pid, "tid": 0, "args": {cname: float(value) if value else 0}})
        except ValueError:
            continue

    meta = _metadata_events(process_names, thread_names)
    stats = _write_perfetto(events, meta, output_path, input_path, "trace")
    stats["csv_rows"] = len(rows)
    return stats

def convert(input_path, output_path):
    """Convert a single SDP CSV file to Perfetto JSON (auto-detects format)."""
    if not os.path.exists(input_path):
        print(f"Error: File {input_path} not found.")
        return None
    fmt = detect_csv_format(input_path)
    if fmt == 'realtime':
        return convert_realtime(input_path, output_path)
    if fmt == 'trace':
        return convert_trace(input_path, output_path)
    print(f"Error: Unrecognized CSV format in {input_path}.")
    return None

def resolve_input_files(patterns):
    """Resolve input file patterns (supports glob) to deduplicated CSV file paths."""
    files = []
    for p in patterns:
        expanded = glob.glob(p)
        files.extend(expanded if expanded else [p])
    seen, unique = set(), []
    for f in files:
        n = os.path.normpath(f)
        if n not in seen:
            seen.add(n)
            unique.append(f)
    return unique

def derive_output_path(input_path, output_dir=None):
    """Derive JSON output path from CSV input path."""
    name = os.path.splitext(os.path.basename(input_path))[0] + '.json'
    if output_dir:
        os.makedirs(output_dir, exist_ok=True)
        return os.path.join(output_dir, name)
    return os.path.join(os.path.dirname(input_path), name)

def main():
    parser = argparse.ArgumentParser(description="Convert SDP CSV to Perfetto JSON",
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input", nargs='+', help="Path(s) to SDP CSV file(s)")
    parser.add_argument("-o", "--output", default=None, help="Output JSON path (single file only)")
    parser.add_argument("--output-dir", default=None, help="Output directory for converted files")
    args = parser.parse_args()

    input_files = resolve_input_files(args.input)
    if not input_files:
        print("Error: No input files found.")
        return

    if len(input_files) == 1 and args.output:
        convert(input_files[0], args.output)
        return

    if args.output and len(input_files) > 1:
        print("Warning: --output/-o is ignored for multiple files. Use --output-dir instead.")

    ok = sum(1 for f in input_files if convert(f, derive_output_path(f, args.output_dir)))
    if len(input_files) > 1:
        print(f"\nBatch complete: {ok} succeeded, {len(input_files) - ok} failed out of {len(input_files)} files.")


if __name__ == "__main__":
    main()