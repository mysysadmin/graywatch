#!/bin/sh
set -eu

duration=60
output=""
kernel_log=1
bpf=1
ignored_ports=""
usage() {
    echo "usage: $0 [--duration SECONDS] [--output ARCHIVE.tar.gz]" \
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
[ -n "$output" ] || output="$PWD/graywatch-support-${host}-${timestamp}.tar.gz"
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
trap cleanup EXIT INT TERM

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

snapshot() {
    for resource in cpu io memory network fuse; do
        "$client" status --socket "$socket" --resource "$resource" \
            > "$work/$resource-$1.json" 2>> "$work/collection-errors.txt" ||
            note_error "status $resource $1 failed"
    done
    cp /proc/meminfo "$work/meminfo-$1.txt" 2>/dev/null || true
    cp /proc/vmstat "$work/vmstat-$1.txt" 2>/dev/null || true
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
echo "$output"
