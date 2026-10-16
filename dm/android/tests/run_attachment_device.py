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
    parser.add_argument('--command-state-checks', action='store_true', help='Compare all four command-state/PSO-last cache modes with GPU readback')
    parser.add_argument('--depth-copy-checks', action='store_true', help='Compare native/forced drawn depth+stencil copies with independent expected pixels and command caching off/on')
    parser.add_argument('--vertex-checks', action='store_true', help='Read back 79 vertex formats, four shader widths and four layouts with native/forced split and command caching off/on')
    args = parser.parse_args()
    if args.run_device and not args.serial:
        parser.error('--serial is required with --run-device')
    if args.command_state_checks and args.cache_roundtrip:
        parser.error('Run command-state and persistent-cache comparisons separately')
    if args.depth_copy_checks and (args.command_state_checks or args.cache_roundtrip or args.sampler_checks):
        parser.error('Run depth-copy comparisons separately from other optional suites')
    if args.vertex_checks and (args.depth_copy_checks or args.command_state_checks or args.cache_roundtrip or args.sampler_checks):
        parser.error('Run vertex comparisons separately from other optional suites')
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
    if args.vertex_checks:
        import vertex_device_assets
        source = source.replace('#include "nvn_obj.h"', '#include "nvn_obj.h"\n#include <math.h>')
        copied_cache = out / 'vertex-cache'
        shutil.copytree(args.cache, copied_cache, dirs_exist_ok=True); args.cache = copied_cache
        vertex_cases, report['vertex_shaders'] = vertex_device_assets.prepare(args.cache, out, ndk, run)
        extra_cases += '\n' + vertex_cases
        report['vertex_fixture_sha256'] = {p.name: sha(p) for p in
            (Path(__file__).with_name('vertex_device_assets.py'), Path(__file__).with_name('vertex_device_cases.inc'))}
    if args.depth_copy_checks:
        source = source.replace('#include "nvn_obj.h"', '#include "nvn_obj.h"\n#include <math.h>')
        source = source.replace('void dm_vk_attachment_config(int);',
            'void dm_vk_depth_copy_stats(uint64_t *,uint64_t *,uint64_t *);\nvoid dm_vk_attachment_config(int);')
        depth_cases = Path(__file__).with_name('depth_copy_device_cases.inc')
        extra_cases = depth_cases.read_text(encoding='utf-8') + '\n' + extra_cases
        report['depth_copy_fixture_sha256'] = sha(depth_cases)
    if args.command_state_checks:
        commands = importlib.import_module('command_state_device_check')
        pso = importlib.import_module('pso_state_device_check')
        draw = 'p_CommandBufferDrawArrays(&cb,test==5?NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP:NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);'
        if pso.EXTRA.count(draw) != 1:
            raise RuntimeError('Command fixture draw anchor changed')
        state_cases = commands.SETUP + pso.EXTRA.replace(draw, commands.REPEATED)
        if state_cases.count('FILE *states=fopen("/data/local/tmp/d2r-mobile-nvn-api/states.rgba","wb");') != 1:
            raise RuntimeError('Command fixture output anchor changed')
        state_cases = state_cases.replace(
            'FILE *states=fopen("/data/local/tmp/d2r-mobile-nvn-api/states.rgba","wb");',
            'char state_path[1200];snprintf(state_path,sizeof state_path,"%s.states",argv[2]);FILE *states=fopen(state_path,"wb");')
        state_cases += '\n    p_EventFinalize(&command_event);\n    p_TextureBuilderSetFormat(&tb,NVN_FORMAT_RGBA8);\n    memcpy(pixels,baseline,sizeof pixels);\n    fprintf(stderr,"COMMAND_STATE_DEVICE state_cases=8 indexed_draws=64 event_cuts=8 compute_switches=8 passed=1\\n");\n'
        extra_cases = state_cases + extra_cases
        report['command_fixture_sha256'] = {
            p.name: sha(p) for p in (args.fixture_tools / 'command_state_device_check.py',
                                     args.fixture_tools / 'pso_state_device_check.py')}
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
    if args.depth_copy_checks:
        flags += ['-DDM_VK_COPY_TEST']
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
                                 if p.suffix in ('.c', '.h', '.inc', '.vert', '.frag') and 'third_party' not in p.parts},
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
    if args.command_state_checks:
        cache_env = ['env', 'DM_RESOURCE_STATS=1', 'DM_COMMAND_STATE_CACHE=0', 'DM_PSO_LAST_CACHE=0']
    if args.depth_copy_checks:
        cache_env = ['env', 'DM_RESOURCE_STATS=1', 'DM_VK_DEPTH_COPY_DRAW=0', 'DM_COMMAND_STATE_CACHE=0']
    if args.vertex_checks:
        run([*adb, 'shell', 'mkdir', remote + '/adapted'], 'mkdir-adapted.log')
        cache_env = ['env', 'DM_RESOURCE_STATS=1', 'DM_VK_VERTEX_SPLIT=0', 'DM_COMMAND_STATE_CACHE=0',
                     'DM_VERTEX_SHADER_DIR=' + remote + '/adapted']
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
    if args.vertex_checks:
        records = []
        for mode in range(4):
            text = output
            if mode:
                checked = run([*adb, 'shell', 'env', 'DM_RESOURCE_STATS=1',
                    'DM_VK_VERTEX_SPLIT=' + str(mode & 1), 'DM_COMMAND_STATE_CACHE=' + str(int(bool(mode & 2))),
                    'DM_VERTEX_SHADER_DIR=' + remote + '/adapted',
                    'timeout', '90', remote + '/probe', remote + '/cache', remote + '/color.rgba', 'run'],
                    f'vertex-{mode}-device.log', 100)
                text = (checked.stdout + checked.stderr).decode('utf-8', errors='replace')
            if ('VERTEX_DEVICE formats=79 input_widths=4 layouts=4 cases=1264 read_bytes=5177344 passed=1' not in text or
                'mismatches=0 same_process=1 passed=1' not in text):
                raise RuntimeError('Vertex format/layout readback proof missing')
            runtime_mode = runtime if not mode else json.loads(checked.stdout.decode('utf-8').strip().splitlines()[-1])
            if not all(runtime_mode[k] for k in ('compute_passed', 'draw_passed', 'real_write_fault_hook_passed', 'compact_records_passed')):
                raise RuntimeError('Vertex base fixture failed')
            driver = re.search(r'\[nvn-vulkan\] driver id=(\d+)', text)
            variants = re.findall(r'VERTEX_INPUT_ADAPT shader=([0-9a-f]+) split=(0x[0-9a-f]+|0) unsigned=(0x[0-9a-f]+|0) signed=(0x[0-9a-f]+|0)', text)
            if (not driver or (mode & 1 and not any(int(v[1], 0) for v in variants)) or
                not any(int(v[2], 0) for v in variants) or not any(int(v[3], 0) for v in variants)):
                raise RuntimeError('Vertex split/unsigned/signed adaptation was not exercised')
            readback = out / f'vertex-{mode}.bin'
            run([*adb, 'pull', remote + '/color.rgba.vertex', readback], f'vertex-{mode}-pull.log')
            if readback.stat().st_size != 5177344:
                raise RuntimeError('Vertex readback size mismatch')
            records.append(dict(mode=mode, driver_id=int(driver[1]), force_split=bool(mode & 1),
                command_cache=bool(mode & 2), variants=variants, sha256=sha(readback)))
        if len({r['sha256'] for r in records}) != 1 or len({r['driver_id'] for r in records}) != 1:
            raise RuntimeError('Vertex modes changed pixels or driver')
        run([*adb, 'pull', remote + '/adapted', out / 'adapted'], 'pull-adapted.log')
        adapted = sorted((out / 'adapted').glob('*.spv'))
        if not adapted:
            raise RuntimeError('Missing actual adapted shader modules')
        for p in adapted:
            run([ndk / 'shader-tools/windows-x86_64/spirv-val.exe', '--target-env', 'vulkan1.1', p], p.stem + '-adapted-validate.log')
        negatives = []
        for layout in (0, 1, 3):
            checked = subprocess.run([str(x) for x in [*adb, 'shell', 'env', 'DM_VERTEX_BAD_RANGE=' + str(layout+1),
                'timeout', '90', remote + '/probe', remote + '/cache', remote + '/negative.rgba', 'run']],
                cwd=repo, capture_output=True, timeout=100)
            text = (checked.stdout + checked.stderr).decode('utf-8', errors='replace')
            (out / f'vertex-negative-{layout}.log').write_text(text, encoding='utf-8')
            if not checked.returncode or 'STOP vertex fetch outside bound range (2)' not in text:
                raise RuntimeError('Short vertex range was not rejected before GPU draw')
            negatives.append(dict(layout=layout, rejected=True, returncode=checked.returncode))
        report.update(vertex_modes=records, vertex_negative_ranges=negatives,
            adapted_vertex_spirv_sha256={p.name: sha(p) for p in adapted})
    if args.depth_copy_checks:
        records = []
        for mode in range(4):
            text = output
            if mode:
                checked = run([*adb, 'shell', 'env', 'DM_RESOURCE_STATS=1',
                    'DM_VK_DEPTH_COPY_DRAW=' + str(mode & 1), 'DM_COMMAND_STATE_CACHE=' + str(int(bool(mode & 2))),
                    'timeout', '90', remote + '/probe', remote + '/cache', remote + '/color.rgba', 'run'],
                    f'depth-{mode}-device.log', 100)
                text = (checked.stdout + checked.stderr).decode('utf-8', errors='replace')
                runtime_mode = json.loads(checked.stdout.decode().strip().splitlines()[-1])
                if not all(runtime_mode[k] for k in ('compute_passed', 'draw_passed', 'real_write_fault_hook_passed', 'compact_records_passed')):
                    raise RuntimeError('Depth-copy base fixture failed')
            found = re.findall(r'DEPTH_COPY_DEVICE formats=4 cases=36 read_texels=(\d+) read_bytes=(\d+) plain=4 blits=(\d+) draws=(\d+) color_rebind=36 occlusion=36 passed=1', text)
            driver = re.search(r'\[nvn-vulkan\] driver id=(\d+)', text)
            if len(found) != 1 or not driver or 'mismatches=0 same_process=1 passed=1' not in text:
                raise RuntimeError('Depth-copy GPU proof missing')
            texels, size, blits, draws = map(int, found[0])
            if texels != 138240 or size != 622080 or blits + draws != 32 or (mode & 1 and (blits or draws != 32)):
                raise RuntimeError('Depth-copy path/coverage mismatch')
            commands = [dict(re.findall(r'(\w+)=([^\s]+)', row)) for row in text.splitlines()
                        if 'COMMAND_STATE_STATS ' in row]
            if not commands or any(int(row['enabled']) != bool(mode & 2) for row in commands):
                raise RuntimeError('Depth-copy command-cache mode not exercised')
            if bool(int(commands[-1]['skipped'])) != bool(mode & 2):
                raise RuntimeError('Depth-copy command-cache skip proof missing')
            pixels_file = out / f'depth-{mode}.bin'
            run([*adb, 'pull', remote + '/color.rgba.depth', pixels_file], f'depth-{mode}-pull.log')
            if pixels_file.stat().st_size != size + 36 * 64 * 64 * 4:
                raise RuntimeError('Depth-copy readback size mismatch')
            records.append(dict(mode=mode, force_draw=bool(mode & 1), command_cache=bool(mode & 2),
                driver_id=int(driver[1]), read_texels=texels, read_bytes=size, blits=blits, draws=draws,
                command_stats=commands[-1], readback_sha256=sha(pixels_file)))
        if len({r['readback_sha256'] for r in records}) != 1 or len({r['driver_id'] for r in records}) != 1:
            raise RuntimeError('Depth-copy modes changed GPU pixels or driver')
        report['depth_copy_ab'] = records
        report['native_depth_blit_exercised'] = any(r['blits'] for r in records)
        report['natural_depth_draw_exercised'] = any(r['draws'] for r in records if not r['force_draw'])
    if args.command_state_checks:
        def state_record(text, mode):
            if ('COMMAND_STATE_DEVICE state_cases=8 indexed_draws=64 event_cuts=8 compute_switches=8 passed=1' not in text
                    or 'PSO_STATE mask=1' not in text
                    or 'mismatches=0 same_process=1 passed=1' not in text):
                raise RuntimeError('Command-state GPU proof missing')
            stats = [dict(re.findall(r'(\w+)=([^\s]+)', line)) for line in text.splitlines()
                     if 'COMMAND_STATE_STATS ' in line]
            if not stats or any(int(row['enabled']) != bool(mode & 1) for row in stats):
                raise RuntimeError('Command-state mode inconsistent')
            last = stats[-1]
            if not (mode & 1) and int(last['skipped']):
                raise RuntimeError('Disabled command cache skipped commands')
            if mode & 1 and any(not int(last[key + '_skipped']) for key in
                               ('pipeline', 'descriptors', 'viewport', 'scissor', 'blend', 'vertex', 'index')):
                raise RuntimeError('Fixture did not exercise every command cache')
            driver = re.search(r'\[nvn-vulkan\] driver id=(\d+)', text)
            pso_stats = [dict(re.findall(r'(\w+)=([^\s]+)', line)) for line in text.splitlines()
                         if 'PSO_LOOKUP_STATS ' in line]
            if not pso_stats or any(int(row['last_cache']) != bool(mode & 2) for row in pso_stats):
                raise RuntimeError('PSO last-cache mode inconsistent')
            if bool(int(pso_stats[-1]['last_hits'])) != bool(mode & 2):
                raise RuntimeError('PSO last-cache path not exercised')
            return dict(mode=mode, command_cache=bool(mode & 1), pso_last=bool(mode & 2),
                        driver_id=int(driver[1]), command_stats=last, pso_stats=pso_stats[-1])
        records = []
        for mode in range(4):
            text = output
            if mode:
                checked = run([*adb, 'shell', 'env', 'DM_RESOURCE_STATS=1',
                               'DM_COMMAND_STATE_CACHE=' + str(mode & 1), 'DM_PSO_LAST_CACHE=' + str(int(bool(mode & 2))),
                               'timeout', '90', remote + '/probe', remote + '/cache', remote + '/color.rgba', 'run'],
                              f'command-{mode}-device.log', 100)
                text = (checked.stdout + checked.stderr).decode('utf-8', errors='replace')
                state_runtime = json.loads(checked.stdout.decode().strip().splitlines()[-1])
                if not all(state_runtime[k] for k in ('compute_passed', 'draw_passed',
                                                     'real_write_fault_hook_passed', 'compact_records_passed')):
                    raise RuntimeError('Command-state base fixture failed')
            record = state_record(text, mode)
            pixels = out / f'command-{mode}.rgba'
            run([*adb, 'pull', remote + '/color.rgba.states', pixels], f'command-{mode}-pull.log')
            record.update(readback_bytes=pixels.stat().st_size, readback_sha256=sha(pixels))
            if record['readback_bytes'] != 8 * 64 * 64 * 4:
                raise RuntimeError('Command-state readback size mismatch')
            records.append(record)
        if len({r['readback_sha256'] for r in records}) != 1 or len({r['driver_id'] for r in records}) != 1:
            raise RuntimeError('Command-state mode changed GPU pixels or driver')
        report['command_state_ab'] = records
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
