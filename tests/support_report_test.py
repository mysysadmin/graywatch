"""Exercise tools/graywatch-report.py against a synthetic support archive."""

import importlib.util
import io
import json
import os
import pathlib
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
TOOL = ROOT / "tools" / "graywatch-report.py"
spec = importlib.util.spec_from_file_location("graywatch_report", TOOL)
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


def ts(second):
    return "2026-11-07T21:%02d:%02d.000Z" % (second // 60, second % 60)


def cpu_event(second, blocked):
    fresh = second % 5 == 0
    threads = []
    if blocked:
        threads.append({"pid": 4242, "tid": 4243, "uid": 1000, "command": "app",
                        "state": "D", "wait_channel": "request_wait_answer",
                        "cpu_ms": 1.0, "runqueue_wait_ms": 0.0,
                        "runnable_wait_percent": 0.0, "timeslices": 1})
    threads.append({"pid": 77, "tid": 77, "uid": 0, "command": "kworker",
                    "state": "S", "wait_channel": "", "cpu_ms": 10.0,
                    "runqueue_wait_ms": 2.0, "runnable_wait_percent": 16.0,
                    "timeslices": 4})
    return {"schema_version": 1, "event_type": "resource_pressure",
            "timestamp": ts(second)[:-5] + ".642Z", "resource": "cpu", "condition": "unavailable",
            "severity": 0.0, "interval_seconds": 1.0,
            "evidence": {"psi_available": False, "psi_some_stall_percent": 0.0,
                         "psi_full_stall_percent": 0.0},
            "selection": {"kind": "system", "value": "all",
                          "attribution_available": second >= 5,
                          "attribution_interval_seconds": 5.0,
                          "attribution_age_seconds": 0.0 if fresh else float(second % 5),
                          "observed_threads": 900, "active_threads": 50,
                          "waiting_threads": 20, "blocked_threads": len(threads) - 1},
            "affected_threads": threads}


def io_event(second, name, latency):
    condition = "degraded" if latency >= 100 else "healthy"
    return {"schema_version": 1, "event_type": "resource_health",
            "timestamp": ts(second), "resource": "io", "condition": condition,
            "severity": 0.8 if condition == "degraded" else 0.02,
            "interval_seconds": 1.0,
            "device": {"name": name, "major": 253, "minor": 0, "model": ""},
            "evidence": {"source": "sysfs_block_stat",
                         "assessment_reason": "absolute_latency" if latency >= 100
                         else "responsive",
                         "maximum_average_latency_ms": latency,
                         "read_iops": 10.0, "write_iops": 0.0, "discard_iops": 0.0,
                         "flush_iops": 0.0, "read_bytes_per_second": 4096.0,
                         "write_bytes_per_second": 0.0,
                         "average_read_latency_ms": latency,
                         "average_write_latency_ms": 0.0, "busy_percent": 10.0,
                         "average_queue_depth": 0.1, "io_in_progress": 0}}


def memory_event(second, oom):
    return {"schema_version": 1, "event_type": "resource_health",
            "timestamp": ts(second), "resource": "memory",
            "condition": "degraded" if oom else "healthy",
            "severity": 0.95 if oom else 0.01, "interval_seconds": 1.0,
            "evidence": {"assessment_reason": "oom_kill" if oom else "no_pressure",
                         "total_kib": 1048576, "available_kib": 524288,
                         "available_percent": 50.0, "direct_reclaim_stalls": 0,
                         "compaction_stalls": 0, "swap_in_pages_per_second": 0.0,
                         "major_faults_per_second": 0.0, "oom_kills": 1 if oom else 0,
                         "psi_available": False}}


def ib_event(second, port, state, condition):
    return {"schema_version": 1, "event_type": "resource_health",
            "timestamp": ts(second), "resource": "network", "condition": condition,
            "severity": 0.0, "interval_seconds": 1.0,
            "interface": {"kind": "infiniband", "device": "mlx4_0", "port": port},
            "evidence": {"in_use": condition != "inactive", "state": state,
                         "physical_state": "LinkUp", "rate": "56 Gb/sec",
                         "receive_errors": 0, "transmit_discards": 0,
                         "symbol_errors": 0, "link_downed": 0, "transmit_wait": 0}}


def build_bundle(directory):
    events = []
    for second in range(1, 41):
        events.append(cpu_event(second, blocked=10 <= second <= 35))
        events.append(memory_event(second, oom=second == 20))
        events.append(io_event(second, "nvme0n1", 0.3))
        events.append(io_event(second, "dm-0", 250.0 if 12 <= second <= 16 else 0.4))
        events.append(ib_event(second, 1, "ACTIVE", "healthy"))
        events.append(ib_event(second, 2, "DOWN", "inactive"))
    files = {
        "events.jsonl": "\n".join(json.dumps(e) for e in events) + "\n",
        "system.txt": "collected_utc=2026-11-07T21:00:00Z\nduration_seconds=40\n"
                      "hostname=nodea\nuid=0\nkernel_release=5.14.0\ncpus=32\n"
                      "psi_status=disabled_by_default\nproc_mount=rw,nosuid\n"
                      'os_PRETTY_NAME="Rocky Linux 9.6"\n',
        "lsblk.json": json.dumps({"blockdevices": [
            {"name": "nvme0n1", "kname": "nvme0n1", "type": "disk", "children": [
                {"name": "vg-data", "kname": "dm-0", "type": "lvm"}]}]}),
        "kernel.log": "2026-11-01T08:00:00,000000+01:00 Out of memory: Killed "
                      "process 12 (old)\n"
                      "2026-11-07T20:59:00,000000+00:00 INFO: task app:4243 blocked for more "
                      "than 122 seconds.\n2026-11-07T22:00:20,5+01:00 Out of memory: "
                      "Killed process 999 (hog)\n",
        "daemon.stderr": "",
    }
    for name, text in files.items():
        pathlib.Path(directory, name).write_text(text)


class ReportTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = self.temporary.name
        build_bundle(self.directory)

    def tearDown(self):
        self.temporary.cleanup()

    def analyse(self):
        return report.Analysis(report.load_bundle(self.directory)).run()

    def titles(self, analysis):
        return [(f.severity, f.title) for f in analysis.findings]

    def test_findings_and_ranking(self):
        analysis = self.analyse()
        titles = self.titles(analysis)
        self.assertEqual(analysis.findings[0].severity, report.CRITICAL)
        self.assertIn((report.CRITICAL, "OOM killer ran during the window"), titles)
        self.assertTrue(any("uninterruptible" in t for _, t in titles))
        blocked = [f for f in analysis.findings if "uninterruptible" in f.title][0]
        self.assertEqual(blocked.severity, report.CRITICAL)  # stuck >= 15s
        self.assertTrue(any("FUSE" in step for step in blocked.next_steps))
        self.assertTrue(any("PSI" in t for _, t in titles))

    def test_stacked_device_is_attributed_to_dm_layer(self):
        analysis = self.analyse()
        device = [f for f in analysis.findings if f.area == "io"]
        self.assertEqual(len(device), 1)
        self.assertIn("dm-0 (vg-data) on nvme0n1", device[0].title)
        self.assertTrue(any("stacking layer" in s for s in device[0].next_steps))

    def test_unused_port_is_informational(self):
        analysis = self.analyse()
        ports = [f for f in analysis.findings if "InfiniBand" in f.title]
        self.assertEqual([(f.severity, f.title) for f in ports],
                         [(report.INFO, "Unused InfiniBand ports ignored")])

    def test_fuse_conclusion_requires_healthy_disks(self):
        analysis = self.analyse()
        self.assertFalse(any("above the local block layer" in line
                             for line in analysis.facts["conclusions"]))
        events = pathlib.Path(self.directory, "events.jsonl")
        lines = [l for l in events.read_text().splitlines()
                 if '"dm-0"' not in l]
        events.write_text("\n".join(lines) + "\n")
        analysis = self.analyse()
        self.assertTrue(any("above the local block layer" in line
                            for line in analysis.facts["conclusions"]))

    def test_psi_help_matches_status(self):
        analysis = self.analyse()
        psi = [f for f in analysis.findings if "PSI" in f.title][0]
        self.assertTrue(any("grubby --update-kernel=ALL --args=psi=1" in step
                            for step in psi.next_steps))

    def test_kernel_log_patterns(self):
        analysis = self.analyse()
        self.assertEqual(analysis.facts["kernel_log_matches"],
                         {"hung_task": 1, "oom": 2})
        oom = [f for f in analysis.findings if f.title.startswith("Kernel log: oom")]
        self.assertEqual(len(oom), 1)
        self.assertEqual(oom[0].severity, report.CRITICAL)
        self.assertTrue(any("1 older line" in d for d in oom[0].details))

    def test_old_kernel_events_are_history(self):
        pathlib.Path(self.directory, "kernel.log").write_text(
            "2026-10-01T08:00:00,000000+00:00 Out of memory: Killed process 12\n")
        analysis = self.analyse()
        oom = [f for f in analysis.findings if "Kernel log" in f.title]
        self.assertEqual([(f.severity, f.title) for f in oom],
                         [(report.INFO, "Kernel log: earlier oom (1 line, before the window)")])

    def test_cli_reads_tarball_in_all_formats(self):
        archive = os.path.join(self.directory, "bundle.tar.gz")
        with tarfile.open(archive, "w:gz") as handle:
            for name in os.listdir(self.directory):
                if name != "bundle.tar.gz":
                    handle.add(os.path.join(self.directory, name), arcname="./" + name)
        for fmt in ("text", "markdown", "json"):
            result = subprocess.run(
                [sys.executable, "-I", str(TOOL), archive, "--format", fmt],
                capture_output=True, text=True, check=True)
            self.assertIn("nodea", result.stdout)
        text = subprocess.run([sys.executable, "-I", str(TOOL), archive],
                              capture_output=True, text=True, check=True).stdout
        self.assertIn("Verdict: CRITICAL", text)
        self.assertIn("Timeline (UTC)", text)
        json.loads(subprocess.run(
            [sys.executable, "-I", str(TOOL), archive, "--format", "json"],
            capture_output=True, text=True, check=True).stdout)

    def test_rejects_non_bundle(self):
        empty = tempfile.mkdtemp(dir=self.directory)
        result = subprocess.run([sys.executable, "-I", str(TOOL), empty],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)


if __name__ == "__main__":
    unittest.main()
