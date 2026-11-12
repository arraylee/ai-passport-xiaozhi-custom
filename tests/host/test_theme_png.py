"""Pixel-oracle and malformed-input tests for custom official-theme PNGs."""
from pathlib import Path
import json, os, random, struct, subprocess, tempfile, unittest, zlib

ROOT=Path(__file__).resolve().parents[2]
XZ=ROOT/'components/platform/xiaozhi'
ADAM7=((0,0,8,8),(4,0,8,8),(0,4,4,8),(2,0,4,4),(0,2,2,4),(1,0,2,2),(0,1,1,2))

def chunk(tag,payload):
    return struct.pack('>I',len(payload))+tag+payload+struct.pack('>I',zlib.crc32(tag+payload))

def png(w,h,kind=6,depth=8,interlace=0,split=997,filters=True,trns=False,noise=False,edge=32):
    channels={0:1,2:3,3:1,4:2,6:4}[kind]
    limit=(1<<depth)-1
    palette=bytes(v for i in range(min(limit+1,256)) for v in (i,i^127,255-i)) if kind==3 else b''
    transparent=(3,2,1) if kind==2 else (0,)
    alpha=(bytes(range(min(limit+1,256))) if kind==3 else
           b''.join(struct.pack('>H',v) for v in transparent)) if trns else b''
    rng=random.Random(17)
    values=[tuple((rng.randrange(limit+1) if noise else (x*263+y*257+c*8911)&limit) for c in range(channels))
            for y in range(h) for x in range(w)]
    if trns and kind in (0,2):values[0]=transparent
    raw=bytearray()
    def paeth(a,b,c):
        p=a+b-c;d=[abs(p-a),abs(p-b),abs(p-c)]
        return (a,b,c)[d.index(min(d))]
    for x0,y0,dx,dy in ADAM7 if interlace else ((0,0,1,1),):
        if x0>=w or y0>=h:continue
        previous=None
        for y in range(y0,h,dy):
            samples=[v for x in range(x0,w,dx) for v in values[y*w+x]]
            if depth==16:row=b''.join(struct.pack('>H',v) for v in samples)
            elif depth==8:row=bytes(samples)
            else:
                row=bytearray((len(samples)*depth+7)//8)
                for i,v in enumerate(samples):row[i*depth//8]|=v<<(8-depth-i*depth%8)
                row=bytes(row)
            bpp=(depth*channels+7)//8;method=y%5 if filters else 0
            coded=bytearray([method])
            for i,v in enumerate(row):
                a=row[i-bpp] if i>=bpp else 0;b=previous[i] if previous else 0
                c=previous[i-bpp] if previous and i>=bpp else 0
                predictor=(0,a,b,(a+b)//2,paeth(a,b,c))[method]
                coded.append((v-predictor)&255)
            raw.extend(coded);previous=row
    packed=zlib.compress(raw)
    header=struct.pack('>IIBBBBB',w,h,depth,kind,0,0,interlace)
    data=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',header)
    if palette:data+=chunk(b'PLTE',palette)
    if alpha:data+=chunk(b'tRNS',alpha)
    data+=b''.join(chunk(b'IDAT',packed[i:i+split]) for i in range(0,len(packed),split))+chunk(b'IEND',b'')
    rgba=bytearray()
    for q in values:
        c=[v>>8 if depth==16 else v for v in q]
        if kind==3:c=list(palette[3*q[0]:3*q[0]+3])+[alpha[q[0]] if q[0]<len(alpha) else 255]
        elif kind in (0,4):
            gray=q[0]*255//limit if depth<8 else c[0]
            c=[gray]*3+[c[1] if kind==4 else 0 if trns and q==transparent else 255]
        else:c=c[:3]+[c[3] if kind==6 else 0 if trns and q==transparent else 255]
        rgba.extend(c)
    dw=edge if w>=h else max(1,(w*edge+h//2)//h)
    dh=edge if h>=w else max(1,(h*edge+w//2)//w)
    left=(edge-dw)//2;top=(edge-dh)//2;expected=bytearray(edge*edge*4)
    for y in range(dh):
        for x in range(dw):
            source=((y*h//dh)*w+x*w//dw)*4;dest=((top+y)*edge+left+x)*4
            expected[dest:dest+4]=rgba[source:source+4]
    return data,bytes(expected)

class CustomPng(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();cls.folder=Path(cls.temp.name)
        if os.environ.get('PASSPORT_PNG_DECODER_TEST'):
            cls.exe=Path(os.environ['PASSPORT_PNG_DECODER_TEST']).resolve()
            return
        cls.exe=cls.folder/'decode'
        flags=['cc','-std=c11','-g','-O1','-fno-omit-frame-pointer','-fsanitize=address,undefined',
               '-I'+str(XZ),'-I'+str(XZ/'include')]
        obj=cls.folder/'image.o'
        subprocess.run([*flags,'-Dmalloc=png_test_malloc','-Dcalloc=png_test_calloc','-Dfree=png_test_free',
                        '-c',str(XZ/'xz_theme_image.c'),'-o',str(obj)],check=True)
        subprocess.run([*flags,str(ROOT/'tests/host/theme_image_harness.c'),str(obj),'-lz','-o',str(cls.exe)],check=True)
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def decode(self,data,expected=None,fail=0,edge=32):
        image=self.folder/'test.png';out=self.folder/'result.rgba';image.write_bytes(data)
        p=subprocess.run([str(self.exe),str(image),str(out),'1' if expected is not None else '0',str(fail),str(edge)],
                         capture_output=True,text=True,timeout=15)
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        if expected is not None:self.assertEqual(out.read_bytes(),expected)
        return p.stdout
    def test_portrait_color_modes_and_adam7(self):
        for kind,depths in [(0,[1,2,4,8,16]),(2,[8,16]),(3,[1,2,4,8]),(4,[8,16]),(6,[8,16])]:
            for depth in depths:
                for interlace in [0,1]:
                    with self.subTest(kind=kind,depth=depth,interlace=interlace):
                        data,want=png(240,320,kind,depth,interlace,trns=kind in [0,2,3])
                        self.decode(data,want)
                        data,want=png(240,320,kind,depth,interlace,trns=kind in [0,2,3],edge=64)
                        self.decode(data,want,edge=64)
    def test_tiny_and_narrow_images(self):
        for w,h in [(1,1),(1,320),(240,1),(2,3),(3,2),(31,17),(128,128),(240,240)]:
            for interlace in [0,1]:
                with self.subTest(w=w,h=h,interlace=interlace):self.decode(*png(w,h,interlace=interlace))
    def test_idat_boundaries_and_large_compressed_input(self):
        for split in [1,2,7,32768]:
            with self.subTest(split=split):self.decode(*png(37,51,split=split,noise=True))
        data,want=png(240,320,split=4096,noise=True)
        self.assertGreater(len(data),80000)
        self.decode(data,want)
    def test_large_cache_memory_bounds_and_allocation_failures(self):
        data,want=png(200,200,interlace=1,edge=64)
        result=self.decode(data,want,edge=64)
        self.assertLessEqual(int(result.split('peak=')[1]),70000)
        for fail in [1,2,3]:self.decode(data,fail=fail,edge=64)
        for edge in [0,31,128]:self.decode(data,edge=edge)
    def test_allocation_failure_and_sink_atomicity(self):
        data,want=png(240,320)
        for fail in [1,2,3]:self.decode(data,fail=fail)
        self.decode(data,want)
    def test_corrupt_truncated_and_invalid_images(self):
        data,_=png(35,51,split=997)
        for n in [8,16,33,len(data)-12,len(data)-1]:self.decode(data[:n])
        broken=bytearray(data);broken[-20]^=1;self.decode(bytes(broken))
        # Correct PNG CRC does not conceal a corrupt zlib checksum.
        data,_=png(35,51,split=100000)
        at=data.index(b'IDAT')-4;n=struct.unpack_from('>I',data,at)[0]
        body=bytearray(data[at+8:at+8+n]);body[-1]^=1
        self.decode(data[:at]+chunk(b'IDAT',body)+data[at+n+12:])
        for w,h in [(241,320),(240,321)]:self.decode(png(w,h)[0])
        # Valid compression but extra decoded bytes, invalid filter, or an invalid palette index.
        for raw in [b'\0'+b'\xff'*4+b'x',b'\5'+b'\xff'*4]:
            self.decode(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',1,1,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND',b''))
        for _ in range(20):self.decode(data[:-4])

if __name__=='__main__':unittest.main()
