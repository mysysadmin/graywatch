#!/usr/bin/env python3
"""Turn a Graywatch support archive into a human-readable system summary.

The input is the .tar.gz written by collect-graywatch-support.sh, or a
directory holding its extracted contents. The archive is read in memory and
never extracted to disk. Only the Python standard library is used.

Every conclusion is derived from recorded evidence and names that evidence.
Thresholds used here are triage heuristics for a human reader; the daemon's
own per-interval conditions remain the authoritative classification.
"""

import argparse
import collections
import datetime
import io
import json
import os
import re
import statistics
import sys
import tarfile

CRITICAL, WARNING, INFO = "critical", "warning", "info"
RANK = {CRITICAL: 0, WARNING: 1, INFO: 2}
QUIET_CONDITIONS = {"healthy", "idle", "inactive", "no_pressure", "unavailable"}
MAX_MEMBER_BYTES = 512 * 1024 * 1024

# Kernel wait channels grouped by the subsystem they implicate. Matching is by
# prefix or substring because channel names vary between kernel versions.
WAIT_CHANNEL_HINTS = [
    (("request_wait_answer", "fuse_"), "fuse",
     "FUSE request outstanding: the userspace filesystem daemon (or the storage "
     "service behind it) has not answered"),
    (("rpc_wait_bit_killable", "nfs_", "rpc_"), "nfs",
     "NFS/SunRPC call outstanding: the NFS server or network path is slow"),
    (("io_schedule", "blk_mq_get_tag", "wait_on_page_bit", "folio_wait_bit",
      "bit_wait_io", "__lock_page", "wait_on_buffer", "blkdev_"), "block",
     "waiting for block I/O or page writeback to complete"),
    (("jbd2", "xlog", "xfs_", "ext4_", "btrfs_"), "filesystem",
     "waiting on local filesystem journal or metadata"),
    (("rwsem", "mutex", "down_read", "down_write", "lock"), "lock",
     "waiting on a kernel lock held by another task"),
    (("throttle_direct_reclaim", "balance_dirty_pages", "shrink_"), "memory",
     "throttled by memory reclaim or dirty-page writeback"),
]

KERNEL_LOG_PATTERNS = [
    ("storage_error", WARNING,
     re.compile(r"I/O error|blk_update_request|critical (medium|target) error|"
                r"Buffer I/O error|end_request", re.I),
     "Block device errors were logged. Check SMART/NVMe health "
     "(`smartctl -a`, `nvme smart-log`) and the controller/HBA."),
    ("nvme_timeout", WARNING,
     re.compile(r"nvme.*(timeout|resetting controller|I/O \d+ QID)", re.I),
     "NVMe command timeouts or controller resets. Check firmware, PCIe errors "
     "(`dmesg | grep -i aer`) and the drive's error log."),
    ("hung_task", CRITICAL,
     re.compile(r"blocked for more than \d+ seconds"),
     "The kernel's hung-task detector fired. The stack printed after each "
     "report in kernel.log shows what the task was waiting on."),
    ("oom", CRITICAL,
     re.compile(r"Out of memory|oom-kill|Killed process \d+", re.I),
     "The OOM killer terminated processes. Check which process was killed and "
     "its cgroup memory limit."),
    ("page_allocation_failure", WARNING,
     re.compile(r"page allocation failure", re.I),
     "Kernel allocations failed (often fragmentation for high-order pages)."),
    ("lockup", CRITICAL,
     re.compile(r"soft lockup|hard LOCKUP|rcu_(sched|preempt) detected stall|"
                r"rcu: INFO", re.I),
     "CPU lockup or RCU stall: a CPU stopped scheduling. This often points at a "
     "driver or firmware problem."),
    ("link_flap", WARNING,
     re.compile(r"(Link is (Down|Up)|link (down|up)|port .*(DOWN|ACTIVE)|"
                r"mlx\d_core.*(link|port))", re.I),
     "Network or InfiniBand link state changes were logged. Check cables, "
     "transceivers and the switch port."),
    ("filesystem_error", CRITICAL,
     re.compile(r"EXT4-fs error|XFS .*(error|shutdown|corrupt)|"
                r"BTRFS.*(error|corrupt)", re.I),
     "Filesystem errors were logged. The filesystem may be remounted read-only."),
    ("hardware_error", CRITICAL,
     re.compile(r"Machine Check|mce: \[Hardware Error\]|EDAC .*error", re.I),
     "Hardware errors (MCE/EDAC) were logged. Engage the hardware vendor."),
    ("segfault", INFO,
     re.compile(r"segfault at|general protection fault|traps:"),
     "Userspace processes crashed during the logged period."),
]

PSI_ENABLE_HELP = {
    "disabled_by_default": [
        "This kernel was built with PSI but turned off by default "
        "(CONFIG_PSI_DEFAULT_DISABLED=y, the RHEL/Rocky/Alma default). Enable it "
        "with a kernel boot parameter and a reboot:",
        "  RHEL family:   grubby --update-kernel=ALL --args=psi=1",
        "  Debian/Ubuntu: add psi=1 to GRUB_CMDLINE_LINUX in /etc/default/grub, "
        "then update-grub",
        "  then reboot, and verify with: cat /proc/pressure/cpu",
        "PSI cannot be enabled at runtime. Its overhead is small, but schedule "
        "the reboot through the customer's change process.",
    ],
    "disabled_by_cmdline": [
        "PSI was explicitly disabled with psi=0 on the kernel command line. "
        "Remove it (RHEL family: grubby --update-kernel=ALL --remove-args=psi) "
        "and reboot.",
    ],
    "not_built": [
        "This kernel was built without CONFIG_PSI (it needs Linux 4.20 or later "
        "with CONFIG_PSI=y). Only a different kernel build can provide PSI.",
    ],
    "unknown": [
        "/proc/pressure is absent and the kernel configuration was not readable. "
        "If the kernel is 4.20 or later, try booting with psi=1.",
    ],
}


class Finding:
    def __init__(self, severity, area, title, details=None, next_steps=None):
        self.severity = severity
        self.area = area
        self.title = title
        self.details = details or []
        self.next_steps = next_steps or []

    def as_dict(self):
        return {"severity": self.severity, "area": self.area,
                "title": self.title, "details": self.details,
                "next_steps": self.next_steps}


# ---------------------------------------------------------------------------
# Loading
# ---------------------------------------------------------------------------

def load_bundle(path):
    files = {}
    if os.path.isdir(path):
        for name in os.listdir(path):
            full = os.path.join(path, name)
            if os.path.isfile(full) and not os.path.islink(full):
                with open(full, "rb") as handle:
                    files[name] = handle.read()
    else:
        with tarfile.open(path, "r:*") as archive:
            for member in archive.getmembers():
                # Only regular files, flattened to their base name. Nothing is
                # written to disk, so path traversal in member names is inert.
                if not member.isfile() or member.size > MAX_MEMBER_BYTES:
                    continue
                extracted = archive.extractfile(member)
                if extracted is not None:
                    files[os.path.basename(member.name)] = extracted.read()
    return {name: data.decode("utf-8", "replace") for name, data in files.items()}


def parse_system(text):
    system = {}
    for line in text.splitlines():
        key, separator, value = line.partition("=")
        if separator and re.match(r"^[A-Za-z_]+$", key):
            system[key] = value.strip().strip('"')
    return system


def parse_events(text):
    events, invalid = [], 0
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            event = json.loads(line)
        except ValueError:
            invalid += 1
            continue
        if isinstance(event, dict) and "resource" in event:
            events.append(event)
    return events, invalid


def parse_time(value):
    try:
        return datetime.datetime.strptime(value, "%Y-%m-%dT%H:%M:%S.%fZ")
    except (TypeError, ValueError):
        return None


KERNEL_TIME = re.compile(
    r"^\[?(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d)[,.]?\d*\s*([+-]\d\d):?(\d\d)?")


def kernel_line_time(line):
    """UTC time of a `dmesg --time-format iso` line, or None."""
    match = KERNEL_TIME.match(line)
    if not match:
        return None
    try:
        local = datetime.datetime.strptime(match.group(1), "%Y-%m-%dT%H:%M:%S")
    except ValueError:
        return None
    hours = int(match.group(2))
    minutes = int(match.group(3) or 0) * (1 if hours >= 0 else -1)
    return local - datetime.timedelta(hours=hours, minutes=minutes)


def block_topology(text):
    """Map each block device kernel name to its name and to the base devices
    (disks, loop devices) it is stacked on, using lsblk -J output."""
    try:
        tree = json.loads(text).get("blockdevices", [])
    except (ValueError, AttributeError):
        return {}
    info = {}

    def visit(node, ancestors):
        kname = node.get("kname") or node.get("name")
        entry = info.setdefault(kname, {"name": node.get("name"),
                                        "type": node.get("type"),
                                        "mountpoint": node.get("mountpoint"),
                                        "backing": set()})
        # lsblk prints stacked devices as children of the devices they use, so
        # the root of each path is a base device this one is built on.
        if ancestors:
            entry["backing"].add(ancestors[0])
        for child in node.get("children") or []:
            visit(child, ancestors + [kname])

    for root in tree:
        visit(root, [])
    return info


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def percentile(values, fraction):
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(round(fraction * (len(ordered) - 1)))))
    return ordered[index]


def classify_wait_channel(channel):
    if not channel:
        return "hidden", ("wait channel not visible (collect as root to see it)")
    for needles, category, meaning in WAIT_CHANNEL_HINTS:
        if any(needle in channel for needle in needles):
            return category, meaning
    return "other", "kernel wait channel " + channel


def episodes(events, is_bad):
    """Collapse consecutive bad intervals into (start, end, count, peak) runs."""
    runs, current = [], None
    for event in events:
        if is_bad(event):
            if current is None:
                current = {"start": event.get("timestamp"), "count": 0,
                           "peak": event, "conditions": collections.Counter()}
            current["end"] = event.get("timestamp")
            current["count"] += 1
            current["conditions"][event.get("condition")] += 1
            if event.get("severity", 0) > current["peak"].get("severity", 0):
                current["peak"] = event
        elif current is not None:
            runs.append(current)
            current = None
    if current is not None:
        runs.append(current)
    return runs


def clock(timestamp):
    return timestamp[11:19] if timestamp and len(timestamp) >= 19 else "?"


def human_bytes(value):
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if abs(value) < 1024 or unit == "TiB":
            return "%.1f %s" % (value, unit)
        value /= 1024.0
    return "%.1f TiB" % value


def plural(count, word):
    if count == 1:
        return "%d %s" % (count, word)
    return "%d %s%s" % (count, word, "es" if word.endswith("s") else "s")


# ---------------------------------------------------------------------------
# Analysis
# ---------------------------------------------------------------------------

class Analysis:
    def __init__(self, files):
        self.files = files
        self.system = parse_system(files.get("system.txt", ""))
        self.events, self.invalid_lines = parse_events(files.get("events.jsonl", ""))
        self.topology = block_topology(files.get("lsblk.json", ""))
        self.findings = []
        self.coverage = []
        self.facts = {}
        self.timeline = []
        self.window_start = None
        self.by_resource = collections.defaultdict(list)
        for event in self.events:
            self.by_resource[event["resource"]].append(event)

    def add(self, *args, **kwargs):
        self.findings.append(Finding(*args, **kwargs))

    def run(self):
        self.check_coverage()
        self.check_cpu()
        self.check_memory()
        self.check_io()
        self.check_network()
        self.check_kernel_log()
        self.correlate()
        self.findings.sort(key=lambda item: RANK[item.severity])
        self.timeline.sort(key=lambda item: item[0] or "")
        return self

    # -- coverage ---------------------------------------------------------

    def check_coverage(self):
        system = self.system
        times = [parse_time(e.get("timestamp")) for e in self.by_resource["cpu"]]
        times = [t for t in times if t]
        self.window_start = min(times) if times else None
        self.facts["window_start"] = min(times).isoformat() + "Z" if times else None
        self.facts["window_end"] = max(times).isoformat() + "Z" if times else None
        self.facts["intervals"] = len(self.by_resource["cpu"])
        requested = int(system.get("duration_seconds", "0") or 0)

        if not self.events:
            self.coverage.append("No interval events were recorded; the daemon "
                                 "may have failed. See daemon.stderr.")
        elif requested and self.facts["intervals"] < requested * 0.8:
            self.coverage.append(
                "Only %d one-second CPU intervals were recorded for a %ds window; "
                "sampling had gaps or the daemon stopped early."
                % (self.facts["intervals"], requested))
        if self.invalid_lines:
            self.coverage.append("%s in events.jsonl could not be parsed."
                                 % plural(self.invalid_lines, "line"))
        if system.get("uid") not in (None, "0"):
            self.coverage.append(
                "Collected without root (uid %s): threads of other users and "
                "their wait channels may be missing." % system.get("uid"))
        if "hidepid" in system.get("proc_mount", ""):
            self.coverage.append("/proc is mounted with hidepid; task "
                                 "attribution only covers visible processes.")
        for name in ("daemon.stderr", "collection-errors.txt"):
            text = self.files.get(name, "").strip()
            if text:
                self.coverage.append("%s: %s" % (name, " | ".join(text.splitlines()[:5])))
        if system.get("ignored_ib_ports"):
            self.coverage.append("InfiniBand ports excluded on request: "
                                 + system["ignored_ib_ports"])

        psi = system.get("psi_status")
        cpu = self.by_resource["cpu"]
        psi_seen = any(e.get("evidence", {}).get("psi_available") for e in cpu)
        if psi is None:
            psi = "enabled" if psi_seen else "unknown"
        self.facts["psi_status"] = psi
        if psi != "enabled" and not psi_seen:
            self.add(INFO, "coverage",
                     "PSI (pressure stall information) is not available",
                     ["Without PSI, CPU events report condition=unavailable. "
                      "Memory health falls back to reclaim, swap and OOM "
                      "counters, and I/O health to block statistics; both still "
                      "work. System-wide stall percentages for CPU, memory and "
                      "I/O are missing."],
                     PSI_ENABLE_HELP.get(psi, PSI_ENABLE_HELP["unknown"]))

    # -- CPU / tasks -------------------------------------------------------

    def check_cpu(self):
        cpu = self.by_resource["cpu"]
        if not cpu:
            return
        stalls = [e["evidence"].get("psi_some_stall_percent", 0.0) for e in cpu
                  if e.get("evidence", {}).get("psi_available")]
        if stalls:
            self.facts["cpu_psi_mean"] = statistics.mean(stalls)
            self.facts["cpu_psi_p95"] = percentile(stalls, 0.95)
            self.facts["cpu_psi_max"] = max(stalls)
            heavy = [s for s in stalls if s >= 25.0]
            for run in episodes(cpu, lambda e: e.get("severity", 0) >= 0.25):
                self.timeline.append((run["start"], "cpu", "CPU stall >= 25%",
                                      run["count"], run["peak"].get("severity")))
            if self.facts["cpu_psi_p95"] >= 25.0 or len(heavy) >= max(3, len(stalls) // 10):
                level = CRITICAL if self.facts["cpu_psi_mean"] >= 50.0 else WARNING
                self.add(level, "cpu", "CPU contention: runnable work waited for CPU",
                         ["PSI cpu some: mean %.1f%%, p95 %.1f%%, max %.1f%% of wall "
                          "time with at least one task waiting for a CPU"
                          % (self.facts["cpu_psi_mean"], self.facts["cpu_psi_p95"],
                             self.facts["cpu_psi_max"]),
                          "%d of %d intervals were at or above 25%%."
                          % (len(heavy), len(stalls))],
                         ["Identify the CPU consumers listed under the top waiting "
                          "threads and check whether the node is oversubscribed or a "
                          "cgroup has a CPU quota (cpu.max).",
                          "Correlate with application latency for the same "
                          "timestamps."])

        # Attribution snapshots repeat for several intervals; keep fresh ones.
        fresh = [e for e in cpu if e.get("selection", {}).get("attribution_available")
                 and e["selection"].get("attribution_age_seconds", 1.0) < 1e-6]
        self.facts["attribution_snapshots"] = len(fresh)
        if not fresh:
            return
        interval = statistics.mean(
            e["selection"].get("attribution_interval_seconds", 5.0) for e in fresh)
        self.facts["observed_threads"] = max(
            e["selection"].get("observed_threads", 0) for e in fresh)

        blocked = collections.defaultdict(lambda: {"snapshots": 0, "pids": set(),
                                                   "first": None, "last": None})
        blocked_series = collections.defaultdict(list)
        waits = collections.defaultdict(lambda: {"wait_ms": 0.0, "cpu_ms": 0.0,
                                                 "worst_percent": 0.0, "pids": set()})
        for index, event in enumerate(fresh):
            seen = set()
            for thread in event.get("affected_threads", []):
                command = thread.get("command", "?")
                if thread.get("state") == "D":
                    key = (command, thread.get("wait_channel", ""))
                    entry = blocked[key]
                    if key not in seen:
                        entry["snapshots"] += 1
                        seen.add(key)
                    entry["pids"].add(thread.get("pid"))
                    entry["first"] = entry["first"] or event.get("timestamp")
                    entry["last"] = event.get("timestamp")
                    blocked_series[(thread.get("tid"), key)].append(index)
                entry = waits[command]
                entry["wait_ms"] += thread.get("runqueue_wait_ms", 0.0)
                entry["cpu_ms"] += thread.get("cpu_ms", 0.0)
                entry["worst_percent"] = max(entry["worst_percent"],
                                             thread.get("runnable_wait_percent", 0.0))
                entry["pids"].add(thread.get("pid"))

        # The longest consecutive run of snapshots in which one thread stayed in D.
        longest = {}
        for (tid, key), indexes in blocked_series.items():
            best = run = 1
            for previous, current in zip(indexes, indexes[1:]):
                run = run + 1 if current == previous + 1 else 1
                best = max(best, run)
            longest[key] = max(longest.get(key, 0), best)

        self.facts["blocked_groups"] = []
        if blocked:
            categories = collections.Counter()
            details = []
            ordered = sorted(blocked.items(), key=lambda item: -item[1]["snapshots"])
            for (command, channel), entry in ordered[:10]:
                category, meaning = classify_wait_channel(channel)
                categories[category] += entry["snapshots"]
                stuck = longest.get((command, channel), 1) * interval
                details.append(
                    "%s (%s) in D state on '%s' in %d of %d snapshots "
                    "between %s and %s; one thread stayed blocked for at least "
                    "%.0fs. Meaning: %s."
                    % (command, plural(len(entry["pids"]), "process"),
                       channel or "?", entry["snapshots"], len(fresh),
                       clock(entry["first"]), clock(entry["last"]),
                       stuck, meaning))
                self.facts["blocked_groups"].append(
                    {"command": command, "wait_channel": channel,
                     "category": category, "snapshots": entry["snapshots"],
                     "longest_seconds": stuck})
            self.facts["blocked_categories"] = dict(categories)
            worst_stuck = max(longest.values()) * interval
            level = CRITICAL if worst_stuck >= 15 else (
                WARNING if worst_stuck >= 2 * interval else INFO)
            steps = []
            if categories.get("fuse"):
                steps.append("FUSE waits: check the FUSE client daemon for the "
                             "affected mount (its logs, CPU and connectivity to "
                             "its storage backend). See mounts.txt for fuse mounts.")
            if categories.get("nfs"):
                steps.append("NFS waits: check `nfsstat -c`, `/proc/self/mountstats` "
                             "and the server's responsiveness.")
            if categories.get("block"):
                steps.append("Block I/O waits: compare with the I/O device findings "
                             "below for the same time window.")
            if categories.get("lock"):
                steps.append("Lock waits: capture kernel stacks of the blocked "
                             "threads (`cat /proc/PID/task/TID/stack` as root) to "
                             "find the lock holder.")
            if categories.get("hidden"):
                steps.append("Re-run the collection as root to reveal hidden wait "
                             "channels.")
            steps.append("For any thread stuck for minutes, capture "
                         "`echo w > /proc/sysrq-trigger` (as root) to log all "
                         "blocked task stacks to the kernel log.")
            self.add(level, "cpu",
                     "Threads in uninterruptible sleep (D state)", details, steps)
            for (command, channel), entry in ordered[:5]:
                self.timeline.append((entry["first"], "tasks",
                                      "%s blocked on %s" % (command, channel or "?"),
                                      entry["snapshots"], None))

        ranked = sorted(waits.items(), key=lambda item: -item[1]["wait_ms"])[:8]
        self.facts["top_waiters"] = [
            {"command": command, "runqueue_wait_ms": round(entry["wait_ms"], 1),
             "cpu_ms": round(entry["cpu_ms"], 1),
             "worst_runnable_wait_percent": round(entry["worst_percent"], 1),
             "processes": len(entry["pids"])}
            for command, entry in ranked if entry["wait_ms"] > 0]
        starved = [(command, entry) for command, entry in ranked
                   if entry["worst_percent"] >= 50.0
                   and entry["wait_ms"] >= 100.0 * len(fresh)]
        if starved and not stalls:
            self.add(WARNING, "cpu",
                     "CPU starvation in task attribution (PSI unavailable)",
                     ["%s waited %.0f ms for CPU in total; in its worst snapshot a "
                      "thread spent %.0f%% of its runnable time waiting."
                      % (command, entry["wait_ms"], entry["worst_percent"])
                      for command, entry in starved[:5]],
                     ["Check CPU utilisation (`mpstat -P ALL 1`) and cgroup CPU "
                      "limits for these processes."])

    # -- memory --------------------------------------------------------------

    def check_memory(self):
        memory = self.by_resource["memory"]
        if not memory:
            self.coverage.append("No memory events (archive from an older "
                                 "Graywatch version).")
            return
        evidence = [e.get("evidence", {}) for e in memory]
        oom = sum(e.get("oom_kills", 0) for e in evidence)
        reclaim = sum(e.get("direct_reclaim_stalls", 0) for e in evidence)
        compaction = sum(e.get("compaction_stalls", 0) for e in evidence)
        swap_in = sum(e.get("swap_in_pages_per_second", 0.0) *
                      m.get("interval_seconds", 1.0)
                      for e, m in zip(evidence, memory))
        major = sum(e.get("major_faults_per_second", 0.0) *
                    m.get("interval_seconds", 1.0) for e, m in zip(evidence, memory))
        min_available = min(e.get("available_percent", 100.0) for e in evidence)
        psi_some = [e.get("psi_some_stall_percent", 0.0) for e in evidence
                    if e.get("psi_available")]
        psi_full = [e.get("psi_full_stall_percent", 0.0) for e in evidence
                    if e.get("psi_available")]
        reasons = collections.Counter(e.get("assessment_reason") for e in evidence)
        conditions = collections.Counter(m.get("condition") for m in memory)
        total = evidence[-1].get("total_kib", 0) * 1024
        self.facts["memory"] = {
            "total": human_bytes(total), "min_available_percent": min_available,
            "oom_kills": oom, "direct_reclaim_stalls": reclaim,
            "swap_in_pages": int(swap_in), "major_faults": int(major),
            "conditions": dict(conditions)}
        for run in episodes(memory, lambda e: e.get("condition") not in QUIET_CONDITIONS):
            self.timeline.append((run["start"], "memory",
                                  "memory %s (%s)" % (
                                      run["peak"].get("condition"),
                                      run["peak"]["evidence"].get("assessment_reason")),
                                  run["count"], run["peak"].get("severity")))

        details = ["%s memory; lowest MemAvailable during the window: %.1f%%."
                   % (human_bytes(total), min_available)]
        if psi_some:
            details.append("PSI memory some: max %.1f%%, full: max %.1f%%."
                           % (max(psi_some), max(psi_full or [0.0])))
        details.append("Direct reclaim stalls: %d, compaction stalls: %d, "
                       "swap-in pages: %d, major faults: %d, OOM kills: %d."
                       % (reclaim, compaction, swap_in, major, oom))
        bad = sum(count for condition, count in conditions.items()
                  if condition not in QUIET_CONDITIONS)
        if oom:
            self.add(CRITICAL, "memory", "OOM killer ran during the window", details,
                     ["Find the killed process in kernel.log ('Killed process') "
                      "and the cgroup that hit its limit ('oom-kill:' line).",
                      "Check memory.max / memory.high of the affected cgroup and "
                      "the workload's working-set size."])
        elif bad:
            level = WARNING if bad >= 3 or reasons.get("memory_stall_full") else INFO
            self.add(level, "memory",
                     "Memory pressure in %d of %d intervals" % (bad, len(memory)),
                     details + ["Reasons: " + ", ".join(
                         "%s=%d" % item for item in reasons.most_common()
                         if item[0] != "no_pressure")],
                     ["Direct reclaim means allocating tasks (possibly the "
                      "application) had to free memory themselves, which adds "
                      "latency. Check for page-cache-heavy jobs and large dirty "
                      "page backlogs (Dirty/Writeback in meminfo-*.txt).",
                      "If swap-in is non-zero, the working set does not fit in "
                      "memory: identify the largest consumers "
                      "(`ps aux --sort=-rss | head`)."])

    # -- block I/O ---------------------------------------------------------

    def device_label(self, name):
        entry = self.topology.get(name)
        if not entry:
            return name
        label = name
        if entry.get("name") and entry["name"] != name:
            label += " (%s)" % entry["name"]
        backing = sorted(entry["backing"] - {name})
        if backing:
            label += " on " + ", ".join(backing)
        return label

    def check_io(self):
        devices = collections.defaultdict(list)
        for event in self.by_resource["io"]:
            devices[event.get("device", {}).get("name", "?")].append(event)
        summary = []
        unhealthy = {}
        for name, events in sorted(devices.items()):
            active = [e for e in events if e.get("condition") != "idle"]
            if not active:
                summary.append({"device": name, "active_intervals": 0})
                continue
            evidence = [e["evidence"] for e in active]
            latency = [e.get("maximum_average_latency_ms", 0.0) for e in evidence]
            row = {
                "device": name, "label": self.device_label(name),
                "active_intervals": len(active),
                "conditions": dict(collections.Counter(e["condition"] for e in active)),
                "latency_p50_ms": percentile(latency, 0.50),
                "latency_p95_ms": percentile(latency, 0.95),
                "latency_max_ms": max(latency),
                "busy_max_percent": max(e.get("busy_percent", 0.0) for e in evidence),
                "queue_depth_max": max(e.get("average_queue_depth", 0.0) for e in evidence),
                "read_bytes": sum(e.get("read_bytes_per_second", 0.0) *
                                  a.get("interval_seconds", 1.0)
                                  for e, a in zip(evidence, active)),
                "write_bytes": sum(e.get("write_bytes_per_second", 0.0) *
                                   a.get("interval_seconds", 1.0)
                                   for e, a in zip(evidence, active)),
            }
            summary.append(row)
            bad = [e for e in active if e.get("condition") not in QUIET_CONDITIONS]
            if bad:
                unhealthy[name] = (row, bad)
            for run in episodes(events, lambda e: e.get("condition") not in QUIET_CONDITIONS):
                self.timeline.append((run["start"], "io",
                                      "%s %s (%.1f ms)" % (
                                          name, run["peak"].get("condition"),
                                          run["peak"]["evidence"].get(
                                              "maximum_average_latency_ms", 0.0)),
                                      run["count"], run["peak"].get("severity")))
        self.facts["io_devices"] = summary
        self.facts["io_unhealthy"] = sorted(unhealthy)

        for name, (row, bad) in sorted(unhealthy.items(),
                                       key=lambda item: -len(item[1][1])):
            conditions = collections.Counter(e["condition"] for e in bad)
            reasons = collections.Counter(e["evidence"].get("assessment_reason")
                                          for e in bad)
            level = CRITICAL if conditions.get("degraded", 0) >= 3 else (
                WARNING if conditions.get("degraded") or len(bad) >= 3 else INFO)
            details = [
                "%s of %d active intervals non-healthy: %s." % (
                    len(bad), row["active_intervals"],
                    ", ".join("%s=%d" % item for item in conditions.most_common())),
                "Average latency per interval: p50 %.2f ms, p95 %.2f ms, max %.2f ms. "
                "Max busy %.0f%%, max average queue depth %.1f." % (
                    row["latency_p50_ms"], row["latency_p95_ms"],
                    row["latency_max_ms"], row["busy_max_percent"],
                    row["queue_depth_max"]),
                "Reasons: " + ", ".join("%s=%d" % item for item in reasons.most_common()),
            ]
            steps = []
            entry = self.topology.get(name, {})
            backing = sorted(entry.get("backing", set()) - {name})
            if backing:
                healthy_backing = [b for b in backing if b not in unhealthy]
                if healthy_backing and len(healthy_backing) == len(backing):
                    steps.append(
                        "The underlying device(s) %s stayed healthy, so the delay "
                        "is added in the stacking layer (device-mapper, LVM, "
                        "multipath, dm-crypt, RAID). Inspect `dmsetup table` and "
                        "`dmsetup status`." % ", ".join(backing))
                else:
                    steps.append("Backing device(s) %s were also unhealthy; start "
                                 "with the physical device." % ", ".join(backing))
            if reasons.get("busy_with_queue") or reasons.get("queueing"):
                steps.append("Queueing with a busy device means demand exceeds the "
                             "device's capacity. Identify the I/O sources "
                             "(`iotop -o`, `pidstat -d 1`) before suspecting "
                             "hardware.")
            if reasons.get("absolute_latency") or reasons.get("elevated_latency"):
                steps.append("High latency: check the device's health "
                             "(`smartctl -a`, `nvme smart-log`), kernel.log storage "
                             "errors, and run the optional eBPF collector to "
                             "separate queueing from device service time.")
            self.add(level, "io", "Block device %s: %s" % (
                row["label"], ", ".join(sorted(conditions))), details, steps)

    # -- network -------------------------------------------------------------

    def check_network(self):
        tcp = [e for e in self.by_resource["network"]
               if e.get("interface", {}).get("kind") == "tcp"]
        if tcp:
            evidence = [e["evidence"] for e in tcp]
            segments = sum(e.get("out_segments", 0) for e in evidence)
            retrans = sum(e.get("retransmitted_segments", 0) for e in evidence)
            errors = sum(e.get("input_errors", 0) for e in evidence)
            resets = sum(e.get("output_resets", 0) for e in evidence)
            unstable = [e for e in tcp if e.get("condition") not in QUIET_CONDITIONS]
            ratio = retrans / segments * 100.0 if segments else 0.0
            self.facts["tcp"] = {"out_segments": segments, "retransmitted": retrans,
                                 "retransmit_percent": ratio, "input_errors": errors,
                                 "resets_sent": resets, "bad_intervals": len(unstable)}
            for run in episodes(tcp, lambda e: e.get("condition") not in QUIET_CONDITIONS):
                self.timeline.append((run["start"], "tcp",
                                      "TCP %s" % run["peak"].get("condition"),
                                      run["count"], run["peak"].get("severity")))
            if unstable or errors:
                level = WARNING if len(unstable) >= 3 or errors else INFO
                self.add(level, "network",
                         "TCP retransmissions or errors in %d of %d intervals"
                         % (len(unstable), len(tcp)),
                         ["%d of %d segments retransmitted (%.2f%%), %d input "
                          "errors, %d resets sent." % (retrans, segments, ratio,
                                                       errors, resets),
                          "TCP counters are system-wide; they do not identify the "
                          "connection or peer."],
                         ["Find the affected connections with `ss -ti` (look for "
                          "high retrans and rto) and check NIC counters with "
                          "`ip -s link` and `ethtool -S IFACE`.",
                          "Input errors usually mean checksum or malformed segments: "
                          "check the NIC, cabling and MTU consistency."])

        ports = collections.defaultdict(list)
        for event in self.by_resource["network"]:
            interface = event.get("interface", {})
            if interface.get("kind") == "infiniband":
                ports["%s/%s" % (interface.get("device"), interface.get("port"))].append(event)
        self.facts["ib_ports"] = []
        inactive = []
        for name, events in sorted(ports.items()):
            conditions = collections.Counter(e.get("condition") for e in events)
            evidence = [e["evidence"] for e in events]
            states = []
            for e in evidence:
                if not states or states[-1] != e.get("state"):
                    states.append(e.get("state"))
            totals = {key: sum(e.get(key, 0) for e in evidence)
                      for key in ("receive_errors", "transmit_discards",
                                  "symbol_errors", "link_downed", "transmit_wait")}
            row = {"port": name, "rate": evidence[-1].get("rate"),
                   "states": states, "conditions": dict(conditions), **totals}
            self.facts["ib_ports"].append(row)
            for run in episodes(events, lambda e: e.get("condition") not in QUIET_CONDITIONS):
                self.timeline.append((run["start"], "infiniband",
                                      "%s %s" % (name, run["peak"].get("condition")),
                                      run["count"], run["peak"].get("severity")))
            if set(conditions) == {"inactive"}:
                inactive.append(name)
                continue
            counts = ", ".join("%s=%d" % item for item in sorted(totals.items()) if item[1])
            details = ["States during window: %s; rate %s." % (" -> ".join(states),
                                                               row["rate"])]
            if counts:
                details.append("Counter increments: " + counts + ".")
            if conditions.get("link_down"):
                self.add(CRITICAL, "network", "InfiniBand port %s link down" % name,
                         details,
                         ["The port carried traffic before (or flapped) and is now "
                          "down. Check `ibstat`, the cable/transceiver and the switch "
                          "port. If the port is intentionally unused, rerun with "
                          "--ignore-ib-port %s." % name])
            elif conditions.get("degraded"):
                self.add(WARNING, "network", "InfiniBand port %s errors" % name, details,
                         ["Symbol and receive errors point at the physical link "
                          "(cable, transceiver, port). Check `perfquery -x` on both "
                          "ends and the switch-side counters."])
            elif conditions.get("congested"):
                busy = conditions["congested"]
                self.add(WARNING if busy >= 3 else INFO, "network",
                         "InfiniBand port %s congested in %d intervals" % (name, busy),
                         details,
                         ["port_xmit_wait means the port had data to send but no "
                          "credits from the link partner: congestion in the fabric "
                          "or a slow receiver. Check the switch for hotspots."])
        if inactive:
            self.add(INFO, "network", "Unused InfiniBand ports ignored",
                     ["%s down for the whole window and never carried traffic since "
                      "boot; not treated as a failure." % ", ".join(inactive)],
                     ["If the application is configured to use one of these "
                      "ports, that configuration (or the cabling) is the problem."])

    # -- kernel log ----------------------------------------------------------

    def check_kernel_log(self):
        text = self.files.get("kernel.log")
        if text is None:
            self.coverage.append("No kernel log in the archive (collected with "
                                 "--no-kernel-log or an older collector).")
            return
        if not text.strip():
            self.coverage.append("Kernel log empty or unreadable (dmesg_restrict "
                                 "requires root).")
            return
        # Lines older than this are history, not evidence about the window.
        start = self.window_start
        recent_since = start - datetime.timedelta(minutes=15) if start else None
        hits = collections.defaultdict(lambda: {"recent": [], "old": []})
        for line in text.splitlines():
            for name, level, pattern, advice in KERNEL_LOG_PATTERNS:
                if pattern.search(line):
                    when = kernel_line_time(line)
                    old = recent_since is not None and when is not None and when < recent_since
                    hits[name]["old" if old else "recent"].append(line.strip())
                    break
        self.facts["kernel_log_matches"] = {
            name: len(lines["recent"]) + len(lines["old"])
            for name, lines in hits.items()}
        for name, level, pattern, advice in KERNEL_LOG_PATTERNS:
            if name not in hits:
                continue
            recent, old = hits[name]["recent"], hits[name]["old"]
            label = name.replace("_", " ")
            if recent:
                details = ["Most recent:"] + ["  " + line[:200] for line in recent[-3:]]
                if old:
                    details.append("Plus %s from more than 15 minutes before the "
                                   "window." % plural(len(old), "older line"))
                self.add(level, "kernel log", "Kernel log: %s (%s)" % (
                    label, plural(len(recent), "line")), details,
                    [advice] + ([] if recent_since else [
                        "Kernel log timestamps could not be compared with the "
                        "window; check that these lines belong to the incident."]))
            else:
                self.add(INFO, "kernel log",
                         "Kernel log: earlier %s (%s, before the window)" % (
                             label, plural(len(old), "line")),
                         ["Most recent: " + old[-1][:200]],
                         ["History only: relevant if the incident started at that "
                          "time. " + advice])

    # -- correlation -------------------------------------------------------

    def correlate(self):
        categories = self.facts.get("blocked_categories", {})
        unhealthy_io = self.facts.get("io_unhealthy", [])
        ib_bad = [f for f in self.findings if f.area == "network"
                  and f.severity != INFO]
        memory = self.facts.get("memory", {})
        conclusions = []
        if categories.get("fuse") or categories.get("nfs"):
            layer = "FUSE" if categories.get("fuse") else "NFS"
            if not unhealthy_io:
                conclusions.append(
                    "Threads were blocked in %s while every local block device "
                    "stayed healthy: the delay is above the local block layer, in "
                    "the %s client, its storage service or the network path to it."
                    % (layer, layer))
            if ib_bad:
                conclusions.append(
                    "Network/InfiniBand problems occurred at the same time as %s "
                    "waits; the network path to the storage service is a likely "
                    "contributor." % layer)
        if categories.get("block") and unhealthy_io:
            conclusions.append(
                "Threads waited on block I/O and %s showed degraded or congested "
                "service: start with the storage device findings."
                % ", ".join(unhealthy_io))
        if categories.get("block") and not unhealthy_io:
            conclusions.append(
                "Threads waited on block I/O but no device crossed the latency "
                "guardrails; look at short bursts (the eBPF collector) or at "
                "dirty-page throttling.")
        memory_significant = any(f.area == "memory" and f.severity != INFO
                                 for f in self.findings)
        if memory_significant and (unhealthy_io or categories.get("memory")):
            conclusions.append(
                "Memory reclaim coincided with I/O waits; page-cache pressure can "
                "turn into extra read and writeback I/O.")
        self.facts["conclusions"] = conclusions


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------

def render(analysis, markdown):
    out = []
    system, facts = analysis.system, analysis.facts

    def heading(text, level=2):
        if markdown:
            out.append("%s %s" % ("#" * level, text))
        else:
            out.append(text)
            out.append(("=" if level == 1 else "-") * len(text))
        out.append("")

    def bullet(text, indent=0):
        out.append("%s%s %s" % ("  " * indent, "-" if markdown else "*", text))

    worst = analysis.findings[0].severity if analysis.findings else INFO
    actionable = [f for f in analysis.findings if f.severity != INFO]
    heading("Graywatch support summary: %s" % system.get("hostname", "unknown host"), 1)
    os_name = system.get("os_PRETTY_NAME", "unknown OS")
    out.append("Kernel %s on %s, %s CPUs. Collected %s for %ss as uid %s."
               % (system.get("kernel_release", "?"), os_name, system.get("cpus", "?"),
                  system.get("collected_utc", "?"), system.get("duration_seconds", "?"),
                  system.get("uid", "?")))
    out.append("")
    if actionable:
        out.append("Verdict: %s. %s %s attention (%s critical)."
                   % (worst.upper(), plural(len(actionable), "finding"),
                      "needs" if len(actionable) == 1 else "need",
                      sum(1 for f in actionable if f.severity == CRITICAL)))
    else:
        out.append("Verdict: no degradation detected in this window. If the "
                   "incident was not happening during collection, collect again "
                   "while it is.")
    out.append("")

    if facts.get("conclusions"):
        heading("Assessment")
        for line in facts["conclusions"]:
            bullet(line)
        out.append("")

    heading("Findings")
    if not analysis.findings:
        out.append("None.")
        out.append("")
    for number, finding in enumerate(analysis.findings, 1):
        title = "%d. [%s] %s" % (number, finding.severity.upper(), finding.title)
        out.append("**%s**" % title if markdown else title)
        for line in finding.details:
            bullet(line, 1)
        if finding.next_steps:
            out.append("   Next steps:" if not markdown else "  Next steps:")
            for line in finding.next_steps:
                bullet(line, 2)
        out.append("")

    if analysis.timeline:
        heading("Timeline (UTC)")
        for start, area, text, count, severity in analysis.timeline[:25]:
            suffix = (", peak severity %.2f" % severity) if severity is not None else ""
            bullet("%s %-10s %s (%s%s)" % (clock(start), area, text,
                                           plural(count, "interval" if area != "tasks"
                                                  else "snapshot"), suffix))
        if len(analysis.timeline) > 25:
            bullet("... %d more episodes" % (len(analysis.timeline) - 25))
        out.append("")

    heading("Resource overview")
    if "cpu_psi_mean" in facts:
        bullet("CPU PSI some: mean %.1f%%, p95 %.1f%%, max %.1f%%."
               % (facts["cpu_psi_mean"], facts["cpu_psi_p95"], facts["cpu_psi_max"]))
    else:
        bullet("CPU PSI: %s." % facts.get("psi_status", "unknown"))
    if facts.get("observed_threads"):
        bullet("Task attribution: %d snapshots, up to %d readable threads."
               % (facts.get("attribution_snapshots", 0), facts["observed_threads"]))
    for waiter in facts.get("top_waiters", [])[:5]:
        bullet("Top CPU waiter %s: %.0f ms run-queue wait, %.0f ms on CPU, worst "
               "%.0f%% of runnable time waiting (%s)."
               % (waiter["command"], waiter["runqueue_wait_ms"], waiter["cpu_ms"],
                  waiter["worst_runnable_wait_percent"],
                  plural(waiter["processes"], "process")), 1)
    if "memory" in facts:
        memory = facts["memory"]
        bullet("Memory %s: lowest available %.1f%%, %d direct reclaim stalls, "
               "%d swap-in pages, %d OOM kills."
               % (memory["total"], memory["min_available_percent"],
                  memory["direct_reclaim_stalls"], memory["swap_in_pages"],
                  memory["oom_kills"]))
    idle = [row["device"] for row in facts.get("io_devices", [])
            if not row["active_intervals"]]
    for row in facts.get("io_devices", []):
        if not row["active_intervals"]:
            continue
        bullet("Disk %s: %d active intervals, latency p50 %.2f / p95 %.2f / max "
               "%.2f ms, busy max %.0f%%, read %s, written %s."
               % (row["label"], row["active_intervals"], row["latency_p50_ms"],
                  row["latency_p95_ms"], row["latency_max_ms"],
                  row["busy_max_percent"], human_bytes(row["read_bytes"]),
                  human_bytes(row["write_bytes"])))
    if idle:
        bullet("Idle devices: " + ", ".join(idle) + ".")
    if "tcp" in facts:
        tcp = facts["tcp"]
        bullet("TCP: %d segments out, %.2f%% retransmitted, %d input errors."
               % (tcp["out_segments"], tcp["retransmit_percent"], tcp["input_errors"]))
    for port in facts.get("ib_ports", []):
        bullet("InfiniBand %s (%s): %s." % (
            port["port"], port["rate"],
            ", ".join("%s=%d" % item for item in sorted(port["conditions"].items()))))
    out.append("")

    heading("Collection coverage")
    bullet("PSI status: %s." % facts.get("psi_status", "unknown"))
    if facts.get("window_start"):
        bullet("Sampled %s to %s (%d intervals)."
               % (facts["window_start"], facts["window_end"], facts["intervals"]))
    for line in analysis.coverage:
        bullet(line)
    out.append("")
    out.append("Thresholds in this summary are triage heuristics. The daemon's "
               "per-interval condition in events.jsonl is the authoritative "
               "classification.")
    return "\n".join(out).rstrip() + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("archive", help="support .tar.gz or extracted directory")
    parser.add_argument("--format", choices=("text", "markdown", "json"),
                        default="text")
    parser.add_argument("--output", help="write the report here instead of stdout")
    args = parser.parse_args(argv)
    try:
        files = load_bundle(args.archive)
    except (OSError, tarfile.TarError) as error:
        print("graywatch-report: cannot read %s: %s" % (args.archive, error),
              file=sys.stderr)
        return 1
    if "events.jsonl" not in files and "system.txt" not in files:
        print("graywatch-report: %s is not a Graywatch support archive"
              % args.archive, file=sys.stderr)
        return 1
    analysis = Analysis(files).run()
    if args.format == "json":
        def default(value):
            return sorted(value) if isinstance(value, set) else str(value)
        report = json.dumps({"system": analysis.system, "facts": analysis.facts,
                             "findings": [f.as_dict() for f in analysis.findings],
                             "coverage": analysis.coverage,
                             "timeline": analysis.timeline},
                            indent=2, default=default) + "\n"
    else:
        report = render(analysis, args.format == "markdown")
    if args.output:
        with open(args.output, "w", encoding="utf-8") as handle:
            handle.write(report)
    else:
        sys.stdout.write(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
