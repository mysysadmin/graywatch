#!/bin/sh
set -eu

duration=600
output=""
output_dir=""
keep_archives=""
history_minutes=30
history_dir=/var/log/graywatch
trigger=""
kernel_log=1
bpf=1
ignored_ports=""
usage() {
    echo "usage: $0 [--duration SECONDS] [--output ARCHIVE.tar.gz | --output-dir DIR]" \
         "[--keep N] [--history MINUTES] [--history-dir DIR] [--trigger FILE]" \
         "[--ignore-ib-port DEVICE/PORT]... [--no-kernel-log] [--no-bpf]" >&2
    exit 2
}
while [ "$#" -gt 0 ]; do
    case "$1" in
        --duration)
            [ "$#" -ge 2 ] || { echo "--duration requires seconds" >&2; exit 2; }
            duration=$2
            shift 2
            ;;
        --output)
            [ "$#" -ge 2 ] || { echo "--output requires a path" >&2; exit 2; }
            output=$2
            shift 2
            ;;
        --output-dir)
            [ "$#" -ge 2 ] || { echo "--output-dir requires a directory" >&2; exit 2; }
            output_dir=$2
            shift 2
            ;;
        --keep)
            [ "$#" -ge 2 ] || { echo "--keep requires a count" >&2; exit 2; }
            keep_archives=$2
            shift 2
            ;;
        --history)
            [ "$#" -ge 2 ] || { echo "--history requires minutes" >&2; exit 2; }
            history_minutes=$2
            shift 2
            ;;
        --history-dir)
            [ "$#" -ge 2 ] || { echo "--history-dir requires a directory" >&2; exit 2; }
            history_dir=$2
            shift 2
            ;;
        --trigger)
            [ "$#" -ge 2 ] || { echo "--trigger requires a file" >&2; exit 2; }
            trigger=$2
            shift 2
            ;;
        --ignore-ib-port)
            [ "$#" -ge 2 ] || { echo "--ignore-ib-port requires DEVICE/PORT" >&2; exit 2; }
            case "$2" in
                */*) ;;
                *) echo "--ignore-ib-port must be DEVICE/PORT" >&2; exit 2 ;;
            esac
            ignored_ports="$ignored_ports $2"
            shift 2
            ;;
        --no-kernel-log)
            kernel_log=0
            shift
            ;;
        --no-bpf)
            bpf=0
            shift
            ;;
        *)
            usage
            ;;
    esac
done

case "$duration" in
    ''|*[!0-9]*) echo "duration must be a positive integer" >&2; exit 2 ;;
    0) echo "duration must be a positive integer" >&2; exit 2 ;;
esac
case "$history_minutes" in
    ''|*[!0-9]*) echo "history must be a number of minutes" >&2; exit 2 ;;
esac
case "$keep_archives" in
    '') ;;
    *[!0-9]*|0) echo "keep must be a positive integer" >&2; exit 2 ;;
esac
if [ -n "$keep_archives" ] && [ -z "$output_dir" ]; then
    echo "--keep needs --output-dir" >&2
    exit 2
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ -x "$script_dir/bin/graywatchd" ]; then
    daemon="$script_dir/bin/graywatchd"
    client="$script_dir/bin/graywatchctl"
    reporter="$script_dir/bin/graywatch-report"
    bpf_helper="$script_dir/bin/graywatch-bpf"
else
    daemon=${GRAYWATCHD:-graywatchd}
    client=${GRAYWATCHCTL:-graywatchctl}
    reporter=${GRAYWATCH_REPORT:-graywatch-report}
    bpf_helper=${GRAYWATCH_BPF:-graywatch-bpf}
fi

timestamp=$(date -u +%Y%m%dT%H%M%SZ)
host=$(hostname 2>/dev/null || echo unknown)
if [ -n "$output" ] && [ -n "$output_dir" ]; then
    echo "use either --output or --output-dir" >&2
    exit 2
fi
if [ -n "$output_dir" ]; then
    mkdir -p -- "$output_dir" || exit 1
    output_dir=$(CDPATH= cd -- "$output_dir" && pwd)
fi
[ -n "$output" ] || output="${output_dir:-$PWD}/graywatch-support-${host}-${timestamp}.tar.gz"
work=$(mktemp -d "${TMPDIR:-/tmp}/graywatch-support.XXXXXX")
socket="$work/graywatch.sock"
daemon_pid=""
bpf_pid=""

cleanup() {
    for pid in $daemon_pid $bpf_pid; do
        kill "$pid" >/dev/null 2>&1 || true
        wait "$pid" 2>/dev/null || true
    done
    rm -rf "$work"
}
trap cleanup EXIT
# Without the exit the script would carry on after cleanup removed $work.
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM

# Record a failed optional step instead of aborting the whole collection: a
# partial archive is more useful to support than none.
note_error() {
    echo "$1" >> "$work/collection-errors.txt"
}

kernel_config() {
    config="/boot/config-$(uname -r)"
    if [ -r "$config" ]; then
        grep -E "^(# )?CONFIG_$1[= ]" "$config" 2>/dev/null || true
    elif [ -r /proc/config.gz ]; then
        gzip -dc /proc/config.gz 2>/dev/null | grep -E "^(# )?CONFIG_$1[= ]" || true
    fi
}

# enabled, disabled_by_default (CONFIG_PSI_DEFAULT_DISABLED without psi=1),
# disabled_by_cmdline (psi=0), not_built, or unknown.
psi_status() {
    if [ -r /proc/pressure/cpu ]; then echo enabled; return; fi
    case " $(cat /proc/cmdline 2>/dev/null) " in
        *" psi=0 "*) echo disabled_by_cmdline; return ;;
    esac
    psi=$(kernel_config PSI)
    default_disabled=$(kernel_config PSI_DEFAULT_DISABLED)
    case "$psi" in
        CONFIG_PSI=y)
            case "$default_disabled" in
                CONFIG_PSI_DEFAULT_DISABLED=y) echo disabled_by_default ;;
                *) echo unknown ;;
            esac
            ;;
        "# CONFIG_PSI is not set") echo not_built ;;
        *) echo unknown ;;
    esac
}

{
    echo "collected_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "duration_seconds=$duration"
    echo "hostname=$host"
    echo "uid=$(id -u)"
    echo "kernel_release=$(uname -r)"
    echo "cpus=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo unknown)"
    echo "psi_status=$(psi_status)"
    echo "cgroup_fs=$(stat -fc %T /sys/fs/cgroup 2>/dev/null || echo unknown)"
    echo "proc_mount=$(findmnt -n -o OPTIONS /proc 2>/dev/null || true)"
    echo "sys_mount=$(findmnt -n -o OPTIONS /sys 2>/dev/null || true)"
    echo "ignored_ib_ports=${ignored_ports# }"
    echo "kernel_cmdline=$(cat /proc/cmdline 2>/dev/null || true)"
    echo "uname=$(uname -a)"
    echo "uptime=$(uptime 2>/dev/null || true)"
    if [ -r /etc/os-release ]; then sed 's/^/os_/' /etc/os-release; fi
} > "$work/system.txt"

# An automatic collection records what asked for it. Moving the file first
# also re-arms the systemd path unit that started this run.
if [ -n "$trigger" ] && [ -f "$trigger" ]; then
    mv -f "$trigger" "$work/trigger.json" 2>/dev/null ||
        note_error "cannot move trigger file $trigger"
fi

# When graywatchd runs as a service with --record-dir, add what it recorded
# before this collection: every event of the last minutes, and its log of
# problem periods. A segment that is being compressed right now exists both
# plain and as a partial .gz; the plain file is the complete one.
if [ "$history_minutes" -gt 0 ] && [ -d "$history_dir" ]; then
    # Oldest first by modification time: a segment continued after a restart
    # (..._2.jsonl) must follow its first part.
    find "$history_dir" -maxdepth 1 -name 'events-*.jsonl*' -mmin "-$history_minutes" \
            -printf '%T@ %p\n' 2>/dev/null | sort -n | cut -d ' ' -f 2- |
        while IFS= read -r segment; do
            case "$segment" in
                *.gz) [ -e "${segment%.gz}" ] || gzip -dc "$segment" 2>/dev/null || true ;;
                *) cat "$segment" ;;
            esac
        done > "$work/history-events.jsonl"
    [ -s "$work/history-events.jsonl" ] || rm -f "$work/history-events.jsonl"
    {
        for rotated in $(find "$history_dir" -maxdepth 1 -name 'changes-*.jsonl*' \
                -printf '%T@ %p\n' 2>/dev/null | sort -n | cut -d ' ' -f 2-); do
            case "$rotated" in
                *.gz) [ -e "${rotated%.gz}" ] || gzip -dc "$rotated" 2>/dev/null || true ;;
                *) cat "$rotated" ;;
            esac
        done
        cat "$history_dir/changes.jsonl" 2>/dev/null || true
    } | tail -n 20000 > "$work/changes.jsonl"
    [ -s "$work/changes.jsonl" ] || rm -f "$work/changes.jsonl"
fi

lsblk -J -o NAME,KNAME,MAJ:MIN,TYPE,SIZE,ROTA,MODEL,MOUNTPOINT \
    > "$work/lsblk.json" 2>/dev/null || note_error "lsblk unavailable"
findmnt -rn -o TARGET,SOURCE,FSTYPE,OPTIONS > "$work/mounts.txt" 2>/dev/null ||
    note_error "findmnt unavailable"

daemon_args=""
for port in $ignored_ports; do daemon_args="$daemon_args --ignore-ib-port $port"; done
# shellcheck disable=SC2086 # word splitting of validated DEVICE/PORT values
"$daemon" --socket "$socket" --interval 1 --task-interval 5 $daemon_args \
    --log "$work/events.jsonl" > "$work/daemon.stdout" 2> "$work/daemon.stderr" &
daemon_pid=$!

ready=0
attempt=0
while [ "$attempt" -lt 50 ]; do
    if [ -S "$socket" ]; then ready=1; break; fi
    if ! kill -0 "$daemon_pid" 2>/dev/null; then break; fi
    attempt=$((attempt + 1))
    sleep 0.1
done
if [ "$ready" -ne 1 ]; then
    echo "graywatchd did not create its socket" >&2
    cat "$work/daemon.stderr" >&2 || true
    exit 1
fi

# Network interfaces backed by hardware (a PCI or other bus device), not
# loopback, bridges, VLANs or bonds.
physical_nics() {
    for path in /sys/class/net/*; do
        if [ -e "$path/device" ]; then echo "${path##*/}"; fi
    done
}

# Counters that only mean something as a change over the window: NIC
# statistics, per-CPU packet backlog drops, interrupts, CPU time (steal) and
# thermal throttling.
platform_counters() {
    ip -s -s -j link > "$work/links-$1.json" 2>/dev/null ||
        note_error "ip -j link unavailable"
    if command -v ethtool >/dev/null 2>&1; then
        for nic in $(physical_nics); do
            echo "== $nic"
            ethtool -S "$nic" 2>/dev/null || true
        done > "$work/ethtool-stats-$1.txt"
    fi
    cp /proc/net/softnet_stat "$work/softnet-$1.txt" 2>/dev/null || true
    cp /proc/interrupts "$work/interrupts-$1.txt" 2>/dev/null || true
    cp /proc/stat "$work/stat-$1.txt" 2>/dev/null || true
    for counter in /sys/devices/system/cpu/cpu[0-9]*/thermal_throttle/*_throttle_count; do
        if [ -r "$counter" ]; then echo "$counter $(cat "$counter")"; fi
    done > "$work/throttle-$1.txt" 2>/dev/null
    [ -s "$work/throttle-$1.txt" ] || rm -f "$work/throttle-$1.txt"
}

# How the platform is set up: NIC link settings, bonds, PCIe links of NICs
# and storage controllers, NUMA, and the block-layer settings that shape
# latency. Each line is KEY VALUE... so the report can compare devices.
platform_settings() {
    if command -v ethtool >/dev/null 2>&1; then
        for nic in $(physical_nics); do
            echo "== $nic"
            ethtool "$nic" 2>/dev/null || true
        done > "$work/ethtool-settings.txt"
    else
        note_error "ethtool not installed: no NIC counters or link settings"
    fi
    for bond in /proc/net/bonding/*; do
        [ -r "$bond" ] || continue
        echo "== ${bond##*/}"
        cat "$bond"
    done > "$work/bonding.txt" 2>/dev/null
    [ -s "$work/bonding.txt" ] || rm -f "$work/bonding.txt"
    {
        for path in /sys/class/net/*/device /sys/class/scsi_host/*/device \
                    /sys/class/nvme/*/device; do
            [ -e "$path" ] || continue
            pci=$(readlink -f "$path")
            # A SCSI host's device is the controller's PCI function's child.
            while [ -n "$pci" ] && [ "$pci" != / ] && [ ! -e "$pci/current_link_speed" ]; do
                pci=${pci%/*}
            done
            [ -e "$pci/current_link_speed" ] || continue
            owner=${path%/device}
            echo "pci ${pci##*/} ${owner##*/} driver=$(basename "$(readlink -f "$pci/driver" 2>/dev/null)" 2>/dev/null)" \
                "current_speed=$(tr ' ' '_' < "$pci/current_link_speed" 2>/dev/null)" \
                "max_speed=$(tr ' ' '_' < "$pci/max_link_speed" 2>/dev/null)" \
                "current_width=$(cat "$pci/current_link_width" 2>/dev/null)" \
                "max_width=$(cat "$pci/max_link_width" 2>/dev/null)" \
                "numa_node=$(cat "$pci/numa_node" 2>/dev/null)"
        done | awk '!seen[$2]++'  # one line per PCI function (a controller has many hosts)
        for nic in $(physical_nics); do
            master=""
            if [ -e "/sys/class/net/$nic/master" ]; then
                master=$(basename "$(readlink -f "/sys/class/net/$nic/master")")
            fi
            echo "nic $nic mtu=$(cat "/sys/class/net/$nic/mtu" 2>/dev/null)" \
                "operstate=$(cat "/sys/class/net/$nic/operstate" 2>/dev/null)" \
                "master=$master"
        done
        for node in /sys/devices/system/node/node[0-9]*; do
            if [ -r "$node/cpulist" ]; then
                echo "numa ${node##*node} cpus=$(cat "$node/cpulist")"
            fi
        done
        for queue in /sys/block/*/queue; do
            device=${queue%/queue}
            device=${device##*/}
            case "$device" in loop*|ram*|zram*) continue ;; esac
            scheduler=$(sed -n 's/.*\[\(.*\)\].*/\1/p' "$queue/scheduler" 2>/dev/null)
            echo "block $device rotational=$(cat "$queue/rotational" 2>/dev/null)" \
                "scheduler=${scheduler:-none}" \
                "wbt_lat_usec=$(cat "$queue/wbt_lat_usec" 2>/dev/null)" \
                "nr_requests=$(cat "$queue/nr_requests" 2>/dev/null)" \
                "read_ahead_kb=$(cat "$queue/read_ahead_kb" 2>/dev/null)" \
                "max_sectors_kb=$(cat "$queue/max_sectors_kb" 2>/dev/null)" \
                "write_cache=$(tr ' ' '_' < "$queue/write_cache" 2>/dev/null)"
        done
        for key in vm.dirty_ratio vm.dirty_background_ratio vm.dirty_bytes \
                   vm.dirty_background_bytes vm.swappiness vm.zone_reclaim_mode \
                   net.netfilter.nf_conntrack_max net.netfilter.nf_conntrack_count \
                   net.core.netdev_max_backlog; do
            if value=$(sysctl -n "$key" 2>/dev/null); then echo "sysctl $key $value"; fi
        done
        for cpu in /sys/devices/system/cpu/cpu0/cpufreq; do
            [ -d "$cpu" ] || continue
            echo "cpufreq driver=$(cat "$cpu/scaling_driver" 2>/dev/null)" \
                "governor=$(cat "$cpu/scaling_governor" 2>/dev/null)" \
                "max_khz=$(cat "$cpu/cpuinfo_max_freq" 2>/dev/null)"
        done
    } > "$work/platform.txt" 2>/dev/null
}

snapshot() {
    for resource in cpu io memory network fuse; do
        "$client" status --socket "$socket" --resource "$resource" \
            > "$work/$resource-$1.json" 2>> "$work/collection-errors.txt" ||
            note_error "status $resource $1 failed"
    done
    cp /proc/meminfo "$work/meminfo-$1.txt" 2>/dev/null || true
    cp /proc/vmstat "$work/vmstat-$1.txt" 2>/dev/null || true
    platform_counters "$1"
}

# The eBPF helper needs root and kernel BTF. It stops by itself after the
# window, and the collection continues without it if it cannot load.
if [ "$bpf" -eq 1 ]; then
    if [ "$(id -u)" -ne 0 ]; then
        note_error "eBPF collectors skipped: not running as root"
    elif [ ! -r /sys/kernel/btf/vmlinux ]; then
        note_error "eBPF collectors skipped: /sys/kernel/btf/vmlinux missing"
    elif ! command -v "$bpf_helper" >/dev/null 2>&1; then
        note_error "eBPF collectors skipped: graywatch-bpf not in this bundle"
    else
        "$bpf_helper" --interval 1 --duration $((duration + 2)) \
            > "$work/bpf-events.jsonl" 2> "$work/bpf.stderr" &
        bpf_pid=$!
    fi
fi

platform_settings
sleep 2
snapshot initial
echo "Collecting for ${duration}s; press Ctrl-C to abort." >&2
sleep "$duration"
if ! kill -0 "$daemon_pid" 2>/dev/null; then
    note_error "graywatchd exited before the collection window ended"
fi
snapshot final

for pid in $daemon_pid $bpf_pid; do
    kill "$pid" >/dev/null 2>&1 || true
    wait "$pid" 2>/dev/null || true
done
daemon_pid=""
bpf_pid=""
if [ -f "$work/bpf-events.jsonl" ] && [ ! -s "$work/bpf-events.jsonl" ]; then
    note_error "eBPF helper produced no events: $(head -n 1 "$work/bpf.stderr" 2>/dev/null)"
fi

if [ "$kernel_log" -eq 1 ]; then
    # Kernel messages carry I/O errors, hung-task reports, OOM kills and link
    # flaps. dmesg_restrict can deny this to non-root users.
    { dmesg --time-format iso 2>/dev/null || dmesg 2>/dev/null; } |
        tail -n 5000 > "$work/kernel.log" ||
        note_error "kernel log unavailable"
    [ -s "$work/kernel.log" ] || note_error "kernel log empty or not readable"
fi

rm -f "$socket"
if command -v "$reporter" >/dev/null 2>&1; then
    "$reporter" "$work" > "$work/report.txt" 2>> "$work/collection-errors.txt" ||
        note_error "graywatch-report failed"
fi
tar -C "$work" -czf "$output" .
# A copy of the report next to the archive, so nobody has to unpack it to read
# it, even when a browser or upload portal renames or decompresses the archive.
if [ -s "$work/report.txt" ]; then
    report_copy="${output%.tar.gz}"
    report_copy="${report_copy%.tgz}-report.txt"
    cp "$work/report.txt" "$report_copy" && echo "Report: $report_copy" >&2
fi
# Automatic collections keep only the newest archives (names sort by time).
if [ -n "$keep_archives" ]; then
    find "$output_dir" -maxdepth 1 -name 'graywatch-support-*.tar.gz' | sort -r |
        tail -n "+$((keep_archives + 1))" |
        while IFS= read -r old; do
            rm -f -- "$old" "${old%.tar.gz}-report.txt"
        done
fi
echo "$output"
