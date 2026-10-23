#ifndef NP_ASSETS_H
#define NP_ASSETS_H

typedef struct {
    const char *path;               /* URL 路径，如 /index.html */
    const unsigned char *data;
    long size;
} np_asset;

extern const np_asset np_assets[];
extern const int np_asset_count;

#endif
