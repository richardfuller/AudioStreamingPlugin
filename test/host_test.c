/* Loads the plugin like the host would, pumps a sine through process_block
 * at real-time pace, connects as an HTTP client and validates the stream. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <math.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "schwung/audio_fx_api_v2.h"

#define FAIL(...) do { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)

int main(int argc, char **argv)
{
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) FAIL("dlopen: %s", dlerror());
    audio_fx_init_v2_fn init = (audio_fx_init_v2_fn)dlsym(h, AUDIO_FX_INIT_V2_SYMBOL);
    if (!init) FAIL("no entry symbol");
    host_api_v1_t host = { .api_version = 1, .sample_rate = 44100, .frames_per_block = 128 };
    audio_fx_api_v2_t *api = init(&host);
    void *inst = api->create_instance(".", "{\"port\": 18800}");
    if (!inst) FAIL("create_instance");
    usleep(100000);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(18800) };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) FAIL("connect");
    const char req[] = "GET / HTTP/1.0\r\n\r\n";
    if (write(fd, req, sizeof req - 1) < 0) FAIL("write");

    static uint8_t rx[400000]; size_t got = 0;
    int16_t blk[256]; double ph = 0;
    for (int b = 0; b < 800; b++) {                  /* ~2.3 s of audio */
        for (int i = 0; i < 128; i++) {
            int16_t v = (int16_t)(8000 * sin(ph)); ph += 2 * M_PI * 440 / 44100;
            blk[i * 2] = v; blk[i * 2 + 1] = (int16_t)-v;
        }
        int16_t orig[256]; memcpy(orig, blk, sizeof blk);
        api->process_block(inst, blk, 128);
        if (memcmp(orig, blk, sizeof blk)) FAIL("audio was modified");
        usleep(2900);
        struct timeval tv = {0, 0};
        fd_set fs; FD_ZERO(&fs); FD_SET(fd, &fs);
        while (select(fd + 1, &fs, NULL, NULL, &tv) > 0 && got < sizeof rx) {
            ssize_t n = read(fd, rx + got, sizeof rx - got);
            if (n <= 0) break;
            got += (size_t)n;
            FD_SET(fd, &fs);
        }
    }
    char st[64]; api->get_param(inst, "status", st, sizeof st);
    printf("received %zu bytes, status=%s\n", got, st);
    if (got < 100000) FAIL("too little data");
    if (strncmp((char *)rx, "HTTP/1.0 200 OK", 15)) FAIL("bad HTTP status");
    uint8_t *w = memmem(rx, got, "RIFF", 4);
    if (!w || memcmp(w + 8, "WAVEfmt ", 8)) FAIL("no WAV header");
    int16_t *pcm = (int16_t *)(w + 44);
    size_t nfr = (got - (size_t)(w + 44 - rx)) / 4, mism = 0;
    for (size_t i = 1; i < nfr; i++)           /* L == -R and non-silent */
        if (pcm[i * 2] != -pcm[i * 2 + 1]) mism++;
    if (mism) FAIL("%zu stereo mismatches (stream misaligned?)", mism);
    int peak = 0; for (size_t i = 0; i < nfr; i++) if (abs(pcm[i*2]) > peak) peak = abs(pcm[i*2]);
    if (peak < 7000) FAIL("signal too quiet: %d", peak);
    api->set_param(inst, "enabled", "0");
    close(fd);
    api->destroy_instance(inst);
    puts("PASS");
    return 0;
}
