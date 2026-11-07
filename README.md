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
