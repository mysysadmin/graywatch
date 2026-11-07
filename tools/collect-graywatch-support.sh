#!/bin/sh
set -eu

duration=60
output=""
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
        *)
            echo "usage: $0 [--duration SECONDS] [--output ARCHIVE.tar.gz]" >&2
            exit 2
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
else
    daemon=${GRAYWATCHD:-graywatchd}
    client=${GRAYWATCHCTL:-graywatchctl}
fi

timestamp=$(date -u +%Y%m%dT%H%M%SZ)
host=$(hostname 2>/dev/null || echo unknown)
[ -n "$output" ] || output="$PWD/graywatch-support-${host}-${timestamp}.tar.gz"
work=$(mktemp -d "${TMPDIR:-/tmp}/graywatch-support.XXXXXX")
socket="$work/graywatch.sock"
daemon_pid=""

cleanup() {
    if [ -n "$daemon_pid" ]; then
        kill "$daemon_pid" >/dev/null 2>&1 || true
        wait "$daemon_pid" 2>/dev/null || true
    fi
    rm -rf "$work"
}
trap cleanup EXIT INT TERM

{
    echo "collected_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "duration_seconds=$duration"
    echo "uid=$(id -u)"
    uname -a
    uptime 2>/dev/null || true
    [ -r /etc/os-release ] && cat /etc/os-release
    echo "proc_mount=$(findmnt -n -o OPTIONS /proc 2>/dev/null || true)"
    echo "sys_mount=$(findmnt -n -o OPTIONS /sys 2>/dev/null || true)"
} > "$work/system.txt"

"$daemon" --socket "$socket" --interval 1 --task-interval 5 \
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
    exit 1
fi

sleep 2
"$client" status --socket "$socket" --resource cpu > "$work/cpu-initial.json"
"$client" status --socket "$socket" --resource io > "$work/io-initial.json"
"$client" status --socket "$socket" --resource network > "$work/network-initial.json"
sleep "$duration"
"$client" status --socket "$socket" --resource cpu > "$work/cpu-final.json"
"$client" status --socket "$socket" --resource io > "$work/io-final.json"
"$client" status --socket "$socket" --resource network > "$work/network-final.json"

kill "$daemon_pid" >/dev/null 2>&1 || true
wait "$daemon_pid" 2>/dev/null || true
daemon_pid=""

tar -C "$work" -czf "$output" .
echo "$output"
