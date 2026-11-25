# SPDX-License-Identifier: GPL-3.0-or-later
# Controlled video-like sequences, not camera-calibrated/real video.
# Usage: python WhiteBalanceSequence.py WB06-binary WB07-binary output.csv
import csv,sys,tempfile,subprocess
from pathlib import Path
import numpy as np
from PIL import Image
from skimage import data
W,H=256,144
M=np.array([[1.10,-.06,-.04],[-.03,1.07,-.04],[.02,-.08,1.06]])
def linear(x): return np.where(x<=.04045,x/12.92,((x+.055)/1.055)**2.4)
def encode(x): return np.where(x<=.0031308,x*12.92,1.055*np.maximum(x,0)**(1/2.4)-.055)
def render(x):
 q=np.maximum(x@M.T,0)**np.array([.94,1.04,1.12]);return np.clip(encode(q/(1+.18*q)*1.12),0,1)
rows=[]
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp)/'sequence.bin'
 for scene in ['astronaut','coffee','chelsea','rocket','camera','coins']:
  src=linear(np.asarray(Image.fromarray(getattr(data,scene)()).convert('RGB').resize((W+48,H+24)),dtype=float)/255)
  for domain in ['linear','post_isp']:
   for mode in ['pan','exposure','cast_change']:
    truth=[]
    with p.open('wb') as f:
     for i in range(48):
      t=np.clip((i-12)/16,0,1); x=round(48*t) if mode=='pan' else 0;y=round(24*t) if mode=='pan' else 0
      logcast=np.array([.20,0,-.20])+ (t*np.array([-.32,.14,.18]) if mode=='cast_change' else 0)
      exposure=1-.35*t if mode=='exposure' else 1
      img=src[y:y+H,x:x+W]*exposure*np.exp(logcast);rgb=np.clip(encode(img),0,1) if domain=='linear' else render(img)
      rgba=np.concatenate([rgb,np.ones((H,W,1))],axis=-1).astype('<f4');f.write(rgba.tobytes());truth.append(logcast)
    for method,exe in [('WB06',sys.argv[1]),('WB07',sys.argv[2])]:
     z=np.loadtxt(subprocess.check_output([exe,str(p)],text=True).splitlines(),delimiter=','); active=np.flatnonzero(z[:,1]);anchor=z[active[0],2:] if len(active) else np.zeros(3)
     for i,r in enumerate(z):
      d=r[2:]-anchor; err=np.linalg.norm(d-(truth[i]-truth[0])) if r[1] else np.nan
      rows.append([scene,domain,mode,method,i,int(r[1]),*r[2:],float(np.linalg.norm(d)) if r[1] else np.nan,float(err)])
with open(sys.argv[3],'w',newline='') as f:
 w=csv.writer(f);w.writerow(['scene','domain','mode','method','frame','locked','r','g','b','drift','relative_error']);w.writerows(rows)
print('rows',len(rows))
