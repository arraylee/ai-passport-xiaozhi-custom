"""Regenerate the small startup masks/audio. Requires Pillow, numpy, av, pymupdf."""
from pathlib import Path
import hashlib,json,wave,sys
import numpy as np
from PIL import Image
import av,pymupdf
from adpcm_codec import encode,decode
ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'assets/source/boot'
def main():
 doc=pymupdf.open(SOURCE/'folotoy-logo.svg')
 pix=doc[0].get_pixmap(matrix=pymupdf.Matrix(100/945,100/945),alpha=True)
 im=Image.frombytes('RGBA',[pix.width,pix.height],pix.samples)
 # MuPDF's raw samples are premultiplied; invert that before extracting the
 # white lettering over the pink bubble. Both layers retain antialiased edges.
 px=np.asarray(im).astype(float);a=px[:,:,3]
 green=np.divide(px[:,:,1]*255,a,out=np.zeros_like(a),where=a>0)
 white=np.clip((green-39)/(255-39),0,1)*a
 planes=[a,white];chunks=[];descriptors=[]
 for layer,alpha in enumerate(planes):
  for side in range(2):
   values=np.rint(alpha[:,side*50:(side+1)*50]/17).clip(0,15).astype('uint8').reshape(-1)
   raw=bytes((values[::2]<<4)|values[1::2]);name=f'boot_logo_{layer}_{side}'
   chunks.append('static const uint8_t '+name+'[] = {\n'+ '\n'.join('  '+','.join(str(v) for v in raw[i:i+32])+',' for i in range(0,len(raw),32))+'\n};\n')
   descriptors.append('{.header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_A4,.w=50,.h=100,.stride=25},.data_size=sizeof '+name+',.data='+name+'}')
 chunks.append('static const lv_image_dsc_t boot_logo[4] = {\n'+',\n'.join(descriptors)+'\n};\n')
 (ROOT/'components/ui/presentation/src/boot_logo.inc').write_text('/* Generated from the official FoloToy SVG; four Flash-resident A4 masks. */\n'+''.join(chunks))
 c=av.open(str(SOURCE/'magicbox-open.mp3'));res=av.AudioResampler(format='s16',layout='mono',rate=16000);blocks=[]
 for f in c.decode(audio=0):
  blocks += [v.to_ndarray().reshape(-1) for v in res.resample(f)]
 blocks += [v.to_ndarray().reshape(-1) for v in res.resample(None)]
 pcm=np.concatenate([*blocks,np.zeros(1600,dtype='int16')]).astype('<i2')
 if len(pcm)%2: pcm=np.append(pcm,np.int16(0)).astype('<i2')
 data=encode(pcm);decoded=np.asarray(decode(data),dtype='<i2')
 out=ROOT/'components/platform/xiaozhi/startup_magicbox.adpcm';out.write_bytes(data)
 noise=np.mean((pcm.astype(float)-decoded)**2);power=np.mean(pcm.astype(float)**2)
 meta={'source':'FoloToy MagicBox product startup open.mp3','sample_rate':16000,'samples':len(pcm),'duration_ms':len(pcm)*1000//16000,'encoded_bytes':len(data),'encoded_sha256':hashlib.sha256(data).hexdigest(),'pcm_sha256':hashlib.sha256(pcm).hexdigest(),'snr_db':round(float(10*np.log10(power/noise)),2),'logo_mask_bytes':10000,'source_sha256':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in SOURCE.iterdir() if p.suffix in ('.svg','.mp3')}}
 (SOURCE/'prepared.json').write_text(json.dumps(meta,indent=2)+'\n');print(json.dumps(meta))
 if len(sys.argv)>1:
  with wave.open(sys.argv[1],'wb') as w:w.setparams((1,2,16000,0,'NONE','not compressed'));w.writeframes(decoded.tobytes())
if __name__=='__main__':main()
