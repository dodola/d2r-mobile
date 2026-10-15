"""Observe two manual entries into the same level; never sends game input.

First run from character selection, wait for ready_to_enter, enter camp and
stay until first_complete. Save/exit, then enter the same character again.
All rendering gaps, game-data observations and startup logs are retained.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
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
DIAG = 'files/d2r-mobile/diagnostics/'


def fields(line):
    return {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)(?=\s|$)', line)}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--serial', required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--level', type=int, default=1)
    ap.add_argument('--settle', type=int, default=120)
    ap.add_argument('--entry-seconds', type=int, default=60)
    ap.add_argument('--max-seconds', type=int, default=1200)
    ap.add_argument('--resume-ready', type=Path, help='Resume an already-ready, pre-entry capture after a transport interruption; same app PID/mode required')
    args = ap.parse_args()
    if args.entry_seconds < 60 or args.settle < 120:
        ap.error('Require >=120 seconds settling and >=60 seconds per entry')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    adb = [str(Path(os.environ['LOCALAPPDATA']) / 'Android/Sdk/platform-tools/adb.exe'), '-s', args.serial]
    report = {'completed': False, 'serial': args.serial, 'started_utc': datetime.now(timezone.utc).isoformat(),
              'source_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'input_sent': False, 'screenshots_taken': False, 'phases': [],
              'entry_anchor': 'First game-data observation of the expected level after absence; not loading-screen disappearance or operator input time',
              'hardware_bandwidth_measured': False}
    receipt = out / 'result.json'

    def save():
        receipt.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')

    def run(*command):
        for attempt in range(5):
            p = subprocess.run(adb + list(command), capture_output=True, timeout=20)
            if not p.returncode:
                return p.stdout
            message = (p.stdout + p.stderr).decode(errors='replace')
            if not any(s in message for s in ('not found', 'offline', 'closed', 'no devices')) or attempt == 4:
                raise RuntimeError(message[-500:])
            time.sleep(.5 * (attempt + 1))

    def private(*command):
        return run('exec-out', 'run-as', PKG, *command)

    pid = run('shell', 'pidof', PKG).decode().strip()
    if not pid.isdecimal():
        raise RuntimeError('Expected one running app PID')
    initial = private('cat', DIAG + 'game-startup.log')
    base = re.search(rb'main: base (0x[0-9a-f]+)', initial)[1].decode()
    mode = int(re.search(rb'PIPELINE_CACHE mode=(\d+)', initial)[1])
    report.update(pid=pid, main_base=base, pipeline_mode=mode,
                  attachment_mode=private('cat', DIAG + 'attachment-mode.txt').decode().strip())
    layers = run('shell', 'dumpsys', 'SurfaceFlinger', '--list').decode()

    def find_layer(listing, required=True):
        candidates = [l.removeprefix('RequestedLayerState{').split(' parentId=')[0].rstrip('}')
                      for l in listing.splitlines() if 'SurfaceView[' + PKG in l and '](BLAST)#' in l]
        if len(candidates) != 1:
            if required:
                raise RuntimeError('Expected one game SurfaceView layer')
            return None
        return candidates[0]

    layer = find_layer(layers)
    frames, display, states, thermal = [], {}, [], []
    stage = 'settling'
    offset = 0
    pending = b''
    began = time.monotonic()
    last_thermal = last_log = -100
    first_ready = None
    epoch = None
    absent_since = None
    prior_states = prior_display = ''
    if args.resume_ready:
        old = json.loads((args.resume_ready / 'result.json').read_text(encoding='utf-8'))
        if old['pid'] != pid or old['pipeline_mode'] != mode or old.get('phases') or old.get('stage') != 'await_first':
            raise RuntimeError('Resume requires the same process/mode already waiting for its first entry')
        prior_states = (args.resume_ready / 'state.jsonl').read_text()
        prior_display = (args.resume_ready / 'display.jsonl').read_text()
        states = [json.loads(l) for l in prior_states.splitlines()]
        thermal = [s['thermal'] for s in states if 'thermal' in s]
        stage = 'await_first'
        first_ready = states[-1]['mono_ns']
        report['resumed_from'] = str(args.resume_ready)
        report['resumed_prior_state_last_mono_ns'] = first_ready
    save()
    print(json.dumps({'stage': stage, 'pid': pid, 'mode': mode}), flush=True)

    def stable(now):
        rows = [t for t in thermal if now - 120_000_000_000 <= t['mono_ns'] <= now]
        if len(rows) < 20 or rows[-1]['mono_ns'] - rows[0]['mono_ns'] < 110_000_000_000:
            return {'passed': False, 'reason': 'insufficient duration'}
        battery = max(t['battery_c'] for t in rows) - min(t['battery_c'] for t in rows)
        skin = max(t['skin_c'] for t in rows) - min(t['skin_c'] for t in rows)
        statuses = sorted({t['status'] for t in rows})
        return {'passed': battery <= .5 and skin <= 1 and len(statuses) == 1,
                'battery_range_c': battery, 'skin_range_c': skin, 'statuses': statuses}

    with (out / 'interval.log').open('wb') as logs, (out / 'display.jsonl').open('w') as dl, (out / 'state.jsonl').open('w') as sl, ThreadPoolExecutor(max_workers=5) as pool:
      dl.write(prior_display);dl.flush();sl.write(prior_states);sl.flush()
      try:
        while time.monotonic() - began < args.max_seconds:
            tick = time.monotonic()
            jobs = [pool.submit(private, 'files/attachment-scene-probe', pid, base),
                    pool.submit(run, 'shell', 'dumpsys', 'SurfaceFlinger', '--latency', '"' + layer + '"')]
            sensor_due = tick - last_thermal >= 5
            if sensor_due:
                sensors = [pool.submit(run, 'shell', 'dumpsys', c) for c in ('battery', 'thermalservice', 'display')]
            scene = json.loads(jobs[0].result())
            if not scene.get('version_verified') or str(scene['pid']) != pid:
                raise RuntimeError('Scene probe version/process mismatch')
            raw_display = jobs[1].result().decode()
            rows = []
            for line in raw_display.splitlines():
                parts = line.split()
                if len(parts) == 3 and all(p.isdecimal() for p in parts):
                    t = tuple(map(int, parts))
                    if min(t) > 0 and max(t) < (1 << 63) - 1:
                        rows.append(t)
            if not rows:
                new_layer = find_layer(run('shell', 'dumpsys', 'SurfaceFlinger', '--list').decode(), required=False)
                if new_layer and new_layer != layer:
                    dl.write(json.dumps({'mono_ns': scene['mono_ns'], 'layer_recreated': True, 'old': layer, 'new': new_layer, 'rows': []}) + '\n')
                    layer = new_layer
            for row in rows:
                display[row[1]] = row
            dl.write(json.dumps({'mono_ns': scene['mono_ns'], 'layer': layer, 'rows': rows}) + '\n');dl.flush()
            item = {'mono_ns': scene['mono_ns'], 'scene': scene, 'display_rows': len(rows),
                    'latest_display_mono_ns': max((r[1] for r in rows), default=None)}
            if sensor_due:
                battery, raw_thermal, raw_mode = [p.result().decode(errors='replace') for p in sensors]
                skin = re.search(r'Temperature\{mValue=([-\d.]+), mType=\d+, mName=skin,', raw_thermal)
                device = next(l for l in raw_mode.splitlines() if 'DisplayDeviceInfo{' in l)
                active_id = re.search(r'modeId (\d+)', device)[1]
                rate = re.search(r'\{id=' + active_id + r',.*?fps=([\d.]+), vsync=([\d.]+)', device)
                item['thermal'] = {'mono_ns': scene['mono_ns'], 'battery_c': int(re.search(r'temperature:\s*(\d+)', battery)[1]) / 10,
                                   'skin_c': float(skin[1]), 'status': int(re.search(r'Thermal Status:\s*(\d+)', raw_thermal)[1]),
                                   'active_display_hz': float(rate[2]), 'battery_raw': battery, 'thermal_raw': raw_thermal, 'display_raw': device}
                thermal.append(item['thermal']);last_thermal = tick
            states.append(item);sl.write(json.dumps(item) + '\n');sl.flush()
            if tick - last_log >= 1:
                size = int(private('stat', '-c', '%s', DIAG + 'game-startup.log'))
                if size < offset:
                    raise RuntimeError('App log truncated/restarted')
                if size > offset:
                    length = size - offset
                    data = private('sh', '-c', f'tail -c +{offset + 1} {DIAG}game-startup.log | head -c {length}')
                    if len(data) != length:
                        raise RuntimeError('Short log read')
                    logs.write(data);logs.flush();offset = size
                    lines = (pending + data).split(b'\n');pending = lines.pop()
                    for raw_line in lines:
                        line = raw_line.decode(errors='replace')
                        if 'PIPELINE_FRAME ' in line:
                            frames.append(fields(line))
                        if any(t in line for t in ('[nvn-vulkan] STOP', 'ANDROID_FATAL', 'VK_ERROR_DEVICE_LOST')):
                            raise RuntimeError('Fatal renderer message: ' + line)
                last_log = tick
            target = len(scene.get('players', [])) == 1 and scene['players'][0]['level'] == args.level
            now = scene['mono_ns']
            if stage == 'settling':
                if target:
                    raise RuntimeError('Entered target before ready_to_enter; first entry not captured under the protocol')
                if tick - began >= args.settle and stable(now)['passed']:
                    stage = 'await_first';first_ready = now
                    print(json.dumps({'stage': 'ready_to_enter', 'thermal': stable(now)}), flush=True)
            elif stage in ('await_first', 'await_repeat') and target:
                epoch = {'label': 'first' if stage == 'await_first' else 'repeat', 'entry_mono_ns': now,
                         'entry_player': scene['players'][0], 'request_window_begin_mono_ns': first_ready if stage == 'await_first' else absent_since}
                stage = 'recording'
                print(json.dumps({'stage': 'entry_detected', 'entry': epoch}), flush=True)
            elif stage == 'recording':
                if not target:
                    raise RuntimeError('Left target before entry capture completed')
                if now - epoch['entry_mono_ns'] >= args.entry_seconds * 1_000_000_000:
                    epoch.update(end_mono_ns=now, thermal=stable(now))
                    report['phases'].append(epoch);save()
                    print(json.dumps({'stage': epoch['label'] + '_complete', 'phase': epoch}), flush=True)
                    if epoch['label'] == 'repeat':
                        report['completed'] = True
                        break
                    stage = 'await_exit'
            elif stage == 'await_exit' and not target:
                if absent_since is None:
                    absent_since = now
                if now - absent_since >= 1_000_000_000:
                    stage = 'await_repeat'
                    print(json.dumps({'stage': 'exit_detected'}), flush=True)
            elif stage == 'await_exit':
                absent_since = None
            report['stage'] = stage
            if int(tick - began) % 30 == 0:
                save()
            time.sleep(max(0, .5 - (time.monotonic() - tick)))
        else:
            raise RuntimeError('Capture time limit exceeded')
      except Exception as e:
        report['error'] = str(e)
        raise
      finally:
        if report['completed']:
            # Actual-present fences for the final few frames may signal late.
            time.sleep(2)
            (out / 'final-game.log').write_bytes(private('cat', DIAG + 'game-startup.log'))
            (out / 'final-display.txt').write_bytes(run('shell', 'dumpsys', 'SurfaceFlinger', '--latency', '"' + layer + '"'))
        report['ended_utc'] = datetime.now(timezone.utc).isoformat()
        report['raw_sha256'] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in out.iterdir() if p.is_file() and p != receipt}
        save()
    print(json.dumps({'completed': report['completed'], 'receipt': str(receipt)}), flush=True)


if __name__ == '__main__':
    main()
