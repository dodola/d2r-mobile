"""Package the real Android host; keep the existing app UID and original image."""
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import uuid
from zipfile import ZipFile, ZIP_DEFLATED, ZIP_STORED
import struct
import argparse
import sys
import xml.etree.ElementTree as ET
from urllib.parse import urlsplit

SCRIPT_ROOT=Path(__file__).resolve().parents[2]
ROOT=Path(os.environ.get('ANDROID_SWITCH_WORKSPACE',str(SCRIPT_ROOT))).resolve()
REPO=Path(os.environ.get('D2R_ANDROID_SOURCE_ROOT',str(SCRIPT_ROOT if (SCRIPT_ROOT/'dm/android/src').is_dir() else ROOT/'runtime/d2r-mobile'))).resolve()
# The pinned module/driver preparation remains in the established workspace.
sys.path.insert(0,str(ROOT/'tools/d2r-mobile-port'))
import prepare_offline_entry
import prepare_render_resolution
ART=ROOT/'analysis/d2r-mobile-port'
SDK=Path(os.environ['LOCALAPPDATA'])/'Android/Sdk'
JAVA=ROOT/'tools/strato-jdk17/jdk-17.0.20.1+1'
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def checked_update_url(value):
    value=(value or '').strip()
    if not value:return ''
    if any(ord(ch)<=0x20 or ord(ch)==0x7f for ch in value):raise ValueError('Update URL contains whitespace or control characters')
    try: parsed=urlsplit(value)
    except ValueError as error:raise ValueError('Update URL is malformed') from error
    if parsed.scheme.lower()!='https' or not parsed.hostname or parsed.username is not None or parsed.password is not None:
        raise ValueError('Update URL must be an absolute HTTPS URL without embedded credentials')
    return value

def elf_alignment(path):
    data=path.read_bytes()
    if data[:6]!=b'\x7fELF\x02\x01': raise RuntimeError(f'Expected little-endian ELF64: {path}')
    offset=struct.unpack_from('<Q',data,32)[0]
    size,count=struct.unpack_from('<HH',data,54)
    loads=[]
    for i in range(count):
        kind,flags,off,va,pa,fs,ms,align=struct.unpack_from('<IIQQQQQQ',data,offset+i*size)
        if kind==1:
            if align<16384 or (va-off)%16384: raise RuntimeError(f'Native library is not 16 KB aligned: {path}')
            loads.append({'offset':off,'vaddr':va,'alignment':align})
    if not loads: raise RuntimeError(f'ELF has no load segments: {path}')
    return loads
def main(use_render_resolution=False,offline_manifest=None,output=None,package='org.d2r.binaryprobe',update_url=None,modules_dir=None):
    update_url=checked_update_url(update_url or os.environ.get('DM_UPDATE_URL',''))
    default_modules=ART/'android-images'
    modules_dir=(Path(modules_dir) if modules_dir else default_modules).resolve()
    manifest=json.loads((ART/'build-manifest.json').read_text(encoding='utf-8'))
    driver=ART/'shader-private-driver/driver/vulkan.ad07xx.so'
    driver_selection=None
    if (ART/'android-build-config.json').is_file():
        driver_selection=json.loads((ART/'android-build-config.json').read_text(encoding='utf-8'))['private_driver']
        driver=(ROOT/driver_selection['path']).resolve()
        if not driver.is_relative_to(ROOT) or sha(driver)!=driver_selection['sha256']: raise RuntimeError('Selected private driver provenance mismatch')
    # Keep the explicitly selectable bundled driver pinned to the verified
    # binary. Auto uses the system driver; bundling does not select Turnip.
    tested_turnip='c2dc3ff00a1016bfc96e1513a7b0e21b92ea79d33fc8076b34e5fa70b0af8e9e'
    if sha(driver)!=tested_turnip: raise RuntimeError('Explicit compatibility mode requires the verified bundled Turnip build')
    native=ROOT/'build/d2r-mobile-android/libd2r_mobile_android.so'
    if sha(native)!=manifest['outputs'][native.name]['sha256']: raise RuntimeError('Native build provenance mismatch')
    build=ROOT/'build'/('d2r-mobile-apk-'+uuid.uuid4().hex[:12])
    for directory in ('classes','dex','assets/modules'): (build/directory).mkdir(parents=True)
    offline_manifest=offline_manifest or ART/'offline-release/offline-manifest.json'
    offline_data=json.loads(offline_manifest.read_text(encoding='utf-8'))
    if offline_data.get('format')!=1 or offline_data.get('game_build')!=91738:
        raise RuntimeError('Unsupported offline package manifest; build_offline_package.py must run first')
    shutil.copyfile(offline_manifest,build/'assets/offline-manifest.json')
    env=dict(os.environ);env['JAVA_HOME']=str(JAVA);env['PATH']=str(JAVA/'bin')+os.pathsep+env['PATH']
    tools=SDK/'build-tools/35.0.0';android=SDK/'platforms/android-34/android.jar'
    def run(*args):
        p=subprocess.run([str(a) for a in args],env=env,capture_output=True,text=True,encoding='utf-8',errors='replace')
        if p.returncode:
            (ART/'apk-build-failure.txt').write_text(p.stdout+p.stderr,encoding='utf-8')
            raise RuntimeError((p.stdout+p.stderr)[:2500])
        return p.stdout
    files=[]
    offline_entry=None
    render_resolution=None
    module_manifest=modules_dir/'module-manifest.json'
    if module_manifest.is_file():
        source_manifest=json.loads(module_manifest.read_text(encoding='utf-8'))
        expected={f'{name}.{extension}' for name in ('main','sdk','subsdk0')
            for extension in ('patched.bin','fixups.tsv','gates.tsv')}
        entries=source_manifest.get('files')
        if not isinstance(entries,list) or {entry.get('name') for entry in entries}!=expected:
            raise RuntimeError('Module manifest does not list the exact Android module set')
        for entry in entries:
            name=entry['name'];source=modules_dir/name
            if source.stat().st_size!=entry.get('bytes') or sha(source)!=entry.get('sha256'):
                raise RuntimeError(f'Module manifest provenance mismatch: {source}')
            shutil.copyfile(source,build/'assets/modules'/name)
            files.append({'name':name,'bytes':entry['bytes'],'sha256':entry['sha256']})
        packaged_module_manifest=source_manifest
    else:
        if modules_dir==default_modules.resolve() and (prepare_offline_entry.OUTPUT/'result.json').is_file():
            _,offline_entry=prepare_offline_entry.verified_override()
        if modules_dir==default_modules.resolve() and use_render_resolution:
            render_main,render_resolution=prepare_render_resolution.verified_override()
        for name in ('main','sdk','subsdk0'):
            for extension in ('patched.bin','fixups.tsv','gates.tsv'):
                source=modules_dir/f'{name}.{extension}'
                if offline_entry and name=='main' and extension=='patched.bin':
                    source=prepare_offline_entry.OUTPUT/source.name
                if render_resolution and name=='main' and extension=='patched.bin':
                    source=render_main
                shutil.copyfile(source,build/'assets/modules'/source.name)
                files.append({'name':source.name,'bytes':source.stat().st_size,'sha256':sha(source)})
        packaged_module_manifest={'files':files}
    (build/'assets/module-manifest.json').write_text(json.dumps(packaged_module_manifest),encoding='utf-8')
    sources=sorted((REPO/'dm/android/src').rglob('*.java'))
    run(JAVA/'bin/javac.exe','--release','8','-encoding','UTF-8','-cp',android,'-d',build/'classes',*sources)
    run(tools/'d8.bat','--min-api','29','--lib',android,'--output',build/'dex',*sorted((build/'classes').rglob('*.class')))
    # Classify the packaged native game without changing SDK levels or permissions.
    ET.register_namespace('android','http://schemas.android.com/apk/res/android')
    manifest_source=REPO/'dm/android/AndroidManifest.xml'
    manifest_tree=ET.parse(manifest_source)
    manifest_tree.getroot().set('package',package)
    application=manifest_tree.getroot().find('application')
    if application is None:raise RuntimeError('Android manifest has no application')
    application.set('{http://schemas.android.com/apk/res/android}appCategory','game')
    android_ns='{http://schemas.android.com/apk/res/android}'
    update_metadata=None
    for child in application.findall('meta-data'):
        if child.get(android_ns+'name')=='org.d2r.binaryprobe.UPDATE_URL':
            update_metadata=child
            break
    if update_metadata is None:
        update_metadata=ET.SubElement(application,'meta-data',{android_ns+'name':'org.d2r.binaryprobe.UPDATE_URL'})
    update_metadata.set(android_ns+'value',update_url)
    packaged_manifest=build/'AndroidManifest.xml'
    manifest_tree.write(packaged_manifest,encoding='utf-8',xml_declaration=True)
    resources=build/'resources.zip'
    run(tools/'aapt2.exe','compile','--dir',REPO/'dm/android/res','-o',resources)
    run(tools/'aapt2.exe','link','-o',build/'base.apk','--manifest',packaged_manifest,
        '-I',android,'--min-sdk-version','29','--target-sdk-version','33','-A',build/'assets',resources)
    with ZipFile(build/'base.apk','a',compression=ZIP_DEFLATED) as archive:
        archive.write(build/'dex/classes.dex','classes.dex');archive.write(native,'lib/arm64-v8a/'+native.name,compress_type=ZIP_STORED)
        driver_build=ART/'shader-driver-build'
        private_libraries=[driver_build/'libadrenotools.so',*(driver_build/'src/hook').glob('*.so'),
            SDK/'ndk/27.2.12479018/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so']
        validation_config=ART/'vulkan-validation-build.json'
        if validation_config.is_file():
            validation=json.loads(validation_config.read_text(encoding='utf-8'))
            layer=(ROOT/validation['path']).resolve()
            if not layer.is_relative_to(ROOT) or sha(layer)!=validation['sha256']: raise RuntimeError('Validation layer provenance mismatch')
            private_libraries.append(layer)
        for library in private_libraries:
            archive.write(library,'lib/arm64-v8a/'+library.name,compress_type=ZIP_STORED)
        archive.write(driver,'lib/arm64-v8a/libd2r_turnip.so',compress_type=ZIP_STORED)
    alignment={p.name:elf_alignment(p) for p in [native,*private_libraries,driver]}
    run(tools/'zipalign.exe','-f','-P','16','4',build/'base.apk',build/'aligned.apk')
    apk=output or ART/'d2r-mobile-android.apk'
    apk.parent.mkdir(parents=True,exist_ok=True)
    run(tools/'apksigner.bat','sign','--ks',Path(os.environ['USERPROFILE'])/'.android/debug.keystore',
        '--ks-pass','pass:android','--key-pass','pass:android','--out',apk,build/'aligned.apk')
    run(tools/'apksigner.bat','verify',apk)
    run(tools/'zipalign.exe','-c','-P','16','4',apk)
    badging=run(tools/'aapt2.exe','dump','badging',apk)
    permissions=sorted(set(re.findall(r"uses-permission:\s+name='([^']+)'",badging)))
    expected_permissions=['android.permission.INTERNET','android.permission.VIBRATE']
    if permissions!=expected_permissions:raise RuntimeError(f'Unexpected permissions: {permissions!r}')
    (ART/'apk-badging.txt').write_text(badging,encoding='utf-8')
    report={'apk':str(apk),'bytes':apk.stat().st_size,'sha256':sha(apk),
        'native_sha256':sha(native),'package':package,'requested_permissions':permissions,
        'private_driver_libraries':{p.name:sha(p) for p in private_libraries},
        'private_turnip_sha256':sha(driver),'private_driver_selection':driver_selection,
        'runtime_driver_policy':{'default':'auto','choices':['auto','system','turnip'],
            'auto_resolved':'system','auto_turnip_soc':[],'tested_turnip_sha256':tested_turnip},
        'module_assets':files,'modules_dir':str(modules_dir),'module_build_id':packaged_module_manifest.get('build_id'),
        'java_sources':{str(p.relative_to(REPO)):sha(p) for p in sources},
        'manifest_sha256':sha(REPO/'dm/android/AndroidManifest.xml'),
        'launcher_resources':{str(p.relative_to(REPO)):sha(p) for p in sorted((REPO/'dm/android/res').rglob('*')) if p.is_file()},
        'packaged_manifest_sha256':sha(packaged_manifest),'application_category':'game',
        'existing_original_image_reused':True,'cpu_interpreter_or_jit':False,'game_3d_frame_verified':False,
        'offline_manifest_sha256':sha(offline_manifest),'offline_payload_bytes':offline_data['total_bytes'],
        'offline_installer':True,'launcher':'org.d2r.binaryprobe.InstallerActivity'}
    report['offline_entry_policy']=offline_entry
    report['render_resolution_policy']=render_resolution
    report['native_16kb_load_segments']=alignment
    report['apk_16kb_zip_alignment_verified']=True
    report['update_url_configured']=bool(update_url)
    report_path=apk.with_suffix('.build.json') if output else ART/'apk-build.json'
    report_path.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'apk':str(apk),'bytes':apk.stat().st_size,'sha256':sha(apk)}))
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--render-resolution',action='store_true',help='Use the experimental pinned size variant; UI layout must be checked')
    parser.add_argument('--offline-manifest',type=Path,help='Pinned manifest produced with the offline resource pack')
    parser.add_argument('--output',type=Path,help='Write a separate APK and build report without replacing the development APK')
    parser.add_argument('--package',default='org.d2r.binaryprobe',help='Separate application ID for isolated installer QA')
    parser.add_argument('--update-url',help='Absolute HTTPS manifest URL for version information; DM_UPDATE_URL is used when omitted')
    parser.add_argument('--modules-dir',type=Path,help='Module directory with a verified module-manifest.json when available')
    args=parser.parse_args();main(args.render_resolution,args.offline_manifest,args.output,args.package,args.update_url,args.modules_dir)
