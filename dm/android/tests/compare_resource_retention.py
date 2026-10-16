"""Compare live immutable-resource retention without restarting or moving the player.

Requires resource-control.txt to exist before app startup. Changes only this
diagnostic option; capture_present_profile verifies the stationary camp scene.
Each mode waits for its first complete renderer reporting boundary before the
read-only presentation capture starts. Leaves the final requested mode active.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import statistics
import subprocess
import sys
import time


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--serial',required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--seconds',type=int,default=60)
    p.add_argument('--modes',default='0,1,0,1,1,0')
    p.add_argument('--first-sequence',type=int,default=2)
    a=p.parse_args()
    modes=[int(x) for x in a.modes.split(',')]
    if a.seconds<30 or not modes or any(x not in (0,1) for x in modes) or a.first_sequence<1:
        p.error('Require >=30 seconds, binary modes and positive sequence')
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    adb=['C:/tools/platform-tools/adb.exe','-s',a.serial]
    pkg='org.d2r.binaryprobe';log='files/d2r-mobile/diagnostics/game-startup.log'
    control='files/d2r-mobile/diagnostics/resource-control.txt'
    def run(*parts,input=None):
        r=subprocess.run([*adb,*parts],input=input,capture_output=True,timeout=15)
        if r.returncode: raise RuntimeError((r.stdout+r.stderr).decode(errors='replace'))
        return r.stdout
    pid=run('shell','pidof',pkg).decode().strip()
    report=dict(passed=False,started_utc=datetime.now(timezone.utc).isoformat(),serial=a.serial,pid=pid,
        tool_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),modes=modes,records=[],
        driver_required=8,input_sent=False,diagnostic_option_changed=True)
    receipt=out/'result.json'
    def save(): receipt.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    save()
    for i,enabled in enumerate(modes):
        sequence=a.first_sequence+i
        run('shell','run-as',pkg,'tee',control,input=f'{sequence} {enabled}\n'.encode())
        deadline=time.monotonic()+30
        while True:
            if run('shell','pidof',pkg).decode().strip()!=pid: raise RuntimeError('Game process changed')
            tail=run('exec-out','run-as',pkg,'tail','-c','48000',log).decode(errors='replace')
            rows=[dict(re.findall(r'(\w+)=([^\s]+)',line)) for line in tail.splitlines() if 'RENDER_RESOURCE_CACHE_PROFILE ' in line]
            if rows and rows[-1].get('enabled')==str(enabled) and rows[-1].get('sequence')==str(sequence): break
            if time.monotonic()>deadline: raise RuntimeError('No confirmed renderer boundary for requested option')
            time.sleep(.5)
        label=f'{i}-retain-{enabled}';(out/(label+'-confirmed.log')).write_text(tail,encoding='utf-8')
        command=[sys.executable,'-X','utf8',str(Path(__file__).with_name('capture_present_profile.py')),
            '--serial',a.serial,'--scene','rogue-encampment','--scene-probe','files/attachment-scene-probe',
            '--level','1','--stationary','--seconds',str(a.seconds),'--output',str(out/label)]
        captured=subprocess.run(command,capture_output=True,timeout=a.seconds+60)
        (out/(label+'-capture.log')).write_bytes(captured.stdout+captured.stderr)
        captured.check_returncode()
        result=json.loads((out/label/'result.json').read_text())
        profiles=json.loads((out/label/'profile-groups.json').read_text())
        header=(out/label/'startup-header.log').read_text(errors='replace')
        if not re.search(r'\[nvn-vulkan\] driver id=8\b',header): raise RuntimeError('Expected Qualcomm system driver')
        if result['pid']!=int(pid) or not result['scene_identity_verified']: raise RuntimeError('Unverified process/scene')
        if report['records'] and result['scene_identity']!=report['records'][0]['scene_identity']: raise RuntimeError('Scene moved between modes')
        controls=profiles.get('RENDER_RESOURCE_CACHE_PROFILE',[])
        if len(controls)<3 or any(row['sequence']!=str(sequence) or row['enabled']!=str(enabled) for row in controls):
            raise RuntimeError('Capture contains unconfirmed/mixed option windows')
        render=profiles.get('RENDER_PROFILE',[])
        if not render or any(float(row['pipeline_ms'])!=0 for row in render): raise RuntimeError('Capture includes pipeline creation')
        record=dict(mode=enabled,sequence=sequence,directory=label,actual_present_fps=result['actual_present_fps'],
            actual_present_span_s=result['actual_present_span_s'],scene_identity=result['scene_identity'],
            battery_range_c=result['battery_range_c'],thermal_statuses=result['thermal_statuses'],
            interval_ms=result['interval_ms'],resource_first=controls[0],resource_last=controls[-1])
        for tag,fields in {'RENDER_PHASE_CPU_PROFILE':['replay_inclusive_ms','retire_ms','writeback_subset_ms'],
                           'RENDER_VIEW_CACHE_PROFILE':['framebuffer_hits','framebuffer_misses','view_misses']}.items():
            record[tag]={key:statistics.median(float(r[key]) for r in profiles[tag]) for key in fields}
        report['records'].append(record);save()
        print(json.dumps(record),flush=True)
    report['fps_by_mode']={str(mode):[r['actual_present_fps'] for r in report['records'] if r['mode']==mode] for mode in set(modes)}
    report.update(passed=True,ended_utc=datetime.now(timezone.utc).isoformat());save()


if __name__=='__main__':main()
