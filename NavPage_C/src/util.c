#ifdef _WIN32
#define _CRT_RAND_S
#endif
#include "util.h"
#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <pthread.h>
#include <sys/time.h>
#define MKDIR(p) mkdir(p, 0755)
#define closesocket_inner close
#endif

/* ---------- 字符串缓冲 ---------- */

void sb_init(sb *b) { b->s = NULL; b->len = b->cap = 0; }

void sb_free(sb *b) { free(b->s); b->s = NULL; b->len = b->cap = 0; }

void sb_reserve(sb *b, size_t need) {
    if (b->len + need + 1 <= b->cap) return;
    size_t nc = b->cap ? b->cap : 256;
    while (nc < b->len + need + 1) nc *= 2;
    char *ns = realloc(b->s, nc);
    if (!ns) { fprintf(stderr, "内存不足\n"); exit(1); }
    b->s = ns; b->cap = nc;
}

void sb_putc(sb *b, char c) { sb_reserve(b, 1); b->s[b->len++] = c; b->s[b->len] = '\0'; }

void sb_putn(sb *b, const void *data, size_t n) {
    sb_reserve(b, n);
    memcpy(b->s + b->len, data, n);
    b->len += n;
    b->s[b->len] = '\0';
}

void sb_puts(sb *b, const char *s) { sb_putn(b, s, strlen(s)); }

void sb_printf(sb *b, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    sb_reserve(b, (size_t)n);
    vsnprintf(b->s + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

void sb_put_json_escaped(sb *b, const char *s) {
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  sb_puts(b, "\\\""); break;
        case '\\': sb_puts(b, "\\\\"); break;
        case '\b': sb_puts(b, "\\b"); break;
        case '\f': sb_puts(b, "\\f"); break;
        case '\n': sb_puts(b, "\\n"); break;
        case '\r': sb_puts(b, "\\r"); break;
        case '\t': sb_puts(b, "\\t"); break;
        default:
            if (c < 0x20) sb_printf(b, "\\u%04x", c);
            else sb_putc(b, (char)c);
        }
    }
}

/* ---------- 文件 ---------- */

int np_read_file(const char *path, char **out, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return -1; }
    fclose(f);
    buf[sz] = '\0';
    *out = buf;
    *len = (size_t)sz;
    return 0;
}

int np_write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (len > 0 && fwrite(data, 1, len, f) != len) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

int np_replace_file(const char *tmp, const char *dst) {
#ifdef _WIN32
    if (!MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING)) return -1;
    return 0;
#else
    if (rename(tmp, dst) != 0) return -1;
    return 0;
#endif
}

int np_mkdir_p(const char *path) {
    char buf[1024];
    if (snprintf(buf, sizeof buf, "%s", path) >= (int)sizeof buf) return -1;
    for (char *p = buf + (buf[1] == ':' ? 3 : 1); *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
            MKDIR(buf);
            *p = '/';
        }
    }
    return MKDIR(buf) == 0 || errno == EEXIST ? 0 : -1;
}

int np_file_size(const char *path, long *size) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    *size = (long)st.st_size;
    return 0;
}

/* ---------- 字符串 ---------- */

int np_str_ieq(const char *a, const char *b) {
#ifdef _WIN32
    return _stricmp(a, b) == 0;
#else
    return strcasecmp(a, b) == 0;
#endif
}

int np_has_prefix(const char *s, const char *pre) {
    size_t n = strlen(pre);
    return strncmp(s, pre, n) == 0;
}

int np_has_suffix_ci(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    if (ls < lf) return 0;
    const char *tail = s + ls - lf;
    while (*tail && *suf) {
        char a = *tail++, b = *suf++;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return *tail == '\0' && *suf == '\0';
}

const char *np_strcasestr(const char *h, const char *n) {
    size_t ln = strlen(n);
    if (!ln) return h;
    for (; *h; h++) {
        size_t i = 0;
        for (; i < ln; i++) {
            char a = h[i], b = n[i];
            if (!a) break;
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (i == ln) return h;
    }
    return NULL;
}

const void *np_memmem(const void *h, size_t hl, const void *n, size_t nl) {
    if (nl == 0) return h;
    if (hl < nl) return NULL;
    const char *p = h, *end = (const char *)h + hl - nl;
    for (; p <= end; p++)
        if (p[0] == *(const char *)n && memcmp(p, n, nl) == 0) return p;
    return NULL;
}

char *np_path_join(char *dst, size_t cap, const char *a, const char *b) {
    size_t la = strlen(a);
    int need = la && a[la - 1] == '/' ? 0 : 1;
    snprintf(dst, cap, "%s%s%s", a, need ? "/" : "", b);
    return dst;
}

/* ---------- 杂项 ---------- */

uint64_t np_now(void) { return (uint64_t)time(NULL); }

void np_to_hex(const unsigned char *in, size_t n, char *out) {
    static const char hexd[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = hexd[in[i] >> 4];
        out[i * 2 + 1] = hexd[in[i] & 0xF];
    }
    out[n * 2] = '\0';
}

void np_rand_bytes(unsigned char *buf, size_t n) {
#ifdef _WIN32
    for (size_t i = 0; i < n; i++) {
        unsigned v = 0;
        rand_s(&v);
        buf[i] = (unsigned char)v;
    }
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t got = fread(buf, 1, n, f);
        fclose(f);
        if (got == n) return;
    }
    for (size_t i = 0; i < n; i++) buf[i] = (unsigned char)(rand() & 0xFF);
#endif
}

/* ---------- 线程与互斥 ---------- */

#ifdef _WIN32
struct np_mutex { CRITICAL_SECTION cs; };
np_mutex *np_mutex_new(void) { np_mutex *m = malloc(sizeof *m); InitializeCriticalSection(&m->cs); return m; }
void np_mutex_lock(np_mutex *m) { EnterCriticalSection(&m->cs); }
void np_mutex_unlock(np_mutex *m) { LeaveCriticalSection(&m->cs); }
void np_mutex_free(np_mutex *m) { DeleteCriticalSection(&m->cs); free(m); }
int np_thread_start(void (*fn)(void *), void *arg) {
    /* void(*)(void*) 与 DWORD(*)(LPVOID) 调用约定一致，经 void* 中转消除告警 */
    HANDLE h = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)(void *)fn, arg, 0, NULL);
    if (!h) return -1;
    CloseHandle(h);
    return 0;
}
#else
struct np_mutex { pthread_mutex_t m; };
np_mutex *np_mutex_new(void) { np_mutex *m = malloc(sizeof *m); pthread_mutex_init(&m->m, NULL); return m; }
void np_mutex_lock(np_mutex *m) { pthread_mutex_lock(&m->m); }
void np_mutex_unlock(np_mutex *m) { pthread_mutex_unlock(&m->m); }
void np_mutex_free(np_mutex *m) { pthread_mutex_destroy(&m->m); free(m); }
struct np_thread_arg { void (*fn)(void *); void *arg; };
static void *np_thread_entry(void *p) {
    struct np_thread_arg *a = p;
    void (*fn)(void *) = a->fn;
    void *arg = a->arg;
    free(a);
    fn(arg);
    return NULL;
}
int np_thread_start(void (*fn)(void *), void *arg) {
    pthread_t t;
    struct np_thread_arg *a = malloc(sizeof *a);
    if (!a) return -1;
    a->fn = fn; a->arg = arg;
    if (pthread_create(&t, NULL, np_thread_entry, a) != 0) { free(a); return -1; }
    pthread_detach(t);
    return 0;
}
#endif

/* ---------- socket ---------- */

int np_net_init(void) {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0 ? 0 : -1;
#else
    return 0;
#endif
}

int np_closesocket(np_sock s) {
#ifdef _WIN32
    return closesocket(s) == 0 ? 0 : -1;
#else
    return close(s) == 0 ? 0 : -1;
#endif
}
