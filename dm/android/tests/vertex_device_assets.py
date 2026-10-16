"""Authored SPIR-V and independent expected vertex values for device readback."""
import gzip
import hashlib
from pathlib import Path
import struct


def prepare(cache, out, ndk, run):
    index = bytearray((cache / 'shader_index.bin').read_bytes())
    count = struct.unpack_from('<I', index, 8)[0]
    compiled = {}

    def shader(stage, glsl, attrs):
        nonlocal count
        key = hashlib.sha256(glsl.encode()).hexdigest()[:16]
        if key in compiled:
            return key
        source = out / (key + '.' + stage)
        source.write_text(glsl, encoding='utf-8')
        spv = out / (key + '.spv')
        run([ndk / 'shader-tools/windows-x86_64/glslc.exe', '--target-env=vulkan1.1', '-O0', source, '-o', spv], key + '-compile.log')
        run([ndk / 'shader-tools/windows-x86_64/spirv-val.exe', '--target-env', 'vulkan1.1', spv], key + '-validate.log')
        model = int(stage == 'frag')
        module = spv.read_bytes()
        (cache / (key + '.spv.gz')).write_bytes(gzip.compress(module, mtime=0))
        meta = struct.pack('<12I', 0x4d4b5644, 1, model, 0, len(attrs), 0, 1, 0, 0, 0, model, len(module))
        meta += b''.join(struct.pack('<2I', loc, 0) for loc in attrs) + struct.pack('<I', 1)
        (cache / (key + '.vkmeta')).write_bytes(meta)
        index.extend(key.encode() + struct.pack('<I', model) + bytes.fromhex(key) * (0x880 // 8))
        count += 1
        compiled[key] = dict(stage=stage, glsl_sha256=hashlib.sha256(glsl.encode()).hexdigest())
        return key

    fragment = shader('frag', '#version 450\nlayout(location=0) flat in vec4 value;layout(location=0) out vec4 color;void main(){color=value;}\n', [])
    records = []
    specs = []
    for fmt in list(range(1, 0x31)) + list(range(0x51, 0x69)):
        scaled = fmt >= 0x51
        group = (fmt - 0x51) // 6 if scaled else (fmt - 1) // 12
        kind = ((2, 7, 10)[(fmt - 0x51) % 6 // 2] + ((fmt - 0x51) & 1)) if scaled else (fmt - 1) % 12
        components = group + 1
        width = (1, 1, 1, 1, 2, 2, 2, 2, 2, 4, 4, 4)[kind]
        signed = kind in (1, 3, 6, 8, 11)
        integer = kind in (2, 3, 7, 8, 10, 11)
        floating = kind in (4, 9)
        shader_kind = 0 if scaled or not integer else 2 if signed else 1
        if floating:
            values = [-0.5, 0.25, 0.75, 1.0]
            packing = 'e' if width == 2 else 'f'
        else:
            values = {1: [64, 32, 96, 127], 2: [12000, 8000, 24000, 32767],
                      4: [16777217, 20000000, 30000000, 40000000]}[width]
            if signed:
                values = [-values[0], values[1], -values[2], values[3]]
            packing = {1: 'b' if signed else 'B', 2: 'h' if signed else 'H', 4: 'i' if signed else 'I'}[width]
        payload = struct.pack('<' + packing * components, *values[:components])
        decoded = list(values[:components])
        if not floating and not integer:
            divisor = (1 << (width * 8 - int(signed))) - 1
            decoded = [max(-1.0, x / divisor) for x in decoded]
        decoded += [0] * (3 - components) + ([1] if components < 4 else [])
        specs.append((fmt, payload, decoded, shader_kind))
    for fmt in (0x3d, 0x3e, 0x69, 0x6a, 0x6b, 0x6c):
        signed = fmt in (0x69, 0x6a, 0x6c)
        values = [-512, 127, -255, -1] if signed else [511, 300, 700, 3]
        word = sum((x & ((1 << bits)-1)) << shift for x, bits, shift in zip(values, (10, 10, 10, 2), (0, 10, 20, 30)))
        decoded = list(values)
        if fmt in (0x3d, 0x69):
            decoded = [max(-1.0, x / ((1 << (bits-int(signed)))-1)) for x, bits in zip(values, (10, 10, 10, 2))]
        shader_kind = 1 if fmt == 0x3e else 2 if fmt == 0x6a else 0
        specs.append((fmt, struct.pack('<I', word), decoded, shader_kind))
    specs.append((0x3f, struct.pack('<I', (14 << 6) | ((15 << 6) << 11) | ((16 << 5) << 22)), [0.5, 1.0, 2.0, 1.0], 0))
    for fmt, payload, decoded, shader_kind in specs:
        for inputs in range(1, 5):
            scalar = ('float', 'uint', 'int')[shader_kind]
            typ = scalar if inputs == 1 else ('vec', 'uvec', 'ivec')[shader_kind] + str(inputs)
            body = ('value.x=float(data);' if inputs == 1 else
                    f'{typ} direct=data;for(int j=0;j<{inputs};j++)value[j]=float(direct[j]);int k=gl_VertexIndex%{inputs};value[k]=float(data[k]);')
            glsl = ('#version 450\nlayout(location=2) in ' + typ + ' data;layout(location=0) flat out vec4 value;'
                    'void main(){vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));'
                    'gl_Position=vec4(p[gl_VertexIndex%3],.5,1);value=vec4(0,0,0,1);' + body + '}\n')
            key = shader('vert', glsl, [2])
            expected = decoded[:inputs] + [0] * (3 - inputs) + ([1] if inputs < 4 else [])
            bits = struct.unpack('<4I', struct.pack('<4f', *expected))
            records.append('{%d,%d,"%s",{%s},{%s}}' %
                           (fmt, len(payload), key, ','.join(str(x) for x in payload + bytes(16-len(payload))),
                            ','.join(str(x) + 'u' for x in bits)))
    struct.pack_into('<I', index, 8, count)
    (cache / 'shader_index.bin').write_bytes(index)
    template = Path(__file__).with_name('vertex_device_cases.inc').read_text(encoding='utf-8')
    return template.replace('@VERTEX_CASES@', ',\n'.join(records)).replace('@FRAGMENT@', fragment), compiled
