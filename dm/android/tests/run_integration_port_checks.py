"""Offline checks for the android-integration port; never connects to a device.

Run on Linux/WSL with GCC and Vulkan headers (NDK sysroot headers also work):
python3 dm/android/tests/run_integration_port_checks.py --output /tmp/d2r-port \
    --vulkan-include /path/to/ndk/sysroot/usr/include
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--vulkan-include', type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    result = {'passed': False, 'device_contacted': False, 'gpu_executed': False, 'tests': []}
    report = out / 'result.json'
    report.write_text(json.dumps(result) + '\n')
    sources = ['dm/gfx/tests/t_texture_upload.c', 'dm/gfx/nvn/nvn_tex.c',
               'dm/gfx/tex/nvn_deswizzle.c', 'dm/gfx/tex/nvn_bcdec.c']
    common = ['gcc', '-std=c11', '-D_GNU_SOURCE', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
              '-Wno-misleading-indentation', '-Idm/gfx/nvn/include']
    if args.vulkan_include:
        # Use host libc headers first, not the NDK's Android libc declarations.
        common += ['-idirafter', str(args.vulkan_include.resolve())]

    def run(command, name, expected=0):
        proc = subprocess.run(command, cwd=repo, capture_output=True, text=True, timeout=120)
        (out / (name + '.log')).write_text(proc.stdout + proc.stderr, encoding='utf-8')
        if proc.returncode != expected:
            raise RuntimeError(f'{name}: exit {proc.returncode}; see {out / (name + ".log")}')
        result['tests'].append({'name': name, 'passed': True, 'command': command})
        return proc.stdout + proc.stderr

    for name, flags in [('upload', []), ('upload-sanitized', ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie'])]:
        exe = out / name
        run(common + sources + flags + ['-lm', '-o', str(exe)], name + '-build')
        print(run([str(exe)], name).strip(), flush=True)
    run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic', '-fsyntax-only',
         'dm/gfx/nvn/nvn_vk.c'], 'non-android-guard')
    error = run(['gcc', '-std=c11', '-DDM_EXPECT_VULKAN_BACKEND=1', '-fsyntax-only',
                 'dm/gfx/nvn/nvn_vk.c'], 'wrong-platform-rejected', expected=1)
    if 'Android Vulkan target requires' not in error:
        raise RuntimeError('Wrong-platform check failed for an unrelated reason')
    tracked = sources + ['dm/gfx/nvn/nvn_vk.c', 'dm/gfx/nvn/nvn_vk_texture_upload.inc',
                         'dm/gfx/nvn/nvn_vk_upload.h', 'dm/gfx/nvn/nvn_vk_texture_refresh.inc',
                         'dm/host/dm_yield_backoff.h']
    result['source_sha256'] = {p: hashlib.sha256((repo / p).read_bytes()).hexdigest() for p in tracked}
    result['passed'] = True
    report.write_text(json.dumps(result, indent=2) + '\n')
    print('PLATFORM_GUARDS passed; device_contacted=0 gpu_executed=0', flush=True)


if __name__ == '__main__':
    main()
