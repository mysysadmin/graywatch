# I/O detection model

Graywatch intentionally separates three questions which are often collapsed
into one utilization percentage.

1. **Saturation:** demand exceeds immediate capacity and work queues.
2. **Relative degradation:** service is worse than expected for comparable
   demand and operation type.
3. **Absolute degradation:** service is objectively poor even if it has been
   poor long enough to look statistically normal.

The current daemon implements baseline-independent saturation, congestion and
absolute-latency guardrails. A device at 100% utilization with low latency and
little queueing remains healthy. This is important: using paid capacity is not
itself a fault.

## Adaptive baseline requirements

A single idle or busy training period is not a valid baseline. The future
relative detector must:

- learn read and write latency distributions separately;
- compare windows from similar demand ranges (IOPS, request size and queue
  depth), rather than comparing an idle device with a loaded one;
- require minimum sample counts before using tail percentiles;
- freeze or heavily down-weight learning while degraded;
- retain absolute guardrails and error signals independently;
- report `insufficient_data` instead of pretending confidence;
- keep short-term and time-of-day history so a permanently bad recent period
  cannot erase a known-good reference immediately.

The event should expose both conclusions, for example
`relative_change=8.4x` and `absolute_condition=congested`. Applications can act
on the semantic condition without knowing the statistical implementation.

## Evidence boundaries

Portable `/sys/block/*/stat` intervals include block-layer queueing and provide
average completion latency, utilization and average queue depth. The eBPF
helper (`graywatch-bpf`) splits each request into queue time
(`block_rq_insert` to `blk_mq_start_request`) and device time
(`blk_mq_start_request` to completion), giving per-operation distributions
and errors that reach hardware dispatch. Its FUSE collector times requests
above the block layer, from the kernel sending them to the daemon answering.

These signals deliberately overlap but are not interchangeable:

- high queue-inclusive latency plus low service latency means delay above or
  before the physical device;
- high service tail latency on several unrelated processes supports a shared
  device/system diagnosis;
- high queue time with low device time means the delay is in the I/O
  scheduler or tag allocation rather than in the device;
- an RPC timeout or a device-mapper error completed above
  `blk_mq_start_request` is invisible to the physical-device probe. FUSE
  waits are visible only through the FUSE collector;
- absence of block degradation is evidence, not proof, that the problem is in
  the application, filesystem client or network path.

The eBPF helper currently runs separately as root and writes JSON lines into
support archives. The intended production
boundary is a minimal privileged collector that publishes bounded aggregates
to an unprivileged `graywatchd`; applications never receive BPF privileges.

