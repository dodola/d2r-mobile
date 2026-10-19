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
    parser.add_argument('--index-checks', action='store_true', help='Compare CPU/GPU-authored 16/32-bit indices, high raw indices and negative base vertices across chunk thresholds')
    parser.add_argument('--resource-checks', action='store_true', help='Verify cross-submit view/framebuffer reuse, texture reincarnation and bounded retention with GPU pixels')
    parser.add_argument('--image-usage-checks', action='store_true', help='Compare image usage policies and late storage promotion across mip/layer GPU contents')
    parser.add_argument('--pool-owned-bypass', action='store_true', help='Enable fully GPU-owned pool-read bypass in all fixture processes')
    parser.add_argument('--chunk-checks', action='store_true', help='Compare live submission thresholds with GPU-produced vertex data and alternating attachments')
    parser.add_argument('--pass-stats-checks', action='store_true', help='With --chunk-checks, compare disabled/enabled pipeline statistics, query overflow and both reset paths')
    parser.add_argument('--pipeline-check', action='store_true', help='Also repeat with a fresh persistent cache, then warm it across a process restart')
    args = parser.parse_args()
    if args.pipeline_check and any((args.cache_roundtrip,args.sampler_checks,args.command_state_checks,args.depth_copy_checks,args.vertex_checks,args.index_checks,args.resource_checks,args.image_usage_checks,args.chunk_checks)):
        parser.error("Run pipeline prewarming separately from other optional suites")
    if args.image_usage_checks and any((args.resource_checks,args.index_checks,args.chunk_checks,args.vertex_checks,args.depth_copy_checks,args.command_state_checks,args.cache_roundtrip,args.sampler_checks)):
        parser.error('Run image usage checks separately from other optional suites')
    if args.resource_checks and any((args.index_checks,args.chunk_checks,args.vertex_checks,args.depth_copy_checks,args.command_state_checks,args.cache_roundtrip,args.sampler_checks)):
        parser.error('Run resource-retention checks separately from other optional suites')
    if args.run_device and not args.serial:
        parser.error('--serial is required with --run-device')
    if args.pass_stats_checks and not args.chunk_checks:
        parser.error('--pass-stats-checks requires --chunk-checks')
    if args.index_checks and (args.chunk_checks or args.vertex_checks or args.depth_copy_checks or args.command_state_checks or args.cache_roundtrip or args.sampler_checks):
        parser.error('Run index comparisons separately from other optional suites')
    if args.command_state_checks and args.cache_roundtrip:
        parser.error('Run command-state and persistent-cache comparisons separately')
    if args.depth_copy_checks and (args.command_state_checks or args.cache_roundtrip or args.sampler_checks):
        parser.error('Run depth-copy comparisons separately from other optional suites')
    if args.vertex_checks and (args.depth_copy_checks or args.command_state_checks or args.cache_roundtrip or args.sampler_checks):
        parser.error('Run vertex comparisons separately from other optional suites')
    if args.chunk_checks and (args.vertex_checks or args.depth_copy_checks or args.command_state_checks or args.cache_roundtrip or args.sampler_checks):
        parser.error('Run chunk comparisons separately from other optional suites')
    repo = Path(__file__).resolve().parents[3]
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=not args.pipeline_check)
    report = {'passed': False, 'device_executed': False, 'original_game_frame': False,
              'recorded_at_utc': datetime.now(timezone.utc).isoformat()}
    receipt = out / 'result.json'
    receipt.write_text(json.dumps(report) + '\n')

    def run(command, log, timeout=90):
        command=list(command)
        if args.pool_owned_bypass and 'shell' in command and 'timeout' in command and 'run' in command:
            at=command.index('shell')+1
            command[at:at]=['env','DM_POOL_OWNED_READ_BYPASS=1']
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
    if args.image_usage_checks:
        import image_usage_device_assets
        source=source.replace('void dm_vk_attachment_config(int);','void dm_vk_image_usage_stats(uint64_t [5]);\nvoid dm_vk_attachment_config(int);')
        copied_cache=out/'image-cache';shutil.copytree(args.cache,copied_cache,dirs_exist_ok=True);args.cache=copied_cache
        image_cases,report['image_shaders']=image_usage_device_assets.prepare(args.cache,out,ndk,run)
        extra_cases+='\n'+image_cases
        report['image_fixture_sha256']={p.name:sha(p) for p in (Path(__file__).with_name('image_usage_device_assets.py'),Path(__file__).with_name('image_usage_device_cases.inc'))}
    if args.resource_checks:
        source = source.replace('void dm_vk_attachment_config(int);',
            'void dm_vk_resource_config(int);\nint dm_vk_resource_applied(void);\nvoid dm_vk_resource_stats(uint64_t [8]);\nvoid dm_vk_attachment_config(int);')
        resource_cases = Path(__file__).with_name('resource_device_cases.inc')
        extra_cases += '\n' + resource_cases.read_text(encoding='utf-8')
        report['resource_fixture_sha256'] = sha(resource_cases)
    if args.index_checks:
        import index_device_assets
        source = source.replace('void dm_vk_attachment_config(int);',
            'int dm_vk_index_full_uint32(void);\nvoid dm_vk_chunk_config(int);\nint dm_vk_chunk_applied(void);\nuint64_t dm_vk_chunk_count(void);\nvoid dm_vk_attachment_config(int);')
        copied_cache = out / 'index-cache'
        shutil.copytree(args.cache, copied_cache, dirs_exist_ok=True); args.cache = copied_cache
        index_cases, report['index_shaders'] = index_device_assets.prepare(args.cache, out, ndk, run)
        extra_cases += '\n' + index_cases
        report['index_fixture_sha256'] = {p.name: sha(p) for p in
            (Path(__file__).with_name('index_device_assets.py'), Path(__file__).with_name('index_device_cases.inc'))}
    if args.chunk_checks:
        import chunk_device_assets
        source = source.replace('void dm_vk_attachment_config(int);',
            'void dm_vk_pass_stats(uint64_t [7]);\nvoid dm_vk_chunk_config(int);\nint dm_vk_chunk_applied(void);\nuint64_t dm_vk_chunk_count(void);\nvoid dm_vk_attachment_config(int);')
        copied_cache = out / 'chunk-cache'
        shutil.copytree(args.cache, copied_cache, dirs_exist_ok=True); args.cache = copied_cache
        chunk_cases, report['chunk_shaders'] = chunk_device_assets.prepare(args.cache, out, ndk, run)
        extra_cases += '\n' + chunk_cases
        report['chunk_fixture_sha256'] = {p.name: sha(p) for p in
            (Path(__file__).with_name('chunk_device_assets.py'), Path(__file__).with_name('chunk_device_cases.inc'))}
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
    if args.pipeline_check:
        source = source.replace('    int draw_ok=', '    if(getenv("DM_VK_PIPELINE_DIR"))sleep(6);\n    int draw_ok=')
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
    if args.index_checks:
        flags += ['-DDM_VK_INDEX_TEST']
    if args.image_usage_checks:
        flags += ['-DDM_VK_IMAGE_USAGE_TEST']
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
    if args.chunk_checks or args.index_checks or args.resource_checks:
        cache_env = ['env', 'DM_RESOURCE_STATS=1', 'DM_COMMAND_STATE_CACHE=0']
    if args.image_usage_checks:
        cache_env=['env','DM_RESOURCE_STATS=1','DM_COMMAND_STATE_CACHE=0','DM_VK_IMAGE_USAGE_FLAGS=0']
    if args.cache_roundtrip:
        run([*adb, 'shell', 'mkdir', remote + '/pipeline'], 'mkdir-pipeline.log')
        cache_env = ['env', 'DM_VK_PIPELINE_CACHE_DIR=' + remote + '/pipeline']
    report.update(device_executed=True, serial=args.serial,pool_owned_bypass=args.pool_owned_bypass)
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
    if args.image_usage_checks:
        records=[]
        for mode in range(4):
            declared=mode in (1,2);command_cache=mode>=2;text=output
            if mode:
                checked=run([*adb,'shell','env','DM_RESOURCE_STATS=1','DM_COMMAND_STATE_CACHE='+str(int(command_cache)),
                    'DM_VK_IMAGE_USAGE_FLAGS='+str(int(declared)),'timeout','90',remote+'/probe',remote+'/cache',remote+'/color.rgba','run'],f'image-mode-{mode}.log',100)
                text=(checked.stdout+checked.stderr).decode('utf-8',errors='replace')
                runtime_mode=json.loads(checked.stdout.decode().strip().splitlines()[-1])
                if not all(runtime_mode[k] for k in ('compute_passed','draw_passed','real_write_fault_hook_passed','compact_records_passed')):raise RuntimeError('Image usage base fixture failed')
            rows=[dict(re.findall(r'(\w+)=([^\s]+)',line)) for line in text.splitlines() if 'IMAGE_USAGE_PHASE ' in line]
            expected=[dict(variant=str(i),declared=str(int(declared)),image_flag=str(i&1),promotions=str(int(declared and not (i&1))),freed_old_images='1',passed='1') for i in range(4)]
            if rows!=expected or 'IMAGE_USAGE_DEVICE variants=4 levels=3 layers=3 compute=8 compared_bytes=516096 same_submit_producer=1 passed=1' not in text:
                raise RuntimeError('Image promotion/content/lifetime evidence missing: '+repr(rows))
            driver=re.search(r'\[nvn-vulkan\] driver id=(\d+)',text)
            if not driver:raise RuntimeError('Image usage driver identity missing')
            pixels_file=out/f'images-{mode}.bin';run([*adb,'pull',remote+'/color.rgba.images',pixels_file],f'images-{mode}-pull.log')
            if pixels_file.stat().st_size!=516096:raise RuntimeError('Image usage readback size mismatch')
            records.append(dict(declared=declared,command_cache=command_cache,driver_id=int(driver[1]),phases=rows,compute=8,readback_bytes=pixels_file.stat().st_size,sha256=sha(pixels_file)))
        if len({r['sha256'] for r in records})!=1 or len({r['driver_id'] for r in records})!=1:raise RuntimeError('Image usage policies changed pixels or driver')
        report['image_usage_modes']=records
    if args.resource_checks:
        records=[]
        for mode in range(2):
            text=output
            if mode:
                checked=run([*adb,'shell','env','DM_RESOURCE_STATS=1','DM_COMMAND_STATE_CACHE=1','DM_VK_RETAIN_RESOURCES=1',
                    'timeout','90',remote+'/probe',remote+'/cache',remote+'/color.rgba','run'], 'resource-cache-on.log',100)
                text=(checked.stdout+checked.stderr).decode('utf-8',errors='replace')
                runtime_mode=json.loads(checked.stdout.decode().strip().splitlines()[-1])
                if not all(runtime_mode[k] for k in ('compute_passed','draw_passed','real_write_fault_hook_passed','compact_records_passed')):
                    raise RuntimeError('Resource base fixture failed')
            rows=[dict(re.findall(r'(\w+)=([^\s]+)',row)) for row in text.splitlines() if 'RESOURCE_PHASE ' in row]
            expected=[dict(phase=str(i),enabled=str(on),draws='24',creates=str(2 if i==1 else 0 if i==2 else 24),hits=str(22 if i==1 else 24 if i==2 else 0),passed='1') for i,on in enumerate((0,1,1,0))]
            if rows!=expected or 'RESOURCE_DEVICE modes=4 draws=374 read_bytes=6127616 recreations=4 trim_views=270 trim_passes=270 passed=1' not in text:
                raise RuntimeError('Resource reuse/invalidation/trim evidence missing: '+repr(rows))
            driver=re.search(r'\[nvn-vulkan\] driver id=(\d+)',text)
            if not driver: raise RuntimeError('Resource driver identity missing')
            pixels_file=out/f'resources-{mode}.bin'
            run([*adb,'pull',remote+'/color.rgba.resources',pixels_file],f'resources-{mode}-pull.log')
            if pixels_file.stat().st_size!=6127616: raise RuntimeError('Resource readback size mismatch')
            records.append(dict(command_cache=bool(mode),driver_id=int(driver[1]),phases=rows,readback_bytes=pixels_file.stat().st_size,sha256=sha(pixels_file)))
        if len({r['sha256'] for r in records})!=1 or len({r['driver_id'] for r in records})!=1:
            raise RuntimeError('Resource modes changed pixels or driver')
        report['resource_modes']=records
    if args.index_checks:
        records = []
        for mode in range(2):
            text = output
            if mode:
                checked = run([*adb, 'shell', 'env', 'DM_RESOURCE_STATS=1', 'DM_COMMAND_STATE_CACHE=1',
                    'timeout', '90', remote + '/probe', remote + '/cache', remote + '/color.rgba', 'run'],
                    'index-cache-on-device.log', 100)
                text = (checked.stdout + checked.stderr).decode('utf-8', errors='replace')
                tested_runtime = json.loads(checked.stdout.decode().strip().splitlines()[-1])
                if not all(tested_runtime[k] for k in ('compute_passed', 'draw_passed', 'real_write_fault_hook_passed', 'compact_records_passed')):
                    raise RuntimeError('Indexed base fixture failed')
            if ('INDEX_CAPS full_uint32_supported=1 full_uint32_enabled=1 max_device_index=4294967295' not in text or
                    'INDEX_DEVICE modes=3 index_cases=7 sources=2 draws=2688 compute=21 read_bytes=1376256 full_uint32=1 writeback_canaries=1 passed=1' not in text or
                    'mismatches=0 same_process=1 passed=1' not in text):
                raise RuntimeError('Full-width GPU index proof missing')
            rows = [dict(re.findall(r'(\w+)=([^\s]+)', row)) for row in text.splitlines() if 'INDEX_PHASE ' in row]
            specs = [(16, 0, 0, 16), (16, 65533, -65533, 16), (32, 0, 0, 16),
                     (32, 0x00fffffe, -0x00fffffe, 16), (32, 0x7ffffffe, -0x7ffffffe, 16),
                     (32, 0x80000000, -2147483648, 16), (32, 0xfffffffd, -2147483648, 0)]
            expected = [dict(mode=str(m), threshold=str(threshold), case=str(ci), bits=str(bits),
                             first=str(first), base=str(base), stride=str(stride), gpu_producer=str(producer),
                             draws='64', chunks=str(63 // threshold if threshold else 0), passed='1')
                        for m, threshold in enumerate((0, 16, 48))
                        for ci, (bits, first, base, stride) in enumerate(specs) for producer in range(2)]
            if rows != expected:
                raise RuntimeError('GPU index case/chunk coverage mismatch')
            commands = [dict(re.findall(r'(\w+)=([^\s]+)', row)) for row in text.splitlines() if 'COMMAND_STATE_STATS ' in row]
            driver = re.search(r'\[nvn-vulkan\] driver id=(\d+)', text)
            if (not driver or not commands or any(int(row['enabled']) != mode for row in commands) or
                    bool(int(commands[-1]['index_skipped'])) != bool(mode)):
                raise RuntimeError('Index command cache/driver proof missing')
            pixels = out / f'index-{mode}.rgba'
            run([*adb, 'pull', remote + '/color.rgba.indices', pixels], f'index-{mode}-pull.log')
            if pixels.stat().st_size != 1376256:
                raise RuntimeError('Index readback size mismatch')
            records.append(dict(command_cache=bool(mode), phases=rows, draws=2688, compute=21,
                readback_bytes=pixels.stat().st_size, readback_sha256=sha(pixels), driver_id=int(driver[1]),
                command_stats=commands[-1]))
        if len({r['readback_sha256'] for r in records}) != 1 or len({r['driver_id'] for r in records}) != 1:
            raise RuntimeError('Index modes changed pixels or driver')
        report['index_ab'] = records
    if args.chunk_checks:
        records = []
        for mode in range(5 if args.pass_stats_checks else 2):
            command_cache=mode in (1,3,4)
            pipeline_stats=mode>=2
            reset_range=mode in (2,3)
            text = output
            if mode:
                checked = run([*adb, 'shell', 'env', 'DM_RESOURCE_STATS=1', 'DM_COMMAND_STATE_CACHE='+str(int(command_cache)),
                    'DM_PASS_STATS='+str(int(pipeline_stats)), 'DM_QUERY_RESET_RANGE='+str(int(reset_range)),
                    'timeout', '90', remote + '/probe', remote + '/cache', remote + '/color.rgba', 'run'],
                    f'chunk-{mode}-device.log', 100)
                text = (checked.stdout + checked.stderr).decode('utf-8', errors='replace')
            phase_rows = [dict(re.findall(r'(\w+)=([^\s]+)', row)) for row in text.splitlines() if 'CHUNK_PHASE ' in row]
            thresholds = [0, 16, 48, 256, 256, 48, 16, 0]
            if (len(phase_rows) != 8 or 'CHUNK_DEVICE modes=8 draws=4800 compute=24 read_bytes=262144 same_process=1 passed=1' not in text
                    or 'mismatches=0 same_process=1 passed=1' not in text):
                raise RuntimeError('Chunk GPU readback proof missing')
            for i, row in enumerate(phase_rows):
                expected = dict(mode=str(i), threshold=str(thresholds[i]), draws='600', compute='3',
                    chunks=str(599 // thresholds[i] if thresholds[i] else 0), gpu_vertex_writeback='1', passed='1')
                if row != expected:
                    raise RuntimeError('Chunk path/coverage mismatch: ' + repr(row))
            tested_runtime = runtime if not mode else json.loads(checked.stdout.decode().strip().splitlines()[-1])
            if not all(tested_runtime[k] for k in ('compute_passed', 'draw_passed', 'real_write_fault_hook_passed', 'compact_records_passed')):
                raise RuntimeError('Chunk base fixture failed')
            driver = re.search(r'\[nvn-vulkan\] driver id=(\d+)', text)
            stats = [dict(re.findall(r'(\w+)=([^\s]+)', row)) for row in text.splitlines() if 'COMMAND_STATE_STATS ' in row]
            if not driver or not stats or any(int(row['enabled']) != command_cache for row in stats) or bool(int(stats[-1]['skipped'])) != command_cache:
                raise RuntimeError('Chunk command cache path not exercised')
            query_rows = [dict(re.findall(r'(\w+)=([^\s]+)', row)) for row in text.splitlines() if 'PASS_STATS_PHASE ' in row]
            caps = re.search(r'PASS_STATS_CAPS requested=(\d+) supported=(\d+) enabled=(\d+)', text)
            if not caps or int(caps[1])!=pipeline_stats or int(caps[3])!=pipeline_stats or len(query_rows)!=8:
                raise RuntimeError('Pipeline statistics capability/path proof missing')
            for i,row in enumerate(query_rows):
                expected={'mode':i,'enabled':int(pipeline_stats),'passes':500 if pipeline_stats else 0,
                    'omitted':100 if pipeline_stats else 0,'ia_vertices':1500 if pipeline_stats else 0,
                    'ia_primitives':500 if pipeline_stats else 0,'passed':1}
                if any(int(row[k])!=v for k,v in expected.items()) or any(bool(int(row[k]))!=pipeline_stats for k in ('vs_invocations','fs_invocations')):
                    raise RuntimeError('Pipeline statistics counters/overflow mismatch')
            pixels_file = out / f'chunk-{mode}.bin'
            run([*adb, 'pull', remote + '/color.rgba.chunks', pixels_file], f'chunk-{mode}-pull.log')
            if pixels_file.stat().st_size != 262144:
                raise RuntimeError('Chunk readback length mismatch')
            records.append(dict(command_cache=command_cache, pipeline_statistics=pipeline_stats, query_reset_range=reset_range,
                pipeline_stat_phases=query_rows, driver_id=int(driver[1]), phases=phase_rows,
                read_bytes=pixels_file.stat().st_size, sha256=sha(pixels_file)))
        if len({r['sha256'] for r in records}) != 1 or len({r['driver_id'] for r in records}) != 1:
            raise RuntimeError('Chunk command-cache modes changed GPU pixels or driver')
        report['chunk_modes'] = records
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
    if args.pipeline_check:
        baseline_hash = sha(out / 'color.rgba')
        run([*adb, 'shell', 'mkdir', '-p', remote + '/pipeline-cache'], 'pipeline-mkdir.log')
        pipeline_runs = []
        for label in ('first', 'repeat'):
            tested_pipeline = run([*adb, 'shell', 'env', 'DM_VK_PIPELINE_MODE=2',
                'DM_VK_PIPELINE_DIR=' + remote + '/pipeline-cache', 'timeout', '90', remote + '/probe',
                remote + '/cache', remote + '/color.rgba', 'run'], 'pipeline-' + label + '.log', 100)
            pipeline_log = (tested_pipeline.stdout + tested_pipeline.stderr).decode(errors='replace')
            if 'cases=120 compared_bytes=1720320 mismatches=0 same_process=1 passed=1' not in pipeline_log:
                raise RuntimeError('Pipeline GPU pixel comparison failed')
            run([*adb, 'pull', remote + '/color.rgba', out / ('pipeline-' + label + '.rgba')], 'pipeline-' + label + '-pull.log')
            if sha(out / ('pipeline-' + label + '.rgba')) != baseline_hash:
                raise RuntimeError('Persistent pipeline changed base GPU pixels')
            loaded = int(re.search(r'PIPELINE_CACHE_LOAD .*seed_bytes=(\d+)', pipeline_log)[1])
            claims = len(re.findall(r'PIPELINE_USE seed=1 ', pipeline_log))
            if label == 'first' and loaded:
                raise RuntimeError('First pipeline run must use a fresh cache directory')
            if label == 'repeat' and (not loaded or not claims):
                raise RuntimeError('Expected disk-cache load and manifest pipeline claim')
            pipeline_runs.append({'label': label, 'passed': True, 'loaded_bytes': loaded,
                                  'manifest_claims': claims, 'pixels_sha256': baseline_hash})
        report['pipeline_runs'] = pipeline_runs
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
