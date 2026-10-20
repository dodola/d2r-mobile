"""Compile/run host checks, full real-pack import, and optional isolated Android QA.

Does not replace or clear any installed D2R app: device QA uses the isolated org.d2r.offline.installertest package.
Build that QA APK with dm/tools/android_d2r_apk.py (no --offline-manifest, --package org.d2r.offline.installertest).
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

SCRIPT_ROOT = Path(__file__).resolve().parents[2]
ROOT = Path(os.environ.get('ANDROID_SWITCH_WORKSPACE', str(SCRIPT_ROOT))).resolve()
REPO = Path(os.environ.get('D2R_ANDROID_SOURCE_ROOT', str(SCRIPT_ROOT if (SCRIPT_ROOT/'dm/android/src').is_dir() else ROOT/'runtime/d2r-mobile'))).resolve()
JAVA = ROOT / 'tools/strato-jdk17/jdk-17.0.20.1+1'
SDK = Path(os.environ['LOCALAPPDATA']) / 'Android/Sdk'
BUILD = ROOT / 'build/offline-installer-tests'
RELEASE = ROOT / 'analysis/d2r-mobile-port/release-94012/pack'
PACKAGE = 'org.d2r.offline.installertest'


def run(*args):
    command = [str(a) for a in args]
    result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace')
    print(result.stdout, end='', flush=True)
    if result.returncode:
        raise RuntimeError(result.stderr + result.stdout)
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--full', action='store_true')
    parser.add_argument('--device', help='Explicit connected device serial for isolated Android tests')
    parser.add_argument('--qa-apk', type=Path, help='Debug-signed APK with package '+PACKAGE+' (required with --device)')
    args = parser.parse_args()
    BUILD.mkdir(parents=True, exist_ok=True)
    core = REPO / 'dm/android/src/org/d2r/binaryprobe/OfflinePackageInstaller.java'
    tests = REPO / 'dm/android/tests'
    run(JAVA/'bin/javac.exe', '--release', '8', '-encoding', 'UTF-8', '-d', BUILD, core,
        tests/'OfflinePackageInstallerTest.java', tests/'OfflinePackageFullTest.java',
        core.with_name('GraphicsDriverPolicy.java'), tests/'GraphicsDriverPolicyTest.java',
        core.with_name('OfflineInstallState.java'), tests/'OfflineInstallStateTest.java',
        core.with_name('ModSelectionStore.java'), tests/'ModSelectionStoreTest.java')
    report = {'host': run(JAVA/'bin/java.exe', '-Xmx64m', '-cp', BUILD, 'org.d2r.binaryprobe.OfflinePackageInstallerTest').strip()}
    report['graphics_driver'] = run(JAVA/'bin/java.exe', '-cp', BUILD, 'org.d2r.binaryprobe.GraphicsDriverPolicyTest').strip()
    report['launcher_state'] = run(JAVA/'bin/java.exe', '-cp', BUILD, 'org.d2r.binaryprobe.OfflineInstallStateTest').strip()
    report['mod_selection'] = run(JAVA/'bin/java.exe', '-cp', BUILD, 'org.d2r.binaryprobe.ModSelectionStoreTest').strip()
    if args.full:
        manifest = RELEASE/'offline-manifest.json'
        tsv = BUILD/'manifest-files.tsv'
        tsv.write_text(''.join(f'{e["path"]}\t{e["bytes"]}\t{e["sha256"]}\n' for e in json.loads(manifest.read_bytes())['files']), encoding='utf-8')
        report['full_import'] = run(JAVA/'bin/java.exe', '-Xmx64m', '-cp', BUILD, 'org.d2r.binaryprobe.OfflinePackageFullTest',
                                    manifest, tsv, RELEASE/'D2R-Offline-94012.d2rpack', BUILD/'full-import').strip()
    if args.device:
        qa = args.qa_apk
        if qa is None or not qa.is_file():
            raise SystemExit('--device requires --qa-apk pointing at the isolated QA package')
        classes = BUILD/'android-classes'
        dex = BUILD/'android-dex'
        classes.mkdir(exist_ok=True)
        dex.mkdir(exist_ok=True)
        android = SDK/'platforms/android-34/android.jar'
        tools = SDK/'build-tools/35.0.0'
        run(JAVA/'bin/javac.exe', '--release', '8', '-encoding', 'UTF-8', '-cp', str(android)+os.pathsep+str(BUILD),
            '-d', classes, tests/'InstallerTestInstrumentation.java', tests/'OfflinePackageInstallerTest.java')
        run(tools/'d8.bat', '--min-api', '29', '--lib', android, '--output', dex, *sorted(classes.rglob('*.class')))
        manifest = BUILD/'AndroidManifest.xml'
        manifest.write_text(f'''<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="org.d2r.offlinetests">
<uses-sdk android:minSdkVersion="29" android:targetSdkVersion="33" />
<application android:label="D2R Installer Tests" />
<instrumentation android:name="org.d2r.binaryprobe.InstallerTestInstrumentation" android:targetPackage="{PACKAGE}" />
</manifest>''', encoding='utf-8')
        unsigned = BUILD/'tests-unsigned.apk'
        run(tools/'aapt2.exe', 'link', '-o', unsigned, '--manifest', manifest, '-I', android)
        from zipfile import ZipFile
        with ZipFile(unsigned, 'a') as archive:
            archive.write(dex/'classes.dex', 'classes.dex')
        apk = BUILD/'tests.apk'
        run(tools/'apksigner.bat', 'sign', '--ks', Path(os.environ['USERPROFILE'])/'.android/debug.keystore',
            '--ks-pass', 'pass:android', '--key-pass', 'pass:android', '--out', apk, unsigned)
        adb = [Path('C:/tools/platform-tools/adb.exe'), '-s', args.device]
        run(*adb, 'install', '-r', qa)
        run(*adb, 'install', '-r', apk)
        report['android'] = run(*adb, 'shell', 'am', 'instrument', '-w', 'org.d2r.offlinetests/org.d2r.binaryprobe.InstallerTestInstrumentation').strip()
        if 'passed=1' not in report['android']:
            raise RuntimeError('Android importer tests did not pass')
        report['device_serial'] = args.device
    output = BUILD/('verification-device.json' if args.device else 'verification-full.json' if args.full else 'verification-host.json')
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')


if __name__ == '__main__':
    # d8/apksigner batch files require the pinned JDK on PATH, as in android_d2r_apk.py.
    os.environ['JAVA_HOME'] = str(JAVA)
    os.environ['PATH'] = str(JAVA/'bin')+os.pathsep+os.environ['PATH']
    main()
