"""Install/activate prepared MOD packages on a stopped debuggable Android app.

No app install, process stop, save copying or automatic restart is performed.
"""
import argparse
from pathlib import Path
import shlex
import subprocess
import uuid

from debug_console import Device, ROOT
from mod_package import MOD_ID, verify_package, digest

MODS = ROOT.rsplit('/', 1)[0] + '/mods'


def install(device, package):
    metadata = verify_package(package)
    device.stopped()
    mod_id = metadata['id']
    stage = MODS + '/.install-' + uuid.uuid4().hex
    target = MODS + '/' + mod_id
    device.call(f'mkdir -p {MODS} && test ! -e {target} && mkdir {stage} && mkdir -p {stage}/overlay/data')
    paths = ['package.json', 'package.txt'] + [entry['path'] for entry in metadata['files']]
    for rel in paths:
        # Every relative name is verified by verify_package, including parents.
        script = f'cat > {stage}/{rel}'
        remote = shlex.join(['run-as', device.package, 'sh', '-c', script])
        with (package / rel).open('rb') as stream:
            proc = subprocess.run(device.prefix + ['shell', remote], stdin=stream, capture_output=True, timeout=600)
        if proc.returncode:
            raise RuntimeError('MOD upload failed; hidden staging directory retained: ' + stage)
        actual = device.call(f'sha1sum {stage}/{rel}').split()
        if not actual or actual[0] != digest(package / rel):
            raise RuntimeError('MOD upload checksum mismatch; staging retained: ' + stage)
    # Do not replace an existing package/save. Install a changed package under a new ID.
    device.stopped()
    device.call(f'test ! -e {target} && mv {stage} {target}')
    return mod_id


def activate(device, mod_id):
    if mod_id and not MOD_ID.fullmatch(mod_id):
        raise ValueError('invalid MOD ID')
    device.stopped()
    check = f'test -f {MODS}/{mod_id}/package.txt && ' if mod_id else ''
    device.call(f'mkdir -p {MODS} && {check}cat > {MODS}/active.tmp && mv {MODS}/active.tmp {MODS}/active.txt', mod_id + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', default='adb')
    parser.add_argument('--serial')
    parser.add_argument('--package', default='org.d2r.binaryprobe')
    actions = parser.add_subparsers(dest='action', required=True)
    add = actions.add_parser('install')
    add.add_argument('directory', type=Path)
    select = actions.add_parser('activate')
    select.add_argument('id')
    actions.add_parser('disable')
    actions.add_parser('status')
    args = parser.parse_args()
    try:
        device = Device(args.adb, args.serial, args.package)
        if args.action == 'install':
            print('Installed ' + install(device, args.directory.resolve()) + '; activate separately for next launch.')
        elif args.action == 'status':
            print(device.call(f'if [ -f {MODS}/active.txt ]; then cat {MODS}/active.txt; fi'))
        else:
            activate(device, args.id if args.action == 'activate' else '')
            print('MOD selection saved for next launch.')
        return 0
    except (RuntimeError, ValueError, OSError, KeyError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f'mod console: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
