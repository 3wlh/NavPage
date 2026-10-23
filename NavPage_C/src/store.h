#ifndef NP_STORE_H
#define NP_STORE_H
#include "json.h"

/* 会话有效期（秒），与登录 Cookie MaxAge 一致 */
#define NP_SESSION_TTL (30LL * 24 * 3600)

typedef struct store {
    char dir[1024];
    np_mutex *mu;
    jval *cfg; /* 根对象：account / settings / groups / bookmark_groups */
} store;

store *store_open(const char *dir);
/* 保存当前配置（原子写盘）。调用者需已持有 mu。 */
int store_save(store *s);

int  store_verify_login(store *s, const char *user, const char *pw);
/* 成功时把令牌写入 out */
int  store_new_token(store *s, char *out, size_t cap);
int  store_check_token(store *s, const char *token);
int  store_change_password(store *s, const char *oldpw, const char *user, const char *newpw);
void store_set_account(store *s, const char *user, const char *pw);

#endif
