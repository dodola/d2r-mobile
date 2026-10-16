"""Authored shaders for CPU/GPU index parity, including full-width raw indices."""
import gzip
import hashlib
from pathlib import Path
import struct


def prepare(cache, out, ndk, run):
    index = bytearray((cache / 'shader_index.bin').read_bytes())
    count = struct.unpack_from('<I', index, 8)[0]
    shaders = {}

    def shader(stage, glsl):
        nonlocal count
        key = hashlib.sha256(glsl.encode()).hexdigest()[:16]
        source = out / (key + '.' + stage)
        source.write_text(glsl, encoding='utf-8')
        spv = out / (key + '.spv')
        run([ndk / 'shader-tools/windows-x86_64/glslc.exe', '--target-env=vulkan1.1', source, '-o', spv], key + '-compile.log')
        run([ndk / 'shader-tools/windows-x86_64/spirv-val.exe', '--target-env', 'vulkan1.1', spv], key + '-validate.log')
        module = spv.read_bytes()
        (cache / (key + '.spv.gz')).write_bytes(gzip.compress(module, mtime=0))
        model = {'vert': 0, 'frag': 1, 'comp': 5}[stage]
        meta = struct.pack('<12I', 0x4d4b5644, 1, model, int(stage == 'comp'), int(stage == 'vert'),
                           0, 1, int(stage == 'comp'), int(stage == 'comp'), int(stage == 'comp'),
                           int(stage == 'frag'), len(module))
        if stage == 'comp':
            meta += struct.pack('<4I2i2I', 0, 0, 7, 3, 3, 0, 1, 1)
        if stage == 'vert':
            meta += struct.pack('<2I', 0, 0)
        (cache / (key + '.vkmeta')).write_bytes(meta + struct.pack('<I', 1))
        index.extend(key.encode() + struct.pack('<I', model) + bytes.fromhex(key) * (0x880 // 8))
        count += 1
        shaders[key] = dict(stage=stage, spirv_sha256=hashlib.sha256(module).hexdigest())
        return key

    vertex = shader('vert', '#version 450\nlayout(location=0) in vec4 data;'
                    'layout(location=0) flat out vec4 value;void main(){'
                    'vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));'
                    'gl_Position=vec4(p[uint(gl_VertexIndex)%3u],.5,1);value=data;}\n')
    fragment = shader('frag', '#version 450\nlayout(location=0) flat in vec4 value;'
                      'layout(location=0) out vec4 color;void main(){color=value;}\n')
    records = []
    # Raw index high bits are independent of the final vertex address. Even the
    # UINT32_MAX test fetches a single in-range element through a zero stride.
    specs = [(16, 0, 0, 16), (16, 65533, -65533, 16), (32, 0, 0, 16),
             (32, 0x00fffffe, -0x00fffffe, 16), (32, 0x7ffffffe, -0x7ffffffe, 16),
             (32, 0x80000000, -2147483648, 16), (32, 0xfffffffd, -2147483648, 0)]
    for bits, first, base_vertex, stride in specs:
        raw = [first, first + 1, first + 2]
        payload = struct.pack('<3' + ('H' if bits == 16 else 'I'), *raw)
        payload += bytes([0x5a]) * (16 - len(payload))
        words = struct.unpack('<4I', payload)
        compute = shader('comp', '#version 450\nlayout(local_size_x=1) in;'
                         'layout(set=0,binding=0,std430) buffer Indices { uint data[]; };'
                         'void main(){' + ''.join(f'data[{i}]={x}u;' for i, x in enumerate(words)) + '}\n')
        records.append('{%d,%d,%s,%d,"%s",{%s}}' %
                       (bits, first, 'INT32_MIN' if base_vertex == -2147483648 else str(base_vertex),
                        stride, compute, ','.join(str(x) + 'u' for x in words)))
    struct.pack_into('<I', index, 8, count)
    (cache / 'shader_index.bin').write_bytes(index)
    cases = Path(__file__).with_name('index_device_cases.inc').read_text(encoding='utf-8')
    return (cases.replace('@INDEX_CASES@', ',\n'.join(records)).replace('@INDEX_VERTEX@', vertex)
            .replace('@INDEX_FRAGMENT@', fragment)), shaders
