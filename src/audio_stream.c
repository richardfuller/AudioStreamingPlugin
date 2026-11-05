/*
 * Audio Stream - Schwung audio_fx module for Ableton Move.
 *
 * Passes audio through untouched and copies it into a lock-free ring buffer.
 * A demoted (SCHED_OTHER, cores 0-2) worker thread serves the ring as an
 * endless HTTP "audio/wav" stream (44.1 kHz, 16-bit, stereo):
 *
 *     ffplay http://move.local:8800/    vlc http://move.local:8800/
 *
 * Realtime rules (see schwung/plugin_api_v1.h): process_block never
 * allocates, locks, logs or does I/O. It writes the ring and bumps an atomic.
 * set_param/get_param also only touch atomics and format short strings.
 *
 * Server behaviour:
 *   - Only "GET /" is served (404/405 otherwise, 400 on junk, 408 on silence).
 *   - Up to MAX_CLIENTS listeners; extra connections get "503 Busy".
 *   - A client that stops reading for stall_ms is evicted.
 *   - A client that falls behind drops to the newest audio (frame-aligned)
 *     instead of losing arbitrary chunks.
 *   - If the port is taken we try port+1 .. port+port_span-1, and retry
 *     every second if nothing is free; status says what is going on.
 *   - Changing `port` at runtime rebinds; `enabled=0` closes everything.
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
#include <time.h>
#include <unistd.h>

#include "schwung/audio_fx_api_v2.h"

/* Keep the binary loadable on old device glibc (needs only GLIBC_2.17): bind
 * the thread calls to their 2.17 versions even when built on a newer system,
 * and avoid sscanf/strtol/atoi (redirected to GLIBC_2.38 symbols by new headers). */
#if defined(__aarch64__) && defined(__GLIBC__)
__asm__(".symver pthread_create,pthread_create@GLIBC_2.17");
__asm__(".symver pthread_join,pthread_join@GLIBC_2.17");
#endif

#define DEFAULT_PORT      8800
#define DEFAULT_PORT_SPAN 16
#define DEFAULT_STALL_MS  3000
#define REQUEST_TIMEOUT_MS 5000
#define BIND_RETRY_MS     1000
#define RING_FRAMES       65536          /* power of two, ~1.5 s */
#define RING_MASK         (RING_FRAMES - 1)
#define MAX_CLIENTS       4
#define CLIENT_BUF        (64 * 1024)
#define LIMIT_NORMAL      (64 * 1024)    /* pending bytes before drop-to-latest, ~370 ms */
#define LIMIT_LOW         (16 * 1024)    /* ~93 ms */
#define SNDBUF_NORMAL     (32 * 1024)
#define SNDBUF_LOW        (8 * 1024)
#define CHUNK_FRAMES      1024
#define REQ_MAX           1024

enum { C_FREE = 0, C_REQUEST, C_STREAM };

typedef struct {
    int fd;
    int state;
    int sndbuf_low;
    uint64_t t_accept, t_progress;
    uint8_t req[REQ_MAX];
    size_t reqlen;
    size_t len, off;                     /* pending bytes: buf[off..len) */
    uint64_t hdr_total, s_total;         /* header bytes queued / bytes sent */
    uint8_t buf[CLIENT_BUF];
} client_t;

typedef struct {
    int16_t ring[RING_FRAMES * 2];
    _Atomic uint64_t wr;                 /* frames written (producer) */
    uint64_t rd;                         /* frames read (worker only) */
    _Atomic int enabled;
    _Atomic int low_latency;
    _Atomic int req_port;                /* port asked for (param/config) */
    _Atomic int bound_port;              /* port actually listening, 0 = none */
    _Atomic int bind_err;                /* errno of last failed bind, 0 = none */
    _Atomic int clients;                 /* streaming clients */
    _Atomic uint32_t drops;              /* overflow/resync events */
    int port_span, stall_ms, sample_rate;
    pthread_t thr;
    int thr_started;
    _Atomic int quit;
    client_t cl[MAX_CLIENTS];
} inst_t;

static const host_api_v1_t *g_host;
static audio_fx_api_v2_t g_api;

/* ------------------------------ helpers --------------------------------- */

/* Minimal atoi: optional leading spaces/sign, digits, saturating. */
static int my_atoi(const char *p)
{
    long v = 0; int neg = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); if (v > 1000000000L) v = 1000000000L; p++; }
    return (int)(neg ? -v : v);
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void build_wav_header(uint8_t *h, int rate)
{
    uint32_t byte_rate = (uint32_t)rate * 4, big = 0xFFFFFFFFu, u;
    uint16_t v;
    memcpy(h, "RIFF", 4);  memcpy(h + 4, &big, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    u = 16;             memcpy(h + 16, &u, 4);
    v = 1;              memcpy(h + 20, &v, 2);        /* PCM */
    v = 2;              memcpy(h + 22, &v, 2);        /* channels */
    u = (uint32_t)rate; memcpy(h + 24, &u, 4);
    memcpy(h + 28, &byte_rate, 4);
    v = 4;              memcpy(h + 32, &v, 2);        /* block align */
    v = 16;             memcpy(h + 34, &v, 2);        /* bits */
    memcpy(h + 36, "data", 4); memcpy(h + 40, &big, 4);
}

static void send_status_line(int fd, const char *status)
{
    char m[160];
    int n = snprintf(m, sizeof m,
        "HTTP/1.0 %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status);
    (void)!send(fd, m, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT);
}

static void client_close(inst_t *s, client_t *c)
{
    if (c->fd < 0) return;
    if (c->state == C_STREAM) atomic_fetch_sub(&s->clients, 1);
    close(c->fd);
    c->fd = -1;
    c->state = C_FREE;
}

static void close_all(inst_t *s)
{
    for (int i = 0; i < MAX_CLIENTS; i++) client_close(s, &s->cl[i]);
}

static size_t pending(const client_t *c) { return c->len - c->off; }

static void client_flush(inst_t *s, client_t *c, uint64_t now)
{
    while (c->off < c->len) {
        ssize_t n = send(c->fd, c->buf + c->off, c->len - c->off, MSG_NOSIGNAL);
        if (n > 0) {
            c->off += (size_t)n; c->s_total += (uint64_t)n; c->t_progress = now;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n < 0 && errno == EINTR) continue;
        client_close(s, c);
        return;
    }
    c->off = c->len = 0;
}

/* Queue n bytes. If the client is more than `limit` bytes behind, discard its
 * backlog from the next frame boundary (never mid-frame, never inside the
 * headers) so it resumes on the newest audio. */
static void client_queue(inst_t *s, client_t *c, const void *data, size_t n, size_t limit)
{
    if (pending(c) + n > limit) {
        uint64_t pos = c->s_total;
        size_t k0 = (pos < c->hdr_total)
                        ? (size_t)(c->hdr_total - pos)
                        : (size_t)((4 - (pos - c->hdr_total) % 4) % 4);
        if (k0 < pending(c)) {
            c->len = c->off + k0;
            atomic_fetch_add(&s->drops, 1);
        }
        if (pending(c) + n > CLIENT_BUF) return;       /* cannot happen with sane limits */
    }
    if (c->off && c->len + n > CLIENT_BUF) {
        memmove(c->buf, c->buf + c->off, pending(c));
        c->len -= c->off; c->off = 0;
    }
    memcpy(c->buf + c->len, data, n);
    c->len += n;
}

static void apply_sndbuf(client_t *c, int low)
{
    int sb = low ? SNDBUF_LOW : SNDBUF_NORMAL;
    setsockopt(c->fd, SOL_SOCKET, SO_SNDBUF, &sb, sizeof sb);
    c->sndbuf_low = low;
}

/* ---------------------------- server pieces ----------------------------- */

static void accept_clients(inst_t *s, int lfd, uint64_t now)
{
    for (;;) {
        int fd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK);
        if (fd < 0) return;
        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (s->cl[i].fd < 0) { slot = i; break; }
        if (slot < 0) {
            send_status_line(fd, "503 Busy");
            close(fd);
            continue;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        client_t *c = &s->cl[slot];
        c->fd = fd; c->state = C_REQUEST;
        c->t_accept = c->t_progress = now;
        c->reqlen = 0; c->off = c->len = 0; c->hdr_total = c->s_total = 0;
        apply_sndbuf(c, atomic_load(&s->low_latency));
    }
}

/* Returns after consuming whatever request bytes are available. */
static void client_read_request(inst_t *s, client_t *c, uint64_t now,
                                const uint8_t *wavhdr, size_t limit)
{
    ssize_t n = recv(c->fd, c->req + c->reqlen, REQ_MAX - 1 - c->reqlen, 0);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        client_close(s, c);
        return;
    }
    if (n < 0) return;
    c->reqlen += (size_t)n;
    c->req[c->reqlen] = 0;
    if (!strstr((char *)c->req, "\r\n\r\n") && !strstr((char *)c->req, "\n\n")) {
        if (c->reqlen >= REQ_MAX - 1) {
            send_status_line(c->fd, "400 Bad Request");
            client_close(s, c);
        }
        return;
    }
    /* Request line: METHOD SP PATH SP VERSION */
    char method[8] = {0}, path[256] = {0};
    {
        const char *p = (const char *)c->req;
        size_t i = 0;
        while (*p && *p != ' ' && *p != '\r' && *p != '\n' && i < sizeof method - 1) method[i++] = *p++;
        int ok = (*p == ' ');
        while (*p == ' ') p++;
        i = 0;
        while (*p && *p != ' ' && *p != '\r' && *p != '\n' && i < sizeof path - 1) path[i++] = *p++;
        if (!ok || !method[0] || !path[0]) {
            send_status_line(c->fd, "400 Bad Request");
            client_close(s, c);
            return;
        }
    }
    if (strcmp(method, "GET")) {
        send_status_line(c->fd, "405 Method Not Allowed");
        client_close(s, c);
        return;
    }
    if (strcmp(path, "/") && strncmp(path, "/?", 2)) {
        send_status_line(c->fd, "404 Not Found");
        client_close(s, c);
        return;
    }
    static const char http[] =
        "HTTP/1.0 200 OK\r\nContent-Type: audio/wav\r\n"
        "Cache-Control: no-cache\r\nAccess-Control-Allow-Origin: *\r\n"
        "icy-name: Ableton Move\r\n\r\n";
    c->state = C_STREAM;
    c->t_progress = now;
    atomic_fetch_add(&s->clients, 1);
    client_queue(s, c, http, sizeof http - 1, limit);
    client_queue(s, c, wavhdr, 44, limit);
    c->hdr_total = c->len;
}

static int try_bind(inst_t *s, int port, int *err_out)
{
    int span = s->port_span < 1 ? 1 : s->port_span;
    *err_out = 0;
    for (int p = port; p < port + span && p <= 65535; p++) {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd < 0) { *err_out = errno; return -1; }
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in a = { .sin_family = AF_INET,
                                 .sin_addr.s_addr = htonl(INADDR_ANY),
                                 .sin_port = htons((uint16_t)p) };
        if (bind(fd, (struct sockaddr *)&a, sizeof a) == 0 && listen(fd, 4) == 0) {
            atomic_store(&s->bound_port, p);
            return fd;
        }
        *err_out = errno;
        close(fd);
        if (*err_out != EADDRINUSE) return -1;
    }
    return -1;
}

static void *worker(void *arg)
{
    inst_t *s = arg;
    /* Demote first and keep core 3 free for SPI (plugin_api_v1.h). */
    struct sched_param sp = { .sched_priority = 0 };
    sched_setscheduler(0, SCHED_OTHER, &sp);
    cpu_set_t set; CPU_ZERO(&set);
    CPU_SET(0, &set); CPU_SET(1, &set); CPU_SET(2, &set);
    sched_setaffinity(0, sizeof set, &set);

    uint8_t wavhdr[44];
    build_wav_header(wavhdr, s->sample_rate);

    int lfd = -1, bound_for_req = 0;
    uint64_t next_bind = 0;
    int16_t chunk[CHUNK_FRAMES * 2];
    struct pollfd pfd[MAX_CLIENTS + 1];
    int pidx[MAX_CLIENTS + 1];

    while (!atomic_load(&s->quit)) {
        uint64_t now = now_ms();
        int low = atomic_load(&s->low_latency);
        size_t limit = low ? LIMIT_LOW : LIMIT_NORMAL;
        int want = atomic_load(&s->req_port);

        if (!atomic_load(&s->enabled)) {
            /* Disabled: close the listener and every client, discard audio. */
            close_all(s);
            if (lfd >= 0) { close(lfd); lfd = -1; atomic_store(&s->bound_port, 0); }
            atomic_store(&s->bind_err, 0);
            s->rd = atomic_load(&s->wr);
            poll(NULL, 0, 50);
            continue;
        }

        if (lfd >= 0 && want != bound_for_req) {         /* port changed: rebind */
            close_all(s);
            close(lfd); lfd = -1;
            atomic_store(&s->bound_port, 0);
            next_bind = 0;
        }
        if (lfd < 0 && now >= next_bind) {
            int err;
            lfd = try_bind(s, want, &err);
            if (lfd >= 0) { bound_for_req = want; atomic_store(&s->bind_err, 0); }
            else { atomic_store(&s->bind_err, err ? err : EIO); next_bind = now + BIND_RETRY_MS; }
        }

        int n = 0;
        if (lfd >= 0) { pfd[n].fd = lfd; pfd[n].events = POLLIN; pidx[n] = -1; n++; }
        for (int i = 0; i < MAX_CLIENTS; i++) {
            client_t *c = &s->cl[i];
            if (c->fd < 0) continue;
            if (c->sndbuf_low != low) apply_sndbuf(c, low);
            pfd[n].fd = c->fd; pidx[n] = i;
            /* Always watch for POLLIN so a disconnect is noticed even when no
             * audio is flowing (otherwise dead clients keep their slot). */
            pfd[n].events = (c->state == C_REQUEST) ? POLLIN
                          : (POLLIN | (pending(c) ? POLLOUT : 0));
            n++;
        }
        poll(pfd, (nfds_t)n, 5);
        now = now_ms();

        if (lfd >= 0) accept_clients(s, lfd, now);
        for (int k = 0; k < n; k++) {
            if (pidx[k] < 0 || !(pfd[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            client_t *c = &s->cl[pidx[k]];
            if (c->fd != pfd[k].fd) continue;
            if (c->state == C_REQUEST) {
                client_read_request(s, c, now, wavhdr, limit);
            } else {                                   /* streaming: discard input, detect EOF */
                uint8_t junk[256];
                ssize_t r = recv(c->fd, junk, sizeof junk, 0);
                if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                    client_close(s, c);
            }
        }

        /* Drain the ring. Resync if the producer lapped us. */
        uint64_t wr = atomic_load_explicit(&s->wr, memory_order_acquire);
        if (wr - s->rd > RING_FRAMES) {
            s->rd = wr - RING_FRAMES / 2;
            atomic_fetch_add(&s->drops, 1);
        }
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
                if (c->fd >= 0 && c->state == C_STREAM)
                    client_queue(s, c, chunk, (size_t)nf * 4, limit);  /* LE host (aarch64) */
            }
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            client_t *c = &s->cl[i];
            if (c->fd < 0) continue;
            if (c->state == C_REQUEST) {
                if (now - c->t_accept > REQUEST_TIMEOUT_MS) {
                    send_status_line(c->fd, "408 Request Timeout");
                    client_close(s, c);
                }
                continue;
            }
            client_flush(s, c, now);
            if (c->fd < 0) continue;
            if (!pending(c)) c->t_progress = now;
            else if (now - c->t_progress > (uint64_t)s->stall_ms) client_close(s, c);  /* stalled */
        }
    }
    close_all(s);
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
    return p ? my_atoi(p + 1) : dflt;
}

static int clamp_port(int p) { return p < 1024 ? 1024 : (p > 65535 ? 65535 : p); }

static void *create_instance(const char *module_dir, const char *config_json)
{
    (void)module_dir;
    inst_t *s = calloc(1, sizeof *s);       /* loader thread: not RT */
    if (!s) return NULL;
    atomic_store(&s->req_port, clamp_port(parse_int_key(config_json, "port", DEFAULT_PORT)));
    s->port_span = parse_int_key(config_json, "port_span", DEFAULT_PORT_SPAN);
    s->stall_ms = parse_int_key(config_json, "stall_ms", DEFAULT_STALL_MS);
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

static int parse_enum01(const char *val, const char *on_name)
{
    return !strcmp(val, on_name) || (strcmp(val, "0") && strcmp(val, "off") &&
                                     strcmp(val, "Off") && my_atoi(val) != 0);
}

static void set_param(void *inst, const char *key, const char *val)
{
    inst_t *s = inst;
    if (!s || !key || !val) return;
    if (!strcmp(key, "enabled"))      atomic_store(&s->enabled, parse_enum01(val, "On"));
    else if (!strcmp(key, "latency")) atomic_store(&s->low_latency, parse_enum01(val, "Low"));
    else if (!strcmp(key, "port"))    atomic_store(&s->req_port, clamp_port(my_atoi(val)));
}

/* Parameter metadata. Enums use index-in/index-out. */
#define PARAMS_JSON \
    "{\"key\":\"enabled\",\"name\":\"Stream\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"],\"default\":1}," \
    "{\"key\":\"latency\",\"name\":\"Latency\",\"type\":\"enum\",\"options\":[\"Normal\",\"Low\"],\"default\":0}," \
    "{\"key\":\"port\",\"name\":\"Port\",\"type\":\"int\",\"min\":1024,\"max\":65535,\"default\":8800}," \
    "{\"key\":\"status\",\"name\":\"Status\",\"type\":\"string\",\"access\":\"read\"}," \
    "{\"key\":\"clients\",\"name\":\"Clients\",\"type\":\"int\",\"min\":0,\"max\":4,\"access\":\"read\"}," \
    "{\"key\":\"drops\",\"name\":\"Drops\",\"type\":\"int\",\"min\":0,\"max\":999999,\"access\":\"read\"}"

#define KNOBS_JSON "[\"enabled\",\"latency\",\"port\",\"status\",\"clients\",\"drops\"]"

static const char CHAIN_PARAMS[] = "[" PARAMS_JSON "]";
static const char UI_HIERARCHY[] =
    "{\"levels\":{\"root\":{\"name\":\"Audio Stream\",\"params\":[" PARAMS_JSON "],"
    "\"knobs\":" KNOBS_JSON "}}}";

static int put(char *buf, int buf_len, const char *s)
{
    int n = (int)strlen(s);
    if (n >= buf_len) return -1;
    memcpy(buf, s, (size_t)n + 1);
    return n;
}

static int status_str(inst_t *s, char *buf, int buf_len)
{
    int bp = atomic_load(&s->bound_port), err = atomic_load(&s->bind_err);
    if (!atomic_load(&s->enabled)) return put(buf, buf_len, "OFF");
    if (bp) {
        int c = atomic_load(&s->clients);
        return snprintf(buf, (size_t)buf_len, ":%d %d client%s", bp, c, c == 1 ? "" : "s");
    }
    if (err == EADDRINUSE) return snprintf(buf, (size_t)buf_len, "PORT %d IN USE", atomic_load(&s->req_port));
    if (err) return snprintf(buf, (size_t)buf_len, "BIND ERR %d", err);
    return put(buf, buf_len, "STARTING");
}

static int get_param(void *inst, const char *key, char *buf, int buf_len)
{
    inst_t *s = inst;
    if (!s || !key || !buf || buf_len <= 0) return -1;
    int n;
    if (!strcmp(key, "chain_params"))      return put(buf, buf_len, CHAIN_PARAMS);
    if (!strcmp(key, "ui_hierarchy"))      return put(buf, buf_len, UI_HIERARCHY);
    if (!strcmp(key, "status"))            n = status_str(s, buf, buf_len);
    else if (!strcmp(key, "enabled"))      n = snprintf(buf, (size_t)buf_len, "%d", atomic_load(&s->enabled));
    else if (!strcmp(key, "latency"))      n = snprintf(buf, (size_t)buf_len, "%d", atomic_load(&s->low_latency));
    else if (!strcmp(key, "port"))         n = snprintf(buf, (size_t)buf_len, "%d", atomic_load(&s->req_port));
    else if (!strcmp(key, "bound_port"))   n = snprintf(buf, (size_t)buf_len, "%d", atomic_load(&s->bound_port));
    else if (!strcmp(key, "clients"))      n = snprintf(buf, (size_t)buf_len, "%d", atomic_load(&s->clients));
    else if (!strcmp(key, "drops"))        n = snprintf(buf, (size_t)buf_len, "%u", (unsigned)atomic_load(&s->drops));
    else return -1;
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
