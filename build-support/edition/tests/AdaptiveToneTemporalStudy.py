# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline AT-02 / WB-02 controlled study; no playback dependencies.
Usage: python AdaptiveToneTemporalStudy.py <AT01 executable> <output-directory>
Requires numpy, scipy, scikit-image, Pillow (test environment only).
Photographs are skimage examples; all sequences/casts are synthetic, not real WB ground truth.
"""
import csv, sys, subprocess
from pathlib import Path
import numpy as np
from scipy.ndimage import gaussian_filter, gaussian_filter1d
from skimage import data, color
from PIL import Image
W=np.array([.2126,.7152,.0722]); X=np.linspace(0,1,257)
exe=sys.argv[1]; out=Path(sys.argv[2]); out.mkdir(parents=True,exist_ok=True)

def write(name, rows):
    with (out/name).open('w',newline='') as f:
        w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)

def image(name):
    im=getattr(data,name)(); im=Image.fromarray(im).convert('RGB').resize((320,240),Image.Resampling.BILINEAR)
    return np.asarray(im,dtype=np.float64)/255

def hist(a):return np.histogram(a@W,256,(0,1))[0]
def analyze(h):
    p=subprocess.run([exe,'hist'],input=' '.join(map(str,h)),text=True,capture_output=True,check=True).stdout.splitlines()
    return np.array(list(map(float,p[1].split()))),float(p[0].split()[3])
def delta(c):return np.max(np.abs(c-X))*255
names=['astronaut','coffee','chelsea','rocket','camera','coins','grass','colorwheel']
static=[]; temporal=[]; wb=[]; fades=[]
for name in names:
    im=image(name)
    for tag, a in [('original',im),('synthetic_haze',.55*im+.27),('synthetic_low_key',.38*im),('synthetic_high_key',.38*im+.62)]:
        c,s=analyze(hist(a));static.append(dict(image=name,case=tag,strength=s,max_curve_delta=round(delta(c),4)))
    # 96-frame camera pan on artificially flattened still. 24fps, known shot boundaries.
    # A fixed code-luma probe measures *mapping drift*, not pixel tracking or subjective quality.
    a=.55*im+.27
    h=[hist(a[:,int(120*(.5-.5*np.cos(np.pi*t/95))):int(120*(.5-.5*np.cos(np.pi*t/95)))+200]) for t in range(96)]
    curves=np.array([analyze(v)[0] for v in h]); oracle=analyze(np.sum(h,axis=0))[0]
    # Compare a 0.5 s prefix estimate frozen for the rest of the shot (known cut, no detector).
    prefix=analyze(np.sum(h[:12],axis=0))[0]
    # For a zero-lookahead player the prefix is only available after 12 frames.
    anchor=np.array([X if i<12 else X+min(1,(i-12)/6)*(prefix-X) for i in range(96)])
    # rolling centered 25-frame median in curve space, and one pooled histogram per shot (oracle).
    ema=[curves[0]]; alpha=1-np.exp(-1/24/.30)
    for c in curves[1:]:ema.append(ema[-1]+alpha*(c-ema[-1]))
    med=np.array([np.median(curves[max(0,i-12):i+13],axis=0) for i in range(96)])
    for mode, c in [('frame',curves),('ema_300ms',np.array(ema)),('centered_median_1s',med),('shot_pooled_oracle',np.tile(oracle,(96,1))),('prefix_anchor_500ms',anchor)]:
        p=np.array([np.interp([.35,.5,.7],X,v) for v in c])*255
        temporal.append(dict(image=name,mode=mode,probe_span=round(np.ptp(p,axis=0).max(),4),max_step=round(np.max(np.abs(np.diff(p,axis=0))),4),max_correction=round(np.max(np.abs(c-X))*255,4),settled_probe_span=round(np.ptp(p[18:],axis=0).max(),4)))
    # A true fade in a *fixed* shot. Measure difference between applied tone delta
    # on a source probe following fade vs same curve fixed throughout shot.
    # This demonstrates compensation; no assertion a constant curve preserves fade exactly.
    base=analyze(hist(a))[0]; probe=np.array([.35,.5,.7]); fadesrc=np.linspace(1,0,49)
    # compare with no-op and fixed curve at the same source code values; black endpoint exact.
    fade_curves=np.array([analyze(hist(a*f))[0] for f in fadesrc]); f_ema=[fade_curves[0]]
    for c in fade_curves[1:]:f_ema.append(f_ema[-1]+alpha*(c-f_ema[-1]))
    target=np.array([np.interp(probe*f,X,base) for f in fadesrc])*255
    for mode,cur in [('frame',fade_curves),('ema_300ms',np.array(f_ema)),('fixed_curve',np.tile(base,(49,1)))]:
        p=np.array([np.interp(probe*f,X,c) for f,c in zip(fadesrc,cur)])*255
        fades.append(dict(image=name,mode=mode,maximum_deviation_from_fixed_curve=round(np.max(np.abs(p-target)),4),upward_steps=int(np.sum(np.diff(p,axis=0)>.01))))
    # WB: inject multiplicative linear-display-RGB casts into a rendered photograph.
    # Original is reference for perturbation only, not a certified neutral image.
    lin=np.where(im<=.04045,im/12.92,((im+.055)/1.055)**2.4)
    def enc(a):return np.where(a<=.0031308,12.92*a,1.055*np.maximum(a,0)**(1/2.4)-.055)
    def est(a,method):
        # exclude near black and near clipped pixels; same mask for channels.
        mask=(a.max(2)<.96)&(a.min(2)>.005)
        if not np.any(mask):return np.ones(3)
        if method=='gray_edge':
            d=np.stack([np.hypot(gaussian_filter(a[:,:,j],1,order=(1,0)),gaussian_filter(a[:,:,j],1,order=(0,1))) for j in range(3)],2)
            e=(np.mean(d[mask]**2,axis=0))**.5
        else:
            p=1 if method=='gray_world' else 6
            e=(np.mean(a[mask]**p,axis=0))**(1/p)
        if not np.all(np.isfinite(e)) or np.min(e)<1e-8:return np.ones(3)
        return e/np.exp(np.mean(np.log(e)))
    for tag,g in [('identity',np.ones(3)),('warm',np.array([1.18,1.,.78])),('green',np.array([.85,1.15,.85])),('cool',np.array([.78,1.,1.18]))]:
        v=np.clip(lin*g,0,1); ref=color.rgb2lab(im)
        for method in ['none','gray_world','shades_gray_p6','gray_edge']:
            e=np.ones(3) if method=='none' else est(v,method)
            for strength in ([0] if method=='none' else [.5,1.]):
                # geometric gain interpolation; isolate chroma from exposure by preserving linear Y.
                q=v*e**(-strength); q*=((v@W)/np.maximum(q@W,1e-9))[...,None]
                q=np.clip(q,0,1); lab=color.rgb2lab(np.clip(enc(q),0,1))
                # L* differs due to injected cast; report chromatic error alone plus full DeltaE.
                wb.append(dict(image=name,cast=tag,method=method,strength=strength,mean_de00=round(float(color.deltaE_ciede2000(ref,lab).mean()),4),mean_ab_error=round(float(np.linalg.norm((ref-lab)[...,1:],axis=-1).mean()),4)))
write('adaptive-tone-at02-fade.csv',fades);write('adaptive-tone-at02-static.csv',static);write('adaptive-tone-at02-pan.csv',temporal);write('white-balance-wb02-controlled.csv',wb)
