#include "presentation/ui_xiaozhi.h"
#include "presentation/ui_theme.h"
#include "presentation/ui_xiaozhi_theme.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "battery_pixels.h"
static uint16_t buffer[240*20];
static uint8_t face_pixels[144*144*3];
static lv_image_dsc_t face_frames[COMPANION_FACE_COUNT];
static companion_portrait_t face_art={.frames=face_frames};
static lv_obj_t *find_face(lv_obj_t *o) {
    if(lv_obj_check_type(o,&lv_image_class))return o;
    for(unsigned i=0;i<lv_obj_get_child_count(o);i++){lv_obj_t *f=find_face(lv_obj_get_child(o,i));if(f)return f;}
    return NULL;
}
static void step(unsigned ms) {lv_tick_inc(ms);lv_timer_handler();lv_obj_update_layout(lv_screen_active());}
static void assert_face(unsigned face) {
    lv_obj_t *image=find_face(lv_screen_active());assert(image);
    assert(lv_image_get_src(image)==&face_frames[face]);
    assert(lv_obj_get_x(image)==48 && lv_obj_get_y(image)==66);
}

static unsigned flushed_pixels;
static uint16_t mouth_pixels[17][21];
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *p){
    flushed_pixels+=(a->x2-a->x1+1)*(a->y2-a->y1+1);collect_gauge(a,p);
    const uint16_t *src=(const uint16_t *)p;int stride=a->x2-a->x1+1;
    for(int y=173;y<=189;y++)if(y>=a->y1&&y<=a->y2)
        for(int x=112;x<=132;x++)if(x>=a->x1&&x<=a->x2)
            mouth_pixels[y-173][x-112]=src[(y-a->y1)*stride+x-a->x1];
    lv_display_flush_ready(d);
}
static bool has_text(lv_obj_t *o, const char *text) {
    if(lv_obj_check_type(o,&lv_label_class) && !strcmp(lv_label_get_text(o),text))return true;
    for(unsigned i=0;i<lv_obj_get_child_count(o);i++)if(has_text(lv_obj_get_child(o,i),text))return true;
    return false;
}
static lv_obj_t *label(lv_obj_t *o,const char *s) {
    if(lv_obj_check_type(o,&lv_label_class)&&!strcmp(lv_label_get_text(o),s))return o;
    for(unsigned i=0;i<lv_obj_get_child_count(o);i++){lv_obj_t *found=label(lv_obj_get_child(o,i),s);if(found)return found;}
    return NULL;
}
static void bounds(lv_obj_t *o){
    if(lv_obj_has_flag(o,LV_OBJ_FLAG_HIDDEN))return;
    lv_area_t a;lv_obj_get_coords(o,&a);
    assert(a.x1>=0&&a.y1>=0&&a.x2<240&&a.y2<320);
    for(unsigned i=0;i<lv_obj_get_child_count(o);i++)bounds(lv_obj_get_child(o,i));
}
static void assert_binding_layout(void) {
    const char *steps[]={"联网成功","请前往 xiaozhi.me","登录并进入",
                         "添加设备时输入","123456","绑定"};
    const int rows[]={65,101,136,170,207,244};
    lv_area_t previous={0};
    for(unsigned i=0;i<sizeof steps/sizeof steps[0];i++) {
        lv_obj_t *line=label(lv_screen_active(),steps[i]);assert(line);
        const lv_font_t *font=i==4?&lv_font_montserrat_28:&ui_font_title;
        assert(lv_obj_get_style_text_font(line,0)==font);
        assert(lv_obj_get_height(line)==font->line_height);
        lv_area_t area;lv_obj_get_coords(line,&area);
        assert(area.y1==rows[i]);
        if(i)assert(previous.y2<area.y1);
        previous=area;
    }
    lv_obj_t *login=label(lv_screen_active(),"登录并进入");assert(login);
    lv_obj_t *console=label(lv_screen_active(),"控制台");assert(console);
    assert(lv_obj_get_style_text_font(console,0)==&ui_font_title);
    assert(lv_obj_get_height(console)==ui_font_title.line_height);
    assert(lv_color_eq(lv_obj_get_style_text_color(login,0),lv_color_hex(XZ_UI_INK)));
    assert(lv_color_eq(lv_obj_get_style_text_color(console,0),lv_color_hex(XZ_UI_ACCENT)));
    lv_area_t login_area,console_area;
    lv_obj_get_coords(login,&login_area);lv_obj_get_coords(console,&console_area);
    assert(login_area.y1==console_area.y1&&login_area.y2==console_area.y2);
    assert(login_area.x2+1==console_area.x1);
    assert(console_area.x2<224);
    assert(lv_color_eq(lv_obj_get_style_text_color(label(lv_screen_active(),"添加设备时输入"),0),lv_color_hex(XZ_UI_INK)));
    assert(lv_color_eq(lv_obj_get_style_text_color(label(lv_screen_active(),"绑定"),0),lv_color_hex(XZ_UI_INK)));
    lv_obj_t *retry=label(lv_screen_active(),"重新获取");assert(retry);
    lv_obj_t *menu=label(lv_screen_active(),"菜单");assert(menu);
    assert(previous.y2<lv_obj_get_y(lv_obj_get_parent(retry)));
    assert(lv_obj_get_y(lv_obj_get_parent(retry))==274);
    assert(lv_obj_get_y(lv_obj_get_parent(menu))==274);
}
static void settle(void){lv_tick_inc(300);lv_timer_handler();lv_obj_update_layout(lv_screen_active());lv_refr_now(NULL);bounds(lv_screen_active());assert(lv_mem_test()==LV_RESULT_OK);}
void native_tx_reset(void);
void native_tx_complete(int16_t *stereo,size_t samples);
static void check_male_mouth(void) {
    xz_view_t v={.phase=XZ_SPEAK,.avatar=1,.output_active=true,.revision=1};
    uint16_t inside=lv_color_to_u16(lv_color_hex(0x56251F));
    uint16_t tongue=lv_color_to_u16(lv_color_hex(0xDE785E));
    ui_xiaozhi_open(&v,&face_art);step(40);lv_refr_now(NULL);
    assert_face(COMPANION_TALK);
    assert(mouth_pixels[7][10]==inside&&mouth_pixels[13][10]==tongue);
    unsigned dark=0;for(unsigned y=0;y<17;y++)for(unsigned x=0;x<21;x++)dark+=mouth_pixels[y][x]==inside;
    assert(dark>100); // A visible opening, rather than one changed center pixel.
    step(180);lv_refr_now(NULL);assert_face(COMPANION_IDLE);assert(mouth_pixels[7][10]!=inside);
    v.output_active=false;v.revision++;ui_xiaozhi_update(&v,&face_art);step(180);lv_refr_now(NULL);
    assert(mouth_pixels[7][10]!=inside);ui_xiaozhi_close();
    v.avatar=0;v.output_active=true;v.revision++;
    ui_xiaozhi_open(&v,&face_art);step(40);lv_refr_now(NULL);
    assert_face(COMPANION_TALK);assert(mouth_pixels[7][10]!=inside);ui_xiaozhi_close();
    puts("Male mouth: visible opening, alternating close, silent/female exclusion PASS");
}
static void check_speaker_sync(unsigned rate) {
    int16_t pcm[240];
    native_tx_reset();
    xz_view_t v={.phase=XZ_SPEAK,.revision=1}; strcpy(v.detail,"正在回答你");
    ui_xiaozhi_open(&v,&face_art); step(0);
    const unsigned block_us=120000000u/rate;
    unsigned first_sound_us=120u*block_us, last_sound_us=145u*block_us;
    unsigned opened=0,closed=0,last_ui=0,next_ui=25000;
    for(unsigned block=1;block<=175;block++) {
        unsigned end=block*block_us;
        bool voice=block>120 && block<=145;
        for(unsigned i=0;i<240;i++)pcm[i]=voice?((i&2)?1200:-1100):0;
        native_tx_complete(pcm,240); // Hardware event after, not before, transfer.
        if(end<next_ui)continue;
        v.output_active=xiaozhi_output_active();
        ui_xiaozhi_update(&v,&face_art);step((end-last_ui)/1000);
        last_ui=end;next_ui+=25000;
        bool mouth=lv_image_get_src(find_face(lv_screen_active()))==&face_frames[COMPANION_TALK];
        if(end<=first_sound_us)assert(!mouth);
        if(mouth&&!opened)opened=end;
        if(opened && end>last_sound_us && !v.output_active && !closed){assert(!mouth);closed=end;}
    }
    assert(opened>=first_sound_us && opened-first_sound_us<=25000+block_us);
    assert(closed>=last_sound_us && closed-last_sound_us<=25000+8*block_us);
    printf("Actual TX callback to native portrait at %u Hz: onset %+d us, silence close %+d us, zero early frames.\n",
           rate,(int)(opened-first_sound_us),(int)(closed-last_sound_us));
    ui_xiaozhi_close();
}
static void check_device_settings(void) {
 unsigned value=0;xz_view_t v={.phase=XZ_STANDBY,.volume=95,.revision=1};
 ui_xiaozhi_set_device_settings(100,5);ui_xiaozhi_open(&v,&face_art);
 ui_xiaozhi_key(UI_PLAY_BACK,&value);
 for(unsigned i=0;i<4;i++)ui_xiaozhi_key(UI_PLAY_DOWN,&value);
 ui_xiaozhi_key(UI_PLAY_OK,&value);settle();assert(has_text(lv_screen_active(),"屏幕亮度")&&has_text(lv_screen_active(),"100"));
 assert(ui_xiaozhi_key(UI_PLAY_DOWN,&value)==UI_XZ_BRIGHTNESS_PREVIEW&&value==95);
 assert(ui_xiaozhi_key(UI_PLAY_BACK,&value)==UI_XZ_BRIGHTNESS_CANCEL);
 ui_xiaozhi_key(UI_PLAY_OK,&value);settle();assert(has_text(lv_screen_active(),"100"));
 for(unsigned i=0;i<25;i++)ui_xiaozhi_key(UI_PLAY_DOWN,&value);
 assert(ui_xiaozhi_key(UI_PLAY_OK,&value)==UI_XZ_BRIGHTNESS&&value==10);
 ui_xiaozhi_setting_result(false);settle();assert(has_text(lv_screen_active(),"保存失败，请重试"));
 ui_xiaozhi_set_device_settings(10,5);ui_xiaozhi_setting_result(true);settle();
 ui_xiaozhi_key(UI_PLAY_DOWN,&value);settle();assert(has_text(lv_screen_active(),"休眠时间"));
 ui_xiaozhi_key(UI_PLAY_OK,&value);settle();assert(has_text(lv_screen_active(),"5"));
 ui_xiaozhi_key(UI_PLAY_UP,&value);assert(ui_xiaozhi_key(UI_PLAY_OK,&value)==UI_XZ_SLEEP&&value==10);
 ui_xiaozhi_setting_result(false);settle();assert(has_text(lv_screen_active(),"保存失败，请重试"));
 ui_xiaozhi_key(UI_PLAY_BACK,&value);ui_xiaozhi_key(UI_PLAY_OK,&value);settle();assert(has_text(lv_screen_active(),"5"));
 for(unsigned i=0;i<3;i++)ui_xiaozhi_key(UI_PLAY_DOWN,&value);
 settle();assert(has_text(lv_screen_active(),"不自动休眠"));
 assert(ui_xiaozhi_key(UI_PLAY_OK,&value)==UI_XZ_SLEEP&&value==0);
 ui_xiaozhi_set_device_settings(10,0);ui_xiaozhi_setting_result(true);
 ui_xiaozhi_key(UI_PLAY_DOWN,&value);settle();assert(has_text(lv_screen_active(),"返回"));
 ui_xiaozhi_key(UI_PLAY_OK,&value);settle();assert(has_text(lv_screen_active(),"对话"));
 ui_xiaozhi_close();
}
int main(void){
    lv_init();lv_display_t *d=lv_display_create(240,320);lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d,buffer,NULL,sizeof buffer,LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(d,flush);
    for(unsigned f=0;f<COMPANION_FACE_COUNT;f++)face_frames[f]=(lv_image_dsc_t){
        .header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_RGB565A8,.w=144,.h=144,.stride=288},
        .data_size=sizeof face_pixels,.data=face_pixels};
    lv_obj_t *home=lv_screen_active();xz_view_t v={.phase=XZ_READY,.avatar=0,.revision=1};strcpy(v.detail,"你好，今天想聊些什么？");
    check_speaker_sync(16000);check_speaker_sync(24000);
    check_male_mouth();
    check_device_settings();
    unsigned avatar=0;lv_mem_monitor_t first,last;
    ui_xiaozhi_set_battery(12,1); v.phase=XZ_BOOT;
    ui_xiaozhi_open(&v,&face_art);settle();assert(find_face(lv_screen_active())&&!label(lv_screen_active(),"配置网络"));assert_gauge(12,1);
    v.phase=XZ_WELCOME;v.revision++;ui_xiaozhi_update(&v,&face_art);
    assert(!ui_xiaozhi_welcome_drawn()); settle();assert(ui_xiaozhi_welcome_drawn());
    lv_obj_t *welcome_cta=label(lv_screen_active(),"配置网络"); assert(welcome_cta);
    assert(lv_obj_get_y(lv_obj_get_parent(welcome_cta))>lv_obj_get_y(find_face(lv_screen_active()))+144);
    assert(ui_xiaozhi_key(UI_PLAY_DOWN,&avatar)==UI_XZ_NONE && ui_xiaozhi_key(UI_PLAY_BACK,&avatar)==UI_XZ_NONE);
    assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_BEGIN_SETUP && !label(lv_screen_active(),"菜单"));
    ui_xiaozhi_set_battery(30,2);settle();assert_gauge(30,2);assert(!label(lv_screen_active(),"30%"));
    ui_xiaozhi_set_battery(-1,0);settle();assert_gauge(-1,0);ui_xiaozhi_close();
    v.phase=XZ_SETUP;strcpy(v.detail,"");strcpy(v.hotspot,"AI-PASSPORT-ABCD");
    strcpy(v.password,"12345678"); // A stale view must never reintroduce a password prompt.
    ui_xiaozhi_open(&v,NULL);settle();
    lv_obj_t *prompt=label(lv_screen_active(),"手机连接热点");
    lv_obj_t *ssid=label(lv_screen_active(),v.hotspot);
    lv_obj_t *address=label(lv_screen_active(),"192.168.4.1");
    lv_obj_t *wifi=label(lv_screen_active(),"选择或添加 2.4G Wi-Fi");
    assert(prompt&&ssid&&address&&wifi&&!has_text(lv_screen_active(),"等待配网"));
    assert(lv_obj_get_y(prompt)<70);
    assert(lv_obj_get_style_text_font(ssid,0)==&ui_font_title);
    assert(lv_obj_get_style_text_font(address,0)==&lv_font_montserrat_24);
    ui_xiaozhi_set_battery(87,5);settle();assert_gauge(87,5);
    assert(lv_obj_get_y(prompt)<lv_obj_get_y(ssid)&&lv_obj_get_y(ssid)<lv_obj_get_y(address)&&lv_obj_get_y(address)<lv_obj_get_y(wifi));
    assert(!has_text(lv_screen_active(),"密码 12345678"));
    ui_xiaozhi_close();v.phase=XZ_READY;strcpy(v.detail,"你好，今天想聊些什么？");
    v.phase=XZ_BIND;strcpy(v.code,"123456");ui_xiaozhi_open(&v,NULL);settle();
    assert(has_text(lv_screen_active(),"联网成功"));
    assert(has_text(lv_screen_active(),"请前往 xiaozhi.me"));
    assert(has_text(lv_screen_active(),"登录并进入"));
    assert(has_text(lv_screen_active(),"控制台"));
    assert(has_text(lv_screen_active(),"添加设备时输入"));
    assert(has_text(lv_screen_active(),"123456"));
    assert(has_text(lv_screen_active(),"绑定"));
    assert(!has_text(lv_screen_active(),"绑定设备，配置角色"));
    assert(!has_text(lv_screen_active(),"登录并绑定设备"));
    assert(!has_text(lv_screen_active(),"绑定时需要输入："));
    assert(!has_text(lv_screen_active(),"绑定旅伴"));
    assert_binding_layout();
    assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_RETRY);
    ui_xiaozhi_key(UI_PLAY_DOWN,&avatar);ui_xiaozhi_key(UI_PLAY_OK,&avatar);settle();
    assert(has_text(lv_screen_active(),"屏幕亮度")&&!has_text(lv_screen_active(),"返回玩法"));
    ui_xiaozhi_key(UI_PLAY_BACK,&avatar);settle();
    assert_binding_layout();
    assert(ui_xiaozhi_key(UI_PLAY_BACK,&avatar)==UI_XZ_NONE);
    ui_xiaozhi_close();v.phase=XZ_CLOUD;v.revision++;
    strcpy(v.detail,"正在获取设备绑定信息");
    ui_xiaozhi_open(&v,NULL);settle();
    assert(has_text(lv_screen_active(),"联网成功"));
    assert(has_text(lv_screen_active(),v.detail));
    ui_xiaozhi_close();v.phase=XZ_ERROR;v.stage=XZ_STAGE_BIND;v.wifi_saved=true;v.revision++;
    strcpy(v.detail,"绑定超时，请重新获取验证码");
    ui_xiaozhi_open(&v,NULL);settle();
    assert(has_text(lv_screen_active(),"设备绑定未完成"));
    assert(!has_text(lv_screen_active(),"稍作休息"));
    assert(has_text(lv_screen_active(),v.detail));
    assert(has_text(lv_screen_active(),"123456"));
    assert(has_text(lv_screen_active(),"重新获取"));
    assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_RETRY);
    ui_xiaozhi_close();v.code[0]=0;v.stage=XZ_STAGE_VOICE;v.revision++;
    strcpy(v.detail,"连接失败，请重试");
    ui_xiaozhi_open(&v,NULL);settle();
    assert(has_text(lv_screen_active(),"语音连接未完成"));
    assert(has_text(lv_screen_active(),"Wi-Fi 已保存"));
    assert(has_text(lv_screen_active(),"重试"));
    assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_RETRY);
    ui_xiaozhi_close();
    const struct { xz_stage_t stage; const char *title; bool saved; } failures[] = {
        {XZ_STAGE_WIFI,"Wi-Fi 未连接",false},
        {XZ_STAGE_CLOUD,"小智服务未连接",true},
        {XZ_STAGE_BIND,"设备绑定未完成",true},
        {XZ_STAGE_VOICE,"语音连接未完成",true},
        {XZ_STAGE_DEVICE,"设备暂不可用",true},
    };
    for(unsigned i=0;i<sizeof failures/sizeof failures[0];i++) {
        v.phase=XZ_ERROR;v.stage=failures[i].stage;v.wifi_saved=failures[i].saved;v.revision++;
        strcpy(v.code,"123456");
        strcpy(v.detail,"连接诊断：网络请求未完成\n请稍后重试，已保存的网络无需重新填写。\n原因：安全连接检查失败，详细错误位于最后一行。\nTLS: -0x2700");
        ui_xiaozhi_open(&v,NULL);settle();
        assert(has_text(lv_screen_active(),failures[i].title));
        assert(has_text(lv_screen_active(),"Wi-Fi 已保存")==failures[i].saved);
        lv_obj_t *full_reason=label(lv_screen_active(),v.detail);assert(full_reason);
        assert(lv_label_get_long_mode(full_reason)==LV_LABEL_LONG_SCROLL);
        assert(has_text(lv_screen_active(),"123456")== (failures[i].stage==XZ_STAGE_BIND));
        assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_RETRY);
        // Retry reuses saved Wi-Fi; only the separate menu choice requests provisioning.
        ui_xiaozhi_key(UI_PLAY_DOWN,&avatar);ui_xiaozhi_key(UI_PLAY_OK,&avatar);settle();
        assert(!has_text(lv_screen_active(),"重新连接")&&has_text(lv_screen_active(),"重新配网"));
        assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_WIFI);
        step(12000);bounds(lv_screen_active());ui_xiaozhi_close();
    }
    v.phase=XZ_OFF;v.stage=XZ_STAGE_VOICE;v.wifi_saved=true;v.code[0]=0;v.revision++;
    strcpy(v.detail,"对话已结束，确定后重新连接");
    ui_xiaozhi_open(&v,NULL);settle();
    assert(has_text(lv_screen_active(),"对话已结束")&&has_text(lv_screen_active(),v.detail));
    assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_RETRY);
    v.phase=XZ_RADIO;v.revision++;strcpy(v.detail,"深圳飞扬971");ui_xiaozhi_update(&v,&face_art);settle();
    assert(has_text(lv_screen_active(),"网络电台"));
    assert(has_text(lv_screen_active(),"下键 · 返回"));
    assert(has_text(lv_screen_active(),"上键 · 换台"));
    assert(!find_face(lv_screen_active())); // No portrait/bitmap decoding during MP3 playback.
    v.output_active=true;ui_xiaozhi_update(&v,&face_art);settle();
    assert(has_text(lv_screen_active(),"正在播放"));
    flushed_pixels=0;
    for(unsigned i=0;i<20;i++)step(160);
    assert(flushed_pixels<20*150*75); // Animate only the compact meter, never the whole LCD.
    v.output_active=false;ui_xiaozhi_update(&v,&face_art);settle();
    assert(has_text(lv_screen_active(),"正在播放")); // Silence does not imply a reconnect.
    flushed_pixels=0;for(unsigned i=0;i<20;i++)step(160);assert(!flushed_pixels);
    v.revision++;strcpy(v.detail,"北京音乐广播");ui_xiaozhi_update(&v,&face_art);settle();
    assert(has_text(lv_screen_active(),"北京音乐广播"));bounds(lv_screen_active());
    assert(ui_xiaozhi_key(UI_PLAY_UP,&avatar)==UI_XZ_RADIO_NEXT);
    assert(ui_xiaozhi_key(UI_PLAY_DOWN,&avatar)==UI_XZ_TALK);
    assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_TALK);
    v.phase=XZ_STANDBY;v.revision++;ui_xiaozhi_update(&v,&face_art);settle();
    assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_RESUME);
    ui_xiaozhi_close();v.phase=XZ_LISTEN;v.revision++;
    ui_xiaozhi_open(&v,&face_art);settle();
    ui_xiaozhi_key(UI_PLAY_UP,&avatar); // CHAT's menu control, index2.
    v.phase=XZ_SPEAK;v.revision++;ui_xiaozhi_update(&v,&face_art);settle();
    assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_NONE);
    assert(has_text(lv_screen_active(),"重新配网"));
    v.phase=XZ_LISTEN;v.revision++;ui_xiaozhi_update(&v,&face_art);settle();
    assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_WIFI);
    // A real3->2 control change resets CHAT's invalid selection to retry.
    ui_xiaozhi_key(UI_PLAY_DOWN,&avatar);ui_xiaozhi_key(UI_PLAY_DOWN,&avatar);
    v.phase=XZ_ERROR;v.stage=XZ_STAGE_VOICE;v.revision++;
    ui_xiaozhi_update(&v,&face_art);settle();
    assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_RETRY);
    ui_xiaozhi_close();v.phase=XZ_READY;
    extern void native_official_install(bool);
    for(unsigned cycle=0;cycle<101;cycle++){
        native_official_install(cycle%2);
        ui_xiaozhi_open(&v,&face_art);settle();assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_TALK);
        ui_xiaozhi_key(UI_PLAY_DOWN,&avatar);ui_xiaozhi_key(UI_PLAY_OK,&avatar);settle();
        assert(ui_xiaozhi_key(UI_PLAY_DOWN,&avatar)==UI_XZ_AVATAR&&avatar==1);
        v.avatar=avatar;v.revision++;ui_xiaozhi_update(&v,&face_art);settle();
        assert(ui_xiaozhi_key(UI_PLAY_DOWN,&avatar)==UI_XZ_AVATAR&&avatar==2);
        v.avatar=avatar;v.revision++;ui_xiaozhi_update(&v,NULL);settle();
        assert(has_text(lv_screen_active(),"官方主题"));
        lv_obj_t *icon=find_face(lv_screen_active());assert(icon);
        assert(lv_obj_get_parent(icon)==lv_screen_active());
        lv_area_t icon_bounds;lv_obj_get_coords(icon,&icon_bounds);
        assert(icon_bounds.x1==104 && icon_bounds.y1==122);
        assert(lv_obj_get_width(icon)==32 && lv_obj_get_height(icon)==32);
        assert(lv_image_get_scale_x(icon)==256 && lv_image_get_scale_y(icon)==256);
        assert(ui_xiaozhi_key(UI_PLAY_UP,&avatar)==UI_XZ_AVATAR&&avatar==1);
        v.avatar=avatar;ui_xiaozhi_update(&v,&face_art);settle();
        assert(ui_xiaozhi_key(UI_PLAY_DOWN,&avatar)==UI_XZ_AVATAR&&avatar==2);
        v.avatar=avatar;ui_xiaozhi_update(&v,NULL);settle();ui_xiaozhi_key(UI_PLAY_OK,&avatar);
        ui_xiaozhi_key(UI_PLAY_DOWN,&avatar);ui_xiaozhi_key(UI_PLAY_OK,&avatar);settle();
        ui_xiaozhi_key(UI_PLAY_UP,&avatar);assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_NONE);
        ui_xiaozhi_close();settle();assert(lv_screen_active()==home);
        if(cycle==0)lv_mem_monitor(&first);v.avatar=0;
    }
    lv_mem_monitor(&last);assert(first.used_cnt==last.used_cnt&&first.free_size==last.free_size);
    for(unsigned phase=XZ_WIFI;phase<=XZ_ERROR;phase++){
        v.phase=phase;v.revision++;strcpy(v.detail,"连接失败，请重新填写名称和密码\n192.168.4.1");
        strcpy(v.hotspot,"AI-PASSPORT-ABCD");strcpy(v.password,"12345678");strcpy(v.code,"123456");
        ui_xiaozhi_open(&v,NULL);settle();ui_xiaozhi_close();
    }
    v.phase=XZ_ERROR; v.restart_required=true; v.revision++;
    strcpy(v.detail,"蓝牙暂不可用，请重启设备");
    ui_xiaozhi_open(&v,NULL);settle();
    assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_EXIT);ui_xiaozhi_close();
    v.phase=XZ_READY;v.restart_required=false;v.revision++;
    ui_xiaozhi_open(&v,NULL);
    ui_xiaozhi_key(UI_PLAY_UP,&avatar);ui_xiaozhi_key(UI_PLAY_OK,&avatar);
    ui_xiaozhi_key(UI_PLAY_UP,&avatar);assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_NONE);
    v.phase=XZ_ERROR;v.restart_required=true;v.revision++;
    ui_xiaozhi_update(&v,NULL);settle();assert(has_text(lv_screen_active(),v.detail));
    assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_EXIT);ui_xiaozhi_close();
    v.phase=XZ_READY;v.restart_required=false;v.avatar=0;v.revision++;
    ui_xiaozhi_open(&v,&face_art);step(0);assert_face(COMPANION_IDLE);
    assert((has_text(lv_screen_active(),"FoloToy") && has_text(lv_screen_active(),"小智")));
    step(3399);assert_face(COMPANION_IDLE);step(1);assert_face(COMPANION_BLINK);
    step(140);assert_face(COMPANION_IDLE);
    v.phase=XZ_LISTEN;strcpy(v.detail,"聆听中。。。");v.revision++;ui_xiaozhi_update(&v,&face_art);step(0);assert_face(COMPANION_LISTEN);
    assert(has_text(lv_screen_active(),"聆听中。。。")&&!has_text(lv_screen_active(),"认真聆听"));
    assert(has_text(lv_screen_active(),"结束")&&!has_text(lv_screen_active(),"说完了"));
    assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_TALK);
    v.phase=XZ_THINK;v.revision++;ui_xiaozhi_update(&v,&face_art);step(0);assert_face(COMPANION_THINK);
    v.phase=XZ_SPEAK;v.revision++;ui_xiaozhi_update(&v,&face_art);step(0);assert_face(COMPANION_IDLE);
    step(800);assert_face(COMPANION_IDLE); // TTS control arrived; speaker has not emitted a sample.
    v.output_active=true;ui_xiaozhi_update(&v,&face_art);step(0);assert_face(COMPANION_TALK);
    v.output_active=false;ui_xiaozhi_update(&v,&face_art);step(0);assert_face(COMPANION_IDLE);
    v.output_active=true;ui_xiaozhi_update(&v,&face_art);step(0);assert_face(COMPANION_TALK);
    assert(has_text(lv_screen_active(),"打断")&&!has_text(lv_screen_active(),"结束"));
    assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_INTERRUPT);
    step(180);assert_face(COMPANION_IDLE);step(180);assert_face(COMPANION_TALK);
    v.phase=XZ_READY;v.revision++;ui_xiaozhi_update(&v,&face_art);step(0);assert_face(COMPANION_HAPPY);
    step(650);assert_face(COMPANION_IDLE);
    ui_xiaozhi_close();step(10000);assert(lv_screen_active()==home && lv_mem_test()==LV_RESULT_OK);
    v.phase=XZ_LISTEN;v.natural_interrupt=false;v.volume=95;v.revision++;
    ui_xiaozhi_open(&v,&face_art);
    ui_xiaozhi_key(UI_PLAY_UP,&avatar);ui_xiaozhi_key(UI_PLAY_OK,&avatar);settle();
    const char *menu[]={"重新配网","调整音量","打断方式","智能体设置","屏幕亮度"};
    for(unsigned i=0;i<5;i++) {
        lv_obj_t *row=label(lv_screen_active(),menu[i]);assert(row);
        lv_obj_t *card=lv_obj_get_parent(row);
        assert(lv_obj_get_y(card)>=50 && lv_obj_get_y(card)+lv_obj_get_height(card)<=278);
        if(i) {
            lv_obj_t *previous=lv_obj_get_parent(label(lv_screen_active(),menu[i-1]));
            assert(lv_obj_get_y(previous)+lv_obj_get_height(previous)+6<=lv_obj_get_y(card));
        }
    }
    assert(!has_text(lv_screen_active(),"对话设置")&&!has_text(lv_screen_active(),"返回玩法"));
    ui_xiaozhi_key(UI_PLAY_DOWN,&avatar);ui_xiaozhi_key(UI_PLAY_DOWN,&avatar);
    assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_NONE);settle();
    assert(has_text(lv_screen_active(),"按键打断")&&has_text(lv_screen_active(),"自然打断（测试功能）"));
    ui_xiaozhi_key(UI_PLAY_DOWN,&avatar);
    assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_INTERRUPT_MODE&&avatar==1);
    ui_xiaozhi_setting_result(false);settle();assert(has_text(lv_screen_active(),"保存失败，请重试"));
    // Failed persistence stays on the editor with the old model; retry applies once.
    assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_INTERRUPT_MODE&&avatar==1);
    v.natural_interrupt=true;v.revision++;ui_xiaozhi_update(&v,&face_art);ui_xiaozhi_setting_result(true);settle();
    assert(has_text(lv_screen_active(),"调整音量")&&!has_text(lv_screen_active(),"保存失败，请重试"));
    ui_xiaozhi_key(UI_PLAY_UP,&avatar);ui_xiaozhi_key(UI_PLAY_OK,&avatar);settle();
    assert(has_text(lv_screen_active(),"95"));
    ui_xiaozhi_key(UI_PLAY_DOWN,&avatar);ui_xiaozhi_key(UI_PLAY_BACK,&avatar);settle();
    ui_xiaozhi_key(UI_PLAY_OK,&avatar);settle();
    assert(has_text(lv_screen_active(),"95")); // Cancel discards the edited value.
    for(unsigned i=0;i<25;i++)ui_xiaozhi_key(UI_PLAY_DOWN,&avatar);
    settle();assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_VOLUME&&avatar==0);
    ui_xiaozhi_setting_result(false);settle();assert(has_text(lv_screen_active(),"保存失败，请重试"));
    assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_VOLUME&&avatar==0);
    v.volume=0;ui_xiaozhi_update(&v,&face_art);ui_xiaozhi_setting_result(true);settle();
    ui_xiaozhi_key(UI_PLAY_OK,&avatar);settle();assert(has_text(lv_screen_active(),"0"));
    for(unsigned i=0;i<25;i++)ui_xiaozhi_key(UI_PLAY_UP,&avatar);
    settle();assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_VOLUME&&avatar==100);
    v.volume=100;ui_xiaozhi_update(&v,&face_art);ui_xiaozhi_setting_result(true);settle();
    ui_xiaozhi_key(UI_PLAY_DOWN,&avatar);ui_xiaozhi_key(UI_PLAY_DOWN,&avatar);ui_xiaozhi_key(UI_PLAY_OK,&avatar);settle();
    assert(has_text(lv_screen_active(),"前往xiaozhi.me，\n进入控制台设置")&&!has_text(lv_screen_active(),"名字、角色和音色"));assert_gauge(87,5);
    ui_xiaozhi_key(UI_PLAY_BACK,&avatar);
    for(unsigned n=0;n<3;n++)ui_xiaozhi_key(UI_PLAY_DOWN,&avatar);
    assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_NONE); // Return is conversation, not Plays.
    v.phase=XZ_SPEAK;v.revision++;ui_xiaozhi_update(&v,&face_art);settle();
    assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_INTERRUPT);
    // Long-confirm opens the same menu; all background phase changes keep the editor stable.
    assert(ui_xiaozhi_key(UI_PLAY_BACK,&avatar)==UI_XZ_NONE);
    ui_xiaozhi_key(UI_PLAY_DOWN,&avatar);ui_xiaozhi_key(UI_PLAY_OK,&avatar);
    v.phase=XZ_LISTEN;v.volume=60;v.revision++;ui_xiaozhi_update(&v,&face_art);settle();
    assert(has_text(lv_screen_active(),"100"));
    ui_xiaozhi_key(UI_PLAY_BACK,&avatar);ui_xiaozhi_key(UI_PLAY_BACK,&avatar);
    ui_xiaozhi_close();settle();assert(lv_screen_active()==home&&lv_mem_test()==LV_RESULT_OK);
    v.phase=XZ_LISTEN;v.revision++;ui_xiaozhi_open(&v,&face_art);settle();
    lv_obj_t *portrait=find_face(lv_screen_active());assert(portrait);
    v.phase=XZ_FAREWELL;v.revision++;strcpy(v.detail,"拜拜");
    ui_xiaozhi_update(&v,&face_art);settle();
    assert(find_face(lv_screen_active())==portrait&&has_text(lv_screen_active(),"拜拜"));
    v.phase=XZ_STANDBY;v.revision++;strcpy(v.detail,"按对话开始聊天");
    ui_xiaozhi_update(&v,&face_art);settle();
    assert(find_face(lv_screen_active())==portrait&&has_text(lv_screen_active(),"对话"));
    assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_RESUME);
    ui_xiaozhi_key(UI_PLAY_BACK,&avatar);settle();
    lv_obj_t *wifi_row=label(lv_screen_active(),"重新配网");flushed_pixels=0;
    ui_xiaozhi_key(UI_PLAY_DOWN,&avatar);settle();
    assert(label(lv_screen_active(),"重新配网")==wifi_row);
    assert(flushed_pixels>0&&flushed_pixels<240*160);
    printf("Menu focus update flushed %u of 76800 pixels; portrait retained across end/start.\n",flushed_pixels);
    ui_xiaozhi_close();settle();
    // Exercise every edit/return path repeatedly, including failed saves and
    // background phase changes. Compare actual LVGL heap after warm-up.
    v.phase=XZ_LISTEN;v.restart_required=false;v.volume=95;v.natural_interrupt=false;
    for(unsigned cycle=0;cycle<501;cycle++) {
        strcpy(v.detail,cycle&1?"哦，呀！嗯嗯，嗨～\n呦、诶、噢、喔、嘞、嘟。":"欸，啰嗦一点也没关系。\n珺、玥、祎、龟。");
        ui_xiaozhi_open(&v,&face_art);
        settle(); // Actually rasterize the complete rare-character reply.
        ui_xiaozhi_key(UI_PLAY_BACK,&avatar); // Menu.
        ui_xiaozhi_key(UI_PLAY_DOWN,&avatar);ui_xiaozhi_key(UI_PLAY_OK,&avatar);
        ui_xiaozhi_key(UI_PLAY_DOWN,&avatar);
        assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_VOLUME&&avatar==90);
        ui_xiaozhi_setting_result(false);settle();
        ui_xiaozhi_key(UI_PLAY_BACK,&avatar);ui_xiaozhi_key(UI_PLAY_DOWN,&avatar);
        ui_xiaozhi_key(UI_PLAY_OK,&avatar);ui_xiaozhi_key(UI_PLAY_DOWN,&avatar);
        assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_INTERRUPT_MODE&&avatar==1);
        ui_xiaozhi_setting_result(false);settle();
        ui_xiaozhi_key(UI_PLAY_BACK,&avatar);ui_xiaozhi_key(UI_PLAY_DOWN,&avatar);
        ui_xiaozhi_key(UI_PLAY_OK,&avatar);settle();
        ui_xiaozhi_key(UI_PLAY_OK,&avatar);ui_xiaozhi_key(UI_PLAY_DOWN,&avatar);
        assert(ui_xiaozhi_key(UI_PLAY_OK,&avatar)==UI_XZ_NONE);
        ui_xiaozhi_close();settle();
        if(cycle==0)lv_mem_monitor(&first);
    }
    lv_mem_monitor(&last);assert(first.used_cnt==last.used_cnt&&first.free_size==last.free_size);
    printf("500 settings/failure/return cycles: no LVGL allocation leak; peak=%zu/%zu bytes\n",last.max_used,last.total_size);
    printf("FoloToy face animation, all six expressions, blink and talk timing, cancellation PASS.\n");
    printf("XiaoZhi UI: all states in bounds, clear return, avatar switch, 100 no-leak cycles. peak=%zu/%zu\n",last.max_used,last.total_size);
}
