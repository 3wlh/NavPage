/* ============ 全局状态 ============ */
let cfg = null;           // 服务器公开配置
let authed = false;       // 是否已登录
let cardSortables = [];   // 首页卡片排序实例（SortableJS），仅排序模式下创建
let cardSorting = false;  // 卡片拖拽进行中（拖拽松手后的 click 需忽略，避免误跳转）
let mgSortable = null;    // 设置抽屉内分组列表的排序实例（只保留一个，重建前销毁旧的）
let sortGid = null;       // 正在排序的分组 ID（null=未排序），仅该组卡片可拖，保存按钮在组内
let wpAssets = [];        // 内嵌 assets 里的预设壁纸列表（default-bg* 图片，由服务端枚举）
let saveTimer = null;
let savedSnap = null;     // 上次成功保存的配置快照，内容未变（改回原值）时跳过保存
let bootImportURL = '';   // 启动参数 -url 下发的配置导入地址，用于预填下载框

const $ = s => document.querySelector(s);
const $$ = s => [...document.querySelectorAll(s)];

// 壁纸缺省兜底渐变（仅用于未配置壁纸时的显示，不再作为预设供选择）
const FALLBACK_WP = 'linear-gradient(135deg,#667eea,#764ba2)';

// 默认壁纸按主干 default-bg 匹配（不限后缀）：/assets/default-bg 与 /assets/default-bg.webp 视为同一张
function wpKey(u) {
  const base = (u || '').split('/').pop();
  return base.replace(/\.[^.]+$/, '') === 'default-bg' ? '/assets/default-bg' : u;
}

// 解析线性渐变的起止色与角度；非双色线性渐变返回 null（自定义渐变选择器回显用）
function parseGrad(v) {
  const s = String(v || '');
  const colors = (s.match(/#[0-9a-f]{6}/gi) || []).map(c => c.slice(0, 7));
  if (colors.length < 2) return null;
  const dm = /(-?[\d.]+)deg/i.exec(s);
  return { c1: colors[0], c2: colors[1], deg: dm ? Math.round(+dm[1]) : 135 };
}

/* ============ 工具 ============ */
function esc(s) {
  return String(s ?? '').replace(/[&<>"']/g, c =>
    ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

function toast(msg, type = '') {
  const t = $('#toast');
  t.textContent = msg;
  t.className = 'show ' + type;
  clearTimeout(t._timer);
  t._timer = setTimeout(() => t.classList.remove('show'), 2200);
}

/* 自绘确认弹窗（替代浏览器原生 confirm）：okText 默认「删除」 */
function confirmDialog(msg, onOk, okText = '删除') {
  const mask = document.createElement('div');
  mask.className = 'np-confirm-mask';
  mask.innerHTML = `
    <div class="np-confirm">
      <div class="np-confirm-msg">${esc(msg)}</div>
      <div class="np-confirm-btns">
        <button class="np-confirm-cancel">取消</button>
        <button class="np-confirm-ok">${esc(okText)}</button>
      </div>
    </div>`;
  const close = () => mask.remove();
  mask.querySelector('.np-confirm-cancel').addEventListener('click', close);
  mask.querySelector('.np-confirm-ok').addEventListener('click', () => { close(); onOk(); });
  mask.addEventListener('click', e => { if (e.target === mask) close(); });
  document.body.appendChild(mask);
}

async function api(method, url, body) {
  const opt = { method, headers: {} };
  if (body !== undefined) {
    opt.headers['Content-Type'] = 'application/json';
    opt.body = JSON.stringify(body);
  }
  const r = await fetch(url, opt);
  const d = await r.json().catch(() => ({}));
  if (!r.ok) throw Object.assign(new Error(d.error || 'HTTP ' + r.status), { status: r.status });
  return d;
}

function nextId(arr) { return arr.reduce((m, x) => Math.max(m, x.id || 0), 0) + 1; }

function strColor(name) {
  let h = 0;
  for (const c of String(name)) h = (h * 31 + c.codePointAt(0)) % 360;
  return `hsl(${h},62%,52%)`;
}

function autoIcon(url) {
  try { return new URL(dynURL(url)).origin + '/favicon.ico'; } catch { return ''; }
}

// 待保存配置的序列化快照（与 PUT /api/config 的内容一致）
const snapOf = () => JSON.stringify({ settings: cfg.settings, groups: cfg.groups, bookmark_groups: cfg.bookmark_groups });
const markSaved = () => { savedSnap = snapOf(); };

let saveDirty = false; // 拖动/输入只标记待保存，不立即发请求，关闭设置面板时统一 flush，避免抖动
function debounceSave() {
  saveDirty = true;
  clearTimeout(saveTimer);
}

// 立即落盘待保存改动：关闭/隐藏设置抽屉时调用
async function flushSave() {
  clearTimeout(saveTimer);
  if (!saveDirty) return;
  saveDirty = false;
  const snap = snapOf();
  if (snap === savedSnap) return; // 改回原值/无实际变化，不落盘
  try {
    await api('PUT', '/api/config', {
      settings: cfg.settings,
      groups: cfg.groups,
      bookmark_groups: cfg.bookmark_groups,
    });
    savedSnap = snap;
  } catch (e) {
    saveDirty = true; // 保存失败仍需下次重试
    toast('保存失败：' + e.message, 'err');
  }
}

/* ============ 上传 ============ */
let uploadCb = null;
function pickImage(cb) { uploadCb = cb; $('#file-input').value = ''; $('#file-input').click(); }

async function uploadFile(file, cb) {
  const fd = new FormData();
  fd.append('file', file);
  try {
    const d = await fetch('/api/upload', { method: 'POST', body: fd }).then(r => r.json());
    if (d.url) cb(d.url);
    else toast(d.error || '上传失败', 'err');
  } catch { toast('上传失败', 'err'); }
}

/* ============ 时钟 ============ */
function tick() {
  if (!cfg) return; // 配置未就绪时不动时钟，保住 index.html 内联脚本的预渲染（否则秒会先消失几秒）
  const n = new Date();
  const p = x => String(x).padStart(2, '0');
  const sec = cfg && cfg.settings && cfg.settings.clock_seconds ? `:${p(n.getSeconds())}` : '';
  $('#clock-time').textContent = `${p(n.getHours())}:${p(n.getMinutes())}${sec}`;
  $('#clock-date').textContent = `${n.getMonth() + 1}月${n.getDate()}日 星期${'日一二三四五六'[n.getDay()]}`;
}
setInterval(tick, 1000);

function greet() {
  const h = new Date().getHours();
  if (h < 5) return '凌晨好';
  if (h < 9) return '早上好';
  if (h < 12) return '上午好';
  if (h < 14) return '中午好';
  if (h < 18) return '下午好';
  return '晚上好';
}

/* ============ 深色模式 ============ */
function applyDark() {
  const saved = localStorage.getItem('np_dark');
  const dark = saved === null
    ? matchMedia('(prefers-color-scheme: dark)').matches
    : saved === '1';
  document.body.classList.toggle('dark', dark);
}

/* ============ 外观 ============ */
function applyAppearance() {
  const s = cfg.settings;
  document.documentElement.style.setProperty('--accent', s.accent || '#667eea');
  document.documentElement.style.setProperty('--card-text', s.text_color || '#ffffff');
  document.title = s.title || '导航页';
  $('#login-title').textContent = s.title || '导航页';
  const logo = $('#logo-box');
  if (logo) logo.textContent = s.title || '导航页';
  $('#search-box').style.display = s.show_search === false ? 'none' : '';
  const bg = $('#bg'), wp = s.wallpaper || {};
  if (wp.type === 'image') {
    bg.style.background = `url("${wp.value}") center/cover no-repeat fixed`;
  } else {
    bg.style.background = wp.value || FALLBACK_WP;
  }
  const blur = Math.min(60, Math.max(0, +s.wallpaper_blur || 0));
  const mask = Math.min(80, Math.max(0, +s.wallpaper_mask || 0)) / 100;
  bg.style.filter = blur ? `blur(${blur}px)` : '';
  bg.style.transform = blur ? 'scale(1.08)' : '';   // 放大弥补模糊产生的透明边缘
  document.documentElement.style.setProperty('--wp-mask', mask);
  // 内容区域布局：最大宽度 / 左右边距 / 上下边距 / 自定义页脚
  const rs = document.documentElement.style;
  rs.setProperty('--card-bd', (+s.card_blur || 0) > 0 ? `blur(${Math.min(40, Math.max(0, +s.card_blur))}px)` : 'none');
  // 搜索框样式：颜色固定在 style.css 的 #search-box，这里只写透明度；毛玻璃与字号同样由变量下发
  const sop = Math.min(100, Math.max(0, s.search_opacity == null ? 55 : +s.search_opacity));
  rs.setProperty('--search-alpha', (sop / 100).toFixed(3));
  const sblur = Math.min(25, Math.max(0, +s.search_blur || 0));
  rs.setProperty('--search-bd', sblur ? `blur(${sblur}px)` : 'none');
  const sfont = Math.min(24, Math.max(10, +s.search_font || 16));
  rs.setProperty('--search-fs', sfont + 'px');
  // 最大宽度支持 px 与 %：% 相对于视口宽度，px 为固定像素
  const maxUnit = s.content_max_unit === '%' ? '%' : 'px';
  const maxW = +s.content_max_width || (maxUnit === '%' ? 100 : 1200);
  rs.setProperty('--content-max', (maxUnit === '%'
    ? Math.min(100, Math.max(30, maxW))
    : Math.min(3000, Math.max(600, maxW))) + maxUnit);
  rs.setProperty('--content-px', Math.min(200, Math.max(0, s.content_pad_x == null ? 20 : +s.content_pad_x)) + 'px');
  rs.setProperty('--content-mt', Math.min(40, Math.max(0, s.content_margin_top == null ? 8 : +s.content_margin_top)) + '%');
  rs.setProperty('--content-mb', Math.min(40, Math.max(0, s.content_margin_bottom == null ? 0 : +s.content_margin_bottom)) + '%');
  const foot = $('#foot');
  // 页脚显示自定义 HTML（默认值在服务端默认配置中，留空不显示）
  if (foot) foot.innerHTML = s.custom_footer || '';
  // 缓存外观到 localStorage，供 index.html 内联脚本在刷新时零闪烁预应用
  try { localStorage.setItem('np_appearance', JSON.stringify({
    wallpaper: s.wallpaper, wallpaper_blur: s.wallpaper_blur, wallpaper_mask: s.wallpaper_mask,
    accent: s.accent, clock_seconds: !!s.clock_seconds
  })); } catch (e) {}
  tick(); // 配置就绪后立即刷新时钟（含秒），不等下一个 interval
}

/* ============ 当前页面弹窗打开 ============ */
function pmEsc(e) { if (e.key === 'Escape') closePageModal(); }

function openPageModal(url, name) {
  closePageModal();
  const mask = document.createElement('div');
  mask.id = 'pagemodal-mask';
  mask.innerHTML = `
    <div id="pagemodal">
      <div class="pm-head"><span>${esc(name || '')}</span><button type="button" id="pm-close" title="关闭">✕</button></div>
      <iframe src="${esc(url)}"></iframe>
    </div>`;
  document.body.appendChild(mask);
  mask.addEventListener('click', e => { if (e.target === mask) closePageModal(); });
  $('#pm-close').addEventListener('click', closePageModal);
  document.addEventListener('keydown', pmEsc);
}

function closePageModal() {
  $('#pagemodal-mask')?.remove();
  document.removeEventListener('keydown', pmEsc);
}

// 动态主机：URL 里的 "location.hostname" 在打开时替换为当前访问的域名/IP
// 例：http://location.hostname:8080 -> http://<当前域名或IP>:8080
function dynURL(u) {
  const s = (u || '').trim();
  return s.includes('location.hostname') ? s.replaceAll('location.hostname', window.location.hostname) : s;
}

/* ============ 内外网模式（auto=按访问来源 IP 自动判定，manual=右下角按钮手动切换） ============ */
function netMode() {
  if ((cfg.settings.net_mode_switch || 'manual') === 'auto') {
    return cfg.net_mode === 'lan' ? 'lan' : 'wan';
  }
  return localStorage.getItem('np_net_mode') === 'lan' ? 'lan' : 'wan';
}

// 内网模式优先用卡片内网地址；空值或只填了协议头（如 "https://"）视为未填，用默认网址
function cardHref(it) {
  if (netMode() === 'lan') {
    const lan = (it.intranet_url || '').trim();
    if (lan && lan.replace(/^https?:\/\//i, '').trim() !== '') return dynURL(lan);
  }
  return dynURL(it.url);
}

function renderNetToggle() {
  const btn = $('#net-toggle');
  if (!btn) return;
  if ((cfg.settings.net_mode_switch || 'manual') === 'auto') {
    btn.classList.add('hidden');
    btn.title = '自动判断内外网模式（设置中可改为手动）';
    return;
  }
  btn.classList.remove('hidden');
  const lan = netMode() === 'lan';
  btn.textContent = lan ? '🏠' : '🌐';
  btn.title = lan ? '当前内网模式，点击切换外网' : '当前外网模式，点击切换内网';
}

/* ============ 渲染：主页 ============ */
function renderMain() {
  renderGroups();
  renderBookmarks();
  renderEnginesSelect();
  bindEngineIcon();
  applyAppearance();
  renderNetToggle();
}

/* 文字图标颜色随卡片背景自动高对比：浅底深字、深底白字；无背景/全透明返回 ''（用默认白） */
function iconTextOn(bg) {
  const pv = bg ? parseHexa(bg) : null;
  if (!pv || pv.a === 0) return '';
  const lum = (0.299 * pv.r + 0.587 * pv.g + 0.114 * pv.b) / 255;
  return lum > 0.6 ? '#2b2f3a' : '#fff';
}

function iconHTML(item, tileBg) {
  const letter = esc((item.name || '?').trim()[0] || '?');
  const color = strColor(item.name);
  const tileStyle = tileBg ? ` style="background:${esc(tileBg)}"` : '';
  if (item.icon_url) {
    return `<div class="card-icon"${tileStyle}><img class="icon-img" data-name="${esc(item.name)}" src="${esc(item.icon_url)}" alt=""></div>`;
  }
  const bgc = iconTextOn(item.icon_bg);
  if (item.emoji) return `<div class="card-icon"${tileBg || bgc ? ` style="${tileBg ? `background:${esc(tileBg)};` : ''}${bgc ? `color:${bgc}` : ''}"` : ''}>${esc(item.emoji)}</div>`;
  const auto = autoIcon(item.url);
  if (auto) return `<div class="card-icon"><img class="icon-img icon-auto" data-name="${esc(item.name)}" src="${esc(auto)}" alt=""></div>`;
  // 字母图标：设置了非透明卡片背景时直接铺该背景并配高对比字色，否则用名字哈希色块
  if (bgc) return `<div class="card-icon"><div class="letter" style="background:${esc(item.icon_bg)};color:${bgc}">${letter}</div></div>`;
  return `<div class="card-icon"><div class="letter" style="background:${color}">${letter}</div></div>`;
}

// 图标加载失败时回退为字母色块
function bindIconFallback(root) {
  root.querySelectorAll('img.icon-img').forEach(img => {
    img.onerror = () => {
      const div = document.createElement('div');
      div.className = 'letter';
      div.style.background = strColor(img.dataset.name);
      div.textContent = (img.dataset.name || '?').trim()[0] || '?';
      img.replaceWith(div);
    };
  });
}

function renderGroups() {
  const wrap = $('#groups-wrap');
  wrap.innerHTML = cfg.groups.map(g => `
    <section class="group${authed && sortGid === g.id ? ' sorting' : ''}" data-g="${g.id}">
      <div class="group-head">
        <h2>${esc(g.name)}</h2>
        ${authed ? `<button class="link-btn group-add-btn" data-add-item="${g.id}" title="添加卡片">＋</button>
        <button class="link-btn group-sort-btn${sortGid === g.id ? ' active' : ''}" data-sortmode="${g.id}" title="排序"><svg viewBox="0 0 1024 1024" width="16" height="16" aria-hidden="true"><path fill="currentColor" d="M68.273876 68.273876H0V0h68.273876v68.273876z m272.96753 0H273.031517V0h68.209889v68.273876z m273.031516 0H545.999046V0H614.272922v68.273876zM204.757641 204.757641h750.756687v273.031516h-68.209889v-204.75764h-614.272922v614.272922h204.75764v68.209889H204.757641V204.757641zM68.273876 341.241406H0V273.031517h68.273876v68.209889z m419.497216 146.529686a34.104945 34.104945 0 0 1 33.529064-8.702199l477.725171 136.547751a34.104945 34.104945 0 0 1 4.095152 64.114737L776.7993 776.7993l-97.067919 226.321179a34.104945 34.104945 0 0 1-64.114737-4.095152L479.068893 521.300156a34.104945 34.104945 0 0 1 8.702199-33.529064z m73.840725 73.840725l93.228713 326.33249 64.62663-150.62484a34.168931 34.168931 0 0 1 17.916294-17.916294l150.624839-64.562643-326.396476-93.228713z m-493.337941 52.661105H0V545.999046h68.273876V614.272922z"/></svg></button>` : ''}
      </div>
      <div class="cards-grid${cfg.settings.icon_style === 'mini' ? ' mini' : ''}" data-g="${g.id}">
        ${g.items.map(it => {
          const pv = it.icon_bg ? parseHexa(it.icon_bg) : null;
          const transparent = pv && pv.a === 0;   // 全透明：整卡无底无边框
          const mini = cfg.settings.icon_style === 'mini';   // 小图标模式：图标上、名字下
          const style = (!transparent && !mini && it.icon_bg) ? `style="background:${esc(it.icon_bg)}"` : '';
          // 小图标模式：背景色垫在图标方块上（不设则用默认色），文字在色块外下方
          const tileBg = mini && !transparent ? (it.icon_bg || '#0000006f') : '';
          return `
          <a class="card${mini ? ' mini' : ''}${transparent ? ' transparent' : ''}" draggable="false" href="${esc(cardHref(it))}" ${it.open_mode === 'new' ? 'target="_blank" rel="noopener"' : ''}${it.open_mode === 'modal' ? ' data-modal="1"' : ''}
             data-g="${g.id}" data-id="${it.id}"
             ${style}>
            ${iconHTML(it, tileBg)}
            <div class="card-info">
              <div class="card-name">${esc(it.name)}</div>
              ${!mini && !cfg.settings.hide_desc ? `<div class="card-desc">${esc(it.desc || '')}</div>` : ''}
            </div>
          </a>`;
        }).join('')}
      </div>
      ${authed && sortGid === g.id ? `<div class="sort-save-row"><button class="sort-save-inner" id="sort-save"><svg viewBox="0 0 24 24" width="15" height="15" fill="#fff" aria-hidden="true"><path d="M17 3H5c-1.11 0-2 .9-2 2v14c0 1.1.89 2 2 2h14c1.1 0 2-.9 2-2V7l-4-4zm-5 16c-1.66 0-3-1.34-3-3s1.34-3 3-3 3 1.34 3 3-1.34 3-3 3zm3-10H5V5h10v4z"/></svg>保存排序</button></div>` : ''}
    </section>`).join('');

  bindIconFallback(wrap);
  bindCardSort();
  // 分组排序走设置抽屉，首页仅卡片拖拽
  bindMainActions();
  $('#groups-wrap #sort-save')?.addEventListener('click', () => {
    sortGid = null;
    renderGroups();
    debounceSave();
    toast('排序已保存', 'ok');
  });
}

function renderBookmarks() {
  const wrap = $('#bookmarks-wrap');
  const bgs = cfg.bookmark_groups || [];
  if (!bgs.length) { wrap.innerHTML = ''; return; }
  wrap.innerHTML = `
    <section class="bookmark-section glass">
      <div class="bookmark-head"><h2>书签</h2></div>
      <div class="bookmark-cols">
        ${bgs.map(g => `
          <div class="bm-col" data-bg="${g.id}">
            <h3>${esc(g.name)}</h3>
            ${g.links.map(l => `
              <a class="bm-link" href="${esc(l.url)}" target="_blank" rel="noopener">${esc(l.name)}</a>`).join('')}
          </div>`).join('')}
      </div>
    </section>`;
}

/* ============ 搜索引擎图标 ============ */
let curEngine = localStorage.getItem('np_cur_engine') || null; // 浏览器缓存的选择（仅本机记忆，落盘默认走设置页）

function currentEngine() {
  const engines = cfg.settings.engines || [];
  if (curEngine && !engines.some(e => e.id === curEngine)) {
    curEngine = null; // 缓存的引擎已被删除，回退默认
    localStorage.removeItem('np_cur_engine');
  }
  return engines.find(e => e.id === curEngine) ||
    engines.find(e => e.id === cfg.settings.default_engine) || engines[0];
}

function renderEnginesSelect() {
  const e = currentEngine();
  const img = $('#engine-img');
  if (!e) { img.style.display = 'none'; return; }
  img.style.display = '';
  img.src = autoIcon(e.url);
  img.onerror = () => {
    img.onerror = null;
    img.src = 'data:image/svg+xml,' + encodeURIComponent(
      `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32"><circle cx="16" cy="16" r="15" fill="#667eea"/><text x="16" y="21" font-size="14" fill="#fff" text-anchor="middle" font-family="sans-serif">${esc(e.name[0] || '?')}</text></svg>`);
  };
  $('#engine-icon').title = `搜索引擎：${e.name}（点击切换）`;
  // 引擎切换面板：一排图标
  const list = $('#engine-list');
  if (list) list.innerHTML = (cfg.settings.engines || []).map(en => `
    <button class="engine-item ${en.id === (curEngine || cfg.settings.default_engine) ? 'active' : ''}"
      data-engine="${esc(en.id)}" title="${esc(en.name)}">
      <img src="${autoIcon(en.url)}" alt="" onerror="this.src='data:image/svg+xml,'+encodeURIComponent('<svg xmlns=&quot;http://www.w3.org/2000/svg&quot; viewBox=&quot;0 0 32 32&quot;><circle cx=&quot;16&quot; cy=&quot;16&quot; r=&quot;15&quot; fill=&quot;#667eea&quot;/><text x=&quot;16&quot; y=&quot;21&quot; font-size=&quot;14&quot; fill=&quot;#fff&quot; text-anchor=&quot;middle&quot; font-family=&quot;sans-serif&quot;>${esc(en.name[0] || '?')}</text></svg>')">
    </button>`).join('');
}

function toggleEnginePop(show) {
  const pop = $('#engine-pop');
  if (!pop) return;
  pop.classList.toggle('hidden', !show);
}

function bindEngineIcon() {
  if (bindEngineIcon.done) return;
  bindEngineIcon.done = true;
  $('#engine-icon').addEventListener('click', e => {
    e.stopPropagation();
    toggleEnginePop($('#engine-pop').classList.contains('hidden'));
  });
  // 点外部收起
  document.addEventListener('click', e => {
    const pop = $('#engine-pop');
    if (pop && !pop.classList.contains('hidden') &&
        !pop.contains(e.target) && e.target !== $('#engine-icon') &&
        !$('#engine-icon').contains(e.target)) toggleEnginePop(false);
  });
  // 引擎切换
  $('#engine-list').addEventListener('click', e => {
    const btn = e.target.closest('.engine-item');
    if (!btn) return;
    curEngine = btn.dataset.engine;
    localStorage.setItem('np_cur_engine', curEngine); // 存浏览器缓存，不改服务端配置
    renderEnginesSelect();
    toggleEnginePop(false);
  });
  // 新窗口打开偏好（本地记忆）
  const nw = $('#search-newwin');
  nw.checked = localStorage.getItem('np_search_newwin') !== '0';
  nw.addEventListener('change', () => {
    localStorage.setItem('np_search_newwin', nw.checked ? '1' : '0');
  });
}

/* ============ 搜索 ============ */
function doSearch() {
  const q = $('#search-input').value.trim();
  if (!q) return;
  const isUrl = /^https?:\/\//i.test(q) ||
    (/^[\w-]+(\.[\w-]+)+(:\d+)?([\/?#].*)?$/i.test(q) && !q.includes(' '));
  let url;
  if (isUrl) {
    url = /^https?:\/\//i.test(q) ? q : 'https://' + q;
  } else {
    const e = currentEngine();
    if (!e || !e.url.includes('%s')) { toast('搜索引擎配置有误', 'err'); return; }
    url = e.url.replace('%s', encodeURIComponent(q));
  }
  window.open(url, localStorage.getItem('np_search_newwin') === '0' ? '_self' : '_blank');
}

/* ============ 主页编辑操作 ============ */
function bindMainActions() {
  // 卡片点击跳转（编辑/删除走右键菜单）
  $$('#groups-wrap .card').forEach(card => {
    card.addEventListener('click', e => {
      if (!authed) return; // 正常跳转由 href 完成
      if (cardSorting) e.preventDefault();
    });

    // 右键菜单：新窗口打开 / 编辑 / 删除
    card.addEventListener('contextmenu', e => {
      if (!authed) return;
      e.preventDefault();
      const g = +card.dataset.g, id = +card.dataset.id;
      const grp = cfg.groups.find(x => x.id === g);
      const it = grp && grp.items.find(x => x.id === id);
      if (!it) return;
      showContextMenu(e.clientX, e.clientY, [
        { label: '新窗口打开', action: () => window.open(dynURL(it.url), '_blank') },
        { label: '编辑', action: () => itemModal(g, id) },
        {
          label: '删除', danger: true, action: () => {
            confirmDialog(`删除卡片「${it.name}」？`, () => {
              grp.items = grp.items.filter(x => x.id !== id);
              renderGroups(); debounceSave();
            });
          }
        },
      ]);
    });
  });

  $$('[data-add-item]').forEach(el =>
    el.addEventListener('click', () => itemModal(+el.dataset.addItem, null)));
  $$('[data-sortmode]').forEach(el =>
    el.addEventListener('click', () => {
      const gid = +el.dataset.sortmode;
      sortGid = sortGid === gid ? null : gid;   // 点哪个组排哪个组，再点取消
      renderGroups();
    }));
}

/* 卡片拖拽排序：SortableJS 驱动（鼠标事件自绘拖拽，拖动中实时跟随、卡片之间平滑让位）。
   仅当前排序分组的网格挂载实例，单分组独立排序，不能跨组拖。 */
function bindCardSort() {
  cardSortables.forEach(s => s.destroy());
  cardSortables = [];
  if (!authed || sortGid === null || typeof Sortable === 'undefined') return;
  const grid = $(`#groups-wrap .cards-grid[data-g="${sortGid}"]`);
  if (!grid) return;
  cardSortables.push(Sortable.create(grid, {
    animation: 150,          // 卡片让位动画
    draggable: '.card',
    ghostClass: 'dragging',  // 被拖卡片留在原位半透明，充当落点指示
    /* 自绘拖拽（forceFallback）：卡片是 <a>，用浏览器原生拖拽会走链接拖拽并生成半透明拖影；
       自绘后是一份不透明卡片副本跟随鼠标，触屏也走同一套逻辑 */
    forceFallback: true,
    fallbackOnBody: true,
    fallbackClass: 'sortable-fallback',
    onStart: () => { cardSorting = true; },
    onEnd: () => {
      // 等 Sortable 自行收尾（移除跟随鼠标的拖拽副本）后再同步顺序，避免与新渲染的实例打架。
      // 此延迟发生在松手后的 click 之后，正好用它吞掉那次 click，避免拖拽结束误触发卡片跳转
      setTimeout(() => {
        commitCardOrder();
        cardSorting = false;
      }, 0);
    },
  }));
}

/* 松手同步：SortableJS 已把 DOM 排好，这里只把顺序写回配置（排序模式下不落盘，点「保存排序」统一保存） */
function commitCardOrder() {
  const order = new Map();
  $$('#groups-wrap .cards-grid').forEach(grid => {
    order.set(+grid.dataset.g, [...grid.querySelectorAll('.card')].map(c => +c.dataset.id));
  });
  cfg.groups.forEach(g => {
    const ids = order.get(g.id);
    if (!ids || !ids.length) return;
    const byId = new Map(g.items.map(it => [it.id, it]));
    g.items = ids.map(id => byId.get(id)).filter(Boolean);
  });
  renderGroups();
}

/* 分组排序不在首页拖拽（在设置抽屉「分组」标签页用上移/下移），此处仅卡片排序 */

/* ============ 弹窗 ============ */
function openModal(html) {
  $('#modal').innerHTML = `<button class="modal-close" id="m-close" title="关闭">×</button>` + html;
  $('#modal-mask').classList.remove('hidden');
  $('#m-close').addEventListener('click', closeModal);
}
function closeModal() {
  closeHexaPicker();
  $('#modal-mask').classList.add('hidden');
  $('#modal').innerHTML = '';
}
function modalFooter(saveId, extra = '') {
  return `<div class="modal-btns">${extra}
    <button class="btn accent" id="${saveId}">保存</button></div>`;
}

/* ---------- 右键上下文菜单 ---------- */
let ctxEl = null;

function showContextMenu(x, y, items) {
  closeContextMenu();
  const list = items.filter(i => !i.hidden);
  if (!list.length) return;
  const el = document.createElement('div');
  el.className = 'ctx-menu';
  el.innerHTML = list.map((i, idx) =>
    `<div class="ctx-item ${i.danger ? 'danger' : ''}" data-idx="${idx}">${esc(i.label)}</div>`).join('');
  document.body.appendChild(el);
  const r = el.getBoundingClientRect();
  el.style.left = Math.max(4, Math.min(x, innerWidth - r.width - 8)) + 'px';
  el.style.top = Math.max(4, Math.min(y, innerHeight - r.height - 8)) + 'px';
  el.addEventListener('click', e => {
    const t = e.target.closest('.ctx-item');
    if (!t) return;
    const item = list[+t.dataset.idx];
    closeContextMenu();
    item && item.action && item.action();
  });
  ctxEl = el;
}

function closeContextMenu() {
  if (ctxEl) { ctxEl.remove(); ctxEl = null; }
}

/* ---------- HEXA 取色器（SV 色块 + 色相 + 透明度 + 8位HEX） ---------- */
function hsvToRgb(h, s, v) {
  s /= 100; v /= 100;
  const k = n => (n + h / 60) % 6;
  const f = n => v - v * s * Math.max(Math.min(k(n), 4 - k(n), 1), 0);
  return { r: Math.round(f(5) * 255), g: Math.round(f(3) * 255), b: Math.round(f(1) * 255) };
}
function rgbToHsv(r, g, b) {
  r /= 255; g /= 255; b /= 255;
  const max = Math.max(r, g, b), min = Math.min(r, g, b), d = max - min;
  let h = 0;
  if (d) {
    if (max === r) h = ((g - b) / d) % 6;
    else if (max === g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    h *= 60; if (h < 0) h += 360;
  }
  return { h, s: max ? d / max * 100 : 0, v: max * 100 };
}
function parseHexa(str) {
  const m = /^#?([0-9a-f]{3}|[0-9a-f]{6}|[0-9a-f]{8})$/i.exec(String(str || '').trim());
  if (!m) return null;
  let x = m[1];
  if (x.length === 3) x = x.split('').map(c => c + c).join('');
  const n = parseInt(x, 16);
  const wide = x.length === 8;
  return {
    r: (n >> (wide ? 24 : 16)) & 255,
    g: (n >> (wide ? 16 : 8)) & 255,
    b: (n >> (wide ? 8 : 0)) & 255,
    a: wide ? n & 255 : 255,
  };
}
function toHexa(r, g, b, a) {
  return '#' + [r, g, b, a].map(x => x.toString(16).padStart(2, '0')).join('');
}

const HEXA_PRESETS = [
  '#00000000', '#000000ff', '#ffffffff', '#0fbf6bff',
  '#2f7cf6ff', '#f7a714ff', '#f5222dff', '#722ed1ff',
];

let hexaPickerEl = null, hexaPickerClose = null;
function closeHexaPicker() {
  if (hexaPickerClose) { hexaPickerClose(); hexaPickerClose = null; }
  hexaPickerEl = null;    // 必须同步清引用，否则下次点击被误判为"已打开"
}

function openHexaPicker(anchor, hexa, onChange) {
  closeHexaPicker();
  const init = parseHexa(hexa) || { r: 0, g: 0, b: 0, a: 0 };
  const ih = rgbToHsv(init.r, init.g, init.b);
  const P = { h: ih.h, s: ih.s, v: ih.v, a: init.a / 255 };

  const el = document.createElement('div');
  el.className = 'hexa-picker';
  el.innerHTML = `
    <div class="hp-sv"><div class="hp-hue-bg"></div><div class="hp-w"></div><div class="hp-b"></div><div class="hp-dot"></div></div>
    <div class="hp-bar hp-hue"><div class="hp-dot"></div></div>
    <div class="hp-bar hp-alpha"><div class="hp-checker"></div><div class="hp-grad"></div><div class="hp-dot"></div></div>
    <div class="hp-row"><span>HEXA</span><input type="text" class="hp-hex" maxlength="9" spellcheck="false"></div>
    <div class="hp-presets">${HEXA_PRESETS.map(p => `<span class="hp-preset hp-checker"><i style="background:${p}"></i></span>`).join('')}</div>`;
  document.body.appendChild(el);

  // 定位在触发字段下方，越界时翻转
  const ar = anchor.getBoundingClientRect();
  const pr = el.getBoundingClientRect();
  let left = Math.max(8, Math.min(ar.left, innerWidth - pr.width - 10));
  let top = ar.bottom + 6;
  if (top + pr.height > innerHeight - 10) top = Math.max(8, ar.top - pr.height - 6);
  el.style.left = left + 'px';
  el.style.top = top + 'px';

  const sv = el.querySelector('.hp-sv'), hueBar = el.querySelector('.hp-hue'),
        alphaBar = el.querySelector('.hp-alpha'), hexInput = el.querySelector('.hp-hex');

  function update(from) {
    const c = hsvToRgb(P.h, P.s, P.v);
    sv.querySelector('.hp-hue-bg').style.background = `hsl(${P.h},100%,50%)`;
    sv.querySelector('.hp-dot').style.left = P.s + '%';
    sv.querySelector('.hp-dot').style.top = (100 - P.v) + '%';
    hueBar.querySelector('.hp-dot').style.left = (P.h / 360 * 100) + '%';
    alphaBar.querySelector('.hp-grad').style.background =
      `linear-gradient(to right, rgba(${c.r},${c.g},${c.b},0), rgb(${c.r},${c.g},${c.b}))`;
    alphaBar.querySelector('.hp-dot').style.left = (P.a * 100) + '%';
    if (from !== 'hex') hexInput.value = toHexa(c.r, c.g, c.b, Math.round(P.a * 255));
    onChange(toHexa(c.r, c.g, c.b, Math.round(P.a * 255)));
  }

  function bindDrag(bar, fn) {
    bar.addEventListener('pointerdown', e => {
      bar.setPointerCapture(e.pointerId);
      const r = bar.getBoundingClientRect();
      const move = ev => fn(Math.min(Math.max((ev.clientX - r.left) / r.width, 0), 1));
      move(e);
      const mv = ev => move(ev);
      const up = () => { bar.removeEventListener('pointermove', mv); bar.removeEventListener('pointerup', up); };
      bar.addEventListener('pointermove', mv);
      bar.addEventListener('pointerup', up);
    });
  }
  bindDrag(hueBar, p => { P.h = p * 360; update('hue'); });
  bindDrag(alphaBar, p => { P.a = p; update('alpha'); });

  sv.addEventListener('pointerdown', e => {
    sv.setPointerCapture(e.pointerId);
    const r = sv.getBoundingClientRect();
    const move = ev => {
      P.s = Math.min(Math.max((ev.clientX - r.left) / r.width, 0), 1) * 100;
      P.v = (1 - Math.min(Math.max((ev.clientY - r.top) / r.height, 0), 1)) * 100;
      update('sv');
    };
    move(e);
    const mv = ev => move(ev);
    const up = () => { sv.removeEventListener('pointermove', mv); sv.removeEventListener('pointerup', up); };
    sv.addEventListener('pointermove', mv);
    sv.addEventListener('pointerup', up);
  });

  hexInput.addEventListener('input', () => {
    const p = parseHexa(hexInput.value);
    if (!p) return;
    const h = rgbToHsv(p.r, p.g, p.b);
    P.h = h.h; P.s = h.s; P.v = h.v; P.a = p.a / 255;
    update('hex');
  });

  el.querySelectorAll('.hp-preset').forEach((s, i) => {
    s.addEventListener('click', () => {
      const p = parseHexa(HEXA_PRESETS[i]);
      const h = rgbToHsv(p.r, p.g, p.b);
      P.h = h.h; P.s = h.s; P.v = h.v; P.a = p.a / 255;
      update('preset');
    });
  });

  const onDocClick = e => {
    if (!el.contains(e.target) && !anchor.contains(e.target)) closeHexaPicker();
  };
  const onKey = e => { if (e.key === 'Escape') closeHexaPicker(); };
  setTimeout(() => document.addEventListener('click', onDocClick), 0);
  document.addEventListener('keydown', onKey);
  hexaPickerEl = el;
  hexaPickerClose = () => {
    el.remove();
    document.removeEventListener('click', onDocClick);
    document.removeEventListener('keydown', onKey);
  };
  update();
}

/* ---------- 卡片编辑弹窗 ---------- */
function itemModal(gid, item) {
  const srcGrp = cfg.groups.find(g => g.id === gid);
  if (!srcGrp) return;
  // item 为空表示添加；为数字表示卡片 ID（编辑）
  const isEdit = item != null;
  const it = isEdit
    ? srcGrp.items.find(x => x.id === (typeof item === 'number' ? item : item.id))
    : { name: '', url: '', desc: '', emoji: '', icon_url: '', intranet_url: '', icon_bg: '#0000006f', open_mode: 'new' };   // 新建默认半透明黑（弹窗里可改）
  if (isEdit && !it) return;
  let bgVal = it.icon_bg || '';                  // ''=默认毛玻璃，'#00000000'=全透明，其他=自定义色
  let iconType = it.id ? (it.icon_url ? 'img' : 'text') : 'img';   // 图标模式（新建默认图片）

  openModal(`
    <h3>修改项目</h3>
    <div class="form-grid2">
      <div class="form-row"><label>分组</label>
        <select id="f-group">${cfg.groups.map(g =>
          `<option value="${g.id}" ${g.id === gid ? 'selected' : ''}>${esc(g.name)}</option>`).join('')}</select></div>
      <div class="form-row"><label>标题<b class="req">*</b></label>
        <div class="input-count"><input type="text" id="f-name" maxlength="20" value="${esc(it.name)}">
          <span class="in-count" id="c-name">${it.name.length} / 20</span></div></div>
    </div>
    <div class="form-row"><label>图标</label>
      <div class="radio-row">
        <label><input type="radio" name="f-icontype" value="text" ${iconType === 'text' ? 'checked' : ''}> 文字</label>
        <label><input type="radio" name="f-icontype" value="img" ${iconType === 'img' ? 'checked' : ''}> 图片</label>
      </div>
      <div class="icon-edit-row">
        <div class="card-icon icon-preview" id="f-iconprev"></div>
        <div class="grow">
          <div class="form-row" id="row-emoji"><input type="text" id="f-emoji" placeholder="文字图标" value="${esc(it.emoji)}"></div>
          <div class="form-row" id="row-iconurl" hidden>
            <input type="text" id="f-iconurl" placeholder="http(s)://" value="${esc(it.icon_url)}">
            <button class="btn small" id="f-upload">本地上传</button>
          </div>
        </div>
      </div>
    </div>
    <div class="inline-row">
      <span class="inline-label">背景颜色</span>
      <div class="hexa-field" id="f-bgfield" title="点击选取颜色">
        <span class="hexa-swatch hp-checker" id="f-bgswatch"><i></i></span>
        <input type="text" id="f-bgtext" maxlength="9" spellcheck="false">
      </div>
      <a class="link-btn" id="f-bgreset">重置</a>
    </div>
    <div class="form-row"><label>地址<b class="req">*</b></label>
      <div class="icon-preview-row">
        <input type="text" id="f-url" placeholder="http(s)://" value="${esc(it.url)}">
        <button class="btn small" data-fetchicon="f-url">获取图标</button>
      </div>
    </div>
    <div class="form-row"><label>内网地址（可选，同样支持 location.hostname，内网模式点击卡片打开）</label>
      <div class="icon-preview-row">
        <input type="text" id="f-intranet" placeholder="http(s)://" value="${esc(it.intranet_url || '')}">
        <button class="btn small" data-fetchicon="f-intranet">获取图标</button>
      </div>
    </div>
    <div class="form-row"><label>描述信息</label>
      <div class="input-count"><input type="text" id="f-desc" maxlength="100" value="${esc(it.desc)}">
        <span class="in-count" id="c-desc">${(it.desc || '').length} / 100</span></div></div>
    <div class="form-row"><label>打开方式</label>
      <div class="np-select">
        <input type="hidden" id="f-openmode" value="${it.open_mode === 'current' ? 'current' : it.open_mode === 'modal' ? 'modal' : 'new'}">
        <button type="button" class="np-select-btn" id="f-openmode-btn"><span id="f-openmode-label"></span><i class="np-caret">▾</i></button>
        <div class="np-select-pop hidden" id="f-openmode-pop">
          <div class="np-option" data-v="current">当前页面打开</div>
          <div class="np-option" data-v="new">新窗口打开</div>
          <div class="np-option" data-v="modal">当前页面弹窗打开</div>
        </div>
      </div>
    </div>
    ${modalFooter('f-save')}`);

  const $emoji = $('#f-emoji'), $iconurl = $('#f-iconurl'), $name = $('#f-name'),
        $desc = $('#f-desc'), $bgfield = $('#f-bgfield'), $bgtext = $('#f-bgtext'),
        $bgswatch = $('#f-bgswatch i'), $prev = $('#f-iconprev');

  // 打开方式下拉弹层：同步选中态
  const omPop = $('#f-openmode-pop');
  const omSync = () => {
    const v = $('#f-openmode').value;
    $('#f-openmode-label').textContent = omPop.querySelector(`.np-option[data-v="${v}"]`).textContent;
    omPop.querySelectorAll('.np-option').forEach(o => o.classList.toggle('active', o.dataset.v === v));
  };
  omSync();
  $('#f-openmode-btn').addEventListener('click', () => omPop.classList.toggle('hidden'));
  omPop.addEventListener('click', e => {
    const o = e.target.closest('.np-option');
    if (!o) return;
    $('#f-openmode').value = o.dataset.v;
    omSync();
    omPop.classList.add('hidden');
  });

  function renderBgField(syncText) {
    // 整个字段行即颜色实色，HEX 白字显示在色块上
    const p = parseHexa(bgVal || '#00000000');
    if (p && p.a === 0) {
      $bgfield.style.background =
        'conic-gradient(#cfcfcf 0 25%, #ffffff 0 50%, #cfcfcf 0 75%, #ffffff 0) 0 0/16px 16px';
    } else {
      $bgfield.style.background = bgVal || 'transparent';
    }
    // 全透明显示棋盘格（浅底）时用纯黑字加白光晕，否则按亮度自适应
    if ($bgtext) {
      $bgtext.style.color = p && p.a === 0 ? '#000'
        : p && (0.299 * p.r + 0.587 * p.g + 0.114 * p.b) > 140 ? '#333' : '#fff';
      $bgtext.style.textShadow = p && p.a === 0 ? '0 0 3px rgba(255,255,255,.95), 0 0 2px rgba(255,255,255,.95)' : 'none';
    }
    if (syncText) $bgtext.value = bgVal || '#00000000';
  }

  function renderPreview() {
    // 预览块背景跟随卡片背景色：全透明显示棋盘格，未设置用默认灰底
    const pv = parseHexa(bgVal || '');
    if (!bgVal) $prev.style.background = '';
    else if (pv && pv.a === 0) {
      $prev.style.background =
        'conic-gradient(#cfcfcf 0 25%, #ffffff 0 50%, #cfcfcf 0 75%, #ffffff 0) 0 0/16px 16px';
    } else {
      $prev.style.background = bgVal;
    }
    let inner;
    if (iconType === 'img' && $iconurl.value.trim()) {
      inner = `<img src="${esc($iconurl.value.trim())}" alt="">`;
    } else if (iconType === 'text' && $emoji.value.trim()) {
      inner = esc($emoji.value.trim());
    } else {
      const n = $name.value.trim();
      inner = `<div class="letter">${esc(n.trim()[0] || '?')}</div>`;   // 预览裸字，不带色块
    }
    $prev.innerHTML = inner;
    // 预览图加载失败时回退为裸字母
    const img = $prev.querySelector('img');
    if (img) img.onerror = () => {
      const n = $name.value.trim();
      $prev.innerHTML = `<div class="letter">${esc(n.trim()[0] || '?')}</div>`;
    };
  }

  function syncIconRows() {
    $('#row-emoji').hidden = iconType !== 'text';
    $('#row-iconurl').hidden = iconType !== 'img';
    renderPreview();
  }
  function setIconType(t) {
    iconType = t;
    $$('input[name="f-icontype"]').forEach(r => r.checked = r.value === t);
    syncIconRows();
  }

  $$('input[name="f-icontype"]').forEach(r =>
    r.addEventListener('change', () => setIconType(r.value)));
  [$emoji, $iconurl, $name].forEach(el => el.addEventListener('input', renderPreview));
  $name.addEventListener('input', () => $('#c-name').textContent = `${$name.value.length} / 20`);
  $desc.addEventListener('input', () => $('#c-desc').textContent = `${$desc.value.length} / 100`);

  $bgtext.addEventListener('input', () => {
    const p = parseHexa($bgtext.value);
    if (!p) return;
    bgVal = toHexa(p.r, p.g, p.b, p.a);
    renderBgField(false);
    renderPreview();
  });
  $bgfield.addEventListener('click', e => {
    if (hexaPickerEl) { closeHexaPicker(); return; }
    openHexaPicker($bgfield, bgVal, v => { bgVal = v; renderBgField(true); renderPreview(); });
  });
  $('#f-bgreset').addEventListener('click', () => {
    bgVal = '#0000006f';                          // 恢复新建卡片默认色（半透明黑）
    renderBgField(true); renderPreview(); closeHexaPicker();
  });

  // 从地址一键抓取 favicon（默认下载落盘；设置→外观→获取图标方式=引用外链时仅校验后填外链，不落盘）
  $$('#modal [data-fetchicon]').forEach(b => b.addEventListener('click', async () => {
    const raw = $('#' + b.dataset.fetchicon).value.trim();
    if (!raw) { toast('请先填写地址', 'err'); return; }
    const isLink = (cfg.settings.icon_fetch_mode || 'link') === 'link';   // 默认引用外链，显式 save 才落盘
    b.disabled = true; b.textContent = '获取中…';
    try {
      const d = await api('GET', '/api/favicon?url=' + encodeURIComponent(raw) + (isLink ? '&mode=link' : ''));
      $iconurl.value = d.url; setIconType('img');
      toast(isLink ? '已引用外链图标' : '已获取图标', 'ok');
    } catch (e) { toast('获取失败：' + e.message, 'err'); }
    b.disabled = false; b.textContent = '获取图标';
  }));

  $('#f-upload').addEventListener('click', () =>
    pickImage(url => { $iconurl.value = url; setIconType('img'); toast('图片已上传', 'ok'); }));

  $('#f-save').addEventListener('click', () => {
    const name = $name.value.trim();
    let url = $('#f-url').value.trim();
    if (!name || !url) { toast('标题和地址不能为空', 'err'); return; }
    if (!/^https?:\/\//i.test(url)) url = 'https://' + url;
    let intranet = $('#f-intranet').value.trim();
    if (intranet && !/^https?:\/\//i.test(intranet)) intranet = 'https://' + intranet;
    const data = {
      name, url,
      intranet_url: intranet,
      desc: $desc.value.trim(),
      emoji: iconType === 'text' ? $emoji.value.trim() : '',
      icon_url: iconType === 'img' ? $iconurl.value.trim() : '',
      icon_bg: bgVal,
      open_mode: $('#f-openmode').value,
    };
    const newGid = +$('#f-group').value;
    if (isEdit) {
      Object.assign(it, data);
      if (newGid !== gid) { // 更换分组
        const to = cfg.groups.find(g => g.id === newGid);
        if (to) {
          srcGrp.items = srcGrp.items.filter(x => x.id !== it.id);
          to.items.push(it);
        }
      }
    } else {
      const to = cfg.groups.find(g => g.id === newGid) || srcGrp;
      to.items.push({ id: nextId(to.items), ...data });
    }
    closeModal(); renderGroups(); debounceSave();
  });

  renderBgField(true);
  syncIconRows();
}

/* ---------- 分组弹窗 ---------- */
function groupModal(gid) {
  const g = gid ? cfg.groups.find(x => x.id === gid) : null;
  const name = g ? g.name : '';
  openModal(`
    <h3>${g ? '编辑分组' : '添加分组'}</h3>
    <div class="form-row"><label>分组名称 *</label><input type="text" id="f-gname" value="${esc(name)}"></div>
    ${g ? `<div class="form-row"><label>排序</label>
      <div><button class="btn small" id="f-gup">↑ 上移</button>
      <button class="btn small" id="f-gdown">↓ 下移</button></div></div>` : ''}
    ${modalFooter('f-gsave', g ? '<button class="btn danger" id="f-gdel">删除分组</button>' : '')}`);
  if (g) {
    $('#f-gup').addEventListener('click', () => {
      const i = cfg.groups.indexOf(g);
      if (i > 0) { cfg.groups.splice(i, 1); cfg.groups.splice(i - 1, 0, g); renderGroups(); debounceSave(); toast('已上移', 'ok'); }
    });
    $('#f-gdown').addEventListener('click', () => {
      const i = cfg.groups.indexOf(g);
      if (i < cfg.groups.length - 1) { cfg.groups.splice(i, 1); cfg.groups.splice(i + 1, 0, g); renderGroups(); debounceSave(); toast('已下移', 'ok'); }
    });
    $('#f-gdel').addEventListener('click', () => {
      confirmDialog(`删除分组「${g.name}」及其全部 ${g.items.length} 个卡片？`, () => {
        cfg.groups = cfg.groups.filter(x => x !== g);
        closeModal(); renderGroups(); debounceSave(); toast('分组已删除', 'ok');
      });
    });
  }
  $('#f-gsave').addEventListener('click', () => {
    const name = $('#f-gname').value.trim();
    if (!name) { toast('名称不能为空', 'err'); return; }
    if (g) g.name = name;
    else cfg.groups.push({ id: nextId(cfg.groups), name, items: [] });
    closeModal(); renderGroups(); debounceSave();
  });
}

/* ---------- 书签分组弹窗 ---------- */
function bmGroupModal(gid) {
  const g = gid ? cfg.bookmark_groups.find(x => x.id === gid) : null;
  let links = g ? JSON.parse(JSON.stringify(g.links)) : [];
  openModal(`
    <h3>${g ? '编辑书签分组' : '添加书签分组'}</h3>
    <div class="form-row"><label>分组名称 *</label><input type="text" id="f-bname" value="${esc(g ? g.name : '')}"></div>
    <div class="form-row"><label>链接列表</label><div id="f-links" class="mg-links"></div>
      <button class="btn small" id="f-addlink" style="margin-top:6px">＋ 添加链接</button></div>
    ${modalFooter('f-bsave', g ? '<button class="btn danger" id="f-bdel">删除分组</button>' : '')}`);

  const renderLinks = () => {
    $('#f-links').innerHTML = links.map((l, i) => `
      <div class="mg-item">
        <input type="text" placeholder="名称" value="${esc(l.name)}" data-lname="${i}" style="width:90px">
        <input type="text" placeholder="https://" value="${esc(l.url)}" data-lurl="${i}" style="flex:1">
        <button class="link-btn danger" data-ldel="${i}">✕</button>
      </div>`).join('') || '<div class="mg-empty">暂无链接</div>';
    $$('#f-links [data-ldel]').forEach(el => el.addEventListener('click', () => {
      links.splice(+el.dataset.ldel, 1); renderLinks();
    }));
    $$('#f-links [data-lname]').forEach(el => el.addEventListener('input', () => links[+el.dataset.lname].name = el.value));
    $$('#f-links [data-lurl]').forEach(el => el.addEventListener('input', () => links[+el.dataset.lurl].url = el.value));
  };
  renderLinks();
  $('#f-addlink').addEventListener('click', () => { links.push({ id: nextId(links), name: '', url: '' }); renderLinks(); });

  if (g) $('#f-bdel').addEventListener('click', () => {
    confirmDialog(`删除书签分组「${g.name}」？`, () => {
      cfg.bookmark_groups = cfg.bookmark_groups.filter(x => x !== g);
      closeModal(); renderBookmarks(); debounceSave(); toast('已删除', 'ok');
    });
  });
  $('#f-bsave').addEventListener('click', () => {
    const name = $('#f-bname').value.trim();
    if (!name) { toast('名称不能为空', 'err'); return; }
    const clean = links.filter(l => l.name.trim() && l.url.trim());
    if (g) { g.name = name; g.links = clean; }
    else cfg.bookmark_groups.push({ id: nextId(cfg.bookmark_groups), name, links: clean });
    closeModal(); renderBookmarks(); debounceSave(); toast('已保存', 'ok');
  });
}

/* ============ 设置抽屉 ============ */
let curTab = 'appearance';

function openDrawer() {
  $('#drawer').classList.remove('hidden');
  $('#drawer-mask').classList.remove('hidden');
  renderTab();
}
function closeDrawer() {
  $('#drawer').classList.add('hidden');
  $('#drawer-mask').classList.add('hidden');
  flushSave(); // 关闭/隐藏设置面板时统一落盘，避免拖动过程频繁请求造成的抖动
}

function renderTab() {
  $$('.tab-btn').forEach(b => b.classList.toggle('active', b.dataset.tab === curTab));
  ['appearance', 'groups', 'bookmarks', 'engines', 'files', 'account'].forEach(t =>
    $('#tab-' + t).classList.toggle('hidden', t !== curTab));
  ({ appearance: renderAppearanceTab, groups: renderGroupsTab, bookmarks: renderBookmarksTab,
     engines: renderEnginesTab, files: renderFilesTab, account: renderAccountTab })[curTab]();
}

/* ---------- 上传文件管理 ---------- */
function fmtSize(n) {
  if (n < 1024) return n + ' B';
  if (n < 1048576) return (n / 1024).toFixed(1) + ' KB';
  return (n / 1048576).toFixed(2) + ' MB';
}
function renderFilesTab() {
  const panel = $('#tab-files');
  panel.innerHTML = `
    <h3>上传文件</h3>
    <div class="setting-hint">管理上传到服务器的图标、壁纸等文件（data/uploads 目录）</div>
    <div class="files-grid" id="files-grid"><div class="mg-empty">加载中...</div></div>`;
  fetch('/api/files').then(r => { if (!r.ok) throw 0; return r.json(); }).then(list => {
    const grid = $('#files-grid');
    if (!list.length) { grid.innerHTML = '<div class="mg-empty">还没有上传过文件</div>'; return; }
    grid.innerHTML = list.map(f => `
      <div class="file-card" data-name="${esc(f.name)}">
        <div class="file-thumb"><img src="${esc(f.url)}" loading="lazy" alt=""></div>
        <div class="file-name" title="${esc(f.name)}">${esc(f.name)}</div>
        <div class="file-meta">${fmtSize(f.size)}</div>
        <div class="file-ops">
          <button class="file-op" data-copy="${esc(f.url)}" title="复制地址"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="9" y="9" width="12" height="12" rx="2"/><path d="M5 15V5a2 2 0 0 1 2-2h10"/></svg></button>
          <button class="file-op" data-wp="${esc(f.url)}" title="设为壁纸"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="4" width="18" height="16" rx="2"/><circle cx="9" cy="10" r="1.6"/><path d="M21 16l-5-5-8 8"/></svg></button>
          <button class="file-op danger" data-del="${esc(f.name)}" title="删除"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M4 7h16M9 7V5a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v2m3 0v12a2 2 0 0 1-2 2H8a2 2 0 0 1-2-2V7"/></svg></button>
        </div>
      </div>`).join('');
    $$('#files-grid [data-copy]').forEach(el => el.addEventListener('click', () => {
      navigator.clipboard.writeText(location.origin + el.dataset.copy).then(() => toast('地址已复制'));
    }));
    $$('#files-grid [data-wp]').forEach(el => el.addEventListener('click', () => {
      cfg.settings.wallpaper = { type: 'image', value: el.dataset.wp };
      applyAppearance(); debounceSave(); toast('已设为壁纸');
    }));
    $$('#files-grid [data-del]').forEach(el => el.addEventListener('click', () => {
      confirmDialog(`删除文件 ${el.dataset.del}？正在使用它的卡片/壁纸将无法显示`, () => {
        fetch('/api/files/' + encodeURIComponent(el.dataset.del), { method: 'DELETE' })
          .then(r => { if (!r.ok) throw 0; toast('已删除'); renderFilesTab(); })
          .catch(() => toast('删除失败'));
      });
    }));
  }).catch(() => { $('#files-grid').innerHTML = '<div class="mg-empty">加载失败</div>'; });
}

/* ---------- 分组管理 ---------- */
function renderGroupsTab() {
  const panel = $('#tab-groups');
  panel.innerHTML = `
    <h3>卡片分组</h3>
    <div class="mg-list">
      ${cfg.groups.map(g => `
        <div class="mg-item" data-g="${g.id}">
          <span class="drag-handle" title="拖动排序">⠿</span>
          <div class="grow">${esc(g.name)} <span class="sub">(${g.items.length})</span></div>
          <button class="link-btn" data-edit="${g.id}">编辑</button>
          <button class="link-btn danger" data-del="${g.id}">删除</button>
        </div>`).join('') || '<div class="mg-empty">暂无分组，点击下方按钮添加</div>'}
    </div>
    <button class="btn accent" id="g-add" style="margin-top:12px">＋ 添加分组</button>`;
  $$('#tab-groups [data-edit]').forEach(el =>
    el.addEventListener('click', () => { closeDrawer(); groupModal(+el.dataset.edit); }));
  $$('#tab-groups [data-del]').forEach(el =>
    el.addEventListener('click', () => {
      const g = cfg.groups.find(x => x.id === +el.dataset.del);
      if (g) confirmDialog(`删除分组「${g.name}」及其 ${g.items.length} 个卡片？`, () => {
        cfg.groups = cfg.groups.filter(x => x !== g);
        renderGroups(); renderGroupsTab(); debounceSave();
      });
    }));
  $('#g-add').addEventListener('click', () => { closeDrawer(); groupModal(null); });

  // 分组拖拽排序：SortableJS（与首页卡片同款），拖动中实时跟随，松手按 DOM 顺序写回并落盘
  const finishGroupSort = () => {
    const list = $$('#tab-groups .mg-item');
    const byId = new Map(cfg.groups.map(g => [g.id, g]));
    const next = list.map(r => byId.get(+r.dataset.g)).filter(Boolean);
    if (!next.some((g, i) => cfg.groups[i] !== g)) return;   // 顺序未变
    cfg.groups = next;
    renderGroupsTab(); renderGroups(); debounceSave();
    toast('分组排序已保存', 'ok');
  };
  const mgList = $('#tab-groups .mg-list');
  mgSortable?.destroy();
  mgSortable = null;
  if (mgList && typeof Sortable !== 'undefined') {
    mgSortable = Sortable.create(mgList, {
      animation: 150,
      handle: '.drag-handle',
      draggable: '.mg-item',
      ghostClass: 'dragging',
      forceFallback: true,
      fallbackOnBody: true,
      fallbackClass: 'sortable-fallback',
      // 等 Sortable 收尾后再重建列表（会替换掉它正在使用的 DOM）
      onEnd: () => setTimeout(finishGroupSort, 0),
    });
  }
}

/* ---------- 外观 ---------- */
function renderAppearanceTab() {
  const s = cfg.settings;
  const wp = s.wallpaper || {};
  // 自定义渐变回显：当前是渐变壁纸就用当前值，否则沿用上次保存的自定义渐变
  const isGrad = wp.type === 'gradient';
  const cg = parseGrad(isGrad ? wp.value : s.wallpaper_grad) || { c1: '#667eea', c2: '#764ba2', deg: 135 };
  const gradValue = `linear-gradient(${cg.deg}deg, ${cg.c1}, ${cg.c2})`;
  const dis = isGrad ? '' : ' disabled';
  const wpList = wpAssets.length ? wpAssets : ['/assets/default-bg'];
  const isPct = s.content_max_unit === '%';
  // 左右边距为独立设置（与最大宽度解耦）：固定 0–200px，便于细调
  const padv = Math.min(200, Math.max(0, s.content_pad_x == null ? 20 : +s.content_pad_x));
  const panel = $('#tab-appearance');
  panel.innerHTML = `
    <div class="set-card"><h3>页面标题</h3>
      <div class="setting-row"><label>标题</label><input type="text" id="s-title" value="${esc(s.title || '')}"></div>
    </div>
    <div class="set-card"><h3>主题色</h3>
      <div class="setting-row"><label>强调色</label>
        <div class="hexa-field" id="s-accentfield"><span class="hexa-swatch"><i></i></span>
          <input type="text" maxlength="9" spellcheck="false"></div></div>
      <div class="setting-row"><label>深色模式</label>
        <label class="switch"><input type="checkbox" id="s-dark" ${document.body.classList.contains('dark') ? 'checked' : ''}><i></i></label></div>
    </div>
    <div class="set-card"><h3>时钟组件</h3>
      <div class="setting-row"><label>显示秒</label>
        <label class="switch"><input type="checkbox" id="s-clocksec" ${s.clock_seconds ? 'checked' : ''}><i></i></label></div>
    </div>
    <div class="set-card"><h3>搜索栏组件</h3>
      <div class="setting-row"><label>显示</label>
        <label class="switch"><input type="checkbox" id="s-showsearch" ${s.show_search !== false ? 'checked' : ''}><i></i></label></div>
      <div id="s-searchopts" ${s.show_search === false ? 'hidden' : ''}>
        <div class="setting-row" id="s-searchstylerow">
          <label>样式</label>
          <input type="range" id="s-searchblur" min="0" max="25" step="1" value="${Math.min(25, Math.max(0, +s.search_blur || 0))}" title="毛玻璃模糊">
          <span class="row-unit" id="s-searchblur-v">${Math.min(25, Math.max(0, +s.search_blur || 0))}px</span>
          <input type="number" id="s-searchopacity" min="0" max="100" step="5" value="${Math.min(100, Math.max(0, s.search_opacity == null ? 55 : +s.search_opacity))}" title="背景透明度(%)">
          <input type="number" id="s-searchfont" min="10" max="24" step="1" value="${+s.search_font || 16}" title="字体大小(px)"></div>
      </div>
    </div>
    <div class="set-card"><h3>卡片</h3>
      <div class="setting-row"><label>图标风格</label>
        <select id="s-iconstyle">
          <option value="detail" ${(s.icon_style || 'detail') === 'detail' ? 'selected' : ''}>详情图标</option>
          <option value="mini" ${s.icon_style === 'mini' ? 'selected' : ''}>小图标</option>
        </select></div>
      <div class="setting-row"><label>获取图标方式</label>
        <select id="s-iconfetch">
          <option value="save" ${s.icon_fetch_mode === 'save' ? 'selected' : ''}>下载保存</option>
          <option value="link" ${(s.icon_fetch_mode || 'link') === 'link' ? 'selected' : ''}>引用外链</option>
        </select></div>
      <div class="setting-row"><label>隐藏描述信息</label>
        <label class="switch"><input type="checkbox" id="s-hidedesc" ${s.hide_desc ? 'checked' : ''}><i></i></label></div>
      <div class="setting-row"><label>毛玻璃</label><input type="range" id="s-cardblur" min="0" max="40" step="1" value="${+s.card_blur || 0}"></div>
      <div class="setting-row"><label>文字颜色</label>
        <div class="hexa-field" id="s-textcolorfield"><span class="hexa-swatch"><i></i></span>
          <input type="text" maxlength="9" spellcheck="false"></div></div>
    </div>
    <div class="set-card"><h3>内外网切换</h3>
      <div class="setting-row"><label>自动判断内外网</label>
        <label class="switch"><input type="checkbox" id="s-netauto" ${s.net_mode_switch === 'auto' ? 'checked' : ''}><i></i></label></div>
    </div>
    <div class="set-card"><h3>内容区域</h3>
      <div class="setting-row"><label>最大宽度</label>
        <input type="number" id="s-maxw" min="${isPct ? 30 : 600}" max="${isPct ? 100 : 3000}" step="${isPct ? 1 : 10}" value="${+s.content_max_width || (isPct ? 100 : 1200)}" style="flex:none;width:110px">
        <select id="s-maxu" class="unit-sel">
          <option value="px"${isPct ? '' : ' selected'}>px</option>
          <option value="%"${isPct ? ' selected' : ''}>%</option>
        </select></div>
      <div class="setting-row"><label>左右边距</label>
        <input type="range" id="s-padx" min="0" max="200" step="1" value="${padv}"></div>
      <div class="setting-row"><label>上边距 (%)</label>
        <input type="range" id="s-mt" min="0" max="40" step="1" value="${s.content_margin_top == null ? 8 : +s.content_margin_top}"></div>
      <div class="setting-row"><label>下边距 (%)</label>
        <input type="range" id="s-mb" min="0" max="40" step="1" value="${s.content_margin_bottom == null ? 0 : +s.content_margin_bottom}"></div>
    </div>
    <div class="set-card"><h3>自定义页脚</h3>
      <textarea id="s-footer" rows="4" placeholder="支持 HTML，留空则不显示页脚">${esc(s.custom_footer || '')}</textarea>
    </div>
    <div class="set-card"><h3>壁纸</h3>
      <div id="wp-upload" title="点击上传替换图片或拖拽到框内"
        style="background:${wp.type === 'image' ? `url('${esc(wp.value)}') center/cover` : esc(wp.value || FALLBACK_WP)}">
        <span>点击上传替换图片或拖拽到框内</span>
      </div>
      <div class="setting-row"><label>预设</label>
        <div class="wp-mini">
          ${wpList.map(u => `<div class="wp-dot ${wp.type === 'image' && wpKey(wp.value) === wpKey(u) ? 'active' : ''}" data-wp="${esc(u)}" title="${esc(u.split('/').pop())}" style="background:url('${esc(u)}') center/cover"></div>`).join('')}
          <div class="wp-dot ${isGrad ? 'active' : ''}" data-wp="grad" title="自定义渐变" style="background:${gradValue}"></div>
        </div></div>
      <div class="setting-row"><label>自定义渐变</label>
        <div class="wp-grad">
          <input type="color" id="s-wp1" value="${cg.c1}" title="起始色"${dis}>
          <span class="wp-grad-bar" id="s-wpgbar" style="background:${gradValue}"></span>
          <input type="color" id="s-wp2" value="${cg.c2}" title="结束色"${dis}>
        </div></div>
      <div class="setting-row"><label>渐变角度</label>
        <input type="range" id="s-wpangle" min="0" max="360" step="5" value="${cg.deg}"${dis}>
        <span class="row-unit" id="s-wpangle-v">${cg.deg}°</span></div>
      <div class="setting-row"><label>展开图片链接输入框</label>
        <label class="switch"><input type="checkbox" id="s-wpurltoggle"><i></i></label></div>
      <div class="setting-row" id="s-wpurlrow" hidden><label>图片 URL</label>
        <input type="text" id="s-wpurl" placeholder="http(s)://">
        <button class="btn small" id="s-wpapply">应用</button></div>
      <div class="setting-row"><label>模糊</label><input type="range" id="s-wpblur" min="0" max="60" step="1" value="${+s.wallpaper_blur || 0}"></div>
      <div class="setting-row"><label>遮罩</label><input type="range" id="s-wpmask" min="0" max="80" step="1" value="${+s.wallpaper_mask || 0}"></div>
    </div>`;

  $('#s-title').addEventListener('input', e => {
    cfg.settings.title = e.target.value.trim();
    applyAppearance(); debounceSave();
  });
  $('#s-dark').addEventListener('change', e => {
    localStorage.setItem('np_dark', e.target.checked ? '1' : '0');
    applyDark();
  });
/* 设置抽屉里的颜色字段：色块 + HEX 显示 + 取色器 */
function bindSettingColorField(fieldId, initial, onChange) {
  const field = $(fieldId);
  if (!field) return;
  const text = field.querySelector('input');
  const norm = v => { const p = parseHexa(v); return p ? toHexa(p.r, p.g, p.b, p.a) : null; };
  const start = norm(initial) || '#ffffffff';
  const render = () => {
    const p = parseHexa(text.value);
    if (p && p.a === 0) {
      field.style.background =
        'conic-gradient(#cfcfcf 0 25%, #ffffff 0 50%, #cfcfcf 0 75%, #ffffff 0) 0 0/16px 16px';
    } else {
      field.style.background = text.value;
    }
    // 全透明显示棋盘格（浅底）时用纯黑字加白光晕，否则按亮度自适应
    text.style.color = p && p.a === 0 ? '#000'
      : p && (0.299 * p.r + 0.587 * p.g + 0.114 * p.b) > 140 ? '#333' : '#fff';
    text.style.textShadow = p && p.a === 0 ? '0 0 3px rgba(255,255,255,.95), 0 0 2px rgba(255,255,255,.95)' : 'none';
  };
  text.value = start;
  render();
  field.addEventListener('click', () => {
    if (hexaPickerEl) { closeHexaPicker(); return; }
    openHexaPicker(field, text.value, v => { text.value = v; render(); onChange(v); });
  });
  text.addEventListener('change', () => {
    const nv = norm(text.value);
    if (nv) { text.value = nv; render(); onChange(nv); }
    else text.value = start;
  });
}

  $('#s-clocksec').addEventListener('change', e => {
    cfg.settings.clock_seconds = e.target.checked;
    applyAppearance(); // 立即生效并同步预渲染缓存
    debounceSave();
  });
  $('#s-showsearch').addEventListener('change', e => {
    cfg.settings.show_search = e.target.checked;
    // 搜索框样式设置仅在显示搜索栏时出现
    $('#s-searchopts').hidden = !e.target.checked;
    applyAppearance(); debounceSave();
  });
  $('#s-searchblur').addEventListener('input', e => {
    cfg.settings.search_blur = +e.target.value;
    $('#s-searchblur-v').textContent = e.target.value + 'px';
    applyAppearance(); debounceSave();
  });
  // 透明度数字框：步进 5，越界或空值回落默认 55
  $('#s-searchopacity').addEventListener('change', e => {
    let v = parseInt(e.target.value, 10);
    if (isNaN(v)) v = 55;
    v = Math.min(100, Math.max(0, v));
    e.target.value = v;
    cfg.settings.search_opacity = v;
    applyAppearance(); debounceSave();
  });
  $('#s-searchfont').addEventListener('change', e => {
    // 数字框：越界或空值回落到默认 16
    const v = Math.min(24, Math.max(10, parseInt(e.target.value, 10) || 16));
    e.target.value = v;
    cfg.settings.search_font = v;
    applyAppearance(); debounceSave();
  });
  $('#s-hidedesc').addEventListener('change', e => {
    cfg.settings.hide_desc = e.target.checked;
    renderGroups(); debounceSave();
  });
  $('#s-cardblur').addEventListener('input', e => {
    cfg.settings.card_blur = +e.target.value;
    applyAppearance(); debounceSave();
  });
  $('#s-iconstyle').addEventListener('change', e => {
    cfg.settings.icon_style = e.target.value;
    renderGroups(); debounceSave();
  });
  $('#s-iconfetch').addEventListener('change', e => {
    cfg.settings.icon_fetch_mode = e.target.value;
    debounceSave();
    toast(e.target.value === 'link' ? '获取图标将引用外链地址（不落盘）' : '获取图标将下载保存到服务器', 'ok');
  });
  $('#s-netauto').addEventListener('change', e => {
    cfg.settings.net_mode_switch = e.target.checked ? 'auto' : 'manual';
    renderNetToggle(); renderGroups(); debounceSave();
    toast(e.target.checked ? '已开启自动判断（按访问来源 IP）' : '已改为手动切换（右下角按钮）', 'ok');
  });
  bindSettingColorField('#s-accentfield', s.accent || '#667eea', v => {
    cfg.settings.accent = v;
    applyAppearance(); debounceSave();
  });
  bindSettingColorField('#s-textcolorfield', s.text_color || '#ffffff', v => {
    cfg.settings.text_color = v;
    applyAppearance(); debounceSave();
  });
  // 壁纸大上传框：点击或拖拽上传
  const wpBox = $('#wp-upload');
  const applyWp = () => { applyAppearance(); renderAppearanceTab(); debounceSave(); };
  wpBox.addEventListener('click', () =>
    pickImage(url => { cfg.settings.wallpaper = { type: 'image', value: url }; applyWp(); toast('壁纸已更新', 'ok'); }));
  wpBox.addEventListener('dragover', e => { e.preventDefault(); wpBox.classList.add('drag'); });
  wpBox.addEventListener('dragleave', () => wpBox.classList.remove('drag'));
  wpBox.addEventListener('drop', e => {
    e.preventDefault(); wpBox.classList.remove('drag');
    const f = e.dataTransfer.files && e.dataTransfer.files[0];
    if (f) uploadFile(f, url => { cfg.settings.wallpaper = { type: 'image', value: url }; applyWp(); toast('壁纸已更新', 'ok'); });
  });
  $$('#tab-appearance [data-wp]').forEach(el => el.addEventListener('click', () => {
    if (el.dataset.wp === 'grad') {
      // 选中「自定义渐变」预设，才启用下方取色器与角度滑条
      cfg.settings.wallpaper = { type: 'gradient', value: gradValue };
      cfg.settings.wallpaper_grad = gradValue;
    } else {
      cfg.settings.wallpaper = { type: 'image', value: el.dataset.wp };
    }
    applyWp();
  }));
  // 自定义渐变：拖动取色器或角度滑条即实时生成壁纸并保存
  const updateGrad = () => {
    const c1 = $('#s-wp1').value, c2 = $('#s-wp2').value, deg = +$('#s-wpangle').value;
    const value = `linear-gradient(${deg}deg, ${c1}, ${c2})`;
    $('#s-wpangle-v').textContent = deg + '°';
    $('#s-wpgbar').style.background = value;
    cfg.settings.wallpaper = { type: 'gradient', value };
    cfg.settings.wallpaper_grad = value;
    applyAppearance(); debounceSave();
  };
  ['#s-wp1', '#s-wp2', '#s-wpangle'].forEach(id => $(id).addEventListener('input', updateGrad));
  $('#s-wpurltoggle').addEventListener('change', e => {
    $('#s-wpurlrow').hidden = !e.target.checked;
  });
  $('#s-wpapply').addEventListener('click', () => {
    const u = $('#s-wpurl').value.trim();
    if (!u) { toast('请先填写图片 URL', 'err'); return; }
    cfg.settings.wallpaper = { type: 'image', value: u };
    applyWp(); toast('壁纸已更新', 'ok');
  });
  $('#s-wpblur').addEventListener('input', e => {
    cfg.settings.wallpaper_blur = +e.target.value;
    applyAppearance(); debounceSave();
  });
  $('#s-wpmask').addEventListener('input', e => {
    cfg.settings.wallpaper_mask = +e.target.value;
    applyAppearance(); debounceSave();
  });
  // 内容区域：最大宽度支持 px / %，切换单位时按当前视口换算，避免数值含义突变
  const maxw = $('#s-maxw'), maxu = $('#s-maxu');
  const applyMaxW = () => {
    cfg.settings.content_max_unit = maxu.value;
    cfg.settings.content_max_width = +maxw.value || (maxu.value === '%' ? 100 : 1200);
    applyAppearance(); debounceSave();
  };
  maxw.addEventListener('change', applyMaxW);
  maxu.addEventListener('change', () => {
    const vw = window.innerWidth || 1200;
    const cur = +maxw.value || 1200;
    if (maxu.value === '%') {
      maxw.min = 30; maxw.max = 100; maxw.step = 1;
      maxw.value = Math.min(100, Math.max(30, Math.round(cur / vw * 100)));
    } else {
      maxw.min = 600; maxw.max = 3000; maxw.step = 10;
      maxw.value = Math.min(3000, Math.max(600, Math.round(cur / 100 * vw / 10) * 10));
    }
    applyMaxW();
  });
  $('#s-padx').addEventListener('input', e => {
    cfg.settings.content_pad_x = +e.target.value;
    applyAppearance(); debounceSave();
  });
  $('#s-mt').addEventListener('input', e => {
    cfg.settings.content_margin_top = +e.target.value;
    applyAppearance(); debounceSave();
  });
  $('#s-mb').addEventListener('input', e => {
    cfg.settings.content_margin_bottom = +e.target.value;
    applyAppearance(); debounceSave();
  });
  $('#s-footer').addEventListener('input', e => {
    cfg.settings.custom_footer = e.target.value;
    applyAppearance(); debounceSave();
  });
}

/* ---------- 书签 ---------- */
function renderBookmarksTab() {
  const panel = $('#tab-bookmarks');
  const bgs = cfg.bookmark_groups || [];
  panel.innerHTML = `
    <h3>书签分组</h3>
    <div class="mg-list">
      ${bgs.map(g => `
        <div class="mg-item">
          <div class="grow">${esc(g.name)} <span class="sub">(${g.links.length})</span></div>
          <button class="link-btn" data-edit="${g.id}">编辑</button>
          <button class="link-btn danger" data-del="${g.id}">删除</button>
        </div>`).join('') || '<div class="mg-empty">暂无书签分组，点击下方按钮添加</div>'}
    </div>
    <button class="btn accent" id="b-add" style="margin-top:12px">＋ 添加书签分组</button>`;
  $$('#tab-bookmarks [data-edit]').forEach(el =>
    el.addEventListener('click', () => bmGroupModal(+el.dataset.edit)));
  $$('#tab-bookmarks [data-del]').forEach(el =>
    el.addEventListener('click', () => {
      const g = cfg.bookmark_groups.find(x => x.id === +el.dataset.del);
      if (g) confirmDialog(`删除书签分组「${g.name}」？`, () => {
        cfg.bookmark_groups = cfg.bookmark_groups.filter(x => x !== g);
        renderBookmarks(); renderBookmarksTab(); debounceSave();
      });
    }));
  $('#b-add').addEventListener('click', () => { bmGroupModal(null); closeDrawer(); });
}

/* ---------- 搜索引擎 ---------- */
function renderEnginesTab() {
  const panel = $('#tab-engines');
  const es = cfg.settings.engines || [];
  panel.innerHTML = `
    <h3>搜索引擎（URL 中用 %s 表示关键词）</h3>
    <div class="mg-list">
      ${es.map(e => `
        <div class="mg-item">
          <div class="grow">${esc(e.name)} <span class="sub">${esc(e.url)}</span></div>
          <button class="link-btn" data-def="${esc(e.id)}" title="设为默认">${e.id === cfg.settings.default_engine ? '★ 默认' : '☆'}</button>
          <button class="link-btn" data-edit="${esc(e.id)}">编辑</button>
          <button class="link-btn danger" data-del="${esc(e.id)}">删除</button>
        </div>`).join('') || '<div class="mg-empty">暂无搜索引擎</div>'}
    </div>
    <button class="btn accent" id="e-add" style="margin-top:12px">＋ 添加搜索引擎</button>`;

  $$('#tab-engines [data-def]').forEach(el => el.addEventListener('click', () => {
    cfg.settings.default_engine = el.dataset.def;
    renderEnginesSelect(); renderEnginesTab(); debounceSave();
  }));
  $$('#tab-engines [data-del]').forEach(el => el.addEventListener('click', () => {
    const id = el.dataset.del;
    if (es.length <= 1) { toast('至少保留一个搜索引擎', 'err'); return; }
    cfg.settings.engines = cfg.settings.engines.filter(x => x.id !== id);
    if (cfg.settings.default_engine === id) cfg.settings.default_engine = cfg.settings.engines[0].id;
    renderEnginesSelect(); renderEnginesTab(); debounceSave();
  }));
  $$('#tab-engines [data-edit]').forEach(el => el.addEventListener('click', () => {
    engineModal(cfg.settings.engines.find(x => x.id === el.dataset.edit));
  }));
  $('#e-add').addEventListener('click', () => engineModal(null));
}

function engineModal(e) {
  const isEdit = !!e;
  e = e || { id: '', name: '', url: '' };
  openModal(`
    <h3>${isEdit ? '编辑搜索引擎' : '添加搜索引擎'}</h3>
    <div class="form-row"><label>名称 *</label><input type="text" id="f-ename" value="${esc(e.name)}"></div>
    <div class="form-row"><label>搜索 URL *（用 %s 表示关键词，如 https://www.baidu.com/s?wd=%s）</label>
      <input type="text" id="f-eurl" value="${esc(e.url)}"></div>
    ${modalFooter('f-esave')}`);
  $('#f-esave').addEventListener('click', () => {
    const name = $('#f-ename').value.trim();
    const url = $('#f-eurl').value.trim();
    if (!name || !url.includes('%s')) { toast('请填写名称，URL 需包含 %s', 'err'); return; }
    if (isEdit) { e.name = name; e.url = url; }
    else {
      const id = 'e' + Date.now().toString(36);
      cfg.settings.engines.push({ id, name, url });
      if (!cfg.settings.default_engine) cfg.settings.default_engine = id;
    }
    closeModal(); renderEnginesSelect(); renderEnginesTab(); debounceSave();
  });
}

/* ---------- 账号 ---------- */
function renderAccountTab() {
  const panel = $('#tab-account');
  panel.innerHTML = `
    <h3>账号</h3>
    <div class="setting-row"><label>当前用户：</label><span class="static-text">${esc(cfg.username || '')}</span></div>
    <div class="form-row" style="margin-top:8px"><label>新用户名</label>
      <input type="text" id="a-user" placeholder="留空表示不修改"></div>
    <div class="form-row"><label>旧密码（修改任何账号信息都需要）</label>
      <input type="password" id="a-old" autocomplete="current-password"></div>
    <div class="form-row"><label>新密码（至少 6 位，留空表示不修改）</label>
      <input type="password" id="a-new" autocomplete="new-password"></div>
    <button class="btn accent" id="a-save">保存账号修改</button>

    <h3 style="margin-top:26px">访问</h3>
    <div class="setting-row">
      <label>访客模式（允许未登录访问首页，只读）</label>
      <input type="checkbox" id="a-visitor" ${cfg.settings.visitor_mode ? 'checked' : ''}>
    </div>

    <h3 style="margin-top:26px">配置备份</h3>
    <div style="display:flex;gap:10px">
      <button class="btn" id="a-export" style="flex:1">⬇ 导出配置</button>
      <button class="btn" id="a-import" style="flex:1">⬆ 导入配置</button>
    </div>
    <div class="form-row" style="margin-top:10px"><label>从 URL 下载导入</label>
      <input type="text" id="a-import-url" placeholder="https://example.com/navpage-config.json" value="${esc(bootImportURL)}"></div>
    <button class="btn" id="a-import-url-btn" style="width:100%">下载并导入</button>

    <h3 style="margin-top:26px">会话</h3>
    <button class="btn danger" id="a-logout">退出登录</button>`;

  $('#a-save').addEventListener('click', async () => {
    const user = $('#a-user').value.trim();
    const oldPw = $('#a-old').value;
    const newPw = $('#a-new').value;
    if (!user && !newPw) { toast('没有需要修改的内容', 'err'); return; }
    if (!oldPw) { toast('请输入旧密码', 'err'); return; }
    try {
      await api('PUT', '/api/account', {
        username: user || cfg.username,
        old_password: oldPw, new_password: newPw,
      });
      toast('账号信息已更新', 'ok');
      $('#a-old').value = ''; $('#a-new').value = ''; $('#a-user').value = '';
      cfg.username = user || cfg.username;
      renderMain();
    } catch (e) { toast(e.message, 'err'); }
  });
  $('#a-visitor').addEventListener('change', e => {
    cfg.settings.visitor_mode = e.target.checked;
    debounceSave();
    toast(e.target.checked ? '已开启访客模式' : '已关闭访客模式', 'ok');
  });

  // 导出配置（不含密码等敏感字段）
  $('#a-export').addEventListener('click', async () => {
    try {
      const data = await api('GET', '/api/config');
      const out = {
        exported_at: new Date().toISOString(),
        settings: data.settings,
        groups: data.groups,
        bookmark_groups: data.bookmark_groups,
      };
      const blob = new Blob([JSON.stringify(out, null, 2)], { type: 'application/json' });
      const a = document.createElement('a');
      a.href = URL.createObjectURL(blob);
      const d = new Date(), p = x => String(x).padStart(2, '0');
      a.download = `navpage-config-${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}.json`;
      a.click();
      URL.revokeObjectURL(a.href);
      toast('配置已导出', 'ok');
    } catch (e) { toast('导出失败：' + e.message, 'err'); }
  });

  // 导入配置（本地文件，覆盖分组/设置/书签）
  $('#a-import').addEventListener('click', () => {
    const inp = document.createElement('input');
    inp.type = 'file';
    inp.accept = '.json,application/json';
    inp.onchange = async () => {
      const f = inp.files[0];
      if (!f) return;
      let obj;
      try { obj = JSON.parse(await f.text()); } catch { toast('JSON 解析失败', 'err'); return; }
      if (!Array.isArray(obj.groups)) { toast('文件格式不对：缺少 groups', 'err'); return; }
      confirmDialog('导入将覆盖当前所有分组、设置和书签，确定继续？', async () => {
        try {
          await api('PUT', '/api/config', {
            settings: obj.settings || cfg.settings,
            groups: obj.groups,
            bookmark_groups: Array.isArray(obj.bookmark_groups) ? obj.bookmark_groups : (cfg.bookmark_groups || []),
          });
          toast('导入成功，正在刷新…', 'ok');
          setTimeout(() => location.reload(), 600);
        } catch (e) { toast('导入失败：' + e.message, 'err'); }
      }, '确定');
    };
    inp.click();
  });

  // 从 URL 下载并导入（由服务端代理下载，避免跨域）
  $('#a-import-url-btn').addEventListener('click', () => {
    const u = $('#a-import-url').value.trim();
    if (!u) { toast('请输入下载地址', 'err'); return; }
    confirmDialog('将从该 URL 下载配置并覆盖当前所有分组、设置和书签，确定继续？', async () => {
      try {
        await api('POST', '/api/import-from-url', { url: u });
        toast('导入成功，正在刷新…', 'ok');
        setTimeout(() => location.reload(), 600);
      } catch (e) { toast('导入失败：' + e.message, 'err'); }
    }, '确定');
  });

  $('#a-logout').addEventListener('click', async () => {
    try { await api('POST', '/api/logout'); } catch {}
    location.reload();
  });
}

/* ============ 登录 ============ */
// 未登录：拉公开外观接口，让登录页也显示壁纸/主题色（失败则保持 CSS 兜底背景）
async function applyLoginBg() {
  try {
    const st = await api('GET', '/api/appearance');
    cfg = { settings: st };
    applyAppearance();
  } catch {}
}

function showLogin() {
  $('#login-view').classList.remove('hidden');
  $('#main-view').classList.add('hidden');
  // 登录页只保留登录入口，隐藏设置与内外网切换
  $('#login-link').classList.remove('hidden');
  $('#settings-btn').classList.add('hidden');
  $('#net-toggle').classList.add('hidden');
  $('#login-user').focus();
}

async function tryLogin() {
  const btn = $('#login-btn');
  btn.disabled = true;
  $('#login-err').classList.add('hidden');
  try {
    await api('POST', '/api/login', {
      username: $('#login-user').value.trim(),
      password: $('#login-pass').value,
    });
    authed = true;
    const [c, list] = await Promise.all([api('GET', '/api/config'), loadWpAssets()]);
    cfg = c;
    wpAssets = list;
    markSaved();   // 登录后重新记录已保存状态
    $('#login-view').classList.add('hidden');
    $('#main-view').classList.remove('hidden');
    updateHeaderButtons();
    renderMain();
  } catch (e) {
    const err = $('#login-err');
    err.textContent = e.message;
    err.classList.remove('hidden');
  } finally {
    btn.disabled = false;
  }
}

/* ============ 初始化 ============ */
// 拉取内嵌 assets 里的预设壁纸列表（default-bg*，服务端枚举）
async function loadWpAssets() {
  try {
    const d = await api('GET', '/api/wallpapers');
    return Array.isArray(d.items) && d.items.length ? d.items : ['/assets/default-bg'];
  } catch {
    return ['/assets/default-bg'];
  }
}

async function init() {
  applyDark();
  tick();

  // 上传
  $('#file-input').addEventListener('change', async e => {
    const f = e.target.files[0];
    if (!f) return;
    uploadFile(f, url => uploadCb && uploadCb(url));
  });

  // 一次请求同时拿配置与登录态（服务端 pub.authed），免去 session 探测的串行往返，主页更快显示
  try {
    // 配置、预设壁纸列表、启动参数（-url 预填下载框）并行请求，避免多一次串行往返拖慢首屏
    const [c, list, boot] = await Promise.all([
      api('GET', '/api/config'),
      loadWpAssets(),
      api('GET', '/api/bootstrap').catch(() => ({})),
    ]);
    cfg = c;
    authed = !!cfg.authed;
    wpAssets = list;
    bootImportURL = boot.import_url || '';
    markSaved();   // 记录初始已保存状态
  } catch {
    await applyLoginBg();
    showLogin();
    return;
  }
  $('#main-view').classList.remove('hidden');
  updateHeaderButtons();
  renderMain();
}

// 未登录的访客：显示登录入口，隐藏设置
function updateHeaderButtons() {
  $('#login-link').classList.toggle('hidden', authed);
  $('#settings-btn').classList.toggle('hidden', !authed);
}

/* ============ 事件绑定 ============ */
document.addEventListener('DOMContentLoaded', () => {
  init();

  $('#login-form').addEventListener('submit', e => { e.preventDefault(); tryLogin(); });

  $('#search-btn').addEventListener('click', doSearch);
  $('#search-input').addEventListener('keydown', e => { if (e.key === 'Enter') doSearch(); });
  // 清空按钮：输入内容时出现，点击清空并回焦
  const si = $('#search-input'), sc = $('#search-clear');
  si.addEventListener('input', () => { sc.hidden = si.value.length === 0; });
  sc.addEventListener('click', () => { si.value = ''; sc.hidden = true; si.focus(); });

  $('#settings-btn').addEventListener('click', openDrawer);
  // 打开方式下拉：点击外部关闭所有弹层
  document.addEventListener('click', e => {
    if (!e.target.closest('.np-select')) $$('.np-select-pop').forEach(p => p.classList.add('hidden'));
  });
  // 描述过长被截断时：悬停显示半透明气泡展示完整内容（自绘，仅实际溢出时出现）
  let descTip = null;
  const closeDescTip = () => { descTip?.remove(); descTip = null; };
  document.addEventListener('mouseover', e => {
    const d = e.target.closest('.card-desc');
    if (!d || d.scrollWidth <= d.clientWidth) { closeDescTip(); return; }
    if (descTip && descTip._src === d) return;
    closeDescTip();
    descTip = document.createElement('div');
    descTip.className = 'desc-tip';
    descTip._src = d;
    descTip.textContent = d.textContent;
    document.body.appendChild(descTip);
    const r = d.getBoundingClientRect(), tr = descTip.getBoundingClientRect();
    let x = Math.min(Math.max(8, r.left), window.innerWidth - tr.width - 8);
    let y = r.bottom + 8;   // 默认显示在描述下方，底部放不下再挪到上方
    if (y + tr.height > window.innerHeight - 8) y = r.top - tr.height - 8;
    descTip.style.left = x + 'px';
    descTip.style.top = y + 'px';
  });
  // 卡片「当前页面弹窗打开」
  $('#groups-wrap').addEventListener('click', e => {
    const card = e.target.closest('a.card[data-modal="1"]');
    if (!card) return;
    e.preventDefault();
    openPageModal(card.href, card.querySelector('.card-name')?.textContent);
  });
  $('#net-toggle').addEventListener('click', () => {
    if ((cfg.settings.net_mode_switch || 'manual') === 'auto') return; // 自动模式下不可手动切换
    localStorage.setItem('np_net_mode', netMode() === 'lan' ? 'wan' : 'lan');
    renderNetToggle();
    renderGroups();
    toast(netMode() === 'lan' ? '已切换到内网模式' : '已切换到外网模式', 'ok');
  });
  $('#drawer-close').addEventListener('click', closeDrawer);
  $('#drawer-mask').addEventListener('click', closeDrawer);
  $('#login-link').addEventListener('click', () => {
    $('#main-view').classList.add('hidden');
    showLogin();
  });

  $$('.tab-btn').forEach(b => b.addEventListener('click', () => { curTab = b.dataset.tab; renderTab(); }));

  $('#modal-mask').addEventListener('click', e => { if (e.target.id === 'modal-mask') closeModal(); });
  document.addEventListener('click', e => { if (e.target.closest('[data-close]')) closeModal(); });
  document.addEventListener('keydown', e => {
    if (e.key === 'Escape') { closeModal(); closeDrawer(); closeContextMenu(); }
  });
  document.addEventListener('click', e => {
    if (ctxEl && !ctxEl.contains(e.target)) closeContextMenu();
  });
  window.addEventListener('scroll', () => { closeContextMenu(); closeHexaPicker(); closeDescTip(); }, true);
  window.addEventListener('resize', () => { closeContextMenu(); closeHexaPicker(); closeDescTip(); });
  window.addEventListener('blur', closeContextMenu);

  // 兜底：有改动未落盘时，页面刷新/关闭标签页也尽量保存（keepalive 保证页面卸载后请求仍会发出）
  window.addEventListener('beforeunload', () => {
    if (!saveDirty) return;
    const body = snapOf();
    if (body === savedSnap) return;
    fetch('/api/config', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body,
      keepalive: true,
    });
  });
});
