import gzip
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock
from bnsh_vulkan_cache import (descriptor, merge_caches, overlay_cache, read_validated_cache,
                               declare_distance_capabilities, trim_unused_capabilities)


class BindingTests(unittest.TestCase):
    def reflection(self,query=False):
        image={"binding":4,"handle":72,"array":1,"type":3,"flags":7,"separate":False,"cbuf_slot":2}
        texture={**image,"binding":1,"flags":2,"cbuf_slot":-1}
        return {"cbufs":[],"sbufs":[],"images":[image],"textures":[texture] if query else []}

    def types(self):return {"100":{"opcode":32,"operands":[0,101]},"101":{"opcode":25,"operands":[99,1,0,0,0,1,0]}}

    def test_storage_image_uses_image_slot_zero(self):
        resource={"set":0,"binding":4,"type_id":100}
        self.assertEqual(descriptor(resource,self.reflection(),5,self.types())[2:5],(3,6,0))

    def test_size_query_uses_image_bank_without_sampler(self):
        resource={"set":0,"binding":1,"type_id":100,"query_only":True}
        self.assertEqual(descriptor(resource,self.reflection(True),5,self.types())[2:6],(2,11,0,-1))

    def test_image_fetch_cannot_masquerade_as_size_query(self):
        reflection=self.reflection(True);reflection["textures"][0]["flags"]=3
        with self.assertRaises(ValueError):descriptor({"set":0,"binding":1,"type_id":100},reflection,5,self.types())


class DistanceCapabilityTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "distance.spv"

    def module(self, instructions):
        words = [0x07230203, 0x00010300, 0, 32, 0]
        for opcode, operands in instructions:
            words += [((len(operands) + 1) << 16) | opcode] + operands
        self.path.write_bytes(struct.pack(f"<{len(words)}I", *words))

    def caps(self):
        raw = self.path.read_bytes()
        words = struct.unpack(f"<{len(raw) // 4}I", raw)
        caps, i = [], 5
        while i < len(words):
            count, opcode = words[i] >> 16, words[i] & 0xffff
            if opcode == 17:
                caps.append(words[i + 1])
            i += count
        return caps

    def test_member_clip_and_cull_declarations_add_both_missing_caps(self):
        self.module([(17, [1]), (72, [20, 2, 11, 3]), (72, [20, 3, 11, 4])])
        self.assertEqual(declare_distance_capabilities(self.path), [32, 33])
        self.assertEqual(set(self.caps()), {1, 32, 33})
        self.assertEqual(declare_distance_capabilities(self.path), [])

    def test_direct_builtin_declaration_is_supported(self):
        self.module([(17, [1]), (71, [20, 11, 3])])
        self.assertEqual(declare_distance_capabilities(self.path), [32])

    def test_unrelated_decoration_literals_do_not_trigger_caps(self):
        self.module([(17, [1]), (71, [20, 30, 3]), (72, [20, 0, 35, 4])])
        self.assertEqual(declare_distance_capabilities(self.path), [])
        self.assertEqual(self.caps(), [1])

    def test_trim_preserves_distance_even_when_validator_accepts_removal(self):
        self.module([(17, [1]), (17, [32]), (17, [33]), (17, [64]),
                     (72, [20, 2, 11, 3]), (72, [20, 3, 11, 4])])
        # Reproduce spirv-val's permissive behavior without depending on its version.
        with mock.patch("bnsh_vulkan_cache.subprocess.run", return_value=mock.Mock(returncode=0)):
            removed = trim_unused_capabilities(self.path, Path("validator"))
        self.assertEqual(set(self.caps()), {1, 32, 33})
        self.assertEqual(removed, [{"opcode": 17, "operands": [64]}])

    def test_truncated_instruction_is_rejected(self):
        self.module([(17, [1])])
        self.path.write_bytes(self.path.read_bytes() + struct.pack("<I", (4 << 16) | 72))
        with self.assertRaisesRegex(ValueError, "instruction bounds"):
            declare_distance_capabilities(self.path)


class CacheMergeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def cache(self, name, code=b"original program", spirv=b"validated spirv", metadata=b"metadata"):
        root = self.root / name
        root.mkdir()
        control = bytearray(0x880)
        struct.pack_into("<I", control, 0x704, len(code))
        struct.pack_into("<I", control, 0x714, 0)
        key = hashlib.sha256(control + code).hexdigest()[:16]
        index = struct.pack("<III", 0x494b5644, 2, 1) + key.encode() + struct.pack("<II", 0, len(code)) + control + code
        compressed = gzip.compress(spirv, mtime=0)
        (root / (key + ".spv.gz")).write_bytes(compressed)
        (root / (key + ".vkmeta")).write_bytes(metadata)
        (root / "shader_index.bin").write_bytes(index)
        row = {"key": key, "stage": "vs", "validated": True,
               "spirv_sha256": hashlib.sha256(spirv).hexdigest(),
               "metadata_sha256": hashlib.sha256(metadata).hexdigest(),
               "gzip_sha256": hashlib.sha256(compressed).hexdigest()}
        report = {"programs": 1, "exported": 1, "structural_validation_complete": True,
                  "index_sha256": hashlib.sha256(index).hexdigest(), "records": [row]}
        (root / "validation.json").write_text(json.dumps(report))
        return root, row

    def overlay_plan(self, before, after):
        evidence = self.root / "vertex-type-evidence.txt"
        evidence.write_text("Original uint vertex input confirmed by runtime format diagnostic")
        plan = self.root / "overlay-plan.json"
        plan.write_text(json.dumps({"reason": "Correct a proved vertex query fact",
            "evidence": [{"path": evidence.name, "sha256": hashlib.sha256(evidence.read_bytes()).hexdigest()}],
            "replacements": [{"key": before["key"],
                "before": {k: before[k] for k in ("spirv_sha256", "metadata_sha256", "gzip_sha256")},
                "after": {k: after[k] for k in ("spirv_sha256", "metadata_sha256", "gzip_sha256")}}]}))
        return plan

    def test_identical_duplicate_is_accepted_with_both_sources(self):
        first, row = self.cache("first")
        second, _ = self.cache("second")
        result = merge_caches([first, second], self.root / "merged")
        self.assertEqual(result["exported"], 1)
        self.assertEqual(len(result["merged_sources"]), 2)
        self.assertEqual(result["identical_duplicates"][0]["key"], row["key"])
        read_validated_cache(self.root / "merged")

    def test_disjoint_programs_are_kept(self):
        first, _ = self.cache("first")
        second, _ = self.cache("second", code=b"another program")
        result = merge_caches([first, second], self.root / "merged")
        self.assertEqual(result["exported"], 2)
        self.assertEqual(result["identical_duplicates"], [])

    def test_conflicting_translation_is_rejected_without_output(self):
        first, _ = self.cache("first")
        for name, overrides in (("changed-spv", {"spirv": b"different valid translation"}),
                                ("changed-meta", {"metadata": b"different mapping"})):
            with self.subTest(name=name):
                second, _ = self.cache(name, **overrides)
                output = self.root / (name + "-output")
                with self.assertRaisesRegex(ValueError, "Conflicting duplicate payload"):
                    merge_caches([first, second], output)
                self.assertFalse(output.exists())

    def test_duplicate_cannot_hide_tampered_payload(self):
        first, row = self.cache("first")
        second, _ = self.cache("second")
        (second / (row["key"] + ".vkmeta")).write_bytes(b"tampered")
        with self.assertRaisesRegex(ValueError, "payload hash mismatch"):
            merge_caches([first, second], self.root / "merged")

    def test_uncompressed_hash_is_checked(self):
        root, _ = self.cache("first")
        report = json.loads((root / "validation.json").read_text())
        report["records"][0]["spirv_sha256"] = "0" * 64
        (root / "validation.json").write_text(json.dumps(report))
        with self.assertRaisesRegex(ValueError, "uncompressed SPIR-V hash mismatch"):
            read_validated_cache(root)

    def test_incomplete_source_is_rejected(self):
        root, _ = self.cache("first")
        report = json.loads((root / "validation.json").read_text())
        report["structural_validation_complete"] = False
        (root / "validation.json").write_text(json.dumps(report))
        with self.assertRaisesRegex(ValueError, "complete validated"):
            read_validated_cache(root)

    def test_explicit_overlay_preserves_identity_and_records_change(self):
        base, before = self.cache("base")
        replacement, after = self.cache("typed", spirv=b"typed valid translation")
        plan = self.overlay_plan(before, after)
        output = self.root / "overlaid"
        result = overlay_cache(base, replacement, plan, output)
        self.assertEqual((output / "shader_index.bin").read_bytes(), (base / "shader_index.bin").read_bytes())
        self.assertEqual(result["records"][0]["spirv_sha256"], after["spirv_sha256"])
        self.assertEqual(len(result["overlay_plan"]["evidence"]), 1)
        read_validated_cache(output)

    def test_stale_overlay_hash_is_rejected(self):
        base, before = self.cache("base")
        replacement, after = self.cache("typed", spirv=b"typed valid translation")
        plan = self.overlay_plan(before, after)
        raw = json.loads(plan.read_text())
        raw["replacements"][0]["before"]["metadata_sha256"] = "0" * 64
        plan.write_text(json.dumps(raw))
        with self.assertRaisesRegex(ValueError, "before/after hash mismatch"):
            overlay_cache(base, replacement, plan, self.root / "overlaid")

    def test_overlay_evidence_must_match(self):
        base, before = self.cache("base")
        replacement, after = self.cache("typed", spirv=b"typed valid translation")
        plan = self.overlay_plan(before, after)
        (self.root / "vertex-type-evidence.txt").write_text("modified")
        with self.assertRaisesRegex(ValueError, "evidence hash mismatch"):
            overlay_cache(base, replacement, plan, self.root / "overlaid")

    def test_overlay_cannot_add_a_new_original_program(self):
        base, before = self.cache("base")
        replacement, after = self.cache("typed", code=b"new original program")
        plan = self.overlay_plan({**before, "key": after["key"]}, after)
        with self.assertRaisesRegex(ValueError, "preserve an existing original"):
            overlay_cache(base, replacement, plan, self.root / "overlaid")


if __name__ == "__main__":
    unittest.main()
