# SPDX-License-Identifier: GPL-3.0-or-later
# Actual production C++ estimator, synthetic known casts; not a real WB benchmark.
# Usage: python WhiteBalanceRuntimeStudy.py /path/to/WhiteBalance-test output.csv [--extended]
import csv,sys,subprocess,tempfile
from pathlib import Path
import numpy as np
from PIL import Image
from skimage import data,color
W,H=256,144
w=np.array([.2126,.7152,.0722]); M=np.array([[1.10,-.06,-.04],[-.03,1.07,-.04],[.02,-.08,1.06]])
def lin(x):return np.where(x<=.04045,x/12.92,((x+.055)/1.055)**2.4)
def enc(x):return np.where(x<=.0031308,x*12.92,1.055*np.maximum(0,x)**(1/2.4)-.055)
def render(x):
 q=np.maximum(x@M.T,0)**np.array([.94,1.04,1.12]);return np.clip(enc(q/(1+.18*q)*1.12),0,1)
def correct(a,e,s):
 if s==0:return a
 e=np.clip(e,-.35,.35);e-=e.mean();x=lin(a);y=x@w;q=x*np.exp(-s*e);z=q@w;q*=np.divide(y,z,out=np.ones_like(y),where=z>1e-10)[...,None];c=q-y[...,None];hi=np.divide(1-y[...,None],c,out=np.full_like(c,np.inf),where=c>0);lo=np.divide(-y[...,None],c,out=np.full_like(c,np.inf),where=c<0);fit=np.minimum(1,np.minimum(hi,lo).min(-1));return np.clip(enc(y[...,None]+c*fit[...,None]),0,1)
rows=[]
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp)/'sample.bin'
 for name in ('astronaut','coffee','chelsea','rocket','camera','coins') + (('grass','colorwheel','immunohistochemistry','retina','hubble_deep_field','horse') if '--extended' in sys.argv[3:] else ()):
  a=np.asarray(Image.fromarray(getattr(data,name)()).convert('RGB').resize((W,H)),dtype=np.float32)/255; x=lin(a)
  for domain in ('linear','post_isp'):
   ref=a if domain=='linear' else render(x);lab=color.rgb2lab(ref)
   for label,gain in [('none',[1,1,1]),('warm',[1.24,1,.76]),('cool',[.78,1,1.26]),('green',[.92,1.22,.90])]:
    inp=np.clip(enc(x*np.array(gain)),0,1) if domain=='linear' else render(x*np.array(gain));rgba=np.concatenate([inp,np.ones((H,W,1))],-1).astype('<f4');rgba.tofile(p)
    v,*e=map(float,subprocess.check_output([sys.argv[1],str(p)],text=True).split());e=np.array(e) if v else np.zeros(3)
    for s in (0,.5,1):
     out=correct(inp,e,s);rows.append([name,domain,label,int(v),s,float(color.deltaE_ciede2000(color.rgb2lab(out),lab).mean())])
with open(sys.argv[2],'w',newline='') as f:
 writer=csv.writer(f);writer.writerow(['image','domain','cast','estimate_valid','strength','mean_deltaE00']);writer.writerows(rows)
for s in (0,.5,1):
 print(s,'cast',np.mean([r[-1] for r in rows if r[2]!='none' and r[4]==s]),'control',np.mean([r[-1] for r in rows if r[2]=='none' and r[4]==s]))
print('Valid estimates',sum(r[3] for r in rows if r[4]==0),'/',len(rows)//3)
