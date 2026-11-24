# SPDX-License-Identifier: GPL-3.0-or-later
"""Run with GRAPH [IMAGES ...]; g++, numpy, torch, Pillow, scikit-image are
optional developer dependencies, not renderer runtime dependencies.
Compiles the actual scalar shader helper as C++ (not a GPU shader compiler).
Uses the real manifest graph, B2.1 CPU estimator, and four natural images.
"""
import contextlib, io, json, os, pathlib, runpy, subprocess, sys, tempfile
import numpy as np
import torch
import torch.nn.functional as F
from PIL import Image
from skimage import data
from SvdLutAdaptiveLumaModel import pack
here = pathlib.Path(__file__).resolve().parent
root = here.parents[2]
graph = pathlib.Path(sys.argv[1]).resolve()
images = [pathlib.Path(s).resolve() for s in sys.argv[2:]]
sys.argv = [str(here/'SvdLutReferenceModel.py'), str(graph)]
with contextlib.redirect_stdout(io.StringIO()):
    ref = runpy.run_path(sys.argv[0])
run, shade = ref['run'], ref['shade']
w = torch.tensor([.2126,.7152,.0722])
def project(c,y):
    f = torch.minimum((1-y)/c.amax(-1).clamp(min=1e-6), y/(-c).amax(-1).clamp(min=1e-6)).clamp(0,1)
    return c*f[:,None]
def stat(src,g):
    d=g-src; dy=d@w; c=d-dy[:,None]
    return dict(mean_chroma_code=float(c.abs().mean()*255), max_chroma_code=float(c.abs().max()*255), mean_y_code=float(dy.abs().mean()*255))
with tempfile.TemporaryDirectory() as tmp:
    tmp=pathlib.Path(tmp)
    shader=(root/'Source/DLSS/SvdLutShader.h').read_text().split('inline constexpr char SvdLutAdaptiveLumaShader[] = R"(',1)[1].split(')";',1)[0]
    (tmp/'SvdLutAdaptiveChromaShader.inc').write_text(shader)
    compiler=os.environ.get('CXX','g++')
    chroma=tmp/'chroma'; luma=tmp/'luma'
    subprocess.run([compiler,'-std=c++20','-O2','-I',str(tmp),str(here/'SvdLutAdaptiveChroma.cpp'),'-o',str(chroma)],check=True)
    subprocess.run([compiler,'-std=c++20','-O2',str(here/'SvdLutAdaptiveLuma.cpp'),'-o',str(luma)],check=True)
    print(subprocess.check_output([str(chroma)],text=True).strip(),file=sys.stderr)
    print(subprocess.check_output([str(luma)],text=True).strip(),file=sys.stderr)
    cases=[(p.stem,np.asarray(Image.open(p).convert('RGB'))) for p in images]
    cases += [(n,getattr(data,n)()) for n in ('astronaut','coffee','chelsea','rocket')]
    for name,im in cases:
        im=torch.from_numpy(np.array(im,dtype=np.float32)/255).permute(2,0,1)[None]
        thumb=F.interpolate(im,(256,256),mode='bilinear',align_corners=False)
        out=run(thumb); packed=pack(out); src=thumb[0].permute(1,2,0).contiguous(); rgb=src.reshape(-1,3)
        raw=shade(out,rgb)
        samples=np.array([src[y+dy,x+dx].numpy() for y in range(0,256,16) for x in range(0,256,16) for dx,dy in [(4,12),(12,4)]],np.float32)
        f=tmp/'case.bin'; f.write_bytes(samples.tobytes()+packed.tobytes())
        alpha=float(subprocess.check_output([str(luma),str(f)],text=True).split()[0])
        sy=rgb@w; gy=raw@w; y=(sy+(alpha*(gy-sy)).clamp(-4/255,4/255)).clamp(0,1)
        c=project(raw-gy[:,None],y); base=project(rgb-sy[:,None],y)
        b21=(y[:,None]+c).clamp(0,1); delta=c-base; radius=delta.abs().amax(-1)
        limit=2/255+1.5*(rgb-sy[:,None]).abs().amax(-1); knee=.65*limit
        excess=(radius-knee).clamp(min=0); span=limit-knee
        compressed=knee+span*excess/(span+excess)
        scale=torch.where(radius>knee,compressed/radius.clamp(min=1e-6),1.)
        vector=(y[:,None]+base+delta*scale[:,None]).clamp(0,1)
        f=tmp/'pairs.bin'; dst=tmp/'out.bin'; f.write_bytes(torch.cat((rgb,b21),dim=1).numpy().astype('<f4').tobytes())
        subprocess.run([str(chroma),str(f),str(dst)],check=True)
        b22=torch.from_numpy(np.fromfile(dst,dtype='<f4').reshape(-1,3))
        agreement=float((b22-vector).abs().max()*255)
        yerr=float(((b22-b21)@w).abs().max()*255)
        assert agreement<.001 and yerr<.001
        assert b22.min()>=0 and b22.max()<=1
        a,b=stat(rgb,b21),stat(rgb,b22)
        neutral=(rgb.amax(-1)-rgb.amin(-1))<=1/255
        def neutral_tint(g):
            ch=g-(g@w)[:,None]
            return float(ch[neutral].abs().mean()*255) if neutral.any() else None
        print(json.dumps(dict(name=name,alpha=alpha,b21=a,b22=b,retained_pct=b['mean_chroma_code']/max(a['mean_chroma_code'],1e-8)*100,active_pct=float((radius>knee).float().mean()*100),neutral_count=int(neutral.sum()),neutral_tint_b21=neutral_tint(b21),neutral_tint_b22=neutral_tint(b22),max_luma_change_code=yerr,scalar_vector_error_code=agreement)),flush=True)
