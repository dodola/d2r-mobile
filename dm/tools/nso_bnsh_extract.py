"""Extract bounded embedded BNSH archives from a SHA-256-pinned original NSO.

Produces a resource manifest accepted by bnsh_inventory.py. No executable code
is run or patched. Console files and generated shader data stay outside Git.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
from bnsh_inventory import records
from nso_inventory import Nso


def extract(nso: Nso):
    for segment in nso.segments:
        if segment["name"] not in ("rodata", "data"):
            continue
        raw = nso.read_memory(segment["memory_offset"], segment["size_bytes"])
        for match in re.finditer(b"BNSH\0\0\0\0", raw):
            start = match.start()
            if len(raw) - start < 32:
                continue
            size = struct.unpack_from("<I", raw, start + 28)[0]
            if size < 0x98 or size > len(raw) - start:
                continue  # A literal marker string is not an embedded resource.
            archive = raw[start:start + size]
            entries = list(records(archive))  # Validate every control/code range.
            yield segment["memory_offset"] + start, archive, len(entries)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nso", required=True, type=Path)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    out = args.out.resolve()
    repo = Path(__file__).resolve().parents[2]
    if out.is_relative_to(repo) or out.exists() and any(out.iterdir()):
        parser.error("use a fresh/empty output directory outside the checkout")
    nso = Nso(args.nso)
    if nso.source_sha256 != args.sha256.lower():
        parser.error("original NSO SHA-256 does not match pinned source")
    archives = list(extract(nso))
    if not archives:
        parser.error("no validated embedded BNSH archive found")
    out.mkdir(parents=True, exist_ok=True)
    files = []
    for offset, archive, count in archives:
        name = f"{args.nso.name}-{offset:x}.bnsh"
        dest = out / name
        dest.write_bytes(archive)
        files.append({"path": name, "extracted": str(dest), "size": len(archive),
                      "sha256": hashlib.sha256(archive).hexdigest(), "stage_records": count,
                      "nso_memory_offset": offset, "nso_source_sha256": nso.source_sha256})
    manifest = {"source_kind": "sha256-pinned-nso", "metadata_validated": True, "payload_sha256_complete": True,
                "source_nso": str(args.nso.resolve()), "source_sha256": nso.source_sha256,
                "build_id": nso.data[0x40:0x60].hex(), "files": files}
    (out / "resources.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf8")
    print(json.dumps({"archives": len(files), "stage_records": sum(f["stage_records"] for f in files)}))


if __name__ == "__main__":
    main()
