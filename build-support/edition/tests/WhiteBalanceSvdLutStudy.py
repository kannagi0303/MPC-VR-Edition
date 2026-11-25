# SPDX-License-Identifier: GPL-3.0-or-later
# [WB-05 | 2026-11-25 +08:00] CPU pinned-graph study. No renderer changes.
# Usage: python WhiteBalanceSvdLutStudy.py AT06_ROOT GRAPH B2_CPP_EXE CSV
# Compile existing AdaptiveToneMidtoneStudy.cpp with C++20 first.
import sys, csv, pathlib, contextlib,io,runpy,tempfile,subprocess
from pathlib import Path
import numpy as np
from PIL import Image
from skimage import data,color
import torch
import torch.nn.functional as F
from WhiteBalanceEstimatorStudy import linear,srgb,render,correct
from WhiteBalanceTemporalStudy import ge,bound
root,graph,exe,out=sys.argv[1:]; here=pathlib.Path(root)/'build-support/edition/tests';sys.path.insert(0,str(here));from SvdLutAdaptiveLumaModel import pack
sys.argv=[str(here/'SvdLutReferenceModel.py'),graph]
with contextlib.redirect_stdout(io.StringIO()): ref=runpy.run_path(sys.argv[0])
run=ref['run'];w=torch.tensor([.2126,.7152,.0722]);exe=str(pathlib.Path(exe).resolve())
# Reuse prior exact native B2.1 + vector B2.2 reference, not duplicate divergent formulas.
def resize(a,h,w):return F.interpolate(a.permute(2,0,1)[None],(h,w),mode='bilinear',align_corners=False)[0].permute(1,2,0).contiguous()

def project(c,y):
    f=torch.minimum((1-y)/c.amax(-1).clamp(min=1e-6),y/(-c).amax(-1).clamp(min=1e-6)).clamp(0,1)
    return c*f[...,None]

def model(thumb):return pack(run(thumb.permute(2,0,1)[None]))

def apply_ve(rgb,thumb,lut):
    samples=np.array([thumb[y+dy,x+dx].numpy() for y in range(0,256,16) for x in range(0,256,16) for dx,dy in [(4,12),(12,4)]],np.float32)
    with tempfile.TemporaryDirectory() as tmp:
        f=Path(tmp)/'input.bin';f.write_bytes(samples.tobytes()+lut.tobytes());alpha=float(subprocess.check_output([exe,str(f)],text=True))
    shape=rgb.shape;rgb=rgb.reshape(-1,3);r,g,b=rgb.T
    def plane(i,u,v):
        grid=torch.stack((2*u-1,2*v-1),-1).reshape(1,1,-1,2)
        return F.grid_sample(torch.from_numpy(lut[i,:,:,:3]).permute(2,0,1)[None],grid,align_corners=True,padding_mode='border')[0,:,0].T
    raw=(plane(0,r,g)+plane(1,r,b)+plane(2,g,b)).clamp(0,1);sy=rgb@w;gy=raw@w;y=(sy+(alpha*(gy-sy)).clamp(-4/255,4/255)).clamp(0,1)
    c=project(raw-gy[:,None],y);base=project(rgb-sy[:,None],y);d=c-base;radius=d.abs().amax(-1);limit=2/255+1.5*(rgb-sy[:,None]).abs().amax(-1);knee=.65*limit;excess=(radius-knee).clamp(min=0);span=limit-knee;scale=torch.where(radius>knee,(knee+span*excess/(span+excess))/radius.clamp(min=1e-6),1.)
    result=(y[:,None]+base+d*scale[:,None]).clamp(0,1)
    assert float(((result@w)-sy).abs().max()*255)<4.002
    return result.reshape(shape),alpha
def ve(a,lut=None,reference=None):
    t=torch.tensor(a,dtype=torch.float32); thumb=resize(t,256,256);ref=thumb if reference is None else resize(torch.tensor(reference,dtype=torch.float32),256,256)
    return apply_ve(t,ref,model(thumb) if lut is None else lut)[0].numpy()
def lutof(a):return model(resize(torch.tensor(a,dtype=torch.float32),256,256))
def wb(a,e,s):return np.clip(srgb(correct(linear(a),e,s)),0,1)
def err(a,b):return float(color.deltaE_ciede2000(color.rgb2lab(a),color.rgb2lab(b)).mean())
rows=[]
for name in ('astronaut','coffee','chelsea','rocket','grass','retina'):
    a=np.asarray(Image.fromarray(getattr(data,name)()).convert('RGB').resize((192,128)),dtype=float)/255; lin=linear(a)
    for domain in ('linear','post_isp'):
        neutral=a if domain=='linear' else render(lin); target=ve(neutral)
        for cast_name,cast in [('none',[1,1,1]),('warm',[1.24,1,.76]),('green',[.92,1.22,.90])]:
            raw=np.clip(srgb(lin*np.array(cast)),0,1) if domain=='linear' else render(lin*np.array(cast));e=ge(linear(raw));e=bound(e) if e is not None else np.zeros(3)
            oldlut=lutof(raw);base=ve(raw,oldlut)
            for s in (.5,1.):
                corrected=wb(raw,e,s);matched=ve(corrected);stale=ve(corrected,oldlut,raw);post=wb(base,e,s)
                for method,b,t in [('off',raw,neutral),('wb_only',corrected,neutral),('ve_only',base,target),('wb_pre_matched',matched,target),('wb_pre_stale',stale,target),('wb_post',post,target)]:
                    rows.append(dict(scene=name,domain=domain,cast=cast_name,strength=s,method=method,de=err(b,t),mean_delta_to_matched=err(b,matched) if method.startswith('wb_pre') or method=='wb_post' else ''))
        print(name,domain,flush=True)
with open(out,'w',newline='') as f:wr=csv.DictWriter(f,fieldnames=rows[0]);wr.writeheader();wr.writerows(rows)
print('rows',len(rows))
