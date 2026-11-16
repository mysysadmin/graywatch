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

RUN IT AS A SERVICE

  A one-off collection only sees the minute it runs. Installed as a service,
  graywatchd keeps watching, so it can tell you what happened before anyone
  noticed, and collect by itself while a problem lasts:

    sudo ./install-graywatch.sh

  This installs to /opt/graywatch and starts two systemd units:

  - graywatchd.service runs unprivileged as the user graywatch and samples
    every second. It keeps every event of the last 30 minutes in
    /var/log/graywatch (a flight recorder), and logs each problem period, when
    a resource was non-healthy for a while and when it recovered, to
    /var/log/graywatch/changes.jsonl.
  - graywatch-capture.path starts a full collection as root, with eBPF, when
    a problem lasts 60 seconds (severity 0.75 or more, for example a stalled
    FUSE mount or a disk averaging 100 ms per request), at most once an hour.
    The archives go to /var/lib/graywatch/captures; the newest 10 are kept.

  Every collection on that node, automatic or by hand, then also contains
  the recorded minutes before it and the problem log, and its report says
  when the problems began.

  Settings are in /etc/default/graywatch; restart graywatchd after a change.
  To record all the time instead of only the last 30 minutes:

    GRAYWATCH_KEEP=7d       # 30m is the flight recorder default
    GRAYWATCH_MAX_SIZE=2G   # the oldest files are deleted first

  Compressed, the recording takes about 50 MB a day on a node with ten disks
  and several hundred MB on a storage server with a hundred. The daemon uses
  a few percent of one CPU core, most of it for reading per-thread CPU
  statistics on busy nodes.

  Without automatic collections:  sudo ./install-graywatch.sh --no-capture
  Remove, keep the recordings:    sudo ./install-graywatch.sh --uninstall
  Remove everything:              sudo ./install-graywatch.sh --purge

MORE OPTIONS

Choose the duration and output path, exclude a known-unused InfiniBand port,
or leave out the kernel log or the eBPF helper:

  sudo ./collect-graywatch-support.sh --duration 120 \
    --output /tmp/incident-graywatch.tar.gz \
    --ignore-ib-port mlx5_0/2 --no-kernel-log --no-bpf

Write the archive to a directory instead, for example a larger filesystem,
and keep only the newest five there:

  sudo ./collect-graywatch-support.sh --output-dir /data/graywatch --keep 5

When the graywatchd service records on the node, the collection adds its
last 30 minutes (--history MINUTES changes that, 0 leaves it out).

The archive contains system metadata (including the kernel command line and
PSI status), block-device topology, mounts, initial/final CPU, I/O, memory,
network and FUSE state, raw interval JSON events, eBPF evidence
(bpf-events.jsonl), the last 5000 kernel log lines, daemon diagnostics, and
report.txt. With the service installed it also holds history-events.jsonl
(the recorded minutes before the collection), changes.jsonl (the problem
log) and, for an automatic collection, trigger.json (what asked for it).
Process names, mount points and kernel log lines appear in it.
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
