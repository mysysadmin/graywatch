# Graywatch

Graywatch is an experimental local Linux degradation detector. It observes the
quality of a process's execution environment and exposes evidence-backed health
states to applications.

The first milestone reads Linux Pressure Stall Information (PSI). This gives us
a useful, unprivileged signal and lets us validate the build and deployment
workflow before adding eBPF collectors.

## Build

Requirements:

- Linux with PSI enabled (`/proc/pressure`)
- CMake 3.20 or newer
- A C++20 compiler

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Read the current PSI snapshot:

```sh
./build/graywatchctl psi
```

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

This is currently CPU attribution only. System and cgroup PSI cannot be
filtered directly by PID or UID, and Graywatch does not infer per-process I/O or
memory stalls from CPU scheduling counters. Those require separate accounting
or eBPF collectors.

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

`graywatchd` keeps the latest CPU-pressure event and publishes matching future
events through a Unix-domain socket:

```sh
./build/graywatchd --socket /tmp/graywatchd.sock --interval 1 \
  --log /tmp/graywatchd.jsonl

./build/graywatchctl status --socket /tmp/graywatchd.sock
./build/graywatchctl subscribe --socket /tmp/graywatchd.sock \
  --min-severity 0.10
```

`status` returns one current schema-v1 JSON event. `subscribe` remains connected
and prints future events whose severity is at least the requested value. Add
`--count N` for a bounded subscription. The initial protocol supports `GET` and
`SUBSCRIBE <minimum-severity>` as newline-delimited commands. The daemon refuses
to replace an existing socket path, preventing it from unlinking another live
service accidentally.

### Install with systemd

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
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
/var/log/graywatch/events.jsonl
```

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
- `selection.active_threads` counts sampled threads that ran or waited during
  the interval; `waiting_threads` counts those with run-queue delay.
- `affected_threads` contains the highest absolute run-queue waits first. The
  daemon includes at most `--top N` entries (default 20), while the counts still
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

Example output:

```text
cpu some avg10=0.00 avg60=0.00 avg300=0.00 total_us=1234
memory some avg10=0.00 avg60=0.00 avg300=0.00 total_us=5678
memory full avg10=0.00 avg60=0.00 avg300=0.00 total_us=42
io some avg10=0.00 avg60=0.00 avg300=0.00 total_us=9012
io full avg10=0.00 avg60=0.00 avg300=0.00 total_us=321
```

## Why start with PSI?

PSI measures time during which tasks cannot make progress because CPU, memory,
or I/O is unavailable. It is already provided by the Linux kernel, requires no
privileged probe, and gives Graywatch a sound first input while its event model,
baseline learning, and local API are developed.
