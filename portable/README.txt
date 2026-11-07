GRAYWATCH PORTABLE SUPPORT BUNDLE

This bundle contains fully static Linux binaries. It does not install files,
start a system service, or require a monitoring server.

Collect a bounded 60-second support archive while the problem is happening:

  sudo ./collect-graywatch-support.sh --duration 60

Choose the output path, exclude a known-unused InfiniBand port, or leave out
the kernel log:

  sudo ./collect-graywatch-support.sh --duration 120 \
    --output /tmp/incident-graywatch.tar.gz \
    --ignore-ib-port mlx5_0/2 --no-kernel-log

Root is optional, but it improves visibility into threads owned by other users
and allows reading the kernel log. The archive contains system metadata
(including the kernel command line and PSI status), block-device topology,
mounts, initial/final CPU, I/O, memory and network state, raw interval JSON
events, the last 5000 kernel log lines and daemon diagnostics. Review it before
sharing.

Summarize an archive for a support engineer (Python 3.8+, any machine):

  python3 ./graywatch-report.py /tmp/incident-graywatch.tar.gz
  python3 ./graywatch-report.py /tmp/incident-graywatch.tar.gz --format markdown

PSI (pressure stall information) adds CPU, memory and I/O stall percentages.
If /proc/pressure is missing on RHEL-family kernels, PSI is compiled in but
disabled by default. Enable it with a reboot:

  grubby --update-kernel=ALL --args=psi=1 && reboot

Manual use:

  ./bin/graywatchd --socket /tmp/graywatch.sock --interval 1 &
  ./bin/graywatchctl status --socket /tmp/graywatch.sock --resource io
  ./bin/graywatchctl status --socket /tmp/graywatch.sock --resource memory
  ./bin/graywatchctl subscribe --socket /tmp/graywatch.sock \
    --resource io --min-severity 0.5

The core bundle does not include the optional eBPF collector. The collector
has additional kernel BTF and privilege requirements and is distributed
separately.
