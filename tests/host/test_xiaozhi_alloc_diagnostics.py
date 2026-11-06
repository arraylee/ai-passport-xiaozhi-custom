"""Execute production allocation hook/registration/reporting without heap work in hook."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_xz_channel_memory import function
ROOT=Path(__file__).resolve().parents[2]

class AllocationDiagnosticsTest(unittest.TestCase):
 def test_callback_is_numeric_nonallocating_running_only_and_registration_checked(self):
  source=(ROOT/'components/platform/xiaozhi/xiaozhi.c').read_text()
  if 'static void allocation_failed(' not in source:
   self.fail('production must expose a running-only numeric allocation failure hook')
  helpers='\n'.join(function(source,name) for name in ['allocation_failed','allocation_failure_metric','allocation_failure_init'])
  declarations=source[source.index('static atomic_uint allocation_fail_count'):source.index('static void allocation_failed(')]
  code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#define ESP_OK 0
static atomic_bool running;
static const char *TAG="fixture";
static unsigned registrations,logs,warnings;
static int register_result;
static void (*registered)(size_t,uint32_t,const char *);
static int heap_caps_register_failed_alloc_callback(void (*cb)(size_t,uint32_t,const char *)){
 registrations++;if(!register_result)registered=cb;return register_result;
}
static char line[256];
static void record(const char *tag,const char *fmt,...){assert(tag==TAG);logs++;va_list ap;va_start(ap,fmt);vsnprintf(line,sizeof line,fmt,ap);va_end(ap);}
#define ESP_LOGI(...) record(__VA_ARGS__)
#define ESP_LOGW(...) do{warnings++;record(__VA_ARGS__);}while(0)
'''+declarations+helpers+r'''
int main(void){
 register_result=-1;allocation_failure_init();assert(registrations==1&&warnings==1&&!registered);
 register_result=0;allocation_failure_init();assert(registrations==2&&registered==allocation_failed);
 allocation_failure_init();assert(registrations==2); // Do not replace an installed hook on repeated init.
 unsigned before=logs;registered(999,3,"SECRET_SENTINEL");allocation_failure_metric();assert(logs==before);
 running=true;registered(24524,1,"SECRET_SENTINEL");assert(logs==before); // Hook must never log.
 allocation_failure_metric();assert(logs==before+1&&strstr(line,"count=1")&&strstr(line,"size=24524")&&strstr(line,"caps=1")&&!strstr(line,"SECRET"));
 allocation_failure_metric();assert(logs==before+1); // Same snapshot logged once.
 for(unsigned i=0;i<100;i++)registered(i+1,4,"SECRET_SENTINEL");
 allocation_failure_metric();assert(logs==before+2&&strstr(line,"count=101")&&strstr(line,"size=100")&&strstr(line,"caps=4"));
 running=false;registered(65536,99,"SECRET_SENTINEL");allocation_failure_metric();assert(logs==before+2);
 running=true;registered(16,8,NULL);allocation_failure_metric();assert(strstr(line,"count=102")&&strstr(line,"size=16"));
 puts("numeric allocation diagnostics registration, running gate, count and one-shot reporting PASS");
}
'''
  with tempfile.TemporaryDirectory() as folder:
   c,exe=Path(folder)/'diag.c',Path(folder)/'diag';c.write_text(code)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(c),'-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True,timeout=5)
  for name in ['status','codec_arena_metric']:
   self.assertIn('allocation_failure_metric();',function(source,name))
  self.assertIn('allocation_failure_init();',function(source,'xiaozhi_init'))

if __name__=='__main__':unittest.main()
