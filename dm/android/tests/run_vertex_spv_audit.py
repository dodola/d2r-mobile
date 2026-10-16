"""Run the production vertex pass against captured shaders and validate its IR."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--serial', required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    ndk = Path(os.environ['LOCALAPPDATA']) / 'Android/Sdk/ndk/27.2.12479018'
    shader = ndk / 'shader-tools/windows-x86_64'
    adb = [Path(os.environ['LOCALAPPDATA']) / 'Android/Sdk/platform-tools/adb.exe', '-s', args.serial]
    remote = '/data/local/tmp/d2r-vertex-spv-audit'

    def run(command, allowed=()):
        process = subprocess.run([str(x) for x in command], capture_output=True, timeout=60)
        if process.returncode and process.returncode not in allowed:
            raise RuntimeError((command, process.returncode, process.stdout, process.stderr))
        return process

    source = Path(__file__).with_name('vertex_spv_audit.c')
    run([ndk / 'toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android29-clang.cmd',
         '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-I', repo / 'dm/gfx/nvn', source, '-o', out / 'audit'])
    run([*adb, 'shell', 'mkdir', '-p', remote])
    run([*adb, 'push', out / 'audit', remote + '/audit'])
    run([*adb, 'shell', 'chmod', '700', remote + '/audit'])
    records = []
    for p in sorted(args.cache.glob('*.spv.gz')):
        src = out / p.name.removesuffix('.gz'); src.write_bytes(gzip.decompress(p.read_bytes()))
        run([*adb, 'push', src, remote + '/source.spv'])
        for mode in range(3):
            dst = out / (src.stem + f'-{mode}.spv')
            process = run([*adb, 'shell', remote + '/audit', remote + '/source.spv', remote + '/output.spv', mode], (77,))
            if process.returncode == 77:
                records.append(dict(key=src.stem, mode=mode, skipped_no_affected_inputs=True)); continue
            run([*adb, 'pull', remote + '/output.spv', dst])
            result = run([shader / 'spirv-val.exe', '--target-env', 'vulkan1.1', dst])
            (out / (dst.stem + '.log')).write_bytes(result.stdout + result.stderr)
            records.append(dict(key=src.stem, mode=mode, validated=True,
                                sha256=hashlib.sha256(dst.read_bytes()).hexdigest()))
    if not records:
        raise RuntimeError('No captured modules found')
    report = dict(passed=True, records=records, source_sha256={str(p.relative_to(repo)): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in (source, Path(__file__), repo / 'dm/gfx/nvn/nvn_vk_vertex.h', repo / 'dm/gfx/nvn/nvn_vk_vertex_spv.h')})
    (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(passed=True, validated=sum(r.get('validated', False) for r in records), records=len(records))))


if __name__ == '__main__':
    main()
