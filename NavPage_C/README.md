# Navigation-Page (C 版)

一个自托管的个人导航页：C11 单二进制 + 内嵌前端，零外部依赖（favicon 抓取除外），适合放在 NAS、软路由或小主机上长期运行。

> 本项目为 AI 从 [NavPage_GO](../NavPage_GO/) 重写而来，功能与 Go 版对齐。

## 特性

- **单文件部署**：前端（HTML/CSS/JS、默认壁纸、拖拽库）通过 `.incbin` 汇编指令编进二进制，拷贝一个可执行文件即可运行
- **卡片分组**：分组增删改、卡片增删改、单分组独立拖拽排序；支持详情图标 / 小图标两种卡片风格
- **书签夹**：独立于卡片的分组式书签列表
- **搜索框**：内置多个搜索引擎，可设为默认，支持当前页 / 新窗口打开
- **壁纸**：上传图片、拖拽上传、图片 URL、自定义双色渐变（角度可调）、模糊与遮罩调节
- **外观设置**：主题色、页面标题、内容区宽度（px / %）、边距、卡片毛玻璃、自定义页脚
- **内外网切换**：右下角悬浮按钮手动切换；内网模式优先打开卡片填写的内网地址
- **图标获取**：填网址一键抓取高清站点图标，可选「引用外链」或「下载保存」（依赖系统 curl）
- **多端登录**：无状态签名令牌，多设备同时在线互不顶号，重启服务不掉登录
- **访客模式**：允许未登录浏览首页（只读）
- **文件管理**：统一管理已上传的图片，支持复制地址、设为壁纸、删除

## 快速开始

### 直接运行

```bash
make
./Build/navpage_arm64
```

或先交叉编译：

```bash
# Linux amd64
make amd64

# Linux arm64（默认）
make arm64

# 同时出两个架构
make linux
```

启动后访问 `http://127.0.0.1:8080`。

**默认账号：`admin` / `admin`**，首次登录后请到「设置 → 账号」修改密码。

首次启动会在数据目录生成 `data/config.json` 与 `data/uploads/`。

### 手动编译

```bash
# Windows（llvm-mingw clang）
clang -O2 -std=c11 -Isrc -o navpage.exe src/*.c build/assets.c build/assets.S -lws2_32

# Linux / OpenWrt（gcc 或 clang）
clang -O2 -std=c11 -Isrc -o navpage src/*.c build/assets.c build/assets.S -lpthread

# 交叉编译 musl 静态版（需 clang，gcc 不支持 -target）
clang -O2 -std=c11 -target x86_64-linux-musl -static -Isrc -o navpage src/*.c build/assets.c build/assets.S
clang -O2 -std=c11 -target aarch64-linux-musl -static -Isrc -o navpage src/*.c build/assets.c build/assets.S
```

## 命令行参数

| 参数 | 说明 | 默认值 |
| --- | --- | --- |
| `-p` / `--port` | 监听端口 | `8080`（或环境变量 `PORT`） |
| `-d` / `--data` | 数据目录 | `data`（或环境变量 `DATA_DIR`） |
| `-u` / `--user` | 指定用户名，指定即写入配置文件 | 无 |
| `-pwd` / `--password` | 指定密码，指定即写入配置文件 | 无 |
| `-url` / `--url` | 配置导入地址，自动填入账号设置的下载框 | 无 |

单双横线等价，例如 `-port 9000` 与 `--port 9000` 效果相同；也支持 `--port=9000` 写法。

所有参数值（端口、数据目录、用户名、密码、URL）都支持用引号包裹，`'...'` 与 `"..."` 均可，程序会自动去掉首尾成对的引号，因此含空格、`&`、`?`、`%` 等特殊字符时也不会出错：

```bash
./navpage -p 9000 -d '/mnt/my data/navpage' -url 'https://example.com/navpage-config.json?token=a&v=1'
```

## 目录结构

```
.
├── Makefile                # 一键构建（支持 make / make amd64 / make arm64 / make linux）
├── main.c                  HTTP 服务、API 路由、静态资源服务
├── plat.h                  跨平台 socket / 线程 / 互斥封装
├── util.c / util.h         动态字符串缓冲、文件 IO、随机数、目录创建
├── crypto.c / crypto.h     自实现 MD5 / SHA256 / HMAC-SHA256 / Base64URL
├── json.c / json.h         轻量 JSON DOM 解析 + 序列化（含 UTF-8 BOM 跳过）
├── store.c / store.h       配置读写、原子写盘、账号密码、无状态签名令牌
├── http.c / http.h         thread-per-connection HTTP 服务（keep-alive / Expect: 100-continue / multipart）
├── favicon.c / favicon.h   通过 curl 子进程抓取站点图标并打分排序
├── fetch.c / fetch.h       curl 抓取封装（favicon 与配置 URL 导入共用）
├── assets.h                内嵌资源表结构
├── tools/
│   └── embedgen.c          扫描 web/ 生成 .incbin 汇编文件 + C 索引表
├── embedgen/
│   ├── assets.c            # 由 embedgen 生成（路径 → 内嵌符号映射表）
│   └── assets.S            # 由 embedgen 生成（.incbin 内嵌每个 web/ 文件的原始字节）
├── web/                    # 内嵌前端（与 Go 版相同）
│   ├── index.html
│   ├── app.js
│   ├── style.css
│   ├── assets/             # 默认壁纸等编译内嵌资源
│   └── vendor/             # SortableJS 拖拽库（离线内嵌）
└── data/                   # 运行时生成（不在二进制内）
    ├── config.json         # 全部配置（原子写盘：tmp + rename）
    └── uploads/            # 上传的壁纸与图标（文件名 = 内容 MD5）
```

## 内嵌资源

前端通过 `.incbin` 汇编指令编进二进制，保持单文件部署特性。资源生成流程：

```bash
# 1. 编译 embedgen
gcc -O2 -Isrc -o Build/embedgen tools/embedgen.c

# 2. 扫描 web/ 生成 assets.c + assets.S
./Build/embedgen web embedgen/assets

# 3. 正常编译主程序即可
clang ... assets.c assets.S ...
```

- `assets.S`：`.section .rodata` + 每个文件一条 `.incbin "web/xxx"`，原始字节原样链进只读数据段
- `assets.c`：`np_assets[]` 表，把路径（如 `/app.js`）映射到对应的内嵌符号指针和长度

## 自定义预设壁纸

把图片放到 `web/assets/`，文件名以 `default-bg` 开头、扩展名为 `png/jpg/jpeg/webp`，**重新编译**后即会自动出现在「设置 → 外观 → 预设」中，无需改代码。

```
web/assets/default-bg-2.png
web/assets/default-bg-sunset.jpg
```

注意两点：

- `assets` 是编译期内嵌的，加图后必须重新编译才会生效
- `/assets/` 使用一年期长缓存（immutable），**替换同名文件浏览器不会更新**；换图请使用新文件名

## 数据与备份

所有状态都在数据目录：

- `config.json`：配置、分组卡片、书签、搜索引擎、账号（盐 + SHA256 迭代哈希）
- `uploads/`：上传文件，文件名是内容 MD5，同内容重复上传会复用

备份时直接复制整个数据目录即可；「设置 → 账号 → 配置备份」支持导出/导入配置 JSON（不含敏感信息），也可从 URL 下载导入。

## 会话与安全

- 登录令牌为**无状态签名令牌**（`deviceId.expire.HMAC-SHA256`），不写入配置文件
- 有效期 30 天，服务重启后仍然有效（签名密钥可由当前盐与密码哈希重新派生）
- **修改密码会使所有端已登录的令牌立即失效**（盐变化导致密钥变化）
- 多设备可同时登录，互不顶号；退出登录只清除本机 Cookie
- 单用户设计，不支持多账号

反向代理部署时建议开启 HTTPS；如需限制来源，可在反代层做 IP 白名单。

## 与 Go 版的差异

| 项 | Go 版 | C 版 |
|----|-------|------|
| favicon 抓取 | 原生 TLS（net/http） | 系统 curl 子进程（`curl -K <配置文件>`） |
| 内嵌方式 | `go:embed` | `.incbin` 汇编 + embedgen 生成工具 |
| 加密实现 | 标准库 crypto | 自实现 MD5 / SHA256 / HMAC / Base64URL |
| HTTP 服务 | `net/http` | 自实现 thread-per-connection |
| JSON 解析 | `encoding/json` | 自实现轻量 DOM 解析 |
| 编译器 | `go build` | `gcc` / `clang`（需 `-target` 交叉编译） |

## API 一览

| 方法 | 路径 | 鉴权 | 说明 |
| --- | --- | --- | --- |
| POST | `/api/login` | 否 | 登录，下发会话 Cookie |
| POST | `/api/logout` | 否 | 退出登录（清除本机 Cookie） |
| GET | `/api/session` | 否 | 查询当前登录状态 |
| GET | `/api/appearance` | 否 | 外观配置（登录页也需使用，故公开） |
| GET | `/api/bootstrap` | 否 | 启动参数（`-url` 指定的配置导入地址） |
| GET | `/api/wallpapers` | 否 | 内嵌预设壁纸列表 |
| GET | `/api/config` | 否 | 完整配置（未登录且开启访客模式时可用） |
| PUT | `/api/config` | 是 | 保存配置 |
| PUT | `/api/account` | 是 | 修改用户名 / 密码 |
| POST | `/api/import-from-url` | 是 | 从 URL 下载配置 JSON 并覆盖导入 |
| POST | `/api/upload` | 是 | 上传图片，返回 `/uploads/<md5>.<ext>` |
| GET | `/api/favicon?url=` | 是 | 抓取站点图标 |
| GET | `/api/files` | 是 | 列出已上传文件 |
| DELETE | `/api/files/{name}` | 是 | 删除上传文件 |

鉴权使用 Cookie `np_session`。

## 前端缓存策略

- `index.html` / `app.js` / `style.css`：`no-cache`，且 css/js 引用带 `?v=N` 版本参数，升级二进制后立即生效
- `/assets/` 与 `/uploads/`：`max-age=31536000, immutable`，刷新时直接读本地缓存，避免壁纸白屏

修改前端资源后请同时递增 `index.html` 中的版本号，否则浏览器可能继续使用旧文件。

## 已知限制

- favicon 抓取依赖系统 `curl`（Windows 10+ / 主流 Linux 均自带）
- 交叉编译 musl 静态版需 `clang`（`-target` 是 clang 语法，gcc 不支持）
- `socklen_t` 在某些平台上与 `int` 不同，代码已做兼容处理
