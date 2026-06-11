#!/usr/bin/env python3


import re
import sys
import struct
from collections import defaultdict, Counter

# 用于从 stage spec 的 description 中解析 "Process <name> [<pid>]"
PROCESS_DESC_RE = re.compile(r'Process\s+(\S.*?)\s*\[(\d+)\]')


# ============================================================
# Protobuf 解码
# ============================================================

def decode_varint(data, pos):
    result = 0
    shift = 0
    while pos < len(data):
        b = data[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if not (b & 0x80):
            return result, pos
        shift += 7
    raise ValueError("Truncated varint")


def iter_fields(data):
    """遍历 protobuf 消息所有字段，yield (field_number, wire_type, value)。"""
    pos = 0
    data = bytes(data)
    while pos < len(data):
        try:
            tag, pos = decode_varint(data, pos)
        except Exception:
            break
        fn = tag >> 3
        wt = tag & 0x7
        try:
            if wt == 0:
                val, pos = decode_varint(data, pos)
            elif wt == 1:
                val = struct.unpack_from('<Q', data, pos)[0]
                pos += 8
            elif wt == 2:
                length, pos = decode_varint(data, pos)
                val = bytes(data[pos:pos + length])
                pos += length
            elif wt == 5:
                val = struct.unpack_from('<I', data, pos)[0]
                pos += 4
            else:
                break
        except Exception:
            break
        yield fn, wt, val


def get_field(data, fn_want, wt_want=None):
    for fn, wt, val in iter_fields(data):
        if fn == fn_want and (wt_want is None or wt == wt_want):
            return val
    return None


def get_all(data, fn_want, wt_want=None):
    return [v for fn, wt, v in iter_fields(data)
            if fn == fn_want and (wt_want is None or wt == wt_want)]


# ============================================================
# Perfetto proto 字段编号
# https://github.com/google/perfetto/tree/main/protos/perfetto/trace
# ============================================================

TRACE_PACKET = 1

# TracePacket 顶层字段
TP_PROCESS_TREE     = 2
TP_TRUSTED_UID      = 3    # oneof optional_trusted_uid (int32)
TP_TIMESTAMP        = 8
TP_PROCESS_STATS    = 9
TP_TRUSTED_SEQ_ID   = 10
TP_TRACK_EVENT      = 11
TP_INTERNED_DATA    = 12
TP_SEQUENCE_FLAGS   = 13
TP_VULKAN_API_EVENT = 45
TP_GPU_COUNTER      = 52   # GpuCounterEvent
TP_GPU_RENDER_STAGE = 53   # GpuRenderStageEvent  ← 正确字段号
TP_TRACK_DESCRIPTOR = 60
TP_TRUSTED_PID      = 79   # oneof optional_trusted_pid (int32)

# ProcessTree
PT_PROCESSES = 1
PROC_PID     = 1
PROC_CMDLINE = 3
PROC_UID     = 5

# ProcessStats.ProcessEntry
PS_PROCESSES = 1
PSP_PID      = 1
PSP_NAME     = 5   # process_name in ProcessStats

# GpuRenderStageEvent 字段
# 新版 perfetto 用 13/14 作为 iid（指向 InternedData），3/4 是已废弃的旧字段
GRSE_EVENT_ID         = 1
GRSE_DURATION         = 2
GRSE_HW_QUEUE_ID_OLD  = 3   # deprecated
GRSE_STAGE_ID_OLD     = 4   # deprecated
GRSE_CONTEXT          = 5
GRSE_SUBMISSION_ID    = 9
GRSE_HW_QUEUE_IID     = 13
GRSE_STAGE_IID        = 14

# VulkanApiEvent
VAE_VK_DEBUG_UTILS  = 1
VAE_VK_QUEUE_SUBMIT = 2
VQS_PID           = 1
VQS_SUBMISSION_ID = 5
VDUO_PID       = 1
VDUO_VK_DEVICE = 2

# InternedData GPU specs
# 新版 perfetto 把 hw_queue 和 stage 都合并到 field 24 (gpu_render_stage_specifications)
# 并新增 description 字段（field 3），里面常见 "Process <name> [<pid>]" 格式
ID_GPU_HW_QUEUE = 16   # deprecated
ID_GPU_STAGE    = 17   # deprecated
ID_GPU_RENDER_STAGE_SPEC = 24
SPEC_IID         = 1
SPEC_NAME        = 2
SPEC_DESCRIPTION = 3

# TrackDescriptor
TD_UUID  = 1
TD_NAME  = 2
TD_SCOPE = 6

# TrackEvent
TE_TYPE       = 9
TE_TRACK_UUID = 11
TE_NAME       = 23
TE_NAME_IID   = 10
TE_CAT_IIDS   = 3
TE_BEGIN = 1
TE_END   = 2

# InternedData strings
ID_EVENT_NAMES = 2
EN_IID  = 1
EN_NAME = 2


# ============================================================
# 诊断模式
# ============================================================

PACKET_FIELD_NAMES = {
    1:  "ftrace_events",
    2:  "process_tree",
    3:  "trusted_uid",
    4:  "inode_file_map",
    5:  "chrome_events",
    6:  "clock_snapshot",
    7:  "sys_stats",
    8:  "timestamp",
    9:  "process_stats",
    10: "trusted_packet_sequence_id",
    11: "track_event",
    12: "interned_data",
    13: "sequence_flags",
    23: "trace_stats",
    35: "android_log",
    45: "vulkan_api_event",
    52: "gpu_counter_event",
    53: "gpu_render_stage_event",   # ← 正确
    54: "streaming_profile_packet",
    56: "heap_graph",
    57: "graphics_frame_event",
    58: "timestamp_clock_id / vulkan_memory_event",
    59: "gpu_log",
    60: "track_descriptor",
    65: "android_energy_estimation_breakdown",
    76: "android_game_intervention_list / gpu_mem_total",
    79: "trusted_pid",
}


def debug_scan(data):
    print("=" * 65)
    print("诊断模式：扫描 trace 内容")
    print("=" * 65)

    packet_field_counter = Counter()
    track_descriptors = []
    track_event_tracks = Counter()
    interned_names = {}
    track_uuid_to_name = {}
    total_packets = 0
    pid_set = set()

    # 采样几个 GpuRenderStageEvent 包，打印原始字段
    grse_samples = []

    for fn, wt, pkt_data in iter_fields(data):
        if fn != TRACE_PACKET or wt != 2:
            continue
        total_packets += 1

        ts = None; seq_id = None; trusted_pid = None

        for pfn, pwt, pval in iter_fields(pkt_data):
            packet_field_counter[pfn] += 1

            if pfn == TP_TIMESTAMP and pwt == 0:
                ts = pval
            elif pfn == TP_TRUSTED_SEQ_ID and pwt == 0:
                seq_id = pval
            elif pfn == TP_TRUSTED_PID and pwt == 0:
                trusted_pid = pval
                pid_set.add(pval)

            elif pfn == TP_TRACK_DESCRIPTOR and pwt == 2:
                uuid   = get_field(pval, TD_UUID, 0)
                name_b = get_field(pval, TD_NAME, 2)
                scope_b = get_field(pval, TD_SCOPE, 2)
                name  = name_b.decode('utf-8', errors='replace') if name_b else ''
                scope = scope_b.decode('utf-8', errors='replace') if scope_b else ''
                track_descriptors.append((uuid, name, scope))
                if uuid is not None:
                    track_uuid_to_name[uuid] = name

            elif pfn == TP_TRACK_EVENT and pwt == 2:
                tuuid = get_field(pval, TE_TRACK_UUID, 0)
                if tuuid is not None:
                    track_event_tracks[tuuid] += 1

            elif pfn == TP_GPU_RENDER_STAGE and pwt == 2:
                if len(grse_samples) < 5:
                    fields = {f: v for f, wt2, v in iter_fields(pval) if wt2 == 0}
                    grse_samples.append((trusted_pid, ts, fields))

            elif pfn == TP_INTERNED_DATA and pwt == 2:
                for idfn, idwt, idval in iter_fields(pval):
                    if idfn == ID_EVENT_NAMES and idwt == 2:
                        iid = get_field(idval, EN_IID, 0)
                        nb  = get_field(idval, EN_NAME, 2)
                        if iid is not None and nb is not None and seq_id is not None:
                            interned_names[(seq_id, iid)] = nb.decode('utf-8', errors='replace')
                    elif idfn == ID_GPU_STAGE and idwt == 2:
                        iid = get_field(idval, SPEC_IID, 0)
                        nb  = get_field(idval, SPEC_NAME, 2)
                        if iid is not None and nb is not None and seq_id is not None:
                            interned_names[(seq_id, iid)] = nb.decode('utf-8', errors='replace')

    print(f"\n总 TracePacket 数: {total_packets}")
    print(f"\n【TracePacket 字段出现次数】")
    for fnum, count in sorted(packet_field_counter.items()):
        fname = PACKET_FIELD_NAMES.get(fnum, f"unknown_field_{fnum}")
        print(f"  field {fnum:3d} ({fname}): {count}")

    print(f"\n【trusted_pid 集合 ({len(pid_set)} 个不同 PID)】")
    for pid in sorted(pid_set):
        print(f"  pid={pid}")

    print(f"\n【GpuRenderStageEvent 采样（前 5 个，字段号: 值）】")
    for trusted_pid, ts, fields in grse_samples:
        print(f"  trusted_pid={trusted_pid}  ts={ts}  fields={fields}")

    print(f"\n【TrackDescriptor ({len(track_descriptors)} 个)】")
    for uuid, name, scope in track_descriptors[:30]:
        uuid_str = f"0x{uuid:016x}" if uuid is not None else "None"
        te_count = track_event_tracks.get(uuid, 0)
        print(f"  uuid={uuid_str}  name={name!r:40s}  scope={scope!r}  events={te_count}")

    print(f"\n【InternedData event names (前 20 个)】")
    for (seq, iid), name in list(interned_names.items())[:20]:
        print(f"  seq={seq}  iid={iid}  name={name!r}")

    print("\n" + "=" * 65)


# ============================================================
# Protobuf 编码
# ============================================================

def encode_varint(value):
    if value < 0:
        value &= 0xFFFFFFFFFFFFFFFF
    result = []
    while True:
        bits = value & 0x7F
        value >>= 7
        result.append(bits | (0x80 if value else 0))
        if not value:
            break
    return bytes(result)


def enc_varint(fn, val):
    return encode_varint((fn << 3) | 0) + encode_varint(val)


def enc_bytes(fn, data):
    return encode_varint((fn << 3) | 2) + encode_varint(len(data)) + data


def enc_str(fn, s):
    return enc_bytes(fn, s.encode('utf-8'))


def write_packet(f, pkt):
    f.write(enc_bytes(TRACE_PACKET, pkt))


# ============================================================
# 输出 proto 字段编号
# ============================================================

OUT_TD_UUID = 1; OUT_TD_NAME = 2; OUT_TD_PROCESS = 3
OUT_PD_PID = 1; OUT_PD_NAME = 6
OUT_TE_CAT_IIDS = 3; OUT_TE_TYPE = 9; OUT_TE_TRACK_UUID = 11; OUT_TE_NAME_IID = 10
OUT_TE_BEGIN = 1; OUT_TE_END = 2
OUT_ID_CATS = 1; OUT_ID_NAMES = 2
OUT_EC_IID = 1; OUT_EC_NAME = 2
SEQ_RESET = 1; SEQ_NEEDS_INC = 2


def make_track_descriptor(uuid, name, pid=None, pname=None, seq_id=1):
    td = enc_varint(OUT_TD_UUID, uuid) + enc_str(OUT_TD_NAME, name)
    if pid is not None and pid >= 0:
        pd = enc_varint(OUT_PD_PID, pid)
        if pname:
            pd += enc_str(OUT_PD_NAME, pname)
        td += enc_bytes(OUT_TD_PROCESS, pd)
    pkt = enc_bytes(TP_TRACK_DESCRIPTOR, td) + enc_varint(TP_TRUSTED_SEQ_ID, seq_id)
    return pkt


def make_interned(categories, names):
    """
    categories: {iid(int): name(str)}
    names:      {name(str): iid(int)}  ← name_to_iid 的格式
    """
    b = b''
    for iid, n in categories.items():          # {iid: name}
        b += enc_bytes(OUT_ID_CATS, enc_varint(OUT_EC_IID, iid) + enc_str(OUT_EC_NAME, n))
    for n, iid in names.items():               # {name: iid} — 注意顺序
        b += enc_bytes(OUT_ID_NAMES, enc_varint(OUT_EC_IID, iid) + enc_str(OUT_EC_NAME, n))
    return b


def make_begin(ts, uuid, cat_iid, name_iid, seq_id, interned=None):
    te = (enc_varint(OUT_TE_TYPE, OUT_TE_BEGIN) + enc_varint(OUT_TE_TRACK_UUID, uuid) +
          enc_varint(OUT_TE_CAT_IIDS, cat_iid) + enc_varint(OUT_TE_NAME_IID, name_iid))
    pkt = (enc_varint(TP_TIMESTAMP, ts) + enc_bytes(TP_TRACK_EVENT, te) +
           enc_varint(TP_TRUSTED_SEQ_ID, seq_id))
    if interned is not None:
        pkt += enc_bytes(TP_INTERNED_DATA, interned)
        pkt += enc_varint(TP_SEQUENCE_FLAGS, SEQ_RESET | SEQ_NEEDS_INC)
    return pkt


def make_end(ts, uuid, seq_id):
    te = enc_varint(OUT_TE_TYPE, OUT_TE_END) + enc_varint(OUT_TE_TRACK_UUID, uuid)
    return (enc_varint(TP_TIMESTAMP, ts) + enc_bytes(TP_TRACK_EVENT, te) +
            enc_varint(TP_TRUSTED_SEQ_ID, seq_id))


# ============================================================
# 解析 trace
# ============================================================

def parse_trace(data):
    """
    解析 Perfetto trace，提取 GpuRenderStageEvent 及进程信息。

    进程关联优先级：
      1. TracePacket.trusted_pid (field 79)
      2. 该序列下任一 stage spec 的 description "Process <name> [<pid>]"
      3. VulkanApiEvent.VkQueueSubmit.submission_id → pid
      4. VulkanApiEvent.VkDebugUtilsObjectName.vk_device → pid
      5. GpuRenderStageEvent.context 值作为 fallback 标识
    """
    pid_to_name       = {}   # pid -> process_name
    submission_to_pid = {}   # submission_id -> pid
    context_to_pid    = {}   # context -> pid
    events            = []
    stage_names       = {}   # (seq_id, iid) -> name
    hw_queue_names    = {}   # (seq_id, iid) -> name
    seq_to_process    = {}   # seq_id -> (pid, name)  ← 从 description 解析

    def parse_process_desc(desc):
        m = PROCESS_DESC_RE.search(desc)
        if not m:
            return None
        return m.group(1).strip(), int(m.group(2))

    for fn, wt, pkt_data in iter_fields(data):
        if fn != TRACE_PACKET or wt != 2:
            continue

        # ---- 第一遍：抓 packet 级的 ts / seq_id / trusted_pid（顺序无关） ----
        ts          = None
        seq_id      = None
        trusted_pid = None
        for pfn, pwt, pval in iter_fields(pkt_data):
            if pfn == TP_TIMESTAMP and pwt == 0:
                ts = pval
            elif pfn == TP_TRUSTED_SEQ_ID and pwt == 0:
                seq_id = pval
            elif pfn == TP_TRUSTED_PID and pwt == 0:
                trusted_pid = pval

        # ---- 第二遍：处理具体内容 ----
        for pfn, pwt, pval in iter_fields(pkt_data):

            # ---- ProcessTree ----
            if pfn == TP_PROCESS_TREE and pwt == 2:
                for ptfn, ptwt, ptval in iter_fields(pval):
                    if ptfn == PT_PROCESSES and ptwt == 2:
                        pid = get_field(ptval, PROC_PID, 0)
                        cmdlines = get_all(ptval, PROC_CMDLINE, 2)
                        if pid is not None:
                            if cmdlines:
                                raw = cmdlines[0].decode('utf-8', errors='replace')
                                name = raw.split('/')[-1].rstrip('\x00').split('\x00')[0]
                            else:
                                name = f"pid_{pid}"
                            pid_to_name[pid] = name

            # ---- ProcessStats ----
            elif pfn == TP_PROCESS_STATS and pwt == 2:
                for psfn, pswt, psval in iter_fields(pval):
                    if psfn == PS_PROCESSES and pswt == 2:
                        pid = get_field(psval, PSP_PID, 0)
                        name_b = get_field(psval, PSP_NAME, 2)
                        if pid is not None and name_b is not None:
                            name = name_b.decode('utf-8', errors='replace').rstrip('\x00')
                            if name:
                                pid_to_name[pid] = name

            # ---- VulkanApiEvent ----
            elif pfn == TP_VULKAN_API_EVENT and pwt == 2:
                for vaefn, vaewt, vaeval in iter_fields(pval):
                    if vaefn == VAE_VK_QUEUE_SUBMIT and vaewt == 2:
                        vqs_pid = get_field(vaeval, VQS_PID, 0)
                        vqs_sid = get_field(vaeval, VQS_SUBMISSION_ID, 0)
                        if vqs_pid is not None and vqs_sid is not None:
                            submission_to_pid[vqs_sid] = vqs_pid
                    elif vaefn == VAE_VK_DEBUG_UTILS and vaewt == 2:
                        vduo_pid = get_field(vaeval, VDUO_PID, 0)
                        vduo_dev = get_field(vaeval, VDUO_VK_DEVICE, 0)
                        if vduo_pid is not None and vduo_dev is not None:
                            context_to_pid[vduo_dev] = vduo_pid

            # ---- GpuRenderStageEvent (field 53) ----
            elif pfn == TP_GPU_RENDER_STAGE and pwt == 2:
                dur     = get_field(pval, GRSE_DURATION, 0)
                # 兼容新旧字段：13/14 是 iid，3/4 是已废弃的 id
                stage_iid = (get_field(pval, GRSE_STAGE_IID, 0)
                             or get_field(pval, GRSE_STAGE_ID_OLD, 0))
                hw_iid    = (get_field(pval, GRSE_HW_QUEUE_IID, 0)
                             or get_field(pval, GRSE_HW_QUEUE_ID_OLD, 0))
                context   = get_field(pval, GRSE_CONTEXT, 0)
                sub_id    = get_field(pval, GRSE_SUBMISSION_ID, 0)
                if ts is not None and dur is not None and dur > 0:
                    events.append({
                        'ts':          ts,
                        'dur':         dur,
                        'context':     context,
                        'sub_id':      sub_id,
                        'stage_iid':   stage_iid,
                        'hw_iid':      hw_iid,
                        'seq_id':      seq_id,
                        'trusted_pid': trusted_pid,
                    })

            # ---- InternedData ----
            elif pfn == TP_INTERNED_DATA and pwt == 2:
                for idfn, idwt, idval in iter_fields(pval):
                    # 旧版独立的 stage / hw_queue 列表
                    if idfn == ID_GPU_STAGE and idwt == 2:
                        iid = get_field(idval, SPEC_IID, 0)
                        nb  = get_field(idval, SPEC_NAME, 2)
                        if iid is not None and nb is not None and seq_id is not None:
                            stage_names[(seq_id, iid)] = nb.decode('utf-8', errors='replace')
                    elif idfn == ID_GPU_HW_QUEUE and idwt == 2:
                        iid = get_field(idval, SPEC_IID, 0)
                        nb  = get_field(idval, SPEC_NAME, 2)
                        if iid is not None and nb is not None and seq_id is not None:
                            hw_queue_names[(seq_id, iid)] = nb.decode('utf-8', errors='replace')
                    # 新版合并的 gpu_render_stage_specifications
                    elif idfn == ID_GPU_RENDER_STAGE_SPEC and idwt == 2:
                        iid  = get_field(idval, SPEC_IID, 0)
                        nb   = get_field(idval, SPEC_NAME, 2)
                        descb = get_field(idval, SPEC_DESCRIPTION, 2)
                        if iid is None or seq_id is None:
                            continue
                        if nb is not None:
                            name = nb.decode('utf-8', errors='replace')
                            # 同一个 iid 既可能被 stage_iid 也可能被 hw_queue_iid 引用，
                            # 两个表都填进去，查找时择一即可
                            stage_names.setdefault((seq_id, iid), name)
                            hw_queue_names.setdefault((seq_id, iid), name)
                        if descb is not None:
                            desc = descb.decode('utf-8', errors='replace')
                            parsed = parse_process_desc(desc)
                            if parsed is not None:
                                pname, ppid = parsed
                                # 序列内同一进程，覆盖即可
                                seq_to_process[seq_id] = (ppid, pname)
                                pid_to_name.setdefault(ppid, pname)

    return (pid_to_name, submission_to_pid, context_to_pid,
            events, stage_names, hw_queue_names, seq_to_process)


# ============================================================
# 主逻辑
# ============================================================

def main():
    if len(sys.argv) < 2:
        print("用法: python split_renderstage_by_process.py <input.pftrace> <output.pftrace>")
        print("      python split_renderstage_by_process.py --debug <input.pftrace>")
        sys.exit(1)

    debug_mode = (sys.argv[1] == '--debug')
    if debug_mode:
        if len(sys.argv) < 3:
            print("用法: python split_renderstage_by_process.py --debug <input.pftrace>")
            sys.exit(1)
        input_file  = sys.argv[2]
        output_file = None
    else:
        if len(sys.argv) < 3:
            print("用法: python split_renderstage_by_process.py <input.pftrace> <output.pftrace>")
            sys.exit(1)
        input_file  = sys.argv[1]
        output_file = sys.argv[2]

    # ---- 读取 trace ----
    print(f"正在读取: {input_file}")
    try:
        with open(input_file, 'rb') as f:
            data = f.read()
    except OSError as e:
        print(f"错误: 无法读取文件: {e}")
        sys.exit(1)
    print(f"  文件大小: {len(data) / 1024 / 1024:.1f} MB")

    if debug_mode:
        debug_scan(data)
        return

    # ---- 解析 trace ----
    print("正在解析 trace...")
    (pid_to_name, submission_to_pid, context_to_pid,
     events, stage_names, hw_queue_names, seq_to_process) = parse_trace(data)

    print(f"  进程名映射:       {len(pid_to_name)} 条")
    print(f"  序列→进程 (desc): {len(seq_to_process)} 条")
    print(f"  VkQueueSubmit:    {len(submission_to_pid)} 条")
    print(f"  RenderStage 事件: {len(events)} 条")

    if not events:
        print("\n未找到 RenderStage 事件（field 53）。")
        print("建议运行诊断模式：")
        print(f"  python split_renderstage_by_process.py --debug {input_file}")
        sys.exit(0)

    # ---- 确定每个事件的所属进程 ----
    def resolve_process(ev):
        # 1) trusted_pid（最直接）
        if ev['trusted_pid'] is not None and ev['trusted_pid'] > 0:
            pid = ev['trusted_pid']
            return pid, pid_to_name.get(pid, f"pid_{pid}")

        # 2) 同一序列下的 stage spec description 已经告诉我们 (pid, name)
        if ev['seq_id'] is not None:
            pp = seq_to_process.get(ev['seq_id'])
            if pp is not None:
                return pp  # (pid, name)

        # 3) submission_id → pid
        pid = None
        if ev['sub_id'] is not None:
            pid = submission_to_pid.get(ev['sub_id'])

        # 4) context → pid
        if pid is None and ev['context'] is not None:
            pid = context_to_pid.get(ev['context'])

        if pid is not None:
            return pid, pid_to_name.get(pid, f"pid_{pid}")

        # 5) Fallback: 用 context 值区分
        ctx = ev['context']
        if ctx is not None:
            return -1, f"GPU_ctx_0x{ctx:016x}"
        return -1, "Unknown"

    # ---- 获取 stage 名称 ----
    def get_stage_name(ev):
        if ev['stage_iid'] is not None and ev['seq_id'] is not None:
            n = stage_names.get((ev['seq_id'], ev['stage_iid']))
            if n:
                return n
        if ev['hw_iid'] is not None and ev['seq_id'] is not None:
            n = hw_queue_names.get((ev['seq_id'], ev['hw_iid']))
            if n:
                return n
        return "RenderStage"

    # ---- 按 Process 分组 ----
    process_slices = defaultdict(list)
    for ev in events:
        pid, pname = resolve_process(ev)
        process_slices[(pid, pname)].append({
            'ts':   ev['ts'],
            'dur':  ev['dur'],
            'name': get_stage_name(ev),
        })

    print(f"\n按 Process 分组结果:")
    print(f"  {'PID':<8} {'Process Name':<40} {'Slice 数量':>10}")
    print(f"  {'-'*8} {'-'*40} {'-'*10}")
    for (pid, name), slices in sorted(process_slices.items()):
        pid_str = str(pid) if pid >= 0 else "N/A"
        print(f"  {pid_str:<8} {name:<40} {len(slices):>10}")

    # ---- 构建字符串表 ----
    all_names = set()
    for slices in process_slices.values():
        for s in slices:
            all_names.add(s['name'])
    name_to_iid   = {n: i + 1 for i, n in enumerate(sorted(all_names))}
    category_iid  = 1
    category_name = "gpu.renderstage"

    # ---- 写出新 trace ----
    BASE_UUID   = 0xCAFEBABE00000000
    BASE_SEQ_ID = 0xF000
    total = 0

    if output_file == input_file:
        print(f"错误: 输出文件不能和输入文件同名: {input_file}")
        sys.exit(1)

    print(f"\n正在写出: {output_file}")
    with open(output_file, 'wb') as f:
        # 保留原始 trace 内容，让 Perfetto 仍能识别 pid=xxx 的 surfaceflinger
        # 等已有进程，并通过 process.pid 把新的 RenderStage track 合并到同一节点。
        f.write(data)

        for i, ((pid, pname), slices) in enumerate(sorted(process_slices.items())):
            uuid      = BASE_UUID + i
            seq_id    = BASE_SEQ_ID + i
            # process_name 用真实名字（如 de.saschawillems.vulkanBloom），
            # 让 Perfetto UI 把进程显示为 "<name> <pid>"，PID 不会重复
            if pid >= 0:
                td_pid   = pid
                td_pname = pname
                track_name = f"RenderStage [{pid}]"
            else:
                td_pid   = None
                td_pname = None
                track_name = f"RenderStage [{pname}]"

            write_packet(f, make_track_descriptor(
                uuid, track_name,
                pid=td_pid, pname=td_pname, seq_id=seq_id,
            ))

            interned = make_interned({category_iid: category_name}, name_to_iid)

            for j, s in enumerate(slices):
                write_packet(f, make_begin(
                    s['ts'], uuid, category_iid, name_to_iid[s['name']],
                    seq_id, interned if j == 0 else None,
                ))
                write_packet(f, make_end(s['ts'] + s['dur'], uuid, seq_id))
                total += 1

    print(f"完成！写入 {total} 个 slice → {output_file}")
    print()
    print("查看方法:")
    print("  1. 打开 https://ui.perfetto.dev")
    print("  2. 加载输出文件")
    print(f"  3. 可看到 {len(process_slices)} 个独立的 RenderStage Track")


if __name__ == '__main__':
    main()
