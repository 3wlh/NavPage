#include "fetch.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FETCH_UA "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36"
#define FETCH_MAX (2 * 1024 * 1024)

#ifdef _WIN32
#define POPEN _popen
#define PCLOSE _pclose
#else
#define POPEN popen
#define PCLOSE pclose
#endif

static void curl_quote(sb *b, const char *s) {
    sb_putc(b, '"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') sb_putc(b, '\\');
        sb_putc(b, *s);
    }
    sb_putc(b, '"');
}

/* 返回 0 成功：data/body 由调用者 free；*ct 为 Content-Type（不需要时传 NULL） */
int np_fetch_url(const char *url, char **data, size_t *dlen, char *ct, size_t ctcap) {
    *data = NULL;
    *dlen = 0;
    char ctbuf[128];
    if (!ct) { ct = ctbuf; ctcap = sizeof ctbuf; }
    ct[0] = '\0';

    /* 用临时文件承接响应体，避免 shell 引号与 % 展开问题 */
    unsigned char rnd[8];
    np_rand_bytes(rnd, 8);
    char hex[17];
    np_to_hex(rnd, 8, hex);

    char tmp[64], cfgname[64];
    snprintf(tmp, sizeof tmp, ".npfetch-%s.tmp", hex);
    snprintf(cfgname, sizeof cfgname, ".npcfg-%s.tmp", hex);

    sb c;
    sb_init(&c);
    sb_puts(&c, "url = ");
    curl_quote(&c, url);
    sb_puts(&c, "\noutput = ");
    curl_quote(&c, tmp);
    sb_puts(&c, "\nuser-agent = ");
    curl_quote(&c, FETCH_UA);
    sb_puts(&c, "\nlocation\nsilent\nshow-error\nmax-time = 8\n");
    sb_puts(&c, "write-out = \"%{http_code} %{content_type}\"\n");
    if (np_write_file(cfgname, c.s, c.len) != 0) { sb_free(&c); return -1; }
    sb_free(&c);

    char cmd[256];
    snprintf(cmd, sizeof cmd, "curl -sS -K \"%s\"", cfgname);
    FILE *pp = POPEN(cmd, "r");
    if (!pp) { remove(cfgname); return -1; }

    char line[512] = "";
    if (fgets(line, sizeof line, pp) == NULL) line[0] = '\0';
    int pcr = PCLOSE(pp);
    /* 配置文件须等 curl 结束后再删：进程启动有延迟，过早删除导致 curl 读不到配置 */
    remove(cfgname);

    int status = 0;
    char ctypes[128] = "";
    if (sscanf(line, "%d %127s", &status, ctypes) < 1) status = 0;
    (void)pcr;
    snprintf(ct, ctcap, "%s", ctypes);

    size_t n = 0;
    char *body = NULL;
    if (status == 200 && np_read_file(tmp, &body, &n) == 0) {
        if (n > FETCH_MAX) n = FETCH_MAX;
        *data = body;
        *dlen = n;
    } else {
        free(body);
    }
    remove(tmp);
    return status == 200 && *data && *dlen ? 0 : -1;
}
