# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline actual-graph/B2 comparison. Usage: GRAPH CPP_TEST_EXE [IMAGES...].
Developer dependencies only: numpy, torch, Pillow, scikit-image.
"""
import pathlib, runpy, sys, tempfile, subprocess, json, contextlib, io
import numpy as np
from PIL import Image
from skimage import data
import torch
import torch.nn.functional as F
def pack(out):
    l,lm,g,gm=[x.numpy() for x in out];l=l.reshape(3,3,33,33);lm=lm.reshape(3,4);g=g.reshape(6,3,17,17);gm=gm.reshape(2,12)
    packed=np.zeros((12,33,33,4),np.float32)
    for variant in range(4):
        gx=16 if variant&1 else 0;gy=16 if variant&2 else 0
        for c in range(3):
            bias=lm[c,3];curve=np.zeros(17,np.float32)
            for j in range(2):
                group=2*c+j;planes=g[group];m=gm[group//3];k=group%3;w=m[k*3:k*3+3]
                bias+=m[9+k]+planes[0,gy,gx]*w[0]
                curve+=planes[1,:,gx]*w[1]+planes[2,:,gy]*w[2]
            corr=(curve[np.arange(33)//2]+curve[(np.arange(33)+1)//2])*.5+bias
            for i in range(3):
                plane=l[c,i]*lm[c,i]
                if i==(1 if c==2 else 0):plane=plane+(corr[None,:] if c==0 else corr[:,None])
                packed[3*variant+i,:,:,c]=plane
    return packed

def weights(y):
    def smooth(a,b):
        v=((y-a)/(b-a)).clamp(0,1);return v*v*(3-2*v)
    a,b=smooth(.12,.52),smooth(.52,.90)
    return torch.stack(((1-a)*(1-b),a*(1-b),b),-1)

def compare(name,im):
    im=torch.from_numpy(np.array(im,dtype=np.float32)/255).permute(2,0,1)[None]
    thumb=F.interpolate(im,(256,256),mode='bilinear',align_corners=False)
    out=run(thumb);packed=pack(out)
    src=thumb[0].permute(1,2,0).contiguous()
    samples=np.array([src[y+dy,x+dx].numpy() for y in range(0,256,16) for x in range(0,256,16) for dx,dy in [(4,12),(12,4)]],np.float32)
    with tempfile.TemporaryDirectory() as d:
        f=pathlib.Path(d)/'case.bin';f.write_bytes(samples.tobytes()+packed.tobytes())
        values=list(map(float,subprocess.check_output([str(runner),str(f)],text=True).split()))
    rgb=src.reshape(-1,3);r,g,b=rgb.T
    def plane(i,u,v):
        grid=torch.stack((u*2-1,v*2-1),-1).reshape(1,1,-1,2)
        return F.grid_sample(torch.from_numpy(packed[i,:,:,:3]).permute(2,0,1)[None],grid,align_corners=True,padding_mode='border')[0,:,0].T
    raw=shade(out,rgb);actual=(plane(0,r,g)+plane(1,r,b)+plane(2,g,b)).clamp(0,1)
    assert float((actual-raw).abs().max())<2e-6
    luma=torch.tensor([.2126,.7152,.0722]);w=weights(rgb@luma);gain=w@torch.tensor(values[1:4]);gain=torch.minimum(gain,1/raw.amax(1).clamp(min=.0001));grade=(raw*gain[:,None]).clamp(0,1)
    sy=rgb@luma; gy=raw@luma
    target=(sy+(values[0]*(gy-sy)).clamp(-4/255,4/255)).clamp(0,1)
    c=raw-gy[:,None]
    fit=torch.minimum((1-target)/c.amax(1).clamp(min=1e-6),target/(-c).amax(1).clamp(min=1e-6)).clamp(0,1)
    adapt=(target[:,None]+c*fit[:,None]).clamp(0,1)
    assert ((adapt@luma-sy).abs().max()*255)<4.001
    def stats(g):
        diff=(g-rgb)*255; y=(g@luma).reshape(256,256);sy=(rgb@luma).reshape(256,256)
        gd=(y[:,1:]-y[:,:-1])*255;sd=(sy[:,1:]-sy[:,:-1])*255
        mask=(sd.abs()>=.75)&(sd.abs()<=18)&((src[:,1:].amax(-1)-src[:,1:].amin(-1))<.06)&((src[:,:-1].amax(-1)-src[:,:-1].amin(-1))<.06)
        ratios=gd[mask]/sd[mask];
        return dict(mean_y_code=float((diff@luma).abs().mean()),mean_chroma_delta=float((diff-(diff@luma)[:,None]).abs().mean()),mean_rgb_code=float(diff.abs().mean()),max_rgb_code=float(diff.abs().max()),neutral_gradient_p95=float(ratios.quantile(.95)) if ratios.numel() else None,gradient_reversals=int((ratios<0).sum()))
    return dict(name=name,participation=values[0],old_tone=values[1:4],cpu_us=values[4],raw=stats(raw),b2=stats(grade),adaptive=stats(adapt))
if __name__ == '__main__':
    args=sys.argv[1:]; runner=pathlib.Path(args[1]).resolve(); images=args[2:]
    sys.argv=[str(pathlib.Path(__file__).with_name('SvdLutReferenceModel.py')),args[0]]
    with contextlib.redirect_stdout(io.StringIO()): ref=runpy.run_path(sys.argv[0])
    run,shade=ref['run'],ref['shade']
    for name,im in [(pathlib.Path(f).stem,np.asarray(Image.open(f).convert('RGB'))) for f in images]+[(n,getattr(data,n)()) for n in ('astronaut','coffee','chelsea','rocket')]:
        print(json.dumps(compare(name,im)),flush=True)
