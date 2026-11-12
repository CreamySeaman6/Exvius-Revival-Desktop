// The game's HTTP. Under the runner a request is handed to the host, which sends it to the local game server.
// In the plain-Linux test mode it is sent here over a socket to $SERVER (host:port). No TLS or DNS either way.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <link.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "shared.h"
#include "hyper.h"
#include "http.h"

static unsigned long n_req, n_ok, bytes_in;

static void append(char **buf, size_t *len, const void *d, size_t n) {
    *buf = realloc(*buf, *len + n + 1);
    memcpy(*buf + *len, d, n);
    *len += n;
    (*buf)[*len] = 0;
}
// Raw "HTTP/1.1 200 OK\r\nKey: Value\r\n..." -> what the Java class hands the engine: status line, then "Key:Value" lines.
static void parse_head(HttpConn *c, const char *head, size_t hlen) {
    char *h = NULL; size_t hl = 0;
    const char *cur = head, *end = head + hlen;
    sscanf(head, "HTTP/%*s %d", &c->status);
    while (cur < end) {
        const char *eol = memmem(cur, (size_t)(end - cur), "\r\n", 2);
        size_t ll = eol ? (size_t)(eol - cur) : (size_t)(end - cur);
        if (cur == head) { append(&h, &hl, cur, ll); snprintf(c->status_line, sizeof c->status_line, "%.*s", (int)ll, cur); }
        else {
            const char *co = memchr(cur, ':', ll);
            if (co) { const char *val = co + 1; while (val < cur + ll && *val == ' ') val++; append(&h, &hl, cur, (size_t)(co - cur)); append(&h, &hl, ":", 1); append(&h, &hl, val, (size_t)(cur + ll - val)); }
        }
        append(&h, &hl, "\n", 1);
        if (!eol) break;
        cur = eol + 2;
    }
    c->resp_headers = h;
}
static const char *path_of(const char *url) { const char *p = strstr(url, "://"); p = strchr(p ? p + 3 : url, '/'); return p ? p : "/"; }

static int perform_runner(HttpConn *c, const char *save_path, long *size, uint32_t *crc) {
    struct HcHttp rq;
    memset(&rq, 0, sizeof rq);
    rq.url = (unsigned long)c->url; rq.method = (unsigned long)(c->method[0] ? c->method : "GET");
    rq.headers = (unsigned long)c->req_headers; rq.body = (unsigned long)c->body; rq.body_len = c->body_len;
    rq.save_path = (unsigned long)save_path;
    long job = hc(HC_HTTP_START, (long)&rq, 0, 0, 0);
    int rc = -1;
    if (!rq.failed) {
        char *head = malloc(rq.headers_len + 1);
        c->resp_body = save_path ? NULL : malloc(rq.body_len_out + 1);
        hc(HC_HTTP_READ, job, (long)head, (long)c->resp_body, 0);
        head[rq.headers_len] = 0;
        parse_head(c, head, rq.headers_len);
        free(head);
        c->resp_len = save_path ? 0 : rq.body_len_out;
        if (size) *size = (long)rq.body_len_out;
        if (crc) *crc = rq.crc32;
        n_ok++; bytes_in += rq.body_len_out;
        rc = 0;
    }
    hc(HC_HTTP_FREE, job, 0, 0, 0);
    if (g_verbose) logf_("[http] %s %s -> %d, %lu bytes%s\n", c->method[0] ? c->method : "GET", path_of(c->url), rq.failed ? 0 : c->status, rq.body_len_out, rq.failed ? " (failed)" : "");
    return rc;
}

static int perform_socket(HttpConn *c) {
    const char *server = getenv("SERVER");
    const char *p = strstr(c->url, "://");
    const char *host = p ? p + 3 : c->url, *path = strchr(host, '/');
    char hostname[256];
    snprintf(hostname, sizeof hostname, "%.*s", path ? (int)(path - host) : (int)strlen(host), host);
    if (!path) path = "/";
    if (!server) { logf_("[http] %s %s  (no SERVER set: reported to the game as a connection failure)\n", c->method, c->url); return -1; }
    char ip[64]; int port = 80;
    snprintf(ip, sizeof ip, "%s", server);
    char *colon = strchr(ip, ':');
    if (colon) { *colon = 0; port = atoi(colon + 1); }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET; sa.sin_port = htons(port); inet_pton(AF_INET, ip, &sa.sin_addr);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { logf_("[http] cannot reach %s\n", server); close(fd); return -1; }
    char *req = NULL; size_t rl = 0; char line[2048];
    int n = snprintf(line, sizeof line, "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nContent-Length: %zu\r\n", c->method[0] ? c->method : "GET", path, hostname, c->body_len);
    append(&req, &rl, line, n);
    if (c->req_headers) append(&req, &rl, c->req_headers, c->rh_len);
    append(&req, &rl, "\r\n", 2);
    if (c->body_len) append(&req, &rl, c->body, c->body_len);
    for (size_t off = 0; off < rl;) { ssize_t w = write(fd, req + off, rl - off); if (w <= 0) break; off += w; }
    free(req);
    char *resp = NULL; size_t len = 0; char buf[65536]; ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0) append(&resp, &len, buf, r);
    close(fd);
    char *sep = resp ? strstr(resp, "\r\n\r\n") : NULL;
    if (!sep) { free(resp); return -1; }
    size_t hlen = sep - resp;
    parse_head(c, resp, hlen);
    size_t bl = len - hlen - 4;
    c->resp_body = malloc(bl + 1); memcpy(c->resp_body, sep + 4, bl); c->resp_len = bl;
    free(resp);
    n_ok++; bytes_in += bl;
    if (g_verbose) logf_("[http] %s %s -> %d, %zu bytes\n", c->method, path, c->status, bl);
    return 0;
}

int http_perform(HttpConn *c) {
    if (c->done) return c->status ? 0 : -1;
    c->done = 1;
    n_req++;
    return g_runner ? perform_runner(c, NULL, NULL, NULL) : perform_socket(c);
}
// GET url and store the body in a file.
int http_download(const char *url, const char *path, long *size, uint32_t *crc, int *status) {
    HttpConn c;
    memset(&c, 0, sizeof c);
    c.url = (char *)url; strcpy(c.method, "GET");
    n_req++;
    int rc;
    if (g_runner) rc = perform_runner(&c, path, size, crc);
    else {
        rc = perform_socket(&c);
        if (!rc && c.status == 200) { FILE *f = fopen(path, "wb"); if (f) { fwrite(c.resp_body, 1, c.resp_len, f); fclose(f); } else rc = -1; if (size) *size = (long)c.resp_len; }
        free(c.resp_body);
    }
    if (status) *status = c.status;
    free(c.resp_headers);
    return rc == 0 && c.status == 200 ? 0 : -1;
}
const char *http_header(HttpConn *c, const char *key) {
    static __thread char val[1024];
    if (!c->resp_headers) return NULL;
    size_t kl = strlen(key);
    for (char *l = c->resp_headers; l && *l;) {
        char *eol = strchr(l, '\n');
        if (!strncasecmp(l, key, kl) && l[kl] == ':') { snprintf(val, sizeof val, "%.*s", eol ? (int)(eol - l - kl - 1) : 1000, l + kl + 1); return val; }
        l = eol ? eol + 1 : NULL;
    }
    return NULL;
}
void http_report(void) { logf_("\n== HTTP ==\n  requests %lu, answered %lu, %.1f MB received\n", n_req, n_ok, bytes_in / 1048576.0); }
