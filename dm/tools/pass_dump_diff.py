"""Decode two directories of pass-*.bin dumps (DM_DIAG_DUMP_FRAME / diagnostics/dump-frame.txt) and diff them pass by pass.
usage: pass_dump_diff.py <dirA> <dirB> <out-dir>   (writes side-by-side PNGs and prints mean/large-difference per pass)"""
import sys,os,re,glob
import numpy as np
from PIL import Image
def decode(path,w,h,fmt):
    raw=np.fromfile(path,dtype=np.uint8)
    if fmt==37:   # R8G8B8A8_UNORM
        a=raw.reshape(h,w,4)[:,:,:3].astype(np.float32)/255
    elif fmt==122: # B10G11R11_UFLOAT
        v=raw.view('<u4').reshape(h,w)
        def f(bits,mant,x):
            e=(x>>mant)&((1<<(bits-mant))-1); m=x&((1<<mant)-1)
            val=np.where(e==0,m/(1<<mant)*2.0**-14,(1+m/(1<<mant))*2.0**(e.astype(np.float32)-15))
            return val
        r=f(11,6,v&0x7ff); g=f(11,6,(v>>11)&0x7ff); b=f(10,5,(v>>22)&0x3ff)
        a=np.stack([r,g,b],-1).astype(np.float32)
    elif fmt==97: # RGBA16F
        a=raw.view('<f2').reshape(h,w,4)[:,:,:3].astype(np.float32)
    elif fmt==129:
        v=raw.view('<u4').reshape(h,w)&0xffffff
        a=np.repeat((v.astype(np.float32)/16777215)[:,:,None],3,2); return np.clip(a,0,1)
    elif fmt==124:
        v=raw[:w*h*2].view('<u2').reshape(h,w).astype(np.float32)/65535
        a=np.repeat(v[:,:,None],3,2); return np.clip(a,0,1)
    elif fmt in (76,70):
        a=raw.view('<f2' if fmt==76 else '<u2').reshape(h,w).astype(np.float32)
        if fmt==70:a/=65535
        a=np.stack([a,a,a],-1)
    else:
        return None
    a=np.nan_to_num(a,nan=0,posinf=1,neginf=0)
    a=np.clip(a/(1+a),0,1)**(1/2.2)   # simple tone map for HDR
    return a
def thumb(a,maxw=640):
    h,w,_=a.shape; s=min(1.0,maxw/w)
    im=Image.fromarray((a*255).astype(np.uint8))
    return im.resize((max(1,int(w*s)),max(1,int(h*s))),Image.NEAREST) if s<1 else im
A,B,out=sys.argv[1:4]
os.makedirs(out,exist_ok=True)
rows=[]
for fa in sorted(glob.glob(A+'/pass-*.bin')):
    name=os.path.basename(fa); m=re.match(r'pass-(\d+)-(\d+)x(\d+)-fmt(\d+)-bpp(\d+)-s(\d+)',name)
    idx,w,h,fmt=int(m.group(1)),int(m.group(2)),int(m.group(3)),int(m.group(4))
    fb=B+'/'+name
    if not os.path.exists(fb): print(idx,'missing in B'); continue
    da,db=decode(fa,w,h,fmt),decode(fb,w,h,fmt)
    if da is None: print(idx,w,h,fmt,'skip fmt'); continue
    d=float(np.abs(da-db).mean())
    # fraction of pixels differing strongly
    big=float((np.abs(da-db).max(-1)>0.25).mean())
    print('pass %03d %4dx%-4d fmt%-3d s%-3s meanabs=%.4f big=%.3f'%(idx,w,h,fmt,m.group(6),d,big))
    ta,tb=thumb(da),thumb(db)
    sheet=Image.new('RGB',(ta.width+tb.width+4,ta.height),(255,0,255)); sheet.paste(ta,(0,0)); sheet.paste(tb,(ta.width+4,0))
    sheet.save('%s/pass-%03d.png'%(out,idx))
