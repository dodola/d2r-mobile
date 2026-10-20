"""Host/WSL QA-console fixtures. Never contacts a device or invokes the game."""
import argparse
import json
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    repo = Path(__file__).resolve().parents[3]
    result = {'passed': False, 'device_contacted': False, 'original_game_executed': False, 'tests': []}
    report = out / 'result.json'
    report.write_text(json.dumps(result))
    for name, flags in [('normal', []), ('sanitized', ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie'])]:
        exe = out / name
        command = ['gcc', '-D_GNU_SOURCE', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                   '-Wno-misleading-indentation', '-pthread', 'dm/android/tests/t_debug_mode.c', '-o', str(exe)] + flags
        build = subprocess.run(command, cwd=repo, capture_output=True, text=True, timeout=120)
        (out / (name + '-build.log')).write_text(build.stdout + build.stderr)
        build.check_returncode()
        run = subprocess.run([str(exe)], cwd=repo, capture_output=True, text=True, timeout=60)
        (out / (name + '.log')).write_text(run.stdout + run.stderr)
        run.check_returncode()
        fixture = json.loads(run.stdout.strip().splitlines()[-1])
        receipt = json.loads(Path(fixture['receipt']).read_text())
        assert receipt['state'] == 'cancelled' and receipt['save'] == 'debug/save'
        result['tests'].append({'name': name, **fixture, 'final_receipt': receipt})
        print(name, fixture['checks'], 'checks passed')
    result['passed'] = True
    report.write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
