"""Replace native library and shell classes in a verified local base APK.

Retains the original manifest, modules, shader/driver libraries and resources.
Base-only launcher sources must be supplied with --retained-source-dir.
Rejects APKs whose manifest components have no packaged class definition.
Only packages; never installs, starts, stops or clears an app.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import re
import struct
import zipfile


def dex_classes(data):
    if not data.startswith(b'dex\n'):
        raise RuntimeError('Unsupported DEX header')
    count, strings = struct.unpack_from('<II', data, 56)
    names = []
    for i in range(count):
        pos = struct.unpack_from('<I', data, strings + i * 4)[0]
        while data[pos] & 128:
            pos += 1
        pos += 1
        # DEX strings use modified UTF-8; class descriptors are ASCII. Decode
        # other strings lossily, since they are not used for class identity.
        names.append(data[pos:data.index(b'\0', pos)].decode('utf-8', errors='replace'))
    _, types = struct.unpack_from('<II', data, 64)
    count, classes = struct.unpack_from('<II', data, 96)
    result = set()
    for i in range(count):
        type_id = struct.unpack_from('<I', data, classes + i * 32)[0]
        string_id = struct.unpack_from('<I', data, types + type_id * 4)[0]
        result.add(names[string_id])
    return result


def manifest_components(tree):
    package = re.search(r'A: package="([^"]+)"', tree).group(1)
    result, component, depth = set(), None, 0
    for line in tree.splitlines():
        indent = len(line) - len(line.lstrip())
        element = re.match(r'\s*E: (\S+)', line)
        if element:
            component = element.group(1)
            depth = indent
        if component not in ('application', 'activity', 'activity-alias', 'service', 'receiver', 'provider') or indent <= depth:
            continue
        key = 'targetActivity' if component == 'activity-alias' else 'name'
        match = re.search(r':'+key+r'\(0x[0-9a-f]+\)="([^"]+)"', line)
        if not match:
            continue
        name = match.group(1)
        if name.startswith('android.'):
            continue
        if name.startswith('.'):
            name = package + name
        elif '.' not in name:
            name = package + '.' + name
        result.add('L' + name.replace('.', '/') + ';')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-apk', type=Path, required=True)
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--jdk', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--retained-source-dir', type=Path,
                        help='Additional source root for base-only shell classes, e.g. the retained offline-installer worktree dm/android/src')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    for name in ('classes', 'dex'):
        (out / name).mkdir()
    sdk = Path(os.environ['LOCALAPPDATA']) / 'Android/Sdk'
    tools = sdk / 'build-tools/35.0.0'
    android = sdk / 'platforms/android-34/android.jar'
    env = dict(os.environ, JAVA_HOME=str(args.jdk))
    env['PATH'] = str(args.jdk / 'bin') + os.pathsep + env['PATH']
    report = {'passed': False}
    receipt = out / 'result.json'
    receipt.write_text(json.dumps(report) + '\n')

    def run(command, name):
        p = subprocess.run([str(x) for x in command], env=env, capture_output=True, timeout=90)
        (out / (name + '.log')).write_bytes(p.stdout + p.stderr)
        if p.returncode:
            raise RuntimeError((p.stdout + p.stderr).decode(errors='replace')[-2000:])
        return p.stdout.decode('utf-8', errors='replace')

    source_root = repo / 'dm/android/src'
    sources = sorted(source_root.rglob('*.java'))
    retained = []
    if args.retained_source_dir:
        present = {p.relative_to(source_root) for p in sources}
        retained = sorted(p for p in args.retained_source_dir.rglob('*.java')
                          if p.relative_to(args.retained_source_dir) not in present)
        sources += retained
    tree = run([tools / 'aapt2.exe', 'dump', 'xmltree', '--file', 'AndroidManifest.xml', args.base_apk], 'manifest')
    components = manifest_components(tree)
    run([args.jdk / 'bin/javac.exe', '--release', '8', '-encoding', 'UTF-8', '-cp', android,
         '-d', out / 'classes', *sources], 'javac')
    run([tools / 'd8.bat', '--min-api', '29', '--lib', android, '--output', out / 'dex',
         *sorted((out / 'classes').rglob('*.class'))], 'dex')
    native_name = 'lib/arm64-v8a/libd2r_mobile_android.so'
    with zipfile.ZipFile(args.base_apk) as src, zipfile.ZipFile(out / 'unsigned.apk', 'w') as dst:
        if native_name not in src.namelist() or 'classes.dex' not in src.namelist():
            raise RuntimeError('Base APK has no native game shell')
        preserved = {}
        for info in src.infolist():
            if info.filename.startswith('META-INF/'):
                continue
            if info.filename == native_name:
                data = args.native.read_bytes()
            elif info.filename == 'classes.dex':
                data = (out / 'dex/classes.dex').read_bytes()
            else:
                data = src.read(info.filename)
                preserved[info.filename] = hashlib.sha256(data).hexdigest()
            dst.writestr(info, data)
    with zipfile.ZipFile(out / 'unsigned.apk') as packaged:
        classes = set()
        for name in packaged.namelist():
            if re.fullmatch(r'classes\d*\.dex', name):
                definitions = dex_classes(packaged.read(name))
                if classes & definitions:
                    raise RuntimeError('Duplicate packaged DEX class definitions')
                classes |= definitions
        missing = components - classes
        if missing:
            raise RuntimeError('Manifest component classes missing from APK: ' + ', '.join(sorted(missing)) +
                               '; supply matching base shell sources with --retained-source-dir')
    run([tools / 'zipalign.exe', '-f', '-P', '16', '4', out / 'unsigned.apk', out / 'aligned.apk'], 'align')
    apk = out / 'd2r-attachment-menu.apk'
    run([tools / 'apksigner.bat', 'sign', '--ks', Path(os.environ['USERPROFILE']) / '.android/debug.keystore',
         '--ks-pass', 'pass:android', '--key-pass', 'pass:android', '--out', apk, out / 'aligned.apk'], 'sign')
    run([tools / 'apksigner.bat', 'verify', apk], 'signature-verify')
    run([tools / 'zipalign.exe', '-c', '-P', '16', '4', apk], 'alignment-verify')
    with zipfile.ZipFile(apk) as final:
        for name, digest in preserved.items():
            if hashlib.sha256(final.read(name)).hexdigest() != digest:
                raise RuntimeError('Preserved entry changed: ' + name)
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    report.update(passed=True, apk=str(apk), apk_sha256=sha(apk), native_sha256=sha(args.native),
                  base_apk_sha256=sha(args.base_apk), preserved_entries_sha256=preserved,
                  java_source_sha256={str(p.relative_to(repo)): sha(p) for p in sources if p not in retained},
                  retained_java_source_sha256={str(p.resolve()): sha(p) for p in retained},
                  manifest_component_classes=sorted(components), manifest_component_classes_verified=True,
                  signature_verified=True, zip_alignment_16k_verified=True)
    receipt.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ('passed', 'apk', 'apk_sha256', 'native_sha256')}))


if __name__ == '__main__':
    main()
