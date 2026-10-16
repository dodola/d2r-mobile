"""Package a provenance-checked native fix with an existing signed Android shell.

Retains the shell's resources, modules, Java and version; signs an in-place test
update with the existing debug key. Never alters the base APK or native build.
"""
from pathlib import Path
import argparse,hashlib,json,os,struct,subprocess,tempfile
from zipfile import ZipFile

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base',type=Path,required=True)
    p.add_argument('--native-build',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();report=json.loads(a.native_build.read_text(encoding='utf-8'))
    item=report['outputs']['libd2r_mobile_android.so'];native=Path(item['path'])
    if sha(native)!=item['sha256']:raise ValueError('Native build differs from provenance')
    if a.base.resolve()==a.output.resolve():raise ValueError('Output must preserve the base APK')
    data=native.read_bytes();offset=struct.unpack_from('<Q',data,32)[0];size,count=struct.unpack_from('<HH',data,54)
    for i in range(count):
        kind,flags,off,va,pa,fs,ms,align=struct.unpack_from('<IIQQQQQQ',data,offset+i*size)
        if kind==1 and (align<16384 or (va-off)%16384):raise ValueError('Native LOAD not 16 KB aligned')
    sdk=Path(os.environ['LOCALAPPDATA'])/'Android/Sdk';tools=sdk/'build-tools/35.0.0'
    env=dict(os.environ)
    java=Path(os.environ.get('ANDROID_SWITCH_WORKSPACE','D:/AndroidSwitch'))/'tools/strato-jdk17/jdk-17.0.20.1+1'
    env['JAVA_HOME']=str(java);env['PATH']=str(java/'bin')+os.pathsep+env['PATH']
    def run(*args):
        r=subprocess.run(list(map(str,args)),env=env,capture_output=True)
        if r.returncode:raise RuntimeError((r.stdout+r.stderr).decode(errors='replace'))
    a.output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='d2r-native-apk-') as directory:
        temp=Path(directory);unsigned=temp/'unsigned.apk';aligned=temp/'aligned.apk'
        with ZipFile(a.base) as source,ZipFile(unsigned,'w') as target:
            if 'lib/arm64-v8a/libd2r_mobile_android.so' not in source.namelist():raise ValueError('Base has no native host')
            for entry in source.infolist():
                if entry.filename.startswith('META-INF/'):continue
                payload=data if entry.filename=='lib/arm64-v8a/libd2r_mobile_android.so' else source.read(entry.filename)
                target.writestr(entry,payload)
        run(tools/'zipalign.exe','-f','-P','16','4',unsigned,aligned)
        run(tools/'apksigner.bat','sign','--ks',Path(os.environ['USERPROFILE'])/'.android/debug.keystore',
            '--ks-pass','pass:android','--key-pass','pass:android','--out',a.output,aligned)
        run(tools/'apksigner.bat','verify',a.output);run(tools/'zipalign.exe','-c','-P','16','4',a.output)
    with ZipFile(a.base) as source,ZipFile(a.output) as target:
        for name in source.namelist():
            if name.startswith('META-INF/'):continue
            expected=data if name=='lib/arm64-v8a/libd2r_mobile_android.so' else source.read(name)
            if target.read(name)!=expected:raise ValueError('Unexpected packaged payload: '+name)
    proof={'apk':str(a.output.resolve()),'sha256':sha(a.output),'base_apk':str(a.base.resolve()),'base_sha256':sha(a.base),
        'native_sha256':sha(native),'native_build':str(a.native_build.resolve()),'native_build_sha256':sha(a.native_build),
        'native_only_update':True,'native_16kb_alignment_verified':True,'signature_verified':True}
    a.output.with_suffix('.build.json').write_text(json.dumps(proof,indent=2)+'\n',encoding='utf-8');print(json.dumps(proof))
if __name__=='__main__':main()
