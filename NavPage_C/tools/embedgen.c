/* embedgen：扫描 web/ 目录生成 assets.S（.incbin 内嵌）与 assets.c（路径清单表）
 * 用法：embedgen <web目录> <输出前缀，如 build/assets>
 * 生成：<prefix>.S 与 <prefix>.c，编译进主程序即可按路径读取内嵌文件。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0755)
#endif

typedef struct {
    char web[512];   /* 相对 web/ 的路径，如 assets/default-bg.png */
    char sym[256];   /* C 符号名 */
    long size;
} Entry;

static Entry entries[512];
static int nentries;

/* 文件名转合法 C 符号：非字母数字转 _ */
static void to_sym(const char *s, char *out, int cap) {
    int i = 0;
    for (; s[i] && i < cap - 1; i++) {
        char c = s[i];
        out[i] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9')) ? c : '_';
    }
    out[i] = '\0';
}

static void scan(const char *base, const char *rel) {
    char full[1024];
    snprintf(full, sizeof full, "%s%s", base, rel);
    DIR *d = opendir(full);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[512];
        snprintf(sub, sizeof sub, "%s/%s", rel, e->d_name);
        char subfull[1024];
        snprintf(subfull, sizeof subfull, "%s%s", base, sub);
        struct stat st;
        if (stat(subfull, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            scan(base, sub);
        } else if (nentries < (int)(sizeof entries / sizeof entries[0])) {
            snprintf(entries[nentries].web, sizeof entries[0].web, "%s", sub + 1);
            to_sym(entries[nentries].web, entries[nentries].sym, sizeof entries[0].sym);
            entries[nentries].size = st.st_size;
            nentries++;
        }
    }
    closedir(d);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: embedgen <webdir> <outprefix>\n");
        return 1;
    }
    const char *webdir = argv[1], *prefix = argv[2];
    scan(webdir, "");

    char spath[512], cpath[512];
    snprintf(spath, sizeof spath, "%s.S", prefix);
    snprintf(cpath, sizeof cpath, "%s.c", prefix);

    /* 父目录可能不存在 */
    char *slash = strrchr(prefix, '/');
    if (slash) {
        char dir[512];
        snprintf(dir, sizeof dir, "%.*s", (int)(slash - prefix), prefix);
        char *p = dir;
        while ((p = strchr(p, '/')) != NULL) { *p = '\0'; MKDIR(dir); *p = '/'; p++; }
        MKDIR(dir);
    }

    FILE *s = fopen(spath, "w");
    if (!s) { perror(spath); return 1; }
    fprintf(s, "/* 由 embedgen 自动生成，勿手改 */\n");
    fprintf(s, ".section .rodata\n");
    for (int i = 0; i < nentries; i++)
        fprintf(s, ".global np_asset_%s\nnp_asset_%s:\n.incbin \"%s/%s\"\n",
                entries[i].sym, entries[i].sym, webdir, entries[i].web);
    /* 仅 ELF 目标（Linux）需要可执行栈标记；Windows/COFF 汇编器不认识该指令 */
    fprintf(s, "#ifdef __ELF__\n.section .note.GNU-stack,\"\",@progbits\n#endif\n");
    fclose(s);

    FILE *c = fopen(cpath, "w");
    if (!c) { perror(cpath); return 1; }
    fprintf(c, "/* 由 embedgen 自动生成，勿手改 */\n");
    fprintf(c, "#include \"assets.h\"\n");
    for (int i = 0; i < nentries; i++)
        fprintf(c, "extern const unsigned char np_asset_%s[];\n", entries[i].sym);
    fprintf(c, "\nconst np_asset np_assets[] = {\n");
    for (int i = 0; i < nentries; i++)
        fprintf(c, "    { \"/%s\", np_asset_%s, %ld },\n", entries[i].web, entries[i].sym, entries[i].size);
    fprintf(c, "};\nconst int np_asset_count = %d;\n", nentries);
    fclose(c);

    printf("embedgen: %d files embedded\n", nentries);
    return 0;
}
