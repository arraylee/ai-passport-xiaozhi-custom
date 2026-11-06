#include "xz_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    uint8_t key[16]={0};
    assert(xz_hex16("00112233445566778899aabbccddeeff",key));
    assert(key[0]==0&&key[15]==255);
    assert(!xz_hex16("00112233445566778899aabbccddeefg",key));assert(key[15]==255);
    assert(!xz_hex16("00",key));assert(!xz_hex16(NULL,key));
    uint8_t packet[1292]={1,0,0,1};packet[15]=1;
    uint32_t seq=0;assert(xz_udp_valid(packet,17,0,&seq)&&seq==1);
    assert(!xz_udp_valid(packet,16,0,&seq));assert(!xz_udp_valid(packet,18,0,&seq));
    assert(!xz_udp_valid(packet,17,1,&seq));assert(!xz_udp_valid(packet,1292,0,&seq));
    packet[0]=2;assert(!xz_udp_valid(packet,17,0,&seq));
    xz_json_buffer_t b={0};
    assert(!xz_fragment(&b,"{",1,0,2));assert(xz_fragment(&b,"}",1,1,2));assert(!strcmp(b.data,"{}"));
    assert(!xz_fragment(&b,"{",1,0,4));assert(!xz_fragment(&b,"x",1,2,4));
    assert(!xz_fragment(&b,"x",1,1,2));assert(xz_fragment(&b,"{}",2,0,2));
    assert(!xz_fragment(&b,"x",1,0,4097));assert(!xz_fragment(&b,"x",1,-1,1));
    char out[8];xz_text_copy(out,8,"你好呀");assert(!strcmp(out,"你好"));
    xz_text_copy(out,3,"你好");assert(!strcmp(out,""));xz_text_copy(out,8,NULL);assert(!out[0]);
    puts("XiaoZhi protocol: bounded fragments, UTF-8, key lengths, UDP length/type/replay passed");
}
