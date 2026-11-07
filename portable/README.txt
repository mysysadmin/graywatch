GRAYWATCH PORTABLE SUPPORT BUNDLE

This bundle contains fully static Linux binaries. It does not install files,
start a system service, or require a monitoring server.

Collect a bounded 60-second support archive:

  sudo ./collect-graywatch-support.sh --duration 60

Choose the output path:

  sudo ./collect-graywatch-support.sh --duration 120 \
    --output /tmp/incident-graywatch.tar.gz

Root is optional, but it improves visibility into threads owned by other users.
The archive contains system metadata, initial/final CPU, I/O and network state,
raw interval JSON events, and daemon diagnostics. Review it before sharing.

Manual use:

  ./bin/graywatchd --socket /tmp/graywatch.sock --interval 1 &
  ./bin/graywatchctl status --socket /tmp/graywatch.sock --resource io
  ./bin/graywatchctl subscribe --socket /tmp/graywatch.sock \
    --resource io --min-severity 0.5

The core bundle does not include the optional eBPF collector. The collector
has additional kernel BTF and privilege requirements and is distributed
separately.
