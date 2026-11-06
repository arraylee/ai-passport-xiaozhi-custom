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
 # Keep the whole small wordmark in one Flash mask. The native renderer
 # translates it over black; no PNG decoder or animation frame cache is used.
 im=Image.open(SOURCE/'folotoy-logo-pink.png').convert('RGBA')
 color=(242,0,111);width=144;height=round(width*im.height/im.width)
 alpha=np.asarray(im.getchannel('A').resize((width,height),Image.Resampling.LANCZOS))
 values=np.rint(alpha.astype(float)/17).clip(0,15).astype('uint8').reshape(-1)
 raw=bytes((values[::2]<<4)|values[1::2]);mask_bytes=len(raw)
 header=f'#define BOOT_LOGO_WIDTH {width}\n#define BOOT_LOGO_HEIGHT {height}\n#define BOOT_LOGO_COLOR 0xF2006F\n'
 pixels='static const uint8_t boot_logo_pixels[] = {\n'+'\n'.join('  '+','.join(str(v) for v in raw[i:i+32])+',' for i in range(0,len(raw),32))+'\n};\n'
 descriptor='static const lv_image_dsc_t boot_logo = {.header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_A4,.w='+str(width)+',.h='+str(height)+',.stride='+str(width//2)+'},.data_size=sizeof boot_logo_pixels,.data=boot_logo_pixels};\n'
 (ROOT/'components/ui/presentation/src/boot_logo.inc').write_text('/* Generated from the user-provided FoloToy wordmark; one Flash A4 mask. */\n'+header+pixels+descriptor)
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
