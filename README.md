# Graywatch

Graywatch is an experimental local Linux degradation detector. It observes the
quality of a process's execution environment and exposes versioned,
evidence-backed health events directly to applications. It runs locally: no
Prometheus, Grafana, remote collector, or monitoring server is required.

The current implementation provides:

- CPU pressure from Linux PSI when the kernel exposes it;
- per-thread scheduler delay and uninterruptible (`D`-state) attribution;
- per-device block activity, queueing, saturation, and absolute-latency health;
- optional eBPF block service-time histograms and completion errors;
- system-wide TCP retransmission/error intervals;
- InfiniBand link state, rate, congestion, and error-counter intervals;
- current-state queries and resource-filtered subscriptions over a local Unix
  socket; and
- a stateless multi-node support snapshot over SSH.

PSI is optional. On a kernel where `/proc/pressure` is unavailable, CPU events
explicitly report `condition: unavailable` while task, block, TCP, and
InfiniBand evidence continues to work.

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

Rocky Linux 9's currently enabled playground repositories do not contain
`ninja-build`. Install the base tools, enable a repository that provides Ninja
(commonly EPEL), then verify the executable before configuring Graywatch:

```sh
sudo dnf install -y gcc-c++ cmake git make pkgconf-pkg-config
sudo dnf install -y epel-release
sudo dnf install -y ninja-build
ninja --version
```

If EPEL is intentionally unavailable, install a trusted upstream Ninja binary
through the site's normal package-management process. Graywatch's documented
and supported CMake configuration uses `-G Ninja`; do not silently fall back to
a different generator in deployment automation.

The optional eBPF collector additionally requires Clang/LLVM, `bpftool`, ELF
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

Read the current PSI snapshot:

```sh
./build/graywatchctl psi
```

The `psi` and `health` commands require `/proc/pressure`. The long-running
daemon does not: it marks PSI unavailable and continues publishing its other
resources.

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

Each `(resource, scope)` stream learns independently. The initial state is
`LEARNING`; after ten samples it becomes `NORMAL`. A sample is provisionally
anomalous when it exceeds the rolling mean by three standard deviations, and
three consecutive anomalies produce `DEGRADED`. Three normal observations are
required for recovery. Candidate anomalies and degraded observations are not
added to the baseline.

These statistical parameters are intentionally detector configuration, not
resource-specific operational thresholds. They are provisional and remain
visible in the output so their behavior can be evaluated rather than assumed.

Inspect CPU scheduling delay for one process or all processes owned by a user:

```sh
./build/graywatchctl tasks -p 1234 --interval 1
./build/graywatchctl tasks -u postgres --interval 1 --top 20
```

This command samples Linux `schedstat` counters for every selected thread. Its
`RUNNABLE_WAIT%` is run-queue wait divided by CPU service plus run-queue wait;
time spent sleeping voluntarily is excluded. A busy thread with a low value is
receiving CPU efficiently, while a high value means it was runnable but the
scheduler could not run it. Reading processes owned by another user can require
root, depending on `/proc` mount and ptrace restrictions.

This is scheduler and blocked-state attribution, not complete per-process I/O
or memory accounting. System and cgroup PSI cannot be filtered directly by PID
or UID. Graywatch reports a thread currently blocked in uninterruptible state,
but does not yet assign an exact duration or storage/network cause to every
sleep. Those require additional accounting or eBPF collectors.

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

`graywatchd` samples CPU/task, block, TCP, and InfiniBand evidence, keeps the
latest state for each resource, and publishes matching future events through a
Unix-domain socket:

```sh
./build/graywatchd --socket /tmp/graywatchd.sock --interval 1 \
  --log /tmp/graywatchd.jsonl

./build/graywatchctl status --socket /tmp/graywatchd.sock
./build/graywatchctl status --socket /tmp/graywatchd.sock --resource io
./build/graywatchctl status --socket /tmp/graywatchd.sock --resource network
./build/graywatchctl subscribe --socket /tmp/graywatchd.sock \
  --resource io --min-severity 0.10
```

CPU `status` returns one current schema-v1 JSON event; I/O and network status
return JSON arrays because a machine can have several devices or interfaces.
`subscribe` remains connected and prints future events matching both the
resource and minimum severity. Use `--resource all` (the default) for every
resource and add `--count N` for a bounded subscription. The protocol supports `GET` and
`SUBSCRIBE <minimum-severity> [cpu|io|network|all]` as newline-delimited
commands. Omitting the protocol resource retains the original `all` behavior.
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

These are baseline-independent guardrails. They prevent chronic poor service
from becoming statistically normal, but do not replace the planned
workload-aware baseline for smaller relative regressions. `assessment_reason`
distinguishes capacity saturation from latency degradation. These kernel block
averages include time spent queued; the optional eBPF collector adds
per-operation tail service latency.

See [docs/io-detection.md](docs/io-detection.md) for the baseline model and
evidence boundaries, and
[docs/playground-findings.md](docs/playground-findings.md) for validated
cross-kernel, latency-injection and Quobyte correlation results.

`status --resource network` returns one event per InfiniBand port and one
system-wide TCP event with interval segment counts,
retransmissions, retransmission percentage, input errors and output resets.
TCP is `unstable` when an interval has at least ten retransmissions and a
retransmission ratio of at least 1%; the minimum count avoids noisy ratios from
tiny samples. InfiniBand events report logical and physical link state,
configured rate, and interval deltas for receive errors, transmit discards,
symbol errors, link-down events and transmit-wait ticks. Conditions are
`healthy`, `congested`, `degraded`, or `link_down`. A down unused port is still
reported as evidence; Graywatch does not yet know whether an application
intended to use that port. Consumers should correlate the device/port or GID
they selected rather than interpreting any down port as whole-machine failure.

## eBPF block service latency prototype

Build the optional collector after initializing submodules:

```sh
git submodule update --init --recursive
make -C bpf -j
sudo ./bpf/.output/graywatch-biolatency --interval 1
```

The collector requires kernel BTF at `/sys/kernel/btf/vmlinux` and sufficient
privilege to load and attach BPF programs (normally root in the prototype).
The resulting binary uses CO-RE and was validated unchanged across the
playground's Linux 5.14 and 7.1 kernels.

It correlates `blk_mq_start_request` with the raw `block_rq_complete` tracepoint by request pointer
and reports per-device, per-operation interval counts, bytes, average service
latency, logarithmic p50/p95/p99 upper bounds, and errors. This is dispatch-to-
completion service latency; it intentionally excludes scheduler queue time.
The portable `/sys/block` metrics remain the queue-inclusive companion signal.

The hook begins at `blk_mq_start_request`, so it describes physical-device
service rather than every failure in the storage stack. An error completed by
a device-mapper target before hardware dispatch, or a FUSE/client stall above
the block layer, will not appear as an eBPF device error. That absence is useful
correlation evidence: high application latency with healthy local block
service points investigation above the physical device. Future collectors will
cover those upper layers explicitly.

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
- `condition=pressure` means the CPU PSI `some` counter advanced during the
  interval; it does not by itself mean the machine is unhealthy.
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
- `affected_threads` contains the highest absolute run-queue waits first. The
  daemon prioritizes blocked threads, then highest absolute run-queue waits,
  and includes at most `--top N` entries (default 20), while the counts still
  describe all observed threads.

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

The repository includes a stateless support helper that queries each node over
SSH in parallel; it does not install a central service or retain metrics:

```sh
python3 tools/graywatch-cluster-status.py \
  --user root nodea nodeb nodec noded
```

It summarizes PSI availability, waiting-thread counts, the thread with the
largest absolute scheduler wait, the busiest active block device, and current
TCP/InfiniBand issues. Hosts are queried concurrently. Add `--json` when the
snapshot should be attached to a support case or processed by another tool.

Example output:

```text
HOST                   CPU/PSI            WAIT TOP THREAD                   BUSIEST DEVICE                     NETWORK
nodea                  unavailable/no-psi  1229 worker:6.3ms                 nvme2n1:100.0% r=1.13ms w=0.21ms  mlx4_0/2:link_down
nodeb                  unavailable/no-psi  1193 kworker:5.1ms                -                                  mlx4_0/2:link_down
noded                  unavailable/no-psi   918 worker:7.4ms                 -                                  healthy
```

## Current boundaries and next work

Graywatch currently reports direct pressure, absolute health guardrails, and
bounded interval evidence. The generic rolling detector exists for experiments,
but the daemon does not yet publish workload-conditioned relative-degradation
events. A valid storage baseline must compare similar operation types, request
sizes, IOPS, and queue-depth ranges; it must not train only while idle or allow
a chronic bad state to become healthy. The design is documented in
[docs/io-detection.md](docs/io-detection.md).

Memory health beyond PSI, cgroup-scoped attribution, TCP connection-level
evidence, FUSE/RPC latency probes, persisted transition-only logs, and a
privilege-separated production BPF helper remain future work. The standalone
eBPF collector is deliberately optional and privileged; `graywatchd` and its
application clients remain unprivileged.
