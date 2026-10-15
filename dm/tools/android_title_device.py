"""Run isolated, hash-checked Android title startup probes through ADB.

Does not install an APK, control gameplay, or read screenshots.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

from android_title import DEFAULT_PROFILE, external_output, sha

MODES = ("load", "os-init", "os-services", "sdk-startup")
SUFFIXES = ("patched.bin", "fixups.tsv", "gates.tsv", "sites.tsv", "cuts.tsv", "obs.tsv", "params.tsv")


def prepared_files(prepared, profile):
    manifest = json.loads((prepared / "title-audit.json").read_text(encoding="utf-8"))
    if (manifest.get("title") != profile["id"] or manifest.get("content_version") != 196608 or
            manifest.get("rewritten") is not True or manifest.get("full_game_verified") is not False):
        raise ValueError("Expected a prepared Echoes of Wisdom 1.1.0 manifest")
    names = profile["module_order"]
    if [m["module"] for m in manifest["modules"]] != names:
        raise ValueError("Prepared module order does not match the title profile")
    pinned = profile["versions"]["196608"]["modules"]
    for module in manifest["modules"]:
        if (module["source_sha256"] != pinned[module["module"]]["sha256"] or
                module["build_id"] != pinned[module["module"]]["build_id"]):
            raise ValueError("Prepared source identity mismatch")
    expected = {f"{name}.{suffix}" for name in names for suffix in SUFFIXES}
    if set(manifest["products"]) != expected:
        raise ValueError("Unexpected prepared product set")
    hashes = {}
    for name in sorted(expected):
        file = prepared / name
        hashes[name] = sha(file)
        if hashes[name] != manifest["products"][name]["sha256"] or file.stat().st_size != manifest["products"][name]["bytes"]:
            raise ValueError(f"Prepared product hash/size mismatch: {name}")
    return hashes


def check_result(result, mode):
    if (result.get("passed") is not True or result.get("mode") != mode or
            result.get("title_profile") != "echoes-of-wisdom-1.1.0" or
            result.get("modules") != 4 or result.get("original_cpu_execution") is not True or
            result.get("original_sdk_tls_checked") is not True or result.get("full_game_verified") is not False):
        raise ValueError("Unexpected native title probe result")
    if mode != "load" and result.get("nnos_initialized") is not True:
        raise ValueError("Original SDK OS initialization not verified")
    if mode == "os-services" and result.get("original_thread_event_cycles") != 3:
        raise ValueError("Original thread/event lifecycle not verified")
    if mode == "sdk-startup" and (result.get("sdk_startup_stages") != 3 or result.get("fixture_context") is not True):
        raise ValueError("Original SDK startup with fixture context not verified")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adb", default=shutil.which("adb") or "adb")
    parser.add_argument("--serial", required=True)
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--prepared", required=True, type=Path)
    parser.add_argument("--profile", default=DEFAULT_PROFILE, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--mode", choices=(*MODES, "all"), default="all")
    args = parser.parse_args()
    profile = json.loads(args.profile.read_text(encoding="utf-8"))
    if profile["id"] != "echoes-of-wisdom":
        raise ValueError("This probe runner currently supports the pinned Zelda profile only")
    hashes = prepared_files(args.prepared, profile)
    output = external_output(args.out, args.prepared)
    if output.exists() and any(output.iterdir()):
        raise ValueError("Use a fresh local results directory")
    output.mkdir(parents=True, exist_ok=True)

    def adb(*arguments, timeout=60, check=True):
        result = subprocess.run([args.adb, "-s", args.serial, *arguments], capture_output=True,
                                encoding="utf-8", errors="replace", timeout=timeout)
        if check and result.returncode:
            raise RuntimeError(f"ADB failed ({result.returncode}): {result.stdout}{result.stderr}")
        return result

    device = {key: adb("shell", "getprop", key).stdout.strip()
              for key in ("ro.product.model", "ro.product.cpu.abi", "ro.build.version.sdk")}
    if device["ro.product.cpu.abi"] != "arm64-v8a" or int(device["ro.build.version.sdk"]) < 29:
        raise ValueError("The probe requires an ARM64 Android 29+ device")
    # Only fixed text and a generated hexadecimal nonce reach the remote shell.
    # Unique result names prevent stale JSON from validating a failed run.
    remote = f"/data/local/tmp/echoes-title-probe-{uuid.uuid4().hex[:12]}"
    adb("shell", "mkdir", "-p", remote + "/patched", remote + "/fixture")
    for name in hashes:
        adb("push", str(args.prepared / name), remote + "/patched/" + name)
    adb("push", str(args.probe), remote + "/probe")
    adb("shell", "chmod", "700", remote + "/probe")
    native_hash = sha(args.probe)
    remote_hashes = {line.split()[1]: line.split()[0] for line in adb("shell", "sha256sum",
                     *[remote + "/patched/" + name for name in hashes], remote + "/probe").stdout.splitlines()}
    for name, digest in hashes.items():
        if remote_hashes.get(remote + "/patched/" + name) != digest:
            raise ValueError(f"Remote product hash mismatch: {name}")
    if remote_hashes.get(remote + "/probe") != native_hash:
        raise ValueError("Remote probe executable hash mismatch")
    report = {"verified_at": datetime.now(timezone.utc).isoformat(), "serial": args.serial,
              "device": device, "remote_directory": remote, "probe_sha256": native_hash,
              "profile_sha256": sha(args.profile), "product_sha256": hashes,
              "results": {}, "passed": False, "full_game_verified": False}
    report_path = output / "device-verification.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    for mode in MODES if args.mode == "all" else (args.mode,):
        command = ["timeout", "-k", "2", "30", remote + "/probe", "--title-profile",
                   "echoes-of-wisdom-1.1.0", remote + "/patched", remote + f"/{mode}.json", mode]
        if mode == "sdk-startup":
            command.append(remote + "/fixture")
        result = adb("shell", *command, check=False)
        (output / f"{mode}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        if result.returncode:
            report["failure"] = {"mode": mode, "exit_code": result.returncode}
            report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
            raise RuntimeError(f"{mode} failed ({result.returncode}); see {output / (mode + '.log')}")
        text = adb("shell", "cat", remote + f"/{mode}.json").stdout
        (output / f"{mode}.json").write_text(text, encoding="utf-8")
        parsed = json.loads(text)
        check_result(parsed, mode)
        report["results"][mode] = parsed
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"Passed {mode}", flush=True)
    report["passed"] = True
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"report": str(report_path), "passed": True, "full_game_verified": False}))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, RuntimeError, KeyError, OSError, subprocess.TimeoutExpired) as error:
        print(f"Android title device probe failed: {error}", file=sys.stderr)
        sys.exit(1)
