#include "xz_portal.h"
#include <stdbool.h>
#include <string.h>

size_t xz_wifi_option_add(xz_wifi_option_t *list, size_t count, const uint8_t *ssid,
                         size_t length, int rssi, bool secure, unsigned channel) {
  if (!list || !ssid || count>XZ_WIFI_OPTION_MAX || !length || length>32 ||
      !channel || channel>14 || memchr(ssid,0,length) || rssi < -128 || rssi > 0)
    return count;
  size_t at=0;
  while (at<count && (list[at].length!=length || memcmp(list[at].ssid,ssid,length))) at++;
  if (at<count && list[at].rssi>=rssi) return count;
  if (at==count) {
    if (count==XZ_WIFI_OPTION_MAX) {
      if (list[count-1].rssi>=rssi) return count;
      at=count-1;
    } else count++;
  }
  list[at]=(xz_wifi_option_t){.length=length,.rssi=rssi,.secure=secure};
  memcpy(list[at].ssid,ssid,length);
  while (at && list[at-1].rssi<list[at].rssi) {
    xz_wifi_option_t temp=list[at-1];list[at-1]=list[at];list[at]=temp;at--;
  }
  return count;
}
void xz_ssid_encode(const uint8_t *ssid,size_t length,char out[65]) {
  static const char hex[]="0123456789abcdef";
  out[0]=0;if(!ssid || length>32)return;
  for(size_t i=0;i<length;i++){out[2*i]=hex[ssid[i]>>4];out[2*i+1]=hex[ssid[i]&15];}
  out[2*length]=0;
}
static int digit(char c) {
  if(c>='0'&&c<='9')return c-'0';
  if(c>='a'&&c<='f')return c-'a'+10;
  if(c>='A'&&c<='F')return c-'A'+10;
  return -1;
}
bool xz_ssid_decode(const char *hex,uint8_t out[32],size_t *length) {
  if(!hex||!out||!length)return false;
  size_t n=strlen(hex);if(!n||n>64||(n&1))return false;
  uint8_t decoded[32]={0};
  for(size_t i=0;i<n/2;i++){
    int a=digit(hex[2*i]),b=digit(hex[2*i+1]);
    if(a<0||b<0||!(decoded[i]=(a<<4)|b))return false;
  }
  memcpy(out,decoded,n/2);*length=n/2;return true;
}

size_t xz_portal_dns_reply(uint8_t *p, size_t size, size_t capacity,
                          const uint8_t address[4]) {
  if (!p || !address || size < 17 || size > capacity ||
      (p[2] & 0xf8) || p[4] || p[5] != 1) return 0;
  size_t end = 12;
  while (end < size && p[end]) {
    unsigned label = p[end++];
    if (label > 63 || label > size - end || end + label - 12 > 254) return 0;
    end += label;
  }
  if (end >= size || size - ++end < 4 || p[end] || p[end+2] || p[end+3] != 1)
    return 0;
  bool answer = p[end+1] == 1;
  end += 4;
  if (answer && capacity - end < 16) return 0;
  p[2] = 0x84 | (p[2] & 1);
  p[3] = 0;
  p[6] = 0; p[7] = answer ? 1 : 0;
  memset(p+8, 0, 4);
  if (answer) {
    static const uint8_t a[] = {0xc0,0x0c,0,1,0,1,0,0,0,30,0,4};
    memcpy(p+end,a,sizeof a);
    memcpy(p+end+sizeof a,address,4);
    end += 16;
  }
  return end;
}
