"""Build/run capability and cache corruption tests with the Android NDK.

Does not install an APK or touch app data. Outputs include exact source hashes.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import uuid


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--serial')
    p.add_argument('--run-device', action='store_true')
    p.add_argument('--host', action='store_true', help='Build and execute with the host C compiler')
    p.add_argument('--vulkan-include', type=Path, help='Host include directory containing vulkan/')
    p.add_argument('--sanitize', action='store_true', help='Host ASan and UBSan')
    a = p.parse_args()
    if a.run_device and not a.serial:
        p.error('--serial required with --run-device')
    if a.host and (a.run_device or not a.vulkan_include):
        p.error('--host requires --vulkan-include and cannot use --run-device')
    if a.sanitize and not a.host:
        p.error('--sanitize requires --host')
    repo = Path(__file__).resolve().parents[3]
    out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    sdk = Path(os.environ['LOCALAPPDATA']) / 'Android/Sdk' if not a.host else None
    cc = sdk / 'ndk/27.2.12479018/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android29-clang.cmd' if sdk else 'gcc'
    adb = [sdk / 'platform-tools/adb.exe', '-s', a.serial] if a.serial else []
    remote = '/data/local/tmp/d2r-compat-' + uuid.uuid4().hex[:12]
    report = dict(passed=False, device_executed=False, host_executed=a.host, sanitized=a.sanitize, tests=[], serial=a.serial)
    receipt = out / 'result.json'

    def save():
        receipt.write_text(json.dumps(report, indent=2) + '\n')

    def run(command, log):
        r = subprocess.run(list(map(str, command)), cwd=repo, capture_output=True, timeout=90)
        (out / log).write_bytes(r.stdout + r.stderr)
        if r.returncode:
            print((r.stdout + r.stderr).decode(errors='replace'))
        r.check_returncode()
        return r.stdout.decode().strip()

    save()
    if a.run_device:
        run([*adb, 'shell', 'mkdir', '-p', remote], 'mkdir.log')
    for name in ('gpu_profile_policy', 't_pipeline_cache_key', 't_pipeline_cache_io', 't_sampler_policy'):
        source = Path(__file__).with_name(name + '.c')
        binary = out / name
        extra = ['-I', a.vulkan_include] if a.host else []
        if a.sanitize:
            extra += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
        run([cc, '-std=c11', '-O2', '-g', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', *extra,
             source, '-lm', '-o', binary], name + '-build.log')
        result = dict(name=name, build_passed=True, executed=False)
        if a.host:
            result.update(executed=True, stdout=run([binary, out], name + '.log'))
            print(name, result['stdout'], flush=True)
        if a.run_device:
            run([*adb, 'push', binary, remote + '/' + name], name + '-push.log')
            run([*adb, 'shell', 'chmod', '700', remote + '/' + name], name + '-chmod.log')
            result.update(executed=True, stdout=run([*adb, 'shell', 'timeout', '30', remote + '/' + name, remote], name + '.log'))
            print(name, result['stdout'], flush=True)
        report['tests'].append(result); save()
    report.update(passed=True, device_executed=a.run_device, source_sha256={
        str(s.relative_to(repo)): hashlib.sha256(s.read_bytes()).hexdigest()
        for s in sorted([*Path(__file__).parent.glob('t_*policy.c'),
                         Path(__file__).with_name('gpu_profile_policy.c'),
                         *Path(__file__).parent.glob('t_pipeline_cache*.c'),
                         *repo.glob('dm/gfx/nvn/nvn_vk_pipeline_cache*.h'),
                         repo / 'dm/gfx/nvn/nvn_vk_profile.h', repo / 'dm/gfx/nvn/nvn_vk_sampler_policy.h'])})
    save()


if __name__ == '__main__':
    main()
