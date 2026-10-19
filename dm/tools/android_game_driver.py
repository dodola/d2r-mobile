"""adb driver for the D2R Android port: enter the world with a new character, then explore with the touch joystick.
Coordinates assume a 2652x1200 panel (joystick centre (712, 823), IME candidate tap (915, 815)); adjust CENTER for other panels.
Character names must be real words the IME completes verbatim (e.g. Warrior); "Tester2" was completed to the existing "Tester".
usage: drive.py enter <name>      -- cold start, create a character, wait until the world is loaded
       drive.py explore <seconds> -- feedback-driven walk; stops when a loading screen (zone change) is detected
"""
import subprocess, sys, time, io, random, re, json
import numpy as np
from PIL import Image
ADB = 'adb'; PKG = 'org.d2r.newprobe'; DIAG = 'files/d2r-mobile/diagnostics'
CENTER = (712, 823)  # joystick centre on a 2652x1200 panel
def sh(*a, timeout=60):
    return subprocess.run([ADB, *a], capture_output=True, timeout=timeout)
def shell(cmd, timeout=60):
    return sh('shell', cmd, timeout=timeout).stdout.decode('utf-8', 'ignore')
def shot():
    raw = sh('exec-out', 'screencap', '-p').stdout
    return Image.open(io.BytesIO(raw)).convert('L').resize((265, 120))
def diff(a, b):
    return float((np.abs(np.asarray(a, dtype=np.int16) - np.asarray(b, dtype=np.int16)) > 30).mean())
def status():
    raw = sh('exec-out', 'run-as', PKG, 'cat', DIAG + '/status.json').stdout.decode('utf-8', 'ignore')
    try: return json.loads(raw)
    except Exception: return {}
def log_text():
    return sh('exec-out', 'run-as', PKG, 'cat', DIAG + '/game-startup.log').stdout.decode('utf-8', 'ignore')
def gp(key): shell('input gamepad keyevent ' + key)
def alive(): return bool(shell('pidof ' + PKG).strip())
def in_world(): return status().get('scene', {}).get('game', 0) == 1

def enter(name):
    shell('am force-stop ' + PKG); time.sleep(2)
    shell('run-as %s sh -c "rm -f %s/game-startup.log %s/render-scale.txt"' % (PKG, DIAG, DIAG))
    shell('am start -W -n %s/org.d2r.binaryprobe.GameActivity' % PKG)
    for _ in range(60):
        time.sleep(2)
        if 'ANDROID_GAME_STAGE 游戏画面已呈现' in log_text(): break
    time.sleep(6)
    gp('BUTTON_START'); time.sleep(6)          # character list -> new character (class select)
    for _ in range(3): gp('BUTTON_A'); time.sleep(6)   # class, class, game mode -> name dialog
    shell('input text ' + name); time.sleep(2)
    shell('input tap 915 815'); time.sleep(2)  # pick the IME candidate
    shell('input keyevent KEYCODE_ENTER')
    for i in range(60):
        time.sleep(3)
        if in_world(): print('in world after ~%ds' % (i * 3 + 10)); return True
    print('did not reach the world'); return False

def loading_screen(img):
    a = np.asarray(img)
    return float((a < 12).mean()) > 0.55   # near-black letterboxed loading image

def swipe(dx, dy, ms):
    shell('input swipe %d %d %d %d %d' % (CENTER[0], CENTER[1], CENTER[0] + dx, CENTER[1] + dy, ms), timeout=ms // 1000 + 30)

def explore(seconds):
    dirs = [(-260, 0), (-200, -150), (0, -200), (200, -150), (260, 0), (200, 150), (0, 200), (-200, 150)]
    t0 = time.time(); cur = random.randrange(8); moved_total = 0; zone = False
    prev = shot()
    while time.time() - t0 < seconds:
        swipe(*dirs[cur], 5000)
        time.sleep(0.4); now = shot()
        if not alive(): print('PROCESS DIED'); return 'died'
        d = diff(prev, now)
        if loading_screen(now) or 'STOP' in log_text()[-4000:]:
            print('loading screen / stop detected'); zone = True; break
        print('dir', dirs[cur], 'scene change %.3f' % d)
        if d < 0.02: cur = (cur + random.choice([1, 2, 3, 5, 6, 7])) % 8   # blocked -> turn
        else: moved_total += 1
        prev = now
    return 'zone' if zone else 'timeout'

if __name__ == '__main__':
    if sys.argv[1] == 'enter': sys.exit(0 if enter(sys.argv[2]) else 1)
    if sys.argv[1] == 'explore': print(explore(int(sys.argv[2])))
