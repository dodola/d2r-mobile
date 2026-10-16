"""Storage-image producer for conservative/declared usage GPU comparisons."""
import gzip
import hashlib
from pathlib import Path
import struct


def prepare(cache, out, ndk, run):
    glsl = ('#version 450\nlayout(local_size_x=1) in;'
            'layout(set=0,binding=0,rgba8) uniform image2D target;'
            'void main(){vec4 c=imageLoad(target,ivec2(0));'
            'imageStore(target,ivec2(0),vec4(vec3(1)-c.rgb,c.a));}\n')
    key = hashlib.sha256(glsl.encode()).hexdigest()[:16]
    source = out / (key + '.comp'); source.write_text(glsl)
    spv = out / (key + '.spv')
    run([ndk / 'shader-tools/windows-x86_64/glslc.exe', '--target-env=vulkan1.1', source, '-o', spv], 'image-compile.log')
    run([ndk / 'shader-tools/windows-x86_64/spirv-val.exe', '--target-env', 'vulkan1.1', spv], 'image-validate.log')
    module = spv.read_bytes()
    (cache / (key + '.spv.gz')).write_bytes(gzip.compress(module, mtime=0))
    meta = struct.pack('<12I', 0x4d4b5644, 1, 5, 1, 0, 0, 1, 1, 1, 1, 0, len(module))
    meta += struct.pack('<4I2i2I', 0, 0, 3, 6, 0, 0, 1, 1) + struct.pack('<I', 1)
    (cache / (key + '.vkmeta')).write_bytes(meta)
    index = bytearray((cache / 'shader_index.bin').read_bytes())
    count = struct.unpack_from('<I', index, 8)[0]
    struct.pack_into('<I', index, 8, count + 1)
    index.extend(key.encode() + struct.pack('<I', 5) + bytes.fromhex(key) * (0x880 // 8))
    (cache / 'shader_index.bin').write_bytes(index)
    cases = Path(__file__).with_name('image_usage_device_cases.inc').read_text()
    return cases.replace('@IMAGE_COMPUTE@', key), {key: hashlib.sha256(module).hexdigest()}
