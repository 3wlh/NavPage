#include "store.h"
#include "crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- 账号与令牌 ---------- */

static void new_salt(char out[33]) {
    unsigned char b[16];
    np_rand_bytes(b, 16);
    np_to_hex(b, 16, out);
}

/* 加盐多轮 SHA-256，与 Go 版保持一致：h0=SHA256(salt:pw)，再迭代 9999 次 */
static void hash_password(const char *pw, const char *salt, char out[65]) {
    char seed[512];
    snprintf(seed, sizeof seed, "%s:%s", salt, pw);
    unsigned char h[32];
    sha256(seed, strlen(seed), h);
    for (int i = 0; i < 9999; i++) sha256(h, 32, h);
    np_to_hex(h, 32, out);
}

/* 全局会话密钥：HMAC(salt, "navpage-session:"+hash)，改密码即换钥，旧令牌全部失效 */
static void session_key(const char *salt, const char *hash, unsigned char out[32]) {
    char msg[256];
    snprintf(msg, sizeof msg, "navpage-session:%s", hash);
    hmac_sha256(salt, strlen(salt), msg, strlen(msg), out);
}

static void session_sig(const char *salt, const char *hash, const char *payload, char *out, size_t cap) {
    unsigned char key[32], sig[32];
    session_key(salt, hash, key);
    hmac_sha256(key, 32, payload, strlen(payload), sig);
    b64url(sig, 32, out);
    (void)cap;
}

int store_verify_login(store *s, const char *user, const char *pw) {
    np_mutex_lock(s->mu);
    const char *un = jstr(jobj_get(jobj_get(s->cfg, "account"), "username"));
    const char *salt = jstr(jobj_get(jobj_get(s->cfg, "account"), "salt"));
    const char *hash = jstr(jobj_get(jobj_get(s->cfg, "account"), "hash"));
    int ok = un && salt && hash && user && pw && !strcmp(un, user);
    if (ok) {
        char h[65];
        hash_password(pw, salt, h);
        ok = !strcmp(h, hash);
    }
    np_mutex_unlock(s->mu);
    return ok;
}

int store_new_token(store *s, char *out, size_t cap) {
    unsigned char dev[16];
    np_rand_bytes(dev, 16);
    char devhex[33];
    np_to_hex(dev, 16, devhex);

    np_mutex_lock(s->mu);
    const char *salt = jstr(jobj_get(jobj_get(s->cfg, "account"), "salt"));
    const char *hash = jstr(jobj_get(jobj_get(s->cfg, "account"), "hash"));
    int ok = salt && hash;
    if (ok) {
        char payload[128], sig[64];
        snprintf(payload, sizeof payload, "%s.%lld", devhex, (long long)(np_now() + NP_SESSION_TTL));
        session_sig(salt, hash, payload, sig, sizeof sig);
        snprintf(out, cap, "%s.%s", payload, sig);
    }
    np_mutex_unlock(s->mu);
    return ok ? 0 : -1;
}

int store_check_token(store *s, const char *token) {
    if (!token) return 0;
    /* 令牌格式：dev.exp.sig */
    const char *d1 = strchr(token, '.');
    if (!d1) return 0;
    const char *d2 = strchr(d1 + 1, '.');
    if (!d2) return 0;
    if (d1 == token || d2 == d1 + 1 || !d2[1]) return 0;
    char *end = NULL;
    long long exp = strtoll(d1 + 1, &end, 10);
    if (end != d2 || (long long)np_now() > exp) return 0;

    np_mutex_lock(s->mu);
    const char *salt = jstr(jobj_get(jobj_get(s->cfg, "account"), "salt"));
    const char *hash = jstr(jobj_get(jobj_get(s->cfg, "account"), "hash"));
    int ok = salt && hash;
    if (ok) {
        size_t plen = (size_t)(d2 - token);
        char payload[128];
        if (plen >= sizeof payload) { ok = 0; }
        else {
            memcpy(payload, token, plen);
            payload[plen] = '\0';
            char sig[64];
            session_sig(salt, hash, payload, sig, sizeof sig);
            /* 常数时间比较 */
            const unsigned char *a = (const unsigned char *)sig;
            const unsigned char *b = (const unsigned char *)d2 + 1;
            volatile unsigned diff = 0;
            size_t la = strlen(sig), lb = strlen(d2 + 1);
            size_t n = la > lb ? la : lb;
            for (size_t i = 0; i < n; i++) {
                diff |= (i < la ? a[i] : 0) ^ (i < lb ? b[i] : 0);
            }
            ok = diff == 0;
        }
    }
    np_mutex_unlock(s->mu);
    return ok;
}

int store_change_password(store *s, const char *oldpw, const char *user, const char *newpw) {
    np_mutex_lock(s->mu);
    jval *acct = jobj_get(s->cfg, "account");
    const char *salt = jstr(jobj_get(acct, "salt"));
    const char *hash = jstr(jobj_get(acct, "hash"));
    int ok = salt && hash;
    if (ok) {
        char h[65];
        hash_password(oldpw ? oldpw : "", salt, h);
        ok = !strcmp(h, hash);
    }
    if (ok) {
        char nsalt[33], nhash[65];
        new_salt(nsalt);
        hash_password(newpw ? newpw : "", nsalt, nhash);
        jobj_set(acct, "username", jnew_str(user));
        jobj_set(acct, "salt", jnew_str(nsalt));
        jobj_set(acct, "hash", jnew_str(nhash));
        ok = store_save(s) == 0;
    }
    np_mutex_unlock(s->mu);
    return ok;
}

/* 命令行账号覆盖：空值表示该项不修改；值相同时跳过，避免每次启动都换 salt 导致旧 token 失效 */
void store_set_account(store *s, const char *user, const char *pw) {
    if (!user && !pw) return;
    np_mutex_lock(s->mu);
    jval *acct = jobj_get(s->cfg, "account");
    int changed = 0;
    if (user && *user) {
        const char *cur_user = jstr(jobj_get(acct, "username"));
        if (!cur_user || strcmp(cur_user, user)) {
            jobj_set(acct, "username", jnew_str(user));
            changed = 1;
        }
    }
    if (pw && *pw) {
        const char *cur_salt = jstr(jobj_get(acct, "salt"));
        const char *cur_hash = jstr(jobj_get(acct, "hash"));
        char nhash[65];
        if (cur_salt && cur_hash) {
            hash_password(pw, cur_salt, nhash);
            if (!strcmp(nhash, cur_hash)) goto done_pw; /* 密码未变，跳过 */
        }
        char nsalt[33];
        new_salt(nsalt);
        hash_password(pw, nsalt, nhash);
        jobj_set(acct, "salt", jnew_str(nsalt));
        jobj_set(acct, "hash", jnew_str(nhash));
        changed = 1;
    }
done_pw:
    if (changed) store_save(s);
    np_mutex_unlock(s->mu);
}

/* ---------- 保存 ---------- */

static void store_path(store *s, char *dst, size_t cap) {
    np_path_join(dst, cap, s->dir, "config.json");
}

int store_save(store *s) {
    if (np_mkdir_p(s->dir) != 0) return -1;
    sb out;
    sb_init(&out);
    json_write(&out, s->cfg, 2);
    sb_putc(&out, '\n');
    char path[1280], tmp[1288];
    store_path(s, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int ok = np_write_file(tmp, out.s, out.len) == 0 &&
             np_replace_file(tmp, path) == 0;
    sb_free(&out);
    return ok ? 0 : -1;
}

/* ---------- 默认配置 ---------- */

static jval *mkitem(double id, const char *name, const char *url, const char *desc) {
    jval *it = jnew_obj();
    jobj_set(it, "id", jnew_num(id));
    jobj_set(it, "name", jnew_str(name));
    jobj_set(it, "url", jnew_str(url));
    jobj_set(it, "intranet_url", jnew_str(""));
    jobj_set(it, "desc", jnew_str(desc ? desc : ""));
    jobj_set(it, "emoji", jnew_str(""));
    jobj_set(it, "icon_url", jnew_str(""));
    jobj_set(it, "icon_bg", jnew_str(""));
    jobj_set(it, "open_mode", jnew_str(""));
    return it;
}

static jval *mkgroup(double id, const char *name) {
    jval *g = jnew_obj();
    jobj_set(g, "id", jnew_num(id));
    jobj_set(g, "name", jnew_str(name));
    jobj_set(g, "items", jnew_arr());
    return g;
}

static jval *mklink(double id, const char *name, const char *url) {
    jval *l = jnew_obj();
    jobj_set(l, "id", jnew_num(id));
    jobj_set(l, "name", jnew_str(name));
    jobj_set(l, "url", jnew_str(url));
    return l;
}

static jval *mkbookmark(double id, const char *name) {
    jval *g = jnew_obj();
    jobj_set(g, "id", jnew_num(id));
    jobj_set(g, "name", jnew_str(name));
    jobj_set(g, "links", jnew_arr());
    return g;
}

static jval *default_config(void) {
    char salt[33], hash[65];
    new_salt(salt);
    hash_password("admin", salt, hash);

    jval *root = jnew_obj();

    jval *acct = jnew_obj();
    jobj_set(acct, "username", jnew_str("admin"));
    jobj_set(acct, "hash", jnew_str(hash));
    jobj_set(acct, "salt", jnew_str(salt));
    jobj_set(root, "account", acct);

    jval *st = jnew_obj();
    jobj_set(st, "title", jnew_str("我的导航"));

    jval *engines = jnew_arr();
    const char *es[][3] = {
        { "baidu", "百度", "https://www.baidu.com/s?wd=%s" },
        { "bing", "必应", "https://www.bing.com/search?q=%s" },
        { "google", "谷歌", "https://www.google.com/search?q=%s" },
    };
    for (int i = 0; i < 3; i++) {
        jval *e = jnew_obj();
        jobj_set(e, "id", jnew_str(es[i][0]));
        jobj_set(e, "name", jnew_str(es[i][1]));
        jobj_set(e, "url", jnew_str(es[i][2]));
        jarr_push(engines, e);
    }
    jobj_set(st, "engines", engines);
    jobj_set(st, "default_engine", jnew_str("baidu"));

    jval *wp = jnew_obj();
    jobj_set(wp, "type", jnew_str("image"));
    jobj_set(wp, "value", jnew_str("/assets/default-bg.png"));
    jobj_set(st, "wallpaper", wp);
    jobj_set(st, "wallpaper_grad", jnew_str(""));
    jobj_set(st, "accent", jnew_str("#667eea"));
    jobj_set(st, "visitor_mode", jnew_bool(0));
    jobj_set(st, "clock_seconds", jnew_bool(0));
    jobj_set(st, "show_search", jnew_null());
    jobj_set(st, "hide_desc", jnew_bool(0));
    jobj_set(st, "text_color", jnew_str(""));
    jobj_set(st, "icon_style", jnew_str(""));
    jobj_set(st, "wallpaper_blur", jnew_num(0));
    jobj_set(st, "wallpaper_mask", jnew_num(0));
    jobj_set(st, "card_blur", jnew_num(0));
    jobj_set(st, "content_max_width", jnew_num(0));
    jobj_set(st, "content_max_unit", jnew_str(""));
    jobj_set(st, "content_pad_x", jnew_num(0));
    jobj_set(st, "content_margin_top", jnew_num(8));
    jobj_set(st, "content_margin_bottom", jnew_num(0));
    jobj_set(st, "custom_footer", jnew_str(
        "<div class=\"flex justify-center\" style=\"margin-top:100px; color:#fff; font-size:14px;\">"
        "Powered By <a href=\"https://github.com/3wlh/NavPage\" target=\"_blank\" "
        "class=\"ml-[20px]\" style=\"color:#fff\">3wlh</a></div>"));
    jobj_set(st, "net_mode_switch", jnew_str("manual"));
    jobj_set(st, "icon_fetch_mode", jnew_str("link"));
    jobj_set(root, "settings", st);

    jval *groups = jnew_arr();
    jval *g1 = mkgroup(3, "Git仓库");
    jval *gi1 = jobj_get(g1, "items");
    jarr_push(gi1, mkitem(1, "GitHub", "https://github.com", "全球最大代码托管"));
    jarr_push(gi1, mkitem(2, "CND", "https://www.cnd.com", "内容分发网络"));
    jarr_push(gi1, mkitem(3, "Gitee", "https://gitee.com", "代码托管平台"));
    jarr_push(gi1, mkitem(4, "GitLink", "https://www.gitlink.org.cn", "代码托管平台"));
    jval *g2 = mkgroup(2, "视频网站");
    jval *gi2 = jobj_get(g2, "items");
    jarr_push(gi2, mkitem(1, "腾讯视频", "https://v.qq.com/", ""));
    jarr_push(gi2, mkitem(2, "哔哩哔哩", "https://www.bilibili.com", ""));
    jarr_push(gi2, mkitem(3, "优酷视频", "https://www.youku.com/", ""));
    jarr_push(gi2, mkitem(4, "爱奇艺", "https://www.iqiyi.com", ""));
    jarr_push(groups, g1);
    jarr_push(groups, g2);
    jobj_set(root, "groups", groups);

    jval *bm = jnew_arr();
    jval *b1 = mkbookmark(1, "仓库");
    jval *bl1 = jobj_get(b1, "links");
    jarr_push(bl1, mklink(1, "GitHub", "https://github.com"));
    jarr_push(bl1, mklink(2, "CND", "https://www.cnd.com"));
    jarr_push(bl1, mklink(3, "Gitee", "https://gitee.com"));
    jarr_push(bl1, mklink(4, "GitLink", "https://www.gitlink.org.cn"));
    jval *b2 = mkbookmark(2, "视频网站");
    jval *bl2 = jobj_get(b2, "links");
    jarr_push(bl2, mklink(1, "腾讯视频", "https://v.qq.com/"));
    jarr_push(bl2, mklink(2, "哔哩哔哩", "https://www.bilibili.com"));
    jarr_push(bl2, mklink(3, "优酷视频", "https://www.youku.com/"));
    jarr_push(bl2, mklink(4, "爱奇艺", "https://www.iqiyi.com"));
    jarr_push(bm, b1);
    jarr_push(bm, b2);
    jobj_set(root, "bookmark_groups", bm);

    return root;
}

/* ---------- 加载 ---------- */

store *store_open(const char *dir) {
    store *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    snprintf(s->dir, sizeof s->dir, "%s", dir);
    s->mu = np_mutex_new();

    char path[1280];
    store_path(s, path, sizeof path);

    char *data = NULL;
    size_t len = 0;
    if (np_read_file(path, &data, &len) != 0) {
        /* 首次启动：生成默认配置 */
        s->cfg = default_config();
        if (store_save(s) != 0) {
            fprintf(stderr, "无法写入默认配置: %s\n", path);
            exit(1);
        }
        printf("已创建默认配置: %s（默认账号 admin / admin，请登录后修改密码）\n", path);
        return s;
    }

    s->cfg = json_parse(data, len);
    if (!s->cfg || s->cfg->t != JV_OBJ) {
        /* 配置损坏：备份后重建 */
        char bak[1288];
        snprintf(bak, sizeof bak, "%s.bak", path);
        np_write_file(bak, data, len);
        printf("配置文件损坏，已备份为 %s 并重新创建默认配置\n", bak);
        json_free(s->cfg);
        s->cfg = default_config();
        if (store_save(s) != 0) {
            fprintf(stderr, "无法写入默认配置: %s\n", path);
            exit(1);
        }
    }
    free(data);
    return s;
}
