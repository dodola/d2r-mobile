"""Validate BNSH translations and pack a full-code-identity Android Vulkan cache.

Missing GPU query facts remain excluded. Validation proves SPIR-V structure,
not rendering equivalence; keep the source inventories and validation report.
"""
from pathlib import Path
import argparse, concurrent.futures, gzip, hashlib, json, shutil, struct, subprocess, sys
from functools import lru_cache
from spirv_static_offsets import fold_sample_offsets
SUPPORT, CBUF, SSBO, TEX, SAMP, IMG, C1, DRIVER, UBOP, TBUF = range(1, 11)
IMAGE_QUERY = 11
STAGES = {"vs": 0, "ps": 1, "cs": 5}


def distance_capabilities(instructions):
    """BuiltIn declarations require capabilities even if spirv-val permits omission.

    Both direct variables and gl_PerVertex struct members occur in translations.
    Keep declared ClipDistance/CullDistance capabilities conservatively: proving
    an unused member removable requires a separate interface/dataflow rewrite.
    """
    required = set()
    for instruction in instructions:
        opcode = instruction[0] & 0xffff
        if opcode == 71 and len(instruction) >= 4 and instruction[2] == 11:
            builtin = instruction[3]
        elif opcode == 72 and len(instruction) >= 5 and instruction[3] == 11:
            builtin = instruction[4]
        else:
            continue
        if builtin in (3, 4):
            required.add(32 if builtin == 3 else 33)
    return required


def declare_distance_capabilities(path: Path):
    raw = path.read_bytes()
    if len(raw) < 20 or len(raw) % 4:
        raise ValueError("Invalid SPIR-V byte length while declaring distance capabilities")
    words = list(struct.unpack(f"<{len(raw) // 4}I", raw))
    if words[0] != 0x07230203:
        raise ValueError("Invalid SPIR-V magic while declaring distance capabilities")
    instructions, existing = [], set()
    i = 5
    while i < len(words):
        count, opcode = words[i] >> 16, words[i] & 0xffff
        if not count or i + count > len(words):
            raise ValueError("Invalid SPIR-V instruction bounds while declaring distance capabilities")
        instruction = words[i:i + count]
        instructions.append(instruction)
        if opcode == 17:
            if count != 2:
                raise ValueError("Invalid SPIR-V capability declaration")
            existing.add(instruction[1])
        i += count
    added = sorted(distance_capabilities(instructions) - existing)
    if added:
        words[5:5] = [word for capability in added for word in ((2 << 16) | 17, capability)]
        path.write_bytes(struct.pack(f"<{len(words)}I", *words))
    return added


def trim_unused_capabilities(path: Path, validator: Path):
    """Remove a declaration only if the official validator proves it unused.

    Upstream declares optional desktop capabilities even for trivial shaders.
    Declared ClipDistance/CullDistance capabilities are never removed: some
    spirv-val releases accept their omission despite Vulkan feature requirements.
    Instructions, types, constants, control flow and bindings are untouched.
    In particular, a used Float64/Interlock declaration cannot disappear this
    way: its removal is rejected by spirv-val and the device check then fails.
    """
    raw = path.read_bytes()
    words = list(struct.unpack(f"<{len(raw) // 4}I", raw))
    instructions = []
    i = 5
    while i < len(words):
        count, opcode = words[i] >> 16, words[i] & 0xffff
        if not count or i + count > len(words):
            raise ValueError("SPIR-V instruction bounds invalid during capability trimming")
        instructions.append(words[i:i + count])
        i += count
    mandatory = {1} | distance_capabilities(instructions)
    temporary = path.with_suffix(".capcheck.spv")
    removed = []
    # Dependencies may become removable after another capability is removed.
    changed = True
    while changed:
        changed = False
        for instruction in list(instructions):
            opcode = instruction[0] & 0xffff
            if opcode not in (10, 17) or (opcode == 17 and instruction[1] in mandatory):
                continue
            candidate = list(instructions)
            candidate.remove(instruction)
            flat = words[:5] + [v for inst in candidate for v in inst]
            temporary.write_bytes(struct.pack(f"<{len(flat)}I", *flat))
            check = subprocess.run([str(validator), "--target-env", "vulkan1.1", str(temporary)],
                                   capture_output=True)
            if check.returncode == 0:
                removed.append({"opcode": opcode, "operands": instruction[1:]})
                instructions = candidate
                changed = True
    flat = words[:5] + [v for inst in instructions for v in inst]
    path.write_bytes(struct.pack(f"<{len(flat)}I", *flat))
    temporary.unlink(missing_ok=True)
    return removed

def declare_unformatted_storage_image_access(path: Path):
    """Declare the mandatory Vulkan capabilities for unformatted image IO.

    The upstream Metal-bound intermediate omits these declarations. The renderer must validate the corresponding device features. This changes declarations only, never shader operations.
    """
    raw = path.read_bytes()
    words = list(struct.unpack(f"<{len(raw) // 4}I", raw))
    capabilities, image_unknown, image_read, image_write = set(), False, False, False
    i = 5
    while i < len(words):
        count, opcode = words[i] >> 16, words[i] & 0xffff
        if not count or i + count > len(words):
            raise ValueError("Invalid SPIR-V while declaring image capabilities")
        if opcode == 17:
            capabilities.add(words[i + 1])
        elif opcode == 25 and count >= 9:
            # OpTypeImage: result,sampledType,dim,depth,array,ms,sampled,format
            image_unknown |= words[i + 7] == 2 and words[i + 8] == 0
        elif opcode == 98:
            image_read = True
        elif opcode == 99:
            image_write = True
        i += count
    required = ({55} if image_read else set()) | ({56} if image_write else set())
    added = sorted(required - capabilities) if image_unknown else []
    if added:
        insert = [value for cap in added for value in [(2 << 16) | 17, cap]]
        words[5:5] = insert
        path.write_bytes(struct.pack(f"<{len(words)}I", *words))
    return added

def spirv_layout(path: Path, stage: str, rewrite_set: bool):
    raw = path.read_bytes()
    if len(raw) < 20 or len(raw) % 4:
        raise ValueError("SPIR-V byte length invalid")
    words = list(struct.unpack(f"<{len(raw) // 4}I", raw))
    if words[0] != 0x07230203:
        raise ValueError("SPIR-V magic invalid")
    set_number = {"vs": 0, "ps": 1, "cs": 0}[stage]
    entries, modes, capabilities, decorations, variables, types, names = [], [], [], {}, {}, {}, {}
    instructions=[]
    i = 5
    while i < len(words):
        count, opcode = words[i] >> 16, words[i] & 0xffff
        if not count or i + count > len(words):
            raise ValueError("SPIR-V instruction outside module")
        operands = words[i + 1:i + count]
        instructions.append((opcode,operands))
        if opcode == 5:
            names[operands[0]] = struct.pack(f"<{len(operands[1:])}I", *operands[1:]).split(b"\0", 1)[0].decode("utf8")
        elif opcode == 15:
            namebytes = struct.pack(f"<{len(operands[2:])}I", *operands[2:])
            entries.append({"model": operands[0], "id": operands[1],
                            "name": namebytes.split(b"\0", 1)[0].decode("utf8")})
        elif opcode == 16:
            modes.append(operands)
        elif opcode == 17:
            capabilities.append(operands[0])
        elif opcode == 71:
            target, decoration = operands[:2]
            if decoration == 34 and rewrite_set:
                words[i + 3] = set_number
                operands[2] = set_number
            decorations.setdefault(target, {})[str(decoration)] = operands[2:]
        elif opcode == 59:
            variables[operands[1]] = {"type_id": operands[0], "storage_class": operands[2]}
        elif opcode in range(19, 34):
            types[operands[0]] = {"opcode": opcode, "operands": operands[1:]}
        i += count
    expected = {"vs": 0, "ps": 4, "cs": 5}[stage]
    if len(entries) != 1 or entries[0]["model"] != expected:
        raise ValueError("SPIR-V stage does not match original package stage")
    if rewrite_set:
        path.write_bytes(struct.pack(f"<{len(words)}I", *words))
    resources, inputs, outputs = [], [], []
    for vid, var in variables.items():
        deco = decorations.get(vid, {})
        record = {"id": vid, "name": names.get(vid), **var, "decorations": deco}
        if "33" in deco:
            if "34" not in deco or deco["34"] != [set_number]:
                raise ValueError("Descriptor set remapping failed")
            values={vid};query=False;unsupported=False
            for opcode,operands in instructions:
                if opcode in (5,15,59,71):continue
                if opcode in (61,83,100) and len(operands)>2 and operands[2] in values:values.add(operands[1])
                elif opcode in (103,104,106,107) and len(operands)>2 and operands[2] in values:query=True
                elif any(value in values for value in operands):unsupported=True
            resources.append({**record, "set": set_number, "binding": deco["33"][0],"query_only":query and not unsupported})
        elif var["storage_class"] == 1:
            inputs.append(record)
        elif var["storage_class"] == 3:
            outputs.append(record)
    pairs = [(r["set"], r["binding"]) for r in resources]
    if len(pairs) != len(set(pairs)):
        raise ValueError("Duplicate descriptor binding in one shader")
    return {"entry_points": entries, "execution_modes": modes, "capabilities": capabilities,
            "descriptor_set": set_number, "resources": resources, "inputs": inputs,
            "outputs": outputs, "types": types, "spirv_version": hex(words[1])}


def descriptor(resource, reflected, stage, types):
    binding = resource["binding"]
    name = resource.get("name") or ""
    if name == "support_buffer": return (resource["set"],binding,6,SUPPORT,0,0,1,0)
    for b in reflected["cbufs"]:
        if b["binding"] == binding:
            bank = b["slot"]
            kind = CBUF if 3 <= bank <= 16 else C1 if bank == 1 else DRIVER
            return (resource["set"],binding,6,kind,bank - 3 if kind == CBUF else bank,0,1,0)
    for b in reflected["sbufs"]:
        if b["binding"] == binding:
            offset = b["cb_offset"] * 4
            base = {0:0x110,1:0x510,5:0x310}[stage]
            if b["cb_slot"]==0 and stage==5 and 0x210<=offset<0x310 and (offset-0x210)%16==0:
                return (resource["set"],binding,7,UBOP,(offset-0x210)//16,0,1,b["flags"])
            if b["cb_slot"] != 0 or offset < base or (offset-base)%16 or (offset-base)//16 >= 16:
                raise ValueError(f"Storage pointer bank/offset needs adaptation: {b}")
            return (resource["set"],binding,7,SSBO,(offset-base)//16,0,1,b["flags"])
    for table, kind in (("textures",TEX),("images",IMG)):
        for b in reflected[table]:
            if b["binding"] != binding: continue
            if b["array"] != 1:
                raise ValueError("Texture descriptor arrays need a runtime binding implementation")
            handle = b["handle"]
            offset, sampler_offset, handle_kind = (handle & 0x3fff)*4, ((handle>>14)&0x3fff)*4, handle>>28
            if handle_kind != 0 or b["separate"] or b["cbuf_slot"] not in (-1,2):
                raise ValueError("Non-combined texture/image binding needs adaptation")
            image_bank=0x120<=offset<0x168 and (offset-0x120)%8==0
            if kind==IMG:
                if not image_bank:raise ValueError("NVN image handle outside driver image bank")
                slot=(offset-0x120)//8
            elif image_bank:
                slot=(offset-0x120)//8;kind=IMAGE_QUERY
                if b["flags"]!=2:raise ValueError("Only query-only texture usage can alias the image bank")
                if not any(x["handle"]==handle and x["type"]==b["type"] for x in reflected["images"]):
                    raise ValueError("Image-query type lacks matching typed surface instructions")
            elif 0x20<=offset<0x120 and (offset-0x20)%8==0:slot=(offset-0x20)//8
            else:raise ValueError("Combined NVN handle offset outside verified slot range")
            sampler=-1
            pointer=types[str(resource["type_id"])]
            if pointer["opcode"]!=32:raise ValueError("Descriptor variable is not a SPIR-V pointer")
            target=types[str(pointer["operands"][1])]
            descriptor_type=3 if kind==IMG else 1 if target["opcode"]==27 else 2
            if kind==IMAGE_QUERY and (descriptor_type not in (1,2) or not resource.get("query_only")):
                raise ValueError("Only proved size/level/sample queries can use the image bank")
            if target["opcode"]==25 and target["operands"][1]==5:
                descriptor_type=5 if kind==IMG else 4;kind=TBUF
            return (resource["set"],binding,descriptor_type,kind,slot,sampler,1,b["type"])
    raise ValueError(f"No NVN resource mapping for descriptor {resource}")

def pack(metadata, c1, spirv_size):
    stage = STAGES[metadata["stage"]]
    descriptors = [descriptor(r,metadata["nvn_resources"],stage,metadata["layout"]["types"]) for r in metadata["layout"]["resources"]]
    attributes = []
    for var in metadata["layout"]["inputs"]:
        if "30" in var["decorations"]:
            attributes.append((var["decorations"]["30"][0],0))
    capabilities = metadata["layout"]["capabilities"]
    local = metadata["job_facts"].get("local",[1,1,1])
    header = struct.pack("<12I",0x4d4b5644,1,stage,len(descriptors),len(attributes),len(c1),len(capabilities),
                         *local,metadata["nvn_resources"]["frag_out_map"],spirv_size)
    return header + b"".join(struct.pack("<8i",*d) for d in descriptors) + \
        b"".join(struct.pack("<2I",*a) for a in attributes) + struct.pack(f"<{len(capabilities)}I",*capabilities) + c1


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--inventory",required=True,type=Path)
    ap.add_argument("--resources",required=True,type=Path)
    ap.add_argument("--spv",required=True,type=Path)
    ap.add_argument("--validator",required=True,type=Path)
    ap.add_argument("--out",required=True,type=Path)
    ap.add_argument("--key",action="append",default=[])
    args=ap.parse_args();out=args.out.resolve();repo=Path(__file__).resolve().parents[2]
    if out==repo or repo in out.parents or (out.exists() and any(out.iterdir())):
        raise ValueError("Use a fresh output directory outside the checkout")
    inv=json.loads((args.inventory/"inventory.json").read_text(encoding="utf8"))
    resources=json.loads(args.resources.read_text(encoding="utf8"))
    if inv["resources_sha256"]!=hashlib.sha256(args.resources.read_bytes()).hexdigest():
        raise ValueError("Resource inventory provenance mismatch")
    archives={r["path"]:r for r in resources["files"] if r["path"].endswith((".bfsha",".bnsh"))}
    rows={r["key"]:r for r in inv["records"]}
    jobs=json.loads((args.inventory/"jobs.json").read_text(encoding="utf8"))
    if args.key:jobs=[j for j in jobs if j["key"] in args.key]
    out.mkdir(parents=True);(out/"validated").mkdir()
    @lru_cache(maxsize=96)
    def read_archive(path,digest):
        data=Path(path).read_bytes()
        if hashlib.sha256(data).hexdigest()!=digest:raise ValueError("Shader archive provenance mismatch")
        return data
    def one(job):
        key=job["key"];row=rows[key];result={"key":key,"stage":job["stage"],"validated":False}
        try:
            reflection=(args.spv/(key+".res.json")).read_bytes();res=json.loads(reflection.decode("utf-8-sig"))
            if res["notes"]:raise ValueError("Missing GPU query facts: "+repr(res["notes"]))
            control=(args.inventory/(key+".control.bin")).read_bytes()
            if len(control)!=0x880 or hashlib.sha256(control).hexdigest()!=row["control_sha256"]:
                raise ValueError("Control provenance mismatch")
            source=archives[row["source"]];archive=read_archive(source["extracted"],source["sha256"])
            code_size=struct.unpack_from("<I",control,0x704)[0]
            code=archive[row["code_offset"]:row["code_offset"]+code_size]
            if hashlib.sha256(code).hexdigest()!=row["code_sha256"] or hashlib.sha256(control+code).hexdigest()[:16]!=key:
                raise ValueError("Full program identity mismatch")
            path=out/"validated"/(key+".spv");shutil.copyfile(args.spv/(key+".spv"),path)
            constants=bytes.fromhex(job["c1"])
            const_size,const_offset=struct.unpack_from("<II",control,0x6fc)
            if constants!=code[const_offset:const_offset+const_size]:raise ValueError("Immutable c1 provenance mismatch")
            offsets_repaired=fold_sample_offsets(path,constants,res["cbufs"])
            added=declare_unformatted_storage_image_access(path) + declare_distance_capabilities(path)
            check=subprocess.run([str(args.validator),"--target-env","vulkan1.1",str(path)],capture_output=True)
            if check.returncode:raise ValueError((check.stdout+check.stderr).decode("utf8","replace"))
            removed=trim_unused_capabilities(path,args.validator)
            layout=spirv_layout(path,job["stage"],True)
            if set(layout["capabilities"]) & {10,5378}:raise ValueError("Device feature needs adaptation")
            check=subprocess.run([str(args.validator),"--target-env","vulkan1.1",str(path)],capture_output=True)
            if check.returncode:raise ValueError((check.stdout+check.stderr).decode("utf8","replace"))
            meta={"key":key,"stage":job["stage"],"layout":json.loads(json.dumps(layout)),"nvn_resources":res,"job_facts":job}
            raw=path.read_bytes();payload=pack(meta,bytes.fromhex(job["c1"]),len(raw))
            (out/(key+".vkmeta")).write_bytes(payload)
            compressed=gzip.compress(raw,mtime=0);(out/(key+".spv.gz")).write_bytes(compressed)
            result.update(validated=True,spirv_sha256=hashlib.sha256(raw).hexdigest(),capabilities_added=added,unused_declarations_removed=removed,static_offsets_folded=offsets_repaired,
                          source_spirv_sha256=hashlib.sha256((args.spv/(key+".spv")).read_bytes()).hexdigest(),reflection_sha256=hashlib.sha256(reflection).hexdigest(),
                          metadata_sha256=hashlib.sha256(payload).hexdigest(),gzip_sha256=hashlib.sha256(compressed).hexdigest())
            return result,key.encode()+struct.pack("<II",STAGES[job["stage"]],code_size)+control+code
        except (ValueError,KeyError,OSError) as error:
            result["error"]=str(error);return result,None
    results=[];index=[]
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        for result,entry in pool.map(one,jobs):
            results.append(result)
            if entry:index.append(entry)
            if len(results)%250==0:print("checked",len(results),"exported",len(index),flush=True)
    payload=struct.pack("<III",0x494b5644,2,len(index))+b"".join(index)
    (out/"shader_index.bin").write_bytes(payload)
    report={"index_version":2,"identity":"control+full-original-NVN-code (byte comparison)","programs":len(jobs),"exported":len(index),
            "index_sha256":hashlib.sha256(payload).hexdigest(),"runtime_rendering_verified":False,
            "attribute_types_need_runtime_confirmation":True,"structural_validation_complete":len(index)==len(jobs),
            "distance_capability_policy":"preserve and declare ClipDistance/CullDistance from BuiltIn declarations",
            "tool_sha256":hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),"records":results}
    (out/"validation.json").write_text(json.dumps(report,indent=2)+"\n",encoding="utf8")
    print("exported",len(index),"blocked",len(jobs)-len(index),"index_bytes",len(payload))
PAYLOAD_FIELDS = ((".vkmeta", "metadata_sha256"), (".spv.gz", "gzip_sha256"))
HASH_FIELDS = ("metadata_sha256", "gzip_sha256", "spirv_sha256")


def fresh_output(path):
    out = Path(path).resolve()
    repo = Path(__file__).resolve().parents[2]
    if out == repo or repo in out.parents or (out.exists() and any(out.iterdir())):
        raise ValueError("Use a fresh output directory outside the checkout")
    return out


def read_validated_cache(root):
    """Verify a complete cache before accepting any entry or replacement."""
    root = Path(root).resolve()
    report_bytes = (root / "validation.json").read_bytes()
    report = json.loads(report_bytes)
    data = (root / "shader_index.bin").read_bytes()
    if (len(data) < 12 or struct.unpack_from("<II", data) != (0x494b5644, 2)
            or hashlib.sha256(data).hexdigest() != report["index_sha256"]):
        raise ValueError("Validated source index identity mismatch")
    if report.get("structural_validation_complete") is not True:
        raise ValueError("Merge/overlay requires a complete validated source cache")
    verified = {}
    for row in report["records"]:
        if row.get("validated") is not True or row["key"] in verified:
            raise ValueError("Duplicate or unvalidated source validation record")
        verified[row["key"]] = row
    count, = struct.unpack_from("<I", data, 8)
    if report["programs"] != count or report["exported"] != count or len(verified) != count:
        raise ValueError("Source validation/index count mismatch")
    entries, payloads, cursor = {}, {}, 12
    for _ in range(count):
        if cursor + 24 + 0x880 > len(data):
            raise ValueError("Truncated merge index")
        key = data[cursor:cursor + 16].decode("ascii")
        stage, size = struct.unpack_from("<II", data, cursor + 16)
        control = data[cursor + 24:cursor + 24 + 0x880]
        end = cursor + 24 + 0x880 + size
        code = data[cursor + 24 + 0x880:end]
        if (not size or end > len(data) or size != struct.unpack_from("<I", control, 0x704)[0]
                or stage != struct.unpack_from("<I", control, 0x714)[0]
                or hashlib.sha256(control + code).hexdigest()[:16] != key):
            raise ValueError("Merge program identity mismatch")
        if key in entries or key not in verified or STAGES.get(verified[key]["stage"]) != stage:
            raise ValueError("Duplicate, unvalidated or stage-mismatched merge program")
        for suffix, field in PAYLOAD_FIELDS:
            path = root / (key + suffix)
            if hashlib.sha256(path.read_bytes()).hexdigest() != verified[key][field]:
                raise ValueError("Merge payload hash mismatch")
            payloads[key + suffix] = path
        raw = gzip.decompress(payloads[key + ".spv.gz"].read_bytes())
        if hashlib.sha256(raw).hexdigest() != verified[key]["spirv_sha256"]:
            raise ValueError("Merge uncompressed SPIR-V hash mismatch")
        entries[key] = data[cursor:end]
        cursor = end
    if cursor != len(data):
        raise ValueError("Merge index trailing-data mismatch")
    source = {"directory": str(root), "validation_sha256": hashlib.sha256(report_bytes).hexdigest(),
              "index_sha256": report["index_sha256"], "programs": count}
    return entries, verified, payloads, source


def write_cache(out, entries, records, payloads, **provenance):
    out.mkdir(parents=True, exist_ok=True)
    for name, path in payloads.items():
        shutil.copyfile(path, out / name)
    index = struct.pack("<III", 0x494b5644, 2, len(entries)) + b"".join(
        entry for _, entry in sorted(entries.items()))
    (out / "shader_index.bin").write_bytes(index)
    result = {"index_version": 2, "identity": "control+full-original-NVN-code (byte comparison)",
              "programs": len(entries), "exported": len(entries),
              "index_sha256": hashlib.sha256(index).hexdigest(), "structural_validation_complete": True,
              "runtime_rendering_verified": False, "attribute_types_need_runtime_confirmation": True,
              "tool_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              **provenance, "records": [r for _, r in sorted(records.items())]}
    (out / "validation.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf8")
    return result


def merge_caches(inputs, output):
    """Duplicates are accepted only when the original identity and bytes match."""
    out = fresh_output(output)
    entries, records, payloads, sources, owners, duplicates = {}, {}, {}, [], {}, []
    for root in inputs:
        source_entries, source_records, source_payloads, source = read_validated_cache(root)
        for key, entry in source_entries.items():
            if key in entries:
                if entry != entries[key]:
                    raise ValueError(f"Conflicting duplicate index entry: {key}")
                for suffix, _ in PAYLOAD_FIELDS:
                    name = key + suffix
                    if payloads[name].read_bytes() != source_payloads[name].read_bytes():
                        raise ValueError(f"Conflicting duplicate payload: {name}; use an explicit audited overlay")
                duplicates.append({"key": key, "retained_source": owners[key],
                                   "identical_source": source["directory"],
                                   **{field: source_records[key][field] for field in HASH_FIELDS}})
                continue
            entries[key] = entry
            records[key] = source_records[key]
            owners[key] = source["directory"]
            for suffix, _ in PAYLOAD_FIELDS:
                payloads[key + suffix] = source_payloads[key + suffix]
        sources.append(source)
    return write_cache(out, entries, records, payloads, merged_sources=sources,
                       duplicate_policy="identical original index entry and exact metadata/gzip bytes only",
                       identical_duplicates=duplicates)


def overlay_cache(base, replacement, plan_path, output):
    """Replace translated payloads only with explicit expected hashes/evidence.

    The original NVN identity must be unchanged. The replacement cache must
    contain exactly the planned keys; merge never silently selects a variant.
    """
    out = fresh_output(output)
    plan_path = Path(plan_path).resolve()
    plan_bytes = plan_path.read_bytes()
    plan = json.loads(plan_bytes)
    if not isinstance(plan.get("reason"), str) or not plan["reason"].strip() or not plan.get("evidence"):
        raise ValueError("Overlay plan needs a reason and hashed evidence")
    evidence = []
    for item in plan["evidence"]:
        path = Path(item["path"])
        if not path.is_absolute():
            path = plan_path.parent / path
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest != item["sha256"]:
            raise ValueError("Overlay evidence hash mismatch")
        evidence.append({"path": str(path.resolve()), "sha256": digest})
    entries, records, payloads, source = read_validated_cache(base)
    replacement_entries, replacement_records, replacement_payloads, replacement_source = read_validated_cache(replacement)
    changes = {}
    for change in plan["replacements"]:
        key = change["key"]
        if key in changes:
            raise ValueError("Duplicate overlay plan key")
        changes[key] = change
    if set(changes) != set(replacement_entries) or not changes:
        raise ValueError("Replacement cache must contain exactly the planned keys")
    for key, change in changes.items():
        if key not in entries or entries[key] != replacement_entries[key]:
            raise ValueError("Overlay must preserve an existing original NVN identity")
        for field in HASH_FIELDS:
            if records[key][field] != change["before"][field] or replacement_records[key][field] != change["after"][field]:
                raise ValueError(f"Overlay planned before/after hash mismatch: {key} {field}")
        if all(change["before"][field] == change["after"][field] for field in HASH_FIELDS):
            raise ValueError("Overlay replacement does not change payload")
        records[key] = replacement_records[key]
        for suffix, _ in PAYLOAD_FIELDS:
            payloads[key + suffix] = replacement_payloads[key + suffix]
    return write_cache(out, entries, records, payloads, overlay_base=source,
                       overlay_replacement=replacement_source,
                       overlay_plan={"path": str(plan_path), "sha256": hashlib.sha256(plan_bytes).hexdigest(),
                                     "reason": plan["reason"], "evidence": evidence,
                                     "replacements": list(changes.values())})


def merge_main():
    ap = argparse.ArgumentParser(description="Merge validated full-code caches; duplicates must be byte-identical")
    ap.add_argument("--input", type=Path, action="append", required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args(sys.argv[2:])
    result = merge_caches(args.input, args.out)
    print("merged", result["exported"], "validated programs; identical duplicates", len(result["identical_duplicates"]))


def overlay_main():
    ap = argparse.ArgumentParser(description="Apply an explicit hashed-evidence payload overlay, preserving original identities")
    ap.add_argument("--base", type=Path, required=True)
    ap.add_argument("--replacement", type=Path, required=True)
    ap.add_argument("--plan", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args(sys.argv[2:])
    result = overlay_cache(args.base, args.replacement, args.plan, args.out)
    print("overlaid", len(result["overlay_plan"]["replacements"]), "of", result["exported"], "validated programs")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "merge":
        merge_main()
    elif len(sys.argv) > 1 and sys.argv[1] == "overlay":
        overlay_main()
    else:
        main()
