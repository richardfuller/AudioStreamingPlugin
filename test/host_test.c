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
#include <sys/mman.h>
#include "schwung/audio_fx_api_v2.h"
#include "schwung/audio_live_shm.h"

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



static void test_ipv6(int port)
{
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) { puts("  skip IPv6 (no AF_INET6)"); return; }
    struct sockaddr_in6 a; memset(&a, 0, sizeof a);
    a.sin6_family = AF_INET6; a.sin6_port = htons((uint16_t)port); inet_pton(AF_INET6, "::1", &a.sin6_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); puts("  skip IPv6 (::1 unreachable here)"); return; }
    sendreq(fd, "GET / HTTP/1.0\r\n\r\n");
    uint8_t b[64]; size_t got = 0; slurp(fd, b, sizeof b - 1, 500, &got);
    CHECK(got >= 15 && !strncmp((char *)b, "HTTP/1.0 200 OK", 15), "no 200 over IPv6");
    close(fd);
    PASS("listens on IPv6 (::1) as well as IPv4");
}

/* Writer thread-free fake of Schwung's shim: push one block per call. */
static audio_live_shm_t *tap_create(void)
{
    int fd = open(AUDIO_LIVE_SHM_PATH, O_CREAT | O_RDWR | O_TRUNC, 0666);
    CHECK(fd >= 0 && !ftruncate(fd, sizeof(audio_live_shm_t)), "tap file");
    audio_live_shm_t *m = mmap(NULL, sizeof *m, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(m != MAP_FAILED, "tap mmap");
    close(fd);
    audio_live_init(m);
    return m;
}

static double tph;
static void tap_pump(audio_live_shm_t *m, void *inst, int blocks, int fx_too)
{
    int16_t blk[256];
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < 128; i++) {                     /* tap signal: L == R */
            int16_t v = (int16_t)(6000 * sin(tph)); tph += 2 * M_PI * 880 / 44100;
            blk[i * 2] = v; blk[i * 2 + 1] = v;
        }
        if (m) audio_live_push(m, blk, 128, 1.0f);
        if (fx_too) { int16_t f[256]; for (int i = 0; i < 128; i++) { f[i*2] = 100; f[i*2+1] = -100; } api->process_block(inst, f, 128); }
        usleep(2900);
    }
}

/* Classify what a stream carries: 'T' tap (L==R, ~6000), 'F' FX (L==-R). */
static char classify(const uint8_t *rx, size_t got)
{
    const uint8_t *w = memmem(rx, got, "RIFF", 4);
    if (!w || got < (size_t)(w - rx) + 44 + 4000) return '?';
    const int16_t *pcm = (const int16_t *)(w + 44) + 1000;     /* skip start-up */
    size_t nfr = (got - (size_t)((const uint8_t *)pcm - rx)) / 4;
    int tap = 0, fx = 0;
    for (size_t i = 0; i < nfr; i++) {
        if (pcm[i*2] == pcm[i*2+1] && abs(pcm[i*2]) > 1000) tap++;
        if (pcm[i*2] == 100 && pcm[i*2+1] == -100) fx++;
    }
    return tap > fx * 4 ? 'T' : fx > tap * 4 ? 'F' : '?';
}

/* One fresh connection per phase, so each capture starts at a WAV header. */
static char capture_phase(void *inst, audio_live_shm_t *m, int blocks, int fx_too)
{
    int fd = dial(18950, 0);
    CHECK(fd >= 0, "connect tap port");
    sendreq(fd, "GET / HTTP/1.0\r\n\r\n");
    static uint8_t rx[300000]; size_t got = 0;
    for (int i = 0; i < blocks; i++) { tap_pump(m, inst, 1, fx_too); slurp(fd, rx, sizeof rx, 0, &got); }
    close(fd);
    return classify(rx, got);
}

static void test_tap_source(void)
{
    void *inst = api->create_instance(".", "{\"port\": 18950}");
    usleep(200000);
    char st[64];

    /* 1. No capture ring: Auto falls back to the FX input. */
    char c = capture_phase(inst, NULL, 400, 1);
    status(inst, st, sizeof st);
    CHECK(c == 'F' && strstr(st, " FX"), "no-tap fallback: %c '%s'", c, st);
    PASS("Auto falls back to FX input when no capture ring exists");

    /* 2. Ring appears and advances: Auto switches to the Move mix. */
    audio_live_shm_t *m = tap_create();
    for (int i = 0; i < 1000; i++) tap_pump(m, inst, 1, 1);          /* let the 2 s probe find it */
    status(inst, st, sizeof st);
    CHECK(strstr(st, "MIX"), "did not switch to tap: '%s'", st);
    c = capture_phase(inst, m, 400, 1);
    CHECK(c == 'T', "stream is not the tap signal (%c)", c);
    PASS("Auto switches to Move mix tap once the ring is live (aligned, gapless)");

    /* 3. Forced FX input. */
    api->set_param(inst, "source", "1");
    usleep(100000);
    c = capture_phase(inst, m, 400, 1);
    CHECK(c == 'F', "FX input not selected (%c)", c);
    PASS("source=FX in forces the slot audio even while the tap is live");

    /* 4. Forced Mix, ring goes silent: reports NO MIX TAP. */
    api->set_param(inst, "source", "2");
    for (int i = 0; i < 400; i++) tap_pump(NULL, inst, 1, 1);        /* ring stops advancing */
    status(inst, st, sizeof st);
    CHECK(strstr(st, "NO MIX TAP") || !strstr(st, "MIX "), "status '%s'", st);
    int fd = dial(18950, 0); sendreq(fd, "GET / HTTP/1.0\r\n\r\n");
    for (int i = 0; i < 100; i++) tap_pump(NULL, inst, 1, 1);
    status(inst, st, sizeof st);
    CHECK(strstr(st, "NO MIX TAP"), "status '%s'", st);
    close(fd);
    PASS("source=Mix with a dead ring says NO MIX TAP");
    api->destroy_instance(inst);
    munmap(m, sizeof *m);
    unlink(AUDIO_LIVE_SHM_PATH);
}


/* Read an HTTP reply to EOF (for /status and /log). */
static size_t http_get_all(int port, const char *path, char *out, size_t cap)
{
    int fd = dial(port, 0);
    CHECK(fd >= 0, "connect for %s", path);
    char req[128]; snprintf(req, sizeof req, "GET %s HTTP/1.0\r\n\r\n", path);
    sendreq(fd, req);
    size_t got = 0; ssize_t r;
    do { r = slurp(fd, (uint8_t *)out, cap - 1, 500, &got); } while (r > 0);
    out[got] = 0;
    close(fd);
    return got;
}

static void test_status_endpoints(void *inst, int port)
{
    static char b[16384];
    for (int i = 0; i < 400; i++) pump(inst, 1);            /* feed a 1 s meter window */
    http_get_all(port, "/status", b, sizeof b);
    CHECK(!strncmp(b, "HTTP/1.0 200", 12), "status: %.40s", b);
    CHECK(strstr(b, "application/json") && strstr(b, "\"module\":\"audio-stream\""), "status body");
    CHECK(strstr(b, "\"source_in_use\":\"fx\"") && strstr(b, "\"tap\":{\"present\":false"), "status source/tap: %s", b);
    CHECK(strstr(b, "Mirror Display"), "no hint about the display mirror");
    CHECK(strstr(b, "\"log\":[\"") && strstr(b, "listening on port"), "log lines missing from status");
    CHECK(strstr(b, "]}\n"), "status JSON not terminated");
    CHECK(strstr(b, "\"peak_in\":0.2") != NULL || strstr(b, "\"peak_in\":0.24") != NULL, "peak meter: %s", strstr(b, "level_1s"));
    http_get_all(port, "/log", b, sizeof b);
    CHECK(strstr(b, "text/plain") && strstr(b, "worker started"), "/log body");
    CHECK(get_int(inst, "clients") == 0, "status request used a stream slot");
    PASS("GET /status (JSON: source, tap, level, log) and GET /log work without using a stream slot");
}

static int stream_peak(const uint8_t *rx, size_t got)
{
    const uint8_t *w = memmem(rx, got, "RIFF", 4);
    if (!w || got < (size_t)(w - rx) + 44 + 8000) return -1;
    const int16_t *pcm = (const int16_t *)(w + 44) + 2000;
    size_t nfr = (got - (size_t)((const uint8_t *)pcm - rx)) / 4;
    int pk = 0;
    for (size_t i = 0; i < nfr * 2; i++) { int a = abs(pcm[i]); if (a > pk) pk = a; }
    return pk;
}

/* Feed a +-amp square into the FX input and measure the peak on the stream. */
static int measure_peak(void *inst, int amp)
{
    int fd = dial(18970, 0);
    CHECK(fd >= 0, "connect gain port");
    sendreq(fd, "GET / HTTP/1.0\r\n\r\n");
    static uint8_t rx[200000]; size_t got = 0;
    int16_t blk[256];
    for (int b = 0; b < 300; b++) {
        for (int i = 0; i < 128; i++) { int16_t v = (int16_t)(((b * 128 + i) / 50) % 2 ? amp : -amp); blk[i*2] = v; blk[i*2+1] = v; }
        api->process_block(inst, blk, 128);
        usleep(2900);
        slurp(fd, rx, sizeof rx, 0, &got);
    }
    close(fd);
    return stream_peak(rx, got);
}

static void test_gain_and_limiter(void)
{
    void *inst = api->create_instance(".", "{\"port\": 18970}");
    usleep(200000);
    api->set_param(inst, "source", "1");
    int p;
    p = measure_peak(inst, 6000);
    CHECK(p == 6000, "0 dB + limiter below the knee must be bit-exact (got %d)", p);
    api->set_param(inst, "gain", "6");
    p = measure_peak(inst, 6000);
    CHECK(p > 11900 && p < 12020, "+6 dB should be ~11970 (got %d)", p);
    api->set_param(inst, "gain", "-12");
    p = measure_peak(inst, 20000);
    CHECK(p > 4950 && p < 5100, "-12 dB of 20000 should be ~5024 (got %d)", p);
    api->set_param(inst, "gain", "0");
    api->set_param(inst, "limiter", "0");
    p = measure_peak(inst, 32767);
    CHECK(p == 32767, "limiter off, 0 dB: full scale passes (got %d)", p);
    api->set_param(inst, "limiter", "1");
    p = measure_peak(inst, 32767);
    CHECK(p < 32000 && p > 29000, "limiter should keep full-scale input below clipping (got %d)", p);
    api->set_param(inst, "gain", "12");
    p = measure_peak(inst, 20000);
    CHECK(p < 32767 && p > 30000, "+12 dB into the limiter must not hard-clip (got %d)", p);
    api->set_param(inst, "limiter", "0");
    p = measure_peak(inst, 20000);
    CHECK(p == 32767, "+12 dB with limiter off clips (got %d)", p);
    static char b[16384], b2[16384];
    http_get_all(18970, "/status", b, sizeof b);
    CHECK(strstr(b, "\"gain_db\":12") && strstr(b, "\"limiter\":0") && strstr(b, "\"clipped_in_total\":"), "status gain fields");
    PASS("stream gain is exact; soft limiter prevents clipping; clip counters exposed");

    /* Idle meter honesty: no client connected, full-scale input, limiter on, 0 dB. */
    api->set_param(inst, "gain", "0");
    api->set_param(inst, "limiter", "1");
    int16_t blk[256];
    for (int b = 0; b < 800; b++) {
        for (int i = 0; i < 128; i++) { int16_t v = (int16_t)(((b * 128 + i) / 50) % 2 ? 32767 : -32768); blk[i*2] = v; blk[i*2+1] = v; }
        api->process_block(inst, blk, 128);
        usleep(2900);
    }
    CHECK(get_int(inst, "clients") == 0, "a client is attached");
    http_get_all(18970, "/status", b2, sizeof b2);
    CHECK(strstr(b2, "\"peak_in\":1.0000"), "idle peak_in: %s", strstr(b2, "level_1s"));
    CHECK(!strstr(b2, "\"peak_out\":1.0000") && strstr(b2, "\"peak_out\":0.9"), "idle peak_out must reflect the limiter: %s", strstr(b2, "level_1s"));
    CHECK(!strstr(b2, "\"limited_samples\":0}"), "idle limited_samples is 0: %s", strstr(b2, "level_1s"));
    PASS("idle meter applies the same gain+limiter (peak_out/limited_samples honest with no listener)");
    api->destroy_instance(inst);
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

    unlink(AUDIO_LIVE_SHM_PATH);
    test_stream_and_passthrough(inst, 18800);
    test_ipv6(18800);
    test_request_parsing(inst, 18800);
    test_status_endpoints(inst, 18800);
    test_busy_503(inst, 18800);
    test_stall_eviction_and_drops(inst, 18800);
    test_silent_request_timeout(inst, 18800);
    test_disable_closes_clients(inst, 18800);
    test_rebind(inst, 18800);

    test_tap_source();
    test_gain_and_limiter();

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
