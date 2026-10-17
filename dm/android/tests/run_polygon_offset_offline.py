"""Run the real NVN polygon-offset dispatcher before/after under AArch64 Unicorn.

Requires an Android NDK and Python's unicorn package. No phone, Vulkan driver,
game executable, or game resources are used. Example:
  python run_polygon_offset_offline.py --ndk /path/to/ndk --output /tmp/polygon
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys


SHIM = r'''#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
FILE *stderr;
const char *offline_stop;
void *memset(void *p,int c,size_t n){unsigned char *d=p;while(n--)*d++=(unsigned char)c;return p;}
void *memcpy(void *d,const void *s,size_t n){unsigned char *a=d;const unsigned char *b=s;while(n--)*a++=*b++;return d;}
int memcmp(const void *a,const void *b,size_t n){const unsigned char *x=a,*y=b;for(size_t i=0;i<n;i++)if(x[i]!=y[i])return x[i]-y[i];return 0;}
size_t strlen(const char *s){const char *p=s;while(*p)p++;return (size_t)(p-s);}
size_t strlcpy(char *d,const char *s,size_t n){size_t len=strlen(s);if(n){size_t take=len<n-1?len:n-1;memcpy(d,s,take);d[take]=0;}return len;}
int fprintf(FILE *f,const char *fmt,...){(void)f;if(fmt[0]=='['&&fmt[1]=='n'){va_list ap;va_start(ap,fmt);offline_stop=va_arg(ap,const char *);va_end(ap);}return 0;}
int fflush(FILE *f){(void)f;return 0;}
__attribute__((noreturn)) void abort(void){__asm__ volatile("brk #1");__builtin_unreachable();}
__attribute__((noreturn)) void __assert2(const char *f,int line,const char *fn,const char *expr){(void)f;(void)line;(void)fn;(void)expr;__asm__ volatile("brk #0");__builtin_unreachable();}
'''


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def elf_image(data):
    """Read ELF64 LE PT_LOAD and symbol tables using only the Python stdlib."""
    if data[:7] != b'\x7fELF\x02\x01\x01':
        raise ValueError('Expected a little-endian ELF64 file')
    kind, machine = struct.unpack_from('<HH', data, 16)
    if kind != 2 or machine != 183:
        raise ValueError('Expected a static AArch64 ET_EXEC file')
    entry, phoff, shoff = struct.unpack_from('<QQQ', data, 24)
    phsize, phnum, shsize, shnum = struct.unpack_from('<HHHH', data, 54)
    loads = []
    for index in range(phnum):
        ptype, flags, offset, address, _, filesz, memsz, _ = struct.unpack_from(
            '<IIQQQQQQ', data, phoff + index * phsize)
        if ptype == 3:
            raise ValueError('Dynamic interpreter is not allowed')
        if ptype == 1:
            if filesz > memsz or offset + filesz > len(data) or address < 4096:
                raise ValueError('Invalid load segment or mapped null page')
            loads.append((offset, address, filesz, memsz))
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shsize)
                for i in range(shnum)]
    symbols = {}
    undefined = []
    for section in sections:
        if section[1] != 2:  # SHT_SYMTAB
            continue
        strings = sections[section[6]]
        names = data[strings[4]:strings[4] + strings[5]]
        for offset in range(section[4], section[4] + section[5], section[9]):
            name, _, _, index, value, _ = struct.unpack_from('<IBBHQQ', data, offset)
            end = names.find(b'\0', name)
            label = names[name:end].decode('utf-8', errors='replace')
            if label:
                if index:
                    symbols[label] = value
                else:
                    undefined.append({'name': label, 'value': value})
    if not loads or entry != symbols.get('main'):
        raise ValueError('ELF entry must be main')
    if any(symbol['value'] for symbol in undefined):
        raise ValueError('Unresolved function targets must stay zero')
    return entry, loads, symbols, undefined


def replay(path, mode):
    from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM
    from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_X30, UC_ARM64_REG_W0, UC_ARM64_REG_PC
    data = path.read_bytes()
    entry, loads, symbols, undefined = elf_image(data)
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    low = min(address for _, address, _, _ in loads) & ~4095
    high = (max(address + size for _, address, _, size in loads) + 4095) & ~4095
    cpu.mem_map(low, high - low)
    for offset, address, size, _ in loads:
        cpu.mem_write(address, data[offset:offset + size])
    stack, returned = 0x7000000000, 0x40000000
    cpu.mem_map(stack, 0x10000)
    cpu.mem_map(returned, 0x1000)
    cpu.reg_write(UC_ARM64_REG_SP, stack + 0xfff0)
    cpu.reg_write(UC_ARM64_REG_X30, returned)
    error = None
    try:
        cpu.emu_start(entry, returned, count=2000000)
    except UcError as exc:
        error = str(exc)
    pc = cpu.reg_read(UC_ARM64_REG_PC)
    stop_pointer = struct.unpack('<Q', bytes(cpu.mem_read(symbols['offline_stop'], 8)))[0]
    stop = ''
    if stop_pointer:
        text = bytearray()
        for index in range(1024):
            value = cpu.mem_read(stop_pointer + index, 1)[0]
            if not value:
                break
            text.append(value)
        stop = text.decode('utf-8', errors='replace')
    exit_code = cpu.reg_read(UC_ARM64_REG_W0)
    if mode == 'before':
        passed = (error is not None and pc == symbols['abort'] and
                  stop == 'nonzero polygon offset requires Vulkan pipeline bias')
    else:
        passed = error is None and pc == returned and exit_code == 0 and not stop
    return {'mode': mode, 'passed': passed, 'stop_message': stop, 'error': error,
            'pc': hex(pc), 'exit_code': exit_code, 'elf_sha256': digest(path),
            'unresolved_symbols': undefined, 'null_page_mapped': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk', type=Path, required=True)
    parser.add_argument('--unicorn-dir', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--before-ref', default='b0f012c')
    args = parser.parse_args()
    if args.unicorn_dir:
        sys.path.insert(0, str(args.unicorn_dir.resolve()))
    import unicorn
    repo = Path(__file__).resolve().parents[3]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    report_path = out / 'polygon-regression.json'
    report = {'passed': False, 'device_contacted': False, 'gpu_executed': False,
              'issue_1_root_cause_proven': False, 'before_ref': args.before_ref,
              'unicorn_version': unicorn.__version__, 'tests': [],
              'scope': 'Production execute dispatcher plus effective depth bias and PSO key assertions under AArch64 emulation.',
              'limitations': ['No Vulkan call is simulated or executed; no device/scene evidence.',
                              'Single-thread CPU semantics only; no concurrency or driver validation.',
                              'Only nvn_vk.c comes from before-ref; local headers and the fixture are current.',
                              'Unused replay functions retain unresolved target zero; null page is unmapped, so reaching them fails.']}
    report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    try:
        prebuilts = args.ndk.resolve() / 'toolchains/llvm/prebuilt'
        compilers = sorted(prebuilts.glob('*/bin/clang.exe')) + sorted(prebuilts.glob('*/bin/clang'))
        if len(compilers) != 1:
            raise ValueError('Expected one NDK host clang: ' + str(prebuilts))
        clang = compilers[0]
        report['compiler_sha256'] = digest(clang)
        report['compiler_version'] = subprocess.check_output([str(clang), '--version'], text=True)
        before = subprocess.check_output(['git', 'show', args.before_ref + ':dm/gfx/nvn/nvn_vk.c'], cwd=repo).decode('utf-8')
        report['before_commit'] = subprocess.check_output(['git', 'rev-parse', args.before_ref], cwd=repo, text=True).strip()
        source_dir = repo / 'dm/gfx/nvn'
        def absolute_include(match):
            for directory in [source_dir, repo / 'dm/android', repo / 'dm/host', source_dir / 'include']:
                path = (directory / match[2]).resolve()
                if path.is_file():
                    return match[1] + '"' + path.as_posix() + '"'
            raise ValueError('Cannot resolve before-source local include: ' + match[2])
        before = re.sub(r'(?m)^(\s*#include\s+)"([^"]+)"',
                        absolute_include, before)
        old_source, old_test, shim = out / 'before-nvn_vk.c', out / 'before-test.c', out / 'offline-shim.c'
        old_source.write_text(before, encoding='utf-8')
        fixture = repo / 'dm/android/tests/t_polygon_offset.c'
        old_test.write_text(fixture.read_text(encoding='utf-8').replace(
            '#include "../../gfx/nvn/nvn_vk.c"', '#include "' + old_source.as_posix() + '"'), encoding='utf-8')
        shim.write_text(SHIM, encoding='utf-8')
        hashes = {path.relative_to(repo).as_posix(): digest(path) for path in
                  [Path(__file__).resolve(), fixture, source_dir / 'nvn_vk.c']}
        report['generated_sha256'] = {path.name: digest(path) for path in [old_source, old_test, shim]}
        for mode, test in [('before', old_test), ('after', fixture)]:
            elf, dep = out / ('polygon-' + mode + '.elf'), out / (mode + '.d')
            command = [str(clang), '--target=aarch64-linux-android29', '-O1', '-std=c11',
                       '-D_GNU_SOURCE', '-DTARGET_OS_IPHONE=0', '-ffixed-x18', '-fno-builtin',
                       '-fno-stack-protector', '-ffunction-sections', '-fdata-sections',
                       '-Wno-format', '-Wno-unused-function', '-nostdlib', '-static',
                       '-Wl,-e,main', '-Wl,--gc-sections', '-Wl,--unresolved-symbols=ignore-all',
                       '-I' + str(repo / 'dm/host'), '-I' + str(repo / 'dm/android'),
                       '-I' + str(source_dir), '-I' + str(source_dir / 'include')]
            if mode == 'before':
                command.append('-DEXPECT_OLD_ABORT')
            # Compile fixture alone for dependency receipts, then link with the tiny shim.
            obj = out / (mode + '.o')
            compile_command = command[:]
            for flag in ['-nostdlib', '-static', '-Wl,-e,main', '-Wl,--gc-sections', '-Wl,--unresolved-symbols=ignore-all']:
                compile_command.remove(flag)
            commands = [compile_command + ['-MMD', '-MF', str(dep), '-MT', 'fixture', '-c', str(test), '-o', str(obj)],
                        command + [str(obj), str(shim), '-o', str(elf)]]
            for index, invocation in enumerate(commands):
                process = subprocess.run(invocation, cwd=repo, capture_output=True, text=True)
                (out / (mode + '-' + str(index) + '-build.log')).write_text(process.stdout + process.stderr, encoding='utf-8')
                process.check_returncode()
            dependencies = dep.read_text(encoding='utf-8').replace('\\\n', ' ').split(': ', 1)[1]
            for token in re.findall(r'(?:\\.|[^\s])+', dependencies):
                path = Path(token.replace('\\ ', ' '))
                if not path.is_absolute():
                    path = repo / path
                path = path.resolve()
                if path.is_file() and path.is_relative_to(repo):
                    hashes[path.relative_to(repo).as_posix()] = digest(path)
            result = replay(elf, mode)
            result['build_commands'] = commands
            report['tests'].append(result)
            print(json.dumps({key: result[key] for key in ['mode', 'passed', 'stop_message', 'error', 'pc']}), flush=True)
        report['source_sha256'] = hashes
        report['passed'] = all(test['passed'] for test in report['tests'])
        if not report['passed']:
            raise RuntimeError('Polygon-offset regression failed; see report')
    except Exception as exc:
        report['failure'] = str(exc)
        raise
    finally:
        report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
