# Playground findings

Validated 2026-11-07 on nodea–noded (`192.168.65.160`, `.153`, `.157`,
`.142`) with Rocky Linux kernels 5.14 and 7.1.

## Physical block service

The same CO-RE collector binary attached on both kernel generations. A bounded
direct NVMe read on nodea completed 1,024 128 KiB requests (128 MiB) with
0.479 ms average service latency, p99 bucket upper bound 1,024 µs and zero
errors. Nodeb completed 512 requests (64 MiB) at 0.519 ms average with p99
upper bound 2,048 µs and zero errors.

## Disposable latency injection

A loop-backed device-mapper target delayed one direct read by 250 ms. The
daemon subscription emitted:

```json
{
  "event_type": "resource_health",
  "resource": "io",
  "condition": "degraded",
  "severity": 0.845,
  "device": {"name": "dm-3", "major": 253, "minor": 3},
  "evidence": {
    "assessment_reason": "absolute_latency",
    "maximum_average_latency_ms": 252.0,
    "average_read_latency_ms": 252.0,
    "busy_percent": 25.18,
    "average_queue_depth": 0.252
  }
}
```

The mapping, loop and backing file were removed and verified absent. The next
device state was idle. No Quobyte disk was altered.

## Quobyte client stall correlation

A bounded `fio` run on nodec targeted
`/quobyte/test/graywatch-fio-20261107.dat` with a 20-second outer timeout. It
stalled during file creation/layout and never reached the intended data I/O.
The Quobyte client reported a delayed `PosixFileService::createOrOpen`, then
cancelled the RPC and returned `EINTR` when the timeout signalled fio.

During the same window:

- every captured Graywatch block event on all four nodes was healthy;
- physical block service averages were roughly 0.05–0.30 ms;
- observed physical p99 bucket upper bounds were at most 2,048 µs;
- the BPF collector reported zero block errors;
- the exact test file was absent on all nodes after cleanup.

This supports “not a local physical-disk degradation” and narrows the incident
to an upper storage-client/RPC/network path. It does not by itself identify the
root cause.

The client journal also repeatedly reported that local GID
`fe80::e41d:2d03:6f:a882` was not found and that selection fell back to IP.
`ibstat` showed port 1 active at 56 Gb/s and port 2 down/polling; that GID maps
to the down port. Those facts make network/RDMA path health the next useful
collector, but remain correlation rather than a proven cause.

## Healthy Quobyte load reference

After the dedicated `peer-test` volume was created, a 4 KiB create/fsync smoke
test succeeded on nodec. A bounded 15-second fio run then used one uniquely
named file with 128 KiB mixed random I/O, depth 16, direct I/O and no
preallocation. It completed without error at approximately:

- 3,250 read IOPS / 406 MiB/s and 3,229 write IOPS / 404 MiB/s;
- application read p99 6.85 ms and write p99 6.46 ms;
- application read/write p99.9 approximately 10 ms / 9.5 ms.

Graywatch showed the server-side work concentrated on nodea. `nvme2n1` reached
100% busy with average queue depth around 10.6 and was correctly classified as
`saturated`, not `degraded`: average read service was about 1.13 ms, average
write service about 0.21 ms, p99 service bucket upper bound 4,096 µs and zero
errors. Peer writes to `nvme0n1` and `nvme3n1` averaged about 0.06 ms with p99
at or below 256 µs.

This is the useful counterexample to “100% busy means unhealthy.” The device
was delivering high throughput with bounded service latency. Saturation is
capacity evidence applications may use for load placement; degradation needs
separate latency/error or relative-regression evidence. The exact fio file was
removed and verified absent on all four nodes.

## Blocked task and network evidence

A suspended disposable device-mapper target placed a direct-read `dd` process
in uninterruptible `D` state. Graywatch reported `blocked_threads: 1` and the
exact PID/TID even though it consumed no CPU and accumulated no run-queue wait.
The service remains unprivileged, so the kernel hid the other user's wait
channel; an empty `wait_channel` explicitly represents unavailable evidence.
The mapper, loop and backing file were resumed and removed after the test.

Live network events showed nodea, nodeb and nodec InfiniBand port 1 active at
56 Gb/s and port 2 down/polling. Noded had both ports active at 56 Gb/s. All
observed one-second InfiniBand error deltas were zero. TCP interval events on
all four nodes showed active traffic with zero new retransmissions and input
errors in the validation windows. This demonstrates why lifetime TCP counters
must not be interpreted as current degradation.


## eBPF helper (graywatch-bpf)

Validated 2026-11-07 with one CO-RE binary on the 5.14 and 7.1 kernels, using
disposable fixtures only. No Quobyte mount or disk was altered.

- **FUSE on 5.14 (Quobyte, read-only observation):** the helper attached to
  `fuse_simple_request`. LOOKUP averaged 0.06 ms; READDIRPLUS averaged 142 ms.
- **FUSE on 7.1 (`tests/fuse_delay_fixture.c`):** the helper attached to
  `__fuse_simple_request`. Lookups delayed by 40 ms measured 40.1 ms. A
  request held by a stopped fixture was reported as outstanding at 1.65 s and
  timed at 4.0 s once answered.
- **End to end on nodeb (5.14, PSI enabled):** the fixture with a 300 ms delay
  was stopped for 8 s during a `stat`, while `collect-graywatch-support.sh
  --duration 20` ran. `report.txt` raised two critical findings: a LOOKUP
  outstanding for 7.7 s, and slow LOOKUPs (21 requests, 8.3 s maximum). It
  also concluded that the delay was above the local block layer, because every
  local disk stayed healthy.
- **D-state:** a direct read through a disposable dm-delay mapping was reported
  as still blocked after 1.97 s, with its `submit_bio_wait` kernel stack.
- **TCP:** an nftables rule dropping every fifth packet (`numgen random mod 5
  == 0 drop`) in a private network namespace produced per-connection
  retransmits. The 7.1 kernel has no `sch_netem`, so netem could not be used.
  One real retransmit to Quobyte port 7875 was also recorded.
- **Reclaim:** a memory-limited cgroup produced 125 memcg reclaim stalls,
  attributed to the process and cgroup.
- **Overhead (nodea, `kernel.bpf_stats_enabled`):** about 1.5 µs of BPF time
  per context switch across the scheduler collectors, i.e. 57 ms of CPU in
  8.5 s on 72 CPUs (0.009%) at 5,300 switches per second. The helper process
  used 0.47 s of CPU in 9 s and 30 MB RSS. The sysctl was reset to 0 afterwards.

Problems found in this real data and fixed before the commit:
- interrupt noise frames at the start of 5.14 stacks;
- GETXATTR `ENODATA` replies counted as errors;
- Graywatch's own processes listed in the findings;
- InfiniBand `transmit_wait` noise raised as a warning;
- interval maxima below the average;
- IPv4-mapped addresses shown in IPv6 form; and
- empty cgroup paths for cgroups that had already exited.

The portable bundle built in the `rocky9` container (`tools/build-in-docker.sh`)
passed its checksums on nodeb. Its `graywatch-bpf` loaded all six collectors
there, linking only glibc, libelf, libz and libzstd dynamically.
