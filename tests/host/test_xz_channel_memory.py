"""Compile production MQTT channel code against real cJSON/protocol parsing.

Only allocation, MQTT/FreeRTOS, clock, AES and UDP boundaries are fake. The
channel type, callbacks, connection/hello logic and caller cleanup are extracted
from xiaozhi.c on every run; no production state machine is reimplemented here.
This proves host ordering and ownership, not physical TLS heap sufficiency.
"""
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


def block(source, start):
    opening = source.index("{", start)
    depth = 0
    for token in re.finditer(
        r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|[{}]', source[opening:]
    ):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[start:opening + token.end()]
    raise AssertionError("Unclosed production block")


def function(source, name):
    match = re.search(
        r"(?m)^(?:static\s+)?(?:const\s+)?\w+\s+\*?" + name
        + r"\([^;]*?\)\s*\{", source
    )
    if not match:
        raise AssertionError(f"Missing production function {name}")
    return block(source, match.start())


BOUNDARY = r'''
#include "cJSON.h"
#include "xz_protocol.h"
#include <assert.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

typedef int esp_err_t;
typedef const char *esp_event_base_t;
typedef struct fake_mqtt *esp_mqtt_client_handle_t;
typedef struct { unsigned initialized; } mbedtls_aes_context;
typedef struct {
    int error_type, esp_tls_last_esp_err, esp_tls_stack_err;
    int esp_tls_cert_verify_flags, connect_return_code;
} esp_mqtt_error_codes_t;
typedef struct {
    char *data;
    int data_len, current_data_offset, total_data_len, msg_id;
    esp_mqtt_error_codes_t *error_handle;
} esp_mqtt_event_t, *esp_mqtt_event_handle_t;
typedef struct {
    struct {
        struct { const char *uri; } address;
        struct { int (*crt_bundle_attach)(void *); } verification;
    } broker;
    struct {
        const char *client_id, *username;
        struct { const char *password; } authentication;
    } credentials;
    struct { int keepalive; } session;
    struct { int timeout_ms, reconnect_timeout_ms; bool disable_auto_reconnect; } network;
    struct { int stack_size; } task;
    struct { int size, out_size; } buffer;
} esp_mqtt_client_config_t;

#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_EVENT_ANY_ID -1
#define MALLOC_CAP_8BIT 0
#define pdMS_TO_TICKS(ms) (ms)
enum { MQTT_EVENT_CONNECTED, MQTT_EVENT_DISCONNECTED, MQTT_EVENT_ERROR, MQTT_EVENT_DATA, MQTT_EVENT_SUBSCRIBED };
enum { MQTT_ERROR_TYPE_TCP_TRANSPORT=1, MQTT_ERROR_TYPE_CONNECTION_REFUSED, MQTT_ERROR_TYPE_SUBSCRIBE_FAILED };
enum { MQTT_CONNECTION_REFUSE_SERVER_UNAVAILABLE=3,
       MQTT_CONNECTION_REFUSE_BAD_USERNAME=4, MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED=5 };
static const char *TAG="host-channel";
static void codec_arena_metric(unsigned step){(void)step;}
static atomic_bool cancel;
static int message_guard;
#define taskENTER_CRITICAL(g) ((void)(g))
#define taskEXIT_CRITICAL(g) ((void)(g))
static unsigned esp_get_free_heap_size(void) { return 20000; }
static unsigned heap_caps_get_largest_free_block(int cap) { (void)cap; return 18000; }
static char log_line[512];
#define ESP_LOGI(tag,...) ((void)(tag),(void)snprintf(log_line,sizeof log_line,__VA_ARGS__))
#define ESP_LOGW(tag,...) ESP_LOGI(tag,__VA_ARGS__)
static int esp_crt_bundle_attach(void *p) { (void)p; return 0; }
'''


FAKES = r'''
enum { SUCCESS, DELAYED_CONNECT, INVALID_CONFIG, INIT_FAIL, REGISTER_FAIL,
       START_FAIL, CONNECT_TIMEOUT, CANCEL_CONNECT, CONNECT_ERROR, DISCONNECT_CONNECT,
       ALLOC_FAIL, SUBSCRIBE_FAIL, PUBLISH_FAIL, HELLO_TIMEOUT, CANCEL_HELLO,
       DISCONNECT_HELLO, INVALID_HELLO, NOTICE_BEFORE_HELLO, FRAGMENTED_HELLO,
       QUEUE_FAIL, CALLBACK_GATE, DELAYED_SUBACK, SUBACK_TIMEOUT, REJECT_SUBACK,
       CANCEL_SUBACK, DISCONNECT_SUBACK, WRONG_SUBACK, FAILED_THEN_CONNECTED, NO_SUBSCRIBE, DISCONNECT_ALLOC, DISCONNECT_HELLO_PARSE, CONTROL_BUDGET };
enum { JSON_ALLOC, CHANNEL_ALLOC, FRAGMENT_ALLOC, QUEUE_ALLOC, TEXT_ALLOC, ALLOC_KINDS };
static unsigned scenario, detail, now_ms, delays, receives;
static unsigned init_calls, register_calls, start_calls, stops, destroys;
static unsigned control_attempts, subscribe_calls, publish_calls, hello_calls;
static unsigned suback_waits; static bool subscribe_accepted;
static unsigned parse_allocations;
static unsigned queue_sends, pre_ready_data, stopped_data, aes_frees, sockets, closes;
static unsigned socket_options;
static unsigned live[ALLOC_KINDS], allocated[ALLOC_KINDS];
static void handle_mcp(channel_t *c,const cJSON *j){(void)c;(void)j;}
static channel_t *active_channel;
static cJSON **active_cloud;
static void emit(int event,esp_mqtt_event_t *data);
struct allocation { void *pointer; unsigned kind; };
static struct allocation allocations[256];

static void *allocate(unsigned kind, size_t bytes, bool zero) {
    if(kind==FRAGMENT_ALLOC || kind==QUEUE_ALLOC || kind==TEXT_ALLOC) {
        ++control_attempts;
        assert(start_calls==1 && atomic_load(&active_channel->connected));
        assert(atomic_load(&active_channel->control_ready));
        assert(active_cloud && !*active_cloud && live[JSON_ALLOC]==0);
        if((scenario==ALLOC_FAIL && control_attempts==1) ||
           (scenario==35 && control_attempts==3)) return NULL;
        if(scenario==CONTROL_BUDGET && bytes>1024) return NULL;
    }
    void *p=zero?calloc(1,bytes):malloc(bytes); assert(p);
    for(unsigned i=0;i<sizeof allocations/sizeof allocations[0];++i) {
        if(!allocations[i].pointer) {
            allocations[i]=(struct allocation){p,kind};
            ++live[kind]; ++allocated[kind];
            if(scenario==DISCONNECT_ALLOC && kind==FRAGMENT_ALLOC && control_attempts==1) emit(MQTT_EVENT_DISCONNECTED,NULL);
            return p;
        }
    }
    abort();
}
static void release(void *p) {
    if(!p) return;
    for(unsigned i=0;i<sizeof allocations/sizeof allocations[0];++i) {
        if(allocations[i].pointer==p) {
            assert(live[allocations[i].kind]); --live[allocations[i].kind];
            allocations[i].pointer=NULL; free(p); return;
        }
    }
    assert(!"Unknown or double-freed allocation");
}
static void *json_malloc(size_t n) {
    if(scenario==36 && hello_calls && ++parse_allocations==detail)return NULL;
    return allocate(JSON_ALLOC,n,false);
}
static void json_free(void *p) { release(p); }
static void *control_malloc(size_t n) {
    assert(n>=sizeof(channel_message_t)+1 && n<=sizeof(channel_message_t)+XZ_JSON_MAX+1);
    return allocate(FRAGMENT_ALLOC,n,false);
}

struct fake_mqtt {
    bool initialized;
    void (*callback)(void *,esp_event_base_t,int32_t,void *);
    void *arg;
    const char *password_source;
    char uri[208], client[256], username[512], password[768];
};
static struct fake_mqtt mqtt;
static esp_mqtt_error_codes_t event_error;
static void emit(int event,esp_mqtt_event_t *data) {
    assert(mqtt.callback); mqtt.callback(mqtt.arg,NULL,event,data);
}
static void data_event(const char *message,int n,int offset,int total) {
    esp_mqtt_event_t event={.data=(char *)message,.data_len=n,
        .current_data_offset=offset,.total_data_len=total};
    emit(MQTT_EVENT_DATA,&event);
}
static void ignored_data(void) {
    unsigned before=queue_sends;
    assert(!atomic_load(&active_channel->control_ready));
    data_event("{\"type\":\"early\"}",16,0,16);
    assert(queue_sends==before); ++pre_ready_data;
}
static esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *cfg) {
    ++init_calls;
    assert(live[JSON_ALLOC]==0 && active_cloud && !*active_cloud);
    assert(control_attempts==0 && !active_channel->partial && !active_channel->first);
    assert(cfg->broker.verification.crt_bundle_attach==esp_crt_bundle_attach);
    assert(!strcmp(cfg->broker.address.uri,"mqtts://mqtt.example.invalid:8883"));
    assert(cfg->network.timeout_ms==6000 && cfg->network.reconnect_timeout_ms<=500);
    assert(cfg->network.reconnect_timeout_ms>0 && cfg->network.disable_auto_reconnect);
    assert(cfg->session.keepalive==60 && cfg->task.stack_size==5120);
    assert(cfg->buffer.size==1024 && cfg->buffer.out_size==1024);
    if(scenario==INIT_FAIL) return NULL;
    mqtt.initialized=true;
    snprintf(mqtt.uri,sizeof mqtt.uri,"%s",cfg->broker.address.uri);
    snprintf(mqtt.client,sizeof mqtt.client,"%s",cfg->credentials.client_id);
    snprintf(mqtt.username,sizeof mqtt.username,"%s",cfg->credentials.username);
    snprintf(mqtt.password,sizeof mqtt.password,"%s",cfg->credentials.authentication.password);
    mqtt.password_source=cfg->credentials.authentication.password;
    return &mqtt;
}
static esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t client,int event,
    void (*callback)(void *,esp_event_base_t,int32_t,void *),void *arg) {
    assert(client==&mqtt && event==ESP_EVENT_ANY_ID); ++register_calls;
    assert(!*active_cloud && live[JSON_ALLOC]==0);
    if(scenario==REGISTER_FAIL) return detail?ESP_ERR_NO_MEM:ESP_FAIL;
    mqtt.callback=callback; mqtt.arg=arg; return ESP_OK;
}
static esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t client) {
    assert(client==&mqtt && mqtt.initialized); ++start_calls;
    /* This is the TLS boundary: copied config is valid but OTA/control storage is gone. */
    assert(!*active_cloud && live[JSON_ALLOC]==0 && control_attempts==0);
    assert(!active_channel->partial && !active_channel->first);
    assert(!atomic_load(&active_channel->control_ready));
    assert(!strcmp(mqtt.client,"host-test-device") && !strcmp(mqtt.username,"host-test-user"));
    assert(!strcmp(mqtt.password,"fabricated-test-password") && mqtt.password_source[0]==0);
    ignored_data();
    if(scenario==START_FAIL) return detail?ESP_ERR_NO_MEM:ESP_FAIL;
    if(scenario==FAILED_THEN_CONNECTED) {
        emit(MQTT_EVENT_DISCONNECTED,NULL); emit(MQTT_EVENT_CONNECTED,NULL);
        return ESP_OK;
    }
    if(scenario!=DELAYED_CONNECT && scenario!=CONNECT_TIMEOUT && scenario!=CANCEL_CONNECT &&
       scenario!=CONNECT_ERROR && scenario!=DISCONNECT_CONNECT) emit(MQTT_EVENT_CONNECTED,NULL);
    return ESP_OK;
}
static void vTaskDelay(unsigned ticks) {
    assert(ticks==100); ++delays; now_ms+=ticks;
    if(subscribe_calls && !hello_calls) {
        ++suback_waits;
        if(scenario==CANCEL_SUBACK && suback_waits==2) atomic_store(&cancel,true);
        if(scenario==DISCONNECT_SUBACK && suback_waits==2) emit(MQTT_EVENT_DISCONNECTED,NULL);
        if((scenario==DELAYED_SUBACK && suback_waits==3) ||
           (scenario==WRONG_SUBACK && suback_waits==2)) {
            esp_mqtt_event_t ack={.msg_id=1}; subscribe_accepted=true;
            emit(MQTT_EVENT_SUBSCRIBED,&ack);
        }
        return;
    }
    if(hello_calls) {
        ++receives;
        if(scenario==CANCEL_HELLO && receives==2)atomic_store(&cancel,true);
        if(scenario==DISCONNECT_HELLO && receives==2)emit(MQTT_EVENT_DISCONNECTED,NULL);
        return;
    }
    assert(control_attempts==0); ignored_data();
    if(scenario==DELAYED_CONNECT && delays==3) emit(MQTT_EVENT_CONNECTED,NULL);
    if(scenario==CANCEL_CONNECT && delays==2) atomic_store(&cancel,true);
    if(scenario==DISCONNECT_CONNECT && delays==2) emit(MQTT_EVENT_DISCONNECTED,NULL);
    if(scenario==CONNECT_ERROR && delays==2) {
        esp_mqtt_event_t event={.error_handle=detail==12?NULL:&event_error};
        emit(MQTT_EVENT_ERROR,&event);
    }
}
static int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t client,const char *topic,int qos) {
    assert(client==&mqtt && !strcmp(topic,"test/device/down") && qos==0); ++subscribe_calls;
    assert(atomic_load(&active_channel->control_ready));
    if(scenario==SUBSCRIBE_FAIL)return -1;
    if(scenario!=DELAYED_SUBACK && scenario!=SUBACK_TIMEOUT && scenario!=CANCEL_SUBACK && scenario!=DISCONNECT_SUBACK) {
        esp_mqtt_error_codes_t error={.error_type=MQTT_ERROR_TYPE_SUBSCRIBE_FAILED};
        esp_mqtt_event_t ack={.msg_id=scenario==WRONG_SUBACK?73:1,
                             .error_handle=scenario==REJECT_SUBACK?&error:NULL};
        subscribe_accepted=scenario!=REJECT_SUBACK && scenario!=WRONG_SUBACK;
        emit(MQTT_EVENT_SUBSCRIBED,&ack);
    }
    return 1;
}
static const char *hello_message=
    "{\"type\":\"hello\",\"transport\":\"udp\",\"session_id\":\"host-test-session\","
    "\"audio_params\":{\"sample_rate\":16000,\"frame_duration\":60},"
    "\"udp\":{\"server\":\"voice.example.invalid\",\"port\":8888,"
    "\"key\":\"000102030405060708090a0b0c0d0e0f\","
    "\"nonce\":\"101112131415161718191a1b1c1d1e1f\"}}";
static int esp_mqtt_client_publish(esp_mqtt_client_handle_t client,const char *topic,
    const char *payload,int length,int qos,int retain) {
    assert(client==&mqtt && !strcmp(topic,"test/device/up"));
    assert(length==0 && qos==0 && retain==0); ++publish_calls;
    if(!strstr(payload,"\"type\":\"hello\"")) return 1;
    ++hello_calls;
    assert((scenario==NO_SUBSCRIBE || subscribe_accepted) && "hello published before broker acknowledged subscribe");
    assert(atomic_load(&active_channel->control_ready) && atomic_load(&active_channel->connected));
    assert(!live[FRAGMENT_ALLOC] && !live[QUEUE_ALLOC] && !live[TEXT_ALLOC]);
    assert(control_attempts==0 && live[JSON_ALLOC]==0);
    if(scenario==PUBLISH_FAIL) return -1;
    if(scenario==HELLO_TIMEOUT || scenario==CANCEL_HELLO || scenario==DISCONNECT_HELLO) return 1;
    if(scenario==NOTICE_BEFORE_HELLO) {
        const char *notice="{\"type\":\"notice\"}";
        data_event(notice,(int)strlen(notice),0,(int)strlen(notice));
    }
    const char *reply=scenario==37?"{":scenario==INVALID_HELLO?"{\"type\":\"hello\",\"transport\":\"tcp\"}":hello_message;
    int n=(int)strlen(reply);
    if(scenario==FRAGMENTED_HELLO) {
        size_t before=active_channel->queued_bytes;
        data_event(reply,17,0,n); assert(active_channel->queued_bytes==before);
        data_event(reply+17,n-17,17,n);
    } else if(scenario==QUEUE_FAIL) {
        for(unsigned i=0;i<30 && !atomic_load(&active_channel->failed);i++)data_event(reply,n,0,n);
    } else data_event(reply,n,0,n);
    return 1;
}
static esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t client) {
    assert(client==&mqtt && !atomic_load(&active_channel->control_ready)); ++stops;
    if(mqtt.callback) { ignored_data(); ++stopped_data; }
    return ESP_OK;
}
static esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t client) {
    assert(client==&mqtt && stops==1); ++destroys; mqtt.initialized=false; return ESP_OK;
}
static void mbedtls_aes_init(mbedtls_aes_context *aes) { aes->initialized=1; }
static int mbedtls_aes_setkey_enc(mbedtls_aes_context *aes,const unsigned char *key,unsigned bits) {
    assert(aes->initialized && bits==128 && key[0]==0 && key[15]==15); return 0;
}
static void mbedtls_aes_free(mbedtls_aes_context *aes) {
    assert(aes->initialized); aes->initialized=0; ++aes_frees;
}
static struct sockaddr_in udp_address;
static struct addrinfo udp_info;
static int fake_getaddrinfo(const char *host,const char *service,const struct addrinfo *hints,struct addrinfo **out) {
    assert(!strcmp(host,"voice.example.invalid") && !strcmp(service,"8888"));
    assert(hints->ai_family==AF_INET && hints->ai_socktype==SOCK_DGRAM);
    udp_info=(struct addrinfo){.ai_family=AF_INET,.ai_socktype=SOCK_DGRAM,
        .ai_addr=(struct sockaddr *)&udp_address,.ai_addrlen=sizeof udp_address};
    *out=&udp_info; return 0;
}
static void fake_freeaddrinfo(struct addrinfo *p) { assert(p==&udp_info); }
static int fake_socket(int family,int type,int protocol) {
    assert(family==AF_INET && type==SOCK_DGRAM && protocol==0); ++sockets; return 123;
}
static int fake_connect(int fd,const struct sockaddr *address,socklen_t length) {
    assert(fd==123 && address==udp_info.ai_addr && length==udp_info.ai_addrlen);
    if(scenario==DISCONNECT_HELLO_PARSE)emit(MQTT_EVENT_DISCONNECTED,NULL);
    return 0;
}
static int fake_setsockopt(int fd,int level,int name,const void *value,socklen_t size) {
    assert(fd==123 && level==SOL_SOCKET);
    if(name==SO_RCVTIMEO) {
        assert(size==sizeof(struct timeval));const struct timeval *timeout=value;
        assert(timeout->tv_sec==0 && timeout->tv_usec==20000);socket_options|=1;
        return scenario==38 && detail==0 ? -1 : 0;
    }
    assert(name==SO_RCVBUF && size==sizeof(int) && *(const int*)value==2*(XZ_PACKET_MAX+16));
    socket_options|=2;return scenario==38 && detail==1 ? -1 : 0;
}
static int fake_close(int fd) { assert(fd==123); ++closes; return 0; }

#define malloc control_malloc
#define free release
#define getaddrinfo fake_getaddrinfo
#define freeaddrinfo fake_freeaddrinfo
#define socket fake_socket
#define connect fake_connect
#define setsockopt fake_setsockopt
#define close fake_close
'''


CASES = r'''
#undef malloc
#undef calloc
#undef free
#undef getaddrinfo
#undef freeaddrinfo
#undef socket
#undef connect
#undef setsockopt
#undef close

static cJSON *cloud_config(void) {
    return cJSON_Parse("{\"mqtt\":{\"endpoint\":\"mqtt.example.invalid:8883\","
        "\"client_id\":\"host-test-device\",\"username\":\"host-test-user\","
        "\"password\":\"fabricated-test-password\",\"publish_topic\":\"test/device/up\","
        "\"subscribe_topic\":\"test/device/down\"}}");
}
static void invalid_config(cJSON *cloud) {
    cJSON *m=cJSON_GetObjectItem(cloud,"mqtt");
    switch(detail) {
    case 0: cJSON_DeleteItemFromObject(m,"password"); break;
    case 1: cJSON_ReplaceItemInObject(m,"password",cJSON_CreateString("")); break;
    case 2: cJSON_ReplaceItemInObject(m,"endpoint",cJSON_CreateString("tcp://invalid")); break;
    case 3: cJSON_ReplaceItemInObject(m,"endpoint",cJSON_CreateString("invalid\rhost")); break;
    case 4: cJSON_ReplaceItemInObject(m,"endpoint",cJSON_CreateString("invalid\nhost")); break;
    case 5: cJSON_ReplaceItemInObject(m,"username",cJSON_CreateNumber(3)); break;
    case 6: {
        char long_topic[257]; memset(long_topic,'x',sizeof long_topic-1); long_topic[256]=0;
        cJSON_ReplaceItemInObject(m,"subscribe_topic",cJSON_CreateString(long_topic)); break;
    }
    default: abort();
    }
}
static int configure_error(void) {
    event_error=(esp_mqtt_error_codes_t){.error_type=MQTT_ERROR_TYPE_TCP_TRANSPORT};
    switch(detail) {
    case 0: event_error.esp_tls_stack_err=0x7f00; return CHANNEL_MEMORY;
    case 1: event_error.esp_tls_stack_err=-0x7f00; return CHANNEL_MEMORY;
    case 2: event_error.esp_tls_stack_err=0x4290; return CHANNEL_MEMORY;
    case 3: event_error.esp_tls_stack_err=-0x4290; return CHANNEL_MEMORY;
    case 4: event_error.esp_tls_last_esp_err=ESP_ERR_NO_MEM; return CHANNEL_MEMORY;
    case 5: event_error.error_type=MQTT_ERROR_TYPE_CONNECTION_REFUSED;
            event_error.connect_return_code=MQTT_CONNECTION_REFUSE_BAD_USERNAME; return CHANNEL_AUTH;
    case 6: event_error.error_type=MQTT_ERROR_TYPE_CONNECTION_REFUSED;
            event_error.connect_return_code=MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED; return CHANNEL_AUTH;
    case 7: event_error.esp_tls_stack_err=-0x2700; return CHANNEL_TLS;
    case 8: event_error.esp_tls_cert_verify_flags=8; return CHANNEL_TLS;
    case 9: return CHANNEL_CONNECTION;
    case 10: event_error.error_type=MQTT_ERROR_TYPE_CONNECTION_REFUSED;
             event_error.connect_return_code=MQTT_CONNECTION_REFUSE_SERVER_UNAVAILABLE; return CHANNEL_CONNECTION;
    case 11: event_error.esp_tls_stack_err=-0x2880; return CHANNEL_MEMORY;
    case 12: return CHANNEL_CONNECTION;
    case 13: event_error.esp_tls_stack_err=0x3f80; return CHANNEL_MEMORY;
    default: abort();
    }
}
static void check_no_live_allocations(void) {
    for(unsigned i=0;i<ALLOC_KINDS;++i) assert(live[i]==0);
}
int main(int argc,char **argv) {
    assert(argc==3); scenario=(unsigned)atoi(argv[1]); detail=(unsigned)atoi(argv[2]);
    cJSON_Hooks hooks={.malloc_fn=json_malloc,.free_fn=json_free}; cJSON_InitHooks(&hooks);
    channel_t *c=allocate(CHANNEL_ALLOC,sizeof *c,true); c->udp=-1; mbedtls_aes_init(&c->aes);
    active_channel=c;
    cJSON *cloud=cloud_config(); assert(cloud); active_cloud=&cloud;
    if(scenario==INVALID_CONFIG) invalid_config(cloud);
    if(scenario==NO_SUBSCRIBE)cJSON_DeleteItemFromObject(cJSON_GetObjectItem(cloud,"mqtt"),"subscribe_topic");
    int expected_error=scenario==CONNECT_ERROR?configure_error():CHANNEL_OK;
    if(scenario==CALLBACK_GATE) {
        mqtt.callback=mqtt_event; mqtt.arg=c;
        cJSON_Delete(cloud); cloud=NULL; start_calls=1;
        ignored_data();
        mqtt_event(c,NULL,MQTT_EVENT_CONNECTED,NULL);
        const char init[]="{\"type\":\"mcp\",\"payload\":{\"method\":\"initialize\"}}";
        const int length=sizeof init-1;
        data_event(init,12,0,length);
        data_event(init+12,length-12,12,length);
        channel_message_t *message=channel_take(c,0);
        assert(atomic_load(&c->connected) && message && !strcmp(message->data,init));
        release(message);
        atomic_store(&c->closing,true);
        atomic_store(&c->control_ready,false);
        mqtt_event(c,NULL,MQTT_EVENT_CONNECTED,NULL);
        ignored_data();
        atomic_store(&c->connected,false);
        assert(!send_text(c,"test"));
        if(scenario==SUCCESS && detail==99)outcome=SESSION_RENEW;
    unsigned at_close=publish_calls;
    release_channel(c); cJSON_Delete(cloud);
    if(scenario==SUCCESS)assert(publish_calls==at_close+(outcome==SESSION_RENEW?0:1)); check_no_live_allocations();
        puts("callback readiness gate PASS"); return 0;
    }
    if(scenario>=32 && scenario<=35) {
        cJSON_Delete(cloud);cloud=NULL;start_calls=1;
        mqtt.callback=mqtt_event;mqtt.arg=c;
        atomic_store(&c->connected,true);atomic_store(&c->control_ready,true);
        char full[XZ_JSON_MAX];for(unsigned i=0;i<sizeof full;i++)full[i]=(char)('A'+i%26);
        if(scenario==32) {
            data_event(full,17,0,sizeof full);assert(c->partial && !c->first && control_attempts==1);
            data_event(full+17,1007,17,sizeof full);assert(c->partial && !c->first);
            data_event(full+1024,sizeof full-1024,1024,sizeof full);
            assert(!c->partial && c->queued_bytes==XZ_JSON_MAX+4 && live[FRAGMENT_ALLOC]==1);
            channel_message_t *owned=channel_take(c,0);assert(owned && owned->used==sizeof full && !memcmp(owned->data,full,sizeof full));
            assert(!c->queued_bytes && !c->first && owned->data[sizeof full]==0);
            data_event("next",4,0,4);assert(c->queued_bytes==8 && live[FRAGMENT_ALLOC]==2);
            assert(!memcmp(owned->data,full,sizeof full));release(owned);
        } else if(scenario==33) {
            data_event(full,17,0,100);assert(c->partial);
            data_event(full+17,2,18,100);assert(!c->partial && !c->first && !live[FRAGMENT_ALLOC]);
            data_event(full,17,0,100);data_event(full+17,83,17,101);
            assert(!c->partial && !live[FRAGMENT_ALLOC]);
            data_event(full,1,0,XZ_JSON_MAX+1);assert(!c->partial && !live[FRAGMENT_ALLOC]);
            data_event(full,17,0,100);data_event("new",3,0,3);
            assert(!c->partial && c->queued_bytes==7 && live[FRAGMENT_ALLOC]==1);
            unsigned before=control_attempts;cancel=true;data_event("late",4,0,4);
            assert(control_attempts==before && c->queued_bytes==7);
        } else if(scenario==34) {
            unsigned accepted=0;
            while(!atomic_load(&c->failed)){data_event(full,20,0,20);accepted++;assert(accepted<256);}
            assert(accepted==172 && c->queued_bytes==171*24 && live[FRAGMENT_ALLOC]==171);
            assert(atomic_load(&c->error)==CHANNEL_CONTROL_OVERFLOW);
            for(unsigned i=0;i<171;i++){channel_message_t *m=channel_take(c,0);assert(m&&m->used==20);release(m);}
            assert(!c->queued_bytes && !c->first && !c->last && !live[FRAGMENT_ALLOC]);
        } else if(scenario==35) {
            data_event("first",5,0,5);assert(c->first && control_attempts==1);
            data_event(full,17,0,100);assert(c->partial && control_attempts==2);
            data_event("replacement",11,0,11);
            assert(!c->partial && c->first && control_attempts==3 && live[FRAGMENT_ALLOC]==1);
            assert(atomic_load(&c->failed)&&atomic_load(&c->error)==CHANNEL_MEMORY);
        } else abort();
        atomic_store(&c->control_ready,false);
        unsigned before=control_attempts;data_event("late",4,0,4);assert(control_attempts==before);
        release_channel(c);check_no_live_allocations();
        printf("owned frame scenario=%u: exact max length, fragments, byte quota, no alias, late gate and cleanup PASS\n",scenario);
        return 0;
    }
    bool ok=open_channel(c,&cloud);
    bool success=scenario==SUCCESS || scenario==DELAYED_CONNECT ||
                 scenario==NOTICE_BEFORE_HELLO || scenario==FRAGMENTED_HELLO ||
                 scenario==DELAYED_SUBACK || scenario==WRONG_SUBACK || scenario==NO_SUBSCRIBE || scenario==CONTROL_BUDGET;
    assert(ok==success);
    switch(scenario) {
    case SUCCESS: case DELAYED_CONNECT: case NOTICE_BEFORE_HELLO: case FRAGMENTED_HELLO:
    case DELAYED_SUBACK: case WRONG_SUBACK: case NO_SUBSCRIBE: case CONTROL_BUDGET: {
        assert(!cloud && atomic_load(&c->error)==CHANNEL_OK && atomic_load(&c->control_ready));
        assert(control_attempts==(scenario==NOTICE_BEFORE_HELLO?2U:1U) && hello_calls==1 && subscribe_calls==(scenario==NO_SUBSCRIBE?0U:1U));
        assert(!c->partial && !c->first && !c->queued_bytes && !live[FRAGMENT_ALLOC]);
        assert(c->rate==16000 && !strcmp(c->session,"host-test-session") && c->udp==123);
        assert(socket_options==3);
        assert(delays==(scenario==DELAYED_CONNECT || scenario==DELAYED_SUBACK?3U:scenario==WRONG_SUBACK?2U:0U));
        assert(!receives);
        unsigned before=control_attempts, published=publish_calls;
        atomic_store(&c->connected,false); assert(!send_text(c,"not-connected"));
        assert(publish_calls==published); atomic_store(&c->connected,true);
        assert(send_text(c,"{\"type\":\"test\"}"));
        assert(control_attempts==before && !live[TEXT_ALLOC] && !live[FRAGMENT_ALLOC]);
        break;
    }
    case 36: case 37:
        assert(atomic_load(&c->failed) && atomic_load(&c->error)==CHANNEL_CONTROL_PARSE);
        assert(control_attempts==1 && !c->first && !c->partial && !live[FRAGMENT_ALLOC]);
        assert(!live[JSON_ALLOC] && !sockets && !receives); break;
    case 38:
        assert(sockets==1 && c->udp==123 && atomic_load(&c->error)==CHANNEL_HELLO_INVALID);
        assert(socket_options==(detail?3U:1U));break;
    case INVALID_CONFIG:
        assert(cloud && atomic_load(&c->error)==CHANNEL_CONFIG && !init_calls); break;
    case INIT_FAIL:
        assert(!cloud && atomic_load(&c->error)==CHANNEL_MEMORY && init_calls==1 && !start_calls); break;
    case REGISTER_FAIL:
        assert(!cloud && !start_calls && !control_attempts);
        assert(atomic_load(&c->error)==(detail?CHANNEL_MEMORY:CHANNEL_CONNECTION)); break;
    case START_FAIL:
        assert(!cloud && start_calls==1 && !delays && !control_attempts);
        assert(atomic_load(&c->error)==(detail?CHANNEL_MEMORY:CHANNEL_CONNECTION)); break;
    case CONNECT_TIMEOUT:
        assert(delays==150 && now_ms==15000 && !control_attempts);
        assert(atomic_load(&c->error)==CHANNEL_CONNECT_TIMEOUT); break;
    case CANCEL_CONNECT:
        assert(atomic_load(&cancel) && delays==2 && now_ms==200 && !control_attempts); break;
    case CONNECT_ERROR:
        assert(delays==2 && !control_attempts && atomic_load(&c->failed));
        assert(atomic_load(&c->error)==expected_error);
        assert(channel_error_text(c)[0]);
        /* A later disconnect must preserve the more specific TLS/auth reason. */
        emit(MQTT_EVENT_DISCONNECTED,NULL); assert(atomic_load(&c->error)==expected_error); break;
    case DISCONNECT_CONNECT:
        assert(delays==2 && !control_attempts && atomic_load(&c->error)==CHANNEL_CONNECTION); break;
    case ALLOC_FAIL:
        assert(control_attempts==1 && atomic_load(&c->control_ready));
        assert(!live[FRAGMENT_ALLOC] && atomic_load(&c->failed));
        assert(atomic_load(&c->error)==CHANNEL_MEMORY && hello_calls==1 && subscribe_calls==1);
        assert(!stops && !destroys); break;
    case SUBSCRIBE_FAIL:
        assert(control_attempts==0 && subscribe_calls==1 && !hello_calls);
        assert(atomic_load(&c->error)==CHANNEL_SUBSCRIBE); break;
    case PUBLISH_FAIL:
        assert(hello_calls==1 && !receives && atomic_load(&c->error)==CHANNEL_HELLO_SEND); break;
    case HELLO_TIMEOUT:
        assert(receives==100 && now_ms==10000 && atomic_load(&c->error)==CHANNEL_HELLO_TIMEOUT); break;
    case CANCEL_HELLO:
        assert(receives==2 && now_ms==200 && atomic_load(&cancel)); break;
    case DISCONNECT_HELLO:
        assert(receives==2 && now_ms==200 && atomic_load(&c->failed));
        assert(atomic_load(&c->error)==CHANNEL_CONNECTION); break;
    case INVALID_HELLO:
        assert(receives==99 && sockets==0 && atomic_load(&c->error)==CHANNEL_HELLO_INVALID); break;
    case QUEUE_FAIL:
        assert(control_attempts>1 && atomic_load(&c->failed) && !receives);
        assert(c->queued_bytes<=XZ_JSON_MAX+16 && atomic_load(&c->error)==CHANNEL_CONTROL_OVERFLOW); break;
    case DISCONNECT_ALLOC:
        assert(control_attempts==1 && subscribe_calls==1 && hello_calls==1);
        assert(!c->partial && !c->first && !c->queued_bytes);
        assert(atomic_load(&c->error)==CHANNEL_CONNECTION); break;
    case DISCONNECT_HELLO_PARSE:
        assert(sockets==1 && c->udp==123 && atomic_load(&c->failed));
        assert(atomic_load(&c->error)==CHANNEL_CONNECTION); break;
    case FAILED_THEN_CONNECTED:
        assert(!control_attempts && !hello_calls && !receives);
        assert(atomic_load(&c->failed) && !atomic_load(&c->connected));
        assert(atomic_load(&c->error)==CHANNEL_CONNECTION); break;
    case SUBACK_TIMEOUT:
        assert(suback_waits==50 && now_ms==5000);
        assert(atomic_load(&c->error)==CHANNEL_SUBSCRIBE_TIMEOUT);
        assert(!hello_calls && !receives); break;
    case REJECT_SUBACK:
        assert(!suback_waits && atomic_load(&c->failed));
        assert(atomic_load(&c->error)==CHANNEL_SUBSCRIBE && !hello_calls && !receives); break;
    case CANCEL_SUBACK:
        assert(suback_waits==2 && now_ms==200 && atomic_load(&cancel));
        assert(atomic_load(&c->error)==CHANNEL_OK && !hello_calls && !receives); break;
    case DISCONNECT_SUBACK:
        assert(suback_waits==2 && now_ms==200 && atomic_load(&c->failed));
        assert(atomic_load(&c->error)==CHANNEL_CONNECTION && !hello_calls && !receives); break;
    default: abort();
    }
    if(init_calls && scenario!=INIT_FAIL) {
        assert(!cloud && live[JSON_ALLOC]==0);
        if(start_calls) assert(pre_ready_data>=1);
    }
    if(!success && scenario!=DISCONNECT_HELLO_PARSE && scenario!=38) assert(!sockets);
    if(scenario==SUCCESS && detail==99)outcome=SESSION_RENEW;
    unsigned at_close=publish_calls;
    release_channel(c); cJSON_Delete(cloud);
    if(scenario==SUCCESS)assert(publish_calls==at_close+(outcome==SESSION_RENEW?0:1));
    assert(aes_frees==1 && closes==sockets);
    if(init_calls && scenario!=INIT_FAIL) {
        assert(stops==1 && destroys==1);
        if(register_calls && scenario!=REGISTER_FAIL) assert(stopped_data==1);
    } else assert(!stops && !destroys);
    check_no_live_allocations();
    printf("channel scenario=%u detail=%u: ordering, bounded waits and caller cleanup PASS\n",scenario,detail);
}
'''


class ChannelMemoryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        component = ROOT / "components/platform/xiaozhi"
        production = (component / "xiaozhi.c").read_text()
        channel = re.search(
            r"typedef struct \{\s+esp_mqtt_client_handle_t mqtt;[\s\S]*?\} channel_t;",
            production,
        )
        errors = re.search(r"enum \{ CHANNEL_OK[\s\S]*?\};", production)
        if not channel or not errors:
            raise AssertionError("Production channel type/error enum missing")
        run = function(production, "run_session")
        finish = run.index("finish:")
        cleanup = block(run, run.index("if (c) {", finish))
        if "CHANNEL_CONTROL_PARSE" not in errors.group():
            errors = re.match(r"[\s\S]*", errors.group().replace(" };", ", CHANNEL_CONTROL_PARSE };"))
        functions = "\n".join(function(production, name) for name in (
            "channel_take", "channel_discard_partial", "channel_push", "channel_clear",
            "mqtt_error_kind", "channel_error_text", "mqtt_event", "string_field",
            "send_text", "command", "hello", "open_channel", "channel_release",
        ))
        message = re.search(r"typedef struct channel_message \{[\s\S]*?\} channel_message_t;", production)
        if not message: raise AssertionError("Owned channel message type missing")
        code = (BOUNDARY + message.group() + channel.group() + errors.group() + FAKES + functions
                + "\nenum { SESSION_CLOSED, SESSION_RENEW }; static int outcome;\nstatic void release_channel(channel_t *c) {\n" + cleanup + "\n}\n" + CASES)
        idf_path = os.environ.get("IDF_PATH")
        description = ROOT / "build/project_description.json"
        if not idf_path and description.exists():
            idf_path = json.loads(description.read_text())["idf_path"]
        if not idf_path:
            raise RuntimeError("IDF_PATH or build/project_description.json is required for real cJSON")
        cjson = Path(idf_path) / "components/json/cJSON"
        cls.directory = tempfile.TemporaryDirectory(prefix="passport-channel-")
        cls.addClassCleanup(cls.directory.cleanup)
        source = Path(cls.directory.name) / "channel.c"
        source.write_text(code)
        cls.executable = Path(cls.directory.name) / "channel"
        result = subprocess.run([
            "cc", "-std=c11", "-UNDEBUG", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=undefined", "-fno-sanitize-recover=all",
            "-I", str(component), "-I", str(cjson), str(source),
            str(component / "xz_protocol.c"), str(cjson / "cJSON.c"),
            "-o", str(cls.executable),
        ], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def case(self, scenario, detail=0):
        result = subprocess.run(
            [str(self.executable), str(scenario), str(detail)],
            capture_output=True, text=True, timeout=5,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_server_goodbye_is_not_echoed(self):
        self.case(0,99)

    def test_ota_json_freed_before_tls_and_control_buffers_deferred_until_connected(self):
        self.case(0)
        self.case(1)

    def test_first_mcp_delivered_immediately_after_connect_and_ignored_after_close(self):
        self.case(20)

    def test_invalid_configuration_remains_caller_owned(self):
        for detail in range(7):
            with self.subTest(detail=detail):
                self.case(2, detail)

    def test_init_register_and_start_errors_cleanup_both_ownership_states(self):
        self.case(3)
        for scenario in (4, 5):
            for detail in (0, 1):
                with self.subTest(scenario=scenario, memory=bool(detail)):
                    self.case(scenario, detail)

    def test_connection_timeout_and_cancellation_are_bounded_without_control_allocations(self):
        self.case(6)
        self.case(7)
        self.case(9)

    def test_tls_memory_auth_certificate_and_server_error_classification(self):
        for detail in range(14):
            with self.subTest(detail=detail):
                self.case(8, detail)

    def test_frame_allocation_failure_is_cleaned_by_actual_caller(self):
        for detail in (1,):
            with self.subTest(allocation=detail):
                self.case(10, detail)

    def test_subscribe_publish_and_hello_failures_are_bounded_and_clean(self):
        for scenario in (11, 12, 13, 14, 15, 16, 19):
            with self.subTest(scenario=scenario):
                self.case(scenario)

    def test_real_cjson_oom_and_malformed_hello_fail_explicitly_and_release_owned_frame(self):
        for allocation in (1,2,3,6,10,15):
            with self.subTest(allocation=allocation): self.case(36,allocation)
        self.case(37)

    def test_owned_frames_preserve_maximum_length_fragment_rules_byte_quota_and_cancel_cleanup(self):
        for scenario in (32,33,34,35):
            with self.subTest(scenario=scenario): self.case(scenario)

    def test_control_messages_do_not_reserve_three_maximum_frames_for_a_small_hello(self):
        self.case(31)

    def test_disconnect_during_alloc_or_hello_parse_preserves_error_and_caller_cleanup(self):
        self.case(29)
        self.case(30)

    def test_late_connected_event_cannot_revive_an_already_failed_channel(self):
        self.case(27)

    def test_explicit_subscription_is_acknowledged_before_hello(self):
        self.case(21)  # Delayed SUBACK; publishing earlier can lose the reply.
        self.case(26)  # Wrong message ID must not release the hello gate.
        self.case(28)  # Servers without subscribe_topic keep their original flow.

    def test_suback_rejection_timeout_disconnect_and_cancellation_do_not_send_hello(self):
        for scenario in (22, 23, 24, 25):
            with self.subTest(scenario=scenario): self.case(scenario)

    def test_real_fragments_and_cjson_hello_release_transferred_frames(self):
        self.case(17)
        self.case(18)

    def test_udp_receive_budget_and_timeout_failures_release_the_owned_socket(self):
        self.case(38, 0)
        self.case(38, 1)


if __name__ == "__main__":
    unittest.main()
