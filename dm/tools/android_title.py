"""Prepare a pinned Android title without adding game content to the checkout.

Static audit / rewriting is not proof of loading, rendering, or playability.
"""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

from nso_inventory import Nso

REPO = Path(__file__).resolve().parents[2]
DEFAULT_PROFILE = REPO / "dm/android/titles/echoes-of-wisdom.json"
STARTUP = (
    "_init_libc0", "nnosInitialize", "_init_libc1",
    "nninitInitializeSdkModule", "nninitStartup", "_init_libc2",
    "_ZN2nn2os11SetHostArgcEi", "_ZN2nn2os11SetHostArgvEPPc", "nnMain",
)


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def external_output(path, source):
    path = path.resolve()
    if path == REPO or REPO in path.parents:
        raise ValueError("Generated game files must stay outside the Git checkout")
    if path == source.resolve() or source.resolve() in path.parents or path in source.resolve().parents:
        raise ValueError("Output and input directories must be separate")
    return path


def npdm_info(path):
    data = path.read_bytes()
    if len(data) < 0x80 or data[:4] != b"META":
        raise ValueError("Invalid NPDM header")
    offset, size = struct.unpack_from("<II", data, 0x70)
    if size < 0x40 or offset + size > len(data) or data[offset:offset + 4] != b"ACI0":
        raise ValueError("Invalid NPDM ACI0")
    if not data[0xc] & 1:
        raise ValueError("The Android native host requires an AArch64 program")
    return {"program_id": f"{struct.unpack_from('<Q', data, offset + 0x10)[0]:016x}",
            "is_64_bit": True, "main_thread_priority": data[0xe],
            "main_thread_stack_bytes": struct.unpack_from("<I", data, 0x1c)[0]}


def verified_inputs(profile, version, exefs):
    if profile["schema"] != 1 or profile["platform"] != "android-arm64":
        raise ValueError("Unsupported Android title profile")
    pinned = profile["versions"][str(version)]
    names = profile["module_order"]
    if len(set(names)) != len(names) or set(names) != set(pinned["modules"]):
        raise ValueError("Profile module set is inconsistent")
    if any(not re.fullmatch(r"main|sdk|subsdk[0-9]", name) for name in names):
        raise ValueError("Invalid profile module name")
    npdm = exefs / "main.npdm"
    if sha(npdm) != pinned["npdm_sha256"]:
        raise ValueError("NPDM hash does not match the selected content version")
    metadata = npdm_info(npdm)
    if metadata["program_id"] != profile["program_id"]:
        raise ValueError("NPDM program ID does not match the title")
    actual = set()
    for path in exefs.iterdir():
        if path.is_file():
            with path.open("rb") as stream:
                if stream.read(4) == b"NSO0":
                    actual.add(path.name)
    if actual - {"rtld"} != set(names):
        raise ValueError("ExeFS contains a different module set")
    modules = []
    for name in names:
        expected = pinned["modules"][name]
        if sha(exefs / name) != expected["sha256"]:
            raise ValueError(f"{name}: source hash does not match the selected version")
        module = Nso(exefs / name)
        if module.data[0x40:0x60].hex() != expected["build_id"]:
            raise ValueError(f"{name}: source hash/build ID does not match the selected version")
        modules.append(module.inventory())
    return metadata, modules


def audit(profile, version, exefs):
    metadata, modules = verified_inputs(profile, version, exefs)
    providers = {}
    for module in modules:
        for symbol in module["exports"]:
            providers.setdefault(symbol["name"], []).append({"module": module["module"], "address": hex(symbol["value"])})
    unresolved = [{"module": module["module"], "name": s["name"], "weak": s["binding"] == 2}
                  for module in modules for s in module["imports"] if s["name"] not in providers]
    startup = {name: providers.get(name, []) for name in STARTUP}
    names = set((REPO / "dm/gfx/nvn/names.txt").read_text().splitlines())
    main = next(m for m in modules if m["module"] == "main")
    requested = set(main["nvn_symbol_like_strings"])
    return {"schema": 1, "title": profile["id"], "program_id": profile["program_id"],
            "content_version": version, "display_version": profile["versions"][str(version)]["display_version"],
            "npdm": metadata, "modules": modules, "startup_symbols": startup,
            "startup_symbols_all_defined": all(startup.values()), "unresolved_guest_imports": unresolved,
            "nvn": {"main_name_strings": len(requested), "host_registered_names": len(names),
                    "names_outside_host_registry": sorted(requested - names),
                    "limit": "String presence/registry membership does not establish calls or implemented semantics."},
            "rewritten": False, "device_load_verified": False, "full_game_verified": False,
            "limits": ["SDK object layouts, HLE service semantics and execution remain unverified.",
                       "The game requires its own verified Vulkan shader cache and RomFS.",
                       "rtld is inventoried separately; its role is provided by the host loader.",
                       "D2R fixed-address hooks, touch rules and timing overrides must not be reused."]}


def rewrite(exefs, output, report):
    if not report["startup_symbols_all_defined"] or any(not symbol["weak"] for symbol in report["unresolved_guest_imports"]):
        raise ValueError("Resolve the static startup/import audit before rewriting")
    supported = {"ABS64", "GLOB_DAT", "JUMP_SLOT", "RELATIVE", "RELR"}
    for module in report["modules"]:
        if set(module["relocations"]["counts"]) - supported:
            raise ValueError(f"{module['module']}: relocation format is not supported by the native loader")
    env = dict(os.environ, DM_TARGET="android", DM_RULESET="native", DM_PRIVATE_GATES="0", PYTHONUTF8="1")
    rewriter = REPO / "dm/tools/patch_nso.py"
    products = {}
    for module in report["modules"]:
        name = module["module"]
        # Each run gets a fresh directory. A failed rewriter cannot publish a
        # partially written or stale image as a verified product.
        with tempfile.TemporaryDirectory(prefix=f"{name}-", dir=output) as staging:
            result = subprocess.run([sys.executable, str(rewriter), str(exefs / name), staging, name],
                                    env=env, capture_output=True, text=True, encoding="utf-8", errors="replace")
            (output / f"{name}.rewrite.log").write_text(result.stdout + result.stderr, encoding="utf-8")
            if result.returncode:
                raise RuntimeError(f"{name}: native rewriting failed; see its local rewrite log")
            for table in ("cuts", "obs", "params"):
                lines = (Path(staging) / f"{name}.{table}.tsv").read_text().splitlines()
                if any(line.strip() and not line.startswith("#") for line in lines):
                    raise ValueError(f"{name}: unexpected game-specific {table} rules")
            for path in sorted(Path(staging).glob(f"{name}.*")):
                products[path.name] = {"sha256": sha(path), "bytes": path.stat().st_size}
                path.replace(output / path.name)
        print(f"Rewritten {name}", flush=True)
    report.update(rewritten=True, rewrite_target="android", rewrite_ruleset="native",
                  rewriter_sha256=sha(rewriter), products=products)


def extract(nsp_path, keys, output, profile, version):
    # NSZ handles ticket/key interpretation. Never print its diagnostic objects
    # or pass title/content keys in command-line arguments.
    try:
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            from nsz.nut import Keys
            from nsz.Fs.Nsp import Nsp
            from nsz.Fs import Type
            Keys.load_default(keys)
            nsp = Nsp()
            try:
                nsp.open(str(nsp_path), "rb")
                programs = [f for f in nsp if getattr(getattr(f, "header", None), "contentType", None) == Type.Content.PROGRAM]
                if len(programs) != 1:
                    raise ValueError("Expected one Program NCA")
                for file in programs[0][0]:
                    name = file._path
                    if not re.fullmatch(r"main(?:\.npdm)?|sdk|subsdk[0-9]|rtld", name):
                        raise ValueError("Unexpected ExeFS entry")
                    file.seek(0)
                    with (output / name).open("wb") as dest:
                        remaining = file.size
                        while remaining:
                            block = file.read(min(1 << 20, remaining))
                            if not block:
                                raise ValueError("Truncated ExeFS file")
                            dest.write(block)
                            remaining -= len(block)
            finally:
                nsp.close()
    except Exception:
        raise RuntimeError("NSP extraction failed; check the local keys, container, and NSZ installation") from None
    verified_inputs(profile, version, output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("extract", "audit", "prepare"))
    parser.add_argument("--profile", type=Path, default=DEFAULT_PROFILE)
    parser.add_argument("--version", type=int, default=196608)
    parser.add_argument("--exefs", type=Path)
    parser.add_argument("--nsp", type=Path)
    parser.add_argument("--keys", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    profile = json.loads(args.profile.read_text(encoding="utf-8"))
    if args.action == "extract":
        if not args.nsp or not args.keys:
            parser.error("extract requires --nsp and --keys")
        source = args.nsp.resolve()
    else:
        if not args.exefs:
            parser.error("audit/prepare requires --exefs")
        source = args.exefs.resolve()
    output = external_output(args.out, source)
    if output.exists() and any(output.iterdir()):
        raise ValueError("Use a fresh output directory to avoid mixing content versions")
    output.mkdir(parents=True, exist_ok=True)
    if args.action == "extract":
        extract(args.nsp.resolve(), args.keys.resolve(), output, profile, args.version)
        print(f"Extracted and verified {profile['id']} content version {args.version}")
        return
    report = audit(profile, args.version, source)
    report["profile_sha256"] = sha(args.profile)
    report_path = output / "title-audit.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if args.action == "prepare":
        rewrite(source, output, report)
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"report": str(report_path), "modules": len(report["modules"]),
                      "unresolved_guest_imports": len(report["unresolved_guest_imports"]),
                      "rewritten": report["rewritten"], "full_game_verified": False}))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, RuntimeError, KeyError, OSError) as error:
        print(f"Android title preparation failed: {error}", file=sys.stderr)
        sys.exit(1)
