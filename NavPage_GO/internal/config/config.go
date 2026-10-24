package config

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
)

// ---------- 数据模型 ----------

type Account struct {
	Username string `json:"username"`
	Hash     string `json:"hash"`
	Salt     string `json:"salt"`
}

type Engine struct {
	ID   string `json:"id"`
	Name string `json:"name"`
	URL  string `json:"url"` // 使用 %s 作为关键词占位符
}

type Wallpaper struct {
	Type  string `json:"type"` // gradient | color | image
	Value string `json:"value"`
}

type Settings struct {
	Title         string    `json:"title"`
	Engines       []Engine  `json:"engines"`
	DefaultEngine string    `json:"default_engine"`
	Wallpaper     Wallpaper `json:"wallpaper"`
	WallpaperGrad string    `json:"wallpaper_grad"` // 最近一次自定义渐变，切到图片壁纸后仍可一键选回
	Accent        string    `json:"accent"`
	VisitorMode   bool      `json:"visitor_mode"`
	ClockSeconds  bool      `json:"clock_seconds"` // 时钟显示秒
	ShowSearch    *bool     `json:"show_search"`   // 搜索栏显示（nil 视为显示）
	HideDesc      bool      `json:"hide_desc"`     // 隐藏卡片描述信息
	TextColor     string    `json:"text_color"`    // 卡片文字颜色（空=默认白色）
	IconStyle     string    `json:"icon_style"`    // 卡片图标风格：detail=详情图标 mini=小图标
	WallpaperBlur int       `json:"wallpaper_blur"` // 壁纸模糊（px，0=不模糊）
	WallpaperMask int       `json:"wallpaper_mask"` // 壁纸遮罩暗度（%，0=无遮罩）
	CardBlur      int       `json:"card_blur"`      // 卡片毛玻璃（px，0=关闭）
	SearchOpacity int       `json:"search_opacity"` // 搜索框背景透明度（%，背景固定黑色，0=默认 55）
	SearchBlur    int       `json:"search_blur"`    // 搜索框毛玻璃（px，0=关闭）
	SearchFont    int       `json:"search_font"`    // 搜索框字体大小（px，0=默认 16）

	ContentMaxWidth     int    `json:"content_max_width"`      // 内容区最大宽度（配合 content_max_unit，0=默认值）
	ContentMaxUnit      string `json:"content_max_unit"`       // 最大宽度单位：px 或 %（空=px）
	ContentPadX         int    `json:"content_pad_x"`          // 内容区左右边距（px）
	ContentMarginTop    int    `json:"content_margin_top"`     // 内容区上边距（%）
	ContentMarginBottom int    `json:"content_margin_bottom"`  // 内容区下边距（%）
	CustomFooter        string `json:"custom_footer"`          // 自定义页脚 HTML（空=不显示）
	NetModeSwitch       string `json:"net_mode_switch"`        // 内外网切换：manual=手动(默认) auto=按来源IP自动判定
	IconFetchMode       string `json:"icon_fetch_mode"`        // 获取图标方式：save=下载落盘(默认) link=引用外链不落盘
}

type Item struct {
	ID          int64  `json:"id"`
	Name        string `json:"name"`
	URL         string `json:"url"`
	IntranetURL string `json:"intranet_url"` // 内网地址，可选
	Desc        string `json:"desc"`
	Emoji       string `json:"emoji"`    // 文字图标（emoji 或字母）
	IconURL     string `json:"icon_url"` // 图片图标，优先于 Emoji
	IconBG      string `json:"icon_bg"`   // 卡片背景色，''=默认毛玻璃，'#00000000'=全透明
	OpenMode    string `json:"open_mode"`  // new=新窗口(默认) current=当前窗口
}

type Group struct {
	ID    int64  `json:"id"`
	Name  string `json:"name"`
	Items []Item `json:"items"`
}

type Link struct {
	ID   int64  `json:"id"`
	Name string `json:"name"`
	URL  string `json:"url"`
}

type BookmarkGroup struct {
	ID    int64  `json:"id"`
	Name  string `json:"name"`
	Links []Link `json:"links"`
}

// SessionTTL 会话有效期（秒），与登录 Cookie 的 MaxAge 保持一致
const SessionTTL int64 = 30 * 24 * 3600

// Config 全量配置，整体读写，落盘为 JSON 文件
type Config struct {
	Account        Account         `json:"account"`
	Settings       Settings        `json:"settings"`
	Groups         []Group         `json:"groups"`
	BookmarkGroups []BookmarkGroup `json:"bookmark_groups"`
}

// PubConfig 返回给前端的公开配置（不含账号凭证与会话令牌）
type PubConfig struct {
	Username       string          `json:"username"`
	VisitorMode    bool            `json:"visitor_mode"`
	Authed         bool            `json:"authed"`   // 当前请求是否已登录（前端据此显隐设置入口，免一次 session 探测请求）
	NetMode        string          `json:"net_mode"` // 服务端按来源 IP 判定的网络模式：lan/wan（自动模式用）
	Settings       Settings        `json:"settings"`
	Groups         []Group         `json:"groups"`
	BookmarkGroups []BookmarkGroup `json:"bookmark_groups"`
}

// ---------- 存储 ----------

type Store struct {
	mu  sync.Mutex
	dir string
	cfg Config
}

func LoadStore(dir string) (*Store, error) {
	s := &Store{dir: dir}
	path := s.filePath()
	data, err := os.ReadFile(path)
	if err != nil {
		if !os.IsNotExist(err) {
			return nil, err
		}
		// 首次启动：生成默认配置
		s.cfg = defaultConfig()
		if err := s.saveLocked(); err != nil {
			return nil, err
		}
		fmt.Printf("已创建默认配置: %s（默认账号 admin / admin，请登录后修改密码）\n", path)
		return s, nil
	}
	if err := json.Unmarshal(data, &s.cfg); err != nil {
		// 配置损坏：备份后重建
		bak := path + ".bak"
		_ = os.WriteFile(bak, data, 0o644)
		fmt.Printf("配置文件损坏，已备份为 %s 并重新创建默认配置\n", bak)
		s.cfg = defaultConfig()
		if err := s.saveLocked(); err != nil {
			return nil, err
		}
		return s, nil
	}
	// 默认壁纸统一用无后缀路径，服务端按文件名主干 default-bg 解析实际文件（兼容任意图片后缀）
	if v := s.cfg.Settings.Wallpaper.Value; isDefaultWallpaperPath(v) && v != "/assets/default-bg" {
		s.cfg.Settings.Wallpaper.Value = "/assets/default-bg"
		if err := s.saveLocked(); err != nil {
			return nil, err
		}
	}
	return s, nil
}

// isDefaultWallpaperPath 判断路径是否指向默认壁纸：/assets/default-bg 或带任意后缀的同一文件。
func isDefaultWallpaperPath(p string) bool {
	const pre = "/assets/default-bg"
	if !strings.HasPrefix(p, pre) {
		return false
	}
	rest := p[len(pre):]
	return rest == "" || (rest[0] == '.' && !strings.Contains(rest, "/"))
}

func (s *Store) filePath() string { return filepath.Join(s.dir, "config.json") }

// Dir 返回数据目录，供上传文件落盘使用
func (s *Store) Dir() string { return s.dir }

// Save 原子写盘（临时文件 + 重命名）
func (s *Store) Save() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.saveLocked()
}

func (s *Store) saveLocked() error {
	if err := os.MkdirAll(s.dir, 0o755); err != nil {
		return err
	}
	data, err := json.MarshalIndent(&s.cfg, "", "  ")
	if err != nil {
		return err
	}
	tmp := s.filePath() + ".tmp"
	if err := os.WriteFile(tmp, data, 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, s.filePath())
}

func (s *Store) Get() Config {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.cfg
}

// Update 以函数方式修改配置并落盘
func (s *Store) Update(fn func(*Config)) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	fn(&s.cfg)
	return s.saveLocked()
}

func (s *Store) Public() PubConfig {
	c := s.Get()
	return PubConfig{
		Username:       c.Account.Username,
		VisitorMode:    c.Settings.VisitorMode,
		Settings:       c.Settings,
		Groups:         c.Groups,
		BookmarkGroups: c.BookmarkGroups,
	}
}

// ---------- 账号与会话 ----------

func newSalt() string {
	b := make([]byte, 16)
	_, _ = rand.Read(b)
	return hex.EncodeToString(b)
}

// hashPassword 简单加盐多轮 SHA-256，适用于单用户局域网场景
func hashPassword(pw, salt string) string {
	h := sha256.Sum256([]byte(salt + ":" + pw))
	for i := 0; i < 9999; i++ {
		h = sha256.Sum256(h[:])
	}
	return hex.EncodeToString(h[:])
}

func (s *Store) VerifyLogin(username, password string) bool {
	c := s.Get()
	a := c.Account
	return a.Username == username && a.Hash == hashPassword(password, a.Salt)
}

// sessionKey 由当前盐+密码哈希派生全局密钥：改密码时盐变化，密钥随之变化，所有旧令牌自动失效
func sessionKey(salt, hash string) []byte {
	h := hmac.New(sha256.New, []byte(salt))
	h.Write([]byte("navpage-session:" + hash))
	return h.Sum(nil)
}

// NewToken 生成无状态签名令牌 deviceId.exp.sig，不落盘；多端登录各自独立、互不顶掉
func (s *Store) NewToken() (string, error) {
	dev := make([]byte, 16)
	if _, err := rand.Read(dev); err != nil {
		return "", err
	}
	c := s.Get()
	payload := hex.EncodeToString(dev) + "." + strconv.FormatInt(time.Now().Unix()+SessionTTL, 10)
	return payload + "." + sessionSig(c.Account.Salt, c.Account.Hash, payload), nil
}

// CheckToken 校验签名与到期时间，无需任何服务端存储
func (s *Store) CheckToken(token string) bool {
	parts := strings.Split(token, ".")
	if len(parts) != 3 || parts[0] == "" || parts[2] == "" {
		return false
	}
	exp, err := strconv.ParseInt(parts[1], 10, 64)
	if err != nil || time.Now().Unix() > exp {
		return false
	}
	c := s.Get()
	return hmac.Equal([]byte(sessionSig(c.Account.Salt, c.Account.Hash, parts[0]+"."+parts[1])), []byte(parts[2]))
}

// sessionSig 用全局密钥对 payload 做 HMAC-SHA256 签名（base64url 无填充）
func sessionSig(salt, hash, payload string) string {
	h := hmac.New(sha256.New, sessionKey(salt, hash))
	h.Write([]byte(payload))
	return base64.RawURLEncoding.EncodeToString(h.Sum(nil))
}

func (s *Store) ChangePassword(oldPw, username, newPw string) bool {
	c := s.Get()
	if c.Account.Hash != hashPassword(oldPw, c.Account.Salt) {
		return false
	}
	salt := newSalt()
	hash := hashPassword(newPw, salt)
	return s.Update(func(c *Config) {
		c.Account.Username = username
		c.Account.Salt = salt
		c.Account.Hash = hash
	}) == nil
}

// SetAccount 命令行指定的账号覆盖配置文件并落盘（空值表示该项不修改）
// 当传入的用户名/密码与现有一致时跳过更新，避免每次启动都生成新 salt 导致所有旧 token 失效
func (s *Store) SetAccount(username, password string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	changed := false
	if username != "" && s.cfg.Account.Username != username {
		s.cfg.Account.Username = username
		changed = true
	}
	if password != "" {
		currentHash := hashPassword(password, s.cfg.Account.Salt)
		if currentHash != s.cfg.Account.Hash {
			salt := newSalt()
			s.cfg.Account.Salt = salt
			s.cfg.Account.Hash = hashPassword(password, salt)
			changed = true
		}
	}
	if changed {
		_ = s.saveLocked()
	}
}

// ---------- 默认配置 ----------

func defaultConfig() Config {
	salt := newSalt()
	return Config{
		Account: Account{
			Username: "admin",
			Salt:     salt,
			Hash:     hashPassword("admin", salt),
		},
		Settings: Settings{
			Title: "我的导航",
			Engines: []Engine{
				{ID: "baidu", Name: "百度", URL: "https://www.baidu.com/s?wd=%s"},
				{ID: "bing", Name: "必应", URL: "https://www.bing.com/search?q=%s"},
				{ID: "google", Name: "谷歌", URL: "https://www.google.com/search?q=%s"},
			},
			DefaultEngine: "baidu",
			Wallpaper:     Wallpaper{Type: "image", Value: "/assets/default-bg"},
			Accent:        "#667eea",
			SearchOpacity: 50,
			SearchFont:    16,
			VisitorMode:   false,
			ContentMarginTop: 8,
			NetModeSwitch:      "manual",
			IconFetchMode:      "link",
			// 默认页脚：留空时前端不显示页脚
			CustomFooter: `<div class="flex justify-center" style="margin-top:100px; color:#fff; font-size:14px;">Powered By <a href="https://github.com/3wlh/NavPage" target="_blank" class="ml-[20px]" style="color:#fff">3wlh</a></div>`,
		},
		Groups: []Group{
			{
				ID: 3, Name: "Git仓库",
				Items: []Item{
					{ID: 1, Name: "GitHub", URL: "https://github.com", Desc: "全球最大代码托管"},
					{ID: 2, Name: "CND", URL: "https://cnb.cool", Desc: "内容分发网络"},
					{ID: 3, Name: "Gitee", URL: "https://gitee.com", Desc: "代码托管平台"},
					{ID: 4, Name: "GitLink", URL: "https://www.gitlink.org.cn", Desc: "代码托管平台"},
				},
			},
			{
				ID: 2, Name: "视频网站",
				Items: []Item{
					{ID: 1, Name: "腾讯视频", URL: "https://v.qq.com/"},
					{ID: 2, Name: "哔哩哔哩", URL: "https://www.bilibili.com"},
					{ID: 3, Name: "优酷视频", URL: "https://www.youku.com/"},
					{ID: 4, Name: "爱奇艺", URL: "https://www.iqiyi.com"},
				},
			},
		},
		BookmarkGroups: []BookmarkGroup{
			{
				ID: 1, Name: "仓库",
				Links: []Link{
					{ID: 1, Name: "GitHub", URL: "https://github.com"},
					{ID: 2, Name: "CND", URL: "https://cnb.cool"},
					{ID: 3, Name: "Gitee", URL: "https://gitee.com"},
					{ID: 4, Name: "GitLink", URL: "https://www.gitlink.org.cn"},
				},
			},
			{
				ID: 2, Name: "视频网站",
				Links: []Link{
					{ID: 1, Name: "腾讯视频", URL: "https://v.qq.com/"},
					{ID: 2, Name: "哔哩哔哩", URL: "https://www.bilibili.com"},
					{ID: 3, Name: "优酷视频", URL: "https://www.youku.com/"},
					{ID: 4, Name: "爱奇艺", URL: "https://www.iqiyi.com"},
				},
			},
		},
	}
}
