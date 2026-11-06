"""Bounded captive DNS packets, including malformed phone/network input."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CASES = r'''
#include "xz_portal.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static const uint8_t ip[4]={192,168,4,1};
static const uint8_t query[]={0x12,0x34,1,0,0,1,0,0,0,0,0,0,
  7,'c','a','p','t','i','v','e',5,'a','p','p','l','e',3,'c','o','m',0,0,1,0,1};
int main(void) {
  uint8_t buf[514]; size_t n=sizeof query;
  memset(buf,0xa5,sizeof buf);memcpy(buf+1,query,n);
  size_t out=xz_portal_dns_reply(buf+1,n,512,ip);
  assert(out==n+16 && buf[0]==0xa5 && buf[513]==0xa5);
  assert(!memcmp(buf+1,query,2) && (buf[3]&0x80) && buf[8]==1);
  assert(!memcmp(buf+1+out-4,ip,4));
  assert(!memcmp(buf+1+12,query+12,n-12));
  // IPv6/HTTPS lookups get an empty successful response, not a timeout.
  for(unsigned type=28;type<=65;type+=37) {
    memcpy(buf,query,n);buf[n-3]=type;
    assert(xz_portal_dns_reply(buf,n,512,ip)==n && !buf[6] && !buf[7]);
  }
  for(size_t size=0;size<n;size++) {
    memcpy(buf,query,n);assert(!xz_portal_dns_reply(buf,size,512,ip));
  }
  memcpy(buf,query,n);assert(!xz_portal_dns_reply(buf,n,n+15,ip));
  memcpy(buf,query,n);buf[2]|=0x80;assert(!xz_portal_dns_reply(buf,n,512,ip));
  memcpy(buf,query,n);buf[5]=2;assert(!xz_portal_dns_reply(buf,n,512,ip));
  memcpy(buf,query,n);buf[12]=0xc0;assert(!xz_portal_dns_reply(buf,n,512,ip));
  memcpy(buf,query,n);buf[n-1]=3;assert(!xz_portal_dns_reply(buf,n,512,ip));
  // EDNS/additional records are not echoed into the generated reply.
  memcpy(buf,query,n);buf[11]=1;memset(buf+n,0xff,30);
  assert(xz_portal_dns_reply(buf,n+30,512,ip)==n+16 && !buf[11]);
  uint32_t seed=0x1234;
  for(unsigned i=0;i<10000;i++) {
    for(size_t j=1;j<513;j++){seed=seed*1664525+1013904223;buf[j]=seed>>24;}
    buf[0]=buf[513]=0xa5;
    size_t size=seed%513;
    assert(xz_portal_dns_reply(buf+1,size,512,ip)<=512);
    assert(buf[0]==0xa5&&buf[513]==0xa5);
  }
  puts("captive DNS: phone queries, truncation, bounds and malformed input passed");
}
'''


class CaptivePortalTest(unittest.TestCase):
    def test_dns_packets(self):
        component = ROOT/'components/platform/xiaozhi'
        with tempfile.TemporaryDirectory() as folder:
            c, exe = Path(folder)/'dns.c', Path(folder)/'dns'
            c.write_text(CASES)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                            '-fsanitize=undefined','-I',str(component),str(c),
                            str(component/'xz_portal.c'),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True,timeout=10)


if __name__ == '__main__':
    unittest.main()
