"""Rebuild an existing Android CMake tree and record native source provenance.

Requires the local generated romfs_samples.h used by game_resource_probe.c.
Does not package, install or start the app. Rejects a tree for another checkout.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
from datetime import datetime, timezone


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    repo = Path(__file__).resolve().parents[2]
    build = a.build.resolve()
    cache = (build / 'CMakeCache.txt').read_text()
    expected = (repo / 'dm/android').resolve().as_posix()
    if 'CMAKE_HOME_DIRECTORY:INTERNAL=' + expected not in cache.replace('\\', '/'):
        raise RuntimeError('CMake tree belongs to a different checkout')
    sdk = Path(os.environ['LOCALAPPDATA']) / 'Android/Sdk'
    cmake = sdk / 'cmake/3.22.1/bin/cmake.exe'
    a.output.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run([str(cmake), '--build', str(build), '-j', '6'], cwd=repo, capture_output=True)
    a.output.with_suffix('.build.log').write_bytes(result.stdout + result.stderr)
    result.check_returncode()
    commands = json.loads((build / 'compile_commands.json').read_text())
    sources = {Path(row['file']).resolve() for row in commands}
    for directory in ('dm/android', 'dm/gfx/nvn', 'dm/gfx/tex', 'dm/host'):
        for suffix in ('*.h', '*.inc'):
            sources.update((repo / directory).glob(suffix))
    sources.update([repo / 'dm/android/CMakeLists.txt', repo / 'dm/ios/Sources/dm_touch_controls.h'])
    sources.update(p for p in (repo / 'dm/gfx/nvn/shaders').glob('*') if p.is_file())
    native = build / 'libd2r_mobile_android.so'
    report = dict(built_at=datetime.now(timezone.utc).isoformat(), repository=str(repo),
                  commit=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=repo, text=True).strip(),
                  git_status=subprocess.check_output(['git', 'status', '--porcelain'], cwd=repo, text=True),
                  compiled_sources_and_headers={str(s.relative_to(repo)): sha(s) for s in sorted(sources)},
                  generated_resource_samples_sha256=sha(build / 'generated/romfs_samples.h'),
                  cmake_cache_sha256=sha(build / 'CMakeCache.txt'),
                  outputs={native.name: dict(path=str(native), bytes=native.stat().st_size, sha256=sha(native))},
                  full_game_verified=False)
    a.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report['outputs'], indent=2))


if __name__ == '__main__':
    main()
