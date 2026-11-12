#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct HttpConn {
    char *url; char method[16];
    char *req_headers; size_t rh_len;
    unsigned char *body; size_t body_len;
    int done, status;
    char status_line[160];
    char *resp_headers; unsigned char *resp_body; size_t resp_len;
} HttpConn;
int http_perform(HttpConn *c);
int http_download(const char *url, const char *path, long *size, uint32_t *crc, int *status);
const char *http_header(HttpConn *c, const char *key);
void http_report(void);
