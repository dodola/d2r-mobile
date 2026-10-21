import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "tools/coverage_scan_report.py"
spec = importlib.util.spec_from_file_location("coverage_scan_report", SCRIPT)
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


class CoverageReportTests(unittest.TestCase):
    def test_apply_preserves_header_and_latches_red(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            seed = root / "seed.tsv"
            header = "# d2r-coverage-v2 identity=abcd\npk\tw\th\tformat\tis_depth\tverdict\tsamples\tuncovered\ttotal\n"
            seed.write_text(header + "0000000000000001\t4\t4\t98\t0\tgreen\t8\t0\t128\n", encoding="utf-8")
            scan = root / "scan"
            scan.mkdir()
            (scan / "lv1.tsv").write_text(header + "0000000000000001\t4\t4\t98\t0\tred\t9\t1\t144\n", encoding="utf-8")
            result = subprocess.run([sys.executable, str(SCRIPT), "--seed", str(seed), "--scan", str(scan), "--apply"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(seed.read_text(encoding="utf-8").startswith(header))
            row = next(iter(report.read_rows(seed).values()))
            self.assertEqual(row, dict(verdict="red", samples=9, uncovered=1, total=144))
            self.assertEqual(list(root.glob("*.tmp")), [])
            subprocess.run([sys.executable, str(SCRIPT), "--seed", str(seed), "--scan", str(scan), "--apply"], check=True, capture_output=True)
            self.assertEqual(next(iter(report.read_rows(seed).values()))["verdict"], "red")

    def test_invalid_and_uncovered_green(self):
        for line in ("pk\tw\th\tformat\tis_depth\tverdict\tsamples\tuncovered\ttotal", "# comment", "broken", "0000000000000001\t0\t4\t37\t0\tgreen\t8\t0\t16"):
            self.assertIsNone(report.parse_row(line))
        row = report.parse_row("0000000000000001\t4\t4\t37\t0\tgreen\t8\t1\t16")
        self.assertEqual(row[1]["verdict"], "red")


if __name__ == "__main__":
    unittest.main()
