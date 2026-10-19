"""Build/run the real Android NVN ZCull and texture-write GPU regression.

Uses original, externally supplied fixture shaders. Does not install/restart
any APK, alter saves, or treat fixture pixels as a game frame.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

REPO = Path(__file__).resolve().parents[2]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--shader-cache", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--cmake", default="cmake")
    args = parser.parse_args()
    out = args.out.resolve()
    if out.is_relative_to(REPO) or out.exists() and any(out.iterdir()):
        parser.error("output must be a fresh/empty directory outside the checkout")
    cache = args.shader_cache.resolve()
    if not (cache / "shader_index.bin").is_file():
        parser.error("original fixture shader cache is missing")
    out.mkdir(parents=True, exist_ok=True)

    def run(command: list[str | Path], name: str, timeout: int = 60) -> subprocess.CompletedProcess:
        result = subprocess.run([str(x) for x in command], capture_output=True, timeout=timeout)
        (out / (name + ".stdout.txt")).write_bytes(result.stdout)
        (out / (name + ".stderr.txt")).write_bytes(result.stderr)
        return result

    built = run([args.cmake, "--build", args.build_dir, "--target", "dm_android_zcull_probe", "-j6"], "build", 180)
    if built.returncode:
        raise RuntimeError(f"GPU probe build failed; see {out}")
    binary = args.build_dir / "dm_android_zcull_probe"
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    remote = "/data/local/tmp/nvn-zcull-check-" + digest[:12]
    adb = [args.adb, "-s", args.serial]
    for command, name in [
        ([*adb, "shell", "mkdir", "-p", remote + "/cache"], "mkdir"),
        ([*adb, "push", binary, remote + "/probe"], "push-probe"),
        ([*adb, "push", str(cache) + "/.", remote + "/cache/"], "push-cache"),
        ([*adb, "shell", "chmod", "700", remote + "/probe"], "chmod"),
    ]:
        if run(command, name).returncode:
            raise RuntimeError(f"Device setup failed: {name}; see {out}")
    expected = {
        "zcull-corrupt": "ZCull restore incompatible/corrupt state",
        "zcull-incompatible": "ZCull restore incompatible/corrupt state",
        "zcull-bounds": "ZCull storage bounds/alignment",
        "zcull-alignment": "Android ZCull buffer alignment/size",
        "zcull-size": "Android ZCull buffer alignment/size",
        "unsupported": "unsupported function nvnCommandBufferDispatchComputeIndirect",
        "depth-linear": "depth/stencil blit requires nearest and identical formats",
    }
    cases = {}
    for mode in ["run", *expected]:
        tested = run([*adb, "shell", "timeout", "30", remote + "/probe", remote + "/cache", remote + "/color.rgba", mode], mode, 40)
        logs = (tested.stdout + tested.stderr).decode("utf8", "replace")
        markers = [line for line in logs.splitlines() if line.startswith(("ZCULL_DEVICE ", "ZCULL_DEPTH_TEST ", "CPU_TEXTURE_WRITE_DEVICE ","NULL_RESOURCE_DEVICE ","DEPTH_STENCIL_COPY_DEVICE "))]
        if mode == "run":
            passed = tested.returncode == 0 and len(markers) == 6 and all("passed=1" in line for line in markers)
        else:
            passed = tested.returncode != 0 and expected[mode] in logs
        cases[mode] = {"exit_code": tested.returncode, "passed": passed, "markers": markers}
    result = {"device": args.serial, "binary_sha256": digest, "fixture_context": True,
              "original_game_frame": False, "cases": cases, "passed": all(case["passed"] for case in cases.values())}
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf8")
    print(json.dumps(result))
    if not result["passed"]:
        raise RuntimeError(f"GPU regression failed; see {out}")


if __name__ == "__main__":
    main()
