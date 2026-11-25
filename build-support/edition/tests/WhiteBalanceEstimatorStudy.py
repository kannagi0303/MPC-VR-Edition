# SPDX-License-Identifier: GPL-3.0-or-later
"""WB-04 screening, 2026-11-25. No renderer dependencies or third-party model code.
Usage: python WhiteBalanceEstimatorStudy.py output.csv
GI-inspired approximation (not authors' exact implementation): LoG of log chromaticity,
texture mask, 7x7 smoothing, 0.1% selection. Includes a conservative agreement gate.
Synthetic casts are known; references are not calibrated real-scene ground truth.
"""
import csv, sys
import numpy as np
from scipy.ndimage import gaussian_filter, gaussian_laplace, uniform_filter, maximum_filter, minimum_filter
from PIL import Image
from skimage import data, color
LUMA=np.array([.2126,.7152,.0722])
M=np.array([[1.10,-.06,-.04],[-.03,1.07,-.04],[.02,-.08,1.06]])
def linear(x): return np.where(x<=.04045,x/12.92,((x+.055)/1.055)**2.4)
def srgb(x): return np.where(x<=.0031308,12.92*x,1.055*np.maximum(x,0)**(1/2.4)-.055)
def render(x):
    q=np.maximum(x@M.T,0)**np.array([.94,1.04,1.12]); return np.clip(srgb(q/(1+.18*q)*1.12),0,1)
def smooth(a,b,x):
    t=np.clip((x-a)/(b-a),0,1);return t*t*(3-2*t)
def chroma(x):
    v=np.log(np.maximum(x,1e-6)); return v-v.mean(axis=-1,keepdims=True)
def estimate(x):
    # input is decoded linear sRGB, not sensor RGB. Mask shadows/clipped neighbourhoods.
    valid=(minimum_filter(x.min(2),size=5)>.008)&(maximum_filter(x.max(2),size=5)<.97)
    if valid.sum()<64:return np.zeros(3),np.zeros(3),np.zeros(3),0.,0
    blur=gaussian_filter(x,(1,1,0)); dy,dx=np.gradient(blur,axis=(0,1)); edge=np.hypot(dx,dy)
    ge=chroma(np.sqrt(np.mean(edge[valid]**2,axis=0)))
    log=np.log(np.maximum(x,1e-6)); logsum=np.log(np.maximum(x.sum(2),1e-6));
    c=np.stack([gaussian_laplace(log[:,:,i],1) for i in range(3)],axis=2)
    gi=uniform_filter(np.hypot(gaussian_laplace(log[:,:,0]-logsum,1),gaussian_laplace(log[:,:,2]-logsum,1)),7)
    mask=valid & (np.min(np.abs(c),axis=2)>1e-4)
    ids=np.flatnonzero(mask); gi_est=np.zeros(3)
    if len(ids)>=64:
        n=max(16,int(.001*x.shape[0]*x.shape[1])); sel=ids[np.argsort(gi.ravel()[ids])[:n]]; gi_est=chroma(x.reshape(-1,3)[sel].mean(0))
    # independent witness: low-chroma, textured mid/high pixels spread across tiles.
    # GI alone cannot distinguish uniform coloured reflectance from neutral under cast.
    # Gate is heuristic; a cast may move true neutrals outside the witness band.
    z=chroma(x); sat=np.linalg.norm(z,axis=2); y=x@LUMA
    w=valid & (y>.10)&(y<.85)&(sat<.48)&(np.linalg.norm(edge,axis=2)>.003)
    tiles=[]
    for iy in range(4):
        for ix in range(4):
            ys=slice(iy*x.shape[0]//4,(iy+1)*x.shape[0]//4); xs=slice(ix*x.shape[1]//4,(ix+1)*x.shape[1]//4)
            ww=w[ys,xs];
            if ww.sum()>=16:tiles.append(np.median(z[ys,xs][ww],axis=0))
    if len(tiles)<3 or len(ids)<64:return ge,gi_est,np.zeros(3),0.,len(tiles)
    tiles=np.array(tiles); witness=np.median(tiles,axis=0); spread=np.median(np.linalg.norm(tiles-witness,axis=1)); agree=max(np.linalg.norm(ge-witness),np.linalg.norm(gi_est-witness));
    gate=float(smooth(2,8,len(tiles))*(1-smooth(.10,.30,spread))*(1-smooth(.10,.35,agree))*smooth(.035,.10,np.linalg.norm(witness)))
    return ge,gi_est,witness,gate,len(tiles)
def correct(x,est,strength):
    # linear-light bounded diagonal correction, preserve source luminance, then
    # common chroma scaling towards neutral if gamut would be exceeded.
    if strength==0:return x.copy()
    est=np.clip(est,-.35,.35); est=est-est.mean(); y=x@LUMA
    q=x*np.exp(-strength*est); q*=np.divide(y,q@LUMA,out=np.zeros_like(y),where=(q@LUMA)>1e-12)[...,None]
    d=q-y[...,None]; hi=np.divide(1-y[...,None],d,out=np.full_like(d,np.inf),where=d>0);lo=np.divide(-y[...,None],d,out=np.full_like(d,np.inf),where=d<0)
    s=np.minimum(1,np.minimum(hi,lo).min(axis=-1));return np.clip(y[...,None]+s[...,None]*d,0,1)
def main(path):
    rows=[]
    for name in ['astronaut','coffee','chelsea','rocket','camera','coins','grass','colorwheel','immunohistochemistry','retina','hubble_deep_field','horse']:
        a=np.asarray(Image.fromarray(getattr(data,name)()).convert('RGB').resize((256,144)),dtype=float)/255; lin=linear(a)
        for domain in ('linear_cast','post_isp'):
            ref=a if domain=='linear_cast' else render(lin)
            target=color.rgb2lab(ref)
            for label,cast in [('none',[1,1,1]),('warm',[1.24,1,.76]),('cool',[.78,1,1.26]),('green',[.92,1.22,.90])]:
                inp=np.clip(srgb(lin*np.array(cast)),0,1) if domain=='linear_cast' else render(lin*np.array(cast)); x=linear(inp)
                ge,gi,w,gate,tiles=estimate(x)
                for method,e,s in [('off',w,0),('gray_edge',ge,1),('gi_approx',gi,1),('gated',w,gate),('gated_half',w,.5*gate)]:
                    q=correct(x,e,s); assert np.isfinite(q).all() and q.min()>=0 and q.max()<=1
                    assert np.max(np.abs(q@LUMA-x@LUMA))<1e-10
                    out=inp if s==0 else np.clip(srgb(q),0,1)
                    de=color.deltaE_ciede2000(color.rgb2lab(out),target)
                    rows.append(dict(scene=name,domain=domain,cast=label,method=method,mean_de=float(de.mean()),p95_de=float(np.quantile(de,.95)),gate=gate,tiles=tiles))
    with open(path,'w',newline='') as f:
        w=csv.DictWriter(f,fieldnames=rows[0]);w.writeheader();w.writerows(rows)
    # Explicit non-identifiability: same pixels can be intentional colour or cast.
    ramp=np.tile(np.linspace(.1,.6,256),(144,1)); x=ramp[...,None]*np.array([1.2,1,.8]);
    assert np.array_equal(correct(x,np.array([.2,0,-.2]),0),x)
    print('rows',len(rows),'identity / finite / gamut / luminance checks passed')
if __name__=='__main__':main(sys.argv[1])
