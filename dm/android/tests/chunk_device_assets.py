"""Compute producers for submission-boundary GPU readback checks."""
import gzip
import hashlib
from pathlib import Path
import struct


def prepare(cache, out, ndk, run):
    index = bytearray((cache / 'shader_index.bin').read_bytes())
    count = struct.unpack_from('<I', index, 8)[0]
    cases = Path(__file__).with_name('chunk_device_cases.inc').read_text(encoding='utf-8')
    shaders = {}
    for label, color in [('A', '.25,.5,.75,1'), ('B', '.75,.25,.5,1')]:
        glsl = ('#version 450\nlayout(local_size_x=1) in;'
                'layout(set=0,binding=0,std430) buffer Vertices { vec4 values[]; };'
                'void main(){for(int i=0;i<3;i++)values[i*2+1]=vec4(' + color + ');}\n')
        key = hashlib.sha256(glsl.encode()).hexdigest()[:16]
        source = out / (key + '.comp'); source.write_text(glsl, encoding='utf-8')
        spv = out / (key + '.spv')
        run([ndk / 'shader-tools/windows-x86_64/glslc.exe', '--target-env=vulkan1.1', source, '-o', spv], key + '-compile.log')
        run([ndk / 'shader-tools/windows-x86_64/spirv-val.exe', '--target-env', 'vulkan1.1', spv], key + '-validate.log')
        module = spv.read_bytes()
        (cache / (key + '.spv.gz')).write_bytes(gzip.compress(module, mtime=0))
        # Compute stage, writable storage buffer at guest slot 3.
        meta = struct.pack('<12I', 0x4d4b5644, 1, 5, 1, 0, 0, 1, 1, 1, 1, 0, len(module))
        meta += struct.pack('<4I2i2I', 0, 0, 7, 3, 3, 0, 1, 1) + struct.pack('<I', 1)
        (cache / (key + '.vkmeta')).write_bytes(meta)
        index.extend(key.encode() + struct.pack('<I', 5) + bytes.fromhex(key) * (0x880 // 8))
        count += 1
        cases = cases.replace('@COMPUTE_' + label + '@', key)
        shaders[key] = dict(stage='comp', spirv_sha256=hashlib.sha256(module).hexdigest())
    struct.pack_into('<I', index, 8, count)
    (cache / 'shader_index.bin').write_bytes(index)
    return cases, shaders
