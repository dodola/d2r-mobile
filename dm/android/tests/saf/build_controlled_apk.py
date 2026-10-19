"""Build the disposable SAF provider/instrumentation APK, without game/native assets."""
from pathlib import Path
import argparse
import os
import subprocess
import zipfile


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--sdk',type=Path,default=Path(os.environ.get('LOCALAPPDATA',''))/'Android/Sdk')
    parser.add_argument('--jdk',type=Path,default=Path('C:/Program Files/Java/jdk-21'))
    args=parser.parse_args()
    repo=Path(__file__).resolve().parents[4]
    build=args.output.resolve();build.mkdir(parents=True,exist_ok=True)
    tools=sorted((args.sdk/'build-tools').iterdir())[-1]
    android=args.sdk/'platforms/android-35/android.jar'
    classes=build/'classes';classes.mkdir(exist_ok=True)
    dex=build/'dex';dex.mkdir(exist_ok=True)
    def run(*cmd):subprocess.run([str(x) for x in cmd],check=True)
    sources=[repo/'dm/android/src/org/d2r/binaryprobe'/x for x in ('SafCloudFormat.java','SafCloudStore.java')]
    sources+=[repo/'dm/android/tests/SafCloudFormatTest.java',Path(__file__).with_name('ControlledDocumentsProvider.java'),Path(__file__).with_name('SafCloudInstrumentation.java')]
    run(args.jdk/'bin/javac.exe','-encoding','UTF-8','-source','8','-target','8','-Xlint:-options','-cp',android,'-d',classes,*sources)
    run(tools/'d8.bat','--min-api','29','--lib',android,'--output',dex,*classes.rglob('*.class'))
    unsigned=build/'controlled-unsigned.apk'
    run(tools/'aapt2.exe','link','-I',android,'--manifest',Path(__file__).with_name('AndroidManifest.xml'),'-o',unsigned)
    with zipfile.ZipFile(unsigned,'a') as apk:apk.write(dex/'classes.dex','classes.dex')
    aligned=build/'controlled-aligned.apk';run(tools/'zipalign.exe','-f','4',unsigned,aligned)
    key=build/'test-signing.p12'
    if not key.exists():run(args.jdk/'bin/keytool.exe','-genkeypair','-keystore',key,'-storepass','test-only-password','-keypass','test-only-password','-alias','saf-test','-keyalg','RSA','-keysize','2048','-validity','3650','-dname','CN=Disposable SAF Tests')
    signed=build/'saf-controlled.apk';run(tools/'apksigner.bat','sign','--ks',key,'--ks-key-alias','saf-test','--ks-pass','pass:test-only-password','--out',signed,aligned)
    run(tools/'apksigner.bat','verify',signed)
    print(signed)


if __name__=='__main__':main()
