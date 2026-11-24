# SPDX-License-Identifier: GPL-3.0-or-later
import struct as s,numpy as np,collections
import argparse, hashlib, json
from pathlib import Path
root_dir = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser(description="Offline CPU reference for the fixed SVDLUT graph; needs numpy and torch only for development testing.")
parser.add_argument("graph", type=Path)
args = parser.parse_args()
b = args.graph.read_bytes()
manifest = json.loads((root_dir / "models/svdlut_fivek_srgb/manifest.json").read_text())
assert hashlib.sha256(b).hexdigest() == manifest["graph_sha256"], "Only the fixed manifest graph is supported by this test"
def r(p,f='I'):return s.unpack_from('<'+f,b,p)[0]
def fld(p,i):
 v=p-r(p,'i'); o=4+2*i
 return p+r(v+o,'H') if o<r(v,'H') and r(v+o,'H') else 0
def off(p):return p+r(p) if p else 0
def ptr(p,i):return off(fld(p,i))
def st(p):return b[p+4:p+4+r(p)].decode() if p else ''
def vec(p,kind='I'):return [r(p+4+i*s.calcsize(kind),kind) for i in range(r(p))] if p else []
def tabvec(p):return [off(p+4+i*4) for i in range(r(p))] if p else []
def strings(p):return list(map(st,tabvec(p)))
def tensor(p):return (st(ptr(p,0)),vec(ptr(p,1)),vec(ptr(p,2)),r(fld(p,3),'Q') if fld(p,3) else 0)
def attrs(p):
 out=[]
 for a in tabvec(p):
  n=st(ptr(a,0)); t=r(fld(a,1),'B') if fld(a,1) else 0; v=ptr(a,2)
  if t in [3,4,5,6,13]: val=r(v,{3:'I',4:'Q',5:'i',6:'f',13:'B'}[t]) if v else None
  elif t in [7,8,9]: val=vec(ptr(v,0),{7:'I',8:'i',9:'f'}[t]) if v else []
  elif t==1: val=(st(ptr(v,0)),attrs(ptr(v,1))) if v else None
  else: val=(t,v)
  out.append((t,val))
 return out
root=r(0); ops=[]; const={}
print('inputs',strings(ptr(root,1)),'outputs',strings(ptr(root,2)))
for p in tabvec(ptr(root,0)):
 typ=r(fld(p,0),'B'); d=ptr(p,1); name=st(ptr(p,2)); ins=strings(ptr(p,3)); outs=strings(ptr(p,4))
 if typ==2:
  v=ptr(ptr(d,1),0); arr=np.frombuffer(b,dtype='<f4',count=r(v)//4,offset=v+4); const[outs[0]]=arr
  assert np.isfinite(arr).all()
 else:
  op=st(ptr(d,0)); it=list(map(tensor,tabvec(ptr(d,1)))); ot=list(map(tensor,tabvec(ptr(d,2)))); a=attrs(ptr(d,3)); ops.append((name,op,ins,outs,it,ot,a))
print('counts',len(ops),len(const),sum(x.size for x in const.values()))

import torch,torch.nn.functional as F
from collections import Counter
torch.set_num_threads(2)
# Verify all declared shapes and connected buffer sizes, including broadcast strides.
produced={'image':786432}|{k:v.nbytes for k,v in const.items()}
for name,op,ins,outs,it,ot,a in ops:
 for n,t in zip([n for n in ins if n],it):
  dtype,shape,strides,sz=t
  assert dtype=='DML_TENSOR_DATA_TYPE_FLOAT32'
  assert produced[n]==sz,(name,n,produced[n],sz)
  assert 4*(1+sum((x-1)*y for x,y in zip(shape,strides)))==sz if strides else 4*np.prod(shape)==sz
 for n,t in zip(outs,ot):
  assert 4*np.prod(t[1])==t[3];produced[n]=t[3]
print('verified tensor sizes and topological edges; operators',Counter(o[1] for o in ops))
C={k:torch.from_numpy(v.copy()) for k,v in const.items()}
@torch.no_grad()
def run(img):
 data=dict(C);data['image']=img
 for name,op,ins,outs,it,ot,attrs in ops:
  a=[v for _,v in attrs]; x=[]
  for n,(_,shape,strides,sz) in zip([n for n in ins if n],it):
   t=data[n].contiguous().flatten(); x.append(t.as_strided(shape,strides) if strides else t.reshape(shape))
  if op.endswith('CONVOLUTION'):
   assert a[:3]==[1,0,2] and a[5]==a[6]; y=F.conv2d(x[0],x[1],x[2].flatten(),stride=a[3],padding=a[5],dilation=a[4],groups=a[8])
  elif op.endswith('LEAKY_RELU'): y=F.leaky_relu(x[0],a[0])
  elif op.endswith('MEAN_VARIANCE_NORMALIZATION'):
   assert a[:2]==[0,1];y=F.instance_norm(x[0],weight=x[1].flatten(),bias=x[2].flatten(),eps=a[2])
  elif op.endswith('AVERAGE_POOLING'):y=F.avg_pool2d(x[0],a[2],a[1],a[3],count_include_pad=bool(a[5]))
  elif op.endswith('GEMM'):
   A=x[0].transpose(-1,-2) if a[0] else x[0]; B=x[1].transpose(-1,-2) if a[1] else x[1]; y=a[2]*(A@B);y=y+a[3]*x[2] if len(x)>2 else y
  elif op.endswith('MULTIPLY'):y=x[0]*x[1]
  else:raise ValueError(op)
  assert list(y.shape)==ot[0][1];assert torch.isfinite(y).all();data[outs[0]]=y
 return [data[k].flatten() for k in ['lut_planes','lut_mix','grid_planes','grid_mix']]
# Directly evaluate the published edge-interior LUT formula for arbitrary colors.
def shade(out,colors):
 lut,lmix,grid,gmix=out;lut=lut.view(3,3,33,33);lmix=lmix.view(3,4);grid=grid.view(6,3,17,17);gmix=gmix.view(2,12)
 def sample(t,u,v):
  coords=torch.stack((u*2-1,v*2-1),-1).reshape(1,1,-1,2);return F.grid_sample(t[None,None],coords,mode='bilinear',padding_mode='border',align_corners=True).flatten()
 r,g,b=colors.T; z=torch.zeros_like(r); result=[]
 for ch,c in enumerate([r,g,b]):
  v=lmix[ch,3]+sum(lmix[ch,p]*sample(lut[ch,p],u,w) for p,(u,w) in enumerate([(r,g),(r,b),(g,b)]))
  for j in range(2):
   group=2*ch+j; mix=gmix[group//3]; k=group%3
   v=v+mix[9+k]+sum(mix[k*3+p]*sample(grid[group,p],u,w) for p,(u,w) in enumerate([(z,z),(z,c),(z,c)]))
  result.append(v)
 return torch.stack(result,-1).clamp(0,1)

# Test shader's coordinate formula against PyTorch, including a cropped source and tiny inputs.
def half_pixel(image):
 _,_,h,w = image.shape
 x=(torch.arange(256,dtype=torch.float32)+.5)*w/256-.5
 y=(torch.arange(256,dtype=torch.float32)+.5)*h/256-.5
 x=x.clamp(0,w-1); y=y.clamp(0,h-1)
 x0=x.floor().long(); y0=y.floor().long(); x1=(x0+1).clamp(max=w-1); y1=(y0+1).clamp(max=h-1)
 wx=x-x0; wy=(y-y0)[:,None]
 a=image[:,:,y0[:,None],x0]; b=image[:,:,y0[:,None],x1]
 c=image[:,:,y1[:,None],x0]; d=image[:,:,y1[:,None],x1]
 top=a+(b-a)*wx; bottom=c+(d-c)*wx
 return (top+(bottom-top)*wy).clamp(0,1)
gen=torch.Generator().manual_seed(121)
for h,w in [(1,1),(2,3),(17,31),(337,583),(720,1280),(1080,1920),(2160,3840)]:
 im=torch.rand((1,3,h,w),generator=gen)
 ref=F.interpolate(im,size=(256,256),mode='bilinear',align_corners=False)
 error=float((half_pixel(im)-ref).abs().max()); assert error < 2e-6,(h,w,error)
 print('half-pixel',h,w,'max error',error)
# Compare the old downsample (point at texel center, UNORM8) and corrected FP32 route.
y,x=torch.meshgrid(torch.linspace(0,1,337),torch.linspace(0,1,583),indexing='ij')
patterns={'low gradient':torch.stack([.04+.13*x]*3),'gray gradient':torch.stack([x]*3),'RGB gradient':torch.stack([x,y,(x+y)/2]),'glow card':torch.stack([(.8*torch.exp(-((x-.5)**2+(y-.5)**2)*120)+.015).clamp(0,1)]*3),'random':torch.rand((3,337,583),generator=gen)}
colors=torch.rand((4096,3),generator=gen)
for name,im in patterns.items():
 im=(im*255).round()/255
 thumb=half_pixel(im[None]); ref=F.interpolate(im[None],size=(256,256),mode='bilinear',align_corners=False)
 yi=((torch.arange(256)+.5)*im.shape[1]/256).floor().long(); xi=((torch.arange(256)+.5)*im.shape[2]/256).floor().long()
 point=im[:,yi[:,None],xi][None]; out=run(thumb); official=run(ref); old=run(point)
 delta=float((shade(out,colors)-shade(official,colors)).abs().max()*255)
 old_delta=float((shade(out,colors)-shade(old,colors)).abs().max()*255)
 assert delta < .005,(name,delta)
 print(name,'corrected-vs-PyTorch max code',round(delta,6),'old-vs-corrected max code',round(old_delta,5))
# Exact native dither threshold resource; check bounded quantization and mean preservation.
threshold=np.fromfile(root_dir/'Source/res/dither32x32float16.bin',dtype='<f2').astype(np.float64)
assert len(threshold)==1024 and len(np.unique(threshold))==1024 and threshold.min()>=0 and threshold.max()<1
for q in (255,1023):
 integer=np.arange(q+1,dtype=np.float64)[:,None]
 assert np.all(np.floor(integer+threshold)==integer)
 code=np.linspace(0,q,4097)[:,None]
 quant=np.floor(code+threshold)
 assert np.max(np.abs(quant-code))<1
 assert np.max(np.abs(quant.mean(axis=1)-code[:,0])) <= 1/1024 + 1e-8
 print('dither',q,'integer endpoints intact; maximum tile mean error',float(np.max(np.abs(quant.mean(axis=1)-code[:,0]))))
print('PASS: CPU reference only; not D3D11 shader execution or DirectML/PyTorch checkpoint parity')
