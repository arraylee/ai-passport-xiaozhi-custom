"""Exercise installed SDK RX ownership with a bounded, modeled C3 heap.

Actual wlanif_input and recv_udp are compiled; only allocation/netif/mailbox
boundaries are modeled. This measures retained bytes, not RF loss or MCU timing.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_xz_channel_memory import function

ROOT = Path(__file__).resolve().parents[2]
FAKES = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned short u16_t;
typedef int err_t;
typedef int esp_netif_recv_ret_t;
typedef int esp_netif_t;
typedef int ip_addr_t;
struct udp_pcb { int unused; };
struct pbuf { uint16_t len,tot_len; void *payload,*l2; unsigned charged; };
struct netbuf { struct pbuf *p,*ptr; ip_addr_t addr; unsigned port; };
struct netconn { struct { struct udp_pcb *udp; } pcb; int recvmbox,recv_avail,recv_bufsize; };
struct netif { esp_netif_t *state; int up; err_t (*input)(struct pbuf *,struct netif *); };
#define LWIP_SO_RCVBUF 1
#define ESP_LWIP 0
#define LWIP_NETBUF_RECVINFO 0
#define LWIP_UNUSED_ARG(x) ((void)(x))
#define LWIP_ASSERT(text,x) assert(x)
#define SYS_ARCH_GET(x,y) ((y)=(x))
#define SYS_ARCH_INC(x,y) ((x)+=(y))
#define NETCONN_MBOX_VALID(c,m) (*(m)!=0)
#define API_EVENT(c,event,n) ((void)0)
#define LWIP_DEBUGF(...) ((void)0)
#define ESP_NETIF_OPTIONAL_RETURN_CODE(x) (x)
#define unlikely(x) (x)
#define ERR_OK 0
#define ERR_MEM -1
#define ESP_OK 0
#define ESP_FAIL -2
#define ESP_ERR_NO_MEM -3
#define PBUF_RAW 0
#define PBUF_RAM 0
#define MEMP_NETBUF 0
static size_t live,peak,limit=9684;
static unsigned l2_live,l2_freed,allocation_failures;
static bool fail_pbuf,fail_netbuf,fail_input,full_mailbox;
static struct netbuf *mailbox[24];
static unsigned queued;
static struct udp_pcb pcb;
static struct netconn conn={.pcb.udp=&pcb,.recvmbox=1,.recv_bufsize=2582};
static bool charge(size_t n) {
 if(n>limit-live){allocation_failures++;return false;}
 live+=n;if(live>peak)peak=live;return true;
}
static void uncharge(size_t n){assert(live>=n);live-=n;}
static void *driver_packet(size_t len) {
 assert(len<=1700);if(!charge(1700))return NULL;
 void *p=malloc(1700);assert(p);memset(p,0x5a,len);l2_live++;return p;
}
static int netif_is_up(struct netif*n){return n->up;}
static void esp_netif_free_rx_buffer(esp_netif_t*n,void*p){(void)n;assert(p&&l2_live);l2_live--;l2_freed++;uncharge(1700);free(p);}
static struct pbuf *pbuf_alloc(int layer,size_t n,int type) {
 (void)layer;(void)type;if(fail_pbuf||!charge(32+n))return NULL;
 struct pbuf *p=calloc(1,sizeof*p);assert(p);p->payload=malloc(n);assert(p->payload);
 p->len=p->tot_len=n;p->charged=32+n;return p;
}
static struct pbuf *esp_pbuf_allocate(esp_netif_t*n,void*payload,size_t len,void*l2) {
 (void)n;if(fail_pbuf||!charge(32))return NULL;
 struct pbuf *p=calloc(1,sizeof*p);assert(p);p->payload=payload;p->l2=l2;p->len=p->tot_len=len;p->charged=32;return p;
}
static void pbuf_free(struct pbuf*p) {
 if(p->l2)esp_netif_free_rx_buffer(NULL,p->l2);else free(p->payload);
 uncharge(p->charged);free(p);
}
static void *memp_malloc(int type){(void)type;if(fail_netbuf||!charge(32))return NULL;void*p=calloc(1,sizeof(struct netbuf));assert(p);return p;}
static void netbuf_delete(struct netbuf*b){pbuf_free(b->p);uncharge(32);free(b);}
static void ip_addr_set(ip_addr_t*a,const ip_addr_t*b){*a=*b;}
static err_t sys_mbox_trypost(int*m,struct netbuf*b){assert(m==&conn.recvmbox);if(full_mailbox||queued==24)return ERR_MEM;mailbox[queued++]=b;return ERR_OK;}
'''
DISPATCH = r'''
static err_t dispatch(struct pbuf*p,struct netif*n) {
 (void)n;if(fail_input)return ERR_MEM;
 for(unsigned i=0;i<p->len;i++)assert(((unsigned char*)p->payload)[i]==0x5a);
 assert(p->len>=42);p->tot_len-=42; /* Ethernet/IPv4/UDP headers. */
 ip_addr_t addr=1;recv_udp(&conn,&pcb,p,&addr,8888);return ERR_OK;
}
'''
CASES = r'''
static void drain(void){for(unsigned i=0;i<queued;i++)netbuf_delete(mailbox[i]);queued=0;conn.recv_avail=0;assert(!live&&!l2_live);}
static void deliver(struct netif*n,unsigned payload){void*p=driver_packet(payload+42);if(p)wlanif_input(n,p,payload+42,p);}
int main(int argc,char**argv){
 assert(argc==2);bool expect_copy=atoi(argv[1])!=0;struct netif n={.up=1,.input=dispatch};
 /* A delayed consumer receives a small-packet burst. With zero-copy, the
  * original 1700B driver allocations exhaust the measured ready heap. */
 for(unsigned i=0;i<24;i++)deliver(&n,180);
 if(expect_copy){assert(!allocation_failures&&queued==14&&conn.recv_avail==2520&&!l2_live&&peak<6500);}
 else {assert(allocation_failures&&queued<=5&&l2_live==queued);}
 printf("copy=%d queued=%u retained=%zu peak=%zu failures=%u\n",expect_copy,queued,live,peak,allocation_failures);
 if(expect_copy){
  /* A full compact UDP queue can coexist with the configured three in-flight
   * 1700B Wi-Fi buffers, including the temporary L3 copy. Six cannot fit in
   * the measured ready heap even though SO_RCVBUF bounds the UDP payload. */
  void *burst[3];
  for(unsigned i=0;i<3;i++){burst[i]=driver_packet(222);assert(burst[i]);}
  for(unsigned i=0;i<3;i++)wlanif_input(&n,burst[i],222,burst[i]);
  assert(!allocation_failures&&!l2_live&&queued==14&&peak<=limit);
 }
 drain();
 if(!expect_copy)return 0;
 /* Two maximum legal encrypted packets fit; excess is dropped and freed.
  * Each accepted pbuf still contains all original bytes, verified above. */
 peak=allocation_failures=0;
 for(unsigned i=0;i<8;i++)deliver(&n,1291);
 assert(queued==2&&conn.recv_avail==2582&&!l2_live&&!allocation_failures&&peak<6500);drain();
 /* Tiny packets are bounded by the mailbox even below the byte quota. */
 for(unsigned i=0;i<100;i++)deliver(&n,17);
 assert(queued==24&&conn.recv_avail==408&&!l2_live);drain();
 for(unsigned test=0;test<6;test++){
  n.up=test!=0;fail_pbuf=test==1;fail_netbuf=test==2;fail_input=test==3;full_mailbox=test==4;conn.recvmbox=test==5?0:1;
  unsigned before=l2_freed;deliver(&n,180);assert(!queued&&!live&&!l2_live&&l2_freed==before+1);
 }
 puts("SDK RX copy/bounds/error cleanup PASS");
}
'''


class VoiceUdpMemoryTest(unittest.TestCase):
    def test_sdk_retains_compact_packets_and_releases_driver_buffers(self):
        sdk = Path(os.environ.get('IDF_PATH', str(Path.home() / 'esp/esp-idf-v5.5.3')))
        wlan = function((sdk / 'components/esp_netif/lwip/netif/wlanif.c').read_text(), 'wlanif_input')
        api = (sdk / 'components/lwip/lwip/src/api/api_msg.c').read_text()
        # The unpreprocessed body contains mutually exclusive opening braces.
        # Keep both branches intact for the real C preprocessor.
        start = api.index('static void\nrecv_udp(')
        end = api.index('\n}\n#endif /* LWIP_UDP */', start) + 2
        udp = api[start:end]
        with tempfile.TemporaryDirectory(prefix='passport-rx-budget-') as folder:
            source = Path(folder) / 'rx.c'
            source.write_text(FAKES + udp + DISPATCH + wlan + CASES)
            for copy in (False, True):
                executable = Path(folder) / ('rx-copy' if copy else 'rx-zero-copy')
                flags = ['-DCONFIG_LWIP_L2_TO_L3_COPY=1'] if copy else []
                subprocess.run(['cc', '-std=c11', '-UNDEBUG', '-Wall', '-Wextra', '-Werror',
                                '-Wno-unused-function', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                                *flags, str(source), '-o', str(executable)], check=True)
                subprocess.run([str(executable), str(int(copy))], check=True, timeout=5)

    def test_shipped_configuration_uses_bounded_copied_rx(self):
        # Separate wiring contract; runtime ownership is exercised above.
        for filename in ('sdkconfig', 'sdkconfig.defaults'):
            config = (ROOT / filename).read_text()
            for setting in ('CONFIG_LWIP_L2_TO_L3_COPY=y', 'CONFIG_LWIP_SO_RCVBUF=y',
                            'CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=3'):
                self.assertTrue(setting in config, filename + ': missing ' + setting)


if __name__ == '__main__':
    unittest.main()
