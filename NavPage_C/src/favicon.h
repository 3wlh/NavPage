#ifndef NP_FAVICON_H
#define NP_FAVICON_H
#include <stddef.h>

/* 抓取站点图标。
 * rawurl: 站点地址（可无协议头）；mode: "link"=仅返回外链，其他=下载落盘
 * uploads_dir: 上传目录（落盘/临时文件用）
 * 返回 0 成功（out_url 有效）；否则返回 HTTP 状态码，err 为错误消息 */
int favicon_fetch(const char *rawurl, const char *mode, const char *uploads_dir,
                   char *out_url, size_t cap, char *err, size_t errcap);

#endif
