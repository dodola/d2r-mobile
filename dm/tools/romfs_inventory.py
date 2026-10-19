"""Validate an ordinary decompressed RomFS and inventory/extract selected files.

Accepts metadata before or after file payloads. Does not decrypt NCA storage.
All game bytes and generated manifests must remain outside the checkout.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import struct
from pathlib import Path, PurePosixPath
NULL = 0xffffffff
CHUNK = 8 * 1024 * 1024

class ImageReader:
    """Independent disk parser, with no dependency on constructed table objects."""

    def __init__(self, path: Path):
        self.stream = path.open("rb")
        self.size = path.stat().st_size
        values = struct.unpack("<10Q", self.read_exact(0, 80))
        if values[0] != 80:
            raise ValueError("RomFS header size differs from original SDK format")
        spans = [(values[i], values[i + 1]) for i in (1, 3, 5, 7)]
        ends = []
        for offset, size in spans:
            if offset < 80 or size == 0 or offset > self.size or size > self.size - offset:
                raise ValueError("Metadata range is empty or out of bounds")
            ends.append(offset + size)
        for (a, a_size), (b, b_size) in zip(sorted(spans), sorted(spans)[1:]):
            if a + a_size > b:
                raise ValueError("Overlapping metadata ranges")
        if values[9] < 80 or values[9] > self.size:
            raise ValueError("Invalid data region offset")
        self.metadata_spans = spans
        self.data_offset = values[9]
        self.dir_hash = self._buckets(self.read_exact(*spans[0]))
        self.directories = self._entries(self.read_exact(*spans[1]), True)
        self.file_hash = self._buckets(self.read_exact(*spans[2]))
        self.files = self._entries(self.read_exact(*spans[3]), False)
        self.paths: dict[str, dict] = {}
        self.directory_paths: dict[str, int] = {}
        self._validate_tree()
        self._validate_hashes(self.directories, self.dir_hash)
        self._validate_hashes(self.files, self.file_hash)
        ranges = sorted((entry["data"], entry["data"] + entry["size"]) for entry in self.files.values())
        for a, b in zip(ranges, ranges[1:]):
            if a[1] > b[0]:
                raise ValueError("File content intervals overlap")

    def close(self) -> None:
        self.stream.close()

    def read_exact(self, offset: int, size: int) -> bytes:
        if offset < 0 or size < 0 or offset > self.size or size > self.size - offset:
            raise ValueError("Storage read out of bounds")
        self.stream.seek(offset)
        result = self.stream.read(size)
        if len(result) != size:
            raise ValueError("Truncated image read")
        return result

    @staticmethod
    def _buckets(raw: bytes) -> tuple[int, ...]:
        if not raw or len(raw) % 4:
            raise ValueError("Malformed hash bucket table")
        return struct.unpack(f"<{len(raw) // 4}I", raw)

    def _entries(self, raw: bytes, directory: bool) -> dict[int, dict]:
        entries = {}
        position = 0
        fixed = 24 if directory else 32
        while position < len(raw):
            if len(raw) - position < fixed:
                raise ValueError("Truncated metadata entry")
            if directory:
                parent, sibling, child, file, next_hash, name_len = struct.unpack_from("<6I", raw, position)
                entry = {"parent": parent, "sibling": sibling, "child": child, "file": file}
            else:
                parent, sibling, data, size, next_hash, name_len = struct.unpack_from("<IIQQII", raw, position)
                if data > self.size - self.data_offset or size > self.size - self.data_offset - data:
                    raise ValueError("File range exceeds image storage")
                absolute = self.data_offset + data
                if size and any(absolute < off + length and off < absolute + size for off, length in self.metadata_spans):
                    raise ValueError("File payload overlaps metadata")
                entry = {"parent": parent, "sibling": sibling, "data": data, "size": size}
            finish = position + fixed + name_len
            if finish > len(raw):
                raise ValueError("Entry name exceeds table size")
            name = raw[position + fixed:finish]
            decoded = name.decode("utf-8", errors="strict")
            if b"\0" in name or "/" in decoded or "\\" in decoded or decoded in (".", ".."):
                raise ValueError("Invalid RomFS entry name")
            entry.update({"name": name, "decoded_name": decoded, "hash_next": next_hash, "offset": position})
            entries[position] = entry
            padded_finish = (finish + 3) & ~3
            if any(raw[finish:padded_finish]):
                raise ValueError("Nonzero metadata entry alignment padding")
            position = padded_finish
        return entries

    def _validate_tree(self) -> None:
        root = self.directories.get(0)
        if not root or root["name"] or root["parent"] != 0 or root["sibling"] != NULL:
            raise ValueError("Malformed RomFS root directory")
        seen_dirs = set()
        seen_files = set()

        def visit(offset: int, parent: int, path: str) -> None:
            if offset in seen_dirs or offset not in self.directories:
                raise ValueError("Directory tree cycle or invalid offset")
            directory = self.directories[offset]
            if directory["parent"] != parent or path in self.directory_paths:
                raise ValueError("Directory parent or path mismatch")
            seen_dirs.add(offset)
            self.directory_paths[path] = offset
            child = directory["child"]
            while child != NULL:
                item = self.directories.get(child)
                if not item or not item["name"]:
                    raise ValueError("Bad child directory reference")
                child_path = f"{path}/{item['decoded_name']}" if path else item["decoded_name"]
                visit(child, offset, child_path)
                child = item["sibling"]
            file = directory["file"]
            while file != NULL:
                item = self.files.get(file)
                if not item or file in seen_files or item["parent"] != offset or not item["name"]:
                    raise ValueError("Bad file tree reference")
                file_path = f"{path}/{item['decoded_name']}" if path else item["decoded_name"]
                if file_path in self.paths or file_path in self.directory_paths:
                    raise ValueError("Duplicate path")
                self.paths[file_path] = item
                seen_files.add(file)
                file = item["sibling"]

        visit(0, 0, "")
        if seen_dirs != set(self.directories) or seen_files != set(self.files):
            raise ValueError("Unreachable metadata entries")
        if set(self.paths) & set(self.directory_paths):
            raise ValueError("File and directory share a path")

    @staticmethod
    def _lookup_hash(parent: int, name: bytes) -> int:
        value = (parent ^ 0x075BCD15) & 0xFFFFFFFF
        for byte in name:
            value = ((value >> 5) ^ ((value << 27) & 0xFFFFFFFF) ^ byte) & 0xFFFFFFFF
        return value

    def _validate_hashes(self, entries: dict, buckets: tuple[int, ...]) -> None:
        seen = set()
        for bucket, offset in enumerate(buckets):
            while offset != NULL:
                item = entries.get(offset)
                if not item or offset in seen:
                    raise ValueError("Hash chain cycle or invalid entry offset")
                if self._lookup_hash(item["parent"], item["name"]) % len(buckets) != bucket:
                    raise ValueError("Entry assigned to incorrect hash bucket")
                seen.add(offset)
                offset = item["hash_next"]
        if seen != set(entries):
            raise ValueError("Entries absent from hash table")

    def _find(self, entries: dict, buckets: tuple[int, ...], parent: int, name: bytes) -> dict | None:
        offset = buckets[self._lookup_hash(parent, name) % len(buckets)]
        remaining = len(entries)
        while offset != NULL and remaining:
            remaining -= 1
            item = entries[offset]
            if item["parent"] == parent and item["name"] == name:
                return item
            offset = item["hash_next"]
        return None

    def find_file(self, path: str) -> dict | None:
        components = PurePosixPath(path).parts
        parent = 0
        for component in components[:-1]:
            found = self._find(self.directories, self.dir_hash, parent, component.encode("utf-8"))
            if found is None:
                return None
            parent = found["offset"]
        return self._find(self.files, self.file_hash, parent, components[-1].encode("utf-8"))

    def file_digest(self, entry: dict) -> str:
        self.stream.seek(self.data_offset + entry["data"])
        remaining = entry["size"]
        h = hashlib.sha256()
        while remaining:
            block = self.stream.read(min(CHUNK, remaining))
            if not block:
                raise ValueError("Unexpected file content EOF")
            remaining -= len(block)
            h.update(block)
        return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--image", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--extract-suffix", action="append", default=[])
    args = ap.parse_args()
    repo = Path(__file__).resolve().parents[2]
    out = args.out.resolve()
    if out == repo or repo in out.parents:
        raise ValueError("Generated resources must remain outside the checkout")
    if out.exists() and any(out.iterdir()):
        raise ValueError("Output must be empty")
    out.mkdir(parents=True, exist_ok=True)
    reader = ImageReader(args.image)
    rows = []
    try:
        for name, entry in sorted(reader.paths.items()):
            if reader.find_file(name) is not entry:
                raise ValueError("Tree/hash lookup mismatch")
            row = {"path": name, "offset": reader.data_offset + entry["data"],
                   "size": entry["size"], "sha256": reader.file_digest(entry)}
            if any(name.endswith(suffix) for suffix in args.extract_suffix):
                path = out / "files" / name
                path.parent.mkdir(parents=True, exist_ok=True)
                with path.open("xb") as dest:
                    offset, left = row["offset"], row["size"]
                    while left:
                        block = reader.read_exact(offset, min(CHUNK, left))
                        dest.write(block); offset += len(block); left -= len(block)
                row["extracted"] = str(path)
            rows.append(row)
        digest = hashlib.sha256()
        reader.stream.seek(0)
        while block := reader.stream.read(CHUNK):
            digest.update(block)
        result = {"image": str(args.image.resolve()), "image_bytes": reader.size,
                  "image_sha256": digest.hexdigest(), "file_count": len(rows),
                  "directory_count": len(reader.directories), "metadata_validated": True,
                  "payload_sha256_complete": True, "runtime_rendering_verified": False,
                  "files": rows}
        (out / "romfs-inventory.json").write_text(json.dumps(result, indent=2)+"\n", encoding="utf8")
        print(json.dumps({key: value for key, value in result.items() if key != "files"}))
    finally:
        reader.close()

if __name__ == "__main__":
    main()
