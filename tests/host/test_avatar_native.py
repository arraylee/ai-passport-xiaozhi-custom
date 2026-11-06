"""Real demo pixels remain identical and can be read without PNG inflation."""
from pathlib import Path
import io
import subprocess
import tempfile
import unittest
from PIL import Image
from tools.avatar_pack import build_pack

ROOT = Path(__file__).resolve().parents[2]
CODE = r'''
#include "services/avatar_pack.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t crc32_ref(const unsigned char *p,size_t n) {
  uint32_t crc=~0u;while(n--){crc^=*p++;for(unsigned i=0;i<8;i++)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));}return ~crc;
}
static void put32(unsigned char *p,uint32_t n){p[0]=n;p[1]=n>>8;p[2]=n>>16;p[3]=n>>24;}
static uint32_t get32(const unsigned char *p){return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static void repair_index(unsigned char *pack,const char *name) {
  for(unsigned i=0;i<pack[5];i++) {
    unsigned char *e=pack+16+i*28;
    if(!strcmp((char *)e,name))put32(e+24,crc32_ref(pack+get32(e+16),get32(e+20)));
  }
  put32(pack+12,crc32_ref(pack+16,pack[5]*28));
}
int main(int argc,char **argv) {
  assert(argc==4);
  FILE *f=fopen(argv[1],"rb");assert(f);
  unsigned char *pack=malloc(1048576);assert(pack);
  assert(fread(pack,1,1048576,f)==1048576);fclose(f);
  avatar_pack_t catalog;assert(!avatar_pack_init(&catalog,pack,1048576));
  const unsigned char *pixels=NULL;size_t size=0;
  if(!strcmp(argv[3],"legacy")) {
    assert(avatar_pack_find_native(&catalog,argv[2],&pixels,&size)==AVATAR_PACK_NOT_FOUND);
    assert(!pixels&&!size);free(pack);return 0;
  }
  assert(!avatar_pack_find_native(&catalog,argv[2],&pixels,&size));
  assert(size==96*156*4 && pixels>=pack && pixels+size<=pack+1048576);
  assert(((uintptr_t)pixels&3)==0); // C3/LVGL reads native color words.
  f=fopen(argv[3],"rb");assert(f);
  for(size_t i=0;i<size;i++)assert(fgetc(f)==pixels[i]);fclose(f);
  // Immutable frame data comes from the pack, with no full-image allocation.
  unsigned char *raw=(unsigned char *)pixels;
  raw[0]^=1;
  assert(avatar_pack_find_native(&catalog,argv[2],&pixels,&size)==AVATAR_PACK_ERR_CRC);
  repair_index(pack,argv[2]); // A well-formed AVA1 wrapper cannot bypass PNG-cache CRC.
  assert(avatar_pack_find_native(&catalog,argv[2],&pixels,&size)==AVATAR_PACK_ERR_CRC);
  memset(raw-11,0xff,4);repair_index(pack,argv[2]);
  assert(avatar_pack_find_native(&catalog,argv[2],&pixels,&size)==AVATAR_PACK_ERR_BOUNDS);
  assert(!pixels&&!size);
  free(pack);return 0;
}
'''


class NativeAvatarTest(unittest.TestCase):
    def test_every_original_demo_avatar_matches_its_native_flash_frame(self):
        source = ROOT/'assets/avatars'
        packed = build_pack(source,0x100000,native_cache=True)
        import struct,zlib
        with tempfile.TemporaryDirectory() as directory:
            tmp=Path(directory);(tmp/'pack.bin').write_bytes(packed)
            c=tmp/'native.c';c.write_text(CODE);exe=tmp/'native'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                '-fsanitize=undefined','-I',str(ROOT/'components/core/services/include'),
                str(c),str(ROOT/'components/core/services/src/avatar_pack.c'),'-o',str(exe)],check=True)
            for i in range(packed[5]):
                raw_name,offset,length,crc=struct.unpack_from('<16sIII',packed,16+i*28)
                name=raw_name.split(b'\0')[0].decode();png=packed[offset:offset+length]
                self.assertEqual(crc,zlib.crc32(png)&0xffffffff)
                with Image.open(source/(name+'.png')) as original, Image.open(io.BytesIO(png)) as cached:
                    self.assertEqual(original.tobytes(),cached.tobytes())
                    expected=original.tobytes('raw','BGRA')
                (tmp/'golden.bgra').write_bytes(expected)
                subprocess.run([str(exe),str(tmp/'pack.bin'),name,str(tmp/'golden.bgra')],check=True)
            (tmp/'legacy.bin').write_bytes(build_pack(source,0x100000))
            subprocess.run([str(exe),str(tmp/'legacy.bin'),name,'legacy'],check=True)


if __name__=='__main__':unittest.main()
