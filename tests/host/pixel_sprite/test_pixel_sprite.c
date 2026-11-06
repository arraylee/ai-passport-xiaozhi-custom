#include "presentation/pixel_sprite.h"
#include "presentation/bobo_art.h"
#include "src/draw/lv_image_decoder_private.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Host-only fixture/framebuffer arrays are outside the measured LVGL heap. */
static uint8_t packed[70000], original[70000], indices[240*320], rgba[240*320*4], bgra[240*320*4];
static uint16_t draw_buffer[240*20], framebuffer[240*320], reference_frame[240*320];
static lv_image_dsc_t fixture;
static unsigned maximum_session_bytes, native_session_bytes;
static unsigned used_heap(void) {
    lv_mem_monitor_t memory; lv_mem_monitor(&memory);
    return (unsigned)(memory.total_size - memory.free_size);
}
static void write16(uint8_t *p, unsigned value) { p[0] = value; p[1] = value >> 8; }
static void make_fixture(unsigned width, unsigned height, unsigned colors) {
    memcpy(packed, "PSR1", 4); packed[4] = colors; memset(packed+5, 0, 3);
    static const uint8_t alpha[] = {0,64,127,255};
    for (unsigned i=0; i<colors; ++i) {
        uint8_t *color=packed+8+i*4;
        color[0]=i*47+3; color[1]=i*93+17; color[2]=i*29+5; color[3]=alpha[i%4];
    }
    for (unsigned y=0; y<height; ++y) for (unsigned x=0; x<width; ++x) {
        unsigned index=x<width/4 ? 0 : x<width/2 ? y%colors : (x*7+y*3)%colors;
        indices[y*width+x]=index;
        const uint8_t *color=packed+8+index*4; uint8_t *out=rgba+(y*width+x)*4;
        out[0]=color[2]; out[1]=color[1]; out[2]=color[0]; out[3]=color[3];
        memcpy(bgra+(y*width+x)*4, color, 4);
    }
    unsigned table=8+colors*4, base=table+(height+1)*2, length=0;
    for (unsigned y=0; y<height; ++y) {
        write16(packed+table+y*2, length);
        for (unsigned x=0; x<width;) {
            unsigned run=1;
            while (x+run<width && run<128 && indices[y*width+x+run]==indices[y*width+x]) ++run;
            if (run>=3) {
                packed[base+length++]=128+run-1; packed[base+length++]=indices[y*width+x]; x+=run;
            } else {
                unsigned start=x++;
                while (x<width && x-start<128) {
                    if (x+2<width && indices[y*width+x]==indices[y*width+x+1] && indices[y*width+x]==indices[y*width+x+2]) break;
                    ++x;
                }
                packed[base+length++]=x-start-1;
                memcpy(packed+base+length, indices+y*width+start, x-start); length+=x-start;
            }
        }
    }
    assert(length<=UINT16_MAX); write16(packed+table+height*2, length);
    fixture=(lv_image_dsc_t){.header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_RAW_ALPHA,
        .flags=PIXEL_SPRITE_FLAG,.w=width,.h=height},.data_size=base+length,.data=packed};
}
static lv_area_t first_area(void) { return (lv_area_t){LV_COORD_MIN,LV_COORD_MIN,LV_COORD_MIN,LV_COORD_MIN}; }
static void check_decode(const lv_image_dsc_t *image, const uint8_t *expected, lv_area_t full, bool premultiply) {
    assert(pixel_sprite_validate(image));
    lv_image_header_t header; assert(lv_image_decoder_get_info(image,&header)==LV_RESULT_OK);
    assert(header.cf==LV_COLOR_FORMAT_ARGB8888 && header.w==image->header.w && header.h==image->header.h);
    lv_image_decoder_dsc_t decoder;
    lv_image_decoder_args_t args={.no_cache=true,.premultiply=premultiply,.stride_align=true};
    unsigned before=used_heap(); assert(lv_image_decoder_open(&decoder,image,&args)==LV_RESULT_OK);
    unsigned session=used_heap()-before;
    if (session>maximum_session_bytes) maximum_session_bytes=session;
    if (image->header.w==120 && session>native_session_bytes) native_session_bytes=session;
    assert(session<=image->header.w*4u+128u && decoder.decoded==NULL && decoder.cache_entry==NULL);
    lv_area_t area=first_area();
    for (int y=full.y1; y<=full.y2; ++y) {
        assert(lv_image_decoder_get_area(&decoder,&full,&area)==LV_RESULT_OK);
        const lv_draw_buf_t *row=decoder.decoded;
        assert(area.x1==full.x1 && area.x2==full.x2 && area.y1==y && area.y2==y);
        assert(row->header.w==(unsigned)(full.x2-full.x1+1) && row->header.h==1);
        assert(row->header.cf==LV_COLOR_FORMAT_ARGB8888 && row->data_size<=image->header.w*4u+LV_DRAW_BUF_ALIGN);
        assert(used_heap()==before+session); /* No per-row allocations. */
        if (expected) for (int x=full.x1; x<=full.x2; ++x) {
            const uint8_t *pixel=expected+((unsigned)y*image->header.w+(unsigned)x)*4u;
            lv_color32_t color={pixel[2],pixel[1],pixel[0],pixel[3]};
            if (premultiply) lv_color_premultiply(&color);
            assert(!memcmp(row->data+(x-full.x1)*4,&color,4));
        }
    }
    assert(lv_image_decoder_get_area(&decoder,&full,&area)==LV_RESULT_INVALID);
    area=first_area(); assert(lv_image_decoder_get_area(&decoder,&full,&area)==LV_RESULT_OK);
    assert(used_heap()==before+session);
    lv_image_decoder_close(&decoder); assert(used_heap()==before);
}
static void reject(const lv_image_dsc_t *image) {
    assert(!pixel_sprite_validate(image));
    unsigned before=used_heap(); lv_image_decoder_dsc_t decoder;
    lv_result_t result=lv_image_decoder_open(&decoder,image,NULL);
    if (result==LV_RESULT_OK) lv_image_decoder_close(&decoder);
    assert(result==LV_RESULT_INVALID && used_heap()==before);
}
static void check_corruption(void) {
    make_fixture(120,120,64); memcpy(original,packed,fixture.data_size);
    lv_image_dsc_t damaged=fixture;
    for (unsigned n=0; n<fixture.data_size; ++n) { damaged.data_size=n; reject(&damaged); }
    damaged=fixture; damaged.data_size++; reject(&damaged); /* Trailing bytes. */
#define BAD_FIELD(field,value) do { damaged=fixture; damaged.field=value; reject(&damaged); } while(0)
    BAD_FIELD(data,NULL); BAD_FIELD(header.w,0); BAD_FIELD(header.w,241); BAD_FIELD(header.h,321);
    BAD_FIELD(header.stride,120); BAD_FIELD(header.flags,PIXEL_SPRITE_FLAG|LV_IMAGE_FLAGS_PREMULTIPLIED);
    BAD_FIELD(header.flags,0); BAD_FIELD(header.cf,LV_COLOR_FORMAT_ARGB8888);
    BAD_FIELD(header.magic,0); BAD_FIELD(header.reserved_2,1);
#undef BAD_FIELD
    const unsigned changes[]={0,4,5,6,7,264,265,266,264+120*2};
    for (unsigned i=0; i<sizeof(changes)/sizeof(changes[0]); ++i) {
        packed[changes[i]]^=0xff; reject(&fixture); memcpy(packed,original,fixture.data_size);
    }
    unsigned base=8+64*4+121*2;
    packed[4]=0; reject(&fixture); memcpy(packed,original,fixture.data_size);
    packed[base]=0xff; reject(&fixture); memcpy(packed,original,fixture.data_size); /* Overflow. */
    packed[base]=0x80; reject(&fixture); memcpy(packed,original,fixture.data_size); /* Underflow. */
    packed[base+1]=64; reject(&fixture); memcpy(packed,original,fixture.data_size); /* Bad index. */
    unsigned last=packed[264+119*2] | ((unsigned)packed[265+119*2]<<8);
    packed[base+last+1]=64; reject(&fixture); memcpy(packed,original,fixture.data_size); /* Hidden last row. */
    unsigned row_end=packed[266] | ((unsigned)packed[267]<<8);
    write16(packed+266,row_end-1); reject(&fixture); memcpy(packed,original,fixture.data_size); /* Truncated literal. */
    lv_image_decoder_dsc_t decoder; assert(lv_image_decoder_open(&decoder,&fixture,NULL)==LV_RESULT_OK);
    lv_area_t full={60,0,119,0}, area=first_area();
    packed[base+1]=64; /* Bad index outside the crop still must fail. */
    assert(lv_image_decoder_get_area(&decoder,&full,&area)==LV_RESULT_INVALID);
    memcpy(packed,original,fixture.data_size); write16(packed+266,UINT16_MAX); area=first_area();
    assert(lv_image_decoder_get_area(&decoder,&full,&area)==LV_RESULT_INVALID);
    memcpy(packed,original,fixture.data_size);
    static const lv_area_t invalid[]={{-1,0,0,0},{0,-1,0,0},{0,0,120,0},{0,0,0,120},{2,0,1,0},{0,2,0,1}};
    for (unsigned i=0; i<sizeof(invalid)/sizeof(invalid[0]); ++i) {
        area=first_area(); assert(lv_image_decoder_get_area(&decoder,&invalid[i],&area)==LV_RESULT_INVALID);
    }
    area=(lv_area_t){60,1,119,1}; assert(lv_image_decoder_get_area(&decoder,&full,&area)==LV_RESULT_INVALID);
    lv_image_decoder_close(&decoder);
    memmove(packed+1,packed,fixture.data_size); damaged=fixture; damaged.data=packed+1;
    check_decode(&damaged,rgba,(lv_area_t){0,0,119,119},false); /* Unaligned Flash bytes. */
}
static void flush(lv_display_t *display,const lv_area_t *area,uint8_t *pixels) {
    const uint16_t *source=(const uint16_t *)pixels;
    for (int y=area->y1;y<=area->y2;++y) for (int x=area->x1;x<=area->x2;++x) framebuffer[y*240+x]=*source++;
    lv_display_flush_ready(display);
}
static lv_obj_t *draw_image(const lv_image_dsc_t *image,int x,int y) {
    lv_obj_t *screen=lv_screen_active(); lv_obj_clean(screen);
    lv_obj_remove_style_all(screen); lv_obj_remove_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen,lv_color_hex(0x27485D),0); lv_obj_set_style_bg_opa(screen,LV_OPA_COVER,0);
    lv_obj_t *object=lv_image_create(screen); lv_image_set_src(object,image); lv_obj_set_pos(object,x,y);
    lv_obj_update_layout(screen); lv_obj_invalidate(screen); lv_refr_now(NULL); return object;
}
static void check_rendering(void) {
    make_fixture(120,120,64);
    lv_image_dsc_t raw={.header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_ARGB8888,.w=120,.h=120,.stride=480},
        .data_size=120*120*4,.data=bgra};
    static const int positions[][2]={{60,80},{-15,5},{217,297},{-37,-59}};
    for (unsigned i=0;i<4;++i) {
        draw_image(&raw,positions[i][0],positions[i][1]); memcpy(reference_frame,framebuffer,sizeof(framebuffer));
        draw_image(&fixture,positions[i][0],positions[i][1]); assert(!memcmp(framebuffer,reference_frame,sizeof(framebuffer)));
    }
    lv_obj_t *image=draw_image(&fixture,60,80); unsigned before=used_heap();
    for (unsigned i=0;i<200;++i) {
        lv_obj_set_pos(image,56+i%9,72+i%10); lv_obj_update_layout(lv_screen_active()); lv_refr_now(NULL);
        assert(used_heap()==before && lv_mem_test()==LV_RESULT_OK);
    }
    lv_obj_clean(lv_screen_active()); lv_refr_now(NULL);
}
static void check_generated_assets(void) {
    const char *directory=getenv("PIXEL_SPRITE_REFERENCE_DIR");
    size_t count=bobo_art_asset_count();
    assert(count > 0 && count <= PET_ART_PACK_MAX_COUNT);
    assert(!bobo_art_asset(count) && !bobo_art_asset(SIZE_MAX));
    uint32_t last_id=0;
    for (size_t i=0;i<count;++i) {
        const pet_art_asset_t *asset=bobo_art_asset(i); assert(asset && asset->image);
        const lv_image_dsc_t *image=asset->image;
        assert(!i || asset->id > last_id); last_id=asset->id;
        assert(image->header.w==asset->width && image->header.h==asset->height);
        assert(image->data_size==asset->size);
        if (!pixel_sprite_validate(image)) fprintf(stderr,"Invalid generated PSR1 ID: %08x\n",(unsigned)asset->id);
        assert(pixel_sprite_validate(image));
        if (directory) {
            /* prepare_references.py resolves catalog IDs to manifest PNG names. */
            char path[1024]; snprintf(path,sizeof(path),"%s/%08x.rgba",directory,(unsigned)asset->id);
            FILE *file=fopen(path,"rb");
            if (!file) fprintf(stderr,"Missing native PNG reference: %s\n",path);
            assert(file); unsigned length=image->header.w*image->header.h*4u;
            assert(fread(rgba,1,length,file)==length && fgetc(file)==EOF); fclose(file);
        }
        lv_area_t full={0,0,image->header.w-1,image->header.h-1};
        check_decode(image,directory?rgba:NULL,full,false);
        check_decode(image,directory?rgba:NULL,full,true);
        if (image->header.w>20 && image->header.h>24)
            check_decode(image,directory?rgba:NULL,(lv_area_t){7,11,image->header.w-9,image->header.h-13},false);
    }
    printf("Generated assets: %zu PSR1 sprites validated/decoded%s.\n",count,
           directory?", pixel-exact against PNG RGBA fixtures":" (PNG comparison requires PIXEL_SPRITE_REFERENCE_DIR)");
}

int main(void) {
    lv_init(); assert(pixel_sprite_init()==LV_RESULT_OK); unsigned registered=used_heap();
    for (unsigned i=0;i<20;++i) assert(pixel_sprite_init()==LV_RESULT_OK); assert(used_heap()==registered);
    const unsigned dimensions[][3]={{120,120,64},{80,80,32},{240,3,64},{1,320,1}};
    for (unsigned i=0;i<4;++i) {
        unsigned w=dimensions[i][0],h=dimensions[i][1]; make_fixture(w,h,dimensions[i][2]);
        check_decode(&fixture,rgba,(lv_area_t){0,0,w-1,h-1},false);
        check_decode(&fixture,rgba,(lv_area_t){0,0,w-1,h-1},true);
        if (w>20 && h>20) check_decode(&fixture,rgba,(lv_area_t){7,11,w-9,h-13},false);
    }
    check_corruption(); check_generated_assets();
    lv_display_t *display=lv_display_create(240,320); lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display,draw_buffer,NULL,sizeof(draw_buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush); check_rendering(); assert(lv_mem_test()==LV_RESULT_OK);
    printf("Pixel sprite PASS: exact RGBA/alpha/premultiply, native sizes, crop/restart, bad/truncated rows, %zu resources, RGB565 parity, 200 moving draws without heap growth. Session heap: 120px=%u B, max 240px=%u B; one row.\n",bobo_art_asset_count(),native_session_bytes,maximum_session_bytes);
    lv_deinit(); lv_init(); assert(pixel_sprite_init()==LV_RESULT_OK); lv_deinit(); return 0;
}
