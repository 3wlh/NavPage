#ifndef NP_UTIL_H
#define NP_UTIL_H
#include <stddef.h>
#include <stdint.h>

/* ---------- 动态字符串缓冲 ---------- */
typedef struct { char *s; size_t len, cap; } sb;
void sb_init(sb *b);
void sb_free(sb *b);
void sb_reserve(sb *b, size_t need);
void sb_putc(sb *b, char c);
void sb_putn(sb *b, const void *data, size_t n);
void sb_puts(sb *b, const char *s);
void sb_printf(sb *b, const char *fmt, ...);
void sb_put_json_escaped(sb *b, const char *s);

/* ---------- 文件 ---------- */
int  np_read_file(const char *path, char **out, size_t *len);       /* 0=成功 */
int  np_write_file(const char *path, const void *data, size_t len); /* 0=成功 */
int  np_replace_file(const char *tmp, const char *dst);            /* 0=成功，覆盖目标 */
int  np_mkdir_p(const char *path);
int  np_file_size(const char *path, long *size);                   /* 0=存在 */

/* ---------- 字符串 ---------- */
int  np_str_ieq(const char *a, const char *b);
int  np_has_prefix(const char *s, const char *pre);
int  np_has_suffix_ci(const char *s, const char *suf);
const char *np_strcasestr(const char *h, const char *n);
const void *np_memmem(const void *h, size_t hl, const void *n, size_t nl);
char *np_path_join(char *dst, size_t cap, const char *a, const char *b);

/* ---------- 杂项 ---------- */
uint64_t np_now(void);
void np_rand_bytes(unsigned char *buf, size_t n);
void np_to_hex(const unsigned char *in, size_t n, char *out);

/* ---------- 线程与互斥 ---------- */
typedef struct np_mutex np_mutex;
np_mutex *np_mutex_new(void);
void np_mutex_lock(np_mutex *m);
void np_mutex_unlock(np_mutex *m);
void np_mutex_free(np_mutex *m);
int np_thread_start(void (*fn)(void *), void *arg);

#endif
