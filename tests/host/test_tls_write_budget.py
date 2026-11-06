"""Exercise the installed ESP-TLS writer with the reduced outgoing record cap."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

from test_ble_advertising import function


FAKES = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>
#include <stdio.h>
#define MBEDTLS_SSL_OUT_CONTENT_LEN 1024
#define ESP_TLS_ERR_SSL_WANT_READ -0x6900
#define ESP_TLS_ERR_SSL_WANT_WRITE -0x6880
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_INT_EVENT_TRACKER_CAPTURE(...) (errors++)
typedef struct { int ssl; void *error_handle; } esp_tls_t;
static unsigned calls, scenario, errors;
static size_t stored, largest;
static unsigned char received[6000];
static void mbedtls_print_error_msg(int error) { (void)error; }
static int mbedtls_ssl_write(int *ssl, const unsigned char *data, size_t length) {
    (void)ssl; calls++;
    assert(length && length <= MBEDTLS_SSL_OUT_CONTENT_LEN);
    if (length > largest) largest = length;
    if (scenario == 2 && calls == 2) { scenario = 0; return ESP_TLS_ERR_SSL_WANT_WRITE; }
    if (scenario == 3 && calls == 1) return ESP_TLS_ERR_SSL_WANT_READ;
    if (scenario == 4) return -0x7200;
    if (scenario == 1 && length > 17) length = 17;
    assert(stored + length <= sizeof received);
    memcpy(received + stored, data, length); stored += length;
    return (int)length;
}
'''
CASES = r'''
static void reset(unsigned value) { calls=errors=0; stored=largest=0; scenario=value; }
int main(void) {
    esp_tls_t tls={0}; unsigned char data[5003];
    for (unsigned i=0; i<sizeof data; i++) data[i]=(unsigned char)(i%251);
    reset(0);
    assert(esp_mbedtls_write(&tls,(char *)data,sizeof data)==sizeof data);
    assert(calls==5 && largest==1024 && !memcmp(data,received,sizeof data));
    reset(1);
    assert(esp_mbedtls_write(&tls,(char *)data,sizeof data)==sizeof data);
    assert(calls>5 && !memcmp(data,received,sizeof data));
    reset(2);
    ssize_t first=esp_mbedtls_write(&tls,(char *)data,sizeof data);
    assert(first==1024 && !errors);
    ssize_t remaining=(ssize_t)sizeof data-first;
    assert(esp_mbedtls_write(&tls,(char *)data+first,(size_t)remaining)==remaining);
    assert(stored==sizeof data && !memcmp(data,received,sizeof data));
    reset(3);
    assert(esp_mbedtls_write(&tls,(char *)data,sizeof data)==ESP_TLS_ERR_SSL_WANT_READ);
    assert(!stored && !errors);
    reset(4);
    assert(esp_mbedtls_write(&tls,(char *)data,sizeof data)==-0x7200);
    assert(!stored && errors==2);
    puts("ESP-TLS 1KiB writes: full payload, short writes, resumable retry and terminal errors PASS");
}
'''


class TlsWriteBudgetTest(unittest.TestCase):
    def test_sdk_fragments_without_truncating_or_duplicating(self):
        sdk = Path(os.environ.get('IDF_PATH', str(Path.home() / 'esp/esp-idf-v5.5.3')))
        source = (sdk / 'components/esp-tls/esp_tls_mbedtls.c').read_text()
        actual = function(source, 'esp_mbedtls_write')
        with tempfile.TemporaryDirectory(prefix='passport-tls-write-') as folder:
            c, executable = Path(folder) / 'write.c', Path(folder) / 'write'
            c.write_text(FAKES + actual + CASES)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', '-fno-sanitize-recover=all',
                            str(c), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
