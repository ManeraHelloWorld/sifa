"""Distribution-policy tests. Fixtures are inert bytes, not a scanned application."""
from datetime import datetime, timedelta, timezone
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("package_release", ROOT / "tools/package_release.py")
PACKAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGE)


class DistributionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="sifa-package-policy-")
        self.addCleanup(self.temp.cleanup)
        self.exe = Path(self.temp.name) / "fixture.exe"
        self.exe.write_bytes(b"MZ inert packaging test fixture; not executable")
        self.receipt = Path(self.temp.name) / "fixture-receipt.json"
        self.report = {
            "schema_version": 1, "scanner": "Microsoft Defender",
            "result": "no_threats_detected", "engine_version": "test-fixture",
            "signature_version": "test-fixture", "finished_at": datetime.now(timezone.utc).isoformat(),
            "files": [{"path": str(self.exe), "size": self.exe.stat().st_size,
                       "sha256": hashlib.sha256(self.exe.read_bytes()).hexdigest()}],
        }
        self.save_report()

    def save_report(self):
        self.receipt.write_text(json.dumps(self.report), encoding="utf-8")

    def test_matching_receipt_accepts_exact_bytes(self):
        self.assertEqual(PACKAGE.check_scan(self.exe, self.receipt), self.exe.read_bytes())

    def test_changed_executable_rejected(self):
        self.exe.write_bytes(self.exe.read_bytes() + b" changed after scan")
        with self.assertRaisesRegex(ValueError, "does not match"):
            PACKAGE.check_scan(self.exe, self.receipt)

    def test_detection_or_missing_scanner_rejected(self):
        for key, value in [("result", "threat_detected"), ("scanner", "unknown"), ("engine_version", "")]:
            original = self.report[key]
            self.report[key] = value
            self.save_report()
            with self.subTest(key=key), self.assertRaises(ValueError):
                PACKAGE.check_scan(self.exe, self.receipt)
            self.report[key] = original

    def test_old_future_and_unzoned_receipts_rejected(self):
        now = datetime.now(timezone.utc)
        for stamp in [(now - timedelta(days=2)).isoformat(), (now + timedelta(days=2)).isoformat(),
                      datetime.now().isoformat()]:
            self.report["finished_at"] = stamp
            self.save_report()
            with self.subTest(stamp=stamp), self.assertRaises(ValueError):
                PACKAGE.check_scan(self.exe, self.receipt)

    def test_withdrawal_overrides_matching_receipt(self):
        digest = hashlib.sha256(self.exe.read_bytes()).hexdigest()
        with patch.object(PACKAGE, "WITHDRAWN_SHA256", {digest}):
            with self.assertRaisesRegex(ValueError, "withdrawn"):
                PACKAGE.check_scan(self.exe, self.receipt)

    def test_cli_missing_receipt_does_not_create_archive(self):
        output = Path(self.temp.name) / "should-not-exist.zip"
        result = subprocess.run([sys.executable, str(ROOT / "tools/package_release.py"),
                                 "--exe", str(self.exe), "--output", str(output)],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--scan-report", result.stderr)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
