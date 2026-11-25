# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline image comparison. Python/Pillow/NumPy are test-only, not runtime dependencies.
Usage: python AdaptiveToneResearchImages.py <compiled C++ executable> <images...>
Input screenshots: central 95% x 82% excludes edge OSD and margins; not a video ROI policy.
"""
import sys, subprocess, csv
import numpy as np
from PIL import Image
w=np.array([.2126,.7152,.0722])
writer=csv.writer(sys.stdout); writer.writerow(['file','method','p01','p50','p95','p99','strength','peak'])
for f in sys.argv[2:]:
    a=Image.open(f).convert('RGB'); W,H=a.size
    a=a.crop((int(.025*W),int(.09*H),int(.975*W),int(.91*H)))
    tiny=np.asarray(a.resize((256,144),Image.Resampling.BILINEAR),dtype=np.float64)/255
    h=np.histogram(tiny@w,bins=256,range=(0,1))[0]
    p=subprocess.run([sys.argv[1],'hist'],input=' '.join(map(str,h)),text=True,capture_output=True,check=True).stdout.splitlines()
    b,white,med,s,peak=map(float,p[0].split()); curve=np.array(list(map(float,p[1].split())))
    rgb=np.asarray(a,dtype=np.float64)/255; y=rgb@w; z=np.interp(y,np.linspace(0,1,257),curve)
    d=rgb-y[...,None]; scale=np.ones_like(y)
    for i in range(3):
        delta=d[...,i]; lim=np.ones_like(y); np.divide(1-z,delta,out=lim,where=delta>0); np.divide(-z,delta,out=lim,where=delta<0); scale=np.minimum(scale,lim)
    out=np.clip(z[...,None]+d*scale[...,None],0,1)
    # Simple linked min/max baseline (not a claim of running FFmpeg itself).
    lo,hi=rgb.min(),rgb.max(); linked=(rgb-lo)/max(hi-lo,1e-9)
    for name,v in [('original',y),('linked_extrema',linked@w),('candidate',out@w)]:
        writer.writerow([f.split('/')[-1],name,*[round(float(q)*255,2) for q in np.quantile(v,[.01,.5,.95,.99])],round(s,5),round(peak,5)])
