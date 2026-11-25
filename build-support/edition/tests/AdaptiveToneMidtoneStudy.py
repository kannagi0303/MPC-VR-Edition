# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline AT-A/B/C and static real-graph order study. No renderer changes.
Usage: python AdaptiveToneMidtoneStudy.py GRAPH CXX_EXE OUTPUT_DIR HAZE MANUAL AT_ON AT_OFF CARD1 CARD2 CARD3
"""
import contextlib,io,runpy,sys,subprocess,csv,tempfile
from pathlib import Path
import numpy as np
from PIL import Image
from skimage import data
import torch
import torch.nn.functional as F
from SvdLutAdaptiveLumaModel import pack
here=Path(__file__).resolve().parent
_,graph,exe,out,*paths=sys.argv; exe=str(Path(exe).resolve()); out=Path(out);out.mkdir(parents=True,exist_ok=True)
sys.argv=[str(here/'SvdLutReferenceModel.py'),graph]
with contextlib.redirect_stdout(io.StringIO()): ref=runpy.run_path(sys.argv[0])
run=ref['run']; w=torch.tensor([.2126,.7152,.0722]); X=torch.linspace(0,1,257)
def image(p):
    a=Image.open(p).convert('RGB');W,H=a.size
    return np.asarray(a.crop((int(.025*W),int(.09*H),int(.975*W),int(.91*H))))/255

def resize(a,h,w):return F.interpolate(a.permute(2,0,1)[None],(h,w),mode='bilinear',align_corners=False)[0].permute(1,2,0).contiguous()
def project(c,y):
    f=torch.minimum((1-y)/c.amax(-1).clamp(min=1e-6),y/(-c).amax(-1).clamp(min=1e-6)).clamp(0,1)
    return c*f[...,None]
def map_rgb(rgb,curve):
    y=rgb@w;p=y.clamp(0,1)*256;i=p.long().clamp(max=255);z=curve[i]+(p-i)*(curve[i+1]-curve[i]);return (z[...,None]+project(rgb-y[...,None],z)).clamp(0,1)
def curves(a):
    h=np.histogram((resize(a,144,256)@w).numpy(),bins=256,range=(0,1))[0]
    p=subprocess.check_output([exe,'hist','curves'],input=' '.join(map(str,h)),text=True)
    return [torch.tensor([float(x) for x in l.split()]) for l in p.splitlines()]
def stats(rgb):
    y=(rgb@w).flatten();q=torch.quantile(y,torch.tensor([.01,.10,.25,.5,.75,.90,.95,.99]))*255
    return dict(zip(['p01','p10','p25','p50','p75','p90','p95','p99'],[round(float(v),3) for v in q]))
def delta(a,b):
    d=b-a;dy=d@w;return dict(mean_abs_y=round(float(dy.abs().mean()*255),4),mean_abs_chroma=round(float((d-dy[...,None]).abs().mean()*255),4),max_abs_rgb=round(float(d.abs().max()*255),4))
def write(name,rows):
    with (out/name).open('w',newline='') as f: c=csv.DictWriter(f,fieldnames=rows[0].keys());c.writeheader();c.writerows(rows)
labels=['haze','manual','reported_stronger','reported_haze','card1','card2','card3'];cases=[(n,image(p)) for n,p in zip(labels,paths)]
for n in ('astronaut','coffee','chelsea','rocket','camera','coins','grass','colorwheel'):
    a=np.asarray(Image.fromarray(getattr(data,n)()).convert('RGB').resize((320,240)))/255
    for tag,b in [('original',a),('haze',.55*a+.27),('low',.38*a),('high',.38*a+.62)]:cases.append((n+'_'+tag,b))
rows=[]; cr=[]; cache={}
for name,arr in cases:
    a=torch.tensor(arr,dtype=torch.float32);rgb=resize(a,360,640); cs=curves(a);cache[name]=(a,cs)
    for tag,c in zip(('identity','A','B','C'),[X]+cs):
        b=map_rgb(rgb,c);d=(c[1:]-c[:-1])*256
        rows.append(dict(case=name,method=tag,**stats(b),**delta(rgb,b),min_slope=round(float(d.min()),5),max_slope=round(float(d.max()),5),shadow_slope_min=round(float(d[:64].min()),5),highlight_slope_min=round(float(d[192:].min()),5),max_curve_delta=round(float((c-X).abs().max()*255),4)))
        if name in labels:
            for i,v in enumerate(c):cr.append(dict(case=name,method=tag,input=i/256,output=float(v)))
write('adaptive-tone-at04-curves.csv',rows);write('adaptive-tone-at04-curve-knots.csv',cr)
# Real graph + packed interior LUT, native B2.1 estimator; B2.2 vector translation.
# No temporal B1, preferred/skin, depth, dither or DirectML execution.
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
orders=[]
for name in ['haze','manual','astronaut_haze','coffee_haze','chelsea_haze','rocket_haze']:
    a,cs=cache[name];rgb=resize(a,360,640);thumb=resize(a,256,256);lut=model(thumb);v,al=apply_ve(rgb,thumb,lut)
    for tag,c in zip(('A','B','C'),cs):
        t=map_rgb(rgb,c);tt=map_rgb(thumb,c);post=map_rgb(v,c);frozen,af=apply_ve(t,tt,lut);matched,am=apply_ve(t,tt,model(tt))
        for order,b,alpha in [('post',post,al),('pre_frozen_model',frozen,af),('pre_matched_model',matched,am)]:
            orders.append(dict(case=name,method=tag,order=order,alpha=alpha,**stats(b),**{'vs_AT_'+k:v for k,v in delta(t,b).items()},**{'vs_post_'+k:v for k,v in delta(post,b).items()}))
    print('graph order done',name,flush=True)
write('adaptive-tone-at04-order.csv',orders)
