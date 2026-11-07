"""Replay MQTT/UDP arrival order through the production worker's dispatch."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BOUNDARY = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "cJSON.h"
#define XZ_JSON_MAX 4096
#define XZ_PACKET_MAX 1275
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGW(...) ((void)0)
enum {XZ_THINK, XZ_SPEAK, XZ_LISTEN, XZ_READY, XZ_ERROR, XZ_OFF};
static struct { int phase; unsigned emotion; char detail[160]; } working;
static unsigned mcp_seen;
static void handle_mcp(void *c, const cJSON *j){(void)c;assert(j);mcp_seen++;}
static int xz_emotion_index(const char *s){return !strcmp(s,"happy")?1:-1;}
static bool xz_theme_reboot_requested(void){return false;}
static void esp_restart(void){assert(0);}

static atomic_bool cancel,requested_interrupt,requested_talk;
enum {SESSION_CLOSED,SESSION_PARK,SESSION_RENEW,SESSION_QUIET_BYE,SESSION_INTENT_BYE,SESSION_AUDIO_LOST};
static struct {unsigned quiet_samples;} window,*quiet=&window;
static int outcome;static atomic_bool requested_end;
static bool pending_preroll;
static bool command(void*c,const char*t,const char*s){(void)c;assert(!strcmp(t,"abort")&&!s);return true;}
static void discard_reply_audio(void*c,void*p){(void)c;(void)p;}
static const char *messages[8];
static unsigned head, count, udp_reads, waits;
static uint64_t now, last_action, last_audio;
static uint32_t tts_received,tts_valid,tts_written;
static void *encoder;
static bool audio_owned;
static void codec_metric(unsigned a,unsigned b,int c){(void)a;(void)b;(void)c;}
static void esp_opus_enc_close(void *p){assert(p==(void *)1);encoder=NULL;}
static void release_audio(void *p){(void)p;assert(audio_owned);audio_owned=false;}
static struct {void (*release_audio)(void*);void *user,*audio;} config={release_audio,NULL,(void*)1};
static int hal_audio_set_echo_capture(void *a,bool on){assert(a&&!on);return 0;}
static bool tts_stop, tts_text, parse_fail, ignore_aborted_tts;
static void *decoder;
#define MSG_DONTWAIT 1
enum {CHANNEL_CONTROL_PARSE=99};
const char *channel_error_text(void *c){(void)c;return "control parse failed";}
static void *parse_malloc(size_t n){return parse_fail?NULL:malloc(n);}
static uint64_t ms(void) { return now; }
static void status(int phase, const char *detail) { (void)detail; working.phase=phase; }
static void vTaskDelay(unsigned delay) { assert(delay==10); waits++; }
typedef struct {size_t total;char data[];} channel_message_t;
static channel_message_t *channel_take(void *ignored,unsigned timeout) {
    (void)ignored;(void)timeout;
    if(head==count)return NULL;
    size_t n=strlen(messages[head]);channel_message_t *message=malloc(sizeof *message+n+1);assert(message);
    message->total=n;memcpy(message->data,messages[head++],n+1);return message;
}
static int recv(int fd, void *data, size_t cap, int flags) {
    (void)fd;(void)data;(void)cap;(void)flags; udp_reads++; return 1;
}
'''
CASES = r'''
static void reset(void) { outcome=SESSION_CLOSED;requested_end=false;head=count=udp_reads=waits=0; now=last_action=0; working.phase=XZ_THINK; cancel=tts_text=parse_fail=ignore_aborted_tts=false; }
int main(void) {
    cJSON_Hooks hooks={.malloc_fn=parse_malloc,.free_fn=free};cJSON_InitHooks(&hooks);
    reset(); messages[count++]="{\"type\":\"stt\",\"text\":\"Hello\"}";
    dispatch(); assert(working.phase==XZ_THINK && !udp_reads && waits==1);
    messages[count++]="{\"type\":\"tts\",\"state\":\"start\"}";
    dispatch(); assert(working.phase==XZ_SPEAK && udp_reads==1);
    reset(); messages[count++]="{\"type\":\"stt\",\"text\":\"Hello\"}";
    messages[count++]="{\"type\":\"tts\",\"state\":\"start\"}";
    messages[count++]="{\"type\":\"tts\",\"state\":\"stop\"}";
    dispatch(); assert(head==3 && working.phase==XZ_SPEAK && tts_stop && udp_reads==1);
    reset();working.phase=XZ_LISTEN;encoder=(void *)1;audio_owned=true;
    messages[count++]="{\"type\":\"tts\",\"state\":\"start\"}";
    dispatch();assert(working.phase==XZ_SPEAK&&!encoder&&!audio_owned&&udp_reads==1);
    reset();working.phase=XZ_LISTEN;ignore_aborted_tts=true;encoder=(void*)1;audio_owned=true;
    messages[count++]="{\"type\":\"tts\",\"state\":\"start\"}";
    messages[count++]="{\"type\":\"tts\",\"state\":\"stop\"}";
    dispatch();assert(working.phase==XZ_LISTEN&&encoder&&audio_owned&&ignore_aborted_tts);
    messages[count++]="{\"type\":\"stt\",\"text\":\"new utterance\"}";
    messages[count++]="{\"type\":\"tts\",\"state\":\"start\"}";
    dispatch();assert(working.phase==XZ_SPEAK&&!encoder&&!audio_owned&&!ignore_aborted_tts);
    reset(); now=45001; dispatch(); assert(working.phase==XZ_ERROR && !udp_reads);
    reset(); messages[count++]="{\"type\":\"goodbye\",\"session_id\":\"previous\"}";
    dispatch();assert(outcome==SESSION_CLOSED);
    reset(); messages[count++]="{\"type\":\"goodbye\",\"session_id\":\"current\"}";
    dispatch();assert(outcome==SESSION_RENEW);
    reset(); messages[count++]="{\"type\":\"goodbye\"}";
    dispatch(); assert(outcome==SESSION_RENEW && !udp_reads);
    reset();working.phase=XZ_LISTEN;encoder=(void*)1;audio_owned=true;
    messages[count++]="{\"type\":\"stt\",\"text\":\"小智，拜拜！\"}";
    dispatch();assert(outcome==SESSION_INTENT_BYE && !udp_reads);
    reset();working.phase=XZ_LISTEN;encoder=(void*)1;audio_owned=true;
    messages[count++]="{\"type\":\"tts\",\"state\":\"start\"}";
    messages[count++]="{\"type\":\"stt\",\"text\":\"小智，拜拜！\"}";
    dispatch();assert(outcome==SESSION_INTENT_BYE && !udp_reads && !encoder && !audio_owned);
    reset();
    assert(!xz_is_farewell("不要说拜拜") && !xz_is_farewell("拜拜是什么意思"));
    reset(); messages[count++]="{\"type\":\"mcp\"}"; messages[count++]="{\"type\":\"llm\",\"emotion\":\"happy\"}";
    dispatch(); assert(mcp_seen==1 && working.emotion==1 && working.phase==XZ_THINK);
    reset(); cancel=true; dispatch(); assert(!udp_reads);
    reset();messages[count++]="{\"type\":\"stt\",\"text\":\"valid message\"}";parse_fail=true;
    dispatch();assert(working.phase==XZ_ERROR && !udp_reads && head==1);
    reset();messages[count++]="{";dispatch();assert(working.phase==XZ_ERROR && !udp_reads);
}
'''


class AudioOrderingTest(unittest.TestCase):
    def test_control_first_and_preserve_early_audio(self):
        source = (ROOT/'components/platform/xiaozhi/xiaozhi.c').read_text()
        control = source[source.index('    for (unsigned control ='):source.index('    if (working.phase == XZ_READY && atomic_exchange(')]
        marker = '    } else if (working.phase == XZ_THINK) {'
        audio = '    if (working.phase == XZ_THINK) {' + source.split(marker, 1)[1].split('      uint32_t sequence;', 1)[0]
        function = ('static void dispatch(void) {\n'
                    ' struct { int messages, udp; char session[16]; atomic_bool failed; atomic_int error; } channel={.session="current"}, *c=&channel;\n'
                    ' unsigned char packet[XZ_PACKET_MAX+16];\n'
                    ' while (!atomic_load(&cancel)) {\n' + control + audio +
                    ' (void)got; } break; } }\n')
        idf = Path(os.environ.get('IDF_PATH', str(Path.home() / 'esp/esp-idf-v5.5.3')))
        json = idf/'components/json/cJSON'
        with tempfile.TemporaryDirectory() as folder:
            c, exe = Path(folder)/'order.c', Path(folder)/'order'
            component=ROOT/'components/platform/xiaozhi'
            c.write_text(BOUNDARY + '\n#include "xz_protocol.h"\n' + function + CASES)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', '-I', str(json), '-I', str(component), str(c), str(component/'xz_protocol.c'), str(json/'cJSON.c'),
                            '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
