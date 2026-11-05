/*
 * Audio Stream - Schwung audio_fx module for Ableton Move.
 *
 * Passes audio through untouched and copies it into a lock-free ring buffer.
 * A demoted (SCHED_OTHER, cores 0-2) worker thread serves the ring as an
 * endless HTTP "audio/wav" stream (44.1 kHz, 16-bit, stereo) on a TCP port:
 *
 *     ffplay http://move.local:8800/    vlc http://move.local:8800/
 *
 * Realtime rules (see plugin_api_v1.h): process_block never allocates, locks,
 * logs or does I/O. It only writes to the ring and bumps an atomic counter.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "schwung/audio_fx_api_v2.h"

#define DEFAULT_PORT   8800
#define RING_FRAMES    65536            /* power of two, ~1.5 s */
#define RING_MASK      (RING_FRAMES - 1)
#define MAX_CLIENTS    4
#define CLIENT_BUF     (64 * 1024)      /* per-client pending bytes (~370 ms) */
#define CHUNK_FRAMES   1024

typedef struct {
    int fd;
    int hdr_sent;
    size_t len, off;
    uint8_t buf[CLIENT_BUF];
} client_t;

typedef struct {
    int16_t ring[RING_FRAMES * 2];
    _Atomic uint64_t wr;                 /* frames written (producer) */
    uint64_t rd;                         /* frames read (worker only) */
    _Atomic int enabled;
    _Atomic int clients;
    _Atomic uint64_t dropped;            /* frames lost to ring overflow */
    int port;
    int sample_rate;
    pthread_t thr;
    int thr_started;
    _Atomic int quit;
    client_t cl[MAX_CLIENTS];
} inst_t;

static const host_api_v1_t *g_host;
static audio_fx_api_v2_t g_api;

/* ------------------------------ worker ---------------------------------- */

static void build_wav_header(uint8_t *h, int rate)
{
    uint32_t byte_rate = (uint32_t)rate * 4, big = 0xFFFFFFFFu;
    uint16_t v;
    uint32_t u;
    memcpy(h, "RIFF", 4);  memcpy(h + 4, &big, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    u = 16;          memcpy(h + 16, &u, 4);
    v = 1;           memcpy(h + 20, &v, 2);        /* PCM */
    v = 2;           memcpy(h + 22, &v, 2);        /* channels */
    u = (uint32_t)rate; memcpy(h + 24, &u, 4);
    memcpy(h + 28, &byte_rate, 4);
    v = 4;           memcpy(h + 32, &v, 2);        /* block align */
    v = 16;          memcpy(h + 34, &v, 2);        /* bits */
    memcpy(h + 36, "data", 4); memcpy(h + 40, &big, 4);
}

static void client_close(inst_t *s, client_t *c)
{
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    atomic_fetch_sub(&s->clients, 1);
}

static void client_flush(inst_t *s, client_t *c)
{
    while (c->off < c->len) {
        ssize_t n = send(c->fd, c->buf + c->off, c->len - c->off, MSG_NOSIGNAL);
        if (n > 0) { c->off += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n < 0 && errno == EINTR) continue;
        client_close(s, c);
        return;
    }
    c->off = c->len = 0;
}

static void client_queue(client_t *c, const void *data, size_t n)
{
    if (c->len - c->off + n > CLIENT_BUF) return;   /* slow client: drop chunk */
    if (c->off && c->len + n > CLIENT_BUF) {
        memmove(c->buf, c->buf + c->off, c->len - c->off);
        c->len -= c->off; c->off = 0;
    }
    memcpy(c->buf + c->len, data, n);
    c->len += n;
}

static void accept_clients(inst_t *s, int lfd)
{
    for (;;) {
        int fd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK);
        if (fd < 0) return;
        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) if (s->cl[i].fd < 0) { slot = i; break; }
        if (slot < 0) { close(fd); continue; }
        int one = 1, sb = 32 * 1024;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sb, sizeof sb);
        /* We don't parse the request; any GET gets the stream. Give the
         * client a moment to send it, then reply immediately. */
        client_t *c = &s->cl[slot];
        c->fd = fd; c->off = c->len = 0; c->hdr_sent = 0;
        atomic_fetch_add(&s->clients, 1);
    }
}

static void *worker(void *arg)
{
    inst_t *s = arg;
    /* Demote first: we inherit nothing from the RT thread, but be explicit,
     * and keep core 3 free for SPI. */
    struct sched_param sp = { .sched_priority = 0 };
    sched_setscheduler(0, SCHED_OTHER, &sp);
    cpu_set_t set; CPU_ZERO(&set);
    CPU_SET(0, &set); CPU_SET(1, &set); CPU_SET(2, &set);
    sched_setaffinity(0, sizeof set, &set);

    int lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET,
                             .sin_addr.s_addr = htonl(INADDR_ANY),
                             .sin_port = htons((uint16_t)s->port) };
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) < 0 || listen(lfd, 4) < 0) {
        close(lfd); lfd = -1;
    }

    uint8_t wavhdr[44];
    build_wav_header(wavhdr, s->sample_rate);
    static const char http[] =
        "HTTP/1.0 200 OK\r\nContent-Type: audio/wav\r\n"
        "Cache-Control: no-cache\r\nAccess-Control-Allow-Origin: *\r\n"
        "icy-name: Ableton Move\r\n\r\n";

    int16_t chunk[CHUNK_FRAMES * 2];
    struct pollfd pfd[MAX_CLIENTS + 1];

    while (!atomic_load(&s->quit)) {
        int n = 0;
        if (lfd >= 0) { pfd[n].fd = lfd; pfd[n].events = POLLIN; n++; }
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (s->cl[i].fd >= 0) {
                pfd[n].fd = s->cl[i].fd;
                pfd[n].events = (s->cl[i].len > s->cl[i].off) ? POLLOUT : 0;
                n++;
            }
        poll(pfd, (nfds_t)n, 5);

        if (lfd >= 0) accept_clients(s, lfd);

        /* Drain the ring. Resync if the producer lapped us. */
        uint64_t wr = atomic_load_explicit(&s->wr, memory_order_acquire);
        if (wr - s->rd > RING_FRAMES) s->rd = wr - RING_FRAMES / 2;
        while (s->rd < wr) {
            uint64_t avail = wr - s->rd;
            int nf = avail > CHUNK_FRAMES ? CHUNK_FRAMES : (int)avail;
            for (int i = 0; i < nf; i++) {
                size_t idx = (size_t)((s->rd + (uint64_t)i) & RING_MASK) * 2;
                chunk[i * 2] = s->ring[idx];
                chunk[i * 2 + 1] = s->ring[idx + 1];
            }
            s->rd += (uint64_t)nf;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                client_t *c = &s->cl[i];
                if (c->fd < 0) continue;
                if (!c->hdr_sent) {
                    client_queue(c, http, sizeof http - 1);
                    client_queue(c, wavhdr, sizeof wavhdr);
                    c->hdr_sent = 1;
                }
                client_queue(c, chunk, (size_t)nf * 4);   /* LE host assumed (aarch64) */
            }
        }
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (s->cl[i].fd >= 0) client_flush(s, &s->cl[i]);
    }
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (s->cl[i].fd >= 0) client_close(s, &s->cl[i]);
    if (lfd >= 0) close(lfd);
    return NULL;
}

/* ------------------------------ plugin API ------------------------------ */

static int parse_int_key(const char *json, const char *key, int dflt)
{
    if (!json) return dflt;
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return dflt;
    p = strchr(p + strlen(pat), ':');
    return p ? atoi(p + 1) : dflt;
}

static void *create_instance(const char *module_dir, const char *config_json)
{
    (void)module_dir;
    inst_t *s = calloc(1, sizeof *s);       /* loader thread: not RT */
    if (!s) return NULL;
    s->port = parse_int_key(config_json, "port", DEFAULT_PORT);
    s->sample_rate = (g_host && g_host->sample_rate > 0) ? g_host->sample_rate
                                                          : MOVE_SAMPLE_RATE;
    atomic_store(&s->enabled, 1);
    for (int i = 0; i < MAX_CLIENTS; i++) s->cl[i].fd = -1;
    if (pthread_create(&s->thr, NULL, worker, s) == 0) s->thr_started = 1;
    return s;
}

static void destroy_instance(void *inst)
{
    inst_t *s = inst;
    if (!s) return;
    atomic_store(&s->quit, 1);
    if (s->thr_started) pthread_join(s->thr, NULL);
    free(s);
}

static void process_block(void *inst, int16_t *audio, int frames)
{
    inst_t *s = inst;
    if (!s || !atomic_load_explicit(&s->enabled, memory_order_relaxed)) return;
    uint64_t wr = atomic_load_explicit(&s->wr, memory_order_relaxed);
    for (int i = 0; i < frames; i++) {
        size_t idx = (size_t)((wr + (uint64_t)i) & RING_MASK) * 2;
        s->ring[idx] = audio[i * 2];
        s->ring[idx + 1] = audio[i * 2 + 1];
    }
    atomic_store_explicit(&s->wr, wr + (uint64_t)frames, memory_order_release);
}

static void set_param(void *inst, const char *key, const char *val)
{
    inst_t *s = inst;
    if (!s || !key || !val) return;
    if (!strcmp(key, "enabled"))
        atomic_store(&s->enabled, atoi(val) != 0 || !strcmp(val, "on"));
}

static int get_param(void *inst, const char *key, char *buf, int buf_len)
{
    inst_t *s = inst;
    if (!s || !key || !buf || buf_len <= 0) return -1;
    int n = -1;
    if (!strcmp(key, "enabled"))      n = snprintf(buf, (size_t)buf_len, "%d", atomic_load(&s->enabled));
    else if (!strcmp(key, "port"))    n = snprintf(buf, (size_t)buf_len, "%d", s->port);
    else if (!strcmp(key, "clients")) n = snprintf(buf, (size_t)buf_len, "%d", atomic_load(&s->clients));
    else if (!strcmp(key, "status"))
        n = snprintf(buf, (size_t)buf_len, ":%d  %d client%s", s->port,
                     atomic_load(&s->clients), atomic_load(&s->clients) == 1 ? "" : "s");
    if (n < 0) return -1;
    return n >= buf_len ? buf_len - 1 : n;
}

audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host)
{
    g_host = host;
    memset(&g_api, 0, sizeof g_api);
    g_api.api_version = AUDIO_FX_API_VERSION_2;
    g_api.create_instance = create_instance;
    g_api.destroy_instance = destroy_instance;
    g_api.process_block = process_block;
    g_api.set_param = set_param;
    g_api.get_param = get_param;
    g_api.on_midi = NULL;
    return &g_api;
}
