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

