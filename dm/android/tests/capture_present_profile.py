"""Read-only capture of actual SurfaceFlinger presentation times and renderer logs.

Does not install, restart, send input, change settings or clear display statistics.
The scene label describes the operator's observation; it does not verify game state.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import time

PKG = 'org.d2r.binaryprobe'
LOG = 'files/d2r-mobile/diagnostics/game-startup.log'


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int((len(ordered) - 1) * fraction))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=60)
    parser.add_argument('--scene', required=True)
    args = parser.parse_args()
    if args.seconds < 30:
        parser.error('Capture at least 30 seconds')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    adb = Path(os.environ['LOCALAPPDATA']) / 'Android/Sdk/platform-tools/adb.exe'
    command = [str(adb), '-s', args.serial]

    def run(*parts):
        result = subprocess.run([*command, *parts], capture_output=True, timeout=15)
        if result.returncode:
            raise RuntimeError((result.stdout + result.stderr).decode(errors='replace')[-1000:])
        return result.stdout

    def private(*parts):
        return run('exec-out', 'run-as', PKG, *parts)

    report = dict(completed=False, serial=args.serial, scene_label=args.scene,
                  scene_identity_verified=False, input_sent=False, settings_changed=False,
                  tool_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  started_utc=datetime.now(timezone.utc).isoformat())
    receipt = out / 'result.json'

    def save():
        receipt.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')

    save()
    try:
        pid = run('shell', 'pidof', PKG).decode().strip()
        if not pid.isdecimal():
            raise RuntimeError('Expected one running app process')
        report['pid'] = int(pid)
        listing = run('shell', 'dumpsys', 'SurfaceFlinger', '--list').decode()
        (out / 'layers.txt').write_text(listing, encoding='utf-8')
        layers = [line.removeprefix('RequestedLayerState{').split(' parentId=')[0].rstrip('}')
                  for line in listing.splitlines() if 'SurfaceView[' + PKG in line and '](BLAST)#' in line]
        if len(layers) != 1:
            raise RuntimeError('Expected one game SurfaceView: ' + repr(layers))
        layer = layers[0]
        report['surface_layer'] = layer

        def display():
            raw = run('shell', 'dumpsys', 'SurfaceFlinger', '--latency', '"' + layer + '"').decode()
            lines = raw.splitlines()
            if not lines or not lines[0].isdecimal() or int(lines[0]) <= 0:
                raise RuntimeError('Missing display refresh period')
            rows = []
            for line in lines[1:]:
                fields = line.split()
                if len(fields) == 3 and all(x.isdecimal() for x in fields):
                    values = tuple(map(int, fields))
                    if min(values) > 0 and max(values) < (1 << 63) - 1:
                        rows.append(values)
            if not rows:
                raise RuntimeError('No valid actual presentation timestamps')
            return int(lines[0]), rows

        refresh, initial = display()
        cutoff = max(row[1] for row in initial)
        previous_latest = cutoff
        offset = int(private('stat', '-c', '%s', LOG))
        (out / 'startup-header.log').write_bytes(private('head', '-c', '160000', LOG))
        samples, states, frames = [], [], {}
        next_state = 0
        def append_log(log_file):
            nonlocal offset
            size = int(private('stat', '-c', '%s', LOG))
            if size < offset or size - offset > 8 * 1024 * 1024:
                raise RuntimeError('Log truncated or grew beyond bounded interval')
            if size > offset:
                data = private('sh', '-c', f'tail -c +{offset + 1} {LOG} | head -c {size - offset}')
                if len(data) != size - offset:
                    raise RuntimeError('Short renderer log read')
                log_file.write(data)
                log_file.flush()
                offset = size

        deadline = time.monotonic() + args.seconds
        with (out / 'display-samples.jsonl').open('w', encoding='utf-8') as sf, \
                (out / 'state-samples.jsonl').open('w', encoding='utf-8') as state_file, \
                (out / 'interval.log').open('wb') as log_file:
            while time.monotonic() < deadline:
                refresh, rows = display()
                if min(row[1] for row in rows) > previous_latest:
                    raise RuntimeError('Display history wrapped between polls; FPS would be incomplete')
                previous_latest = max(previous_latest, max(row[1] for row in rows))
                samples.append(refresh)
                for row in rows:
                    if row[1] > cutoff:
                        frames[row[1]] = row
                sf.write(json.dumps(dict(host_utc=datetime.now(timezone.utc).isoformat(),
                                         refresh_ns=refresh, rows=rows)) + '\n')
                sf.flush()
                if time.monotonic() >= next_state:
                    if run('shell', 'pidof', PKG).decode().strip() != pid:
                        raise RuntimeError('App process changed during capture')
                    battery = run('shell', 'dumpsys', 'battery').decode()
                    thermal = run('shell', 'dumpsys', 'thermalservice').decode()
                    state = dict(host_utc=datetime.now(timezone.utc).isoformat(),
                                 battery_c=int(re.search(r'temperature:\s*(\d+)', battery)[1]) / 10,
                                 thermal_status=int(re.search(r'Thermal Status:\s*(\d+)', thermal)[1]),
                                 battery_raw=battery, thermal_raw=thermal)
                    states.append(state)
                    state_file.write(json.dumps(state) + '\n')
                    state_file.flush()
                    append_log(log_file)
                    next_state = time.monotonic() + 5
                time.sleep(min(.8, max(0, deadline - time.monotonic())))
            append_log(log_file)
        if run('shell', 'pidof', PKG).decode().strip() != pid:
            raise RuntimeError('App process changed at capture end')
        timestamps = sorted(frames)
        if len(timestamps) < 2 or timestamps[-1] - timestamps[0] < (args.seconds - 10) * 1e9:
            raise RuntimeError('Insufficient new presented frames; check foreground/layer')
        intervals = [(b - a) / 1e6 for a, b in zip(timestamps, timestamps[1:])]
        text = (out / 'interval.log').read_text(encoding='utf-8', errors='replace')
        fatal = [line for line in text.splitlines() if any(x in line for x in
                 ('[nvn-vulkan] STOP', 'ANDROID_FATAL', 'VK_ERROR_DEVICE_LOST'))]
        if fatal:
            raise RuntimeError('Fatal renderer error: ' + fatal[0])
        groups = {}
        for line in text.splitlines():
            match = re.search(r'\b((?:RENDER|ADPF_WORK)\w*PROFILE)\s', line)
            if not match:
                match = re.search(r'\b(SEG[0-3]|PASS_TIME_TOTAL|PASS_TIME|HOT_PS)\s', line)
            if match:
                groups.setdefault(match[1], []).append(dict(re.findall(r'(\w+)=([^\s]+)', line)))
        (out / 'profile-groups.json').write_text(json.dumps(groups, indent=2) + '\n', encoding='utf-8')
        report.update(completed=True, actual_present_frames=len(timestamps),
                      actual_present_span_s=(timestamps[-1] - timestamps[0]) / 1e9,
                      actual_present_fps=(len(timestamps) - 1) * 1e9 / (timestamps[-1] - timestamps[0]),
                      interval_ms=dict(median=statistics.median(intervals), p95=percentile(intervals, .95),
                                       p99=percentile(intervals, .99), maximum=max(intervals)),
                      refresh_periods_ns=sorted(set(samples)),
                      battery_range_c=[min(s['battery_c'] for s in states), max(s['battery_c'] for s in states)],
                      thermal_statuses=sorted(set(s['thermal_status'] for s in states)),
                      thermal_stability_proven=False, profile_windows=len(groups.get('RENDER_PROFILE', [])))
        report['artifacts_sha256'] = {name: hashlib.sha256((out / name).read_bytes()).hexdigest()
                                      for name in ('display-samples.jsonl', 'state-samples.jsonl',
                                                   'interval.log', 'startup-header.log', 'profile-groups.json')}
    except Exception as error:
        report['completed'] = False
        report['error'] = str(error)
        raise
    finally:
        report['ended_utc'] = datetime.now(timezone.utc).isoformat()
        save()
    print(json.dumps(report, ensure_ascii=False), flush=True)


if __name__ == '__main__':
    main()
