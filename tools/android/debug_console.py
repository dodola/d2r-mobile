"""D2R Android QA console. Requires DM_ANDROID_DEBUG_CONSOLE=ON.

Commands use debug/save. Never installs/starts/stops an app, copies saves,
retries mutations, or opens a network listener.
"""
import argparse
import json
import re
import shlex
import subprocess
import sys
import time
import uuid

ROOT = 'files/d2r-mobile/debug'
TERMINAL = {'ok', 'partial', 'failed', 'rejected', 'cancelled', 'timeout', 'io_error'}


def envelope(command, run, request):
    if not re.fullmatch(r'[0-9a-f]{1,16}', run):
        raise ValueError('invalid run token')
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,40}', request):
        raise ValueError('invalid request ID')
    if not command or any(not re.fullmatch(r'[A-Za-z0-9_-]+', part) for part in command):
        raise ValueError('commands must be ASCII tokens without shell syntax')
    line = f'{request} {run} {" ".join(command)}\n'
    if len(line.encode('ascii')) >= 256:
        raise ValueError('request is too long')
    return line


class Device:
    def __init__(self, adb, serial, package):
        if not re.fullmatch(r'[A-Za-z0-9_]+(?:\.[A-Za-z0-9_]+)+', package):
            raise ValueError('invalid Android package name')
        self.prefix = [adb] + (['-s', serial] if serial else [])
        self.package = package

    def call(self, script, data=None):
        # adb shell uses a remote shell even with subprocess shell=False.
        # Quote every remote argument; command bytes go through stdin only.
        remote = shlex.join(['run-as', self.package, 'sh', '-c', script])
        proc = subprocess.run(self.prefix + ['shell', remote], input=data,
                              capture_output=True, text=True, timeout=15)
        if proc.returncode:
            raise RuntimeError(proc.stderr.strip() or proc.stdout.strip() or f'adb exit {proc.returncode}')
        return proc.stdout

    def stopped(self):
        proc = subprocess.run(self.prefix + ['shell', 'pidof ' + shlex.quote(self.package)],
                              capture_output=True, text=True, timeout=15)
        if proc.returncode not in (0, 1):
            raise RuntimeError(proc.stderr.strip() or 'cannot query app process')
        if proc.stdout.strip():
            raise RuntimeError('Exit the game normally and stop the app before changing debug mode.')

    def mode(self, enabled):
        self.stopped()
        script = f'mkdir -p {ROOT} && '
        script += f'touch {ROOT}/enable' if enabled else f'rm -f {ROOT}/enable'
        script += f' && rm -f {ROOT}/commands.txt {ROOT}/status.json'
        self.call(script)

    def status(self):
        return json.loads(self.call(f'cat {ROOT}/status.json'))

    def submit(self, line):
        script = f'''mkdir {ROOT}/submit.lock || exit 75
trap 'rmdir {ROOT}/submit.lock' EXIT
if [ -e {ROOT}/commands.txt ] || [ -e {ROOT}/processing.txt ]; then exit 75; fi
cat > {ROOT}/request.tmp && mv {ROOT}/request.tmp {ROOT}/commands.txt'''
        self.call(script, line)


def send(device, command, timeout=45):
    state = device.status()
    if not state.get('enabled') or not state.get('attached') or not state.get('ready'):
        raise RuntimeError('Console is not ready. Enter a live test character and close panels/loading screens.')
    request = uuid.uuid4().hex
    device.submit(envelope(command, state['run'], request))
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        try:
            state = device.status()
        except (RuntimeError, ValueError, OSError, subprocess.TimeoutExpired) as error:
            raise RuntimeError(f'Request {request}: receipt unavailable; outcome unknown. Do not automatically retry. {error}') from error
        if state.get('request') == request and state.get('state') in TERMINAL:
            return state
        time.sleep(0.25)
    raise RuntimeError(f'Request {request} timed out waiting for a receipt. Outcome unknown; do not automatically retry.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', default='adb')
    parser.add_argument('--serial')
    parser.add_argument('--package', default='org.d2r.binaryprobe', help='Installed debuggable QA package')
    actions = parser.add_subparsers(dest='action', required=True)
    for name in ('enable', 'disable', 'status'):
        actions.add_parser(name)
    execute = actions.add_parser('exec')
    execute.add_argument('command', nargs='+', help='give r30 2 1 inventory; scene lut; heal; gold 10000')
    args = parser.parse_args()
    try:
        device = Device(args.adb, args.serial, args.package)
        if args.action in ('enable', 'disable'):
            device.mode(args.action == 'enable')
            print('Debug marker ' + ('enabled; next launch uses debug/save.' if args.action == 'enable' else 'disabled; next launch uses normal saves.'))
            return 0
        state = send(device, args.command) if args.action == 'exec' else device.status()
        print(json.dumps(state, ensure_ascii=False, indent=2))
        return 0 if args.action == 'status' or state['state'] == 'ok' else 1
    except (RuntimeError, ValueError, OSError, subprocess.TimeoutExpired) as error:
        print(f'debug console: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
