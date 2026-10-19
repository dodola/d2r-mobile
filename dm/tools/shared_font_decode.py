"""Decode user-extracted firmware shared fonts; validate every SFNT table.

No console keys or font contents are bundled. The wrapper's repeating mask is
inferred from SFNT signatures, accepted only if exactly one complete font passes
all directory bounds and table checksums. NCA extraction is a separate step.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct

REPO = Path(__file__).resolve().parents[2]
FONT_PATTERNS = (
    "nintendo_udsg-r_std_*.bfttf",
    "nintendo_udsg-r_org_zh-cn_*.bfttf",
    "nintendo_udsg-r_ext_zh-cn_*.bfttf",
    "nintendo_udjxh-db_zh-tw_*.bfttf",
    "nintendo_udsg-r_ko_*.bfttf",
    "nintendo_ext_*.bfttf",
)


def valid_sfnt(data: bytes) -> bool:
    if len(data) < 12 or data[:4] not in (b"\x00\x01\x00\x00", b"OTTO"):
        return False
    count = struct.unpack_from(">H", data, 4)[0]
    if not 1 <= count <= 512 or 12 + count * 16 > len(data):
        return False
    tags = set()
    for i in range(count):
        tag, checksum, offset, size = struct.unpack_from(">4sIII", data, 12 + i * 16)
        if tag in tags or offset % 4 or offset < 12 + count * 16 or offset > len(data) or size > len(data) - offset:
            return False
        tags.add(tag)
        table = bytearray(data[offset:offset + size])
        if tag == b"head":
            if size < 16:
                return False
            table[8:12] = bytes(4)  # checkSumAdjustment is zero for the table checksum.
        table += bytes(-len(table) % 4)
        actual = sum(struct.unpack(">" + str(len(table) // 4) + "I", table)) & 0xffffffff
        if actual != checksum:
            return False
    return True


def decode(data: bytes) -> bytes:
    if len(data) < 20 or len(data) > 32 * 1024 * 1024 + 8:
        raise ValueError("shared font wrapper size")
    encrypted = data[8:]
    candidates = []
    for signature in (b"\x00\x01\x00\x00", b"OTTO"):
        mask = bytes(a ^ b for a, b in zip(encrypted[:4], signature))
        candidate = bytes(value ^ mask[i % 4] for i, value in enumerate(encrypted))
        if valid_sfnt(candidate):
            candidates.append(candidate)
    if len(candidates) != 1:
        raise ValueError("invalid or ambiguous shared font (SFNT bounds/checksum failure)")
    return candidates[0]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-dir", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    out = args.out.resolve()
    if out.is_relative_to(REPO) or out.exists() and any(out.iterdir()):
        parser.error("output must be a fresh/empty directory outside the checkout")
    fonts = []
    for kind, pattern in enumerate(FONT_PATTERNS):
        matches = sorted(args.input_dir.rglob(pattern))
        if len(matches) != 1:
            parser.error(f"font type {kind} requires exactly one {pattern}")
        source = matches[0]
        wrapped = source.read_bytes()
        fonts.append((source, wrapped, decode(wrapped)))
    out.mkdir(parents=True, exist_ok=True)
    receipt = []
    for kind, (source, wrapped, data) in enumerate(fonts):
        (out / f"{kind}.sfnt").write_bytes(data)
        receipt.append({"type": kind, "source": source.name, "bytes": len(data),
                        "source_sha256": hashlib.sha256(wrapped).hexdigest(),
                        "sha256": hashlib.sha256(data).hexdigest(), "all_table_checksums_valid": True})
    (out / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf8")
    print(json.dumps({"fonts": len(fonts), "out": str(out)}))


if __name__ == "__main__":
    main()
