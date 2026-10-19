#!/usr/bin/env python3
"""Package a complete D2R Android APK from prepared, hash-checked inputs.

This does not generate game modules, RomFS, overlays or shaders. It requires
the reviewed D2R native atlas/XP build and HUD/world asset manifests, compiles
every Android Java source, then aligns and debug-signs the APK.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import struct
import subprocess
from xml.sax.saxutils import escape
from zipfile import ZipFile, ZIP_STORED

from android_title_apk import elf_info

HUD_MANIFEST = "d2r-hud-assets-manifest.json"
WORLD_MANIFEST = "d2r-world-assets-manifest.json"
MODULE_MANIFEST = "module-manifest.json"
TITLE_ID = "0100726014352000"
BUILD_ID = "db0507f36b84200ac05f75d76ed5026f"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def read_json(path):
    value = json.loads(path.read_text(encoding="utf-8-sig"), object_pairs_hook=unique_object)
    if not isinstance(value, dict):
        raise ValueError(f"{path}: expected JSON object")
    return value


def relative_path(value):
    if not isinstance(value, str) or not value or "\\" in value:
        raise ValueError(f"Invalid manifest path: {value!r}")
    path = PurePosixPath(value)
    if (path.is_absolute() or path.as_posix() != value or
            any(part in ("", ".", "..") or ":" in part or
                part.endswith((".", " ")) or any(ord(c) < 32 for c in part)
                for part in value.split("/"))):
        raise ValueError(f"Unsafe manifest path: {value!r}")
    return path


def checked_file(root, name):
    relative = relative_path(name)
    source = root.joinpath(*relative.parts)
    # Also reject a symlink or Windows junction in an ancestor: an input may
    # otherwise escape its declared root despite a safe manifest spelling.
    cursor = root
    for part in relative.parts:
        cursor = cursor / part
        if cursor.is_symlink() or (hasattr(cursor, "is_junction") and cursor.is_junction()):
            raise ValueError(f"Linked manifest input: {source}")
    if not source.is_file() or not source.resolve().is_relative_to(root.resolve()):
        raise ValueError(f"Missing or escaped manifest input: {source}")
    return source


def payloads(root, manifest, path_key, seen=None, allow_empty=False):
    files = manifest.get("files")
    if not isinstance(files, list) or not files:
        raise ValueError("Manifest requires a nonempty files array")
    seen = set() if seen is None else seen
    result = {}
    for entry in files:
        if not isinstance(entry, dict):
            raise ValueError("Manifest entry must be an object")
        name = entry.get(path_key)
        relative_path(name)
        folded = name.casefold()
        if folded in seen:
            raise ValueError(f"Duplicate manifest payload: {name}")
        seen.add(folded)
        size, digest = entry.get("bytes"), entry.get("sha256")
        if type(size) is not int or size < (0 if allow_empty else 1) or not isinstance(digest, str) or not re.fullmatch("[0-9a-f]{64}", digest):
            raise ValueError(f"Invalid size/SHA256: {name}")
        data = checked_file(root, name).read_bytes()
        if len(data) != size or sha(data) != digest:
            raise ValueError(f"Size/SHA256 mismatch: {name}")
        if name.endswith(".png"):
            if len(data) < 33 or data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
                raise ValueError(f"Invalid PNG header: {name}")
            dimensions = list(struct.unpack_from(">II", data, 16))
            if min(dimensions) <= 0 or ("dimensions" in entry and entry["dimensions"] != dimensions):
                raise ValueError(f"PNG dimensions mismatch: {name}")
        result[name] = data
    return result


class ElfSymbols:
    """Read defined dynamic ELF64 symbols as data; never execute the library."""
    def __init__(self, data):
        self.data = data
        self.info = elf_info(data, "D2R native library")
        if not self.info["elf_pt_load_16k_compatible"]:
            raise ValueError("D2R native library must support 16 KiB pages")
        offset = struct.unpack_from("<Q", data, 40)[0]
        stride, count = struct.unpack_from("<HH", data, 58)
        if stride < 64 or not count or offset < 64 or offset + stride * count > len(data):
            raise ValueError("Missing/invalid ELF section table for configuration identity")
        sections = [struct.unpack_from("<IIQQQQIIQQ", data, offset + i * stride) for i in range(count)]
        self.symbols = {}
        for section in sections:
            if section[1] != 11:
                continue
            start, size, linked, entry_size = section[4], section[5], section[6], section[9]
            if linked >= count or entry_size < 24 or size % entry_size or start + size > len(data):
                raise ValueError("Invalid ELF dynamic symbol table")
            strings = sections[linked]
            if strings[1] != 3 or strings[4] + strings[5] > len(data):
                raise ValueError("Invalid ELF dynamic string table")
            names = data[strings[4]:strings[4] + strings[5]]
            for at in range(start, start + size, entry_size):
                name_at, info, other, index, address, length = struct.unpack_from("<IBBHQQ", data, at)
                if not index or info >> 4 not in (1, 2) or other & 3 not in (0, 3):
                    continue
                if name_at >= len(names) or names.find(b"\0", name_at) < 0:
                    raise ValueError("Invalid ELF symbol name")
                name = names[name_at:names.index(b"\0", name_at)].decode("utf-8")
                if name in self.symbols:
                    raise ValueError(f"Duplicate ELF symbol: {name}")
                if index >= count:
                    continue
                target = sections[index]
                file_at = target[4] + address - target[3]
                if address < target[3] or file_at + length > target[4] + target[5] or file_at + length > len(data):
                    continue
                self.symbols[name] = (file_at, length, target[2])

    def datum(self, name, length=None):
        if name not in self.symbols:
            raise ValueError(f"Full D2R build requires ELF symbol: {name}")
        at, size, flags = self.symbols[name]
        if length is not None and size != length:
            raise ValueError(f"Unexpected ELF identity size: {name}")
        return self.data[at:at + size]

    def fingerprint(self, name):
        value = self.datum(name, 65)
        if value[-1:] != b"\0" or not re.fullmatch(b"[0-9a-f]{64}", value[:-1]):
            raise ValueError(f"Invalid ELF fingerprint: {name}")
        return value[:-1].decode("ascii")


def validate_inputs(native, images, assets):
    native_data = native.read_bytes()
    elf = ElfSymbols(native_data)
    if struct.unpack("<I", elf.datum("dm_android_hud_exp_table_entries", 4))[0] != 100:
        raise ValueError("Full D2R build requires the generated 100-entry HUD XP table")
    for name in ("dm_ta_current_set", "dm_android_world_overlay_frame",
                 "Java_org_d2r_binaryprobe_TouchControlsView_nativeHudFrame",
                 "Java_org_d2r_binaryprobe_TouchControlsView_nativeWorldFrame"):
        elf.datum(name)
    module_file = checked_file(images, MODULE_MANIFEST)
    module_manifest = read_json(module_file)
    if module_manifest.get("title_id") != TITLE_ID or module_manifest.get("build_id") != BUILD_ID:
        raise ValueError("Expected reviewed D2R 3.3.94012 title/build profile")
    modules = payloads(images, module_manifest, "name", allow_empty=True)
    required_modules = {f"{module}.{suffix}" for module in ("main", "sdk", "subsdk0")
                        for suffix in ("patched.bin", "fixups.tsv", "gates.tsv")}
    if set(modules) != required_modules:
        raise ValueError("D2R module manifest must contain exactly the 9 module/fixup/gate payloads")
    if any(not data for name, data in modules.items() if name.endswith(".patched.bin")):
        raise ValueError("D2R patched module binaries must be nonempty")
    romfs = module_manifest.get("romfs", {})
    if (type(romfs.get("bytes")) is not int or romfs["bytes"] <= 0 or
            not re.fullmatch("[0-9a-f]{64}", str(romfs.get("sha256", "")))):
        raise ValueError("Module manifest requires the external RomFS size/SHA256")
    seen = {HUD_MANIFEST.casefold(), WORLD_MANIFEST.casefold()}
    all_assets, manifests = {}, {}
    for name in (HUD_MANIFEST, WORLD_MANIFEST):
        path = checked_file(assets, name)
        manifest = read_json(path)
        if manifest.get("passed") is not True:
            raise ValueError(f"Asset manifest is not verified: {name}")
        verified = payloads(assets, manifest, "path", seen)
        prefix = "d2r_hud/" if name == HUD_MANIFEST else "d2r_world/"
        if any(not path.startswith(prefix) for path in verified):
            raise ValueError(f"Asset payload must stay in {prefix}: {name}")
        all_assets.update(verified)
        manifests[name] = manifest
        all_assets[name] = path.read_bytes()
    actual = {p.relative_to(assets).as_posix() for p in assets.rglob("*") if p.is_file()}
    if actual != set(all_assets):
        raise ValueError(f"Unlisted/missing HUD/world files: {sorted(actual ^ set(all_assets))}")
    hud = manifests[HUD_MANIFEST]
    provenance = hud.get("native_art")
    if not isinstance(provenance, dict):
        raise ValueError("HUD manifest requires native_art manifest/geometry/binding provenance")
    native_identity = {}
    for key in ("manifest", "geometry", "binding"):
        path = provenance.get(key)
        relative_path(path)
        if path not in all_assets:
            raise ValueError(f"Missing hashed native-art provenance: {path}")
        digest = elf.fingerprint(f"dm_ta_{key}_sha")
        if sha(all_assets[path]) != digest:
            raise ValueError(f"Native ELF / packaged {key} fingerprint mismatch")
        native_identity[key + "_sha256"] = digest
    official = json.loads(all_assets[provenance["manifest"]], object_pairs_hook=unique_object)
    atlases = elf.datum("dm_ta_atlases", 9 * 32)
    official_assets = official.get("assets", [])
    by_name = {entry["asset"]: entry for entry in official_assets}
    if len(by_name) != len(official_assets):
        raise ValueError("Duplicate official atlas provenance")
    for i in range(9):
        name = f"dm_native_skill_{i}.png"
        item = by_name.get(name)
        path = "d2r_hud/" + name
        if not item or path not in all_assets or sha(all_assets[path]) != item.get("derived_png_sha256"):
            raise ValueError(f"Native atlas payload/provenance mismatch: {name}")
        dimensions = item["atlas_px"] + [item["frame_width"], item["frame_pitch"], item["frame_count"]]
        if list(struct.unpack_from("<5i", atlases, i * 32 + 8)) != dimensions:
            raise ValueError(f"Native atlas geometry/provenance mismatch: {name}")
        if list(struct.unpack_from(">II", all_assets[path], 16)) != item["atlas_px"]:
            raise ValueError(f"Native atlas PNG dimensions mismatch: {name}")
    required_world = {f"d2r_world/mapgoal/label_{i}{suffix}.png" for i in range(31) for suffix in ("", "_en")}
    required_world.update(f"d2r_world/mapgoal/sym_{i}.png" for i in range(7))
    required_world.update(("d2r_world/mapgoal/cross.png", "d2r_world/mapgoal/mapgoal.json",
                           "d2r_world/BlizzardGlobal-V5_81.ttf"))
    if not required_world.issubset(all_assets):
        raise ValueError("Full D2R build requires all world labels, symbols and original font")
    return native_data, modules, all_assets, {
        "native_elf": elf.info, "native_configuration": {"hud_xp_entries": 100, "atlas_count": 9, **native_identity},
        "module_manifest": module_manifest, "hud_asset_payloads": len(all_assets) - 2,
        "input_manifests": {"modules": sha(module_file.read_bytes()),
                            **{name: sha(all_assets[name]) for name in manifests}},
    }


def command_runner(out, env):
    commands = []
    def run(*argv):
        result = subprocess.run([str(a) for a in argv], capture_output=True, env=env)
        commands.append({"argv": [str(a) for a in argv], "exit_code": result.returncode,
                         "output": (result.stdout + result.stderr).decode("utf-8", errors="replace")})
        (out / "commands.json").write_text(json.dumps(commands, indent=2) + "\n", encoding="utf-8")
        if result.returncode:
            raise RuntimeError(f"Packaging failed: {argv[0]}; see {out / 'commands.json'}")
    return run


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--sdk", type=Path, default=os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME"))
    parser.add_argument("--java", type=Path, default=os.environ.get("JAVA_HOME"))
    parser.add_argument("--native", "--so", type=Path, required=True)
    parser.add_argument("--images", type=Path, required=True)
    parser.add_argument("--hud-assets", type=Path, required=True)
    parser.add_argument("--out", "--output", type=Path, required=True)
    parser.add_argument("--package", default="org.d2r.newprobe")
    parser.add_argument("--build-tools", default="35.0.0")
    parser.add_argument("--android-platform", default="android-35")
    parser.add_argument("--keystore", type=Path, default=Path.home() / ".android/debug.keystore",
                        help="Android debug keystore (password android); release signing is a separate step")
    parser.add_argument("--validate-only", action="store_true", help="Check prepared inputs without creating output or invoking SDK tools")
    args = parser.parse_args(argv)
    repo, out = args.repo.resolve(), args.out.resolve()
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+", args.package):
        parser.error("--package must be an Android package name")
    native, images, assets = args.native.resolve(), args.images.resolve(), args.hud_assets.resolve()
    if (out == repo or repo in out.parents or any(out == p or out in p.parents or p in out.parents for p in (native, images, assets)) or
            (out.exists() and (not out.is_dir() or any(out.iterdir())))):
        parser.error("Use a fresh output directory outside the checkout and all input directories")
    native_data, modules, assets_data, report = validate_inputs(native, images, assets)
    sources = sorted((repo / "dm/android/src").rglob("*.java"))
    if not sources:
        raise ValueError("No Android Java sources in --repo")
    report.update({"package": args.package, "native": str(native), "images": str(images), "hud_assets": str(assets),
                   "native_sha256": sha(native_data),
                   "java_sources": {p.relative_to(repo).as_posix(): sha(p.read_bytes()) for p in sources},
                   "assets": {name: {"bytes": len(data), "sha256": sha(data)} for name, data in assets_data.items()},
                   "original_game_or_canvas_verified": False, "external_romfs_verified_by_packager": False})
    if args.validate_only:
        print(json.dumps({"validated": True, **report}, indent=2))
        return 0
    if args.sdk is None or args.java is None:
        parser.error("Specify --sdk/--java or ANDROID_SDK_ROOT/JAVA_HOME")
    sdk, java = args.sdk.resolve(), args.java.resolve()
    bt = sdk / "build-tools" / args.build_tools
    android = sdk / "platforms" / args.android_platform / "android.jar"
    windows = os.name == "nt"
    tool = lambda base: bt / (base + (".bat" if base in ("d8", "apksigner") else ".exe") if windows else base)
    javac = java / "bin" / ("javac.exe" if windows else "javac")
    for required in (javac, android, args.keystore, *(tool(name) for name in ("d8", "aapt2", "zipalign", "apksigner"))):
        if not required.is_file():
            raise ValueError(f"Missing build tool/input: {required}")
    out.mkdir(parents=True, exist_ok=True)
    for name in ("classes", "dex", "assets/modules"):
        (out / name).mkdir(parents=True, exist_ok=True)
    # Copy only the validated bytes. Changes to external inputs after checking
    # cannot silently alter the package built from this snapshot.
    for name, data in assets_data.items():
        destination = out / "assets" / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
    for name, data in modules.items():
        destination = out / "assets/modules" / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
    module_manifest_data = (json.dumps(report["module_manifest"], indent=2) + "\n").encode("utf-8")
    (out / "assets" / MODULE_MANIFEST).write_bytes(module_manifest_data)
    manifest = out / "AndroidManifest.xml"
    manifest.write_text(f'''<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="{escape(args.package)}" android:versionCode="1" android:versionName="94012-parity-test">
 <uses-sdk android:minSdkVersion="29" android:targetSdkVersion="33" />
 <uses-feature android:glEsVersion="0x00030000" android:required="false" />
 <uses-permission android:name="android.permission.VIBRATE" />
 <application android:label="D2R 94012 Test" android:debuggable="true" android:allowBackup="false" android:extractNativeLibs="true" android:theme="@android:style/Theme.Material.NoActionBar.Fullscreen">
  <activity android:name="org.d2r.binaryprobe.GameActivity" android:exported="true" android:screenOrientation="sensorLandscape" android:configChanges="orientation|screenSize|keyboardHidden">
   <intent-filter><action android:name="android.intent.action.MAIN"/><category android:name="android.intent.category.LAUNCHER"/></intent-filter>
  </activity>
 </application>
</manifest>
''', encoding="utf-8")
    env = dict(os.environ, JAVA_HOME=str(java))
    env["PATH"] = str(java / "bin") + os.pathsep + env.get("PATH", "")
    run = command_runner(out, env)
    run(javac, "--release", "8", "-encoding", "UTF-8", "-cp", android, "-d", out / "classes", *sources)
    classes = sorted((out / "classes").rglob("*.class"))
    if not classes:
        raise ValueError("Java compilation emitted no classes")
    run(tool("d8"), "--min-api", "29", "--lib", android, "--output", out / "dex", *classes)
    if not (out / "dex/classes.dex").is_file():
        raise ValueError("D8 emitted no classes.dex")
    run(tool("aapt2"), "link", "-o", out / "base.apk", "--manifest", manifest, "-I", android, "-A", out / "assets")
    with ZipFile(out / "base.apk", "a") as archive:
        for dex in sorted((out / "dex").glob("classes*.dex")):
            archive.write(dex, dex.name, compress_type=ZIP_STORED)
        archive.writestr("lib/arm64-v8a/libd2r_mobile_android.so", native_data, compress_type=ZIP_STORED)
    run(tool("zipalign"), "-f", "-P", "16", "4", out / "base.apk", out / "aligned.apk")
    apk = out / "d2r-94012-test.apk"
    run(tool("apksigner"), "sign", "--ks", args.keystore, "--ks-pass", "pass:android", "--key-pass", "pass:android", "--out", apk, out / "aligned.apk")
    run(tool("apksigner"), "verify", "--verbose", apk)
    run(tool("zipalign"), "-c", "-P", "16", "4", apk)
    with ZipFile(apk) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)):
            raise ValueError("Duplicate final APK entries")
        expected = {"assets/" + name: data for name, data in assets_data.items()}
        expected.update({"assets/modules/" + name: data for name, data in modules.items()})
        expected["assets/" + MODULE_MANIFEST] = module_manifest_data
        expected["lib/arm64-v8a/libd2r_mobile_android.so"] = native_data
        for name, data in expected.items():
            if archive.read(name) != data:
                raise ValueError(f"Final APK payload mismatch: {name}")
    report.update({"apk": str(apk), "apk_sha256": sha(apk.read_bytes()), "final_payloads_verified": len(expected),
                   "sdk": str(sdk), "java": str(java), "build_tools": args.build_tools, "android_platform": args.android_platform,
                   "debug_signed": True})
    (out / "build-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"apk": str(apk), "apk_sha256": report["apk_sha256"], "native_sha256": sha(native_data), "verified_payloads": len(expected)}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
