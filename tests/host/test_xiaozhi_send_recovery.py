"""Replay the observed UDP ENOMEM against the production encrypted sender."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_xz_channel_memory import function

ROOT = Path(__file__).resolve().parents[2]

BOUNDARY = r'''
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#define XZ_PACKET_MAX 1275
static void record(const char *format, ...) { (void)format; }
#define ESP_LOGI(tag, ...) record(__VA_ARGS__)
#define ESP_LOGW(tag, ...) record(__VA_ARGS__)
#define pdMS_TO_TICKS(value) (value)
typedef struct {
  int udp;
  uint8_t nonce[16];
  uint32_t tx_sequence, tx_retries, tx_dropped;
} channel_t;
static atomic_bool cancel, requested_end;
static unsigned calls, crypt_calls, elapsed, cancel_on_delay;
static int failures[3];
static bool crypto_fails, partial;
static uint8_t first_wire[XZ_PACKET_MAX + 16];
static size_t first_size;
static uint64_t ms(void) { return elapsed; }
static void vTaskDelay(unsigned milliseconds) {
  assert(milliseconds == 10);
  elapsed += milliseconds;
  if (cancel_on_delay == 1) cancel = true;
  if (cancel_on_delay == 2) requested_end = true;
}
static bool audio_crypt(channel_t *channel, const uint8_t *input, size_t size,
                        const uint8_t nonce[16], uint8_t *output) {
  (void)channel; (void)nonce;
  ++crypt_calls;
  if (crypto_fails) return false;
  for (size_t i = 0; i < size; ++i) output[i] = input[i] ^ 0x5a;
  return true;
}
static int fake_send(int fd, const void *data, size_t size, int flags) {
  assert(fd == 7 && flags == 0 && size <= sizeof first_wire && calls < 3);
  if (!calls) {
    memcpy(first_wire, data, size);
    first_size = size;
  } else {
    assert(size == first_size && memcmp(first_wire, data, size) == 0);
  }
  int error = failures[calls++];
  if (error) { errno = error; return -1; }
  return partial ? (int)size - 1 : (int)size;
}
#define send fake_send
static void reset(void) {
  calls = crypt_calls = elapsed = cancel_on_delay = 0;
  crypto_fails = partial = false;
  cancel = requested_end = false;
  memset(failures, 0, sizeof failures);
}
'''

CASES = r'''
static bool frame(channel_t *channel) {
  uint8_t packet[XZ_PACKET_MAX + 16];
  memset(packet, 0x33, sizeof packet);
  return send_audio(channel, packet + 16, 120, packet);
}
int main(void) {
  channel_t channel = {.udp = 7};
  reset(); assert(frame(&channel));
  assert(calls == 1 && crypt_calls == 1 && !elapsed && channel.tx_sequence == 1);
  const int transient[] = {ENOMEM, ENOBUFS, EAGAIN, EWOULDBLOCK, EINTR};
  for (unsigned i = 0; i < sizeof transient / sizeof transient[0]; ++i) {
    channel = (channel_t){.udp = 7}; reset(); failures[0] = transient[i];
    assert(frame(&channel));
    assert(calls == 2 && crypt_calls == 1 && elapsed == 10);
    assert(channel.tx_sequence == 1 && channel.tx_retries == 1 && !channel.tx_dropped);
    channel = (channel_t){.udp = 7}; reset();
    failures[0] = failures[1] = failures[2] = transient[i];
    assert(frame(&channel));
    assert(calls == 3 && crypt_calls == 1 && elapsed == 20 && channel.tx_dropped == 1);
    assert(channel.tx_sequence == 1);
    reset(); assert(frame(&channel));
    uint32_t sequence; memcpy(&sequence, first_wire + 12, sizeof sequence);
    assert(ntohl(sequence) == 2 && calls == 1 && !elapsed && channel.tx_dropped == 1);
  }
  channel = (channel_t){.udp = 7}; reset(); failures[0] = ENOMEM; failures[1] = ENOBUFS;
  assert(frame(&channel) && calls == 3 && elapsed == 20 && channel.tx_retries == 2);
  const int permanent[] = {EBADF, EINVAL, ENETDOWN, ENETUNREACH, EHOSTUNREACH, ECONNREFUSED};
  for (unsigned i = 0; i < sizeof permanent / sizeof permanent[0]; ++i) {
    channel = (channel_t){.udp = 7}; reset(); failures[0] = permanent[i];
    assert(!frame(&channel) && calls == 1 && !elapsed && !channel.tx_dropped);
  }
  reset(); failures[0] = ENOMEM; failures[1] = EBADF;
  assert(!frame(&channel) && calls == 2 && elapsed == 10);
  reset(); partial = true;
  assert(!frame(&channel) && calls == 1 && !elapsed);
  reset(); crypto_fails = true;
  assert(!frame(&channel) && !calls && crypt_calls == 1);
  for (unsigned stop = 1; stop <= 2; ++stop) {
    reset(); failures[0] = ENOMEM; cancel_on_delay = stop;
    assert(frame(&channel) && calls == 1 && elapsed == 10);
  }
  uint8_t packet[XZ_PACKET_MAX + 16]; reset();
  assert(!send_audio(&channel, packet + 16, 0, packet));
  assert(!send_audio(&channel, packet + 16, XZ_PACKET_MAX + 1, packet));
  assert(!calls && !crypt_calls);
  puts("UDP sender: observed ENOMEM, unchanged retry bytes/nonce, 20ms bound, congestion drop, next frame, cancel and permanent errors PASS");
}
'''


class VoiceSendRecoveryTest(unittest.TestCase):
    def test_transient_send_pressure_keeps_the_conversation_available(self):
        source = (ROOT / "components/platform/xiaozhi/xiaozhi.c").read_text()
        with tempfile.TemporaryDirectory(prefix="passport-udp-recovery-") as directory:
            path = Path(directory)
            program = path / "send.c"
            executable = path / "send"
            program.write_text(BOUNDARY + function(source, "send_audio") + CASES)
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                str(program), "-o", str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
