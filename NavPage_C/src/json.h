#ifndef NP_JSON_H
#define NP_JSON_H
#include <stddef.h>
#include "util.h"

/* 轻量 JSON DOM：解析 / 访问 / 构建 / 序列化 */

typedef enum { JV_NULL, JV_BOOL, JV_NUM, JV_STR, JV_ARR, JV_OBJ } jtype;

typedef struct jval jval;
struct jval {
    jtype t;
    int b;                 /* JV_BOOL */
    double num;            /* JV_NUM */
    char *s;               /* JV_STR */
    jval **items;          /* JV_ARR */
    int n;
    int cap;
    char **keys;           /* JV_OBJ */
    jval **vals;
    int nk;
    int kcap;
};

const char *json_error(void);

/* 解析整段 JSON 文本，失败返回 NULL（json_error 给出原因） */
jval *json_parse(const char *s, size_t n);

void json_free(jval *v);

/* 访问 */
jval *jobj_get(const jval *o, const char *k);
/* 从对象中摘除 key 并返回其值（所有权转移给调用者），不存在返回 NULL */
jval *jobj_remove(jval *o, const char *k);
const char *jstr(const jval *v);          /* 非 JV_STR 返回 NULL */
double jnum(const jval *v, double def);
int    jbool(const jval *v, int def);

/* 构建（参数 v 由容器接管） */
jval *jnew_str(const char *s);
jval *jnew_num(double n);
jval *jnew_bool(int b);
jval *jnew_null(void);
jval *jnew_arr(void);
jval *jnew_obj(void);
int   jarr_push(jval *a, jval *v);
int   jobj_set(jval *o, const char *k, jval *v); /* 覆盖旧值（旧值被释放） */

/* 序列化：indent < 0 紧凑；否则以 indent 空格缩进美化 */
void json_write(sb *out, const jval *v, int indent);

#endif
