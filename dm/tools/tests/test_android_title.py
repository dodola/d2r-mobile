"""Content identity and input integrity checks use synthetic metadata only."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from android_title import REPO, external_output, npdm_info, verified_inputs
from nso_inventory import Nso


def npdm(program_id=0x01008CF01BAAC000):
    data = bytearray(0xc0)
    data[:4] = b"META"
    data[0xc] = 1
    data[0xe] = 44
    struct.pack_into("<I", data, 0x1c, 1 << 20)
    struct.pack_into("<II", data, 0x70, 0x80, 0x40)
    data[0x80:0x84] = b"ACI0"
    struct.pack_into("<Q", data, 0x90, program_id)
    return data


def nso(compressed=False):
    data = bytearray(0x100)
    data[:4] = b"NSO0"
    struct.pack_into("<I", data, 0xc, 0x38 | (7 if compressed else 0))
    for i in range(3):
        raw = bytes([i + 1]) * 32
        if compressed:
            import lz4.block
            stored = lz4.block.compress(raw, store_size=False)
        else:
            stored = raw
        struct.pack_into("<IIII", data, 0x10 + i * 16, len(data), i * 0x1000, len(raw), 0)
        struct.pack_into("<I", data, 0x60 + i * 4, len(stored))
        data[0xa0 + i * 32:0xc0 + i * 32] = hashlib.sha256(raw).digest()
        data.extend(stored)
    return data


class TitleIdentityTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_npdm_identity_and_architecture(self):
        p = self.root / "main.npdm"
        p.write_bytes(npdm())
        self.assertEqual(npdm_info(p)["program_id"], "01008cf01baac000")
        bad = npdm(); bad[0xc] = 0; p.write_bytes(bad)
        with self.assertRaisesRegex(ValueError, "AArch64"):
            npdm_info(p)

    def test_npdm_bounds(self):
        p = self.root / "main.npdm"
        data = npdm(); struct.pack_into("<I", data, 0x70, 0xffff)
        p.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "ACI0"):
            npdm_info(p)

    def test_generated_content_cannot_enter_repo_or_input(self):
        with self.assertRaises(ValueError):
            external_output(REPO / "generated-game", self.root)
        with self.assertRaises(ValueError):
            external_output(self.root / "exefs/child", self.root / "exefs")
        self.assertEqual(external_output(self.root / "exefs", self.root / "source.nsp"), self.root / "exefs")

    def test_segment_integrity_and_truncation(self):
        p = self.root / "main"
        data = nso(); p.write_bytes(data)
        self.assertTrue(all(s["sha256_matches"] for s in Nso(p).segments))
        data[-1] ^= 1; p.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            Nso(p)
        p.write_bytes(nso()[:-1])
        with self.assertRaisesRegex(ValueError, "Truncated"):
            Nso(p)

    def test_compressed_segments_preserve_memory_and_source_identity(self):
        p = self.root / "sdk"
        data = nso(compressed=True); p.write_bytes(data)
        module = Nso(p)
        self.assertEqual(module.source_sha256, hashlib.sha256(data).hexdigest())
        for i in range(3):
            self.assertEqual(module.read_memory(i * 0x1000, 32), bytes([i + 1]) * 32)

    def test_mixed_versions_stop_before_decode_or_rewrite(self):
        profile = json.loads((REPO / "dm/android/titles/echoes-of-wisdom.json").read_text())
        (self.root / "main.npdm").write_bytes(npdm())
        with self.assertRaisesRegex(ValueError, "NPDM hash"):
            verified_inputs(profile, 196608, self.root)
        profile["versions"]["196608"]["npdm_sha256"] = hashlib.sha256(npdm()).hexdigest()
        # An incomplete module set must not be silently treated as D2R's three modules.
        (self.root / "main").write_bytes(nso())
        with self.assertRaisesRegex(ValueError, "module set"):
            verified_inputs(profile, 196608, self.root)


if __name__ == "__main__":
    unittest.main()
