#!/usr/bin/env python3
"""Summarize current Graywatch state from several hosts over SSH."""

import argparse
import concurrent.futures
import json
import subprocess
import sys


def remote_json(host: str, resource: str, timeout: float, user):
    destination = f"{user}@{host}" if user else host
    command = [
        "ssh", "-o", "BatchMode=yes", "-o", f"ConnectTimeout={int(timeout)}",
        destination, "/usr/bin/graywatchctl", "status", "--resource", resource,
    ]
    result = subprocess.run(command, capture_output=True, text=True,
                            timeout=timeout, check=True)
    return json.loads(result.stdout)


def inspect(host: str, timeout: float, user):
    try:
        cpu = remote_json(host, "cpu", timeout, user)
        devices = remote_json(host, "io", timeout, user)
        network = remote_json(host, "network", timeout, user)
        selection = cpu["selection"]
        affected = cpu.get("affected_threads", [])
        top_thread = max(affected, key=lambda item: item["runqueue_wait_ms"],
                         default=None)
        active_devices = [item for item in devices
                          if item["condition"] != "idle"]
        busiest = max(active_devices,
                      key=lambda item: item["evidence"]["busy_percent"],
                      default=None)
        return {
            "host": host,
            "reachable": True,
            "cpu_condition": cpu["condition"],
            "cpu_severity": cpu["severity"],
            "psi_available": cpu["evidence"]["psi_available"],
            "observed_threads": selection["observed_threads"],
            "waiting_threads": selection["waiting_threads"],
            "attribution_age_seconds": selection["attribution_age_seconds"],
            "top_waiting_thread": top_thread,
            "device_count": len(devices),
            "active_device_count": len(active_devices),
            "busiest_device": busiest,
            "network_event_count": len(network),
            "network_issues": [item for item in network
                               if item["condition"] != "healthy"],
        }
    except Exception as error:  # support tool must report partial cluster state
        return {"host": host, "reachable": False, "error": str(error)}


def compact_thread(value):
    if not value:
        return "-"
    return f'{value["command"]}:{value["runqueue_wait_ms"]:.1f}ms'


def compact_device(value):
    if not value:
        return "-"
    evidence = value["evidence"]
    return (f'{value["device"]["name"]}:{evidence["busy_percent"]:.1f}% '
            f'r={evidence["average_read_latency_ms"]:.2f}ms '
            f'w={evidence["average_write_latency_ms"]:.2f}ms')


def compact_network(values):
    if not values:
        return "healthy"
    value = values[0]
    interface = value["interface"]
    if interface["kind"] == "infiniband":
        identity = f'{interface["device"]}/{interface["port"]}'
    else:
        identity = interface["kind"]
    suffix = f' +{len(values) - 1}' if len(values) > 1 else ''
    return f'{identity}:{value["condition"]}{suffix}'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("hosts", nargs="+", help="SSH host names or addresses")
    parser.add_argument("--timeout", type=float, default=8.0)
    parser.add_argument("--user", help="SSH user (for example, root)")
    parser.add_argument("--json", action="store_true", dest="as_json")
    arguments = parser.parse_args()
    with concurrent.futures.ThreadPoolExecutor(
            max_workers=min(len(arguments.hosts), 16)) as executor:
        futures = [executor.submit(inspect, host, arguments.timeout,
                                   arguments.user)
                   for host in arguments.hosts]
        rows = [future.result() for future in futures]

    if arguments.as_json:
        json.dump(rows, sys.stdout, indent=2)
        print()
        return

    print(f'{"HOST":<22} {"CPU/PSI":<14} {"WAIT":>8} '
          f'{"TOP THREAD":<28} {"BUSIEST DEVICE":<34} NETWORK')
    for row in rows:
        if not row["reachable"]:
            print(f'{row["host"]:<22} {"UNREACHABLE":<14} {"-":>8} '
                  f'{row["error"]}')
            continue
        cpu = (f'{row["cpu_condition"]}/'
               f'{"yes" if row["psi_available"] else "no-psi"}')
        print(f'{row["host"]:<22} {cpu:<14} '
              f'{row["waiting_threads"]:>8} '
              f'{compact_thread(row["top_waiting_thread"]):<28} '
              f'{compact_device(row["busiest_device"]):<34} '
              f'{compact_network(row["network_issues"])}')


if __name__ == "__main__":
    main()
