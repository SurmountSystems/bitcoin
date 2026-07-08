#!/usr/bin/env python3
"""Unit tests for contrib/swords/phase0_live_health.py and phase0-live-health.sh."""
from __future__ import annotations

import importlib.util
import os
import shutil
import signal
import subprocess
import tempfile
import time
import unittest
import unittest.mock
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "contrib" / "swords" / "phase0_live_health.py"
SCRIPT_PATH = ROOT / "contrib" / "swords" / "phase0-live-health.sh"

# Frozen pidstat -u output (pid 12345): sample row has PID in column 4.
PIDSTAT_CPU_FIXTURE = """\
Linux 6.14.2-zen1-1-zen (host)  07/06/2025  _x86_64_    (8 CPU)

07:06:45 PM   UID       PID    %usr %system  %guest   %wait    %CPU   CPU  Command
07:06:46 PM     0     12345   12.00    3.00    0.00    0.00   15.00     2  bitcoind

Average:        0     12345   11.00    2.00    0.00    0.00   13.00     -  bitcoind
"""

# UID 12345 would false-match old $3==pid logic on a sample row.
PIDSTAT_CPU_UID_COLLISION = """\
07:06:46 PM  12345      9999   99.00   99.00    0.00    0.00   99.00     0  other
07:06:46 PM     0     12345   12.00    3.00    0.00    0.00   15.00     2  bitcoind
"""

RUN_STARTED = (
    "2026-07-06T16:00:00Z === Swords run started 2026-07-06T16:00:00Z "
    "(datadir=/tmp/.bitcoin-swords benchstats=1) ==="
)


def benchstats_line(
    ts: str,
    *,
    readers_full: int = 0,
    prefetch_hit: int = 0,
    coin_prevouts: int = 0,
    txindex_ms: float = 0.0,
) -> str:
    return (
        f"{ts} benchstats: block disk=5000.0ms decompress=0.0ms par_jobs=0 "
        f"prefetch_hit={prefetch_hit} prefetch_wait=0.0ms | "
        f"coin prevouts={coin_prevouts} miss=0 lmdb=0.0ms "
        "decode=0.0ms warm=0.0ms skip_thr=0 skip_wrk=0 "
        f"readers_full={readers_full} | "
        "flush chainstate=0.0ms block_index=0.0ms encode=0.0ms lmdb=0.0ms stale=0 "
        f"parallel=0 txindex={txindex_ms:.1f}ms blkidx_sync=0 connect_cs=0.0ms | "
        "wait cs_main_hold=0.0ms flush_mutex_wait=0.0ms blkidx_mutex_wait=0.0ms "
        "abc_idle=0.0ms script_jobs=0 script_done=0 | blocks=1000"
    )


PIDSTAT_MEM_FIXTURE = """\
07:06:45 PM   UID       PID  minflt/s majflt/s     VSZ     RSS  %MEM  Command
07:06:46 PM     0     12345      0.00     0.00  2048000  512000  1.23  bitcoind

Average:        0     12345      0.00     0.00  1999999  499999  1.20  bitcoind
"""


def load_module():
    spec = importlib.util.spec_from_file_location("phase0_live_health", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class Phase0LiveHealthTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mod = load_module()

    def test_watch_csv_header_columns(self):
        self.assertEqual(
            self.mod.WATCH_CSV_HEADER,
            "timestamp_utc,block_height,cpu_pct,usr,sys,vsz,rss",
        )
        self.assertEqual(len(self.mod.WATCH_CSV_COLUMNS), 7)

    def test_format_watch_row(self):
        row = self.mod.format_watch_row(
            "2026-07-06T12:00:00Z",
            "200000",
            "15.00",
            "12.00",
            "3.00",
            "2048000",
            "512000",
        )
        self.assertEqual(
            row,
            "2026-07-06T12:00:00Z,200000,15.00,12.00,3.00,2048000,512000",
        )

    def test_watch_csv_path_default_and_override(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stamp = "20260706T120000Z"
            default = self.mod.watch_csv_path(root, stamp)
            self.assertEqual(default, root / "phase0-watch-20260706T120000Z.csv")
            self.assertTrue(default.parent.is_dir())

            custom = root / "custom.csv"
            self.assertEqual(
                self.mod.watch_csv_path(root, stamp, custom),
                custom,
            )

    def test_snapshot_path_default_and_override(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stamp = "20260706T120000Z"
            default = self.mod.snapshot_path(root, stamp)
            self.assertEqual(
                default,
                root / "phase0-snapshot-20260706T120000Z.txt",
            )
            custom = root / "snap.txt"
            self.assertEqual(self.mod.snapshot_path(root, stamp, custom), custom)

    def test_profile_paths(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stamp = "20260706T120000Z"
            run_dir = self.mod.profile_run_dir(root, stamp)
            perf = self.mod.profile_perf_data(root, stamp)
            self.assertEqual(run_dir, root / stamp)
            self.assertEqual(perf, run_dir / "perf.data")

    def test_meta_txt_roundtrip(self):
        fields = {
            "captured_at_utc": "20260706T120000Z",
            "run_finished_at_utc": "20260706T120100Z",
            "subcommand": "profile",
            "block_height": "250000",
            "datadir": "/home/user/.bitcoin-swords",
            "pid": "12345",
            "duration_seconds": "60",
            "perf_version": "perf version 6.8",
            "profile_logs": "/home/user/.bitcoin-swords-profile-logs",
            "perf_data": "/home/user/.bitcoin-swords-profile-logs/20260706T120000Z/perf.data",
            "interrupted": "false",
        }
        text = self.mod.format_meta_txt(fields)
        parsed = self.mod.parse_meta_txt(text)
        for key in self.mod.META_TXT_FIELDS:
            if key in fields:
                self.assertEqual(parsed[key], fields[key])

    def test_parse_pidstat_cpu_sample_prefers_instantaneous_row(self):
        usr, sys_pct, cpu_pct = self.mod.parse_pidstat_cpu_output(
            PIDSTAT_CPU_FIXTURE, "12345"
        )
        self.assertEqual(usr, "12.00")
        self.assertEqual(sys_pct, "3.00")
        self.assertEqual(cpu_pct, "15.00")

    def test_parse_pidstat_cpu_not_guest_column(self):
        usr, sys_pct, cpu_pct = self.mod.parse_pidstat_cpu_output(
            PIDSTAT_CPU_FIXTURE, "12345"
        )
        self.assertNotEqual(cpu_pct, "0.00")
        self.assertNotEqual(usr, "12345")

    def test_parse_pidstat_cpu_uid_collision(self):
        usr, sys_pct, cpu_pct = self.mod.parse_pidstat_cpu_output(
            PIDSTAT_CPU_UID_COLLISION, "12345"
        )
        self.assertEqual(usr, "12.00")
        self.assertEqual(cpu_pct, "15.00")

    def test_parse_pidstat_cpu_average_fallback(self):
        average_only = "Average:        0     12345   11.00    2.00    0.00    0.00   13.00     -  bitcoind\n"
        usr, sys_pct, cpu_pct = self.mod.parse_pidstat_cpu_output(average_only, "12345")
        self.assertEqual(usr, "11.00")
        self.assertEqual(sys_pct, "2.00")
        self.assertEqual(cpu_pct, "13.00")

    def test_parse_pidstat_mem_sample_prefers_instantaneous_row(self):
        vsz, rss = self.mod.parse_pidstat_mem_output(PIDSTAT_MEM_FIXTURE, "12345")
        self.assertEqual(vsz, "2048000")
        self.assertEqual(rss, "512000")

    def test_pgrep_datadir_pattern_escapes_dots(self):
        pat = self.mod.pgrep_datadir_pattern("/home/user/.bitcoin-swords")
        self.assertIn(r"\.bitcoin\-swords", pat)
        self.assertTrue(pat.endswith(r"/\.bitcoin\-swords"))

    def test_resolve_env_config_reads_swords_env(self):
        with tempfile.TemporaryDirectory() as tmp:
            dd = Path(tmp) / "datadir"
            logs = Path(tmp) / "logs"
            env = {
                "SWORDS_DATADIR": str(dd),
                "SWORDS_LOG_ARCHIVE": str(logs),
                "INTERVAL": "7",
                "DURATION": "30",
            }
            with unittest.mock.patch.dict(os.environ, env, clear=False):
                cfg = self.mod.resolve_env_config()
            self.assertEqual(cfg.datadir, dd)
            self.assertEqual(cfg.profile_logs, logs)
            self.assertEqual(cfg.interval, 7)
            self.assertEqual(cfg.duration, 30)

    def test_shell_export_config_quoting(self):
        cfg = self.mod.Phase0Config(
            subcommand="",
            datadir=Path("/tmp/has spaces/.bitcoin-swords"),
            profile_logs=Path("/tmp/logs"),
            interval=10,
            duration=None,
            out=None,
            bitcoin_cli=None,
        )
        exported = self.mod.shell_export_config(cfg)
        self.assertIn("'", exported)
        self.assertIn("/tmp/has spaces/.bitcoin-swords", exported)

    def test_parse_argv_watch_defaults(self):
        with tempfile.TemporaryDirectory() as tmp:
            datadir = Path(tmp) / "datadir"
            logs = Path(tmp) / "logs"
            cfg = self.mod.parse_argv(
                ["watch", "--datadir", str(datadir), "--profile-logs", str(logs)]
            )
            self.assertEqual(cfg.subcommand, "watch")
            self.assertEqual(cfg.datadir, datadir)
            self.assertEqual(cfg.profile_logs, logs)
            self.assertEqual(cfg.interval, 10)
            self.assertIsNone(cfg.duration)
            self.assertIsNone(cfg.out)

    def test_parse_argv_watch_with_duration_and_out(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "watch.csv"
            cfg = self.mod.parse_argv(
                [
                    "watch",
                    "--interval",
                    "5",
                    "--duration",
                    "120",
                    "--out",
                    str(out),
                ]
            )
            self.assertEqual(cfg.interval, 5)
            self.assertEqual(cfg.duration, 120)
            self.assertEqual(cfg.out, out)

    def test_parse_argv_profile(self):
        cfg = self.mod.parse_argv(["profile", "--duration", "90"])
        self.assertEqual(cfg.subcommand, "profile")
        self.assertEqual(cfg.duration, 90)

    def test_parse_argv_profile_duration_from_env(self):
        with unittest.mock.patch.dict(os.environ, {"DURATION": "120"}, clear=False):
            cfg = self.mod.parse_argv(["profile"])
        self.assertEqual(cfg.duration, 120)

    def test_parse_argv_snapshot(self):
        cfg = self.mod.parse_argv(["snapshot"])
        self.assertEqual(cfg.subcommand, "snapshot")

    def test_parse_argv_help(self):
        cfg = self.mod.parse_argv(["help"])
        self.assertEqual(cfg.subcommand, "help")

    def test_usage_text_documents_timestamp_formats(self):
        text = self.mod.usage_text("/home/user/.bitcoin-swords")
        self.assertIn("just watch-cpu", text)
        self.assertIn("just profile-ibd", text)
        self.assertIn("just phase0-snapshot", text)
        self.assertIn("just phase0-check", text)
        self.assertIn("MDB_READERS_FULL", text)
        self.assertIn("top -H", text)
        self.assertIn("2026-07-06T15:30:55Z", text)
        self.assertIn("20260706T153045Z", text)
        self.assertIn("2026-07-06T15-30Z", text)
        self.assertIn("reset-datadir", text)
        self.assertIn(r"/home/user/\.bitcoin\-swords", text)

    def test_parse_argv_check(self):
        with tempfile.TemporaryDirectory() as tmp:
            datadir = Path(tmp) / "datadir"
            datadir.mkdir()
            cfg = self.mod.parse_argv(
                [
                    "check",
                    "--datadir",
                    str(datadir),
                    "--txindex-warn-ms",
                    "400",
                    "--prefetch-rollups",
                    "2",
                ]
            )
            self.assertEqual(cfg.subcommand, "check")
            self.assertEqual(cfg.txindex_warn_ms, 400.0)
            self.assertEqual(cfg.prefetch_rollups, 2)

    def test_parse_argv_check_uses_env_thresholds(self):
        with tempfile.TemporaryDirectory() as tmp:
            datadir = Path(tmp) / "datadir"
            datadir.mkdir()
            env = {
                "TXINDEX_WARN_MS": "400",
                "PREFETCH_ROLLUPS": "5",
                "PREFETCH_MIN_PREVOUTS": "80",
            }
            with unittest.mock.patch.dict(os.environ, env, clear=False):
                cfg = self.mod.parse_argv(["check", "--datadir", str(datadir)])
            self.assertEqual(cfg.subcommand, "check")
            self.assertEqual(cfg.txindex_warn_ms, 400.0)
            self.assertEqual(cfg.prefetch_rollups, 5)
            self.assertEqual(cfg.prefetch_min_prevouts, 80)

    def test_health_check_ok(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "debug.log"
            log.write_text(
                "\n".join(
                    [
                        RUN_STARTED,
                        benchstats_line("2026-07-06T16:01:40Z"),
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            code, report = self.mod.run_health_check(log)
            self.assertEqual(code, 0)
            self.assertEqual(report["status"], "OK")

    def test_health_check_fail_readers_full(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "debug.log"
            log.write_text(
                "\n".join(
                    [
                        RUN_STARTED,
                        benchstats_line("2026-07-06T16:01:40Z", readers_full=3),
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            code, report = self.mod.run_health_check(log)
            self.assertEqual(code, 1)
            self.assertEqual(report["status"], "FAIL")
            self.assertTrue(any("readers_full=3" in f for f in report["failures"]))

    def test_health_check_fail_mdb_readers_full_log_line(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "debug.log"
            log.write_text(
                "\n".join(
                    [
                        RUN_STARTED,
                        "2026-07-06T16:00:05Z LMDB MDB_READERS_FULL in read transaction begin: "
                        "Environment maxreaders limit reached (-30790)",
                        benchstats_line("2026-07-06T16:01:40Z"),
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            code, report = self.mod.run_health_check(log)
            self.assertEqual(code, 1)
            self.assertTrue(any("MDB_READERS_FULL" in f for f in report["failures"]))

    def test_health_check_warn_txindex(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "debug.log"
            log.write_text(
                "\n".join(
                    [
                        RUN_STARTED,
                        benchstats_line("2026-07-06T16:01:40Z", txindex_ms=600000.0),
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            code, report = self.mod.run_health_check(log, txindex_warn_ms=500.0)
            self.assertEqual(code, 0)
            self.assertEqual(report["status"], "WARN")
            self.assertTrue(any("txindex_per_blk_ms" in w for w in report["warnings"]))

    def test_health_check_ignores_mdb_before_run_start(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "debug.log"
            log.write_text(
                "\n".join(
                    [
                        "2026-07-06T15:00:00Z LMDB MDB_READERS_FULL in read transaction begin: "
                        "Environment maxreaders limit reached (-30790)",
                        RUN_STARTED,
                        benchstats_line("2026-07-06T16:01:40Z"),
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            code, report = self.mod.run_health_check(log)
            self.assertEqual(code, 0)
            self.assertEqual(report["status"], "OK")
            self.assertFalse(any("MDB_READERS_FULL" in f for f in report["failures"]))

    def test_health_check_fail_empty_log(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "debug.log"
            log.write_text("", encoding="utf-8")
            code, report = self.mod.run_health_check(log)
            self.assertEqual(code, 1)
            self.assertEqual(report["status"], "FAIL")
            self.assertTrue(any("no benchstats rollups" in f for f in report["failures"]))

    def test_health_check_fail_run_started_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "debug.log"
            log.write_text(RUN_STARTED + "\n", encoding="utf-8")
            code, report = self.mod.run_health_check(log)
            self.assertEqual(code, 1)
            self.assertEqual(report["status"], "FAIL")
            self.assertTrue(any("no benchstats rollups" in f for f in report["failures"]))

    def test_health_check_ignores_prior_run_rollups(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "debug.log"
            prior_started = (
                "2026-07-06T14:00:00Z === Swords run started 2026-07-06T14:00:00Z "
                "(datadir=/tmp/.bitcoin-swords benchstats=1) ==="
            )
            log.write_text(
                "\n".join(
                    [
                        prior_started,
                        benchstats_line("2026-07-06T14:01:40Z", readers_full=5),
                        RUN_STARTED,
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            code, report = self.mod.run_health_check(log)
            self.assertEqual(code, 1)
            self.assertEqual(report["status"], "FAIL")
            self.assertTrue(any("no benchstats rollups" in f for f in report["failures"]))
            self.assertFalse(any("readers_full=5" in f for f in report["failures"]))

    def test_resolve_env_config_reads_check_thresholds(self):
        env = {
            "TXINDEX_WARN_MS": "400",
            "PREFETCH_ROLLUPS": "5",
            "PREFETCH_MIN_PREVOUTS": "80",
        }
        with unittest.mock.patch.dict(os.environ, env, clear=False):
            cfg = self.mod.resolve_env_config("check")
        self.assertEqual(cfg.txindex_warn_ms, 400.0)
        self.assertEqual(cfg.prefetch_rollups, 5)
        self.assertEqual(cfg.prefetch_min_prevouts, 80)

    def test_health_check_warn_broken_prefetch(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "debug.log"
            lines = [RUN_STARTED]
            for i in range(1, 4):
                lines.append(
                    benchstats_line(
                        f"2026-07-06T16:0{i}:40Z",
                        prefetch_hit=0,
                        coin_prevouts=100,
                    )
                )
            log.write_text("\n".join(lines) + "\n", encoding="utf-8")
            code, report = self.mod.run_health_check(log, prefetch_rollups=3)
            self.assertEqual(code, 0)
            self.assertEqual(report["status"], "WARN")
            self.assertTrue(any("prefetch_hit=0" in w for w in report["warnings"]))

    def test_golden_watch_row_from_pidstat_fixtures(self):
        usr, sys_pct, cpu_pct = self.mod.parse_pidstat_cpu_output(
            PIDSTAT_CPU_FIXTURE, "12345"
        )
        vsz, rss = self.mod.parse_pidstat_mem_output(PIDSTAT_MEM_FIXTURE, "12345")
        row = self.mod.format_watch_row(
            "2026-07-06T15:30:55Z",
            "200123",
            cpu_pct,
            usr,
            sys_pct,
            vsz,
            rss,
        )
        self.assertEqual(
            row,
            "2026-07-06T15:30:55Z,200123,15.00,12.00,3.00,2048000,512000",
        )


class Phase0ShellTests(unittest.TestCase):
    def _minimal_path_with_python(self) -> str:
        tmp = tempfile.mkdtemp()
        self.addCleanup(lambda: shutil.rmtree(tmp, ignore_errors=True))
        py = shutil.which("python3")
        assert py is not None
        os.symlink(py, Path(tmp) / "python3")
        return tmp

    def _run_script(self, *args: str, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
        run_env = os.environ.copy()
        if env:
            run_env.update(env)
        bash = shutil.which("bash") or "/bin/bash"
        return subprocess.run(
            [bash, str(SCRIPT_PATH), *args],
            cwd=str(ROOT),
            env=run_env,
            capture_output=True,
            text=True,
        )

    def test_shell_help_exits_zero(self):
        result = self._run_script("help")
        self.assertEqual(result.returncode, 0)
        self.assertIn("just watch-cpu", result.stdout)

    def test_shell_check_ok(self):
        with tempfile.TemporaryDirectory() as tmp:
            datadir = Path(tmp) / "datadir"
            datadir.mkdir()
            (datadir / "debug.log").write_text(
                RUN_STARTED + "\n" + benchstats_line("2026-07-06T16:01:40Z") + "\n",
                encoding="utf-8",
            )
            result = self._run_script("check", env={"DATADIR": str(datadir)})
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            self.assertIn("phase0_check", result.stdout)
            self.assertIn("status\tOK", result.stdout)

    def test_shell_missing_pidstat_watch(self):
        result = self._run_script(
            "watch",
            env={
                "PATH": self._minimal_path_with_python(),
                "DATADIR": "/tmp/nonexistent-datadir",
            },
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("pidstat not found", result.stderr)

    def test_shell_missing_pidstat_snapshot(self):
        result = self._run_script(
            "snapshot",
            env={
                "PATH": self._minimal_path_with_python(),
                "DATADIR": "/tmp/nonexistent-datadir",
            },
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("pidstat not found", result.stderr)

    def test_shell_missing_perf_profile(self):
        result = self._run_script(
            "profile",
            env={
                "PATH": self._minimal_path_with_python(),
                "DATADIR": "/tmp/nonexistent-datadir",
            },
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("perf not found", result.stderr)

    def test_shell_node_not_running_snapshot(self):
        result = self._run_script(
            "snapshot",
            env={
                "DATADIR": "/tmp/phase0-no-bitcoind-here-xyz",
                "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
            },
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("bitcoind not running", result.stderr)

    def test_shell_syntax_check(self):
        result = subprocess.run(
            ["bash", "-n", str(SCRIPT_PATH)],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, msg=result.stderr)

    def test_shell_watch_stops_on_sigint(self):
        bash = shutil.which("bash") or "/bin/bash"
        py = shutil.which("python3")
        assert py is not None

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            datadir = root / "datadir"
            datadir.mkdir()
            logs = root / "logs"
            bindir = root / "bin"
            bindir.mkdir()

            os.symlink(py, bindir / "python3")

            fake_bitcoind = bindir / "bitcoind"
            fake_bitcoind.write_text("#!/bin/sh\nwhile true; do sleep 1; done\n")
            fake_bitcoind.chmod(0o755)

            fake_pidstat = bindir / "pidstat"
            fake_pidstat.write_text(
                "#!/bin/sh\n"
                'pid="$3"\n'
                'if [ "$1" = "-r" ]; then\n'
                '  echo "07:06:46 PM     0     ${pid}      0.00     0.00  2048000  512000  1.23  bitcoind"\n'
                "else\n"
                '  echo "07:06:46 PM     0     ${pid}   12.00    3.00    0.00    0.00   15.00     2  bitcoind"\n'
                "fi\n"
            )
            fake_pidstat.chmod(0o755)

            node = subprocess.Popen(
                [str(fake_bitcoind), f"-datadir={datadir}"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            self.addCleanup(lambda: self._stop_process_group(node))

            env = os.environ.copy()
            env.update({
                "PATH": f"{bindir}:/bin",
                "DATADIR": str(datadir),
                "PROFILE_LOGS": str(logs),
                "INTERVAL": "30",
                "BITCOIN_CLI": "",
            })

            watch = subprocess.Popen(
                [bash, str(SCRIPT_PATH), "watch"],
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                start_new_session=True,
            )
            try:
                time.sleep(2)
                os.killpg(os.getpgid(watch.pid), signal.SIGINT)
                stdout, stderr = watch.communicate(timeout=15)
            finally:
                self._stop_process_group(watch)

            self.assertEqual(watch.returncode, 0, msg=stderr)
            self.assertIn("Watch stopped (signal)", stderr)

            csv_files = list(logs.glob("phase0-watch-*.csv"))
            self.assertEqual(len(csv_files), 1, msg=stdout + stderr)
            content = csv_files[0].read_text()
            self.assertIn("# watch_stopped=signal", content)

    def _stop_process_group(self, proc: subprocess.Popen) -> None:
        if proc.poll() is not None:
            return
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
            proc.wait(timeout=5)
        except (ProcessLookupError, subprocess.TimeoutExpired):
            try:
                proc.kill()
            except ProcessLookupError:
                pass


if __name__ == "__main__":
    unittest.main()