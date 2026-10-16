"""Build this worktree's real NVN/Vulkan backend and compare GPU pixels on Android.

Uses the existing local original-shader fixture assets; does not install an APK
or modify game data. Device execution requires --run-device and --serial.
"""
import argparse
import hashlib
import importlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import uuid
import gzip
import shutil
import struct
from datetime import datetime, timezone


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture-tools', type=Path, required=True)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--serial')
    parser.add_argument('--run-device', action='store_true')
    parser.add_argument('--cache-roundtrip', action='store_true', help='Compare cold/warm/corrupt/repaired caches with identical GPU readbacks')
    parser.add_argument('--sampler-checks', action='store_true', help='Also read back integer, float and depth comparison sampling')
    args = parser.parse_args()
    if args.run_device and not args.serial:
        parser.error('--serial is required with --run-device')
    repo = Path(__file__).resolve().parents[3]
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    report = {'passed': False, 'device_executed': False, 'original_game_frame': False,
              'recorded_at_utc': datetime.now(timezone.utc).isoformat()}
    receipt = out / 'result.json'
    receipt.write_text(json.dumps(report) + '\n')

    def run(command, log, timeout=90):
        process = subprocess.run([str(x) for x in command], cwd=repo, capture_output=True, timeout=timeout)
        (out / log).write_bytes(process.stdout + process.stderr)
        if process.returncode:
            print((process.stdout + process.stderr).decode('utf-8', errors='replace')[-5000:])
        process.check_returncode()
        return process

    sys.path.insert(0, str(args.fixture_tools.resolve()))
    api = importlib.import_module('shader_nvn_api_check')
    sdk = Path(os.environ['LOCALAPPDATA']) / 'Android/Sdk'
    ndk = sdk / 'ndk/27.2.12479018'
    source = api.HARNESS.replace('    int compute_ok=', '    p_SyncWait(&sync,UINT64_MAX);\n    int compute_ok=')
    source = source.replace('#include "nvn_obj.h"', '#include "nvn_obj.h"\n#include "nvn_vk_attachment_plan.h"\n'
                            'void dm_vk_attachment_config(int);\nvoid dm_vk_attachment_stats(NvnAttachmentStats *);')
    cases = Path(__file__).with_name('attachment_device_cases.inc')
    anchor = 'p_TextureFinalize(&target);'
    assert source.count(anchor) == 1
    extra_cases = cases.read_text(encoding='utf-8')
    if args.sampler_checks:
        copied_cache = out / 'sampler-cache'
        shutil.copytree(args.cache, copied_cache, dirs_exist_ok=True); args.cache = copied_cache
        shaders = {
            'VERTEX': ('vert', 0, '#version 450\nvoid main(){vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);gl_Position=vec4(p*2-1,.5,1);}\n'),
            'UINT': ('frag', 1, '#version 450\nlayout(set=1,binding=0) uniform usampler2D tex;layout(location=0) out vec4 color;void main(){float c=float(texture(tex,gl_FragCoord.xy/64).r)/255.;color=vec4(c,0,0,1);}\n'),
            'FLOAT': ('frag', 1, '#version 450\nlayout(set=1,binding=0) uniform sampler2D tex;layout(location=0) out vec4 color;void main(){float c=texture(tex,gl_FragCoord.xy/64).r;color=vec4(c,0,0,1);}\n'),
            'DEPTH': ('frag', 1, '#version 450\nlayout(set=1,binding=0) uniform sampler2DShadow tex;layout(location=0) out vec4 color;void main(){float c=texture(tex,vec3(gl_FragCoord.xy/64,.5));color=vec4(c,0,0,1);}\n')}
        sampler_cases = Path(__file__).with_name('sampler_device_cases.inc').read_text()
        index = bytearray((args.cache / 'shader_index.bin').read_bytes())
        count = struct.unpack_from('<I', index, 8)[0]
        for label, (stage, model, glsl) in shaders.items():
            key = hashlib.sha256(glsl.encode()).hexdigest()[:16]
            path = out / (label + '.' + stage); path.write_text(glsl)
            spv = out / (label + '.spv')
            run([ndk / 'shader-tools/windows-x86_64/glslc.exe', '--target-env=vulkan1.1', path, '-o', spv], label + '-compile.log')
            run([ndk / 'shader-tools/windows-x86_64/spirv-val.exe', '--target-env', 'vulkan1.1', spv], label + '-validate.log')
            module = spv.read_bytes(); (args.cache / (key + '.spv.gz')).write_bytes(gzip.compress(module, mtime=0))
            nb = 1 if model else 0
            meta = struct.pack('<12I', 0x4d4b5644, 1, model, nb, 0, 0, 1, 0, 0, 0, nb, len(module))
            if nb:
                meta += struct.pack('<4I2i2I', 1, 0, 1, 4, 0, 0, 1, 0)
            (args.cache / (key + '.vkmeta')).write_bytes(meta + struct.pack('<I', 1))
            index.extend(key.encode() + struct.pack('<I', model) + bytes.fromhex(key) * (0x880 // 8))
            sampler_cases = sampler_cases.replace('@' + label + '_SHADER@', key)
        struct.pack_into('<I', index, 8, count + len(shaders)); (args.cache / 'shader_index.bin').write_bytes(index)
        extra_cases += '\n' + sampler_cases
        report['sampler_template_sha256'] = sha(Path(__file__).with_name('sampler_device_cases.inc'))
    source = source.replace(anchor, extra_cases + '\n' + anchor)
    functions = sorted(set(re.findall(r'p_([A-Z][A-Za-z0-9]+)', source)))
    source = source.replace('@DECLARATIONS@', '\n'.join(f'static PFNNVN{x.upper()}PROC p_{x};' for x in functions))
    source = source.replace('@INITIALIZERS@', '\n'.join(f'p_{x}=(PFNNVN{x.upper()}PROC)boot("nvn{x}");' for x in functions))
    harness = out / 'harness.c'; harness.write_text(source, encoding='utf-8')
    sdk = Path(os.environ['LOCALAPPDATA']) / 'Android/Sdk'
    ndk = sdk / 'ndk/27.2.12479018'
    clang = ndk / 'toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android29-clang.cmd'
    dm = repo / 'dm'
    objects = sorted(p for p in (args.build / 'CMakeFiles/dm_android_core.dir').rglob('*.o') if p.name != 'diagnostics.c.o')
    if not objects:
        raise RuntimeError('Build this worktree with CMake first')
    sources = [harness, dm / 'android/diagnostics.c', dm / 'gfx/hle_nv.c',
               *(dm / 'gfx/nvn' / p for p in ('hle_nvn.c', 'nvn_entry.S', 'nvn_thunks.S', 'nvn_vk.c', 'nvn_dirty.c', 'nvn_tex.c')),
               dm / 'android/nvn_platform_clock.c', dm / 'android/nvn_translation_queue.c',
               dm / 'gfx/tex/nvn_deswizzle.c', dm / 'gfx/tex/nvn_bcdec.c']
    binary = out / 'attachment_probe'
    flags = ['-std=c11', '-O2', '-g', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror',
             '-Wno-format', '-Wno-unused-function', '-ffixed-x18']
    for include in ('gfx/nvn/include', 'gfx/nvn', 'android', 'host'):
        flags += ['-I', dm / include]
    flags += sources + objects + ['-lvulkan', '-landroid', '-llog', '-ldl', '-lz', '-lm', '-laaudio',
                                  '-Wl,-z,nostart-stop-gc', '-o', binary]
    response = out / 'build.rsp'
    response.write_text('\n'.join('"' + str(x).replace('\\', '/').replace('"', '\\"') + '"' for x in flags), encoding='utf-8')
    run([clang, '@' + str(response)], 'build.log', 180)
    report.update(build_passed=True, binary_sha256=sha(binary), harness_sha256=sha(harness),
                  template_sha256=sha(args.fixture_tools / 'shader_nvn_api_check.py'),
                  source_sha256={str(p.relative_to(repo)): sha(p) for p in sorted(dm.rglob('*'))
                                 if p.suffix in ('.c', '.h', '.inc') and 'third_party' not in p.parts},
                  linked_core_objects_sha256={str(p.relative_to(args.build)): sha(p) for p in objects},
                  shader_cache_sha256={p.name: sha(p) for p in sorted(args.cache.iterdir()) if p.is_file()})
    receipt.write_text(json.dumps(report, indent=2) + '\n')
    if not args.run_device:
        print('Build passed; device not contacted'); return
    adb = [sdk / 'platform-tools/adb.exe', '-s', args.serial]
    remote = '/data/local/tmp/d2r-attachment-' + uuid.uuid4().hex[:12]
    run([*adb, 'shell', 'mkdir', '-p', remote + '/cache'], 'mkdir.log')
    for p in sorted(args.cache.iterdir()):
        if p.is_file():
            run([*adb, 'push', p, remote + '/cache/' + p.name], 'push-' + p.name + '.log')
    run([*adb, 'push', binary, remote + '/probe'], 'push-probe.log')
    run([*adb, 'shell', 'chmod', '700', remote + '/probe'], 'chmod.log')
    cache_env = []
    if args.cache_roundtrip:
        run([*adb, 'shell', 'mkdir', remote + '/pipeline'], 'mkdir-pipeline.log')
        cache_env = ['env', 'DM_VK_PIPELINE_CACHE_DIR=' + remote + '/pipeline']
    report.update(device_executed=True, serial=args.serial)
    receipt.write_text(json.dumps(report, indent=2) + '\n')
    tested = run([*adb, 'shell', *cache_env, 'timeout', '90', remote + '/probe', remote + '/cache', remote + '/color.rgba', 'run'], 'device.log', 100)
    output = (tested.stdout + tested.stderr).decode('utf-8', errors='replace')
    phases = [dict(re.findall(r'(\w+)=([^\s]+)', line)) for line in output.splitlines() if 'ATTACHMENT_PHASE ' in line]
    summary = [dict(re.findall(r'(\w+)=([^\s]+)', line)) for line in output.splitlines() if 'ATTACHMENT_DEVICE ' in line]
    if (len(phases) != 6 or len(summary) != 1 or summary[0] !=
            dict(phases='6', cases='120', compared_bytes='1720320', mismatches='0', same_process='1', passed='1')):
        raise RuntimeError('Expected GPU comparison markers missing')
    runtime = json.loads(tested.stdout.decode('utf-8').strip().splitlines()[-1])
    if not all(runtime[k] for k in ('compute_passed', 'draw_passed', 'real_write_fault_hook_passed', 'compact_records_passed')):
        raise RuntimeError('Base NVN fixture failed')
    if args.sampler_checks and ('SAMPLER_DEVICE integer_nearest=1 float_filter=1 depth_compare=1 compared_bytes=49152 passed=1' not in output or
                                'SAMPLER_FALLBACK format=98 integer=1' not in output):
        raise RuntimeError('Sampler GPU/fallback proof missing')
    report['sampler_checks'] = args.sampler_checks
    run([*adb, 'pull', remote + '/color.rgba', out / 'color.rgba'], 'pull-color.log')
    if args.cache_roundtrip:
        expected_pixels = sha(out / 'color.rgba')
        def cache_record(text, mode, expected_status):
            load = re.search(r'PIPELINE_CACHE_LOAD status=(-?\d+) seed_bytes=(\d+) enabled=1 disk=1', text)
            saved = re.findall(r'PIPELINE_CACHE_SAVE ok=1 bytes=(\d+) revision=(\d+)', text)
            stats = re.search(r'PIPELINE_CACHE_STATS pipelines=(\d+) compile_ns=(\d+)', text)
            if not load or int(load[1]) != expected_status or not saved or not stats:
                raise RuntimeError('Cache load/save verification failed: ' + mode)
            if 'mismatches=0 same_process=1 passed=1' not in text:
                raise RuntimeError('GPU comparison failed: ' + mode)
            driver = re.search(r'\[nvn-vulkan\] driver id=(\d+)', text)
            return dict(mode=mode, status=int(load[1]), seed_bytes=int(load[2]),
                        saved_bytes=int(saved[-1][0]), pipelines=int(stats[1]),
                        compile_ns=int(stats[2]), driver_id=int(driver[1]))
        records = [cache_record(output, 'cold', 0)]
        for mode, status in [('warm', 1), ('corrupt', -1), ('repaired', 1)]:
            if mode == 'corrupt':
                listing = run([*adb, 'shell', 'ls', remote + '/pipeline'], 'cache-files.log').stdout.decode().split()
                if len(listing) != 1 or not re.fullmatch(r'[0-9a-f]{16}\.bin', listing[0]):
                    raise RuntimeError('Expected one isolated cache file')
                bad = out / 'corrupt-cache.bin'; bad.write_bytes(b'corrupt cache test')
                run([*adb, 'push', bad, remote + '/pipeline/' + listing[0]], 'corrupt-cache-push.log')
            checked = run([*adb, 'shell', *cache_env, 'timeout', '90', remote + '/probe', remote + '/cache',
                           remote + '/color.rgba', 'run'], mode + '-device.log', 100)
            text = (checked.stdout + checked.stderr).decode('utf-8', errors='replace')
            record = cache_record(text, mode, status)
            run([*adb, 'pull', remote + '/color.rgba', out / (mode + '.rgba')], mode + '-pull.log')
            if sha(out / (mode + '.rgba')) != expected_pixels:
                raise RuntimeError('Cache changed pixels: ' + mode)
            record['readback_sha256'] = expected_pixels; records.append(record)
        if len({r['driver_id'] for r in records}) != 1:
            raise RuntimeError('Driver changed across cache runs')
        report['pipeline_cache_roundtrip'] = records
    props = run([*adb, 'shell', 'getprop'], 'device-properties.log').stdout.decode('utf-8', errors='replace')
    report.update(passed=True, phases=phases, summary=summary[0], runtime=runtime, pixel_readback_sha256=sha(out / 'color.rgba'),
                  device_log_sha256=sha(out / 'device.log'),
                  gpu=re.search(r'\[nvn-vulkan\] initialized (.+)', output).group(1),
                  device={k: re.search(r'\[' + re.escape(k) + r'\]: \[(.*?)\]', props).group(1)
                          for k in ('ro.product.model', 'ro.build.version.release', 'ro.build.version.sdk', 'ro.hardware')})
    receipt.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': True, 'serial': args.serial, 'phases': phases, 'receipt': str(receipt)}, indent=2))


if __name__ == '__main__':
    main()
