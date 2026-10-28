#!/usr/bin/env python3
"""Linux/WSL 回归上游 Android 移植，保存命令、输出和结果；不需要游戏内容。
Linux/WSL regression for the upstream Android port, saving commands, output and results; no game content needed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--spirv-lib',type=Path,required=True)
    args=parser.parse_args()
    repo=Path(__file__).resolve().parents[3]
    args.out.mkdir(parents=True,exist_ok=True)
    common=['-D_GNU_SOURCE','-O1','-g','-fno-pie','-no-pie']
    san=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
    host=['-D__ANDROID__','-DDM_ANDROID_HOST=1','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-Wno-format']
    cases=[
        ('map',['gcc',*common,*san,*host,'dm/host/tests/t_map_restore.c','-lm']),
        ('stash',['gcc',*common,*san,*host,'dm/host/tests/t_widestash_restore.c','-lm']),
        ('cam_state',['gcc',*common,*san,*host,'dm/host/tests/t_cam_state.c','-pthread','-lm']),
        ('cam_math',['g++','-std=c++17',*common,*san,'dm/gfx/tests/t_cam.cpp']),
        ('cam_runtime',['g++','-std=c++17',*common,*san,'dm/gfx/tests/t_cam_runtime.cpp']),
        ('camera_touch',['gcc',*common,*san,'dm/ios/tests/t_camera_touch.c','dm/ios/Sources/dm_touch_controls.c','-lm']),
        ('pipe_guard',['gcc',*common,*san,'dm/gfx/tests/t_pipe_guard.c','-pthread','-lz']),
        ('pipeline_wait',['gcc',*common,*san,'dm/gfx/tests/t_pipeline_wait.c','-pthread','-lz']),
        ('bc8',['gcc',*common,*san,'dm/gfx/tests/t_bc8_layout.c','dm/gfx/tex/nvn_bcdec.c','-lm']),
        ('inline',['g++','-std=c++17',*common,'dm/gfx/tests/t_inline.cpp','dm/gfx/nvn/nvn_vk_inline.cpp','-I','dm/third_party/spirv-tools/include',str(args.spirv_lib.resolve()),'-pthread']),
    ]
    result=[]
    def receipt(passed):
        sources=sorted({s for _,command in cases for s in command if s.startswith('dm/') and (repo/s).is_file()})
        sources+=['dm/gfx/nvn/nvn_vk_cam.h','dm/gfx/nvn/nvn_vk_cam_runtime.inc','dm/gfx/nvn/nvn_vk_pipeline_obtain.inc','dm/gfx/nvn/nvn_vk_pipe_guard.h','dm/host/dm_cam.c']
        (args.out/'result.json').write_text(json.dumps(dict(passed=passed,tests=result,source_sha256={s:hashlib.sha256((repo/s).read_bytes()).hexdigest() for s in sources}),indent=2)+'\n',encoding='utf-8')
    for name,cmd in cases:
        binary=args.out.resolve()/name
        for phase,command in [('build',cmd+['-o',str(binary)]),('run',[str(binary)])]:
            start=time.monotonic()
            proc=subprocess.run(command,cwd=repo,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=120)
            (args.out/f'{name}-{phase}.log').write_text(proc.stdout,encoding='utf-8')
            result.append(dict(case=name,phase=phase,command=command,returncode=proc.returncode,seconds=time.monotonic()-start))
            receipt(False)
            if proc.returncode:
                print(proc.stdout)
                raise SystemExit(f'{name} {phase} failed')
        print(f'{name}: PASS',flush=True)
    print(f'UPSTREAM_PORT PASS cases={len(cases)}')
    receipt(True)

if __name__=='__main__':
    main()
