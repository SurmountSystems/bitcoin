#!/usr/bin/env python3
"""Unit tests for contrib/swords/phase5_usdt.py and phase5-usdt.sh."""
from __future__ import annotations

import importlib.util
import os
import shutil
import subprocess
import tempfile
import unittest
import unittest.mock
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "contrib" / "swords" / "phase5_usdt.py"
SCRIPT_PATH = ROOT / "contrib" / "swords" / "phase5-usdt.sh"
TEMPLATE_PATH = ROOT / "contrib" / "tracing" / "connectblock_benchmark.bt"

READELF_USDT_FIXTURE = """\
Displaying notes found in: .note.stapsdt
  Owner                 Data size	Description
  stapsdt              0x0000005d	NT_STAPSDT (SystemTap probe descriptors)
    Provider: validation
    Name: block_connected
    Location: 0x1, Base: 0x2, Semaphore: 0x3
    Arguments: -8@%r12
  stapsdt              0x0000005d	NT_STAPSDT (SystemTap probe descriptors)
    Provider: utxocache
    Name: flush
    Location: 0x4, Base: 0x5, Semaphore: 0x6
    Arguments: -8@%r12
"""

READELF_STAPSDT_UNPARSEABLE = """\
Displaying notes found in: .note.stapsdt
  Owner                 Data size	Description
  stapsdt              0x0000005d	NT_STAPSDT (SystemTap probe descriptors)
    Location: 0x1, Base: 0x2, Semaphore: 0x3
"""

READELF_NO_USDT_FIXTURE = """\
Displaying notes found in: .note.gnu.build-id
  Owner                Data size Description
  GNU                  0x00000020NT_GNU_BUILD_ID (unique build ID bitstring)
"""

TPLIST_FIXTURE = """\
b'validation':b'block_connected' [sema 0xd29bd0]
  1 location(s)
b'utxocache':b'flush' [sema 0xd29be0]
  1 location(s)
"""

CONNECTBLOCK_OUTPUT_FIXTURE = """\
ConnectBlock logging starting at height 199000
Logging blocks taking longer than 25 ms to connect.
Block 199123 (abcd)  100 tx  200 ins  300 sigops  took   42 ms
BENCH    3 blk/s    400 tx/s    800 inputs/s     1200 sigops/s (height 199125)
"""

FLUSH_OUTPUT_FIXTURE = """\
Hooking into bitcoind with pid 12345
Logging utxocache flushes. Ctrl-C to end...
Duration (µs)   Mode       Coins Count     Memory Usage    Flush for Prune
1500000         PERIODIC   12000           45000.00 kB     False
2500000         IF_NEEDED  8000            30000.00 kB     True
"""


def load_module():
    spec = importlib.util.spec_from_file_location("phase5_usdt", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    import sys

    sys.path.insert(0, str(ROOT / "contrib" / "swords"))
    sys.modules["phase5_usdt"] = module
    spec.loader.exec_module(module)
    return module


class Phase5UsdtTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mod = load_module()

    def test_pgrep_reuses_phase0_helper(self):
        pat = self.mod.pgrep_datadir_pattern("/home/user/.bitcoin-swords")
        self.assertIn(r"\.bitcoin\-swords", pat)

    def test_parse_readelf_tracepoints(self):
        tracepoints = self.mod._parse_readelf_tracepoints(READELF_USDT_FIXTURE)
        self.assertEqual(
            sorted(tracepoints),
            ["utxocache:flush", "validation:block_connected"],
        )

    def test_detect_usdt_readelf_success(self):
        with tempfile.NamedTemporaryFile() as tmp:
            path = Path(tmp.name)
            with unittest.mock.patch.object(
                self.mod, "_run_capture", return_value=(0, READELF_USDT_FIXTURE, "")
            ):
                result = self.mod.detect_usdt(path)
        self.assertTrue(result.available)
        self.assertEqual(result.method, "readelf")
        self.assertIn("validation:block_connected", result.tracepoints)
        self.assertIn("utxocache:flush", result.tracepoints)
        self.assertEqual(result.missing_required(), ())

    def test_detect_usdt_readelf_parse_failure(self):
        with tempfile.NamedTemporaryFile() as tmp:
            path = Path(tmp.name)

            def fake_run(cmd):
                if cmd[0] == "readelf":
                    return 0, READELF_STAPSDT_UNPARSEABLE, ""
                return 127, "", "tplist not found"

            with unittest.mock.patch.object(self.mod, "_run_capture", side_effect=fake_run):
                result = self.mod.detect_usdt(path)
        self.assertFalse(result.available)
        self.assertIn("could not parse tracepoints", result.error)

    def test_detect_usdt_missing_binary(self):
        result = self.mod.detect_usdt(Path("/nonexistent/bitcoind"))
        self.assertFalse(result.available)
        self.assertIn("not found", result.error)

    def test_detect_usdt_no_usdt_compiled(self):
        with tempfile.NamedTemporaryFile() as tmp:
            path = Path(tmp.name)

            def fake_run(cmd):
                if cmd[0] == "readelf":
                    return 0, READELF_NO_USDT_FIXTURE, ""
                return 127, "", "tplist not found"

            with unittest.mock.patch.object(self.mod, "_run_capture", side_effect=fake_run):
                result = self.mod.detect_usdt(path)
        self.assertFalse(result.available)
        self.assertIn("ENABLE_USDT", result.error)

    def test_detect_usdt_tplist_fallback(self):
        with tempfile.NamedTemporaryFile() as tmp:
            path = Path(tmp.name)

            def fake_run(cmd):
                if cmd[0] == "readelf":
                    return 0, READELF_NO_USDT_FIXTURE, ""
                if cmd[0] == "tplist":
                    return 0, TPLIST_FIXTURE, ""
                return 127, "", "not found"

            with unittest.mock.patch.object(self.mod, "_run_capture", side_effect=fake_run):
                result = self.mod.detect_usdt(path)
        self.assertTrue(result.available)
        self.assertEqual(result.method, "tplist")

    def test_render_connectblock_bt_substitutes_absolute_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "connectblock.bt"
            bitcoind = Path(tmp) / "bin" / "bitcoind"
            bitcoind.parent.mkdir(parents=True)
            bitcoind.write_text("", encoding="utf-8")
            rendered = self.mod.render_connectblock_bt(
                bitcoind,
                out,
                template_path=TEMPLATE_PATH,
            )
            text = rendered.read_text(encoding="utf-8")
            self.assertNotIn("./build/bin/bitcoind", text)
            resolved = str(bitcoind.resolve())
            self.assertIn(resolved, text)
            usdt_lines = [
                line
                for line in text.splitlines()
                if line.startswith("usdt:") and "validation:block_connected" in line
            ]
            self.assertEqual(len(usdt_lines), 2)
            self.assertTrue(all(resolved in line for line in usdt_lines))

    def test_connectblock_cmd_uses_rendered_script(self):
        with tempfile.TemporaryDirectory() as tmp:
            bt = Path(tmp) / "connectblock.bt"
            bt.write_text("BEGIN { }", encoding="utf-8")
            bitcoind = Path(tmp) / "bitcoind"
            bitcoind.write_text("", encoding="utf-8")
            cmd = self.mod.connectblock_cmd(bitcoind, 199000, 0, 25, bt)
        self.assertEqual(cmd[0], "bpftrace")
        self.assertEqual(cmd[1], str(bt))
        self.assertEqual(cmd[2:], ["199000", "0", "25"])

    def test_connectblock_cli_dry_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            bitcoind = Path(tmp) / "bitcoind"
            bitcoind.write_text("", encoding="utf-8")
            logs = Path(tmp) / "logs"
            rc = self.mod.main(
                [
                    "connectblock",
                    "--bitcoind",
                    str(bitcoind),
                    "--profile-logs",
                    str(logs),
                    "--start",
                    "199000",
                    "--end",
                    "0",
                    "--threshold-ms",
                    "25",
                ]
            )
            self.assertEqual(rc, 0)

    def test_utxo_flush_cmd(self):
        script = ROOT / "contrib" / "tracing" / "log_utxocache_flush.py"
        cmd = self.mod.utxo_flush_cmd("12345", script)
        self.assertEqual(cmd, ["python3", str(script), "12345"])

    def test_utxo_flush_cli_dry_run(self):
        rc = self.mod.main(["utxo-flush", "--pid", "12345"])
        self.assertEqual(rc, 0)

    def test_default_connectblock_start_height(self):
        self.assertEqual(self.mod.default_connectblock_start_height(0), 0)
        self.assertEqual(self.mod.default_connectblock_start_height(500), 0)
        self.assertEqual(self.mod.default_connectblock_start_height(250000), 249000)

    def test_phase5_run_dir(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            run_dir = self.mod.phase5_run_dir(root, "20260706T120000Z")
            self.assertEqual(run_dir, root / "20260706T120000Z")

    def test_write_profile_meta_includes_phase5_fields(self):
        with tempfile.TemporaryDirectory() as tmp:
            meta_path = Path(tmp) / "meta.txt"
            fields = {
                "captured_at_utc": "20260706T120000Z",
                "run_finished_at_utc": "20260706T120100Z",
                "subcommand": "utxo-flush",
                "block_height": "250000",
                "datadir": "/home/user/.bitcoin-swords",
                "pid": "12345",
                "profile_logs": "/home/user/.bitcoin-swords-profile-logs",
                "bitcoind_path": "/home/user/Projects/surmount/bitcoin/build/bin/bitcoind",
                "usdt_tracepoints": "validation:block_connected,utxocache:flush",
                "usdt_detection_method": "readelf",
                "utxo_flush_output": "/tmp/utxo-flush.txt",
                "utxo_flush_script": "/tmp/log_utxocache_flush.py",
                "bcc_available": "true",
                "interrupted": "false",
            }
            self.mod.write_profile_meta(meta_path, fields)
            parsed = self.mod.parse_meta_txt(meta_path.read_text(encoding="utf-8"))
            for key in (
                "bitcoind_path",
                "usdt_tracepoints",
                "utxo_flush_script",
                "utxo_flush_output",
            ):
                self.assertEqual(parsed[key], fields[key])
            self.assertIn("utxo_flush_script", self.mod.META_TXT_FIELDS)

    def test_parse_connectblock_output(self):
        summary = self.mod.parse_connectblock_output(CONNECTBLOCK_OUTPUT_FIXTURE)
        self.assertEqual(summary["slow_block_count"], 1)
        self.assertEqual(summary["bench_sample_count"], 1)
        slow = summary["slow_blocks"][0]
        self.assertEqual(slow["height"], 199123)
        self.assertEqual(slow["duration_ms"], 42)

    def test_parse_flush_output(self):
        summary = self.mod.parse_flush_output(FLUSH_OUTPUT_FIXTURE)
        self.assertEqual(summary["flush_count"], 2)
        self.assertEqual(summary["duration_us_total"], 4000000)
        self.assertFalse(summary["flushes"][0]["flush_for_prune"])
        self.assertTrue(summary["flushes"][1]["flush_for_prune"])

    def test_resolve_env_config_reads_swords_env(self):
        with tempfile.TemporaryDirectory() as tmp:
            dd = Path(tmp) / "datadir"
            logs = Path(tmp) / "logs"
            env = {
                "SWORDS_DATADIR": str(dd),
                "SWORDS_LOG_ARCHIVE": str(logs),
                "THRESHOLD_MS": "50",
                "START": "200000",
                "END": "0",
                "DURATION": "90",
            }
            with unittest.mock.patch.dict(os.environ, env, clear=False):
                cfg = self.mod.resolve_env_config()
            self.assertEqual(cfg.datadir, dd)
            self.assertEqual(cfg.profile_logs, logs)
            self.assertEqual(cfg.threshold_ms, 50)
            self.assertEqual(cfg.start_height, 200000)
            self.assertEqual(cfg.end_height, 0)
            self.assertEqual(cfg.duration, 90)

    def test_shell_export_config_quoting(self):
        cfg = self.mod.Phase5Config(
            subcommand="",
            datadir=Path("/tmp/has spaces/.bitcoin-swords"),
            profile_logs=Path("/tmp/logs"),
            bitcoind=Path("/tmp/build/bin/bitcoind"),
            bitcoin_cli=None,
            duration=60,
            start_height=199000,
            end_height=0,
            threshold_ms=25,
        )
        exported = self.mod.shell_export_config(cfg)
        self.assertIn("BITCOIND=", exported)
        self.assertIn("/tmp/has spaces/.bitcoin-swords", exported)

    def test_usage_text_documents_just_recipes(self):
        text = self.mod.usage_text("/home/user/.bitcoin-swords")
        self.assertIn("just check-usdt", text)
        self.assertIn("sudo -E just profile-connectblock", text)
        self.assertIn("sudo -E just profile-utxo-flush", text)
        self.assertIn("BITCOIND", text)


class Phase5ShellTests(unittest.TestCase):
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
        self.assertIn("sudo -E just profile-connectblock", result.stdout)

    def test_shell_syntax_check(self):
        result = subprocess.run(
            ["bash", "-n", str(SCRIPT_PATH)],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, msg=result.stderr)

    def test_shell_node_not_running_check_usdt(self):
        result = self._run_script(
            "check-usdt",
            env={
                "DATADIR": "/tmp/phase5-no-bitcoind-here-xyz",
                "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
                "BITCOIND": "/tmp/phase5-no-bitcoind-binary-xyz",
            },
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("bitcoind binary not found", result.stderr)

    def test_shell_check_usdt_with_bitcoind_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            bitcoind = Path(tmp) / "bitcoind"
            bitcoind.write_text("", encoding="utf-8")
            result = self._run_script(
                "check-usdt",
                env={
                    "DATADIR": "/tmp/phase5-no-bitcoind-here-xyz",
                    "BITCOIND": str(bitcoind),
                    "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
                },
            )
            self.assertEqual(result.returncode, 1)
            self.assertNotIn("bitcoind not running", result.stderr)

    def test_shell_missing_bpftrace_connectblock(self):
        bindir = self._minimal_path_with_python()
        fake_id = Path(bindir) / "id"
        fake_id.write_text("#!/bin/sh\necho 0\n")
        fake_id.chmod(0o755)
        result = self._run_script(
            "connectblock",
            env={
                "PATH": f"{bindir}:/usr/bin:/bin",
                "DATADIR": "/tmp/phase5-no-bitcoind-here-xyz",
            },
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("bpftrace not found", result.stderr)

    def test_shell_require_root_non_root(self):
        bindir = self._minimal_path_with_python()
        fake_id = Path(bindir) / "id"
        fake_id.write_text("#!/bin/sh\necho 1000\n")
        fake_id.chmod(0o755)
        fake_bitcoind = Path(bindir) / "bitcoind"
        fake_bitcoind.write_text("#!/bin/sh\nsleep 60\n")
        fake_bitcoind.chmod(0o755)
        datadir = tempfile.mkdtemp(prefix="phase5-root-test-")
        self.addCleanup(lambda: shutil.rmtree(datadir, ignore_errors=True))
        node = subprocess.Popen(
            [str(fake_bitcoind), f"-datadir={datadir}"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        def _stop_node() -> None:
            if node.poll() is None:
                node.terminate()
                try:
                    node.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    node.kill()

        self.addCleanup(_stop_node)

        result = self._run_script(
            "connectblock",
            env={
                "PATH": f"{bindir}:/usr/bin:/bin",
                "DATADIR": datadir,
            },
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("requires root", result.stderr)
        self.assertIn("sudo -E bash", result.stderr)

    @unittest.skipUnless(shutil.which("just"), "just not installed")
    def test_just_phase5_help(self):
        result = subprocess.run(
            ["just", "phase5"],
            cwd=str(ROOT),
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0)
        self.assertIn("sudo -E just profile-connectblock", result.stdout)

    @unittest.skipUnless(shutil.which("just"), "just not installed")
    def test_just_check_usdt_no_running_node(self):
        result = subprocess.run(
            ["just", "check-usdt"],
            cwd=str(ROOT),
            env={
                **os.environ,
                "SWORDS_DATADIR": "/tmp/phase5-just-no-bitcoind-xyz",
            },
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 1)
        combined = result.stderr + result.stdout
        self.assertTrue(
            any(
                needle in combined
                for needle in (
                    "USDT",
                    "ENABLE_USDT",
                    "bitcoind binary not found",
                )
            ),
            msg=combined,
        )


if __name__ == "__main__":
    unittest.main()