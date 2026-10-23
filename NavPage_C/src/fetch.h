#ifndef NP_FETCH_H
#define NP_FETCH_H
#include <stddef.h>

/* 通过 curl 抓取 URL 内容。
 * 返回 0 成功：data 与 dlen 由调用者接收并 free；ct 为 Content-Type（不需要时传 NULL）。
 * 失败返回非 0。仅 status==200 视为成功。最大 2MB，超时 8s。 */
int np_fetch_url(const char *url, char **data, size_t *dlen, char *ct, size_t ctcap);

#endif
