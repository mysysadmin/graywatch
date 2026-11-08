# Graywatch

Graywatch is an experimental local Linux degradation detector. It observes the
quality of a process's execution environment and exposes versioned,
evidence-backed health events directly to applications. It runs locally: no
Prometheus, Grafana, remote collector, or monitoring server is required.

The current implementation provides:

- CPU, memory, and I/O pressure from Linux PSI when the kernel exposes it;
- memory health from reclaim, swap, refault, and OOM-kill counters, with or
  without PSI;
- per-thread scheduler delay and uninterruptible (`D`-state) attribution;
- per-device block activity, queueing, saturation, and absolute-latency health;
- an optional eBPF helper with FUSE request latency per opcode, block queue
  versus device time, `D`-state durations with kernel stacks, per-cgroup
  run-queue latency, per-connection TCP retransmits, and direct-reclaim
  stalls per process;
- system-wide TCP retransmission/error intervals;
- InfiniBand link state, rate, congestion, and error-counter intervals;
- current-state queries and resource-filtered subscriptions over a local Unix
  socket; and
- a stateless multi-node support snapshot over SSH; and
- a portable incident bundle plus a report tool (`graywatch-report`, C++, no
  Python) that turns its archive into a summary for support engineers.

PSI is optional. On a kernel where `/proc/pressure` is unavailable, CPU events
explicitly report `condition: unavailable` while task, memory, block, TCP, and
InfiniBand evidence continues to work. See [Enabling PSI](#enabling-psi).

### Enabling PSI

PSI requires Linux 4.20 or later built with `CONFIG_PSI=y`. Check for it with:

```sh
cat /proc/pressure/cpu
grep -E 'CONFIG_PSI(_DEFAULT_DISABLED)?[= ]' /boot/config-$(uname -r)
```

RHEL, Rocky Linux, AlmaLinux, and some elrepo kernels set
`CONFIG_PSI_DEFAULT_DISABLED=y`: PSI is compiled in but turned off until the
kernel boots with `psi=1`. It cannot be switched on at runtime, so enabling it
needs a reboot:

```sh
# RHEL family
sudo grubby --update-kernel=ALL --args=psi=1
# Debian/Ubuntu: add psi=1 to GRUB_CMDLINE_LINUX in /etc/default/grub, then
sudo update-grub
sudo reboot
# afterwards
cat /proc/pressure/cpu /proc/pressure/memory /proc/pressure/io
```

A `psi=0` argument disables PSI explicitly and must be removed instead
(`grubby --update-kernel=ALL --remove-args=psi`). Distribution defaults
differ and change between releases, so check rather than assume. The support
collector records which of these cases applies as `psi_status` in `system.txt`, and the report tool prints the
matching instructions.

With PSI enabled, Graywatch additionally reports:

- CPU `some`: the share of time at least one runnable task waited for a CPU;
- memory `some`/`full`: the share of time some or all non-idle tasks were
  stalled on memory reclaim, refaults, or swap-in; and
- I/O `some`/`full`: the share of time tasks waited for I/O, attached to every
  I/O device event as system-wide context.

Without PSI these stall percentages are missing, but memory events still detect
direct reclaim, swap-in, low available memory, and OOM kills from `/proc/vmstat`
and `/proc/meminfo`.

## Build

Requirements:

- Linux with procfs and sysfs mounted
- CMake 3.20 or newer
- Ninja
- A C++20 compiler
- Git (including submodule support)

On Ubuntu 24.04:

```sh
sudo apt update
sudo apt install -y \
  build-essential cmake ninja-build git pkg-config
```

On Fedora, install the equivalent packages:

```sh
sudo dnf install -y gcc-c++ cmake ninja-build git make pkgconf-pkg-config
```

Rocky Linux 9 provides Ninja and the static runtimes in the CRB repository,
which is disabled by default. Enable CRB, install the packages, and verify the
executable before configuring Graywatch:

```sh
sudo dnf install -y dnf-plugins-core
sudo dnf config-manager --set-enabled crb
sudo dnf install -y \
  gcc-c++ cmake ninja-build git make pkgconf-pkg-config \
  glibc-static libstdc++-static
ninja --version
```

If CRB is intentionally unavailable, install a trusted upstream Ninja binary
through the site's normal package-management process. Graywatch's documented
and supported CMake configuration uses `-G Ninja`; do not silently fall back to
a different generator in deployment automation.

The optional eBPF helper additionally requires Clang/LLVM, `bpftool`, ELF
development headers, and zlib development headers. For Ubuntu:

```sh
sudo apt install -y clang llvm bpftool libelf-dev zlib1g-dev
```

For Fedora or Rocky Linux:

```sh
sudo dnf install -y clang llvm bpftool elfutils-libelf-devel zlib-devel
```

Graywatch vendors libbpf as a pinned Git submodule, so a distribution
`libbpf-devel` package is not required for the optional collector.

```sh
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The core daemon and client do not require eBPF or root to build. Initializing
submodules in the standard setup keeps the optional collector ready to build.

## Portable incident bundle

Build an installation-free bundle on any Linux host with Docker. The build
runs in a Rocky 9 container, so the eBPF helper works on glibc 2.34 or later
whatever the host's glibc is, and no toolchain is needed on the host:

```sh
tools/build-in-docker.sh            # writes dist/rocky9/
GRAYWATCH_JOBS=16 tools/build-in-docker.sh rocky9
```

Each target is a `tools/docker/TARGET.Dockerfile`. To build directly on a host
that already has the toolchain:

```sh
tools/build-portable.sh
```

This uses the same Ninja build and test suite, enables `GRAYWATCH_STATIC`,
verifies that `graywatchd`, `graywatchctl`, and `graywatch-report` have no
dynamic-library dependencies, and writes an archive plus relocatable checksum
to `dist/`:

```text
dist/graywatch-portable-COMMIT-linux-x86_64.tar.gz
dist/graywatch-portable-COMMIT-linux-x86_64.tar.gz.sha256
```

The static build additionally needs the static C and C++ runtimes. On Rocky 9
these are the `glibc-static` and `libstdc++-static` packages from CRB, included
in the installation command above. Override `GRAYWATCH_PORTABLE_BUILD_DIR` or
`GRAYWATCH_PORTABLE_OUTPUT_DIR` when build and artifact directories must live
elsewhere.

Copy the archive to an affected machine, extract it in a temporary directory,
and run a bounded collection without installing a package or service:

```sh
tar -xzf graywatch-portable-COMMIT-linux-x86_64.tar.gz
cd graywatch-portable-COMMIT-linux-x86_64
sudo ./collect-graywatch-support.sh --duration 60 \
  --output /tmp/graywatch-incident.tar.gz
```

When Clang, `bpftool`, and the libbpf submodule are present, the bundle also
contains `bin/graywatch-bpf` (force with `GRAYWATCH_PORTABLE_BPF=1`, skip with
`GRAYWATCH_PORTABLE_BPF=0`). It links libbpf and the C++ runtime statically but
needs the target's glibc 2.34 or later, `libelf.so.1`, `libz.so.1` and
`libzstd.so.1` (present on Rocky/RHEL 9, Debian 12 and Ubuntu 22.04 or later),
because Rocky 9 ships no static libelf. See
[eBPF helper](#ebpf-helper-graywatch-bpf).

Root is optional. It improves visibility into other users' processes and their
wait channels, lets the script read the kernel log where `dmesg_restrict` is
set, and is required for the eBPF helper. A non-root collection still reports all readable CPU, memory, block,
TCP, and InfiniBand evidence. The support archive contains:

- `system.txt`: kernel/OS identity, CPU count, kernel command line, PSI status,
  cgroup filesystem, and procfs/sysfs mount options;
- `lsblk.json` and `mounts.txt`: block-device stacking and mounted filesystems;
- initial and final CPU, I/O, memory, and network JSON state, plus
  `/proc/meminfo` and `/proc/vmstat` snapshots;
- `events.jsonl`: every one-second interval event;
- `kernel.log`: the last 5,000 kernel log lines (omit with `--no-kernel-log`);
- `bpf-events.jsonl` and `bpf.stderr`: eBPF helper output when running as root
  on a kernel with BTF (skip with `--no-bpf`);
- `report.txt`: the support summary, generated on the host at the end of the
  collection; and
- daemon stdout/stderr diagnostics and `collection-errors.txt`.

A failed optional step, such as a missing `lsblk` or a status query that times
out, is recorded in `collection-errors.txt` and does not stop the collection.
Use `--ignore-ib-port DEVICE/PORT` (repeatable) to exclude ports known to be
irrelevant. The script uses the binaries in the bundle's `bin/`. Set
`GRAYWATCH_REPORT` or `GRAYWATCH_BPF` to the path of a different
`graywatch-report` or `graywatch-bpf`, for example when running the script
from a source checkout.

Process command names, thread identifiers, mount points, the kernel command
line and kernel log lines appear in the archive. Review it before sharing it
outside the affected system. The script uses a private temporary directory,
terminates its daemon, and removes working files on exit; the requested output
archive is the only retained result.

### Support report

`graywatch-report` turns a support archive into a ranked summary for a support
engineer. It is a static C++ binary in the bundle (and `build/graywatch-report`
in a normal build), so no Python is needed anywhere. It accepts the archive
or an extracted directory and reads archive members through `tar` without
writing them to disk. The collector already stores its output as `report.txt`
in the archive; rerun it for other formats:

```sh
bin/graywatch-report graywatch-incident.tar.gz
bin/graywatch-report graywatch-incident.tar.gz --format markdown \
  --output incident.md
bin/graywatch-report graywatch-incident.tar.gz --format json
```

The report contains:

- a verdict and findings ranked critical, warning, or info, each with its
  evidence and concrete next steps;
- an assessment that links related signals. For example, threads blocked in
  FUSE while every local disk stays healthy point above the block layer, and a
  slow device-mapper device whose backing disk is healthy points at the
  stacking layer;
- D-state threads grouped by command and wait channel, how long they stayed
  blocked, and what the channel implicates (FUSE, NFS, block, filesystem
  journal, locks, reclaim);
- per-device latency percentiles, memory reclaim/swap/OOM totals, TCP
  retransmissions, and InfiniBand port state;
- a timeline of the degraded episodes;
- kernel log matches (I/O errors, NVMe timeouts, hung tasks, OOM, lockups, link
  flaps, filesystem and hardware errors). Matches from more than 15 minutes
  before the window are reported as history rather than as findings; and
- eBPF findings when `bpf-events.jsonl` is present: FUSE requests still
  waiting for the daemon (critical at 5 s), slow FUSE opcodes, block devices
  whose time is spent queued rather than in the device, tasks still in `D`
  state with their kernel stack, cgroups with run-queue p99 of 10 ms or more,
  the connections that retransmitted, and the processes and cgroups that
  stalled in reclaim; Graywatch's own processes are filtered out; and
- collection coverage: root or not, PSI status with how to enable it,
  `hidepid`, sampling gaps, and daemon errors.

The thresholds in the report are triage heuristics. The daemon's per-interval
`condition` remains the authoritative classification.

The x86-64 artifact was validated unchanged on Rocky Linux 9, Debian 13, and
Pop!_OS with Linux 6.12, 6.18, and the playground's 5.14/7.1 kernels. Separate
architecture builds are required for AArch64. The eBPF helper was validated on
the playground's Rocky 9 5.14 and 7.1 kernels.

## Command-line inspection

Read the current PSI snapshot:

```sh
./build/graywatchctl psi
```

The `psi` and `health` commands require `/proc/pressure`. The long-running
daemon does not: it marks PSI unavailable and continues publishing its other
resources. `tasks` also continues without PSI and reports system CPU pressure
as unavailable while retaining per-thread scheduler and blocked-state evidence.

Measure interval stall percentages once per second, for five samples:

```sh
./build/graywatchctl psi --watch --interval 1 --count 5
```

Omit `--count` to continue until interrupted. Interval percentages are derived
from the kernel's cumulative microsecond counters, so unlike `avg10`, `avg60`,
and `avg300`, they represent exactly the interval Graywatch sampled.

Run the first adaptive health detector:

```sh
./build/graywatchctl health --interval 1 --count 20
```

`health` is an experimental detector. It flags stalls that are both unusual
for this machine *and* large enough to matter. It runs only in this command:
the daemon does not use it and publishes only the absolute guardrails
described below.

#### How `health` decides

Every interval (`--interval`, default 1 s) the command reads `/proc/pressure`
and computes six stall shares: `cpu`, `memory` and `io`, each as `some` (at
least one task stalled) and `full` (all non-idle tasks stalled at once). Each
share is a percentage of the interval: `0.32` means 0.32%, i.e. 3.2 ms of a
second. Each of the six series has its own detector:

1. **Learning.** The first 10 samples are accepted into the baseline
   unconditionally, and the state is `LEARNING`. Start the command while the
   system behaves normally: a stream that starts during an incident learns the
   incident as its baseline.
2. **Baseline.** The rolling window of the last 60 *accepted* samples. Its
   mean and sample standard deviation are reported as `baseline_mean` and
   `baseline_stddev`.
3. **Threshold.** The larger of two limits. `threshold_source` says which one
   applied:
   - `baseline`: `mean + 3 × max(stddev, 0.01)`, i.e. "unusual for this
     machine";
   - `floor`: `--min-stall` (default 1%), i.e. "large enough to matter". On a
     quiet machine the baseline is near zero, so without the floor a few
     milliseconds of activity would look like a many-sigma anomaly.
4. **Anomalous** means `current > threshold`, once the baseline has 10
   samples. The line then shows `anomalous=yes` with the `z_score`,
   `(current − mean) / max(stddev, 0.01)`.
5. **State changes.**
   - `NORMAL` becomes `DEGRADED` after 3 consecutive anomalous samples.
     `streak=N/3` counts them.
   - `DEGRADED` returns to `NORMAL` after 3 consecutive normal samples.
     `recovery=N/3` counts them.
   - `transition=OLD->NEW` marks the sample that changed the state.
6. **Baseline protection.** Anomalous samples, and every sample while
   `DEGRADED`, are kept out of the baseline, so a lasting problem cannot become
   the new normal. Only the sample that completes recovery is added.

Every number on a line describes the comparison that was actually made: the
baseline as it was *before* this sample. `samples` is its size. A series whose
`samples` stops growing while the others continue has had anomalous samples.

An idle node with the default floor (`samples` keeps growing; nothing is
anomalous):

```text
cpu some state=NORMAL current=0.09 baseline_mean=0.14 baseline_stddev=0.08 threshold=1.00 threshold_source=floor samples=12
io some state=NORMAL current=0.00 baseline_mean=0.00 baseline_stddev=0.01 threshold=1.00 threshold_source=floor samples=12
```

The same node with `--min-stall 0`, the purely statistical behaviour:

```text
cpu some state=NORMAL current=0.10 baseline_mean=0.16 baseline_stddev=0.07 threshold=0.37 threshold_source=baseline samples=11
io some state=NORMAL current=0.00 baseline_mean=0.00 baseline_stddev=0.00 threshold=0.03 threshold_source=baseline samples=10
```

Here `io` has one sample fewer than `cpu`. One earlier I/O sample exceeded the
0.03% statistical threshold and was kept out of the baseline. With an all-zero
baseline, any measurable I/O stall does that. With the default floor it would
only count if it reached 1%.

The window (60), learning size (10), 3σ, the 0.01 deviation floor and the 3/3
streaks are fixed detector parameters. Only `--min-stall` is configurable.
They are provisional, and they are printed so their behaviour can be
evaluated rather than assumed.

Inspect CPU scheduling delay for one process or all processes owned by a user:

```sh
./build/graywatchctl tasks -p 1234 --interval 1
./build/graywatchctl tasks -u postgres --interval 1 --top 20
./build/graywatchctl tasks -u 1001 --interval 1 --top 20
```

This command samples Linux `schedstat` counters for every selected thread. Its
`RUNNABLE_WAIT%` is run-queue wait divided by CPU service plus run-queue wait;
time spent sleeping voluntarily is excluded. A busy thread with a low value is
receiving CPU efficiently, while a high value means it was runnable but the
scheduler could not run it. Reading processes owned by another user can require
root, depending on `/proc` mount and ptrace restrictions.
User names are resolved from the target machine's local `/etc/passwd`; numeric
UIDs are accepted directly. Network identity services are intentionally not
queried, which keeps the portable client independent of glibc NSS modules.

This is scheduler and blocked-state attribution, not complete per-process I/O
or memory accounting. System and cgroup PSI cannot be filtered directly by PID
or UID. Graywatch reports a thread currently blocked in uninterruptible state,
but does not assign an exact duration or cause to every sleep. The optional
eBPF helper measures each `D`-state sleep and records its kernel stack.

Each `tasks` interval also includes system-wide CPU pressure from PSI:

```text
SYSTEM_CPU condition=PRESSURE severity=0.50 some_stall_percent=50.00 \
  full_stall_percent=0.00 selected_active_threads=2 selected_waiting_threads=2
```

`severity` is the fraction of wall time in which at least one system task was
waiting for CPU; it is not based on a learned baseline. The selected thread
counts answer whether the requested PID or user was affected. Per-thread wait
times are not summed into PSI because several threads can wait simultaneously.

Use JSON Lines for programmatic consumption or append the same versioned events
to a log:

```sh
./build/graywatchctl tasks -p 1234 --json
./build/graywatchctl tasks -u postgres --log /var/log/graywatch/events.jsonl
```

`--json` changes terminal output only. `--log` can be combined with human or
JSON display and flushes each complete event immediately. The initial event
schema is version 1 and includes the timestamp, interval, resource, condition,
severity, raw PSI evidence, selection scope, and affected thread evidence.

## Local daemon

`graywatchd` samples CPU/task, memory, block, TCP, and InfiniBand evidence, keeps the
latest state for each resource, and publishes matching future events through a
Unix-domain socket:

```sh
./build/graywatchd --socket /tmp/graywatchd.sock --interval 1 \
  --log /tmp/graywatchd.jsonl

./build/graywatchctl status --socket /tmp/graywatchd.sock
./build/graywatchctl status --socket /tmp/graywatchd.sock --resource io
./build/graywatchctl status --socket /tmp/graywatchd.sock --resource memory
./build/graywatchctl status --socket /tmp/graywatchd.sock --resource network
./build/graywatchctl subscribe --socket /tmp/graywatchd.sock \
  --resource io --min-severity 0.10
```

CPU and memory `status` return one current schema-v1 JSON event; I/O and
network status return JSON arrays because a machine can have several devices or
interfaces.

`status` describes only the **most recent interval**, by default the last
second, not the time since the daemon started. The daemon samples every
interval, replaces the previous event, and keeps no history. CPU thread
attribution (`affected_threads`, `waiting_threads`) is refreshed every
`--task-interval` seconds (5 on the playground service);
`attribution_age_seconds` shows how old it is. The exceptions are deliberate
lifetime facts, such as whether an InfiniBand port was ever active or ever
carried traffic. To see a time window, use `subscribe` (every event as it
happens) or the support collector, which records every interval to
`events.jsonl` and summarizes the window in `report.txt`.
`subscribe` remains connected and prints future events matching both the
resource and minimum severity. Use `--resource all` (the default) for every
resource and add `--count N` for a bounded subscription. The protocol supports `GET` (CPU), `GET io`, `GET memory`, `GET network`
and `SUBSCRIBE <minimum-severity> [cpu|io|memory|network|all]` as
newline-delimited commands. Omitting the protocol resource retains the original `all` behavior.
The daemon refuses to replace an existing socket path, preventing it from
unlinking another live service accidentally.

`status --resource io` returns a JSON array with one current event per block
device. These portable events come from `/sys/block/*/stat` and work without
PSI or eBPF. I/O health is classified without treating utilization alone as
failure:

- `healthy`: active and responsive, even when the device is fully busy
- `saturated`: at least 90% busy with average queue depth of at least one
- `congested`: substantial queueing or average operation latency of 20 ms+
- `degraded`: average operation latency of 100 ms+, an absolute guardrail
- `idle`: no completed operations in the interval

Every entry in `/sys/block` is reported, including loop and device-mapper
devices. A stacked device and the disk beneath it both appear, and comparing
them separates delay in the stacking layer from delay in the disk. The
`psi_*` fields on I/O events are system-wide I/O pressure, repeated on every
device for context; they are not per-device measurements.

These are baseline-independent guardrails. They prevent chronic poor service
from becoming statistically normal, but do not replace the planned
workload-aware baseline for smaller relative regressions. `assessment_reason`
distinguishes capacity saturation from latency degradation. These kernel block
averages include time spent queued; the optional eBPF helper splits queue
and device time and adds per-operation tail latency.

See [docs/io-detection.md](docs/io-detection.md) for the baseline model and
evidence boundaries, and
[docs/playground-findings.md](docs/playground-findings.md) for validated
cross-kernel, latency-injection and Quobyte correlation results.

`status --resource network` returns one event per InfiniBand port and one
system-wide TCP event with interval segment counts,
retransmissions, retransmission percentage, input errors and output resets.
TCP is `degraded` when the interval has any input errors (`InErrs`, such as
bad checksums), and otherwise `unstable` when it has at least ten
retransmissions and a retransmission ratio of at least 1%; the minimum count
avoids noisy ratios from tiny samples. InfiniBand events report logical and
physical link state, configured rate, and interval deltas for receive errors,
transmit discards, symbol errors, link-down events and transmit-wait ticks.
Conditions are `healthy`, `congested`, `degraded`, `link_down`, or `inactive`.

A down port is `inactive` (severity 0) when it has never been in use. That
means its lifetime `port_rcv_data`, `port_xmit_data`, and `link_downed`
counters are all zero, and the daemon has never seen it `ACTIVE`. A port that
carried traffic or went down at least once since boot, or went down while
being observed, is `link_down` (severity 0.8). `evidence.in_use` shows which
rule applied. Start `graywatchd` with `--ignore-ib-port DEVICE/PORT`
(repeatable) to drop known-irrelevant ports entirely. An `inactive` port can
still be the one an application was configured to use, so consumers should
correlate the device/port or GID they selected.

`status --resource memory` returns one system-wide memory event from
`/proc/meminfo` and `/proc/vmstat`. It needs neither PSI nor root. Conditions,
in priority order:

- `degraded`, `oom_kill`: the OOM killer ran during the interval;
- `degraded`, `memory_stall_full`: PSI memory `full` of 10% or more;
- `pressure`, `memory_stall_some`: PSI memory `some` of 10% or more;
- `pressure`, `direct_reclaim`: an allocating task had to reclaim memory
  itself (`allocstall_*` increased), the strongest portable stall signal
  without PSI;
- `pressure`, `swap_in`: 100 or more pages per second swapped in;
- `pressure`, `low_available`: `MemAvailable` below 5% of `MemTotal`; and
- `healthy` otherwise.

The evidence includes available memory, swap use, dirty and writeback memory,
major faults, swap-in/out rates, direct and kswapd page scanning, compaction
stalls, file refaults, OOM kills, and the PSI values. PSI availability is
evaluated every interval, so an interval with a failed PSI read reports
`psi_available: false` instead of zero stall time.

## eBPF helper (graywatch-bpf)

`graywatch-bpf` is an optional, root-only helper that adds kernel evidence
`/proc` and `/sys` cannot provide. Build it after initializing submodules:

```sh
git submodule update --init --recursive
make -C bpf -j            # or: make -C bpf STATIC=1 (portable bundle)
sudo ./bpf/.output/graywatch-bpf --interval 1 --duration 60
```

```text
graywatch-bpf [--interval SECONDS] [--duration SECONDS]
              [--collectors fuse,block,offcpu,runqueue,tcp_retransmit,reclaim]
              [--top N] [--blocked-report-ms MS]
```

It needs kernel BTF at `/sys/kernel/btf/vmlinux` and uses CO-RE, so one binary
runs unchanged on the playground's 5.14 and 7.1 kernels. Each collector is
enabled only if the kernel has its hook. If the verifier rejects a collector,
the others still load. The first line is a `bpf_status` event listing each
collector, the hook it attached to, and the reason any collector is disabled.
Then one `bpf_interval` event per key and interval follows on stdout (JSON
lines, schema version 1). Latencies are log2 histograms
(`histogram_us`, keyed by bucket upper bound) with count, total, average, max
and p50/p99 upper bounds. The collectors:

| Collector | Hooks | Key | What it answers |
| --- | --- | --- | --- |
| `fuse` | fentry/fexit `fuse_simple_request` (5.14) or `__fuse_simple_request` (6.x+) | mount, opcode | How long the FUSE daemon took to answer each kind of request, which requests are still waiting and for how long, and error replies (ENOENT, ENODATA, ERANGE, ENOSYS and EOPNOTSUPP are not errors). |
| `block` | `block_rq_insert`, `blk_mq_start_request`, `block_rq_complete` | device, operation | Queue time (insert to dispatch, i.e. scheduler and tag waits) versus device time (dispatch to completion), bytes, errors. |
| `offcpu` | `sched_switch`, `sched_wakeup` | process, kernel stack | How long tasks slept in `D` state and in which kernel path, plus tasks still blocked longer than `--blocked-report-ms` (default 1000). |
| `runqueue` | `sched_wakeup`, `sched_wakeup_new`, `sched_switch` | cgroup v2 path | Wakeup-to-run latency per cgroup, i.e. which service is starved for CPU. |
| `tcp_retransmit` | `tcp_retransmit_skb` | connection | Which peers and ports retransmit, and the socket state. |
| `reclaim` | `mm_vmscan_direct_reclaim_*`, `mm_vmscan_memcg_reclaim_*` | process, cgroup, kind | Who stalled in direct or memcg reclaim, for how long, and how many pages were reclaimed. |

Example FUSE interval with a request still outstanding:

```json
{"event_type":"bpf_interval","collector":"fuse","mount":"/tmp/gw-fuse.Jbw0",
 "opcode":"LOOKUP","count":2,"average_ms":300.124,"max_ms":300.126,
 "p99_upper_us":524288,"errors":0,"outstanding":1,"oldest_outstanding_ms":158.949}
```

Boundaries:

- FUSE latency covers synchronous requests (LOOKUP, GETATTR, OPEN, READDIR,
  and direct or uncached reads and writes). Readahead and writeback are
  background requests and are not timed. The latency includes the daemon's
  backend round trips; it says *that* the daemon was slow, not why.
- A thread waiting for FUSE sleeps interruptibly (`S`) unless it was
  signalled or the request is non-interruptible. The `offcpu` collector
  therefore misses most FUSE waits, which is why the `fuse` collector reports
  outstanding requests itself.
- Device-mapper targets are bio-based and never reach `blk_mq_start_request`.
  Their time appears on the underlying devices. An error completed by a dm
  target before hardware dispatch is not a block error here.
- Stack ids and outstanding entries are checked against `/proc` every
  interval. A map that fills up is cleared, and its dropped updates are
  reported.
- Measured overhead on a 72-CPU Rocky 9 node, with `kernel.bpf_stats_enabled`,
  is about 1.5 µs of BPF time per context switch for all scheduler collectors
  combined. That was 0.009% of CPU at 5,300 switches per second. The helper
  process used about 0.05 CPU and 30 MB RSS.

The collector script runs the helper automatically when it is root and BTF is
present. The report tool reads `bpf-events.jsonl` from the archive.

### Validating the FUSE collector

`tests/fuse_delay_fixture.c` is a disposable FUSE filesystem that speaks the
kernel protocol directly (no libfuse). It answers every LOOKUP with ENOENT
after a fixed delay. Stopping it with `SIGSTOP` simulates a hung client:

```sh
cc -O2 -o /tmp/fuse-fixture tests/fuse_delay_fixture.c
mnt=$(mktemp -d); sudo /tmp/fuse-fixture "$mnt" 300 &
sudo ./bpf/.output/graywatch-bpf --collectors fuse --duration 15 > fuse.jsonl &
helper=$!
for i in $(seq 20); do stat "$mnt/missing-$i" 2>/dev/null; done
sudo pkill -STOP -f fuse-fixture; (stat "$mnt/stuck" &); sleep 8
sudo pkill -CONT -f fuse-fixture
wait "$helper"   # not a bare wait: the fixture only exits on unmount
sudo umount "$mnt"; rmdir "$mnt"
```

On Rocky 9 5.14 the support report for this run lists the LOOKUP as
outstanding for 7.7 s (critical) and the slow LOOKUPs with an 8.3 s maximum.

## Reproducible degradation test

Use a disposable loop device and device-mapper delay target; never attach this
test mapping to a production filesystem. A 250 ms delayed direct read was
validated on the playground as follows:

```sh
truncate -s 16M /tmp/graywatch-delay.img
loop=$(losetup --find --show /tmp/graywatch-delay.img)
sectors=$(blockdev --getsz "$loop")
echo "0 $sectors delay $loop 0 250" | dmsetup create graywatch-delay

graywatchctl subscribe --resource io --min-severity 0.75 --count 1 &
dd if=/dev/mapper/graywatch-delay of=/dev/null \
  bs=4096 count=1 iflag=direct status=none

dmsetup remove graywatch-delay
losetup -d "$loop"
rm -f /tmp/graywatch-delay.img
```

The expected event is `resource_health`, `condition: degraded`, with
`assessment_reason: absolute_latency`. In the validated run, the measured
average was 252 ms and severity was 0.845. Removal was followed by an `idle`
state, demonstrating recovery without training a baseline.

### Install with systemd

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --parallel
ctest --test-dir build --output-on-failure
sudo cmake --install build
sudo systemd-sysusers
sudo systemctl daemon-reload
sudo systemctl enable --now graywatchd
```

The service creates `/run/graywatch` and `/var/log/graywatch` with systemd's
`RuntimeDirectory` and `LogsDirectory` features. The socket is mode `0660`,
owned by `graywatch:graywatch`. Add an application account to the `graywatch`
group when it should be allowed to query or subscribe, then refresh that
application's login or service credentials.

The default paths are:

```text
/run/graywatch/graywatchd.sock
```

Persistent interval logging is opt-in with `graywatchd --log PATH`. The default
systemd service keeps current state in memory and publishes subscriptions but
does not log every raw interval; doing so continuously can consume hundreds of
megabytes per node per day. A later event policy will persist state transitions
and degradation events without turning Graywatch into a time-series database.

`noexec`, `nodev`, and `nosuid` mount options do not prevent Unix socket
creation. Socket creation can fail when the filesystem is read-only, the daemon
lacks directory write/search permission, the path already exists or is too
long, the filesystem has no free inodes, the filesystem does not support Unix
sockets, or an SELinux/AppArmor policy denies the operation. `/run` is normally
a tmpfs that supports Unix sockets. `PrivateTmp=yes` is safe because Graywatch
uses `/run`, not `/tmp`.

### Reading an event

```json
{
  "schema_version": 1,
  "event_type": "resource_pressure",
  "resource": "cpu",
  "condition": "pressure",
  "severity": 0.86,
  "interval_seconds": 1.0,
  "evidence": {
    "psi_some_stall_percent": 86.0,
    "psi_full_stall_percent": 0.0
  }
}
```

- `schema_version` defines the compatibility contract for consumers.
- `condition=pressure` means runnable tasks waited for a CPU during at least
  10% of the interval (PSI `some` >= 10%, the same threshold as memory).
  Below that the condition is `no_pressure`. Small stall shares are normal
  scheduling noise on a busy host, and the exact share remains in `severity`
  and `psi_some_stall_percent`. Pressure alone does not mean the machine is
  unhealthy.
- `severity=0.86` means that during 86% of the interval at least one runnable
  task was waiting for CPU. It is a direct measurement, not a baseline score.
- `interval_seconds` is the actual monotonic-clock sampling duration.
- `psi_some_stall_percent` is the percentage of interval time with one or more
  tasks waiting for CPU.
- `psi_full_stall_percent` is the percentage with all non-idle work stalled.
  System-level CPU `full` is normally zero on Linux; it is more useful for
  cgroup-scoped CPU pressure.
- `selection` says whether evidence represents the whole system, a PID, or a
  user. Daemon events currently use `system/all`.
- `selection.observed_threads` is the number of readable threads sampled.
- `selection.attribution_interval_seconds` is the interval covered by task
  deltas, and `attribution_age_seconds` says how old that evidence is. Resource
  sampling remains one second while task attribution defaults to five seconds
  to bound `/proc` scanning overhead.
- `selection.active_threads` counts sampled threads that ran or waited during
  the interval; `waiting_threads` counts those with run-queue delay.
- `selection.blocked_threads` counts threads currently in uninterruptible
  `D` state. Each affected thread includes `state` and the kernel
  `wait_channel` when `/proc` permissions allow it (for example,
  `request_wait_answer` for a FUSE request). The system daemon remains
  unprivileged and reports an empty channel when the kernel's ptrace access
  check hides another user's wait channel; it does not receive `CAP_SYS_PTRACE`.
- `affected_threads` lists threads currently in `D` state first, then the
  highest absolute run-queue waits, and includes at most `--top N` entries
  (default 20), while the counts still describe all observed threads.

A newly created thread needs two daemon snapshots before Graywatch can compute
a counter delta, so it first becomes attributable in the interval after its
initial observation. Threads that exit before a second snapshot cannot be
attributed by this `/proc` collector; eBPF lifecycle events can close that gap
later.

System attribution depends on `/proc` visibility. With `hidepid` or an LSM
policy, the unprivileged service may observe only a subset of tasks. The
`observed_threads` value makes coverage visible but cannot count processes that
the kernel hides completely.

For example, severity `0.86` does not mean CPUs were 86% utilized. It means
there was unsatisfied runnable CPU demand during 86% of that interval.

### Multi-node support snapshot

`graywatchctl cluster` is a stateless support snapshot. It queries each node
over SSH in parallel, with one connection per host, and does not install a
central service or retain metrics:

```sh
graywatchctl cluster --user root nodea nodeb nodec noded
```

```text
graywatchctl cluster HOST... [--user USER] [--timeout SECONDS]
                     [--remote-command PATH] [--socket PATH] [--json]
```

SSH runs with `BatchMode=yes`, so key-based access is required.
`--remote-command` names the `graywatchctl` on the remote host (default
`/usr/bin/graywatchctl`). The snapshot summarizes the CPU condition and PSI
availability, waiting-thread counts, the thread with the largest absolute
scheduler wait, memory condition, one block device, and current
TCP/InfiniBand issues. Never-used `inactive` ports are not listed as issues.
The DEVICE column shows the most severe `congested`, `saturated` or `degraded`
device with its condition. With none, it shows the busiest healthy device at
1% busy or more, otherwise `-`. Add `--json` when
the snapshot should be attached to a support case or processed by another tool.

Example output from the idle playground:

```text
HOST                   CPU/PSI                WAIT TOP THREAD                 MEMORY                 DEVICE                             NETWORK
nodea                  no_pressure/psi         841 M-Flease-8:5.7ms           healthy avail=89%      -                                  healthy
nodeb                  no_pressure/psi        1072 R-Flease-3:5.2ms           healthy avail=88%      -                                  healthy
nodec                  no_pressure/psi        1044 ZDirector:5.0ms            healthy avail=88%      -                                  healthy
noded                  no_pressure/psi         409 ZDirector:5.3ms            healthy avail=91%      -                                  healthy
```

The same nodea while 144 busy loops and a read-only `fio` random read on
`nvme0n1` ran:

```text
HOST                   CPU/PSI                WAIT TOP THREAD                 MEMORY                 DEVICE                             NETWORK
nodea                  pressure/psi            667 sh:3397.7ms                healthy avail=89%      nvme0n1:saturated 100.0% r=0.46ms w=0.00ms healthy
```

WAIT counts threads that waited for a CPU in the last attribution interval;
on a busy host most of them wait microseconds, so read it together with
CPU/PSI and the top thread's wait. `-` in MEMORY means the remote daemon
predates the memory resource, and `+N` after a network issue counts further
issues.

## Current boundaries and next work

Graywatch currently reports direct pressure, absolute health guardrails, and
bounded interval evidence. The generic rolling detector exists for experiments,
but the daemon does not yet publish workload-conditioned relative-degradation
events. A valid storage baseline must compare similar operation types, request
sizes, IOPS, and queue-depth ranges; it must not train only while idle or allow
a chronic bad state to become healthy. The design is documented in
[docs/io-detection.md](docs/io-detection.md).

The eBPF helper provides cgroup run-queue and reclaim attribution, TCP
connection evidence, and FUSE latency, but only as a standalone root process
whose output goes to support archives. Feeding that evidence into `graywatchd`
events through a privilege-separated helper, cgroup-scoped memory pressure,
asynchronous (readahead/writeback) FUSE timing, and persisted transition-only
logs remain future work. `graywatchd` and its application clients remain
unprivileged.
