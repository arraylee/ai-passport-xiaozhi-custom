#include "presentation/bobo_art.h"
#include "presentation/pet_art_pack.h"
#include "presentation/pixel_sprite.h"
#include "src/draw/lv_image_decoder_private.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t read32(const uint8_t *p) { return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void write32(uint8_t *p, uint32_t v) { for (unsigned i=0;i<4;++i) p[i]=v>>(8*i); }
static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t crc=~0u;
    while(n--) { crc^=*p++; for(unsigned b=0;b<8;++b) crc=(crc>>1)^(0xedb88320u&(0u-(crc&1))); }
    return ~crc;
}
static void repair_crcs(uint8_t *p, size_t n) {
    size_t base=read32(p+24); assert(base<=n);
    write32(p+28,crc32(p+64,base-64)); write32(p+32,crc32(p+base,n-base));
}
static void rejected(const void *data,size_t size) {
    assert(!bobo_art_bind(data,size)); assert(!bobo_art_ready());
    for (size_t i=0;i<bobo_art_asset_count();++i) {
        const pet_art_asset_t *asset=bobo_art_asset(i); assert(asset);
        assert(!asset->image->data && !asset->image->data_size);
    }
    assert(!pixel_sprite_validate(&bobo_art_wave));
    lv_image_decoder_dsc_t decoder;
    assert(lv_image_decoder_open(&decoder,&bobo_art_wave,NULL)==LV_RESULT_INVALID);
}
int main(void) {
    lv_init(); assert(pixel_sprite_init()==LV_RESULT_OK);
    FILE *file=fopen(PET_ART_TEST_FILE,"rb"); assert(file);
    fseek(file,0,SEEK_END); size_t size=(size_t)ftell(file); rewind(file);
    uint8_t *data=malloc(size),*bad=malloc(size+1); assert(data&&bad);
    assert(fread(data,1,size,file)==size); fclose(file);
    rejected(NULL,0); rejected(data,63); rejected(data,size-1); rejected(data,size+1);
    assert(bobo_art_bind(data,size)&&bobo_art_ready()); assert(pixel_sprite_validate(&bobo_art_wave));
    assert(bobo_art_wave.data>=data && bobo_art_wave.data+bobo_art_wave.data_size<=data+size);
    rejected(NULL,size); /* A failed rebind clears a previously valid catalog. */
    for(size_t field=0;field<64;++field) {
        memcpy(bad,data,size); bad[field]^=0x80; rejected(bad,size);
    }
    memcpy(bad,data,size); bad[size-1]^=1; rejected(bad,size);
    /* Recomputed CRC cannot make foreign IDs/dimensions/offsets match app. */
    static const unsigned fields[]={64,68,70,72,76,80,84};
    for(unsigned i=0;i<sizeof(fields)/sizeof(fields[0]);++i) {
        memcpy(bad,data,size); bad[fields[i]]^=1; repair_crcs(bad,size); rejected(bad,size);
    }
    memcpy(bad,data,size); write32(bad+72,UINT32_MAX); repair_crcs(bad,size); rejected(bad,size);
    memcpy(bad,data,size); write32(bad+76,UINT32_MAX); repair_crcs(bad,size); rejected(bad,size);
    memcpy(bad,data,size); memcpy(bad+88,bad+64,24); repair_crcs(bad,size); rejected(bad,size);
    memcpy(bad,data,size); uint8_t entry[24]; memcpy(entry,bad+64,24);
    memcpy(bad+64,bad+88,24); memcpy(bad+88,entry,24); repair_crcs(bad,size); rejected(bad,size);
    /* Malformed PSR1 remains rejected even with a matching custom catalog. */
    uint8_t tiny[106]={0}; memcpy(tiny,"PTP1\1\0\x40\0",8);
    write32(tiny+8,sizeof(tiny)); write32(tiny+12,1); write32(tiny+16,24);
    write32(tiny+20,64); write32(tiny+24,88); write32(tiny+64,1);
    tiny[68]=tiny[70]=1; write32(tiny+72,88); write32(tiny+76,18);
    memcpy(tiny+88,"PSR1\1\0\0\0",8); tiny[104]=0x80; tiny[105]=1; /* palette index out of range */
    tiny[102]=2; write32(tiny+80,crc32(tiny+88,18)); repair_crcs(tiny,sizeof(tiny));
    lv_image_dsc_t image={0}; pet_art_asset_t catalog={1,1,1,18,read32(tiny+80),&image};
    assert(!pet_art_pack_bind(tiny,sizeof(tiny),&catalog,1,bobo_art_pack_fingerprint));
    assert(!image.data);
    assert(bobo_art_bind(data,size));
    bobo_art_unbind(); assert(!bobo_art_ready()); free(data); free(bad); lv_deinit();
    printf("PTP1 bounds, corruption, catalog, order, atomic failure and missing-pack rendering: OK\n");
    return 0;
}
