"""Every actual cJSON allocation boundary must fail without publishing partial controls."""
from pathlib import Path
import os,subprocess,tempfile,unittest
from test_xz_channel_memory import function
ROOT=Path(__file__).resolve().parents[2]
BOUNDARY=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
typedef struct {char session[128];} channel_t;
static unsigned allocations,fail_at,live,publishes;
static bool publish_fail;
static char sent[1024];
static void *allocate(size_t n){++allocations;if(allocations==fail_at)return NULL;void *p=malloc(n);if(p)live++;return p;}
static void release(void *p){if(p){assert(live);live--;free(p);}}
static bool send_text(channel_t *c,const char *s){(void)c;publishes++;snprintf(sent,sizeof sent,"%s",s);return !publish_fail;}
#define free release
'''
CASES=r'''
#undef free
static void reset(unsigned failure){assert(!live);allocations=publishes=0;fail_at=failure;sent[0]=0;publish_fail=false;cJSON_Hooks hooks={allocate,release};cJSON_InitHooks(&hooks);}
static void verify(const char *type,const char *state){
 cJSON_InitHooks(NULL);cJSON *j=cJSON_Parse(sent);assert(j);
 assert(!strcmp(cJSON_GetObjectItem(j,"session_id")->valuestring,"SYNTHETIC_SESSION"));
 assert(!strcmp(cJSON_GetObjectItem(j,"type")->valuestring,type));
 if(state)assert(!strcmp(cJSON_GetObjectItem(j,"state")->valuestring,state));else assert(!cJSON_GetObjectItem(j,"state"));
 if(state&&!strcmp(state,"start"))assert(!strcmp(cJSON_GetObjectItem(j,"mode")->valuestring,"auto"));else assert(!cJSON_GetObjectItem(j,"mode"));
 cJSON_Delete(j);
}
int main(void){
 channel_t c={.session="SYNTHETIC_SESSION"};const char *types[]={"listen","listen","abort","goodbye"},*states[]={"start","stop",NULL,NULL};unsigned checked=0;
 for(unsigned kind=0;kind<4;kind++){
  reset(0);assert(command(&c,types[kind],states[kind])&&publishes==1&&!live);unsigned count=allocations;verify(types[kind],states[kind]);
  for(unsigned failure=1;failure<=count;failure++){
   reset(failure);bool ok=command(&c,types[kind],states[kind]);
   if(ok||publishes||live){fprintf(stderr,"partial/OOM command kind=%u failure=%u ok=%u publishes=%u live=%u\n",kind,failure,ok,publishes,live);abort();}
   assert(!sent[0]);checked++;
  }
  reset(count+1);assert(command(&c,types[kind],states[kind])&&publishes==1&&!live);verify(types[kind],states[kind]);
  reset(0);publish_fail=true;assert(!command(&c,types[kind],states[kind])&&publishes==1&&!live);
 }
 cJSON_InitHooks(NULL);printf("Actual cJSON command OOM: %u failure points rejected with no partial publish/leak, auto/start, stop, abort and goodbye PASS\n",checked);
}
'''
class CommandOomTest(unittest.TestCase):
 def test_real_cjson_nth_allocation_failure_never_publishes_partial_command(self):
  source=(ROOT/'components/platform/xiaozhi/xiaozhi.c').read_text();sdk=Path(os.environ.get('IDF_PATH',str(Path.home() / 'esp/esp-idf-v5.5.3')));json=sdk/'components/json/cJSON'
  with tempfile.TemporaryDirectory() as d:
   c,exe=Path(d)/'command.c',Path(d)/'command';c.write_text(BOUNDARY+function(source,'command')+CASES)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined','-I',str(json),str(c),str(json/'cJSON.c'),'-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True,timeout=5)
if __name__=='__main__':unittest.main()
