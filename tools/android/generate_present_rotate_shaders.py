"""由自编 GLSL 生成并校验呈现模块，不含游戏资源。 / Generate and validate authored presentation shaders, without game content."""
import argparse, os, struct, subprocess, tempfile
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--glslc',type=Path)
p.add_argument('--check',action='store_true')
a=p.parse_args()
root=Path(__file__).resolve().parents[2]/'dm/gfx/nvn'
compiler=a.glslc or Path(os.environ['LOCALAPPDATA'])/'Android/Sdk/ndk/27.2.12479018/shader-tools/windows-x86_64/glslc.exe'
out=['/* 自编 GLSL 的生成结果，请用脚本更新。 / Authored GLSL output; regenerate with generate_present_rotate_shaders.py. */']
with tempfile.TemporaryDirectory(prefix='present-rotate-') as temp:
    for name in ('present_rotate.vert','present_rotate.frag'):
        binary=Path(temp)/(name+'.spv')
        subprocess.run([compiler,'--target-env=vulkan1.1','-O',root/'shaders'/name,'-o',binary],check=True)
        subprocess.run([compiler.with_name('spirv-val.exe' if compiler.suffix=='.exe' else 'spirv-val'),'--target-env','vulkan1.1',binary],check=True)
        data=binary.read_bytes();words=struct.unpack('<%dI'%(len(data)//4),data)
        out.append('static const uint32_t '+name.replace('.','_')+'_spv[] = {')
        for i in range(0,len(words),8):out.append('    '+','.join('0x%08x'%w for w in words[i:i+8])+',')
        out.append('};')
generated='\n'.join(out)+'\n';header=root/'nvn_vk_present_rotate_spv.inc'
if a.check:
    assert header.read_text(encoding='utf-8')==generated,'Regenerate with the pinned compiler'
else:header.write_text(generated,encoding='utf-8',newline='\n')
print('Present rotation shaders verified' if a.check else 'Present rotation shaders generated')
