GRAYWATCH PORTABLE SUPPORT BUNDLE

This bundle contains Linux binaries that need no installation. It does not
install files, start a system service, or require a monitoring server.

Collect a bounded 60-second support archive while the problem is happening:

  sudo ./collect-graywatch-support.sh --duration 60

Choose the output path, exclude a known-unused InfiniBand port, or leave out
the kernel log or the eBPF helper:

  sudo ./collect-graywatch-support.sh --duration 120 \
    --output /tmp/incident-graywatch.tar.gz \
    --ignore-ib-port mlx5_0/2 --no-kernel-log --no-bpf

Root is optional, but it improves visibility into threads owned by other users,
allows reading the kernel log, and is required for the eBPF helper. The archive
contains system metadata (including the kernel command line and PSI status),
block-device topology, mounts, initial/final CPU, I/O, memory and network
state, raw interval JSON events, eBPF evidence (bpf-events.jsonl), the last
5000 kernel log lines, daemon diagnostics, and report.txt. Review it before
sharing.

report.txt is the support summary: a verdict, ranked findings with evidence
and next steps, a timeline and collection coverage. Re-create it or produce
other formats from the archive on any Linux machine:

  ./bin/graywatch-report /tmp/incident-graywatch.tar.gz
  ./bin/graywatch-report /tmp/incident-graywatch.tar.gz --format markdown
  ./bin/graywatch-report /tmp/incident-graywatch.tar.gz --format json

PSI (pressure stall information) adds CPU, memory and I/O stall percentages.
If /proc/pressure is missing on RHEL-family kernels, PSI is compiled in but
disabled by default. Enable it with a reboot:

  grubby --update-kernel=ALL --args=psi=1 && reboot

bin/graywatch-bpf (present when the bundle was built with the eBPF toolchain)
records FUSE request latency per opcode and stuck FUSE requests, block queue
versus device time, uninterruptible sleeps with kernel stacks, per-cgroup
run-queue latency, per-connection TCP retransmits and direct-reclaim stalls.
It needs root, kernel BTF (/sys/kernel/btf/vmlinux), glibc 2.34 or later,
libelf.so.1, libz.so.1 and libzstd.so.1. The collector skips it, with a note
in collection-errors.txt, when these are missing. Standalone use:

  sudo ./bin/graywatch-bpf --interval 1 --duration 30

Manual use:

  ./bin/graywatchd --socket /tmp/graywatch.sock --interval 1 &
  ./bin/graywatchctl status --socket /tmp/graywatch.sock --resource io
  ./bin/graywatchctl status --socket /tmp/graywatch.sock --resource memory
  ./bin/graywatchctl subscribe --socket /tmp/graywatch.sock \
    --resource io --min-severity 0.5

Snapshot several nodes over SSH (needs key-based SSH and graywatchctl on each
node; --remote-command gives its path there):

  ./bin/graywatchctl cluster --user root nodea nodeb nodec
