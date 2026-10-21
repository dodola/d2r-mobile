"""Run host regressions for the v1.0.17 merge with GCC on Linux/WSL.

Usage: python3 dm/android/tests/run_merge_checks.py --output /tmp/d2r-merge
Uses real pthread/mmap/mprotect; it does not contact a phone or run a GPU.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    include = out / 'include/android'
    include.mkdir(parents=True, exist_ok=True)
    (include / 'api-level.h').write_text('int android_get_device_api_level(void);\n')
    common = ['gcc', '-std=c11', '-D_GNU_SOURCE', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-pthread']
    hooks = 'dm/gfx/tests/t_dirty_host_hooks.c'
    core = 'dm/gfx/nvn/nvn_dirty.c'
    tests = [
        ('pace', ['dm/gfx/tests/t_pace.c'], [], [[]]),
        ('present', ['dm/android/tests/t_present_policy.c'], [], [[]]),
        ('dirty-race', ['dm/gfx/tests/t_dirty_race.c', hooks], ['-D__ANDROID__'], [[]]),
        ('dirty-slots', ['dm/gfx/tests/t_dirty_slots.c', core, hooks], ['-D__ANDROID__'], [[]]),
        ('dirty-around', ['dm/gfx/tests/t_dirty_around.c', core, hooks], ['-D__ANDROID__'], [[]]),
        ('storage-bias-literals', ['dm/gfx/tests/t_storage_bias_literals.c'], ['-Wno-misleading-indentation'], [[]]),
        ('dirty-summary', ['dm/gfx/tests/t_dirty_summary.c', core, hooks], ['-D__ANDROID__'],
         [['tracker', '0'], ['tracker', '1'], ['properties', '1']]),
        ('touch', ['dm/ios/tests/t_touch_controls.c', 'dm/ios/Sources/dm_touch_controls.c'],
         ['-Idm/ios/Sources', '-lm'], [[]]),
        ('thread-lifetime', ['dm/android/tests/t_thread_lifetime.c'],
         ['-D__ANDROID__', '-I' + str(include.parent)], [[]]),
        ('save-recovery', ['dm/android/tests/t_fs_host.c', 'dm/host/hle_fs.c',
                           'dm/android/services_fs_probe.c', 'dm/android/services_fs.c', 'dm/android/romfs_image.c',
                           'dm/android/lifecycle_android.c', 'dm/android/save_notice_android.c'],
         ['-D__ANDROID__', '-DDM_ANDROID_HOST_FS_TEST', '-Wno-format', '-Wno-unused-function', '-Wno-format-truncation'],
         [[str(out / 'save-recovery.json')]]),
    ]
    results = []
    report = out / 'result.json'
    report.write_text(json.dumps({'passed': False, 'status': 'running', 'device_contacted': False}) + '\n')
    def run(command, log):
        env = dict(os.environ, DM_FAULT_AROUND='1', DM_PROTECT_AROUND='1')   # the exact-semantics tests; dirty-around sets its own
        process = subprocess.run(command, cwd=repo, capture_output=True, text=True, timeout=90, env=env)
        (out / log).write_text(process.stdout + process.stderr, encoding='utf-8')
        process.check_returncode()
        return process.stdout
    for name, sources, flags, runs in tests:
        run(common + sources + flags + ['-o', str(out / name)], name + '-build.log')
        for i, arguments in enumerate(runs):
            stdout = run([str(out / name)] + arguments, name + '-' + str(i) + '.log')
            last_line = stdout.splitlines()[-1] if stdout.strip() else 'passed (see log)'
            if name == 'save-recovery':
                saved = json.loads(Path(arguments[0]).read_text())
                if not saved['passed'] or saved['failures']:
                    raise RuntimeError('Save recovery probe failed')
                last_line = 'SAVE_RECOVERY checks=%d failures=0 host_linux=1' % saved['checks']
            result = {'test': name, 'arguments': arguments, 'passed': True, 'last_line': last_line}
            results.append(result)
            print(result['last_line'], flush=True)
    # Hash all project C/headers so included production code is part of the receipt.
    hashes = {str(p.relative_to(repo)): hashlib.sha256(p.read_bytes()).hexdigest()
              for p in sorted((repo / 'dm').rglob('*'))
              if p.suffix in ('.c', '.h', '.inc') and 'third_party' not in p.parts}
    report.write_text(json.dumps({'passed': True, 'device_contacted': False, 'gpu_executed': False,
                                'tests': results, 'source_sha256': hashes}, ensure_ascii=False, indent=2) + '\n')


if __name__ == '__main__':
    main()
