// HTTP for the game. Every request the game makes, whatever host or scheme it names, is sent as plain HTTP
// to the local game server (FFBE_SERVER=host:port). Each request runs on its own host thread so the game's
// other threads (sound, drawing) keep running while it waits.
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "runner.h"
#include "ui.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define CLOSESOCK close
#endif

char g_server[128];

typedef struct {
    int used; SDL_atomic_t done;
    char *url, *method, *headers, *save_path; uint8_t *body; size_t body_len;
    int status, failed; char *head; size_t head_len; uint8_t *resp; size_t resp_len; uint32_t crc; size_t saved;
    struct HcHttp *req;
} Job;
#define MAXJ 64
static Job jobs[MAXJ];

static uint32_t crc_table[256];
static uint32_t crc_update(uint32_t c, const uint8_t *d, size_t n) {
    if (!crc_table[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = (v >> 1) ^ (0xEDB88320u & -(v & 1)); crc_table[i] = v; }
    c = ~c;
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}
static void append(uint8_t **buf, size_t *len, size_t *cap, const void *d, size_t n) {
    if (*len + n + 1 > *cap) { *cap = (*len + n + 1) * 2; *buf = realloc(*buf, *cap); }
    memcpy(*buf + *len, d, n); *len += n; (*buf)[*len] = 0;
}
static const char *find_header(const char *head, const char *key) {
    size_t kl = strlen(key);
    for (const char *l = head; l && *l;) {
        if (!SDL_strncasecmp(l, key, kl) && l[kl] == ':') { l += kl + 1; while (*l == ' ') l++; return l; }
        l = strstr(l, "\r\n"); if (l) l += 2;
    }
    return NULL;
}

static int worker(void *ud) {
    Job *j = ud;
    j->failed = 1;
    const char *p = strstr(j->url, "://");
    const char *host = p ? p + 3 : j->url, *path = strchr(host, '/');
    char hostname[256];
    SDL_snprintf(hostname, sizeof hostname, "%.*s", path ? (int)(path - host) : (int)strlen(host), host);
    if (!path) path = "/";
    char ip[100]; int port = 80;
    SDL_snprintf(ip, sizeof ip, "%s", g_server);
    char *colon = strchr(ip, ':');
    if (colon) { *colon = 0; port = atoi(colon + 1); }
    if (!ip[0]) { rlog("[http] no game server configured; %s fails\n", j->url); goto out; }
    sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_port = htons((unsigned short)port); sa.sin_addr.s_addr = inet_addr(ip);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { rlog("[http] cannot reach the game server at %s\n", g_server); CLOSESOCK(fd); goto out; }
    uint8_t *req = NULL; size_t rl = 0, rc = 0; char line[8192], safe[4096]; size_t so = 0;
    // As Java's URL handling does: spaces and other characters that cannot appear in a request line are percent-encoded.
    for (const unsigned char *s = (const unsigned char *)path; *s && so < sizeof safe - 4; s++) {
        if (*s <= 0x20 || *s >= 0x7F || strchr("\"<>\\^`{|}", *s)) so += (size_t)SDL_snprintf(safe + so, 4, "%%%02X", *s);
        else safe[so++] = (char)*s;
    }
    safe[so] = 0;
    int n = SDL_snprintf(line, sizeof line, "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nContent-Length: %u\r\n", j->method[0] ? j->method : "GET", safe, hostname, (unsigned)j->body_len);
    append(&req, &rl, &rc, line, (size_t)n);
    if (j->headers) append(&req, &rl, &rc, j->headers, strlen(j->headers));
    append(&req, &rl, &rc, "\r\n", 2);
    if (j->body_len) append(&req, &rl, &rc, j->body, j->body_len);
    for (size_t off = 0; off < rl;) { int w = send(fd, (const char *)req + off, (int)(rl - off > (1 << 20) ? (1 << 20) : rl - off), 0); if (w <= 0) break; off += (size_t)w; }
    free(req);

    uint8_t *buf = NULL; size_t len = 0, cap = 0; static const int CHUNK = 1 << 16;
    char *tmp = malloc((size_t)CHUNK);
    char *sep = NULL; int r;
    while (!sep && (r = recv(fd, tmp, CHUNK, 0)) > 0) { append(&buf, &len, &cap, tmp, (size_t)r); sep = strstr((char *)buf, "\r\n\r\n"); }
    if (!sep) { free(tmp); free(buf); CLOSESOCK(fd); goto out; }
    size_t hlen = (size_t)(sep - (char *)buf);
    j->head = malloc(hlen + 1); memcpy(j->head, buf, hlen); j->head[hlen] = 0; j->head_len = hlen;
    sscanf(j->head, "HTTP/%*s %d", &j->status);
    const char *te = find_header(j->head, "Transfer-Encoding");
    int chunked = te && !SDL_strncasecmp(te, "chunked", 7);
    uint8_t *body = (uint8_t *)sep + 4; size_t have = len - hlen - 4;
    FILE *out = NULL;
    if (j->save_path && j->status == 200 && !chunked) out = os_fopen(j->save_path, "wb");
    if (out) {
        if (have) { fwrite(body, 1, have, out); j->crc = crc_update(j->crc, body, have); }
        j->saved = have;
        while ((r = recv(fd, tmp, CHUNK, 0)) > 0) { fwrite(tmp, 1, (size_t)r, out); j->crc = crc_update(j->crc, (uint8_t *)tmp, (size_t)r); j->saved += (size_t)r; }
        fclose(out);
    } else {
        uint8_t *b = NULL; size_t bl = 0, bc = 0;
        if (have) append(&b, &bl, &bc, body, have);
        while ((r = recv(fd, tmp, CHUNK, 0)) > 0) append(&b, &bl, &bc, tmp, (size_t)r);
        if (chunked && b) {
            size_t o = 0; uint8_t *q = b, *end = b + bl;
            while (q < end) { unsigned long cl = strtoul((char *)q, NULL, 16); uint8_t *e = (uint8_t *)strstr((char *)q, "\r\n"); if (!e) break; q = e + 2; if (!cl) break; if (q + cl > end) cl = (unsigned long)(end - q); memmove(b + o, q, cl); o += cl; q += cl + 2; }
            bl = o;
        }
        if (j->save_path && j->status == 200) { FILE *f = os_fopen(j->save_path, "wb"); if (f) { fwrite(b, 1, bl, f); fclose(f); } j->crc = crc_update(0, b, bl); j->saved = bl; free(b); }
        else { j->resp = b; j->resp_len = bl; }
    }
    free(tmp); free(buf);
    CLOSESOCK(fd);
    if (j->save_path && j->status == 200) overlay_file_written(j->save_path);
    j->failed = 0;
out:
    SDL_AtomicSet(&j->done, 1);
    return 0;
}

static void sockets_start(void) {
#ifdef _WIN32
    static int started; if (!started) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); started = 1; }
#endif
}
// A request made by the runner itself (the in-game web page). Blocks; returns the HTTP status, or 0 if the server
// could not be reached. The caller frees *out.
int http_fetch(const char *url, const char *method, const char *headers, const void *body, size_t body_len, uint8_t **out, size_t *out_len) {
    sockets_start();
    Job j; memset(&j, 0, sizeof j);
    j.url = (char *)url; j.method = (char *)(method ? method : "GET"); j.headers = (char *)headers;
    j.body = (uint8_t *)body; j.body_len = body_len;
    worker(&j);
    free(j.head);
    *out = j.resp; *out_len = j.resp_len;
    return j.failed ? 0 : j.status;
}
static char *dup_g(uint64_t g) { return g ? SDL_strdup(gstr(g)) : NULL; }

int http_start(struct HcHttp *req) {
    sockets_start();
    for (int i = 0; i < MAXJ; i++) if (!jobs[i].used) {
        Job *j = &jobs[i];
        memset(j, 0, sizeof *j);
        j->used = 1; j->req = req;
        j->url = dup_g(req->url); j->method = dup_g(req->method); j->headers = dup_g(req->headers);
        if (!j->method) j->method = SDL_strdup("GET");
        if (req->save_path) { char h[1100]; if (!guest_path(req->save_path, h, sizeof h)) j->save_path = SDL_strdup(h); }
        if (req->body && req->body_len) { j->body = malloc(req->body_len); memcpy(j->body, g2h(req->body), req->body_len); j->body_len = req->body_len; }
        SDL_Thread *t = SDL_CreateThread(worker, "http", j);
        SDL_DetachThread(t);
        return i + 1;
    }
    fatal("too many HTTP requests at once");
    return 0;
}
int http_done(int job) {
    Job *j = &jobs[job - 1];
    if (!SDL_AtomicGet(&j->done)) return 0;
    if (j->req) {
        j->req->status = j->status; j->req->failed = j->failed;
        j->req->headers_len = j->head_len; j->req->body_len_out = j->save_path ? j->saved : j->resp_len; j->req->crc32 = j->crc;
        j->req = NULL;
    }
    return 1;
}
void http_read(int job, void *headers, void *body) {
    Job *j = &jobs[job - 1];
    if (headers && j->head) memcpy(headers, j->head, j->head_len);
    if (body && j->resp) memcpy(body, j->resp, j->resp_len);
}
void http_free(int job) {
    Job *j = &jobs[job - 1];
    free(j->url); free(j->method); free(j->headers); free(j->save_path); free(j->body); free(j->head); free(j->resp);
    j->used = 0;
}
