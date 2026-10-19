"""Run production pipeline-cache corruption/invalidation checks on Linux/WSL."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    receipt = out / 'result.json'
    receipt.write_text('{"passed":false}\n')
    records = []
    for mode, extra in [('normal', ['-O2']), ('sanitized', ['-O1', '-fsanitize=address,undefined',
                         '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie'])]:
        binary = out / mode
        command = ['gcc', '-std=c11', '-g', '-Wall', '-Wextra', '-Werror',
                   '-Wno-misleading-indentation', *extra, 'dm/gfx/tests/t_pipeline_store.c', '-lz', '-o', str(binary)]
        for name, cmd in [('build', command), ('run', [str(binary)])]:
            p = subprocess.run(cmd, cwd=repo, capture_output=True, timeout=60)
            (out / f'{mode}-{name}.log').write_bytes(p.stdout + p.stderr)
            if p.returncode:
                print((p.stdout + p.stderr).decode(errors='replace'))
            p.check_returncode()
        records.append({'mode': mode, 'passed': True, 'stdout': p.stdout.decode().strip()})
        print(p.stdout.decode().strip())
    sources = ['dm/gfx/tests/t_pipeline_store.c', 'dm/gfx/nvn/nvn_vk_pipeline_store.h',
               'dm/android/tests/run_pipeline_checks.py']
    receipt.write_text(json.dumps({'passed': True, 'tests': records,
        'source_sha256': {s: hashlib.sha256((repo / s).read_bytes()).hexdigest() for s in sources}}, indent=2) + '\n')


if __name__ == '__main__':
    main()
