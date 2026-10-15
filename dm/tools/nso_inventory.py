"""Inventory NSOs: dynamic symbols, relocations and AArch64 sites.

This performs static inspection only and never executes the input code.
"""

import argparse
import hashlib
import json
import re
import struct
from collections import Counter, defaultdict
from pathlib import Path


def nn_scope(name):
    match = re.search(r"_Z[NKVR]*2nn(\d+)", name)
    if match:
        start = match.end()
        return "nn::" + name[start:start + int(match.group(1))]
    if name.startswith("nvn"):
        return "nvn"
    return "other"


class Nso:
    def __init__(self, path):
        self.path = path
        self.data = path.read_bytes()
        self.source_sha256 = hashlib.sha256(self.data).hexdigest()
        if len(self.data) < 256 or self.data[:4] != b"NSO0":
            raise ValueError("Not an NSO0 file")
        self.flags = struct.unpack_from("<I", self.data, 12)[0]
        if self.flags & 7:
            import lz4.block
            source = self.data
            decoded = bytearray(source[:256])
            for index in range(3):
                offset, _, size = struct.unpack_from("<III", source, 16 + index * 16)
                stored = struct.unpack_from("<I", source, 0x60 + index * 4)[0] if self.flags & (1 << index) else size
                if offset + stored > len(source):
                    raise ValueError("Truncated compressed NSO segment")
                raw = source[offset:offset + stored]
                if self.flags & (1 << index):
                    raw = lz4.block.decompress(raw, uncompressed_size=size)
                if len(raw) != size:
                    raise ValueError("NSO decoded size mismatch")
                struct.pack_into("<I", decoded, 16 + index * 16, len(decoded))
                decoded.extend(raw)
            struct.pack_into("<I", decoded, 12, self.flags & ~7)
            self.data = bytes(decoded)
        self.segments = []
        for index, name in enumerate(("text", "rodata", "data")):
            offset, address, size, extra = struct.unpack_from("<IIII", self.data, 16 + index * 16)
            if offset + size > len(self.data):
                raise ValueError("Truncated NSO segment")
            segment = {"name": name, "file_offset": offset, "memory_offset": address, "size_bytes": size}
            if name == "data":
                segment["bss_bytes"] = extra
            segment["sha256_matches"] = hashlib.sha256(self.data[offset:offset + size]).digest() == self.data[0xa0 + index * 32:0xc0 + index * 32]
            if self.flags & (1 << (index + 3)) and not segment["sha256_matches"]:
                raise ValueError("NSO segment hash mismatch")
            self.segments.append(segment)

    def read_memory(self, address, size):
        for segment in self.segments:
            relative = address - segment["memory_offset"]
            if relative >= 0 and relative + size <= segment["size_bytes"]:
                start = segment["file_offset"] + relative
                return self.data[start:start + size]
        raise ValueError("Read outside initialized NSO segments")

    def symbols(self):
        rodata = self.segments[1]
        string_offset, string_size, symbol_offset, symbol_size = struct.unpack_from("<IIII", self.data, 0x90)
        strings = self.read_memory(rodata["memory_offset"] + string_offset, string_size)
        table = self.read_memory(rodata["memory_offset"] + symbol_offset, symbol_size)
        if symbol_size % 24:
            raise ValueError("Invalid ELF64 dynamic symbol table size")
        result = []
        for index in range(symbol_size // 24):
            offset, info, other, section, value, size = struct.unpack_from("<IBBHQQ", table, index * 24)
            if offset >= string_size:
                raise ValueError("Invalid dynamic symbol string offset")
            end = strings.find(b"\0", offset)
            if end < 0:
                raise ValueError("Unterminated dynamic symbol name")
            name = strings[offset:end].decode("utf-8", errors="replace")
            if not name:
                continue
            result.append({
                "index": index, "name": name, "defined": section != 0,
                "binding": info >> 4, "type": info & 15, "visibility": other & 3,
                "value": value, "size": size, "scope": nn_scope(name),
            })
        return result

    def relocations(self):
        text = self.segments[0]
        mod_address = struct.unpack_from("<I", self.read_memory(text["memory_offset"], 8), 4)[0]
        if self.read_memory(mod_address, 4) != b"MOD0":
            raise ValueError("Cannot locate MOD0")
        dynamic_relative = struct.unpack("<i", self.read_memory(mod_address + 4, 4))[0]
        dynamic_address = mod_address + dynamic_relative
        dynamic = {}
        for index in range(4096):
            tag, value = struct.unpack("<qQ", self.read_memory(dynamic_address + index * 16, 16))
            if tag == 0:
                break
            dynamic[tag] = value
        else:
            raise ValueError("Unterminated dynamic table")
        entries = {}
        for label, address, size in (("rela", dynamic.get(7, 0), dynamic.get(8, 0)), ("plt", dynamic.get(23, 0), dynamic.get(2, 0))):
            if not size:
                continue
            if size % 24 or (label == "plt" and dynamic.get(20, 7) != 7):
                raise ValueError("Unsupported relocation format")
            raw = self.read_memory(address, size)
            for index in range(size // 24):
                target, info, addend = struct.unpack_from("<QQq", raw, index * 24)
                entries[address + index * 24] = {"target": target, "type": info & 0xffffffff, "symbol_index": info >> 32, "addend": addend}
        names = {257: "ABS64", 258: "ABS32", 1025: "GLOB_DAT", 1026: "JUMP_SLOT", 1027: "RELATIVE", 1028: "TLS_DTPMOD64", 1029: "TLS_DTPREL64", 1030: "TLS_TPREL64", 1031: "TLSDESC"}
        counts = Counter(names.get(e["type"], str(e["type"])) for e in entries.values())
        relr_size, relr_count = dynamic.get(35, 0), 0
        if relr_size:
            if dynamic.get(37) != 8 or relr_size % 8:
                raise ValueError("Invalid ELF64 RELR entry size")
            cursor = None
            image_end = self.segments[2]["memory_offset"] + self.segments[2]["size_bytes"] + self.segments[2]["bss_bytes"]
            table = self.read_memory(dynamic[36], relr_size)
            for (word,) in struct.iter_unpack("<Q", table):
                if word & 1:
                    if cursor is None:
                        raise ValueError("ELF64 RELR bitmap precedes address")
                    offsets = (cursor + bit * 8 for bit in range(63) if word & (1 << (bit + 1)))
                    next_cursor = cursor + 63 * 8
                else:
                    offsets = (word,)
                    next_cursor = word + 8
                for offset in offsets:
                    if offset < self.segments[1]["memory_offset"] or offset + 8 > image_end:
                        raise ValueError("ELF64 RELR target outside non-code image")
                    relr_count += 1
                cursor = next_cursor
            counts["RELR"] = relr_count
        return {"mod0_address": mod_address, "dynamic_address": dynamic_address,
                "counts": dict(counts), "total": len(entries) + relr_count,
                "relr": {"table_words": relr_size // 8, "relocations": relr_count}}

    def instruction_sites(self):
        text = self.segments[0]
        code = self.read_memory(text["memory_offset"], text["size_bytes"])
        svc = defaultdict(list)
        special = defaultdict(list)
        sysreg_opcodes = {0xd53bd040: "MRS_TPIDR_EL0", 0xd53bd060: "MRS_TPIDRRO_EL0", 0xd51bd040: "MSR_TPIDR_EL0", 0xd53be000: "MRS_CNTFRQ_EL0", 0xd53be020: "MRS_CNTPCT_EL0", 0xd53be040: "MRS_CNTVCT_EL0"}
        for offset in range(0, len(code) - 3, 4):
            word = struct.unpack_from("<I", code, offset)[0]
            if word & 0xffe0001f == 0xd4000001:
                svc[f"0x{(word >> 5) & 0xffff:02x}"].append(text["memory_offset"] + offset)
            name = sysreg_opcodes.get(word & 0xffffffe0)
            if name:
                special[name].append(text["memory_offset"] + offset)
        return {"svc": dict(svc), "system_registers": dict(special), "limit": "Encoding matches within text; reachability and execution are not established."}

    def inventory(self):
        symbols = self.symbols()
        imports = [s for s in symbols if not s["defined"]]
        exports = [s for s in symbols if s["defined"] and s["binding"] in (1, 2) and s["visibility"] in (0, 3)]
        nvn_strings = sorted(set(m.group(1).decode("ascii") for m in re.finditer(rb"(?<![^\x00])(nvn[A-Za-z0-9_]+)(?=\x00)", self.data)))
        return {
            "module": self.path.stem, "path": str(self.path.resolve()),
            "source_sha256": self.source_sha256,
            "build_id": self.data[0x40:0x60].hex(), "segments": self.segments,
            "imports": imports, "exports": exports,
            "import_scope_counts": dict(Counter(s["scope"] for s in imports)),
            "relocations": self.relocations(), "instruction_sites": self.instruction_sites(),
            "nvn_symbol_like_strings": nvn_strings,
        }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    modules = [Nso(path).inventory() for path in sorted(args.directory.glob("*.nso"))]
    if not modules:
        parser.error("No NSOs found")
    providers = defaultdict(list)
    for module in modules:
        for symbol in module["exports"]:
            providers[symbol["name"]].append(module["module"])
    edges = Counter()
    unresolved = []
    for module in modules:
        for symbol in module["imports"]:
            found = providers[symbol["name"]]
            symbol["candidate_providers"] = found
            for provider in found:
                edges[(module["module"], provider)] += 1
            if not found:
                unresolved.append({"module": module["module"], "name": symbol["name"], "binding": symbol["binding"]})
    report = {
        "modules": modules,
        "dependency_edges": [{"consumer": a, "provider": b, "symbols": n} for (a, b), n in sorted(edges.items())],
        "unresolved_imports": unresolved,
        "limits": [
            "Dynamic symbols are direct binary dependencies, not a complete list of IPC services or commands.",
            "Export name matches are candidate providers, not a simulation of runtime linker binding.",
            "NVN strings can include unused SDK routines; they do not demonstrate actual call coverage.",
            "No game execution, Android loading, shader translation or GPU measurements have occurred.",
        ],
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    summary = [{"module": m["module"], "imports": len(m["imports"]), "exports": len(m["exports"]), "scopes": m["import_scope_counts"], "relocations": m["relocations"]["counts"], "svc_sites": sum(map(len, m["instruction_sites"]["svc"].values())), "nvn_strings": len(m["nvn_symbol_like_strings"])} for m in modules]
    print(json.dumps({"modules": summary, "edges": report["dependency_edges"], "unresolved_count": len(unresolved)}, ensure_ascii=True))


if __name__ == "__main__":
    main()
