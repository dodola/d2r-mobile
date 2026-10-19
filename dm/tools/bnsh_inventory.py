"""Extract bounded NVN shader records from authenticated BF SHA/BNSH resources.

Binary field reference: KillzXGaming/ShaderLibrary, Structs.cs and BnshLoader.cs.
This independent reader never executes a shader. Texture facts are inferred
only from explicit typed instructions; ambiguous queries remain unresolved.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
from maxwell_sampler_facts import infer,infer_images

STAGES = ("vs", "hs", "ds", "gs", "ps", "cs")

def program_key(control: bytes, code: bytes) -> str:
    return hashlib.sha256(control+code).hexdigest()[:16]


def span(data: bytes, offset: int, size: int) -> bytes:
    if offset < 0 or size < 0 or offset > len(data) or size > len(data)-offset:
        raise ValueError("BNSH range outside archive")
    return data[offset:offset+size]


def unpack(fmt: str, data: bytes, offset: int):
    return struct.unpack(fmt, span(data, offset, struct.calcsize(fmt)))


def records(data: bytes):
    """Yield stage/control/code with offsets relative to the authenticated file."""
    if not data.startswith((b"FSHA    ", b"BNSH\0\0\0\0")):
        raise ValueError("Expected Switch BF SHA or BNSH")
    found = 0
    for match in re.finditer(b"BNSH\0\0\0\0", data):
        start = match.start()
        size, = unpack("<I", data, start+28)
        binary = span(data, start, size)
        if size < 0x98 or binary[12:14] != b"\xff\xfe" or binary[0x60:0x64] != b"grsc":
            raise ValueError("Invalid BNSH header")
        count, = unpack("<I", binary, 0x7c)
        variations, = unpack("<Q", binary, 0x80)
        if not count or count > 100000:
            raise ValueError("Invalid BNSH variation count")
        span(binary, variations, count*64)
        for variation in range(count):
            program, = unpack("<Q", binary, variations+variation*64+16)
            span(binary, program, 0xc0)
            for index, stage in enumerate(STAGES):
                shader, = unpack("<Q", binary, program+8+index*8)
                if not shader:
                    continue
                control_offset, code_offset, code_size, control_size = unpack("<QQII", binary, shader+8)
                source_control = span(binary, control_offset, control_size)
                # Particle archives append compiler/debug information to the
                # fixed NVN 1.16 runtime header. Preserve provenance for that
                # entire block; identity uses the 0x880 bytes consumed by NVN.
                control = source_control[:0x880]
                code = span(binary, code_offset, code_size)
                if len(control) != 0x880 or control[:4] != bytes.fromhex("34127698"):
                    raise ValueError("Unsupported NVN control block")
                if unpack("<II", control, 4) != (1, 16):
                    raise ValueError("Only the inspected NVN 1.16 control layout is supported")
                const_size, const_offset, _, program_offset = unpack("<4I", control, 0x6fc)
                span(code, const_offset, const_size)
                if not program_offset or program_offset >= len(code):
                    raise ValueError("Invalid NVN program offset")
                if stage in ("vs", "ps"):
                    if code[:4] != bytes.fromhex("78563412"):
                        raise ValueError("Invalid NVN graphics magic")
                    sph, = unpack("<I", code, program_offset)
                    if (sph >> 10) & 15 != {"vs": 1, "ps": 5}[stage]:
                        raise ValueError("BNSH/SPH stage mismatch")
                elif stage == "cs":
                    if code[:4] != bytes.fromhex("79563412"):
                        raise ValueError("Invalid NVN compute magic")
                else:
                    raise ValueError(f"Stage requires renderer adaptation: {stage}")
                facts = {}
                if stage == "cs":
                    local = list(unpack("<3I", control, 0x748))
                    shared, = unpack("<I", control, 0x754)
                    if not all(local) or any(x > 1024 for x in local) or local[0]*local[1]*local[2] > 1024:
                        raise ValueError("Invalid NVN compute workgroup")
                    facts = {"local": local, "shared": shared}
                yield {"bnsh_offset": start, "variation": variation, "stage": stage,
                       "source_control_size": control_size,
                       "source_control_sha256": hashlib.sha256(source_control).hexdigest(),
                       "control_offset": start+control_offset, "code_offset": start+code_offset,
                       "control": control, "code": code, "program_offset": program_offset,
                       "constants": code[const_offset:const_offset+const_size], "facts": facts}
                found += 1
    if not found:
        raise ValueError("No supported BNSH records")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--resources", required=True, type=Path, help="romfs_inventory.py manifest")
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--image-reflection",type=Path,help="Optional first-pass translator reflection for lowered bound surface instructions")
    ap.add_argument("--required-keys",type=Path,help="Optional JSON list of exact program identities to export; all must be found")
    args = ap.parse_args()
    repo = Path(__file__).resolve().parents[2]
    out = args.out.resolve()
    if repo == out or repo in out.parents or (out.exists() and any(out.iterdir())):
        raise ValueError("Use a fresh output directory outside the checkout")
    manifest = json.loads(args.resources.read_text(encoding="utf8"))
    required = None if args.required_keys is None else set(json.loads(args.required_keys.read_text(encoding="utf8")))
    if required is not None and (not required or any(not isinstance(k,str) or re.fullmatch(r"[0-9a-f]{16}",k) is None for k in required)):
        raise ValueError("required keys must be a nonempty list of 16 lowercase hex digits")
    if not manifest.get("metadata_validated") or not manifest.get("payload_sha256_complete"):
        raise ValueError("A validated resource inventory is required")
    out.mkdir(parents=True)
    (out/"code").mkdir()
    known, rows, jobs = {}, [], []
    for file in manifest["files"]:
        if not file["path"].endswith((".bfsha", ".bnsh")):
            continue
        path = Path(file["extracted"])
        data = path.read_bytes()
        if len(data) != file["size"] or hashlib.sha256(data).hexdigest() != file["sha256"]:
            raise ValueError(f"Shader archive identity mismatch: {file['path']}")
        for record in records(data):
            control, code = record.pop("control"), record.pop("code")
            constants, facts = record.pop("constants"), record.pop("facts")
            digest = hashlib.sha256(control).hexdigest()
            # Control blocks are pipeline properties, not a program digest.
            # Distinct machine-code programs can share byte-identical controls.
            key = program_key(control,code)
            if required is not None and key not in required:
                continue
            code_digest = hashlib.sha256(code).hexdigest()
            signature = (digest, code_digest, record["stage"])
            if key in known and known[key] != signature:
                raise ValueError("Control key collision or conflicting program bytes")
            if key not in known:
                known[key] = signature
                (out/(key+".control.bin")).write_bytes(control)
                source = out/"code"/(key+".bin")
                source.write_bytes(code[record["program_offset"]:])
                program_size,=unpack("<I",control,0x6f8)
                texture_facts,evidence,conflicts=infer(code[record["program_offset"]:],record["stage"],program_size)
                image_facts,image_evidence=infer_images(code[record["program_offset"]:],record["stage"],program_size)
                if args.image_reflection:
                    reflected=(args.image_reflection/(key+".res.json")).read_bytes()
                    reflection=json.loads(reflected.decode("utf-8-sig"))
                    if reflection["key"]!=key or reflection["stage"]!=record["stage"]:
                        raise ValueError("Surface reflection identity mismatch")
                    for image in reflection["images"]:
                        offset=(image["handle"]&0x3fff)*4
                        if image["handle"]>>28 or image["separate"] or image["array"]!=1 or image["cbuf_slot"]!=2 or not 0x120<=offset<0x168 or (offset-0x120)%8:
                            raise ValueError("Surface reflection requires bindless/array adaptation")
                        kind={1:"1d",2:"buffer",3:"2d",4:"3d",5:"cube"}.get(image["type"]&255)
                        if kind is None:raise ValueError("Unknown typed surface dimension")
                        if image["type"]&256:kind+="array"
                        if image["type"]&512:kind=kind.replace("2d","2dms")
                        slot=str((offset-0x120)//8)
                        if slot in image_facts and image_facts[slot]["type"]!=kind:raise ValueError("Conflicting surface instruction dimensions")
                        image_facts[slot]={"type":kind}
                        image_evidence.append({"kind":"typed surface with handle lowered to c2","reflection_sha256":hashlib.sha256(reflected).hexdigest(),"resource":image})
                jobs.append({"key": key, "stage": record["stage"], "code": str(source),
                             "c1": constants.hex(), "debug": True, "reduced": False,
                             "texture_handles":"nvn_combined_v1", "tex":texture_facts,"img":image_facts,
                             "image_fact_evidence":image_evidence,
                             "texture_fact_evidence":evidence,"texture_fact_conflicts":conflicts, **facts})
            rows.append({"source": file["path"], "source_sha256": file["sha256"], "key": key,
                         "control_sha256": digest, "code_sha256": code_digest, **record})
    if required is not None and required != known.keys():
        raise ValueError(f"Required original programs missing: {sorted(required-known.keys())}")
    result = {"resources_sha256": hashlib.sha256(args.resources.read_bytes()).hexdigest(),
              "required_keys_sha256": None if args.required_keys is None else hashlib.sha256(args.required_keys.read_bytes()).hexdigest(),
              "records": rows, "unique_programs": len(known), "gpu_query_facts_complete": False,
              "runtime_rendering_verified": False}
    (out/"inventory.json").write_text(json.dumps(result, indent=2)+"\n", encoding="utf8")
    (out/"jobs.json").write_text(json.dumps(jobs, indent=2)+"\n", encoding="utf8")
    print(f"Extracted {len(rows)} records, {len(known)} unique programs; GPU query facts still required")


if __name__ == "__main__":
    main()
