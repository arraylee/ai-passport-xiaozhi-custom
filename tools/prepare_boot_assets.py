"""Regenerate the small startup masks/audio. Requires Pillow, numpy and av."""
from pathlib import Path
import hashlib,json,wave,sys
import numpy as np
from PIL import Image
import av
from adpcm_codec import encode,decode
ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'assets/source/boot'
def main():
 # Preserve the user's complete horizontal wordmark, alpha and aspect ratio.
 # Two cropped halves are Flash masks; no PNG decoder is used on the device.
 im=Image.open(SOURCE/'folotoy-logo-pink.png').convert('RGBA')
 color=(242,0,111);width=208;height=round(width*im.height/im.width);split=114
 alpha=np.asarray(im.getchannel('A').resize((width,height),Image.Resampling.LANCZOS))
 values=np.rint(alpha.astype(float)/17).clip(0,15).astype('uint8')
 # The slanted T overlaps the last o's horizontal bounds. Split by whole
 # letters, so the falling Toy never leaves a sliced-off stroke behind.
 seen=np.zeros(values.shape,dtype=bool);letters=[]
 for y,x in np.argwhere(values):
  if seen[y,x]:continue
  pending=[(int(y),int(x))];seen[y,x]=True;points=[]
  while pending:
   yy,xx=pending.pop();points.append((yy,xx))
   for dy,dx in ((-1,0),(1,0),(0,-1),(0,1),(-1,-1),(-1,1),(1,-1),(1,1)):
    ny,nx=yy+dy,xx+dx
    if 0<=ny<height and 0<=nx<width and values[ny,nx] and not seen[ny,nx]:
     seen[ny,nx]=True;pending.append((ny,nx))
  letters.append(points)
 letters.sort(key=lambda points:min(x for y,x in points))
 assert len(letters)==7,'Expected seven complete wordmark letters'
 planes=[np.zeros_like(values),np.zeros_like(values)]
 for index,points in enumerate(letters):
  for y,x in points:planes[index>=4][y,x]=values[y,x]
 assert np.array_equal(planes[0]+planes[1],values)
 chunks=[f'#define BOOT_LOGO_WIDTH {width}\n#define BOOT_LOGO_HEIGHT {height}\n#define BOOT_LOGO_SPLIT {split}\n#define BOOT_LOGO_COLOR 0xF2006F\n'];descriptors=[];mask_bytes=0
 for side,(left,right) in enumerate(((0,120),(split,width))):
  assert not planes[side][:,:left].any() and not planes[side][:,right:].any()
  part=planes[side][:,left:right].reshape(-1)
  raw=bytes((part[::2]<<4)|part[1::2]);name=f'boot_logo_{side}';mask_bytes+=len(raw)
  chunks.append('static const uint8_t '+name+'[] = {\n'+ '\n'.join('  '+','.join(str(v) for v in raw[i:i+32])+',' for i in range(0,len(raw),32))+'\n};\n')
  descriptors.append('{.header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_A4,.w='+str(right-left)+',.h='+str(height)+',.stride='+str((right-left)//2)+'},.data_size=sizeof '+name+',.data='+name+'}')
 chunks.append('static const lv_image_dsc_t boot_logo[2] = {\n'+',\n'.join(descriptors)+'\n};\n')
 (ROOT/'components/ui/presentation/src/boot_logo.inc').write_text('/* Generated from the user-provided FoloToy wordmark; two Flash A4 masks. */\n'+''.join(chunks))
 c=av.open(str(SOURCE/'magicbox-open.mp3'));res=av.AudioResampler(format='s16',layout='mono',rate=16000);blocks=[]
 for f in c.decode(audio=0):
  blocks += [v.to_ndarray().reshape(-1) for v in res.resample(f)]
 blocks += [v.to_ndarray().reshape(-1) for v in res.resample(None)]
 pcm=np.concatenate([*blocks,np.zeros(1600,dtype='int16')]).astype('<i2')
 if len(pcm)%2: pcm=np.append(pcm,np.int16(0)).astype('<i2')
 data=encode(pcm);decoded=np.asarray(decode(data),dtype='<i2')
 out=ROOT/'components/platform/xiaozhi/startup_magicbox.adpcm';out.write_bytes(data)
 noise=np.mean((pcm.astype(float)-decoded)**2);power=np.mean(pcm.astype(float)**2)
 meta={'source':'FoloToy MagicBox product startup open.mp3','sample_rate':16000,'samples':len(pcm),'duration_ms':len(pcm)*1000//16000,'encoded_bytes':len(data),'encoded_sha256':hashlib.sha256(data).hexdigest(),'pcm_sha256':hashlib.sha256(pcm).hexdigest(),'snr_db':round(float(10*np.log10(power/noise)),2),'logo_mask_bytes':mask_bytes,'logo_size':[width,height],'logo_rgb':list(color),'source_sha256':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in SOURCE.iterdir() if p.suffix in ('.png','.mp3')}}
 (SOURCE/'prepared.json').write_text(json.dumps(meta,indent=2)+'\n');print(json.dumps(meta))
 if len(sys.argv)>1:
  with wave.open(sys.argv[1],'wb') as w:w.setparams((1,2,16000,0,'NONE','not compressed'));w.writeframes(decoded.tobytes())
if __name__=='__main__':main()
