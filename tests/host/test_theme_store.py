"""Install an official-format custom PNG, reboot, and exercise interrupted writes."""
from pathlib import Path
import json, os, struct, subprocess, tempfile, unittest
from test_theme_png import png

ROOT=Path(__file__).resolve().parents[2]
EMOTIONS='neutral happy laughing funny sad angry crying loving embarrassed surprised shocked thinking winking cool relaxed delicious kissy confident sleepy silly confused'.split()

def pack(shared,unique=1,small=False,size=(240,320)):
    index={'version':1,'chip_model':'esp32c3','display_config':{'width':240,'height':320,'color':'RGB565'},
           'emoji_collection':[{'name':e,'file':f'emoji_{i if unique>1 else 0}.png'} for i,e in enumerate(EMOTIONS[:unique] if unique>1 else EMOTIONS if shared else ['neutral'])]}
    entries={'index.json':json.dumps(index).encode()}
    image=png(32,32)[0] if small else png(*size,interlace=1)[0]
    entries.update({f'emoji_{i}.png':image for i in range(unique)})
    table=bytearray();data=bytearray()
    for name,body in entries.items():
        table+=struct.pack('<32sIIHH',name.encode(),len(body),len(data),0,0);data+=b'ZZ'+body
    body=table+data
    return struct.pack('<III',len(entries),sum(body)&65535,len(body))+body

class ThemeStore(unittest.TestCase):
    def test_shared_png_install_reboot_and_failure_isolation(self):
        sdk=os.environ.get('IDF_PATH') or json.loads((ROOT/'build/project_description.json').read_text())['idf_path']
        xz=ROOT/'components/platform/xiaozhi';cjson=Path(sdk)/'components/json/cJSON'
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory);shim=base/'stubs';(shim/'freertos').mkdir(parents=True)
            for name in ['esp_crt_bundle.h','esp_http_client.h','esp_partition.h','esp_rom_crc.h','esp_system.h','freertos/FreeRTOS.h','freertos/task.h','nvs.h']:
                (shim/name).write_text('#include "theme_store_stubs.h"\n')
            inc=[shim,ROOT/'tests/host',xz,xz/'include',cjson,ROOT/'components/core/ports/include']
            exe=base/'store'
            subprocess.run(['cc','-std=c11','-O1','-g','-fsanitize=address,undefined','-Wno-deprecated-declarations',
                            *['-I'+str(p) for p in inc],str(ROOT/'tests/host/theme_store_board.c'),
                            *[str(xz/(n+'.c')) for n in ['xz_theme','xz_theme_store','xz_theme_font','xz_theme_image','xz_theme_transfer']],
                            str(cjson/'cJSON.c'),'-lz','-o',str(exe)],check=True)
            (base/'fixtures').mkdir()
            supplied=os.environ.get('PASSPORT_STORE_PACKS','').split(os.pathsep)
            inputs=[(pack(False),64,1,128),(pack(True),64,1,128),(pack(True,small=True),32,1,32),
                    (pack(False,7),64,7,128),(pack(False,21),32,21,128)]
            for p in supplied:
                if not p:continue
                data=Path(p).read_bytes()
                # Real official packs used here contain one shared static PNG.
                start=data.index(b'\x89PNG\r\n\x1a\n');w,h=struct.unpack_from('>II',data,start+16)
                inputs.append((data,64 if max(w,h)>64 else 32,1,min(128,max(w,h)) if max(w,h)>64 else 32))
            for number,(data,edge,count,display) in enumerate(inputs):
                source=base/'fixture.bin';source.write_bytes(data)
                for mode in ['blank','receive','warm','cache-corrupt','old-cache','bad-edge','offline','power-loss','bad-index','cache-fail','broken-pack','bad-chip','cancel','oversize-download']:
                    with self.subTest(pack=number,mode=mode):
                        p=subprocess.run([str(exe),mode,str(source),str(edge),str(count),str(display)],cwd=base,capture_output=True,text=True,timeout=15)
                        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
            source.write_bytes(pack(False,size=(500,500)))
            p=subprocess.run([str(exe),'oversize-png',str(source),'64','1','128'],cwd=base,capture_output=True,text=True,timeout=15)
            self.assertEqual(p.returncode,0,p.stdout+p.stderr)

if __name__=='__main__':unittest.main()
