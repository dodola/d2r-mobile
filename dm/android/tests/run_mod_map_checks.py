"""Host-only MOD-loader/map-request fixtures, with guest functions mocked."""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--package-root', type=Path)
    parser.add_argument('--base-overlay', type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    result = dict(passed=False, original_game_executed=False, device_contacted=False, tests=[])
    (out / 'result.json').write_text(json.dumps(result))
    for test in ('mod_loader', 'map_reveal'):
        for mode, flags in [('normal', []), ('sanitized', ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie'])]:
            name = test + '-' + mode
            executable = out / name
            command = ['gcc', '-D_GNU_SOURCE', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                       '-Wno-misleading-indentation', '-Wno-format-truncation', '-pthread',
                       f'dm/android/tests/t_{test}.c', '-o', str(executable)] + flags
            build = subprocess.run(command, cwd=repo, capture_output=True, text=True, timeout=90)
            (out / (name + '-build.log')).write_text(build.stdout + build.stderr)
            build.check_returncode()
            command = [str(executable)]
            if test == 'mod_loader' and args.package_root and args.base_overlay:
                command += [str(args.package_root.resolve()), str(args.base_overlay.resolve())]
            run = subprocess.run(command, cwd=repo, capture_output=True, text=True, timeout=60)
            (out / (name + '.log')).write_text(run.stdout + run.stderr)
            run.check_returncode()
            fixture = json.loads(run.stdout.strip().splitlines()[-1])
            result['tests'].append(dict(name=name, **fixture))
            print(name, fixture['checks'], 'checks passed')
    result['passed'] = True
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
