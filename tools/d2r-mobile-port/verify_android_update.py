"""Compile and run the pure-JVM Android update protocol checks."""
from pathlib import Path
import json
import os
import shutil
import subprocess
import sys

SCRIPT_ROOT=Path(__file__).resolve().parents[2]
WORKSPACE=Path(os.environ.get('ANDROID_SWITCH_WORKSPACE',str(SCRIPT_ROOT.parents[1]))).resolve()
REPO=Path(os.environ.get('D2R_ANDROID_SOURCE_ROOT',str(SCRIPT_ROOT))).resolve()
JAVA=WORKSPACE/'tools/strato-jdk17/jdk-17.0.20.1+1'
OUTPUT=WORKSPACE/'analysis/d2r-mobile-port/android-update-checker-tests'
SOURCE=REPO/'dm/android/src/org/d2r/binaryprobe/AndroidUpdateChecker.java'
TEST=REPO/'dm/android/tests/java/org/d2r/binaryprobe/AndroidUpdateCheckerTest.java'


def main():
    javac=JAVA/'bin/javac.exe'
    java=JAVA/'bin/java.exe'
    if not javac.is_file() or not java.is_file():
        raise RuntimeError(f'Pinned JDK is missing: {JAVA}')
    if OUTPUT.exists():
        shutil.rmtree(OUTPUT)
    OUTPUT.mkdir(parents=True)
    compile_result=subprocess.run(
        [str(javac),'--release','8','-encoding','UTF-8','-d',str(OUTPUT),str(SOURCE),str(TEST)],
        capture_output=True,text=True,encoding='utf-8',errors='replace')
    if compile_result.returncode:
        raise RuntimeError(compile_result.stdout+compile_result.stderr)
    run_result=subprocess.run(
        [str(java),'-cp',str(OUTPUT),'org.d2r.binaryprobe.AndroidUpdateCheckerTest'],
        capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=30)
    if run_result.returncode:
        raise RuntimeError(run_result.stdout+run_result.stderr)
    report={
        'format':1,
        'status':'passed',
        'runtime':'pure JVM',
        'source':str(SOURCE.relative_to(REPO)),
        'tests':str(TEST.relative_to(REPO)),
        'stdout':run_result.stdout.strip(),
        'android_transport_verified':False,
        'game_device_verified':False,
    }
    report_path=OUTPUT/'report.json'
    report_path.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,ensure_ascii=False))


if __name__=='__main__':
    try:
        main()
    except Exception as error:
        print(str(error),file=sys.stderr)
        raise
