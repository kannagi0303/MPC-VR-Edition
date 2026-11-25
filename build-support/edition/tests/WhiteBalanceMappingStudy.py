# SPDX-License-Identifier: GPL-3.0-or-later
"""WB-03 oracle paired mapping capacity, not illuminant estimation. Synthetic rendering.
Usage: python WhiteBalanceMappingStudy.py <output.csv>
Requires numpy, Pillow, scikit-image. No runtime dependencies.
"""
import csv,sys
import numpy as np
from PIL import Image
from skimage import data,color
M=np.array([[1.10,-.06,-.04],[-.03,1.07,-.04],[.02,-.08,1.06]])
def linear(x):return np.where(x<=.04045,x/12.92,((x+.055)/1.055)**2.4)
def srgb(x):return np.where(x<=.0031308,12.92*x,1.055*np.maximum(x,0)**(1/2.4)-.055)
def render(x):
    q=np.maximum(x@M.T,0)**np.array([.94,1.04,1.12]); q=q/(1+.18*q)*1.12
    return np.clip(srgb(q),0,1)
def features(x,quadratic):
    r,g,b=x.T; v=[r,g,b,np.ones_like(r)]
    if quadratic:v += [r*r,g*g,b*b,r*g,r*b,g*b]
    return np.column_stack(v)
rows=[]
for name in ['astronaut','coffee','chelsea','rocket','camera','coins','grass','colorwheel']:
    a=np.asarray(Image.fromarray(getattr(data,name)()).convert('RGB').resize((320,240)),dtype=float)/255
    lin=linear(a); ref=render(lin)
    for label,cast in [('warm',[1.24,1,.76]),('cool',[.78,1,1.26]),('green',[.92,1.22,.90])]:
        inp=render(lin*np.array(cast)); mask=np.zeros(a.shape[:2],bool);mask[2::8,2::8]=True
        tx,ty=inp[mask],ref[mask];x,y=inp[~mask],ref[~mask]
        lx,ly=linear(tx),linear(ty); gain=(lx*ly).sum(0)/np.maximum((lx*lx).sum(0),1e-12)
        outputs={'none':x,'diagonal_linear':srgb(linear(x)*gain)}
        for mode,q in [('affine_rgb',False),('quadratic_rgb',True)]:
            coef=np.linalg.lstsq(features(tx,q),ty,rcond=1e-6)[0];outputs[mode]=features(x,q)@coef
        target=color.rgb2lab(y.reshape(-1,1,3));
        for mode,v in outputs.items():
            error=color.deltaE_ciede2000(color.rgb2lab(np.clip(v,0,1).reshape(-1,1,3)),target).ravel()
            rows.append(dict(image=name,cast=label,method=mode,train_pixels=int(mask.sum()),test_pixels=int((~mask).sum()),mean_de=round(float(error.mean()),5),p95_de=round(float(np.quantile(error,.95)),5),out_of_gamut_pct=round(float(np.any((v<0)|(v>1),axis=1).mean()*100),5)))
with open(sys.argv[1],'w',newline='') as f:
    w=csv.DictWriter(f,fieldnames=rows[0]);w.writeheader();w.writerows(rows)
