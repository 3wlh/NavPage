#include "http.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---------- 工具 ---------- */

int url_decode(const char *src, size_t n, char *dst, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (o + 1 >= cap) return -1;
        char c = src[i];
        if (c == '%' && i + 2 < n) {
            int h = 0, l = 0;
            h = src[i + 1] >= '0' && src[i + 1] <= '9' ? src[i + 1] - '0' :
                src[i + 1] >= 'a' && src[i + 1] <= 'f' ? src[i + 1] - 'a' + 10 :
                src[i + 1] >= 'A' && src[i + 1] <= 'F' ? src[i + 1] - 'A' + 10 : -1;
            l = src[i + 2] >= '0' && src[i + 2] <= '9' ? src[i + 2] - '0' :
                src[i + 2] >= 'a' && src[i + 2] <= 'f' ? src[i + 2] - 'a' + 10 :
                src[i + 2] >= 'A' && src[i + 2] <= 'F' ? src[i + 2] - 'A' + 10 : -1;
            if (h >= 0 && l >= 0) {
                dst[o++] = (char)(h * 16 + l);
                i += 2;
            } else {
                dst[o++] = c;
            }
        } else if (c == '+') {
            dst[o++] = ' ';
        } else {
            dst[o++] = c;
        }
    }
    dst[o] = '\0';
    return (int)o;
}

void http_req_free(http_req *r) {
    for (int i = 0; i < r->nh; i++) { free(r->hdrs[i].name); free(r->hdrs[i].value); }
    for (int i = 0; i < r->nq; i++) { free(r->query[i].name); free(r->query[i].value); }
    for (int i = 0; i < r->nc; i++) { free(r->cookies[i].name); free(r->cookies[i].value); }
    free(r->target);
    free(r->body);
    memset(r, 0, sizeof *r);
}

const char *req_header(const http_req *r, const char *name) {
    for (int i = 0; i < r->nh; i++)
        if (np_str_ieq(r->hdrs[i].name, name)) return r->hdrs[i].value;
    return NULL;
}

const char *req_cookie(const http_req *r, const char *name) {
    for (int i = 0; i < r->nc; i++)
        if (!strcmp(r->cookies[i].name, name)) return r->cookies[i].value;
    return NULL;
}

const char *req_query(const http_req *r, const char *key) {
    for (int i = 0; i < r->nq; i++)
        if (!strcmp(r->query[i].name, key)) return r->query[i].value;
    return NULL;
}

/* ---------- 读取 ---------- */

static int buf_append(np_buf *b, const char *data, size_t n) {
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 8192;
        while (nc < b->len + n) nc *= 2;
        char *nd = realloc(b->s, nc);
        if (!nd) return -1;
        b->s = nd;
        b->cap = nc;
    }
    memcpy(b->s + b->len, data, n);
    b->len += n;
    return 0;
}

static int add_header(http_req *r, char *name, char *value) {
    if (r->nh >= (int)(sizeof r->hdrs / sizeof r->hdrs[0])) return -1;
    r->hdrs[r->nh].name = name;
    r->hdrs[r->nh].value = value;
    r->nh++;
    return 0;
}

/* 原地去掉首尾空白，保持指针不变（free 时仍用原地址） */
static void trim(char *s) {
    char *b = s;
    while (*b == ' ' || *b == '\t') b++;
    char *e = b + strlen(b);
    while (e > b && (e[-1] == ' ' || e[-1] == '\t')) e--;
    *e = '\0';
    if (b != s) memmove(s, b, (size_t)(e - b) + 1);
}

static int parse_query_into(http_req *r, const char *qs, size_t len) {
    char pair[2048];
    size_t i = 0;
    while (i < len && r->nq < (int)(sizeof r->query / sizeof r->query[0])) {
        size_t j = i;
        while (j < len && qs[j] != '&') j++;
        if (j - i >= sizeof pair) { i = j + 1; continue; }
        url_decode(qs + i, j - i, pair, sizeof pair);
        char *eq = strchr(pair, '=');
        char *k = pair, *v = (char *)"";
        if (eq) { *eq = '\0'; v = eq + 1; }
        r->query[r->nq].name = strdup(k);
        r->query[r->nq].value = strdup(v);
        r->nq++;
        i = j + 1;
    }
    return 0;
}

static void parse_cookies(http_req *r, const char *ck) {
    const char *p = ck;
    while (p && *p && r->nc < (int)(sizeof r->cookies / sizeof r->cookies[0])) {
        while (*p == ' ' || *p == ';') p++;
        if (!*p) break;
        const char *e = strchr(p, ';');
        size_t seg = e ? (size_t)(e - p) : strlen(p);
        char buf[512];
        if (seg < sizeof buf) {
            memcpy(buf, p, seg);
            buf[seg] = '\0';
            char *eq = strchr(buf, '=');
            if (eq) {
                *eq = '\0';
                r->cookies[r->nc].name = strdup(buf);
                r->cookies[r->nc].value = strdup(eq + 1);
                r->nc++;
            }
        }
        p = e ? e + 1 : NULL;
    }
}

int http_read_request(np_sock fd, np_buf *pending, http_req *r) {
    memset(r, 0, sizeof *r);

    /* 1) 读到 \r\n\r\n */
    size_t hlen = 0;
    while (1) {
        const void *found = np_memmem(pending->s ? pending->s : "", pending->len, "\r\n\r\n", 4);
        if (found) { hlen = (size_t)((const char *)found - pending->s) + 4; break; }
        if (pending->len > NP_MAX_HEADER) return -1;
        char chunk[8192];
        int n = recv(fd, chunk, sizeof chunk, 0);
        if (n <= 0) return -1;
        if (buf_append(pending, chunk, (size_t)n) != 0) return -1;
    }
    if (hlen > NP_MAX_HEADER) return -1;

    /* 2) 拷贝头部并解析 */
    char *head = malloc(hlen + 1);
    if (!head) return -1;
    memcpy(head, pending->s, hlen);
    head[hlen] = '\0';

    char *save = NULL;
    char *line = strtok_r(head, "\r\n", &save);
    if (!line) { free(head); return -1; }

    /* 请求行：METHOD SP TARGET SP VERSION */
    char *sp1 = strchr(line, ' ');
    if (!sp1) { free(head); return -1; }
    *sp1 = '\0';
    char *sp2 = strchr(sp1 + 1, ' ');
    if (!sp2) { free(head); return -1; }
    *sp2 = '\0';
    const char *ver = sp2 + 1;
    snprintf(r->method, sizeof r->method, "%s", line);
    r->target = strdup(sp1 + 1);
    r->keep_alive = strncmp(ver, "HTTP/1.1", 8) == 0;
    if (!r->target) { free(head); return -1; }

    while ((line = strtok_r(NULL, "\r\n", &save)) != NULL) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        char *name = strdup(line);
        char *value = strdup(colon + 1);
        if (!name || !value) { free(name); free(value); continue; }
        trim(value);
        add_header(r, name, value);
    }
    free(head);

    /* 3) Content-Length / Expect */
    size_t clen = 0;
    const char *cl = req_header(r, "Content-Length");
    if (cl) clen = (size_t)strtoull(cl, NULL, 10);
    if (clen > NP_MAX_BODY) return -1;
    const char *expect = req_header(r, "Expect");
    if (expect && np_strcasestr(expect, "100-continue")) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        if (send(fd, cont, (int)sizeof cont - 1, 0) < 0) return -1;
    }

    /* 4) 读 body */
    while (pending->len < hlen + clen) {
        char chunk[16384];
        size_t want = hlen + clen - pending->len;
        if (want > sizeof chunk) want = sizeof chunk;
        int n = recv(fd, chunk, (int)want, 0);
        if (n <= 0) return -1;
        if (buf_append(pending, chunk, (size_t)n) != 0) return -1;
    }
    if (clen) {
        r->body = malloc(clen + 1);
        if (!r->body) return -1;
        memcpy(r->body, pending->s + hlen, clen);
        r->body[clen] = '\0';
        r->body_len = clen;
    }
    /* 消费掉本次请求的字节 */
    memmove(pending->s, pending->s + hlen + clen, pending->len - hlen - clen);
    pending->len -= hlen + clen;

    /* 5) 解析路径与查询串 */
    char *q = strchr(r->target, '?');
    if (q) {
        *q = '\0';
        parse_query_into(r, q + 1, strlen(q + 1));
    }
    if (url_decode(r->target, strlen(r->target), r->path, sizeof r->path) < 0) return -1;

    /* 6) Cookie */
    const char *ck = req_header(r, "Cookie");
    if (ck) parse_cookies(r, ck);

    /* 7) keep-alive */
    const char *conn = req_header(r, "Connection");
    if (conn) {
        if (np_strcasestr(conn, "close")) r->keep_alive = 0;
        else if (np_strcasestr(conn, "keep-alive")) r->keep_alive = 1;
    }
    return 0;
}

/* ---------- 发送 ---------- */

static int send_all(np_sock fd, const char *data, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        int s = send(fd, data + sent, (int)(n - sent), 0);
        if (s <= 0) return -1;
        sent += (size_t)s;
    }
    return 0;
}

int http_send_buf(np_sock fd, int code, const char *status,
                  const char *extra_hdrs, const char *ctype, const char *cache,
                  const char *body, size_t blen, int keep_alive) {
    sb h;
    sb_init(&h);
    sb_printf(&h, "HTTP/1.1 %d %s\r\n", code, status ? status : "OK");
    if (extra_hdrs && *extra_hdrs) sb_puts(&h, extra_hdrs);
    sb_printf(&h, "Content-Type: %s\r\n", ctype ? ctype : "application/octet-stream");
    if (cache && *cache) sb_printf(&h, "Cache-Control: %s\r\n", cache);
    sb_printf(&h, "Content-Length: %zu\r\n", blen);
    sb_printf(&h, "Connection: %s\r\n", keep_alive ? "keep-alive" : "close");
    sb_puts(&h, "\r\n");
    int rc = send_all(fd, h.s, h.len);
    sb_free(&h);
    if (rc != 0) return -1;
    if (blen) rc = send_all(fd, body, blen);
    return rc;
}

int http_send(np_sock fd, int code, const char *status,
              const char *extra_hdrs, const char *ctype, const char *cache,
              const void *body, size_t blen, int keep_alive) {
    return http_send_buf(fd, code, status, extra_hdrs, ctype, cache, body, blen, keep_alive);
}

/* ---------- multipart/form-data ---------- */

/* 从 Content-Type 提取 boundary */
static int get_boundary(const char *ct, char *out, size_t cap) {
    const char *b = np_strcasestr(ct, "boundary=");
    if (!b) return -1;
    b += 9;
    size_t n = 0;
    if (*b == '"') {
        b++;
        while (b[n] && b[n] != '"' && n < cap - 1) { out[n] = b[n]; n++; }
    } else {
        while (b[n] && b[n] != ';' && b[n] != ' ' && n < cap - 1) { out[n] = b[n]; n++; }
    }
    out[n] = '\0';
    return n > 0 ? 0 : -1;
}

/* 在标签字符串中取属性值（大小写不敏感），如 filename="a.png" */
static int attr_get(const char *tag, const char *name, char *out, size_t cap) {
    size_t nl = strlen(name);
    const char *p = tag;
    while ((p = np_strcasestr(p, name)) != NULL) {
        /* 确认是独立属性名：前面是空白或分号，后面是 '=' */
        const char *after = p + nl;
        if (*after != '=') { p = after; continue; }
        if (p > tag && p[-1] != ' ' && p[-1] != ';' && p[-1] != '\t') { p = after; continue; }
        after++;
        size_t n = 0;
        if (*after == '"') {
            after++;
            while (after[n] && after[n] != '"' && n < cap - 1) {
                if (after[n] == '\\' && after[n + 1]) { out[n] = after[n + 1]; n += 2; }
                else { out[n] = after[n]; n++; }
            }
        } else {
            while (after[n] && after[n] != ';' && after[n] != ' ' && n < cap - 1) { out[n] = after[n]; n++; }
        }
        out[n] = '\0';
        return 0;
    }
    return -1;
}

int multipart_file(const http_req *r, const char **fname,
                   const char **fdata, size_t *flen) {
    const char *ct = req_header(r, "Content-Type");
    if (!ct || !np_strcasestr(ct, "multipart/form-data")) return -1;
    char boundary[256];
    if (get_boundary(ct, boundary, sizeof boundary) != 0) return -1;

    char delim[280];
    snprintf(delim, sizeof delim, "--%s", boundary);
    size_t dlen = strlen(delim);

    const char *b = r->body, *end = r->body + r->body_len;

    /* 定位第一个分界线 */
    const void *p = np_memmem(b, (size_t)(end - b), delim, dlen);
    if (!p) return -1;
    b = (const char *)p + dlen;

    while (b < end) {
        if (b[0] == '-' && b[1] == '-') break; /* 结束分界 */
        /* 跳过分界行后的 CRLF */
        if (b[0] == '\r' && b[1] == '\n') b += 2;
        /* 部件头 */
        const void *hh = np_memmem(b, (size_t)(end - b), "\r\n\r\n", 4);
        if (!hh) return -1;
        char head[2048];
        size_t hlen = (size_t)((const char *)hh - b);
        if (hlen >= sizeof head) hlen = sizeof head - 1;
        memcpy(head, b, hlen);
        head[hlen] = '\0';

        /* 部件数据：到下一个 \r\n--boundary */
        char next[284];
        snprintf(next, sizeof next, "\r\n--%s", boundary);
        const void *ne = np_memmem((const char *)hh + 4, (size_t)(end - (const char *)hh - 4), next, strlen(next));
        if (!ne) return -1;

        char name[128], filename[512];
        if (attr_get(head, "filename", filename, sizeof filename) == 0 && filename[0]) {
            (void)name;
            *fname = strdup(filename);
            *fdata = (const char *)hh + 4;
            *flen = (size_t)((const char *)ne - ((const char *)hh + 4));
            return 0;
        }
        b = (const char *)ne + strlen(next);
    }
    return -1;
}
