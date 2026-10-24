#ifndef NP_ASSETS_H
#define NP_ASSETS_H

typedef struct {
    const char *path;               /* URL 路径，如 /index.html */
    const unsigned char *data;
    const unsigned char *data_end;  /* 尺寸 = data_end - data，由链接器定位自动得出 */
} np_asset;

extern const np_asset np_assets[];
extern const int np_asset_count;

#endif
