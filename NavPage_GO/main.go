package main

import (
	"bytes"
	"crypto/md5"
	"embed"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"io/fs"
	"log"
	"net"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"navpage/internal/config"
	"sort"
	"strconv"
	"strings"
	"time"
)

//go:embed web
var webFS embed.FS

// bootImportURL 启动参数 -url 指定的配置下载地址，通过 /api/bootstrap 暴露给前端
var bootImportURL string

const (
	cookieName   = "np_session"
	maxBodySize  = 5 << 20  // 5MB，JSON 请求上限
	maxUpload    = 10 << 20 // 10MB，上传文件上限
	imgUploadExt = ".png.jpg.jpeg.gif.webp.svg.ico.bmp"
)

func main() {
	port := envOr("PORT", "8080")
	dataDir := envOr("DATA_DIR", "data")
	var cliUser, cliPwd string
	if len(os.Args) > 1 {
		// 端口：-p/--port；数据目录：-d/--data
		if v, ok := flagValue(os.Args, "-port"); ok {
			port = cleanArg(v)
		}
		if v, ok := flagValue(os.Args, "-p"); ok {
			port = cleanArg(v)
		}
		if v, ok := flagValue(os.Args, "-data"); ok {
			dataDir = cleanArg(v)
		}
		if v, ok := flagValue(os.Args, "-d"); ok {
			dataDir = cleanArg(v)
		}
		// 账号：-u/--user、-pwd/--password，指定即覆盖并写入配置文件
		if v, ok := flagValue(os.Args, "-user"); ok {
			cliUser = cleanArg(v)
		}
		if v, ok := flagValue(os.Args, "-u"); ok {
			cliUser = cleanArg(v)
		}
		if v, ok := flagValue(os.Args, "-password"); ok {
			cliPwd = cleanArg(v)
		}
		if v, ok := flagValue(os.Args, "-pwd"); ok {
			cliPwd = cleanArg(v)
		}
		// 配置导入地址：-url，填入前端账号设置的下载框便于一键导入
		if v, ok := flagValue(os.Args, "-url"); ok {
			bootImportURL = cleanURLArg(v)
		}
	}

	store, err := config.LoadStore(dataDir)
	if err != nil {
		log.Fatalf("加载配置失败: %v", err)
	}
	if cliUser != "" || cliPwd != "" {
		store.SetAccount(cliUser, cliPwd)   // 命令行账号高于配置文件，落盘持久化
		fmt.Println("已将命令行指定的用户名/密码写入配置文件")
	}

	mux := http.NewServeMux()

	// API
	mux.HandleFunc("POST /api/login", handleLogin(store))
	mux.HandleFunc("POST /api/logout", handleLogout(store))
	mux.HandleFunc("GET /api/session", handleSession(store))
	mux.HandleFunc("GET /api/config", handleGetConfig(store))
	mux.HandleFunc("GET /api/appearance", handleAppearance(store))
	mux.HandleFunc("GET /api/bootstrap", handleBootstrap())
	mux.HandleFunc("GET /api/wallpapers", handleWallpapers())
	mux.HandleFunc("PUT /api/config", handlePutConfig(store))
	mux.HandleFunc("PUT /api/account", handleAccount(store))
	mux.HandleFunc("POST /api/import-from-url", requireAuth(store, handleImportFromURL(store)))
	mux.HandleFunc("POST /api/upload", requireAuth(store, handleUpload(store)))
	mux.HandleFunc("GET /api/favicon", requireAuth(store, handleFavicon(store)))
	mux.HandleFunc("GET /api/files", requireAuth(store, handleListFiles(store)))
	mux.HandleFunc("DELETE /api/files/{name}", requireAuth(store, handleDeleteFile(store)))

	// 上传文件的静态服务
	uploadsDir := filepath.Join(dataDir, "uploads")
	_ = os.MkdirAll(uploadsDir, 0o755)
	mux.Handle("GET /uploads/", cacheStatic(http.StripPrefix("/uploads/", http.FileServer(http.Dir(uploadsDir)))))

	// 默认壁纸别名：/assets/default-bg 不限定后缀，按文件名主干解析到实际内嵌文件
	mux.HandleFunc("GET /assets/default-bg", handleDefaultWallpaper())

	// 前端静态资源（内嵌，禁用缓存以便升级后立即生效）
	sub, err := fs.Sub(webFS, "web")
	if err != nil {
		log.Fatalf("内嵌资源错误: %v", err)
	}
	mux.Handle("GET /", cacheControl(http.FileServerFS(sub)))

	addr := ":" + port
	fmt.Printf("服务启动:  http://0.0.0.0%s\n", addr)
	if err := http.ListenAndServe(addr, mux); err != nil {
		log.Fatal(err)
	}
}

func envOr(key, def string) string {
	if v := strings.TrimSpace(os.Getenv(key)); v != "" {
		return v
	}
	return def
}

// cleanArg 去首尾空白；若整体被同种引号包裹（"..." 或 '...'）则去掉引号，
// 兼容部分启动器把引号一并传入的情况。结果为空时返回空串，调用方可据此忽略。
func cleanArg(v string) string {
	v = strings.TrimSpace(v)
	if len(v) >= 2 && (v[0] == '"' || v[0] == '\'') && v[len(v)-1] == v[0] {
		v = strings.TrimSpace(v[1 : len(v)-1])
	}
	return v
}

// cleanURLArg 在 cleanArg 基础上额外禁止控制字符（换行等，会破坏 curl 配置文件）
func cleanURLArg(v string) string {
	v = cleanArg(v)
	for _, c := range v {
		if c < 0x20 || c == 0x7f {
			return ""
		}
	}
	return v
}

func flagValue(args []string, name string) (string, bool) {
	// 匹配时忽略前导连字符数量：-x 与 --x 等价；全等比较避免 -p 误吞 -port
	want := strings.TrimLeft(name, "-")
	for i, a := range args {
		if !strings.HasPrefix(a, "-") || a == "-" {
			continue
		}
		cur := strings.TrimLeft(a, "-")
		if cur == want && i+1 < len(args) {
			return args[i+1], true
		}
		if strings.HasPrefix(cur, want+"=") {
			return strings.TrimPrefix(cur, want+"="), true
		}
	}
	return "", false
}

func cacheStatic(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// uploads 文件以内容 MD5 命名，内容不可变，可放心长缓存
		w.Header().Set("Cache-Control", "public, max-age=31536000, immutable")
		next.ServeHTTP(w, r)
	})
}

// cacheControl 静态资源缓存策略：编译内嵌图片（/assets/，含默认壁纸）长缓存，刷新时直接读浏览器缓存不再白屏；
// html/css/js 保持 no-cache（css/js 带 ?v=N 版本参数，升级不受影响）
func cacheControl(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if strings.HasPrefix(r.URL.Path, "/assets/") {
			w.Header().Set("Cache-Control", "public, max-age=31536000, immutable")
		} else {
			w.Header().Set("Cache-Control", "no-cache")
		}
		next.ServeHTTP(w, r)
	})
}

// ---------- 通用工具 ----------

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}

func fail(w http.ResponseWriter, code int, msg string) {
	writeJSON(w, code, map[string]string{"error": msg})
}

func readJSON(r *http.Request, v any) error {
	body := http.MaxBytesReader(nil, r.Body, maxBodySize)
	dec := json.NewDecoder(body)
	return dec.Decode(v)
}

func sessionToken(r *http.Request) string {
	ck, err := r.Cookie(cookieName)
	if err != nil {
		return ""
	}
	return ck.Value
}

func authed(s *config.Store, r *http.Request) bool {
	return s.CheckToken(sessionToken(r))
}

func requireAuth(s *config.Store, next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if !authed(s, r) {
			fail(w, http.StatusUnauthorized, "未登录")
			return
		}
		next(w, r)
	}
}

// ---------- API 处理器 ----------

func handleLogin(s *config.Store) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var req struct {
			Username string `json:"username"`
			Password string `json:"password"`
		}
		if err := readJSON(r, &req); err != nil {
			fail(w, 400, "请求格式错误")
			return
		}
		if !s.VerifyLogin(req.Username, req.Password) {
			fail(w, 401, "账号或密码错误")
			return
		}
		token, err := s.NewToken()
		if err != nil {
			fail(w, 500, "生成会话失败")
			return
		}
		http.SetCookie(w, &http.Cookie{
			Name: cookieName, Value: token, Path: "/",
			HttpOnly: true, MaxAge: 30 * 24 * 3600, SameSite: http.SameSiteLaxMode,
		})
		writeJSON(w, 200, map[string]bool{"ok": true})
	}
}

func handleLogout(s *config.Store) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		// 无状态令牌无法在服务端吊销，退出登录仅清除本机 Cookie
		http.SetCookie(w, &http.Cookie{
			Name: cookieName, Value: "", Path: "/", MaxAge: -1, HttpOnly: true,
		})
		writeJSON(w, 200, map[string]bool{"ok": true})
	}
}

func handleSession(s *config.Store) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		isAuthed := authed(s, r)
		visitor := s.Get().Settings.VisitorMode
		if !isAuthed && !visitor {
			fail(w, 401, "未登录")
			return
		}
		writeJSON(w, 200, map[string]any{"authed": isAuthed, "visitor_mode": visitor})
	}
}

func handleGetConfig(s *config.Store) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if !authed(s, r) && !s.Get().Settings.VisitorMode {
			fail(w, 401, "未登录")
			return
		}
		pub := s.Public()
		pub.Authed = authed(s, r)
		pub.NetMode = detectNetMode(r) // 按请求来源判定内外网，供自动模式使用
		writeJSON(w, 200, pub)
	}
}

// handleAppearance 公开的外观配置（登录页渲染壁纸/主题用，Settings 不含账号凭证等敏感字段）
func handleAppearance(s *config.Store) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, 200, s.Get().Settings)
	}
}

// defaultWallpaper 按文件名主干 default-bg 查找内嵌的默认壁纸（不限定图片后缀），未找到返回空串。
func defaultWallpaper() string {
	entries, err := fs.ReadDir(webFS, "web/assets")
	if err != nil {
		return ""
	}
	best := ""
	for _, e := range entries {
		n := e.Name()
		if e.IsDir() {
			continue
		}
		ext := filepath.Ext(n)
		if strings.TrimSuffix(n, ext) != "default-bg" {
			continue
		}
		switch strings.ToLower(ext) {
		case ".png", ".jpg", ".jpeg", ".webp":
			if best == "" || n < best {
				best = n
			}
		}
	}
	if best == "" {
		return ""
	}
	return "/assets/" + best
}

// handleDefaultWallpaper 以无后缀路径提供默认壁纸，前端固定写 /assets/default-bg 即可适配任意图片后缀。
func handleDefaultWallpaper() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		def := defaultWallpaper()
		if def == "" {
			http.NotFound(w, r)
			return
		}
		data, err := fs.ReadFile(webFS, "web"+def)
		if err != nil {
			http.NotFound(w, r)
			return
		}
		ct := "image/webp"
		switch strings.ToLower(filepath.Ext(def)) {
		case ".png":
			ct = "image/png"
		case ".jpg", ".jpeg":
			ct = "image/jpeg"
		}
		w.Header().Set("Content-Type", ct)
		w.Header().Set("Cache-Control", "no-cache")
		http.ServeContent(w, r, filepath.Base(def), time.Time{}, bytes.NewReader(data))
	}
}

// listWallpapers 列出内嵌 assets 下按 default-bg* 命名、图片扩展名的预设壁纸。
// 往 web/assets 放新图后重新编译，前端「预设」会自动多出一个圆点，无需改代码。
func listWallpapers() []string {
	entries, err := fs.ReadDir(webFS, "web/assets")
	if err != nil {
		return []string{}
	}
	var out []string
	for _, e := range entries {
		n := e.Name()
		if e.IsDir() || !strings.HasPrefix(n, "default-bg") {
			continue
		}
		switch strings.ToLower(filepath.Ext(n)) {
		case ".png", ".jpg", ".jpeg", ".webp":
			out = append(out, "/assets/"+n)
		}
	}
	sort.Strings(out)
	// 默认壁纸（主干 default-bg，不限后缀）固定排第一位
	if def := defaultWallpaper(); def != "" {
		for i, u := range out {
			if u == def {
				out = append(out[:i], out[i+1:]...)
				break
			}
		}
		out = append([]string{def}, out...)
	}
	if out == nil {
		out = []string{}
	}
	return out
}

func handleWallpapers() http.HandlerFunc {
	items := listWallpapers()
	return func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, 200, map[string]any{"items": items})
	}
}

func handlePutConfig(s *config.Store) http.HandlerFunc {
	return requireAuth(s, func(w http.ResponseWriter, r *http.Request) {
		var req struct {
			Settings       *config.Settings       `json:"settings"`
			Groups         *[]config.Group        `json:"groups"`
			BookmarkGroups *[]config.BookmarkGroup `json:"bookmark_groups"`
		}
		if err := readJSON(r, &req); err != nil {
			fail(w, 400, "请求格式错误")
			return
		}
		err := s.Update(func(c *config.Config) {
			if req.Settings != nil {
				c.Settings = *req.Settings
			}
			if req.Groups != nil {
				c.Groups = *req.Groups
			}
			if req.BookmarkGroups != nil {
				c.BookmarkGroups = *req.BookmarkGroups
			}
		})
		if err != nil {
			fail(w, 500, "保存失败")
			return
		}
		writeJSON(w, 200, map[string]bool{"ok": true})
	})
}

func handleAccount(s *config.Store) http.HandlerFunc {
	return requireAuth(s, func(w http.ResponseWriter, r *http.Request) {
		var req struct {
			OldPassword string `json:"old_password"`
			Username    string `json:"username"`
			NewPassword string `json:"new_password"`
		}
		if err := readJSON(r, &req); err != nil {
			fail(w, 400, "请求格式错误")
			return
		}
		req.Username = strings.TrimSpace(req.Username)
		if req.Username == "" {
			fail(w, 400, "用户名不能为空")
			return
		}
		if req.NewPassword != "" && len(req.NewPassword) < 6 {
			fail(w, 400, "新密码至少 6 位")
			return
		}
		if !s.ChangePassword(req.OldPassword, req.Username, req.NewPassword) {
			fail(w, 403, "旧密码错误")
			return
		}
		writeJSON(w, 200, map[string]bool{"ok": true})
	})
}

// handleBootstrap 返回启动期一次性下发给前端的参数（如 -url 指定的配置导入地址）
func handleBootstrap() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, 200, map[string]string{"import_url": bootImportURL})
	}
}

// handleImportFromURL 从指定 URL 下载导出的配置 JSON 并整体覆盖落盘。
// 与前端"导入配置"的文件方式效果一致，只是来源从本地文件换成远程 URL。
func handleImportFromURL(s *config.Store) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var req struct {
			URL string `json:"url"`
		}
		if err := readJSON(r, &req); err != nil {
			fail(w, 400, "请求格式错误")
			return
		}
		u := strings.TrimSpace(req.URL)
		if u == "" {
			fail(w, 400, "URL 不能为空")
			return
		}
		for _, c := range u {
			if c < 0x20 {
				fail(w, 400, "URL 含非法字符")
				return
			}
		}
		// 仅允许 http/https，避免 file:// 等读取本地文件
		pu, err := url.Parse(u)
		if err != nil || (pu.Scheme != "http" && pu.Scheme != "https") {
			fail(w, 400, "仅支持 http/https URL")
			return
		}
		cli := &http.Client{Timeout: 15 * time.Second}
		resp, err := cli.Get(u)
		if err != nil {
			fail(w, 502, "下载失败: "+err.Error())
			return
		}
		defer resp.Body.Close()
		if resp.StatusCode != http.StatusOK {
			fail(w, 502, fmt.Sprintf("下载失败: HTTP %d", resp.StatusCode))
			return
		}
		body, err := io.ReadAll(http.MaxBytesReader(w, resp.Body, maxBodySize))
		if err != nil {
			fail(w, 502, "读取响应失败")
			return
		}
		var obj struct {
			Settings       *config.Settings        `json:"settings"`
			Groups         *[]config.Group         `json:"groups"`
			BookmarkGroups *[]config.BookmarkGroup `json:"bookmark_groups"`
		}
		if err := json.Unmarshal(body, &obj); err != nil {
			fail(w, 400, "JSON 解析失败")
			return
		}
		if obj.Groups == nil {
			fail(w, 400, "文件格式不对：缺少 groups")
			return
		}
		if err := s.Update(func(c *config.Config) {
			if obj.Settings != nil {
				c.Settings = *obj.Settings
			}
			c.Groups = *obj.Groups
			if obj.BookmarkGroups != nil {
				c.BookmarkGroups = *obj.BookmarkGroups
			}
		}); err != nil {
			fail(w, 500, "保存失败")
			return
		}
		writeJSON(w, 200, map[string]bool{"ok": true})
	}
}

// ---------- 内外网自动判定 ----------

// clientIP 获取真实客户端 IP：优先反代头（frp/nginx 需配置），其次直连地址
func clientIP(r *http.Request) string {
	if xff := r.Header.Get("X-Forwarded-For"); xff != "" {
		if i := strings.IndexByte(xff, ','); i >= 0 {
			xff = xff[:i]
		}
		if xff = strings.TrimSpace(xff); xff != "" {
			return xff
		}
	}
	if xr := strings.TrimSpace(r.Header.Get("X-Real-IP")); xr != "" {
		return xr
	}
	host := r.RemoteAddr
	// 去掉端口：IPv4 "1.2.3.4:5678" 与 IPv6 "[::1]:5678"
	if i := strings.LastIndexByte(host, ':'); i >= 0 && !strings.HasSuffix(host, "]") {
		host = host[:i]
	}
	return strings.Trim(host, "[]")
}

// isPrivateIP 私网/回环/链路本地来源视为内网
func isPrivateIP(s string) bool {
	ip := net.ParseIP(s)
	return ip != nil && (ip.IsPrivate() || ip.IsLoopback() || ip.IsLinkLocalUnicast())
}

// detectNetMode 内网来源返回 lan，否则 wan
func detectNetMode(r *http.Request) string {
	if isPrivateIP(clientIP(r)) {
		return "lan"
	}
	return "wan"
}

var uploadExt = func() map[string]bool {
	m := map[string]bool{}
	for _, e := range strings.Split(imgUploadExt, ".") {
		if e != "" {
			m["."+e] = true
		}
	}
	return m
}()

func handleUpload(s *config.Store) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		r.Body = http.MaxBytesReader(w, r.Body, maxUpload)
		if err := r.ParseMultipartForm(maxUpload); err != nil {
			fail(w, 400, "文件过大或格式错误")
			return
		}
		file, hdr, err := r.FormFile("file")
		if err != nil {
			fail(w, 400, "未找到文件")
			return
		}
		defer file.Close()

		ext := strings.ToLower(filepath.Ext(hdr.Filename))
		if !uploadExt[ext] {
			fail(w, 400, "仅支持图片格式")
			return
		}
		dataDir := s.Dir()
		if err := os.MkdirAll(filepath.Join(dataDir, "uploads"), 0o755); err != nil {
			fail(w, 500, "创建目录失败")
			return
		}
		data, err := io.ReadAll(file)
		if err != nil {
			fail(w, 500, "读取文件失败")
			return
		}
		// 内容 MD5 作为文件名：同一张图重复上传直接复用已有文件，不占新空间
		sum := md5.Sum(data)
		name := hex.EncodeToString(sum[:]) + ext
		path := filepath.Join(dataDir, "uploads", name)
		if _, err := os.Stat(path); err != nil {
			if err := os.WriteFile(path, data, 0o644); err != nil {
				fail(w, 500, "保存文件失败")
				return
			}
		}
		writeJSON(w, 200, map[string]string{"url": "/uploads/" + name})
	}
}

// handleListFiles 列出 uploads 目录的上传文件（图标/壁纸等）
func handleListFiles(s *config.Store) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		entries, err := os.ReadDir(filepath.Join(s.Dir(), "uploads"))
		if err != nil {
			writeJSON(w, 200, []any{})
			return
		}
		type fileInfo struct {
			Name string `json:"name"`
			Size int64  `json:"size"`
			URL  string `json:"url"`
		}
		list := []fileInfo{}
		for _, e := range entries {
			if e.IsDir() {
				continue
			}
			if info, err := e.Info(); err == nil {
				list = append(list, fileInfo{Name: e.Name(), Size: info.Size(), URL: "/uploads/" + e.Name()})
			}
		}
		writeJSON(w, 200, list)
	}
}

// handleDeleteFile 删除 uploads 目录中的指定文件
func handleDeleteFile(s *config.Store) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		name := r.PathValue("name")
		if name == "" || name == "." || name == ".." || strings.ContainsAny(name, `/\`) {
			fail(w, 400, "非法文件名")
			return
		}
		if err := os.Remove(filepath.Join(s.Dir(), "uploads", name)); err != nil {
			fail(w, 500, "删除失败")
			return
		}
		writeJSON(w, 200, map[string]bool{"ok": true})
	}
}

// ---------- 站点图标抓取（填网址一键获取图标） ----------

// fetchUA 浏览器 UA：部分站点（如 Cloudflare 防护）拦截 Go 默认 UA
const fetchUA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36"

var (
	reLinkTag  = regexp.MustCompile(`(?i)<link\b[^>]*>`)
	reRelAttr  = regexp.MustCompile(`(?i)rel=["']([^"']*)["']`)
	reHrefAtr  = regexp.MustCompile(`(?i)href=["']([^"']+)["']`)
	reSizeAttr = regexp.MustCompile(`(?i)sizes=["']([^"']*)["']`)
)

// iconCand 图标候选：score 越大越优先（apple-touch=300 > sizes≥96=200 > svg=150 > 其他=50）
type iconCand struct {
	url   string
	score int
}

// maxSize 解析 sizes="16x16 32x32" 中的最大边长
func maxSize(sizes string) int {
	m := 0
	for _, p := range strings.Fields(sizes) {
		if i := strings.IndexByte(p, 'x'); i > 0 {
			a, e1 := strconv.Atoi(p[:i])
			if e1 == nil && a > m {
				m = a
			}
		}
	}
	return m
}

// extractIconLinks 从 HTML 解析所有 <link rel="*icon*"> 并按质量打分排序：
// apple-touch-icon（180px 高清）优先，其次声明了大尺寸（≥96）的，再次 svg（矢量无损），
// 最后普通小图；mask-icon（Safari 单色图）排除
func extractIconLinks(html, base string) []string {
	baseU, err := url.Parse(base)
	if err != nil {
		return nil
	}
	var cands []iconCand
	seen := map[string]bool{}
	for _, tag := range reLinkTag.FindAllString(html, -1) {
		rel := reRelAttr.FindStringSubmatch(tag)
		if rel == nil {
			continue
		}
		relLow := strings.ToLower(rel[1])
		if !strings.Contains(relLow, "icon") || strings.Contains(relLow, "mask") {
			continue
		}
		m := reHrefAtr.FindStringSubmatch(tag)
		if m == nil || strings.TrimSpace(m[1]) == "" {
			continue
		}
		u, err := url.Parse(strings.TrimSpace(m[1]))
		if err != nil {
			continue
		}
		abs := baseU.ResolveReference(u).String()
		if seen[abs] {
			continue
		}
		seen[abs] = true
		score := 50
		if strings.Contains(relLow, "apple") {
			score = 300
		} else if s := reSizeAttr.FindStringSubmatch(tag); s != nil {
			if strings.EqualFold(strings.TrimSpace(s[1]), "any") {
				score = 150
			} else if maxSize(s[1]) >= 96 {
				score = 200
			}
		}
		if strings.HasSuffix(strings.ToLower(abs), ".svg") && score < 150 {
			score = 150
		}
		cands = append(cands, iconCand{abs, score})
	}
	sort.SliceStable(cands, func(i, j int) bool { return cands[i].score > cands[j].score })
	out := make([]string, 0, len(cands))
	for _, c := range cands {
		out = append(out, c.url)
	}
	return out
}

// fetchBody 带 UA 请求并读取响应体（限制 2MB）
func fetchBody(client *http.Client, u string) (data []byte, ct string, status int, err error) {
	req, err := http.NewRequest("GET", u, nil)
	if err != nil {
		return nil, "", 0, err
	}
	req.Header.Set("User-Agent", fetchUA)
	resp, err := client.Do(req)
	if err != nil {
		return nil, "", 0, err
	}
	defer resp.Body.Close()
	data, err = io.ReadAll(io.LimitReader(resp.Body, 2<<20))
	return data, resp.Header.Get("Content-Type"), resp.StatusCode, err
}

// extFromMime Content-Type 转图片扩展名
func extFromMime(ct string) string {
	ct = strings.ToLower(strings.TrimSpace(ct))
	if i := strings.IndexByte(ct, ';'); i >= 0 {
		ct = strings.TrimSpace(ct[:i])
	}
	switch ct {
	case "image/png":
		return ".png"
	case "image/jpeg", "image/jpg":
		return ".jpg"
	case "image/gif":
		return ".gif"
	case "image/webp":
		return ".webp"
	case "image/svg+xml":
		return ".svg"
	case "image/x-icon", "image/vnd.microsoft.icon", "image/ico":
		return ".ico"
	case "image/bmp":
		return ".bmp"
	}
	return ""
}

// sniffImageExt 按文件魔数识别图片类型（CDN 常把图标标成 application/octet-stream）
func sniffImageExt(data []byte) string {
	switch {
	case len(data) >= 8 && bytes.Equal(data[:8], []byte{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A}):
		return ".png"
	case len(data) >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF:
		return ".jpg"
	case len(data) >= 6 && (bytes.HasPrefix(data, []byte("GIF87a")) || bytes.HasPrefix(data, []byte("GIF89a"))):
		return ".gif"
	case len(data) >= 12 && bytes.Equal(data[:4], []byte("RIFF")) && bytes.Equal(data[8:12], []byte("WEBP")):
		return ".webp"
	case len(data) >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 1 && data[3] == 0:
		return ".ico"
	case len(data) >= 5 && bytes.Contains(bytes.ToLower(data[:min(512, len(data))]), []byte("<svg")):
		return ".svg"
	}
	return ""
}

// handleFavicon GET /api/favicon?url=<站点地址>[&mode=link]：服务端抓取站点图标落盘并返回 /uploads/ 地址；mode=link 仅校验后返回外链地址不落盘
func handleFavicon(s *config.Store) http.HandlerFunc {
	return requireAuth(s, func(w http.ResponseWriter, r *http.Request) {
		raw := strings.TrimSpace(r.URL.Query().Get("url"))
		mode := r.URL.Query().Get("mode") // link=仅返回外链地址不落盘
		if raw == "" {
			fail(w, 400, "缺少 url 参数")
			return
		}
		if !strings.Contains(raw, "://") {
			raw = "https://" + raw
		}
		u, err := url.Parse(raw)
		if err != nil || u.Host == "" {
			fail(w, 400, "地址格式有误")
			return
		}
		origin := u.Scheme + "://" + u.Host
		client := &http.Client{Timeout: 8 * time.Second}

		// 1) 抓首页 HTML 收集 icon 候选（已按质量打分排序：apple-touch/sizes≥96/svg 高清优先）
		var candidates []string
		if data, _, status, err := fetchBody(client, origin+"/"); err == nil && status == 200 {
			candidates = extractIconLinks(string(data), origin+"/")
		}
		// 2) 兜底通用路径（首页解析不到时才用，favicon.ico 常被 SPA 返回 HTML，靠 Content-Type 校验过滤）
		candidates = append(candidates, origin+"/favicon.ico", origin+"/favicon.png")

		// 逐个下载，Content-Type 必须是图片才算命中
		for _, iu := range candidates {
			data, ct, status, err := fetchBody(client, iu)
			if err != nil || status != 200 || len(data) == 0 {
				continue
			}
			ext := extFromMime(ct)
			if ext == "" {
				ext = sniffImageExt(data) // Content-Type 不可信时按魔数识别
			}
			if ext == "" {
				continue // 非图片内容（如 SPA 返回的 text/html），换下一个候选
			}
			// link 模式：仅校验是真实图片，直接返回外链地址，不落盘
			if mode == "link" {
				writeJSON(w, 200, map[string]string{"url": iu})
				return
			}
			// 与上传一致：MD5 文件名去重落盘
			dataDir := s.Dir()
			_ = os.MkdirAll(filepath.Join(dataDir, "uploads"), 0o755)
			sum := md5.Sum(data)
			name := hex.EncodeToString(sum[:]) + ext
			path := filepath.Join(dataDir, "uploads", name)
			if _, err := os.Stat(path); err != nil {
				if err := os.WriteFile(path, data, 0o644); err != nil {
					fail(w, 500, "保存文件失败")
					return
				}
			}
			writeJSON(w, 200, map[string]string{"url": "/uploads/" + name})
			return
		}
		fail(w, 404, "未找到图标")
	})
}
