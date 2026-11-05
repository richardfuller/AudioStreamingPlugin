/* Host-side tests: load the plugin like Schwung would (dlopen + entry symbol),
 * pump audio through process_block at real-time pace, and exercise the HTTP
 * server as a client. Linux only (uses sockets/dlopen directly). */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "schwung/audio_fx_api_v2.h"

#define FAIL(...) do { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)
#define CHECK(c, ...) do { if (!(c)) FAIL(__VA_ARGS__); } while (0)
#define PASS(name) printf("  ok  %s\n", name)

static audio_fx_api_v2_t *api;
static double ph;

static void pump(void *inst, int blocks)
{
    int16_t blk[256];
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < 128; i++) {
            int16_t v = (int16_t)(8000 * sin(ph)); ph += 2 * M_PI * 440 / 44100;
            blk[i * 2] = v; blk[i * 2 + 1] = (int16_t)-v;
        }
        int16_t orig[256]; memcpy(orig, blk, sizeof blk);
        api->process_block(inst, blk, 128);
        CHECK(!memcmp(orig, blk, sizeof blk), "audio was modified");
        usleep(2900);
    }
}

static int dial(int port, int rcvbuf)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (rcvbuf) setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return -1; }
    return fd;
}

static void sendreq(int fd, const char *r) { CHECK(write(fd, r, strlen(r)) > 0, "write"); }

/* Read whatever arrives within ms. Returns bytes (0 = nothing yet), -1 on EOF. */
static ssize_t slurp(int fd, uint8_t *buf, size_t cap, int ms, size_t *got)
{
    struct pollfd p = { fd, POLLIN, 0 };
    ssize_t last = 0;
    while (poll(&p, 1, ms) > 0 && *got < cap) {
        ssize_t n = read(fd, buf + *got, cap - *got);
        if (n == 0) return -1;
        if (n < 0) return -1;
        *got += (size_t)n; last = n;
    }
    return last;
}

static int get_int(void *inst, const char *k)
{
    char b[64] = "-1"; api->get_param(inst, k, b, sizeof b); return atoi(b);
}
static void status(void *inst, char *b, size_t n) { strcpy(b, "?"); api->get_param(inst, "status", b, (int)n); }

static int expect_http(int port, const char *req, const char *prefix)
{
    int fd = dial(port, 0);
    CHECK(fd >= 0, "connect %d", port);
    sendreq(fd, req);
    uint8_t b[256]; size_t got = 0;
    slurp(fd, b, sizeof b - 1, 500, &got);
    b[got] = 0;
    close(fd);
    CHECK(got >= strlen(prefix) && !strncmp((char *)b, prefix, strlen(prefix)),
          "'%s' -> '%.40s' (wanted %s)", req, b, prefix);
    return 0;
}

/* Stream correctness: HTTP 200 + WAV header + aligned stereo L==-R sine. */
static void test_stream_and_passthrough(void *inst, int port)
{
    int fd = dial(port, 0);
    CHECK(fd >= 0, "connect");
    sendreq(fd, "GET / HTTP/1.0\r\n\r\n");
    static uint8_t rx[400000]; size_t got = 0;
    for (int i = 0; i < 800; i++) { pump(inst, 1); slurp(fd, rx, sizeof rx, 0, &got); }
    CHECK(got > 100000, "too little data (%zu)", got);
    CHECK(!strncmp((char *)rx, "HTTP/1.0 200 OK", 15), "bad status");
    uint8_t *w = memmem(rx, got, "RIFF", 4);
    CHECK(w && !memcmp(w + 8, "WAVEfmt ", 8), "no WAV header");
    int16_t *pcm = (int16_t *)(w + 44);
    size_t nfr = (got - (size_t)(w + 44 - rx)) / 4, mism = 0; int peak = 0;
    for (size_t i = 0; i < nfr; i++) {
        if (pcm[i * 2] != -pcm[i * 2 + 1]) mism++;
        if (abs(pcm[i * 2]) > peak) peak = abs(pcm[i * 2]);
    }
    CHECK(!mism, "%zu stereo mismatches", mism);
    CHECK(peak > 7000, "quiet: %d", peak);
    close(fd);
    PASS("stream is valid, aligned, audio passes through unmodified");
}

static void test_request_parsing(void *inst, int port)
{
    (void)inst;
    expect_http(port, "POST /x HTTP/1.0\r\n\r\n", "HTTP/1.0 405");
    expect_http(port, "GET /foo HTTP/1.0\r\n\r\n", "HTTP/1.0 404");
    expect_http(port, "GARBAGE\r\n\r\n", "HTTP/1.0 400");
    expect_http(port, "GET /?x=1 HTTP/1.0\r\n\r\n", "HTTP/1.0 200");
    PASS("only GET / is served (405/404/400)");
}

static void test_busy_503(void *inst, int port)
{
    int fds[4];
    for (int i = 0; i < 4; i++) {
        fds[i] = dial(port, 0); CHECK(fds[i] >= 0, "connect %d", i);
        sendreq(fds[i], "GET / HTTP/1.0\r\n\r\n");
    }
    for (int i = 0; i < 30; i++) pump(inst, 1);
    CHECK(get_int(inst, "clients") == 4, "expected 4 clients, got %d", get_int(inst, "clients"));
    expect_http(port, "GET / HTTP/1.0\r\n\r\n", "HTTP/1.0 503");
    for (int i = 0; i < 4; i++) close(fds[i]);
    for (int i = 0; i < 60; i++) pump(inst, 1);
    CHECK(get_int(inst, "clients") == 0, "clients not released: %d", get_int(inst, "clients"));
    PASS("5th client gets 503 Busy; slots free up on disconnect");
}

static void test_stall_eviction_and_drops(void *inst, int port)
{
    /* Low latency + tiny receive window + never reading: backlog overflows
     * (drop-to-latest), then the stall timeout evicts the client. */
    char v[4]; (void)v;
    api->set_param(inst, "latency", "1");
    int fd = dial(port, 2048);
    sendreq(fd, "GET / HTTP/1.0\r\n\r\n");
    unsigned d0 = (unsigned)get_int(inst, "drops");
    for (int i = 0; i < 150; i++) pump(inst, 1);          /* ~0.45 s: < stall_ms */
    CHECK(get_int(inst, "clients") == 1, "client dropped too early");
    CHECK((unsigned)get_int(inst, "drops") > d0, "no drop-to-latest recorded");
    PASS("slow client drops to newest audio and is counted in drops");
    for (int i = 0; i < 500; i++) pump(inst, 1);          /* now exceed stall_ms (1000) */
    CHECK(get_int(inst, "clients") == 0, "stalled client not evicted");
    close(fd);
    api->set_param(inst, "latency", "0");
    PASS("stalled client is evicted after stall_ms");
}

static void test_silent_request_timeout(void *inst, int port)
{
    int fd = dial(port, 0);              /* connects, never sends a request */
    uint8_t b[128]; size_t got = 0;
    for (int i = 0; i < 6000 / 100; i++) { usleep(100000); (void)inst; slurp(fd, b, sizeof b, 0, &got); }
    CHECK(got > 0 && !strncmp((char *)b, "HTTP/1.0 408", 12), "no 408 for silent client");
    close(fd);
    PASS("silent connection is closed with 408");
}

static void test_disable_closes_clients(void *inst, int port)
{
    int fd = dial(port, 0);
    sendreq(fd, "GET / HTTP/1.0\r\n\r\n");
    for (int i = 0; i < 50; i++) pump(inst, 1);
    api->set_param(inst, "enabled", "0");
    usleep(300000);
    static uint8_t b[400000]; size_t got = 0;
    ssize_t r;
    do { r = slurp(fd, b, sizeof b, 200, &got); } while (r > 0);
    CHECK(r == -1, "client not closed when disabled");
    char st[64]; status(inst, st, sizeof st);
    CHECK(!strcmp(st, "OFF"), "status '%s' != OFF", st);
    CHECK(dial(port, 0) < 0, "listener still open while disabled");
    api->set_param(inst, "enabled", "On");
    usleep(300000);
    int fd2 = dial(port, 0);
    CHECK(fd2 >= 0, "did not re-listen after re-enable");
    close(fd); close(fd2);
    PASS("enabled=0 closes clients+listener (status OFF); re-enable works");
}

static void test_rebind(void *inst, int port)
{
    int fd = dial(port, 0);
    sendreq(fd, "GET / HTTP/1.0\r\n\r\n");
    for (int i = 0; i < 50; i++) pump(inst, 1);
    api->set_param(inst, "port", "18860");
    usleep(500000);
    CHECK(get_int(inst, "bound_port") == 18860, "bound_port %d", get_int(inst, "bound_port"));
    static uint8_t b[400000]; size_t got = 0; ssize_t r;
    do { r = slurp(fd, b, sizeof b, 200, &got); } while (r > 0);
    CHECK(r == -1, "old client survived rebind");
    CHECK(dial(port, 0) < 0, "old port still open");
    int fd2 = dial(18860, 0); CHECK(fd2 >= 0, "new port not open");
    close(fd); close(fd2);
    api->set_param(inst, "port", "18800");
    usleep(500000);
    PASS("port is settable at runtime (rebind)");
}


int main(int argc, char **argv)
{
    CHECK(argc > 1, "usage: host_test <plugin.so>");
    void *h = dlopen(argv[1], RTLD_NOW);
    CHECK(h, "dlopen: %s", dlerror());
    audio_fx_init_v2_fn init = (audio_fx_init_v2_fn)dlsym(h, AUDIO_FX_INIT_V2_SYMBOL);
    CHECK(init, "no entry symbol");
    host_api_v1_t host = { .api_version = 1, .sample_rate = 44100, .frames_per_block = 128 };
    api = init(&host);

    /* Metadata for Move's UI */
    void *inst = api->create_instance(".", "{\"port\": 18800, \"stall_ms\": 1000}");
    CHECK(inst, "create_instance");
    usleep(200000);
    static char big[8192];
    int n = api->get_param(inst, "chain_params", big, sizeof big);
    CHECK(n > 100 && big[0] == '[' && big[n - 1] == ']', "chain_params");
    n = api->get_param(inst, "ui_hierarchy", big, sizeof big);
    CHECK(n > 100 && strstr(big, "\"levels\"") && strstr(big, "\"knobs\""), "ui_hierarchy");
    PASS("chain_params + ui_hierarchy served");

    test_stream_and_passthrough(inst, 18800);
    test_request_parsing(inst, 18800);
    test_busy_503(inst, 18800);
    test_stall_eviction_and_drops(inst, 18800);
    test_silent_request_timeout(inst, 18800);
    test_disable_closes_clients(inst, 18800);
    test_rebind(inst, 18800);

    /* Port conflicts: a squatter on 18900. */
    int sq = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(sq, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY),
                             .sin_port = htons(18900) };
    CHECK(!bind(sq, (struct sockaddr *)&a, sizeof a) && !listen(sq, 1), "squatter bind");

    /* (a) span 16: second instance moves to the next free port */
    void *i2 = api->create_instance(".", "{\"port\": 18900}");
    usleep(300000);
    CHECK(get_int(i2, "bound_port") == 18901, "auto-increment got %d", get_int(i2, "bound_port"));
    char st[64]; status(i2, st, sizeof st);
    CHECK(strstr(st, ":18901"), "status '%s'", st);
    PASS("busy port -> next free port, status shows the real one");

    /* (b) span 1: reports 'IN USE', then recovers once the port frees */
    void *i3 = api->create_instance(".", "{\"port\": 18900, \"port_span\": 1}");
    usleep(300000);
    status(i3, st, sizeof st);
    CHECK(strstr(st, "IN USE"), "status '%s' lacks IN USE", st);
    close(sq);
    usleep(1500000);
    CHECK(get_int(i3, "bound_port") == 18900, "no retry after port freed (%d)", get_int(i3, "bound_port"));
    PASS("bind failure reported as PORT IN USE and retried until it succeeds");

    api->destroy_instance(i3);
    api->destroy_instance(i2);
    api->destroy_instance(inst);
    puts("ALL PASS");
    return 0;
}
