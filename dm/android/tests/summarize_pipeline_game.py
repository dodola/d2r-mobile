"""Summarize manual scene-entry captures, retaining entry and settled windows.

Game-data entry is an observation anchor, not an exact loading-complete event.
Display gaps come from actual-present timestamps; compiler timing is CPU time
spent creating/acquiring pipelines and does not measure GPU execution.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import statistics


def fields(line):
    return {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)(?=\s|$)', line)}


def summarize(directory, allow_incomplete=False):
    capture = json.loads((directory / 'result.json').read_text(encoding='utf-8'))
    if not capture['completed'] and not (allow_incomplete and (directory / 'capture-stop-reason.json').exists()):
        raise RuntimeError('Capture not complete: ' + str(directory))
    states = [json.loads(l) for l in (directory / 'state.jsonl').read_text().splitlines()]
    sensors = [s['thermal'] for s in states if 'thermal' in s]

    def thermal_at(now):
        rows = [s for s in sensors if now - 120e9 <= s['mono_ns'] <= now]
        if len(rows) < 20 or rows[-1]['mono_ns'] - rows[0]['mono_ns'] < 110e9:
            return {'passed': False, 'reason': 'insufficient duration'}
        battery = max(s['battery_c'] for s in rows) - min(s['battery_c'] for s in rows)
        skin = max(s['skin_c'] for s in rows) - min(s['skin_c'] for s in rows)
        statuses = sorted({s['status'] for s in rows})
        return {'passed': battery <= .5 and skin <= 1 and len(statuses) == 1,
                'battery_range_c': battery, 'skin_range_c': skin, 'statuses': statuses}

    if not capture['completed']:
        # A controlled stop can leave a complete, continuous 60-second entry
        # window on disk. Recover only that window; never claim thermal validity
        # or silently promote the stopped collector's completion status.
        episodes = []
        current = None
        absent_since = states[0]['mono_ns']
        for s in states:
            players = s['scene'].get('players', [])
            target = len(players) == 1 and players[0]['level'] == 1
            if target:
                if current is None:
                    current = {'rows': [], 'absence_begin': absent_since}
                current['rows'].append(s)
            else:
                if current is not None:
                    episodes.append(current);current = None;absent_since = s['mono_ns']
        if current is not None:
            episodes.append(current)
        for i, ep in enumerate(episodes[:2]):
            if i < len(capture['phases']):
                continue
            rows = ep['rows'];begin = rows[0]['mono_ns']
            ends = [s for s in rows if s['mono_ns'] >= begin + 60e9]
            if not ends or max(b['mono_ns'] - a['mono_ns'] for a, b in zip(rows, rows[1:])) > 2e9:
                raise RuntimeError('Stopped capture has no complete continuous entry window')
            end = ends[0]['mono_ns']
            capture['phases'].append({'label': 'first' if i == 0 else 'repeat', 'entry_mono_ns': begin,
                'end_mono_ns': end, 'entry_player': rows[0]['scene']['players'][0],
                'request_window_begin_mono_ns': ep['absence_begin'], 'thermal': thermal_at(end),
                'recovered_from_continuous_observations': True, 'extra_waiting_observations_excluded': True})
        if len(capture['phases']) != 2:
            raise RuntimeError('Two complete entry windows required')
    presented = {}
    for line in (directory / 'display.jsonl').read_text().splitlines():
        for row in json.loads(line)['rows']:
            presented[row[1]] = row
    if (directory / 'final-display.txt').exists():
        for line in (directory / 'final-display.txt').read_text().splitlines():
            p = line.split()
            if len(p) == 3 and all(s.isdecimal() for s in p):
                row = tuple(map(int, p))
                if min(row) > 0 and max(row) < (1 << 63) - 1:
                    presented[row[1]] = row
    log_path = directory / 'final-game.log'
    if not log_path.exists():
        log_path = directory / 'interval.log'
    log = log_path.read_text(encoding='utf-8', errors='replace')
    frames = []
    sync_ns = 0
    for line in log.splitlines():
        if 'PIPELINE_CREATE source=synchronous ' in line:
            sync_ns += fields(line)['build_ns']
        elif 'PIPELINE_FRAME ' in line:
            f = fields(line)
            f['sync_build_ns'] = sync_ns
            frames.append(f)
    if not frames:
        raise RuntimeError('No pipeline frame records')
    cache = fields(next(l for l in log.splitlines() if 'PIPELINE_CACHE mode=' in l))

    def window(begin, end):
        times = sorted(t for t in presented if begin <= t <= end)
        if len(times) < 2:
            raise RuntimeError('No actual-display intervals in measurement window')
        rates = sorted({s['active_display_hz'] for s in sensors if begin - 6e9 <= s['mono_ns'] <= end + 6e9})
        if not rates:
            raise RuntimeError('Missing display-mode observations')
        hz = rates[-1]
        gaps = [b - a for a, b in zip(times, times[1:])]
        slots = [max(1, int(g * hz / 1e9 + .5)) for g in gaps]
        before = [f for f in frames if f['mono_ns'] < begin]
        within = [f for f in frames if begin <= f['mono_ns'] <= end]
        baseline = before[-1] if before else {k: 0 for k in frames[0]}
        last = within[-1] if within else baseline
        counts = {k: last[k] - baseline[k] for k in ('sync_creates', 'claims', 'waits', 'wait_ns', 'bg_created', 'bg_ns', 'sync_build_ns')}
        scene_rows = [s for s in states if begin <= s['mono_ns'] <= end]
        missing = [s for s in scene_rows if not s['display_rows'] or s['latest_display_mono_ns'] is None or s['mono_ns'] - s['latest_display_mono_ns'] > 5e9]
        return {'begin_mono_ns': begin, 'end_mono_ns': end, 'display_frames': len(times),
                'display_intervals': len(gaps), 'display_fps': (len(times) - 1) * 1e9 / (times[-1] - times[0]),
                'gap_p50_ms': statistics.median(gaps) / 1e6,
                'gap_p95_ms': sorted(gaps)[int(.95 * (len(gaps) - 1))] / 1e6,
                'gap_max_ms': max(gaps) / 1e6,
                'gaps_at_least_two_refresh_slots': sum(s >= 2 for s in slots) if len(rates) == 1 else None,
                'drop_interval_percent': 100 * sum(s >= 2 for s in slots) / len(gaps) if len(rates) == 1 else None,
                'drop_unavailable_reason': 'display mode changed; no single refresh period' if len(rates) != 1 else None,
                'gaps_over_100ms': sum(g > 100_000_000 for g in gaps),
                'active_display_hz': rates, 'pipeline_counter_delta': counts,
                'display_missing_observations': len(missing),
                'display_valid': len(rates) == 1 and not missing,
                'thermal': thermal_at(end),
                'scene_samples': len(scene_rows),
                'levels_observed': sorted({p['level'] for s in scene_rows for p in s['scene'].get('players', [])})}

    phases = []
    for p in capture['phases']:
        entry = p['entry_mono_ns']
        phase = dict(p)
        phase['entry_first_30s'] = window(entry, entry + 30_000_000_000)
        phase['settled_last_30s'] = window(p['end_mono_ns'] - 30_000_000_000, p['end_mono_ns'])
        phase['preparation_and_entry'] = window(p['request_window_begin_mono_ns'], entry + 30_000_000_000)
        phase['preparation_includes_operator_wait_and_menu'] = True
        phase['entry_player_identity_is_loading_complete_proof'] = False
        phases.append(phase)
    return {'directory': str(directory), 'pid': capture['pid'], 'mode': capture['pipeline_mode'],
            'capture_completed': capture['completed'], 'capture_error': capture.get('error'),
            'attachment_mode': capture['attachment_mode'], 'cache': cache, 'phases': phases,
            'recorded_at_utc': capture['started_utc'], 'input_sent': False, 'screenshots_taken': False,
            'pipeline_create_events': len(re.findall(r'PIPELINE_CREATE source=', log)),
            'manifest_pipeline_claims': len(re.findall(r'PIPELINE_USE seed=1 ', log)),
            'total_sync_build_ns': sync_ns,
            'raw_sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in directory.iterdir() if p.is_file() and p.name != 'analysis.json'}}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('directories', nargs='+', type=Path)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--allow-controlled-stop', action='store_true', help='Recover complete entry windows only when an explicit controlled-stop receipt exists')
    args = ap.parse_args()
    runs = [summarize(d, args.allow_controlled_stop) for d in args.directories]
    result = {'completed': True, 'runs': runs,
              'drop_rule': 'Round actual-present gaps to nearest active-display refresh-slot count; count intervals spanning >=2 slots',
              'source_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'system_driver_cache_cleared': False, 'true_system_cold_start_claimed': False,
              'exact_operator_input_or_loading_complete_times_measured': False}
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    for run in runs:
        for p in run['phases']:
            print(json.dumps({'run': run['directory'], 'mode': run['mode'], 'phase': p['label'],
                              'first_30s': p['entry_first_30s'], 'last_30s': p['settled_last_30s']}, ensure_ascii=False))


if __name__ == '__main__':
    main()
