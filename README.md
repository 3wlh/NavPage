# NavPage

自托管个人导航页，单二进制 + 内嵌前端，零外部依赖运行。**两版均为 AI 生成**。

- [NavPage_GO/](NavPage_GO/) — Go 版，初始实现，零外部依赖、零 CGO
- [NavPage_C/](NavPage_C/) — C 版，从 Go 版重写，跨平台零依赖

功能对齐：卡片分组、书签夹、搜索框、壁纸、外观设置、内外网切换、图标抓取、多端登录、访客模式、文件管理。

| 项 | Go 版 | C 版 |
|----|-------|------|
| 语言 | Go | C11 |
| favicon 抓取 | 原生 TLS（net/http） | 系统 curl 子进程 |
| 内嵌方式 | go:embed | .incbin 汇编 + embedgen |
| 交叉编译 | `CGO_ENABLED=0 go build` | `clang -target <triple> -static` |
