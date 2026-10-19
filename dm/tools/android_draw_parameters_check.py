"""Build/run the Android Vulkan draw-parameter semantic regression.

Compiles and validates the fixture shaders, checks the exact uploaded payload,
then compares native BuiltIns and portable specialization against real fragment
SSBO readback. Does not launch an APK, load game modules, or touch saves.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import shutil
import uuid

REPO = Path(__file__).resolve().parents[2]
FIXTURES = REPO / "dm/android/tests"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def decode_receipt(output: bytes, mode: str) -> dict:
    """Require one complete fixture result, with all semantic checks present."""
    rows = []
    for line in output.decode("utf8", "replace").splitlines():
        if line.startswith("{"):
            rows.append(json.loads(line))
    if len(rows) != 1:
        raise RuntimeError(f"{mode}: expected exactly one fixture JSON receipt")
    result = rows[0]
    if (not isinstance(result, dict)
            or result.get("passed") is not True
            or result.get("fixture_only") is not True
            or result.get("mode") != mode
            or type(result.get("cases")) is not int or result["cases"] != 3
            or type(result.get("checked_values")) is not int or result["checked_values"] != 90
            or result.get("negative_base_vertex") is not True
            or result.get("nonzero_base_instance") is not True
            or not isinstance(result.get("gpu"), str) or not result["gpu"]):
        raise RuntimeError(f"{mode}: incomplete draw-parameter semantic receipt")
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--glslc", required=True, help="NDK glslc executable")
    parser.add_argument("--validator", required=True, help="spirv-val executable")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--timeout", type=int, default=30, help="seconds per device mode (1..300)")
    args = parser.parse_args()
    out = args.out.resolve()
    if out.is_relative_to(REPO) or out.exists() and (not out.is_dir() or any(out.iterdir())):
        parser.error("use a fresh/empty output directory outside the checkout")
    if not 1 <= args.timeout <= 300:
        parser.error("--timeout must be between 1 and 300 seconds")
    if not args.serial.strip():
        parser.error("--serial must explicitly select a device")
    sources = [FIXTURES / "t_draw_parameters_device.c",
               FIXTURES / "draw_parameters.vert", FIXTURES / "draw_parameters.frag",
               REPO / "dm/gfx/nvn/nvn_vk_draw_parameters.h"]
    if not all(path.is_file() for path in sources):
        parser.error("draw-parameter fixture sources are missing")
    out.mkdir(parents=True, exist_ok=True)
    result = {"passed": False, "created_utc": datetime.now(timezone.utc).isoformat(),
              "serial": args.serial, "fixture_context": True, "original_game_frame": False,
              "source_sha256": {str(path.relative_to(REPO)): sha256(path) for path in sources},
              "commands": [], "cases": {}}

    def run(command: list[str | Path], label: str, timeout: int = 60) -> bytes:
        command = [str(item) for item in command]
        record = {"label": label, "command": command}
        result["commands"].append(record)
        try:
            completed = subprocess.run(command, capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired as error:
            (out / (label + ".stdout.txt")).write_bytes(error.stdout or b"")
            (out / (label + ".stderr.txt")).write_bytes(error.stderr or b"")
            record.update(timed_out=True, timeout_seconds=timeout)
            raise RuntimeError(f"{label} timed out; see {out}") from error
        except OSError as error:
            record["error"] = str(error)
            (out / (label + ".stderr.txt")).write_text(str(error), encoding="utf8")
            raise RuntimeError(f"{label} could not start; see {out}") from error
        (out / (label + ".stdout.txt")).write_bytes(completed.stdout)
        (out / (label + ".stderr.txt")).write_bytes(completed.stderr)
        record["exit_code"] = completed.returncode
        if completed.returncode:
            raise RuntimeError(f"{label} failed ({completed.returncode}); see {out}")
        return completed.stdout

    try:
        payload = {}
        for stage in ("vert", "frag"):
            shader = out / ("draw_parameters." + stage + ".spv")
            run([args.glslc, "--target-env=vulkan1.1", "-fshader-stage=" + stage,
                 FIXTURES / ("draw_parameters." + stage), "-o", shader], "compile-" + stage)
            run([args.validator, "--target-env", "vulkan1.1", shader], "validate-" + stage)
            payload[stage + ".spv"] = shader
        run([args.cmake, "--build", args.build_dir, "--target", "dm_android_draw_probe", "-j6"],
            "build", 180)
        current_sources = {str(path.relative_to(REPO)): sha256(path) for path in sources}
        if current_sources != result["source_sha256"]:
            raise RuntimeError("fixture source changed while compiling/building")
        result["source_sha256_stable"] = True
        # Keep the exact tested executable even if a concurrent build replaces
        # the build-directory product after this point.
        payload["probe"] = out / "dm_android_draw_probe"
        shutil.copyfile(args.build_dir.resolve() / "dm_android_draw_probe", payload["probe"])
        result["payload"] = {name: {"sha256": sha256(path), "bytes": path.stat().st_size}
                             for name, path in payload.items()}
        result["binary_sha256"] = result["payload"]["probe"]["sha256"]
        remote = "/data/local/tmp/draw-parameters-check-" + uuid.uuid4().hex
        result["remote_directory"] = remote
        adb = [args.adb, "-s", args.serial]
        run([*adb, "shell", "mkdir", "-p", remote], "mkdir")
        for name, path in payload.items():
            run([*adb, "push", path, remote + "/" + name], "push-" + name)
        manifest = out / "payload.sha256"
        # LF is intentional: Windows CRLF makes sha256sum include CR in names.
        manifest.write_bytes("".join(record["sha256"] + "  " + remote + "/" + name + "\n"
                                     for name, record in result["payload"].items()).encode("ascii"))
        run([*adb, "push", manifest, remote + "/payload.sha256"], "push-manifest")
        checked = run([*adb, "shell", "sha256sum", "-c", remote + "/payload.sha256"],
                      "verify-payload").decode("utf8", "replace").splitlines()
        expected = {remote + "/" + name + ": OK" for name in payload}
        if len(checked) != len(expected) or set(checked) != expected:
            raise RuntimeError("device payload SHA256 verification incomplete")
        result["device_payload_sha256_verified"] = True
        run([*adb, "shell", "chmod", "700", remote + "/probe"], "chmod")
        for mode in ("native", "portable"):
            output = run([*adb, "shell", "timeout", str(args.timeout), remote + "/probe",
                          remote + "/vert.spv", mode, remote + "/frag.spv"], mode,
                         args.timeout + 15)
            result["cases"][mode] = decode_receipt(output, mode)
        if result["cases"]["native"]["gpu"] != result["cases"]["portable"]["gpu"]:
            raise RuntimeError("native and portable selected different GPUs")
        result["passed"] = True
    except Exception as error:
        result["error"] = str(error)
        raise
    finally:
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf8")
        print(json.dumps(result))


if __name__ == "__main__":
    main()
