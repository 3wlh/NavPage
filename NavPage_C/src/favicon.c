#include "favicon.h"
#include "util.h"
#include "crypto.h"
#include "fetch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* curl 抓取逻辑已抽至 fetch.c；保留宏名兼容旧调用点 */
#define fetch_url np_fetch_url

/* ---------- 类型识别 ---------- */

static const char *ext_from_mime(const char *ct) {
    char low[128];
    snprintf(low, sizeof low, "%s", ct);
    for (char *p = low; *p; p++) *p = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
    char *semi = strchr(low, ';');
    if (semi) *semi = '\0';
    char *p = low + strlen(low);
    while (p > low && p[-1] == ' ') p--;
    *p = '\0';
    if (!strcmp(low, "image/png")) return ".png";
    if (!strcmp(low, "image/jpeg") || !strcmp(low, "image/jpg")) return ".jpg";
    if (!strcmp(low, "image/gif")) return ".gif";
    if (!strcmp(low, "image/webp")) return ".webp";
    if (!strcmp(low, "image/svg+xml")) return ".svg";
    if (!strcmp(low, "image/x-icon") || !strcmp(low, "image/vnd.microsoft.icon") || !strcmp(low, "image/ico")) return ".ico";
    if (!strcmp(low, "image/bmp")) return ".bmp";
    return NULL;
}

static const char *sniff_ext(const char *d, size_t n) {
    const unsigned char *p = (const unsigned char *)d;
    if (n >= 8 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8)) return ".png";
    if (n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return ".jpg";
    if (n >= 6 && (!memcmp(p, "GIF87a", 6) || !memcmp(p, "GIF89a", 6))) return ".gif";
    if (n >= 12 && !memcmp(p, "RIFF", 4) && !memcmp(p + 8, "WEBP", 4)) return ".webp";
    if (n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 1 && p[3] == 0) return ".ico";
    size_t lim = n < 512 ? n : 512;
    for (size_t i = 0; i + 4 <= lim; i++)
        if (p[i] == '<' && (p[i+1] == 's' || p[i+1] == 'S') &&
            (p[i+2] == 'v' || p[i+2] == 'V') && (p[i+3] == 'g' || p[i+3] == 'G'))
            return ".svg";
    return NULL;
}

/* ---------- HTML 图标链接解析 ---------- */

typedef struct {
    char url[2304];
    int score;
} iconcand;

static int attr_get(const char *tag, const char *name, char *out, size_t cap);

static int sizes_max(const char *sizes) {
    int m = 0;
    const char *p = sizes;
    while (p && *p) {
        while (*p == ' ') p++;
        const char *e = p;
        while (*e && *e != ' ') e++;
        char tok[32];
        size_t tl = (size_t)(e - p);
        if (tl && tl < sizeof tok) {
            memcpy(tok, p, tl);
            tok[tl] = '\0';
            char *x = strchr(tok, 'x');
            if (!x) x = strchr(tok, 'X');
            if (x && x > tok) {
                *x = '\0';
                int a = atoi(tok);
                if (a > m) m = a;
            }
        }
        p = e;
    }
    return m;
}

static void resolve_url(const char *href, const char *origin, const char *scheme, char *out, size_t cap) {
    int r;
    if (np_has_prefix(href, "http://") || np_has_prefix(href, "https://")) {
        r = snprintf(out, cap, "%s", href);
    } else if (np_has_prefix(href, "//")) {
        r = snprintf(out, cap, "%s:%s", scheme, href);
    } else if (href[0] == '/') {
        r = snprintf(out, cap, "%s%s", origin, href);
    } else {
        r = snprintf(out, cap, "%s/%s", origin, href);
    }
    if (r < 0 || (size_t)r >= cap) out[0] = '\0';
}

static int extract_icon_links(const char *html, const char *origin, const char *scheme,
                              iconcand *cands, int max) {
    int n = 0;
    const char *p = html;
    while ((p = np_strcasestr(p, "<link")) != NULL && n < max) {
        const char *end = strchr(p + 5, '>');
        if (!end) break;
        size_t tl = (size_t)(end - p);
        if (tl > 4096) tl = 4096;
        char tag[4097];
        memcpy(tag, p, tl);
        tag[tl] = '\0';
        p = end + 1;

        char rel[256] = "", href[1024] = "", sizes[128] = "";
        attr_get(tag, "rel", rel, sizeof rel);
        attr_get(tag, "href", href, sizeof href);
        attr_get(tag, "sizes", sizes, sizeof sizes);
        if (!rel[0] || !href[0]) continue;
        if (!np_strcasestr(rel, "icon") || np_strcasestr(rel, "mask")) continue;

        char abs[2304];
        resolve_url(href, origin, scheme, abs, sizeof abs);
        if (!abs[0]) continue;

        int score = 50;
        if (np_strcasestr(rel, "apple")) {
            score = 300;
        } else if (sizes[0]) {
            char szlow[128];
            snprintf(szlow, sizeof szlow, "%s", sizes);
            for (char *q = szlow; *q; q++) *q = (char)(*q >= 'A' && *q <= 'Z' ? *q + 32 : *q);
            /* 去两端空白 */
            char *b = szlow;
            while (*b == ' ') b++;
            char *t = b + strlen(b);
            while (t > b && t[-1] == ' ') t--;
            *t = '\0';
            if (!strcmp(b, "any")) score = 150;
            else if (sizes_max(sizes) >= 96) score = 200;
        }
        if (np_has_suffix_ci(abs, ".svg") && score < 150) score = 150;
        snprintf(cands[n].url, sizeof cands[0].url, "%s", abs);
        cands[n].score = score;
        n++;
    }
    /* 去重 */
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (!strcmp(cands[i].url, cands[j].url)) { cands[j].score = -1; }
    /* 稳定按分数降序 */
    for (int i = 1; i < n; i++) {
        iconcand c = cands[i];
        int j = i - 1;
        while (j >= 0 && cands[j].score < c.score) { cands[j + 1] = cands[j]; j--; }
        cands[j + 1] = c;
    }
    return n;
}

/* ---------- 主入口 ---------- */

static int save_upload(const char *uploads_dir, const char *data, size_t n, const char *ext,
                       char *out_url, size_t cap) {
    unsigned char sum[16];
    md5(data, n, sum);
    char hex[33];
    np_to_hex(sum, 16, hex);
    char name[64];
    snprintf(name, sizeof name, "%s%s", hex, ext);
    char path[1280];
    np_path_join(path, sizeof path, uploads_dir, name);
    long sz;
    if (np_file_size(path, &sz) != 0) {
        if (np_write_file(path, data, n) != 0) return -1;
    }
    snprintf(out_url, cap, "/uploads/%s", name);
    return 0;
}

int favicon_fetch(const char *rawurl, const char *mode, const char *uploads_dir,
                  char *out_url, size_t cap, char *err, size_t errcap) {
    char url[1200];
    snprintf(url, sizeof url, "%s", rawurl);
    /* 去首尾空白 */
    char *s = url;
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) e--;
    *e = '\0';
    if (!*s) { snprintf(err, errcap, "缺少 url 参数"); return 400; }
    if (!strstr(s, "://")) {
        memmove(s + 8, s, strlen(s) + 1);
        memcpy(s, "https://", 8);
    }

    /* 解析 scheme://host */
    char *p3 = strstr(s, "://");
    if (!p3) { snprintf(err, errcap, "地址格式有误"); return 400; }
    char scheme[16];
    size_t sl = (size_t)(p3 - s);
    if (sl >= sizeof scheme) { snprintf(err, errcap, "地址格式有误"); return 400; }
    memcpy(scheme, s, sl);
    scheme[sl] = '\0';
    if (strcmp(scheme, "http") && strcmp(scheme, "https")) { snprintf(err, errcap, "地址格式有误"); return 400; }
    char host[512];
    char *hp = p3 + 3;
    char *he = hp;
    while (*he && *he != '/' && *he != '?' && *he != '#') he++;
    size_t hl = (size_t)(he - hp);
    if (!hl || hl >= sizeof host) { snprintf(err, errcap, "地址格式有误"); return 400; }
    memcpy(host, hp, hl);
    host[hl] = '\0';
    /* 主机名合法性（同时防命令注入） */
    for (char *q = host; *q; q++) {
        char c = *q;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-' || c == ':' || c == '[' || c == ']')) {
            snprintf(err, errcap, "地址格式有误");
            return 400;
        }
    }
    char origin[600];
    snprintf(origin, sizeof origin, "%s://%s", scheme, host);

    /* 1) 抓首页 HTML 收集候选 */
    iconcand cands[32];
    int nc = 0;
    char *html = NULL;
    size_t hlen = 0;
    char ct[128];
    char pageurl[700];
    snprintf(pageurl, sizeof pageurl, "%s/", origin);
    if (fetch_url(pageurl, &html, &hlen, ct, sizeof ct) == 0 && hlen) {
        nc = extract_icon_links(html, origin, scheme, cands, 32);
        free(html);
    }
    /* 2) 兜底通用路径 */
    if (nc < 32) snprintf(cands[nc].url, sizeof cands[0].url, "%s/favicon.ico", origin), cands[nc++].score = 0;
    if (nc < 32) snprintf(cands[nc].url, sizeof cands[0].url, "%s/favicon.png", origin), cands[nc++].score = 0;

    /* 逐个下载，必须确认为图片 */
    for (int i = 0; i < nc; i++) {
        if (cands[i].score < 0 || !cands[i].url[0]) continue;
        char *data = NULL;
        size_t dlen = 0;
        if (fetch_url(cands[i].url, &data, &dlen, ct, sizeof ct) != 0 || !dlen) {
            free(data);
            continue;
        }
        const char *ext = ext_from_mime(ct);
        if (!ext) ext = sniff_ext(data, dlen);
        if (!ext) { free(data); continue; }

        if (mode && !strcmp(mode, "link")) {
            snprintf(out_url, cap, "%s", cands[i].url);
            free(data);
            return 0;
        }
        int rc = save_upload(uploads_dir, data, dlen, ext, out_url, cap);
        free(data);
        if (rc != 0) { snprintf(err, errcap, "保存文件失败"); return 500; }
        return 0;
    }
    snprintf(err, errcap, "未找到图标");
    return 404;
}

/* ---------- 标签属性提取（与 http.c 的实现独立，避免耦合） ---------- */

static int attr_get(const char *tag, const char *name, char *out, size_t cap) {
    size_t nl = strlen(name);
    const char *p = tag;
    while ((p = np_strcasestr(p, name)) != NULL) {
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
