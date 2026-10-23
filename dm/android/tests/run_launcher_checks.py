"""用合成文件运行主机和可选模拟器启动器回归，不访问真实玩家数据。
Run synthetic host and optional emulator launcher regressions without accessing real player data.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
from zipfile import ZipFile, ZIP_STORED

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--java',type=Path,required=True)
p.add_argument('--sdk',type=Path)
p.add_argument('--emulator',help='Only emulator-NNNN targets are allowed')
a=p.parse_args()
if a.emulator and not re.fullmatch(r'emulator-\d+',a.emulator):p.error('Only an isolated emulator is allowed')
root=Path(__file__).resolve().parents[3];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
java=a.java.resolve();src=root/'dm/android/src';tests=root/'dm/android/tests'
env=dict(os.environ,JAVA_HOME=str(java));env['PATH']=str(java/'bin')+os.pathsep+env.get('PATH','')
suffix='.exe' if os.name=='nt' else ''
def run(label,*cmd,timeout=120):
    r=subprocess.run([str(x) for x in cmd],env=env,cwd=root,capture_output=True,timeout=timeout)
    text=(r.stdout+r.stderr).decode('utf-8',errors='replace');(out/(label+'.log')).write_text(text,encoding='utf-8')
    if r.returncode:raise RuntimeError(label+': '+text[-3000:])
    return text
host=out/'host';host.mkdir(exist_ok=True)
core=src/'org/d2r/binaryprobe'
names=['SaveArchive','LauncherSettingsStore','ModSelectionStore','GameSessionGate']
run('host-compile',java/('bin/javac'+suffix),'--release','8','-encoding','UTF-8','-d',host,*[core/(n+'.java') for n in names],tests/'LauncherSaveTest.java')
host_result=run('host-test',java/('bin/java'+suffix),'-cp',host,'org.d2r.binaryprobe.LauncherSaveTest');print(host_result.strip(),flush=True)
report={'host':host_result.strip(),'synthetic_only':True,'physical_device_contacted':False}
if a.emulator:
    if a.sdk is None:p.error('--sdk required with --emulator')
    sdk=a.sdk.resolve();bt=sdk/'build-tools/35.0.0';android=sdk/'platforms/android-35/android.jar'
    classes=out/'classes';dex=out/'dex';assets=out/'assets'
    for folder in (classes,dex,assets):folder.mkdir(exist_ok=True)
    run('java-compile',java/('bin/javac'+suffix),'--release','8','-encoding','UTF-8','-cp',android,'-d',classes,
        *sorted(src.rglob('*.java')),tests/'LauncherTestProvider.java',tests/'LauncherTestInstrumentation.java')
    with ZipFile(out/'dex-input.jar','w') as archive:
        for f in sorted(classes.rglob('*.class')):archive.write(f,f.relative_to(classes).as_posix())
    run('dex',bt/('d8.bat' if os.name=='nt' else 'd8'),'--min-api','29','--lib',android,'--output',dex,out/'dex-input.jar')
    files=[{'path':name,'bytes':1,'sha256':hashlib.sha256(value).hexdigest()} for name,value in [('game.romfs',b'r'),('d2r-mobile/shaders/shader_index.bin',b's')]]
    (assets/'offline-manifest.json').write_text(json.dumps({'format':1,'game_build':94012,'files':files,'total_bytes':2}),encoding='utf-8')
    manifest=out/'AndroidManifest.xml'
    manifest.write_text('''<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="org.d2r.launcher.qa" android:versionCode="6" android:versionName="1.2.0-94012-qa">
      <uses-sdk android:minSdkVersion="29" android:targetSdkVersion="33"/>
      <application android:label="D2R Launcher QA" android:debuggable="true" android:allowBackup="false" android:theme="@android:style/Theme.Material.NoActionBar">
        <activity android:name="org.d2r.binaryprobe.InstallerActivity" android:exported="true"><intent-filter><action android:name="android.intent.action.MAIN"/><category android:name="android.intent.category.LAUNCHER"/></intent-filter></activity>
        <provider android:name="org.d2r.binaryprobe.LauncherTestProvider" android:authorities="org.d2r.launcher.qa.saves" android:exported="false"/>
      </application>
      <instrumentation android:name="org.d2r.binaryprobe.LauncherTestInstrumentation" android:targetPackage="org.d2r.launcher.qa"/>
    </manifest>''',encoding='utf-8')
    run('aapt-compile',bt/('aapt2'+suffix),'compile','--dir',root/'dm/android/res','-o',out/'resources.zip')
    run('aapt-link',bt/('aapt2'+suffix),'link','-o',out/'base.apk','--manifest',manifest,'-I',android,'-A',assets,out/'resources.zip')
    with ZipFile(out/'base.apk','a') as z:
        for f in sorted(dex.glob('classes*.dex')):z.write(f,f.name,compress_type=ZIP_STORED)
    run('align',bt/('zipalign'+suffix),'-f','-P','16','4',out/'base.apk',out/'aligned.apk')
    apk=out/'launcher-qa.apk'
    run('sign',java/('bin/java'+suffix),'-jar',bt/'lib/apksigner.jar','sign','--ks',Path.home()/'.android/debug.keystore','--ks-pass','pass:android','--key-pass','pass:android','--out',apk,out/'aligned.apk')
    adb=[sdk/('platform-tools/adb'+suffix),'-s',a.emulator]
    run('screen-size',*adb,'shell','wm','size','1080x2400')
    run('screen-density',*adb,'shell','wm','density','420')
    run('screen-orientation',*adb,'shell','wm','set-ignore-orientation-request','false')
    run('screen-rotation',*adb,'shell','wm','user-rotation','lock','0')
    run('install',*adb,'install','--no-incremental','-r',apk)
    text=run('instrumentation',*adb,'shell','am','instrument','-w','org.d2r.launcher.qa/org.d2r.binaryprobe.LauncherTestInstrumentation',timeout=180)
    print(text.strip(),flush=True)
    if 'passed=1' not in text or 'failed=' in text:raise RuntimeError('Emulator instrumentation did not pass')
    for name in ('launcher-portrait','launcher-landscape'):
        data=subprocess.check_output([str(x) for x in adb]+['exec-out','run-as','org.d2r.launcher.qa','cat','files/'+name+'.png'])
        (out/(name+'.png')).write_bytes(data)
    report['emulator']=a.emulator;report['android']=text.strip()
(out/'result.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
