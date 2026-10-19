"""Build the pinned, streamable game 94012 offline resource pack paired with the APK."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import os

ROOT = Path(os.environ.get('ANDROID_SWITCH_WORKSPACE', str(Path(__file__).resolve().parents[2]))).resolve()
ART = ROOT / 'analysis/d2r-mobile-port'
INPUTS = ROOT / 'analysis/parity-20261019/d2r-new'
DEFAULT_OUTPUT = ART / 'release-94012/pack'
GAME_BUILD = 94012
PACK_NAME = 'D2R-Offline-94012.d2rpack'
MAGIC = b'D2RPKG1\n'
ROMFS_SHA = 'f5f3bcfbf95abef4ca778f6fa141f54d5ff1028a7f50e25ee0984acf31f12785'
ROMFS_BYTES = 8973550584
CHUNK = 4 * 1024 * 1024


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(CHUNK), b''):
            h.update(block)
    return h.hexdigest()


def sources(image, cache):
    yield 'game.romfs', image
    paths = sorted(p for p in cache.iterdir() if re.fullmatch(r'(?:[0-9a-f]{16}\.(?:spv\.gz|vkmeta)|shader_index\.bin)', p.name))
    if not paths or not (cache / 'shader_index.bin').is_file():
        raise ValueError('Missing shader cache')
    for path in paths:
        if path.suffix == '.vkmeta' and not (cache / (path.stem + '.spv.gz')).is_file():
            raise ValueError('Shader metadata has no payload: ' + path.name)
        if path.name.endswith('.spv.gz') and not (cache / (path.name[:-7] + '.vkmeta')).is_file():
            raise ValueError('Shader payload has no metadata: ' + path.name)
        yield 'd2r-mobile/shaders/' + path.name, path


def recorded_checksums(path):
    # sha256sum-format list of every shader cache file, written when the cache was prepared.
    recorded = {}
    for line in path.read_text(encoding='utf-8').splitlines():
        digest, _, name = line.partition('  ')
        if not re.fullmatch('[0-9a-f]{64}', digest) or not name or name in recorded:
            raise ValueError('Malformed shader cache checksum list')
        recorded[name] = digest
    return recorded


def verify(pack, manifest):
    h = hashlib.sha256()
    with pack.open('rb') as stream:
        def read(count):
            data = stream.read(count)
            if len(data) != count:
                raise ValueError('Truncated resource pack')
            h.update(data)
            return data
        if read(8) != MAGIC:
            raise ValueError('Resource pack magic mismatch')
        size = struct.unpack('>I', read(4))[0]
        if size != len(manifest) or read(size) != manifest:
            raise ValueError('Resource pack manifest mismatch')
        for entry in json.loads(manifest)['files']:
            remaining = entry['bytes']
            digest = hashlib.sha256()
            while remaining:
                data = read(min(CHUNK, remaining))
                digest.update(data)
                remaining -= len(data)
            if digest.hexdigest() != entry['sha256']:
                raise ValueError('Resource checksum mismatch: ' + entry['path'])
        if stream.read(1):
            raise ValueError('Trailing resource pack data')
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=INPUTS / 'romfs.bin')
    parser.add_argument('--cache', type=Path, default=INPUTS / 'shader-full-cache/cache')
    parser.add_argument('--checksums', type=Path, default=INPUTS / 'shader-full-cache/cache-checksums.sha256',
                        help='Recorded per-file hashes of the prepared shader cache')
    parser.add_argument('--output', type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument('--verify-only', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    pack = args.output / PACK_NAME
    manifest_path = args.output / 'offline-manifest.json'
    if args.verify_only:
        print(json.dumps({'sha256': verify(pack, manifest_path.read_bytes())}))
        return
    paths = list(sources(args.image, args.cache))
    print('Hashing original game and shader cache...', flush=True)
    entries = [{'path': name, 'bytes': path.stat().st_size, 'sha256': sha(path)} for name, path in paths]
    if entries[0]['bytes'] != ROMFS_BYTES or entries[0]['sha256'] != ROMFS_SHA:
        raise ValueError(f'The game image is not the pinned original build {GAME_BUILD}')
    recorded = recorded_checksums(args.checksums)
    cached = {name.rsplit('/', 1)[1]: e['sha256'] for name, e in ((e['path'], e) for e in entries[1:])}
    if cached != recorded:
        raise ValueError('Shader cache differs from its recorded checksum list: %s' %
                         sorted(set(cached.items()) ^ set(recorded.items()))[:4])
    data = {'format': 1, 'game_build': GAME_BUILD, 'total_bytes': sum(e['bytes'] for e in entries), 'files': entries}
    manifest = json.dumps(data, ensure_ascii=True, separators=(',', ':')).encode('utf-8')
    if len(manifest) > 4 * 1024 * 1024:
        raise ValueError('Manifest exceeds installer limit')
    temporary = pack.with_suffix('.d2rpack.partial')
    print(f'Writing {data["total_bytes"]} bytes / {len(entries)} files...', flush=True)
    with temporary.open('wb') as out:
        out.write(MAGIC + struct.pack('>I', len(manifest)) + manifest)
        for entry, (_, path) in zip(entries, paths):
            digest = hashlib.sha256()
            count = 0
            with path.open('rb') as stream:
                for block in iter(lambda: stream.read(CHUNK), b''):
                    out.write(block)
                    digest.update(block)
                    count += len(block)
            if count != entry['bytes'] or digest.hexdigest() != entry['sha256']:
                raise ValueError('Resource changed during packaging: ' + entry['path'])
        out.flush()
        os.fsync(out.fileno())
    print('Independently verifying the completed pack...', flush=True)
    digest = verify(temporary, manifest)
    temporary.replace(pack)
    manifest_path.write_bytes(manifest)
    report = {'pack': str(pack), 'bytes': pack.stat().st_size, 'sha256': digest,
              'manifest_sha256': hashlib.sha256(manifest).hexdigest(), 'files': len(entries),
              'payload_bytes': data['total_bytes'], 'game_build': GAME_BUILD, 'fully_verified': True,
              'shader_cache': str(args.cache.resolve()), 'shader_cache_checksums_verified': True,
              'game_romfs_sha256': ROMFS_SHA, 'game_playability_verified_by_packaging': False}
    (args.output / 'offline-pack-build.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False), flush=True)


if __name__ == '__main__':
    main()
