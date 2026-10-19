"""Package the independent Echoes NativeActivity diagnostic (no game assets)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
from zipfile import ZipFile, ZIP_STORED


def elf_info(data, label):
    """Check the target ABI and record each library's own ELF page alignment."""
    if (len(data) < 64 or data[:7] != b"\x7fELF\x02\x01\x01" or
            struct.unpack_from("<HHI", data, 16) != (3, 183, 1)):
        raise ValueError(f"{label}: expected little-endian AArch64 ELF64 shared library")
    offset = struct.unpack_from("<Q", data, 32)[0]
    header_size, stride, count = struct.unpack_from("<HHH", data, 52)
    if header_size != 64 or stride < 56 or not count or offset < 64 or offset+count*stride > len(data):
        raise ValueError(f"{label}: invalid ELF64 program header table")
    loads = [struct.unpack_from("<IIQQQQQQ", data, offset+i*stride) for i in range(count)]
    loads = [item for item in loads if item[0] == 1]
    if not loads or any(item[2]+item[5] > len(data) or item[5] > item[6] for item in loads):
        raise ValueError(f"{label}: invalid ELF64 PT_LOAD segments")
    return {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
            "elf_class": 64, "elf_machine": 183, "elf_endianness": "little",
            "pt_load_min_alignment": min(item[7] for item in loads),
            "elf_pt_load_16k_compatible": all(item[7] >= 16384 and (item[3]-item[2]) % 16384 == 0 for item in loads)}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--native", type=Path, required=True)
    ap.add_argument("--sdk", type=Path, required=True)
    ap.add_argument("--java", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--validation-layer",type=Path,help="Optional external arm64 Khronos layer, for diagnostic APKs only")
    ap.add_argument("--turnip-driver", type=Path, help="Optional external AArch64 Turnip driver; requires --adrenotools. Bundling does not enable it.")
    ap.add_argument("--adrenotools", type=Path, help="Optional external AArch64 adrenotools library; requires --turnip-driver")
    args = ap.parse_args()
    if bool(args.turnip_driver) != bool(args.adrenotools):
        ap.error("--turnip-driver and --adrenotools must be supplied together")
    repo = Path(__file__).resolve().parents[2]
    out = args.out.resolve()
    if repo == out or repo in out.parents or (out.exists() and any(out.iterdir())):
        raise ValueError("Use a fresh output directory outside the checkout")
    data = args.native.read_bytes()
    native_info = elf_info(data, "Native library")
    if not native_info["elf_pt_load_16k_compatible"]:
        raise ValueError("Native library must support 16 KiB pages")
    # Validate all external inputs before creating output or invoking SDK tools.
    private_libraries = []
    for source, name in ((args.turnip_driver, "libd2r_turnip.so"), (args.adrenotools, "libadrenotools.so")):
        if source:
            payload = source.read_bytes()
            library = {"source": str(source.resolve()), "apk_path": f"lib/arm64-v8a/{name}",
                       **elf_info(payload, name)}
            private_libraries.append((library, payload))
    layer = None
    if args.validation_layer:
        layer = args.validation_layer.read_bytes()
        elf_info(layer, "Vulkan validation layer")
    out.mkdir(parents=True)
    env = dict(os.environ, JAVA_HOME=str(args.java))
    env["PATH"] = str(args.java/"bin") + os.pathsep + env["PATH"]
    bt = args.sdk/"build-tools/35.0.0"
    manifest = repo/"dm/android/titles/echoes-manifest.xml"
    def run(*command):
        result = subprocess.run([str(x) for x in command], capture_output=True, env=env)
        if result.returncode:
            (out/"failure.log").write_bytes(result.stdout+result.stderr)
            raise RuntimeError(f"Packaging failed: {command[0]}; see failure.log")
    run(bt/"aapt2.exe", "link", "-o", out/"base.apk", "--manifest", manifest,
        "-I", args.sdk/"platforms/android-34/android.jar")
    with ZipFile(out/"base.apk", "a") as archive:
        archive.writestr("lib/arm64-v8a/libechoes_android.so", data, compress_type=ZIP_STORED)
        for library, payload in private_libraries:
            archive.writestr(library["apk_path"], payload, compress_type=ZIP_STORED)
        if layer is not None:
            archive.writestr("lib/arm64-v8a/libVkLayer_khronos_validation.so",layer,compress_type=ZIP_STORED)
    run(bt/"zipalign.exe", "-f", "-P", "16", "4", out/"base.apk", out/"aligned.apk")
    apk = out/"echoes-bringup.apk"
    run(bt/"apksigner.bat", "sign", "--ks", Path.home()/".android/debug.keystore",
        "--ks-pass", "pass:android", "--key-pass", "pass:android", "--out", apk, out/"aligned.apk")
    run(bt/"apksigner.bat", "verify", apk)
    run(bt/"zipalign.exe", "-c", "-P", "16", "4", apk)
    report = {"package": "org.switchports.echoesofwisdom", "game_assets_included": False,
              "validation_layer_sha256":None if not args.validation_layer else hashlib.sha256(layer).hexdigest(),
              "native_elf": native_info,
              "private_vulkan": {"bundled": bool(private_libraries), "default_enabled": False,
                                 "libraries": [library for library, _ in private_libraries],
                                 "bundle_elf_pt_load_16k_compatible": None if not private_libraries else all(library["elf_pt_load_16k_compatible"] for library, _ in private_libraries),
                                 "opt_in_markers": ["files/private-vulkan", "files/private-vulkan-sysmem"],
                                 "sysmem_marker_implies_private_vulkan": True,
                                 "adreno_echoes_device_verified": False,
                                 "scope": "Optional Adreno experiment; keep disabled on Mali. ELF checks do not establish device compatibility."},
              "apk": str(apk), "apk_sha256": hashlib.sha256(apk.read_bytes()).hexdigest(),
              "native_sha256": hashlib.sha256(data).hexdigest(), "runtime_rendering_verified": False}
    (out/"apk-build.json").write_text(json.dumps(report, indent=2)+"\n")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
