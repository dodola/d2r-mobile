"""Verify the runtime ballot lowering with real system-driver GPU readback.

Exercises divergent control flow, true/false predicates, multiple subgroups,
partial subgroups and untouched output sentinels. Requires an explicit device.
"""
from pathlib import Path
import argparse,gzip,hashlib,json,os,struct,subprocess
REPO=Path(__file__).resolve().parents[2]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial',required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--cache',type=Path,help='Also validate all compute modules from the installed resource-cache source')
    args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    sdk=Path(os.environ['LOCALAPPDATA'])/'Android/Sdk';ndk=sdk/'ndk/27.2.12479018'
    clang=ndk/'toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android29-clang.cmd'
    tools=ndk/'shader-tools/windows-x86_64';adb=['C:/tools/platform-tools/adb.exe','-s',args.serial]
    remote='/data/local/tmp/d2r-system-ballot-test'
    def run(*command):
        r=subprocess.run(list(map(str,command)),capture_output=True,timeout=60)
        if r.returncode:raise RuntimeError(f'{command}: '+(r.stdout+r.stderr).decode(errors='replace'))
        return r.stdout
    run(*adb,'shell','mkdir','-p',remote)
    for name,libraries in [('ballot_transform',[]),('ballot_readback_probe',['-ldl'])]:
        run(clang,'-std=c11','-O2','-Wall','-Wextra','-Werror',REPO/f'dm/android/tests/{name}.c',*libraries,'-o',out/name)
        run(*adb,'push',out/name,remote+'/'+name);run(*adb,'shell','chmod','700',remote+'/'+name)
    reports=[]
    for local_size,explicit_lane in ((32,True),(128,True),(32,False)):
        code=f'''#version 450
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_ballot : require
layout(local_size_x={local_size}) in;
layout(set=0,binding=0,std430) buffer Output {{uint data[];}};
void main() {{
    uint local=gl_LocalInvocationID.x;
    uint row=gl_GlobalInvocationID.x*64;
    data[row]=local;data[row+1]=gl_SubgroupInvocationID;
    data[row+2]=gl_SubgroupID;data[row+3]=gl_WorkGroupID.x;
'''
        for i in range(12):
            code+=f'''    if((local+{i})%5!=0) {{
        uvec4 vote=subgroupBallot((local+{i})%7<3);
        data[row+{4+i*4}]=vote.x;data[row+{5+i*4}]=vote.y;
        data[row+{6+i*4}]=vote.z;data[row+{7+i*4}]=vote.w;
    }}
'''
        code+='}\n'
        if not explicit_lane:
            code=code.replace('gl_SubgroupInvocationID','local').replace('gl_SubgroupID','0')
        prefix=f'local-{local_size}'+('' if explicit_lane else '-injected-lane');source=out/(prefix+'.comp');source.write_text(code,encoding='ascii')
        original=out/(prefix+'.spv');lowered=out/(prefix+'.lowered.spv')
        run(tools/'glslc.exe','--target-env=vulkan1.1','-O0',source,'-o',original)
        run(tools/'spirv-val.exe','--target-env','vulkan1.1',original)
        run(*adb,'push',original,remote+'/'+original.name)
        transform=run(*adb,'shell',remote+'/ballot_transform',remote+'/'+original.name,remote+'/'+lowered.name).decode().strip()
        run(*adb,'pull',remote+'/'+lowered.name,lowered)
        run(tools/'spirv-val.exe','--target-env','vulkan1.1',lowered)
        runtime=run(*adb,'shell',remote+'/ballot_readback_probe',remote+'/'+lowered.name,remote+'/'+prefix).decode()
        raw=out/(prefix+'.readback.bin');run(*adb,'pull',remote+'/'+prefix+'.lowered.bin',raw)
        data=raw.read_bytes();words=struct.unpack('<%dI'%(len(data)//4),data);rows=[words[i*64:(i+1)*64] for i in range(2*local_size)]
        expected=[0xdeadbeef]*len(words)
        for invocation,row in enumerate(rows):
            local,lane,subgroup,workgroup=row[:4]
            assert local==invocation%local_size and workgroup==invocation//local_size and lane<128
            expected[invocation*64:invocation*64+4]=row[:4]
            for i in range(12):
                if (local+i)%5==0:continue
                mask=[0]*4
                for peer in rows:
                    if peer[2]==subgroup and peer[3]==workgroup and (peer[0]+i)%5!=0 and (peer[0]+i)%7<3:
                        mask[peer[1]//32]|=1<<(peer[1]%32)
                expected[invocation*64+4+i*4:invocation*64+8+i*4]=mask
        errors=[i for i,(a,b) in enumerate(zip(words,expected)) if a!=b]
        report={'local_size':local_size,'explicit_lane_input':explicit_lane,'groups':2,'ballot_sites':12,'mismatch_count':len(errors),
            'transform':transform,'runtime':json.loads(runtime),'readback_sha256':hashlib.sha256(data).hexdigest(),
            'lowered_spirv_sha256':hashlib.sha256(lowered.read_bytes()).hexdigest()}
        reports.append(report);print(json.dumps(report),flush=True)
        if errors:raise AssertionError(f'GPU results differ at words {errors[:20]}')
        if report['runtime']['driver_id']!=8:raise AssertionError('Expected the Qualcomm system driver')
    cache_reports=[]
    if args.cache:
        for meta in sorted(args.cache.glob('*.vkmeta')):
            if struct.unpack_from('<12I',meta.read_bytes())[2]!=5:continue
            data=gzip.decompress(meta.with_suffix('.spv.gz').read_bytes())
            original=out/(meta.stem+'.spv');original.write_bytes(data);lowered=out/(meta.stem+'.lowered.spv')
            run(*adb,'push',original,remote+'/'+original.name)
            status=run(*adb,'shell',remote+'/ballot_transform',remote+'/'+original.name,remote+'/'+lowered.name).decode().strip()
            run(*adb,'pull',remote+'/'+lowered.name,lowered)
            run(tools/'spirv-val.exe','--target-env','vulkan1.1',lowered)
            if status.startswith('ballots=0 ') and lowered.read_bytes()!=data:raise AssertionError('Ballot-free module was changed')
            cache_reports.append({'key':meta.stem,'transform':status,'validated':True,
                'input_sha256':hashlib.sha256(data).hexdigest(),'output_sha256':hashlib.sha256(lowered.read_bytes()).hexdigest()})
        print(f'Validated {len(cache_reports)} real compute modules',flush=True)
    source_hashes={str(p.relative_to(REPO)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [
        REPO/'dm/gfx/nvn/nvn_vk_ballot.h',REPO/'dm/android/tests/ballot_transform.c',REPO/'dm/android/tests/ballot_readback_probe.c',Path(__file__)]}
    (out/'result.json').write_text(json.dumps({'serial':args.serial,'system_driver_only':True,'passed':True,'cases':reports,
        'real_compute_modules':cache_reports,'source_sha256':source_hashes},indent=2)+'\n',encoding='utf-8')
if __name__=='__main__':main()
