/* NavPage C 版：单文件导航页服务
 * 端口 -p/-port、数据目录 -d/-data、账号 -u/-user、密码 -pwd/-password
 * 配置导入地址 -url（预填前端账号设置的下载框）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "plat.h"
#include "util.h"
#include "crypto.h"
#include "json.h"
#include "store.h"
#include "http.h"
#include "favicon.h"
#include "fetch.h"
#include "assets.h"

#define COOKIE_NAME "np_session"
#define MAX_JSON_BODY (5 * 1024 * 1024)
#define MAX_UPLOAD (10 * 1024 * 1024)

static store *g_store;
static char g_data_dir[1024];
/* 启动参数 -url 指定的配置导入地址，通过 /api/bootstrap 下发给前端 */
static char g_boot_import_url[1024];

/* ---------- 参数与环境 ---------- */

static char *env_or(const char *key, const char *def) {
    const char *v = getenv(key);
    if (v && *v) {
        while (*v == ' ' || *v == '\t') v++;
        if (*v) return (char *)v;
    }
    return (char *)def;
}

/* 等价匹配 -x 与 --x，支持空格与 = 两种赋值 */
static const char *flag_value(int argc, char **argv, const char *name) {
    size_t nl = strlen(name);
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-' || !argv[i][1]) continue;
        const char *cur = argv[i];
        while (*cur == '-') cur++;
        if (!strncmp(cur, name, nl)) {
            if (cur[nl] == '=') return cur + nl + 1;
            if (!cur[nl] && i + 1 < argc) return argv[i + 1];
        }
    }
    return NULL;
}

static int clean_arg(const char *src, char *out, size_t cap);

static void flag_str(int argc, char **argv, const char *n1, const char *n2, char *out, size_t cap) {
    const char *v = flag_value(argc, argv, n1);
    if (!v) v = flag_value(argc, argv, n2);
    if (v) clean_arg(v, out, cap);
}

/* 参数清理：去首尾空白；若整体被同种引号包裹（"..." 或 '...'）则去掉引号，
 * 兼容部分启动器把引号一并传入的情况。返回 1 成功写入 out，0 表示参数为空/非法。 */
static int clean_arg(const char *src, char *out, size_t cap) {
    if (!src) return 0;
    while (*src == ' ' || *src == '\t') src++;
    size_t n = strlen(src);
    while (n && (src[n - 1] == ' ' || src[n - 1] == '\t')) n--;
    if (n >= 2 && (src[0] == '"' || src[0] == '\'') && src[n - 1] == src[0]) {
        src++;
        n -= 2;
    }
    if (!n || n >= cap) return 0;
    memcpy(out, src, n);
    out[n] = '\0';
    return 1;
}

/* URL 额外禁止控制字符：换行等会破坏 curl 配置文件 */
static int clean_url_arg(const char *src, char *out, size_t cap) {
    if (!clean_arg(src, out, cap)) return 0;
    for (const char *c = out; *c; c++) {
        if ((unsigned char)*c < 0x20) {
            out[0] = '\0';
            return 0;
        }
    }
    return 1;
}

/* ---------- 响应助手 ---------- */

static const char *status_text(int code) {
    switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 500: return "Internal Server Error";
    default:  return "OK";
    }
}

static void send_json(np_sock fd, http_req *r, int code, sb *body, const char *extra) {
    http_send(fd, code, status_text(code), extra,
              "application/json; charset=utf-8", "no-cache",
              body->s, body->len, r->keep_alive);
    sb_free(body);
}

static void send_ok(np_sock fd, http_req *r) {
    sb b;
    sb_init(&b);
    sb_puts(&b, "{\"ok\":true}");
    send_json(fd, r, 200, &b, NULL);
}

static void send_err(np_sock fd, http_req *r, int code, const char *msg) {
    sb b;
    sb_init(&b);
    sb_puts(&b, "{\"error\":\"");
    sb_put_json_escaped(&b, msg);
    sb_puts(&b, "\"}");
    send_json(fd, r, code, &b, NULL);
}

/* 请求体解析为 JSON；失败返回 NULL 并已回错 */
static jval *read_json(np_sock fd, http_req *r) {
    if (r->body_len > MAX_JSON_BODY) {
        send_err(fd, r, 400, "请求格式错误");
        return NULL;
    }
    if (!r->body || !r->body_len) {
        send_err(fd, r, 400, "请求格式错误");
        return NULL;
    }
    jval *v = json_parse(r->body, r->body_len);
    if (!v) {
        send_err(fd, r, 400, "请求格式错误");
        return NULL;
    }
    return v;
}

/* ---------- 鉴权与网络判定 ---------- */

static int authed(http_req *r) {
    return store_check_token(g_store, req_cookie(r, COOKIE_NAME));
}

static int is_private_ip(const char *ip) {
    unsigned a, b, c, d;
    if (sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 && a <= 255 && b <= 255) {
        return a == 10 || a == 127 ||
               (a == 192 && b == 168) ||
               (a == 172 && b >= 16 && b <= 31) ||
               (a == 169 && b == 254);
    }
    /* IPv6 */
    if (!strcmp(ip, "::1")) return 1;
    if (strstr(ip, "::ffff:")) return is_private_ip(strstr(ip, "::ffff:") + 7);
    if (!strncmp(ip, "fe8", 3) || !strncmp(ip, "fe9", 3) ||
        !strncmp(ip, "fea", 3) || !strncmp(ip, "feb", 3)) return 1; /* 链路本地 */
    if (ip[0] == 'f' && (ip[1] == 'c' || ip[1] == 'd')) return 1;   /* ULA */
    return 0;
}

static const char *client_ip(http_req *r) {
    const char *xff = req_header(r, "X-Forwarded-For");
    if (xff && *xff) {
        static char buf[64];
        size_t n = 0;
        while (xff[n] && xff[n] != ',' && n < sizeof buf - 1) { buf[n] = xff[n]; n++; }
        buf[n] = '\0';
        char *t = buf;
        while (*t == ' ') t++;
        if (*t) return t;
    }
    const char *xr = req_header(r, "X-Real-IP");
    if (xr && *xr) return xr;
    return r->peer;
}

static const char *detect_net_mode(http_req *r) {
    return is_private_ip(client_ip(r)) ? "lan" : "wan";
}

static int visitor_mode(void) {
    np_mutex_lock(g_store->mu);
    int v = jbool(jobj_get(jobj_get(g_store->cfg, "settings"), "visitor_mode"), 0);
    np_mutex_unlock(g_store->mu);
    return v;
}

/* ---------- 静态资源 ---------- */

static const char *mime_of(const char *path) {
    if (np_has_suffix_ci(path, ".html")) return "text/html; charset=utf-8";
    if (np_has_suffix_ci(path, ".css")) return "text/css; charset=utf-8";
    if (np_has_suffix_ci(path, ".js")) return "application/javascript; charset=utf-8";
    if (np_has_suffix_ci(path, ".png")) return "image/png";
    if (np_has_suffix_ci(path, ".jpg") || np_has_suffix_ci(path, ".jpeg")) return "image/jpeg";
    if (np_has_suffix_ci(path, ".webp")) return "image/webp";
    if (np_has_suffix_ci(path, ".gif")) return "image/gif";
    if (np_has_suffix_ci(path, ".svg")) return "image/svg+xml";
    if (np_has_suffix_ci(path, ".ico")) return "image/x-icon";
    if (np_has_suffix_ci(path, ".bmp")) return "image/bmp";
    return "application/octet-stream";
}

/* 默认壁纸：按文件名主干 default-bg 查找（不限定图片后缀），返回实际资源路径；无匹配返回 NULL */
static const char *default_wallpaper(void) {
    static const char *pre = "/assets/default-bg";
    const char *best = NULL;
    for (int i = 0; i < np_asset_count; i++) {
        const char *p = np_assets[i].path;
        if (strncmp(p, pre, strlen(pre))) continue;
        const char *rest = p + strlen(pre);
        if (*rest != '.') continue; /* default-bg1 等衍生图不算默认壁纸 */
        if (!(np_has_suffix_ci(p, ".png") || np_has_suffix_ci(p, ".jpg") ||
              np_has_suffix_ci(p, ".jpeg") || np_has_suffix_ci(p, ".webp"))) continue;
        if (!best || strcmp(p, best) < 0) best = p;
    }
    return best;
}

static void serve_embedded(np_sock fd, http_req *r, const char *path) {
    const char *lookup = !strcmp(path, "/") ? "/index.html" : path;
    /* 默认壁纸别名：/assets/default-bg 不限定后缀，解析到实际文件后按真实路径输出 */
    if (!strcmp(lookup, "/assets/default-bg")) {
        lookup = default_wallpaper();
        if (!lookup) {
            send_err(fd, r, 404, "未找到资源");
            return;
        }
    }
    for (int i = 0; i < np_asset_count; i++) {
        if (!strcmp(np_assets[i].path, lookup)) {
            const char *cache = np_has_prefix(lookup, "/assets/")
                ? "public, max-age=31536000, immutable" : "no-cache";
            http_send(fd, 200, "OK", NULL, mime_of(lookup), cache,
                      np_assets[i].data, (size_t)np_assets[i].size, r->keep_alive);
            return;
        }
    }
    send_err(fd, r, 404, "未找到资源");
}

static void serve_upload(np_sock fd, http_req *r, const char *name) {
    if (!*name || !strcmp(name, ".") || !strcmp(name, "..") || strchr(name, '/') || strchr(name, '\\')) {
        send_err(fd, r, 400, "非法文件名");
        return;
    }
    char path[1280];
    char updir[1280];
    np_path_join(updir, sizeof updir, g_data_dir, "uploads");
    np_path_join(path, sizeof path, updir, name);
    char *data = NULL;
    size_t len = 0;
    if (np_read_file(path, &data, &len) != 0) {
        send_err(fd, r, 404, "未找到文件");
        return;
    }
    http_send(fd, 200, "OK", NULL, mime_of(name),
              "public, max-age=31536000, immutable", data, len, r->keep_alive);
    free(data);
}

/* ---------- API 处理器 ---------- */

static void handle_login(np_sock fd, http_req *r) {
    jval *req = read_json(fd, r);
    if (!req) return;
    const char *user = jstr(jobj_get(req, "username"));
    const char *pw = jstr(jobj_get(req, "password"));
    if (!store_verify_login(g_store, user ? user : "", pw ? pw : "")) {
        json_free(req);
        send_err(fd, r, 401, "账号或密码错误");
        return;
    }
    json_free(req);
    char token[256];
    if (store_new_token(g_store, token, sizeof token) != 0) {
        send_err(fd, r, 500, "生成会话失败");
        return;
    }
    char cookie[512];
    snprintf(cookie, sizeof cookie,
             "Set-Cookie: %s=%s; Path=/; HttpOnly; Max-Age=%lld; SameSite=Lax\r\n",
             COOKIE_NAME, token, (long long)NP_SESSION_TTL);
    sb b;
    sb_init(&b);
    sb_puts(&b, "{\"ok\":true}");
    send_json(fd, r, 200, &b, cookie);
}

static void handle_logout(np_sock fd, http_req *r) {
    char cookie[128];
    snprintf(cookie, sizeof cookie,
             "Set-Cookie: %s=; Path=/; Max-Age=0; HttpOnly\r\n", COOKIE_NAME);
    sb b;
    sb_init(&b);
    sb_puts(&b, "{\"ok\":true}");
    send_json(fd, r, 200, &b, cookie);
}

static void handle_session(np_sock fd, http_req *r) {
    int is_auth = authed(r);
    int visitor = visitor_mode();
    if (!is_auth && !visitor) {
        send_err(fd, r, 401, "未登录");
        return;
    }
    sb b;
    sb_init(&b);
    sb_printf(&b, "{\"authed\":%s,\"visitor_mode\":%s}",
              is_auth ? "true" : "false", visitor ? "true" : "false");
    send_json(fd, r, 200, &b, NULL);
}

static void handle_get_config(np_sock fd, http_req *r) {
    int is_auth = authed(r);
    if (!is_auth && !visitor_mode()) {
        send_err(fd, r, 401, "未登录");
        return;
    }
    const char *net = detect_net_mode(r);
    sb b;
    sb_init(&b);
    np_mutex_lock(g_store->mu);
    const char *username = jstr(jobj_get(jobj_get(g_store->cfg, "account"), "username"));
    int visitor = jbool(jobj_get(jobj_get(g_store->cfg, "settings"), "visitor_mode"), 0);
    sb_puts(&b, "{\"username\":\"");
    sb_put_json_escaped(&b, username ? username : "");
    sb_puts(&b, "\"");
    sb_printf(&b, ",\"visitor_mode\":%s,\"authed\":%s,\"net_mode\":\"%s\",\"settings\":",
              visitor ? "true" : "false", is_auth ? "true" : "false", net);
    json_write(&b, jobj_get(g_store->cfg, "settings"), -1);
    sb_puts(&b, ",\"groups\":");
    json_write(&b, jobj_get(g_store->cfg, "groups"), -1);
    sb_puts(&b, ",\"bookmark_groups\":");
    json_write(&b, jobj_get(g_store->cfg, "bookmark_groups"), -1);
    sb_puts(&b, "}");
    np_mutex_unlock(g_store->mu);
    send_json(fd, r, 200, &b, NULL);
}

static void handle_appearance(np_sock fd, http_req *r) {
    sb b;
    sb_init(&b);
    np_mutex_lock(g_store->mu);
    json_write(&b, jobj_get(g_store->cfg, "settings"), -1);
    np_mutex_unlock(g_store->mu);
    send_json(fd, r, 200, &b, NULL);
}

static void handle_wallpapers(np_sock fd, http_req *r) {
    const char *items[64];
    int n = 0;
    for (int i = 0; i < np_asset_count && n < 64; i++) {
        const char *p = np_assets[i].path;
        if (np_has_prefix(p, "/assets/default-bg") &&
            (np_has_suffix_ci(p, ".png") || np_has_suffix_ci(p, ".jpg") ||
             np_has_suffix_ci(p, ".jpeg") || np_has_suffix_ci(p, ".webp"))) {
            items[n++] = p;
        }
    }
    /* 排序，默认壁纸（主干 default-bg，不限后缀）固定第一 */
    for (int i = 1; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (strcmp(items[j], items[i]) < 0) {
                const char *t = items[i]; items[i] = items[j]; items[j] = t;
            }
    const char *def = default_wallpaper();
    if (def) {
        for (int i = 0; i < n; i++)
            if (!strcmp(items[i], def)) {
                for (int j = i; j > 0; j--) items[j] = items[j - 1];
                items[0] = def;
                break;
            }
    }

    sb b;
    sb_init(&b);
    sb_puts(&b, "{\"items\":[");
    for (int i = 0; i < n; i++) {
        if (i) sb_putc(&b, ',');
        sb_putc(&b, '"');
        sb_put_json_escaped(&b, items[i]);
        sb_putc(&b, '"');
    }
    sb_puts(&b, "]}");
    send_json(fd, r, 200, &b, NULL);
}

static void handle_put_config(np_sock fd, http_req *r) {
    if (!authed(r)) {
        send_err(fd, r, 401, "未登录");
        return;
    }
    jval *req = read_json(fd, r);
    if (!req) return;
    if (req->t != JV_OBJ) {
        json_free(req);
        send_err(fd, r, 400, "请求格式错误");
        return;
    }
    np_mutex_lock(g_store->mu);
    static const char *keys[] = { "settings", "groups", "bookmark_groups" };
    for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        jval *nv = jobj_get(req, keys[i]);
        if (nv && nv->t != JV_NULL) {
            jval *taken = jobj_remove(req, keys[i]); /* 摘除，转移所有权 */
            jobj_set(g_store->cfg, keys[i], taken);
        }
    }
    int ok = store_save(g_store) == 0;
    np_mutex_unlock(g_store->mu);
    json_free(req);
    if (ok) send_ok(fd, r);
    else send_err(fd, r, 500, "保存失败");
}

static void handle_account(np_sock fd, http_req *r) {
    if (!authed(r)) {
        send_err(fd, r, 401, "未登录");
        return;
    }
    jval *req = read_json(fd, r);
    if (!req) return;
    const char *oldpw = jstr(jobj_get(req, "old_password"));
    const char *username = jstr(jobj_get(req, "username"));
    const char *newpw = jstr(jobj_get(req, "new_password"));
    if (!username || !*username) {
        json_free(req);
        send_err(fd, r, 400, "用户名不能为空");
        return;
    }
    if (newpw && *newpw && strlen(newpw) < 6) {
        json_free(req);
        send_err(fd, r, 400, "新密码至少 6 位");
        return;
    }
    int ok = store_change_password(g_store, oldpw ? oldpw : "", username, newpw ? newpw : "");
    json_free(req);
    if (ok) send_ok(fd, r);
    else send_err(fd, r, 403, "旧密码错误");
}

static void handle_upload(np_sock fd, http_req *r) {
    if (!authed(r)) {
        send_err(fd, r, 401, "未登录");
        return;
    }
    if (r->body_len > MAX_UPLOAD) {
        send_err(fd, r, 400, "文件过大或格式错误");
        return;
    }
    const char *fname = NULL, *fdata = NULL;
    size_t flen = 0;
    if (multipart_file(r, &fname, &fdata, &flen) != 0) {
        send_err(fd, r, 400, "未找到文件");
        return;
    }
    /* 扩展名白名单（精确匹配） */
    const char *ext = strrchr(fname, '.');
    if (!ext || !ext[1]) {
        free((void *)fname);
        send_err(fd, r, 400, "仅支持图片格式");
        return;
    }
    char lowext[8];
    snprintf(lowext, sizeof lowext, "%s", ext);
    for (char *p = lowext; *p; p++) *p = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
    static const char *okexts[] = { ".png", ".jpg", ".jpeg", ".gif", ".webp", ".svg", ".ico", ".bmp" };
    int okext = 0;
    for (unsigned i = 0; i < sizeof okexts / sizeof okexts[0]; i++)
        if (!strcmp(lowext, okexts[i])) { okext = 1; break; }
    if (!okext) {
        free((void *)fname);
        send_err(fd, r, 400, "仅支持图片格式");
        return;
    }

    unsigned char sum[16];
    md5(fdata, flen, sum);
    char hex[33];
    np_to_hex(sum, 16, hex);
    char name[64];
    snprintf(name, sizeof name, "%s%s", hex, lowext);

    char updir[1280];
    np_path_join(updir, sizeof updir, g_data_dir, "uploads");
    np_mkdir_p(updir);
    char path[1536];
    np_path_join(path, sizeof path, updir, name);
    long sz;
    if (np_file_size(path, &sz) != 0 && np_write_file(path, fdata, flen) != 0) {
        free((void *)fname);
        send_err(fd, r, 500, "保存文件失败");
        return;
    }
    free((void *)fname);
    sb b;
    sb_init(&b);
    sb_printf(&b, "{\"url\":\"/uploads/%s\"}", name);
    send_json(fd, r, 200, &b, NULL);
}

static void handle_favicon_api(np_sock fd, http_req *r) {
    if (!authed(r)) {
        send_err(fd, r, 401, "未登录");
        return;
    }
    const char *raw = req_query(r, "url");
    const char *mode = req_query(r, "mode");
    if (!raw || !*raw) {
        send_err(fd, r, 400, "缺少 url 参数");
        return;
    }
    char updir[1280];
    np_path_join(updir, sizeof updir, g_data_dir, "uploads");
    np_mkdir_p(updir);
    char out[1024], err[128];
    int rc = favicon_fetch(raw, mode, updir, out, sizeof out, err, sizeof err);
    if (rc == 0) {
        sb b;
        sb_init(&b);
        sb_puts(&b, "{\"url\":\"");
        sb_put_json_escaped(&b, out);
        sb_puts(&b, "\"}");
        send_json(fd, r, 200, &b, NULL);
    } else {
        send_err(fd, r, rc, err);
    }
}

static void handle_list_files(np_sock fd, http_req *r) {
    if (!authed(r)) {
        send_err(fd, r, 401, "未登录");
        return;
    }
    char updir[1280];
    np_path_join(updir, sizeof updir, g_data_dir, "uploads");
    sb b;
    sb_init(&b);
    sb_putc(&b, '[');
    DIR *d = opendir(updir);
    if (d) {
        struct dirent *e;
        int first = 1;
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char path[1600];
            np_path_join(path, sizeof path, updir, e->d_name);
            struct stat st;
            if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) continue;
            if (!first) sb_putc(&b, ',');
            first = 0;
            sb_puts(&b, "{\"name\":\"");
            sb_put_json_escaped(&b, e->d_name);
            sb_printf(&b, "\",\"size\":%ld,\"url\":\"/uploads/", (long)st.st_size);
            sb_put_json_escaped(&b, e->d_name);
            sb_puts(&b, "\"}");
        }
        closedir(d);
    }
    sb_putc(&b, ']');
    send_json(fd, r, 200, &b, NULL);
}

static void handle_delete_file(np_sock fd, http_req *r) {
    if (!authed(r)) {
        send_err(fd, r, 401, "未登录");
        return;
    }
    const char *name = r->path + strlen("/api/files/");
    if (!*name || !strcmp(name, ".") || !strcmp(name, "..") || strchr(name, '/') || strchr(name, '\\')) {
        send_err(fd, r, 400, "非法文件名");
        return;
    }
    char updir[1280], path[1600];
    np_path_join(updir, sizeof updir, g_data_dir, "uploads");
    np_path_join(path, sizeof path, updir, name);
#ifdef _WIN32
    if (DeleteFileA(path) == 0) {
        send_err(fd, r, 500, "删除失败");
        return;
    }
#else
    if (remove(path) != 0) {
        send_err(fd, r, 500, "删除失败");
        return;
    }
#endif
    send_ok(fd, r);
}

/* 下发启动参数给前端（如 -url 指定的配置导入地址） */
static void handle_bootstrap(np_sock fd, http_req *r) {
    sb b;
    sb_init(&b);
    sb_puts(&b, "{\"import_url\":\"");
    sb_put_json_escaped(&b, g_boot_import_url);
    sb_puts(&b, "\"}");
    send_json(fd, r, 200, &b, NULL);
}

/* 从指定 URL 下载导出的配置 JSON 并整体覆盖落盘。
 * 与前端"导入配置"的文件方式效果一致，只是来源从本地文件换成远程 URL。 */
static void handle_import_from_url(np_sock fd, http_req *r) {
    if (!authed(r)) {
        send_err(fd, r, 401, "未登录");
        return;
    }
    jval *req = read_json(fd, r);
    if (!req) return;
    const char *url = jstr(jobj_get(req, "url"));
    if (!url || !*url) {
        json_free(req);
        send_err(fd, r, 400, "URL 不能为空");
        return;
    }
    /* 仅允许 http/https，避免 file:// 等读取本地文件 */
    if (!np_has_prefix(url, "http://") && !np_has_prefix(url, "https://")) {
        json_free(req);
        send_err(fd, r, 400, "仅支持 http/https URL");
        return;
    }
    /* 含换行等控制字符的 URL 会破坏 curl 配置文件，直接拒绝 */
    for (const char *c = url; *c; c++) {
        if ((unsigned char)*c < 0x20) {
            json_free(req);
            send_err(fd, r, 400, "URL 含非法字符");
            return;
        }
    }
    char *body = NULL;
    size_t blen = 0;
    if (np_fetch_url(url, &body, &blen, NULL, 0) != 0) {
        json_free(req);
        send_err(fd, r, 502, "下载失败");
        return;
    }
    json_free(req);

    jval *obj = json_parse(body, blen);
    free(body);
    if (!obj || obj->t != JV_OBJ) {
        if (obj) json_free(obj);
        send_err(fd, r, 400, "JSON 解析失败");
        return;
    }
    jval *groups = jobj_get(obj, "groups");
    if (!groups || groups->t != JV_ARR) {
        json_free(obj);
        send_err(fd, r, 400, "文件格式不对：缺少 groups");
        return;
    }

    /* 覆盖 settings/groups/bookmark_groups：仅导入文件中存在的键，未提供的保持现值 */
    np_mutex_lock(g_store->mu);
    static const char *keys[] = { "settings", "groups", "bookmark_groups" };
    for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        jval *nv = jobj_get(obj, keys[i]);
        if (nv && nv->t != JV_NULL) {
            jval *taken = jobj_remove(obj, keys[i]); /* 摘除，转移所有权 */
            jobj_set(g_store->cfg, keys[i], taken);
        }
    }
    int ok = store_save(g_store) == 0;
    np_mutex_unlock(g_store->mu);
    json_free(obj);
    if (ok) send_ok(fd, r);
    else send_err(fd, r, 500, "保存失败");
}

/* ---------- 路由 ---------- */

static void dispatch(np_sock fd, http_req *r) {
    const char *m = r->method;
    const char *p = r->path;

    if (!strcmp(p, "/api/login") && !strcmp(m, "POST")) return handle_login(fd, r);
    if (!strcmp(p, "/api/logout") && !strcmp(m, "POST")) return handle_logout(fd, r);
    if (!strcmp(p, "/api/session") && !strcmp(m, "GET")) return handle_session(fd, r);
    if (!strcmp(p, "/api/config") && !strcmp(m, "GET")) return handle_get_config(fd, r);
    if (!strcmp(p, "/api/appearance") && !strcmp(m, "GET")) return handle_appearance(fd, r);
    if (!strcmp(p, "/api/bootstrap") && !strcmp(m, "GET")) return handle_bootstrap(fd, r);
    if (!strcmp(p, "/api/wallpapers") && !strcmp(m, "GET")) return handle_wallpapers(fd, r);
    if (!strcmp(p, "/api/config") && !strcmp(m, "PUT")) return handle_put_config(fd, r);
    if (!strcmp(p, "/api/account") && !strcmp(m, "PUT")) return handle_account(fd, r);
    if (!strcmp(p, "/api/import-from-url") && !strcmp(m, "POST")) return handle_import_from_url(fd, r);
    if (!strcmp(p, "/api/upload") && !strcmp(m, "POST")) return handle_upload(fd, r);
    if (!strcmp(p, "/api/favicon") && !strcmp(m, "GET")) return handle_favicon_api(fd, r);
    if (!strcmp(p, "/api/files") && !strcmp(m, "GET")) return handle_list_files(fd, r);
    if (np_has_prefix(p, "/api/files/") && !strcmp(m, "DELETE")) return handle_delete_file(fd, r);

    if (np_has_prefix(p, "/uploads/")) return serve_upload(fd, r, p + strlen("/uploads/"));

    /* 前端静态资源（内嵌） */
    return serve_embedded(fd, r, p);
}

/* ---------- 连接线程 ---------- */

typedef struct {
    np_sock fd;
    struct sockaddr_in addr;
} conn_arg;

static void conn_thread(void *arg) {
    conn_arg ca = *(conn_arg *)arg;
    free(arg);

    /* 30 秒读超时，兼顾 keep-alive 空闲 */
#ifdef _WIN32
    DWORD ms = 30000;
    setsockopt(ca.fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms);
#else
    struct timeval tv = { 30, 0 };
    setsockopt(ca.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
    int one = 1;
#ifndef TCP_NODELAY
#define TCP_NODELAY 1
#endif
    setsockopt(ca.fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);

    char peer[64] = "";
    inet_ntop(AF_INET, &ca.addr.sin_addr, peer, sizeof peer);

    np_buf pending = { NULL, 0, 0 };
    http_req req;
    while (http_read_request(ca.fd, &pending, &req) == 0) {
        snprintf(req.peer, sizeof req.peer, "%s", peer);
        dispatch(ca.fd, &req);
        int keep = req.keep_alive;
        http_req_free(&req);
        if (!keep) break;
    }
    free(pending.s);
    np_closesocket(ca.fd);
}

/* ---------- main ---------- */

int main(int argc, char **argv) {
    char port[16] = "", data[1024] = "", user[256] = "", pwd[256] = "";
    snprintf(port, sizeof port, "%s", env_or("PORT", "8080"));
    snprintf(data, sizeof data, "%s", env_or("DATA_DIR", "data"));
    flag_str(argc, argv, "port", "p", port, sizeof port);
    flag_str(argc, argv, "data", "d", data, sizeof data);
    flag_str(argc, argv, "user", "u", user, sizeof user);
    flag_str(argc, argv, "password", "pwd", pwd, sizeof pwd);
    /* 配置导入地址：-url，预填前端账号设置的下载框便于一键导入 */
    clean_url_arg(flag_value(argc, argv, "url"), g_boot_import_url, sizeof g_boot_import_url);

    snprintf(g_data_dir, sizeof g_data_dir, "%s", data);
    g_store = store_open(g_data_dir);
    if (!g_store) {
        fprintf(stderr, "加载配置失败\n");
        return 1;
    }
    if (*user || *pwd) {
        store_set_account(g_store, user, pwd);
        printf("已将命令行指定的用户名/密码写入配置文件\n");
    }

    if (np_net_init() != 0) {
        fprintf(stderr, "网络初始化失败\n");
        return 1;
    }

    np_sock lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd == NP_BAD_SOCK) {
        fprintf(stderr, "创建 socket 失败\n");
        return 1;
    }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)atoi(port));
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        fprintf(stderr, "端口 %s 绑定失败\n", port);
        return 1;
    }
    if (listen(lfd, 64) != 0) {
        fprintf(stderr, "监听失败\n");
        return 1;
    }
    printf("服务启动:  http://0.0.0.0:%s\n", port);
    fflush(stdout);

    while (1) {
        struct sockaddr_in caddr;
        socklen_t clen = (socklen_t)sizeof caddr;
        np_sock cfd = accept(lfd, (struct sockaddr *)&caddr, &clen);
        if (cfd == NP_BAD_SOCK) continue;
        conn_arg *ca = malloc(sizeof *ca);
        if (!ca) { np_closesocket(cfd); continue; }
        ca->fd = cfd;
        ca->addr = caddr;
        if (np_thread_start(conn_thread, ca) != 0) {
            np_closesocket(cfd);
            free(ca);
        }
    }
    return 0;
}
