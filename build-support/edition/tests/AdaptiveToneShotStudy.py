# SPDX-License-Identifier: GPL-3.0-or-later
"""AT-03 offline production C++ shot-state study. Requires Pillow, numpy, skimage.
Usage: python AdaptiveToneShotStudy.py <compiled AdaptiveToneShot> <output.csv> [screenshots...]
Synthetic sequences, not Windows playback validation.
"""
import csv, sys, subprocess, struct
from pathlib import Path
from PIL import Image
import numpy as np
from skimage import data
X=np.linspace(0,1,257); W=np.array([.2126,.7152,.0722]); rows=[]
def run(name,tag,frames):
    buf=[]
    for i,a in enumerate(frames):
        # The same clamped half-pixel bilinear as the D3D11 analysis shader, no antialias filter.
        a=np.asarray(np.clip(a,0,1),dtype=np.float32); h,w=a.shape[:2]
        x=np.clip((np.arange(256)+.5)*w/256-.5,0,w-1); y=np.clip((np.arange(144)+.5)*h/144-.5,0,h-1)
        ix=x.astype(int); iy=y.astype(int); fx=(x-ix)[None,:,None]; fy=(y-iy)[:,None,None]
        lo=a[iy[:,None],ix[None,:]]*(1-fx)+a[iy[:,None],np.minimum(ix+1,w-1)[None,:]]*fx
        hi=a[np.minimum(iy+1,h-1)[:,None],ix[None,:]]*(1-fx)+a[np.minimum(iy+1,h-1)[:,None],np.minimum(ix+1,w-1)[None,:]]*fx
        a=lo*(1-fy)+hi*fy
        rgba=np.concatenate((a,np.ones((*a.shape[:2],1))),axis=2).astype('<f4')
        buf.append(struct.pack('<q',i*800000)+rgba.tobytes())
    lines=subprocess.run([sys.argv[1],'binary'],input=b''.join(buf),capture_output=True,check=True).stdout.decode().splitlines()
    z=np.array([list(map(float,line.split())) for line in lines]); mapped=X+(z[:,4:]-X)*z[:,3:4]; locked=np.flatnonzero(z[:,0]>=0)
    first=int(locked[0]) if len(locked) else -1
    settled=np.flatnonzero(z[:,3]>=.999)
    span=float(np.ptp(mapped[settled],axis=0).max()*255) if len(settled) else 0
    rows.append(dict(image=name,case=tag,locked_at_ms=first*80 if first>=0 else -1,active_frames=int(np.count_nonzero(z[:,2])),max_curve_delta=round(float(np.max(np.abs(mapped-X))*255),4),settled_curve_span=round(span,4),anchor_changes=int(np.count_nonzero(np.diff(z[:,0])))))
for file in sys.argv[3:]:
    a=Image.open(file).convert('RGB'); w,h=a.size;a=np.array(a.crop((int(.025*w),int(.09*h),int(.975*w),int(.91*h))))/255
    run(Path(file).name,'reference',[a]*16)
for name in ['astronaut','coffee','chelsea','rocket','camera','coins','grass','colorwheel']:
    a=np.array(Image.fromarray(getattr(data,name)()).convert('RGB').resize((320,240)))/255
    for tag,v in [('original',a),('haze',.55*a+.27),('low_key',.38*a),('high_key',.38*a+.62)]:run(name,tag,[v]*16)
    h=.55*a+.27
    run(name,'pan',[h[:,int(120*(.5-.5*np.cos(np.pi*t/47))):int(120*(.5-.5*np.cos(np.pi*t/47)))+200] for t in range(48)])
    run(name,'fade_after_anchor',[h]*16+[h*(1-i/31) for i in range(32)])
with open(sys.argv[2],'w',newline='') as f:
    w=csv.DictWriter(f,fieldnames=rows[0]);w.writeheader();w.writerows(rows)
