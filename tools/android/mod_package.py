"""Build immutable Android CASC MOD packages from D2RMM output / loose data.

Requires matching extracted Switch RomFS and the installed Android touch overlay.
No game, device or D2RMM JavaScript is executed. See docs/ANDROID_MODS_2026-10-20.zh-CN.md.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'dm/tools'))
import casc_overlay as casc

BUILD = 'db0507f36b84200ac05f75d76ed5026f'
ARCHIVE = re.compile(r'\d{2}-\d{8}\.data')
MOD_ID = re.compile(r'[a-z0-9_-]{1,48}')


def digest(path, algorithm='sha1'):
    h = hashlib.new(algorithm)
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def logical_path(value):
    value = str(value).replace('\\', '/').lower()
    parts = value.split('/')
    if any(p in ('', '.', '..') or not re.fullmatch(r'[a-z0-9_ .()@+-]+', p) for p in parts):
        raise ValueError(f'unsupported or unsafe logical path: {value}')
    return value


def payload_files(root):
    if root.is_symlink() or not root.is_dir():
        raise ValueError('payload must be an ordinary directory')
    if (root / 'mod.js').exists():
        raise ValueError('D2RMM scripts need exported data; use --copy-mod only for static copyFile-only components')
    if (root / 'data').is_dir():
        root = root / 'data'
    elif root.suffix.lower() != '.mpq' and not any((root / n).is_dir() for n in ('hd', 'global', 'local')):
        candidates = list(root.glob('*.mpq/data'))
        if len(candidates) != 1:
            raise ValueError('expected data/, <name>.mpq/data/, or a data directory with hd/global/local')
        root = candidates[0]
    if root.is_symlink():
        raise ValueError('payload data directory must not be a symlink')
    result = {}
    for parent, directories, files in os.walk(root, followlinks=False):
        for name in directories + files:
            if (Path(parent) / name).is_symlink():
                raise ValueError('payload symlinks are not supported')
        for name in files:
            path = Path(parent) / name
            if not path.is_file():
                raise ValueError(f'not a regular payload file: {path}')
            rel = logical_path(path.relative_to(root).as_posix())
            if rel.split('/')[0] not in ('hd', 'global', 'local'):
                raise ValueError(f'not a game data path: {rel}')
            logical = 'data/' + rel
            if logical in result:
                raise ValueError(f'case-insensitive duplicate: {logical}')
            result[logical] = path
    if not result:
        raise ValueError('empty MOD payload')
    return result


def copy_component(source, destination):
    """Parse a deliberately small static subset; never evaluate mod.js."""
    script = (source / 'mod.js').read_text(encoding='utf-8-sig')
    tokens = re.compile(r'''"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|//[^\r\n]*|/\*[\s\S]*?\*/''')
    script = tokens.sub(lambda m: '' if m.group().startswith(('//', '/*')) else m.group(), script)
    call = re.compile(r'''\s*D2RMM\.copyFile\(\s*(['"])([^'"\r\n]+)\1\s*,\s*(['"])([^'"\r\n]+)\3\s*,\s*(true|false)\s*\)\s*;?''')
    at = 0
    while script[at:].strip():
        match = call.match(script, at)
        if not match:
            raise ValueError('component is not static copyFile-only; export it with D2RMM first')
        src_name = logical_path(match[2])
        dst_name = logical_path(match[4])
        # Source paths preserve case on Linux; destination paths normalize later.
        src = source / match[2].replace('\\', '/')
        if not src.exists() or not src.resolve().is_relative_to(source.resolve()):
            raise ValueError(f'missing/escaping copyFile source: {src_name}')
        dst = destination / dst_name
        candidates = [(src, dst)] if src.is_file() else [(p, dst / p.relative_to(src)) for p in src.rglob('*') if p.is_file()]
        if src.is_symlink() or (src.is_dir() and any(p.is_symlink() for p in src.rglob('*'))):
            raise ValueError('component symlinks are not supported')
        for original, target in candidates:
            if target.exists() and match[5] == 'false':
                raise ValueError('copyFile conflict without overwrite')
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(original, target)
        at = match.end()


def vfs_index(content):
    path_offset, path_size, files_offset, files_size, cft_offset, cft_size = struct.unpack('>IIIIII', content[12:36])
    width = casc.offset_field_size(cft_size)
    index = {}
    for _, name, value, folder, _ in casc.walk_paths(content, path_offset, path_offset + path_size):
        if folder is not None:
            continue
        entry = files_offset + value
        if entry + 9 + width > files_offset + files_size or content[entry] != 1:
            raise ValueError('unsupported multi-span TVFS file')
        span_offset = struct.unpack('>I', content[entry + 1:entry + 5])[0]
        pos = cft_offset + int.from_bytes(content[entry + 9:entry + 9 + width], 'big')
        if span_offset or pos + 25 > cft_offset + cft_size:
            raise ValueError('unsupported TVFS span bounds')
        logical = name.decode('utf-8').strip('/').lower()
        if logical in index:
            raise ValueError('duplicate TVFS logical path')
        index[logical] = pos
    return index


def build_package(romfs, base, files, output, mod_id, *, allow_ui=False, allow_tables=False, allow_additions=False):
    if not MOD_ID.fullmatch(mod_id):
        raise ValueError('MOD ID: 1..48 lowercase letters/digits/underscore/hyphen')
    if output.exists():
        raise ValueError('output already exists; use a new package directory')
    for logical in files:
        if logical.startswith('data/global/ui/layouts/') and not allow_ui:
            raise ValueError('UI layout conflicts with Android controls; review and use --allow-ui: ' + logical)
        if logical.startswith('data/global/excel/'):
            if not allow_tables:
                raise ValueError('table changes require Switch 94012-compatible BINs and --allow-tables: ' + logical)
            if logical.endswith('.txt') and logical[:-4] + '.bin' not in files:
                raise ValueError('TXT does not compile automatically; supply matching compiled BIN: ' + logical)
    base_config = base / 'data/.build.config'
    if not (base / 'ctrl_touch').is_file() or not base_config.is_file():
        raise ValueError('--base-overlay must be the installed complete Android touch overlay')
    for config in (romfs / 'data/.build.config', base_config):
        _, values = casc.config_values(config)
        if values.get('build-name') != '94012' or values.get('build-target-platform') != 'switch':
            raise ValueError('requires Switch build 94012 resources: ' + str(config))
    base_sha = digest(base_config)
    output.parent.mkdir(parents=True, exist_ok=True)
    # Hardlink immutable inputs instead of copying many GB or requiring Windows symlink privileges.
    with tempfile.TemporaryDirectory(prefix='d2r-mod-', dir=output.parent) as temporary:
        work = Path(temporary)
        merged = work / 'source/data'
        merged.mkdir(parents=True)
        inputs = {p.name: p for p in (romfs / 'data').iterdir() if p.is_file() and ARCHIVE.fullmatch(p.name)}
        inputs.update({p.name: p for p in (base / 'data').iterdir() if p.is_file() and ARCHIVE.fullmatch(p.name)})
        for name, original in inputs.items():
            os.link(original.resolve(), merged / name)
        shutil.copyfile(base_config, merged / '.build.config')
        vfs = casc.load_vfs1(merged.parent)
        index = vfs_index(vfs)
        missing = sorted(set(files) - index.keys())
        if missing and not allow_additions:
            raise ValueError('logical paths absent in Switch index; --allow-additions enables limited new-directory insertion: ' + ', '.join(missing[:10]))
        # The extraction PSV still contains stock keys. Generate current keys so
        # overwriting a path already changed by the touch overlay stays correct.
        rows = []
        for logical, pos in index.items():
            row = [''] * 11
            row[6] = bytes(vfs[pos:pos + 16]).hex()
            row[9] = logical
            rows.append('|'.join(row))
        (merged / 'build_manifest.psv').write_text('\n'.join(rows) + '\n', encoding='utf-8')
        packaged = work / 'package'
        (packaged / 'overlay/data').mkdir(parents=True)
        replacements = {('+' if name in missing else '') + name: path for name, path in sorted(files.items())}
        casc.rewrite_tvfs(merged.parent, packaged / 'overlay', replacements, link_unchanged=False)
        verification = verify_rewrite(vfs, packaged / 'overlay', files)
        emitted = sorted((packaged / 'overlay/data').iterdir())
        descriptor = ['d2r-mod-v1', 'build ' + BUILD, 'base ' + base_sha]
        receipt = dict(format=1, id=mod_id, build_id=BUILD, base_sha1=base_sha,
                       source='D2RMM exported data / static copyFile subset', original_game_executed=False,
                       changed_paths=sorted(files), allow_ui=allow_ui, allow_tables=allow_tables, files=[])
        receipt['verification'] = verification
        for path in emitted:
            rel = path.relative_to(packaged / 'overlay').as_posix()
            descriptor.append(f'file {digest(path)} {path.stat().st_size} {rel}')
            receipt['files'].append(dict(path='overlay/' + rel, size=path.stat().st_size, sha256=digest(path, 'sha256')))
        (packaged / 'package.txt').write_text('\n'.join(descriptor + ['end', '']), encoding='ascii', newline='\n')
        (packaged / 'package.json').write_text(json.dumps(receipt, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        packaged.rename(output)
    return receipt


def decode_entry(data_dir, key, encoded_size, spec):
    import zlib
    if spec.startswith('b:'):
        return casc.read_data_file(data_dir, key, encoded_size)
    archive = data_dir / f'{key[9]:02d}-{key[10]:08d}.data'
    with archive.open('rb') as stream:
        stream.seek(int.from_bytes(key[11:], 'big'))
        content = stream.read(encoded_size)
    return zlib.decompress(content) if spec == 'z' else content


def verify_rewrite(before, overlay, files):
    """Read generated TVFS and actual replacement bytes, not just output hashes."""
    after = casc.load_vfs1(overlay)
    old_index, new_index = vfs_index(before), vfs_index(after)
    if not old_index.keys() <= new_index.keys() or set(new_index) - old_index.keys() != files.keys() - old_index.keys():
        raise ValueError('rewrite changed unexpected logical paths')
    for logical, pos in old_index.items():
        if logical not in files and before[pos:pos + 25] != after[new_index[logical]:new_index[logical] + 25]:
            raise ValueError('untouched resource entry changed: ' + logical)
    est_offset, est_size = struct.unpack('>II', after[38:46])
    table = bytes(after[est_offset:est_offset + est_size])
    for logical, path in files.items():
        pos = new_index[logical]
        key = bytes(after[pos:pos + 16])
        size = struct.unpack('>I', after[pos + 16:pos + 20])[0]
        content = decode_entry(overlay / 'data', key, size, casc.encoding_spec(table, after[pos + 20]))
        if content != path.read_bytes():
            raise ValueError('generated replacement content mismatch: ' + logical)
    return dict(original_paths=len(old_index), untouched_entries_checked=len(old_index.keys() - files.keys()),
                replacement_bytes_checked=len(files), original_game_executed=False)


def verify_package(root):
    metadata = json.loads((root / 'package.json').read_text(encoding='utf-8'))
    if (metadata['format'] != 1 or metadata['build_id'] != BUILD or not MOD_ID.fullmatch(metadata['id'])
            or not re.fullmatch(r'[0-9a-f]{40}', metadata['base_sha1']) or not 2 <= len(metadata['files']) <= 32):
        raise ValueError('unsupported MOD package')
    descriptor = ['d2r-mod-v1', 'build ' + BUILD, 'base ' + metadata['base_sha1']]
    expected = {'package.json', 'package.txt'}
    for item in metadata['files']:
        rel = item['path']
        if rel != 'overlay/data/.build.config' and not re.fullmatch(r'overlay/data/\d{2}-\d{8}\.data', rel):
            raise ValueError('unsupported package file')
        path = root / rel
        if rel in expected or path.is_symlink() or not path.is_file() or path.stat().st_size != item['size'] or digest(path, 'sha256') != item['sha256']:
            raise ValueError('MOD checksum/size/duplicate mismatch: ' + rel)
        expected.add(rel)
        descriptor.append(f'file {digest(path)} {item["size"]} {rel[len("overlay/"):]}')
    if 'overlay/data/.build.config' not in expected or len(metadata['files']) < 2:
        raise ValueError('incomplete MOD package')
    actual = set()
    for parent, directories, files in os.walk(root):
        for name in directories + files:
            if (Path(parent) / name).is_symlink():
                raise ValueError('package symlinks are not supported')
        actual.update((Path(parent) / name).relative_to(root).as_posix() for name in files)
    if actual != expected or (root / 'package.txt').read_bytes() != ('\n'.join(descriptor + ['end', ''])).encode('ascii'):
        raise ValueError('unexpected package files or descriptor mismatch')
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--romfs', type=Path, required=True)
    parser.add_argument('--base-overlay', type=Path, required=True)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--data', type=Path, help='Exported <name>.mpq/data or loose game data')
    source.add_argument('--copy-mod', type=Path, help='D2RMM static copyFile-only component')
    parser.add_argument('--id', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--allow-ui', action='store_true')
    parser.add_argument('--allow-tables', action='store_true')
    parser.add_argument('--allow-additions', action='store_true')
    args = parser.parse_args()
    try:
        with tempfile.TemporaryDirectory(prefix='d2r-copy-mod-') as temporary:
            if args.copy_mod:
                data = Path(temporary) / 'data'
                data.mkdir()
                copy_component(args.copy_mod.resolve(), data)
            else:
                data = args.data
            receipt = build_package(args.romfs.resolve(), args.base_overlay.resolve(), payload_files(data),
                                    args.output.resolve(), args.id, allow_ui=args.allow_ui,
                                    allow_tables=args.allow_tables, allow_additions=args.allow_additions)
        verify_package(args.output.resolve())
        print(json.dumps(receipt, ensure_ascii=False, indent=2))
    except (ValueError, OSError, KeyError, struct.error) as error:
        parser.exit(1, f'mod package: {error}\n')


if __name__ == '__main__':
    main()
