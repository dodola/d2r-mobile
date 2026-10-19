import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import android_d2r_apk as pack


def entry(name, data, key="path"):
    return {key: name, "bytes": len(data), "sha256": pack.sha(data)}


def elf_fixture():
    data = bytearray(4096)
    data[:16] = b"\x7fELF\x02\x01\x01" + bytes(9)
    struct.pack_into("<HHIQQQIHHHHHH", data, 16, 3, 183, 1, 0, 64, 3072, 0, 64, 56, 1, 64, 4, 0)
    struct.pack_into("<IIQQQQQQ", data, 64, 1, 4, 0, 0x10000, 0, 4096, 4096, 16384)
    names = b"\0dm_android_hud_exp_table_entries\0dm_ta_manifest_sha\0"
    data[512:512 + len(names)] = names
    struct.pack_into("<IBBHQQ", data, 1024 + 24, 1, 0x11, 0, 3, 0x10800, 4)
    struct.pack_into("<IBBHQQ", data, 1024 + 48, names.index(b"dm_ta_manifest_sha"), 0x11, 0, 3, 0x10804, 65)
    struct.pack_into("<I", data, 2048, 100)
    data[2052:2117] = b"a" * 64 + b"\0"
    struct.pack_into("<IIQQQQIIQQ", data, 3072 + 64, 0, 3, 2, 0x10200, 512, len(names), 0, 0, 1, 0)
    struct.pack_into("<IIQQQQIIQQ", data, 3072 + 128, 0, 11, 2, 0x10400, 1024, 72, 1, 0, 8, 24)
    struct.pack_into("<IIQQQQIIQQ", data, 3072 + 192, 0, 1, 2, 0x10800, 2048, 69, 0, 0, 4, 0)
    return data


class ManifestTests(unittest.TestCase):
    def test_paths_reject_traversal_absolute_noncanonical_and_windows_streams(self):
        for name in ("../x", "a/../x", "/x", "C:/x", "a\\x", "a//x", "a/./x", "a/", "a:x", "a./b", "a /b", "a/\nx", ""):
            with self.subTest(name=name), self.assertRaises(ValueError):
                pack.relative_path(name)
        self.assertEqual(str(pack.relative_path("d2r_hud/a.png")), "d2r_hud/a.png")

    def test_size_hash_and_missing_payload_fail(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "x").write_bytes(b"original")
            manifest = {"files": [entry("x", b"original")]}
            self.assertEqual(pack.payloads(root, manifest, "path"), {"x": b"original"})
            (root / "x").write_bytes(b"tampered")  # same size, different SHA
            with self.assertRaisesRegex(ValueError, "mismatch"):
                pack.payloads(root, manifest, "path")
            (root / "x").unlink()
            with self.assertRaisesRegex(ValueError, "Missing"):
                pack.payloads(root, manifest, "path")

    def test_duplicates_in_one_and_across_manifests_fail(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "x").write_bytes(b"x")
            item = entry("x", b"x")
            with self.assertRaisesRegex(ValueError, "Duplicate"):
                pack.payloads(root, {"files": [item, item]}, "path")
            with self.assertRaisesRegex(ValueError, "Duplicate"):
                pack.payloads(root, {"files": [item]}, "path", {"X".casefold()})

    def test_empty_gates_are_allowed_only_for_modules(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "gates.tsv").write_bytes(b"")
            manifest = {"files": [entry("gates.tsv", b"")]}
            with self.assertRaisesRegex(ValueError, "Invalid size"):
                pack.payloads(root, manifest, "path")
            self.assertEqual(pack.payloads(root, manifest, "path", allow_empty=True), {"gates.tsv": b""})

    def test_png_dimensions_are_checked(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            data = b"\x89PNG\r\n\x1a\n" + struct.pack(">I4sII", 13, b"IHDR", 8, 9) + bytes(9)
            (root / "x.png").write_bytes(data)
            item = entry("x.png", data)
            item["dimensions"] = [8, 10]
            with self.assertRaisesRegex(ValueError, "dimensions"):
                pack.payloads(root, {"files": [item]}, "path")

    def test_duplicate_json_key_fails(self):
        with self.assertRaisesRegex(ValueError, "Duplicate JSON"):
            json.loads('{"files":[],"files":[]}', object_pairs_hook=pack.unique_object)

    def test_symlink_escape_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "real").write_bytes(b"x")
            try:
                (root / "link").symlink_to(root / "real")
            except OSError:
                self.skipTest("OS does not allow symlink creation")
            with self.assertRaisesRegex(ValueError, "Linked"):
                pack.checked_file(root, "link")


class UpdateUrlTests(unittest.TestCase):
    def test_only_absolute_https_urls_without_credentials_are_accepted(self):
        self.assertEqual(pack.checked_update_url("https://updates.example.com/d2r.json"),
                         "https://updates.example.com/d2r.json")
        self.assertEqual(pack.checked_update_url(""), "")
        for value in ("http://updates.example.com/d2r.json", "https://user:pass@example.com/x",
                      "https://example.com:0/x", "https://example.com/a b", "itms-services://example.com/x"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                pack.checked_update_url(value)


class ElfTests(unittest.TestCase):
    def test_exported_xp_and_fingerprint_are_read_without_execution(self):
        elf = pack.ElfSymbols(bytes(elf_fixture()))
        self.assertEqual(struct.unpack("<I", elf.datum("dm_android_hud_exp_table_entries", 4)), (100,))
        self.assertEqual(elf.fingerprint("dm_ta_manifest_sha"), "a" * 64)
        with self.assertRaisesRegex(ValueError, "requires ELF symbol"):
            elf.datum("absent")

    def test_bad_abi_alignment_and_section_bounds_fail(self):
        for offset, replacement in ((18, struct.pack("<H", 62)), (64 + 48, struct.pack("<Q", 4096)), (40, struct.pack("<Q", 4095))):
            data = elf_fixture()
            data[offset:offset + len(replacement)] = replacement
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                pack.ElfSymbols(bytes(data))


class ModuleProfileTests(unittest.TestCase):
    def reject_modules(self, extra=False, empty_binary=False):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "native.so").write_bytes(b"fixture")
            rows = []
            for module in ("main", "sdk", "subsdk0"):
                for suffix in ("patched.bin", "fixups.tsv", "gates.tsv"):
                    name = f"{module}.{suffix}"
                    data = b"" if suffix == "gates.tsv" or (empty_binary and name == "main.patched.bin") else b"payload"
                    (root / name).write_bytes(data)
                    rows.append(entry(name, data, "name"))
            if extra:
                (root / "extra.bin").write_bytes(b"extra")
                rows.append(entry("extra.bin", b"extra", "name"))
            (root / pack.MODULE_MANIFEST).write_text(json.dumps({"title_id": pack.TITLE_ID, "build_id": pack.BUILD_ID, "files": rows}), encoding="utf-8")
            class FakeElf:
                def __init__(self, unused):
                    self.info = {}
                def datum(self, name, length=None):
                    return struct.pack("<I", 100) if name == "dm_android_hud_exp_table_entries" else b"fixture"
            with patch.object(pack, "ElfSymbols", FakeElf):
                with self.assertRaisesRegex(ValueError, "exactly the 9" if extra else "must be nonempty"):
                    pack.validate_inputs(root / "native.so", root, root)

    def test_extra_module_is_rejected_before_packaging(self):
        self.reject_modules(extra=True)

    def test_empty_patched_binary_is_rejected_even_with_matching_hash(self):
        self.reject_modules(empty_binary=True)


class OfflineReleaseTests(unittest.TestCase):
    ROMFS = {"bytes": 123, "sha256": "a" * 64}

    def offline(self, root, **changes):
        files = [{"path": "game.romfs", "bytes": 123, "sha256": "a" * 64},
                 {"path": "d2r-mobile/shaders/shader_index.bin", "bytes": 7, "sha256": "b" * 64}]
        manifest = {"format": 1, "game_build": 94012, "total_bytes": 130, "files": files}
        manifest.update(changes)
        path = root / "offline-manifest.json"
        path.write_text(json.dumps(manifest), encoding="utf-8")
        return path

    def test_offline_manifest_must_match_the_module_romfs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw, report = pack.validate_offline_manifest(self.offline(root), {"romfs": self.ROMFS})
            self.assertEqual(report["game_build"], 94012)
            self.assertEqual(raw, (root / "offline-manifest.json").read_bytes())
            with self.assertRaisesRegex(ValueError, "differs from the RomFS"):
                pack.validate_offline_manifest(self.offline(root), {"romfs": {"bytes": 123, "sha256": "c" * 64}})

    def test_offline_manifest_rejects_other_builds_paths_and_totals(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for changes, message in (({"game_build": 91738}, "game build"), ({"total_bytes": 1}, "total_bytes"),
                                     ({"files": [{"path": "../x", "bytes": 1, "sha256": "a" * 64}] * 2}, "Invalid offline")):
                with self.subTest(changes=changes), self.assertRaisesRegex(ValueError, message):
                    pack.validate_offline_manifest(self.offline(root, **changes), {"romfs": self.ROMFS})

    def test_overlay_must_contain_exactly_the_required_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for prefix, names in (("base", pack.OVERLAY_BASE_FILES), ("variant", pack.OVERLAY_VARIANT_FILES)):
                for name in names:
                    target = root / prefix / name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(b"x" + name.encode())
            result = pack.validate_overlay(root / "base", root / "variant")
            self.assertIn("romfs-overlay/data/.build.config", result)
            self.assertIn("romfs-overlay-variants/item_notes/features.txt", result)
            (root / "base" / "stray.bin").write_bytes(b"x")
            with self.assertRaisesRegex(ValueError, "exactly"):
                pack.validate_overlay(root / "base", root / "variant")

    def test_signing_properties_require_every_field_and_an_existing_keystore(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            store = root / "release.keystore"
            properties = root / "release.properties"
            properties.write_text(f"storeFile={store}" + chr(10) + chr(10).join(['alias=a', 'storePassword=p', 'keyPassword=k']) + chr(10), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "missing"):
                pack.read_signing_properties(properties)
            store.write_bytes(b"k")
            self.assertEqual(pack.read_signing_properties(properties), (store, "a", "p", "k"))
            properties.write_text(f"storeFile={store}" + chr(10) + chr(10).join(['alias=a', 'storePassword=p']) + chr(10), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "keyPassword"):
                pack.read_signing_properties(properties)

    def test_release_manifest_is_not_debuggable_and_uses_the_new_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "AndroidManifest.xml"
            repo = Path(__file__).resolve().parents[3]
            pack.release_manifest(repo, destination, "org.d2r.mobile", 3, "1.2.3", "https://example.org/u.json")
            text = destination.read_text(encoding="utf-8")
            self.assertIn('package="org.d2r.mobile"', text)
            self.assertIn('android:debuggable="false"', text)
            self.assertNotIn('debuggable="true"', text)
            self.assertIn('android:versionCode="3"', text)
            self.assertIn("InstallerActivity", text)
            self.assertIn('android:value="https://example.org/u.json"', text)


if __name__ == "__main__":
    unittest.main()
