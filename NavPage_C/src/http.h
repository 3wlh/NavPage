#ifndef NP_HTTP_H
#define NP_HTTP_H
#include <stddef.h>
#include "plat.h"

#define NP_MAX_HEADER (64 * 1024)
#define NP_MAX_BODY   (11 * 1024 * 1024) /* 略大于 10MB 上传上限，超限直接拒绝 */

typedef struct {
    char *name;
    char *value;
} kv;

typedef struct {
    char method[8];
    char path[2048];   /* 已解码路径 */
    char *target;       /* 原始请求目标（含查询串） */
    kv hdrs[80];
    int nh;
    kv query[32];      /* 查询参数（已解码） */
    int nq;
    kv cookies[16];
    int nc;
    char *body;
    size_t body_len;
    int keep_alive;
    char peer[64];     /* 客户端 IP */
} http_req;

void http_req_free(http_req *r);
const char *req_header(const http_req *r, const char *name);
const char *req_cookie(const http_req *r, const char *name);
const char *req_query(const http_req *r, const char *key);

/* 读取并解析一个请求。0=成功，-1=连接结束/出错。
 * pending 为连接级缓冲（保留多余字节以支持 keep-alive）。 */
typedef struct { char *s; size_t len, cap; } np_buf;
int http_read_request(np_sock fd, np_buf *pending, http_req *r);

/* 发送响应。extra_hdrs 可为 NULL，需以 \r\n 结尾的完整头行（可多行）。 */
int http_send(np_sock fd, int code, const char *status,
              const char *extra_hdrs, const char *ctype, const char *cache,
              const void *body, size_t blen, int keep_alive);
int http_send_buf(np_sock fd, int code, const char *status,
                  const char *extra_hdrs, const char *ctype, const char *cache,
                  const char *body, size_t blen, int keep_alive);

/* 多部分表单：提取第一个文件字段，返回 0 成功 */
int multipart_file(const http_req *r, const char **fname,
                   const char **fdata, size_t *flen);

int url_decode(const char *src, size_t n, char *dst, size_t cap);

#endif
