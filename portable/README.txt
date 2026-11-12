GRAYWATCH PORTABLE SUPPORT BUNDLE

Graywatch tells you where a Linux node is losing time: the disks, a FUSE
storage client, memory, the network or the CPU. Nothing to install, no reboot.

QUICK START

  1. While the problem is happening, run as root:

       sudo ./collect-graywatch-support.sh

     It collects for 60 seconds, then prints the path of the archive it wrote
     and of a copy of its report next to it.

  2. Read the report, or send the archive to support:

       less graywatch-support-*-report.txt

     The same report.txt is inside the archive. If the archive was renamed or
     decompressed on the way, tar still reads it:

       tar -xOf graywatch-support-*.tar* ./report.txt | less

  The report starts with a verdict and the most likely cause, then lists the
  findings worst first, each with its evidence and what to check next. If the
  verdict finds nothing, collect again while the problem is happening.

ROOT AND PSI

  Run as root. Without root the collector cannot use eBPF, and you lose stuck
  and slow FUSE requests, kernel stacks of blocked threads, which connections
  retransmit and who stalled in memory reclaim. Root also reads the kernel log.

  EBPF.txt lists every eBPF program it loads, what each one reads, that none
  of them can change the system, and how to check that with bpftool.

  PSI (pressure stall information) is optional. It is usually off on RHEL,
  Rocky and AlmaLinux. Without it the report has no CPU pressure verdict and no
  memory stall percentages; everything else works. Enabling it needs a reboot:

    grubby --update-kernel=ALL --args=psi=1 && reboot

MORE OPTIONS

Choose the duration and output path, exclude a known-unused InfiniBand port,
or leave out the kernel log or the eBPF helper:

  sudo ./collect-graywatch-support.sh --duration 120 \
    --output /tmp/incident-graywatch.tar.gz \
    --ignore-ib-port mlx5_0/2 --no-kernel-log --no-bpf

The archive contains system metadata (including the kernel command line and
PSI status), block-device topology, mounts, initial/final CPU, I/O, memory,
network and FUSE state, raw interval JSON events, eBPF evidence
(bpf-events.jsonl), the last 5000 kernel log lines, daemon diagnostics, and
report.txt. Process names, mount points and kernel log lines appear in it.
Review it before sharing.

Re-create the report or produce other formats from the archive on any Linux
machine:

  ./bin/graywatch-report /tmp/incident-graywatch.tar.gz
  ./bin/graywatch-report /tmp/incident-graywatch.tar.gz --format markdown
  ./bin/graywatch-report /tmp/incident-graywatch.tar.gz --format json

bin/graywatch-bpf (present when the bundle was built with the eBPF toolchain)
records FUSE request latency per opcode and stuck FUSE requests, block queue
versus device time, uninterruptible sleeps with kernel stacks, per-cgroup
run-queue latency, per-connection TCP retransmits and direct-reclaim stalls.
It needs root, kernel BTF (/sys/kernel/btf/vmlinux), glibc 2.35 or later (or
the 2.34 of RHEL 9 and its rebuilds), libelf.so.1, libz.so.1 and libzstd.so.1.
Standard installs of RHEL 9, Debian 12 and Ubuntu 22.04 or later have them;
minimal container images often lack libelf. The collector continues without
eBPF, with a note in collection-errors.txt, when these are missing.
Standalone use:

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

LICENSE

  Graywatch is MIT licensed (LICENSE). THIRD_PARTY_NOTICES lists the libraries
  linked into the binaries and their licenses.
