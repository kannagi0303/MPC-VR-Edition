# SPDX-License-Identifier: GPL-3.0-or-later
# [WB-05 | 2026-11-25 +08:00] Offline synthetic motion study. No renderer changes.
import csv,sys
import numpy as np
import cv2
from PIL import Image
from skimage import data
from scipy.ndimage import gaussian_filter,minimum_filter,maximum_filter
from WhiteBalanceEstimatorStudy import linear,srgb,render,chroma
cv2.setNumThreads(1)
def ge(x):
    valid=(minimum_filter(x.min(2),size=5)>.008)&(maximum_filter(x.max(2),size=5)<.97)
    if valid.sum()<64:return None
    dy,dx=np.gradient(gaussian_filter(x,(1,1,0)),axis=(0,1));return chroma(np.sqrt(np.mean((dx*dx+dy*dy)[valid],axis=0)))
def bound(z):
    z=np.asarray(z)-np.mean(z);return z*min(1,.35/max(1e-9,np.max(np.abs(z))))
def track(a,b):
    def gray(x):return cv2.cvtColor(np.uint8(np.clip(srgb(x),0,1)*255+.5),cv2.COLOR_RGB2GRAY)
    ga,gb=gray(a),gray(b);p=cv2.goodFeaturesToTrack(ga,160,.015,5,blockSize=5)
    if p is None or len(p)<12:return None
    q,s,_=cv2.calcOpticalFlowPyrLK(ga,gb,p,None,winSize=(21,21),maxLevel=2)
    if q is None:return None
    r,t,_=cv2.calcOpticalFlowPyrLK(gb,ga,q,None,winSize=(21,21),maxLevel=2)
    if r is None:return None
    p,q,r=p[:,0],q[:,0],r[:,0];h,w=ga.shape
    valid=(s[:,0]>0)&(t[:,0]>0)&(np.linalg.norm(p-r,axis=1)<.75)
    for xy in (p,q):valid&=(xy[:,0]>3)&(xy[:,1]>3)&(xy[:,0]<w-4)&(xy[:,1]<h-4)
    p,q=p[valid],q[valid]
    if len(p)<12:return None
    def sample(x,xy):return cv2.remap(cv2.blur(x.astype(np.float32),(5,5)),xy[:,0].astype(np.float32).reshape(-1,1),xy[:,1].astype(np.float32).reshape(-1,1),cv2.INTER_LINEAR)[:,0]
    u,v=sample(a,p),sample(b,q);mask=(u.min(1)>.015)&(v.min(1)>.015)&(u.max(1)<.90)&(v.max(1)<.90)
    p,u,v=p[mask],u[mask],v[mask]
    if len(p)<12:return None
    d=chroma(v)-chroma(u);med=np.median(d,axis=0);good=np.linalg.norm(d-med,axis=1)<.055
    tiles=np.unique((p[good,0]//(w/4)).astype(int)+4*(p[good,1]//(h/4)).astype(int))
    if good.sum()<12 or good.mean()<.65 or len(tiles)<4:return None
    return np.median(d[good],axis=0)
def main(out):
    rows=[]
    for scene in ['astronaut','coffee','chelsea','rocket','grass','retina']:
        img=linear(np.asarray(Image.fromarray(getattr(data,scene)()).convert('RGB').resize((224,144)),dtype=float)/255.)
        for domain in ('linear','post_isp'):
            for mode in ('pan','cast_ramp','exposure'):
                prev=None;ests={};last={};relative=np.zeros(3)
                for i in range(36):
                    x0=round(64*i/35);y0=round(48*i/35);src=img[y0:y0+96,x0:x0+160]
                    f=np.clip((i-8)/20,0,1) if mode=='cast_ramp' else 0
                    cast=np.exp(np.array([.20,0,-.20])+f*np.array([-.32,.14,.18]));exposure=1-.65*i/35 if mode=='exposure' else 1.
                    ref=src*exposure;raw=np.clip(ref*cast,0,1) if domain=='linear' else linear(render(ref*cast));gt=ge(raw)
                    if gt is None:gt=ests.get('frame',np.zeros(3))
                    if prev is None:
                        for k in ('frame','ema','anchor','track'):ests[k]=bound(gt)
                        anchor=ests['anchor'].copy();applied=anchor.copy();accepted=False
                    else:
                        ests['frame']=bound(gt);ests['ema']=bound(.88*ests['ema']+.12*gt);d=track(prev,raw);accepted=d is not None
                        if accepted:relative=bound(relative+d)
                        applied=.75*applied+.25*bound(anchor+relative);ests['track']=bound(applied)
                    for method,e in ests.items():
                        actual=chroma(cast)
                        rows.append(dict(scene=scene,domain=domain,mode=mode,frame=i,method=method,step=0 if method not in last else float(np.linalg.norm(e-last[method])),drift=float(np.linalg.norm(e-anchor)),relative_error=float(np.linalg.norm((e-anchor)-(actual-chroma(np.exp([.20,0,-.20]))))) if domain=='linear' else '',accepted=int(accepted)))
                        last[method]=e.copy()
                    prev=raw
    with open(out,'w',newline='') as f:w=csv.DictWriter(f,fieldnames=rows[0]);w.writeheader();w.writerows(rows)
    print('rows',len(rows))
if __name__=='__main__':main(sys.argv[1])
