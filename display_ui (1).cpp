#include "display_ui.h"
#include "../player_state.h"   // 联网数据(时钟/农历/天气)只读 gState 已取好的结果, 绘图路径绝不发网络请求
#include <Preferences.h>
#include <SPI.h>    // ST7789 硬件 SPI 驱动(独立 SPI 总线, 与 ST7567 软件 SPI / OLED 软件 I2C 互不干扰)
#include <WiFi.h>   // 第4行显示 WiFi 信号强度(纯读变量, 开销可忽略)
#include <Wire.h>   // ★SSD1306 I2C 地址探测用(临时挂 15/16 扫一遍, 之后由 show() 正常使用 Wire1)
#include <LittleFS.h>  // 屏幕背景图(Web 后台上传的 1bpp 位图)存在这里
#include "../data/t2s_tab.h"   // 繁体 -> 简体 单字表(字库是 GB2312 简体, 画不出繁体 -> 见 t2sFix)

// 中文渲染: 用完整 GB2312 字体 u8g2_font_unifont_t_gb2312(已核验覆盖全部常用汉字, 含"猫")。
// 逐字渲染, 遇 GB2312 之外的字画空心方框占位, 不吞字。
static const uint8_t* const gCnFonts[] = {
  u8g2_font_unifont_t_gb2312,
};
static const int gCnFontN = (int)(sizeof(gCnFonts) / sizeof(gCnFonts[0]));

// 缺字缓存: codepoint -> 字体下标(-1=所有字体都缺)。避免每帧重复探测。
struct GlyphCache { uint16_t cp; int8_t fi; };
static GlyphCache gCache[64];
static uint8_t   gCacheN = 0;
static uint8_t   gCacheW = 0;

// 把 Unicode 码点编码成 UTF-8 字节串(用于 getUTF8Width 测宽)
static int utf8enc(uint16_t cp, char* buf) {
  if (cp < 0x80)        { buf[0] = (char)cp; return 1; }
  if (cp < 0x800)       { buf[0] = (char)(0xC0 | (cp >> 6));
                          buf[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
  buf[0] = (char)(0xE0 | (cp >> 12));
  buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
  buf[2] = (char)(0x80 | (cp & 0x3F));
  return 3;
}

// 为某个码点挑选含它的字体下标(结果缓存)
static int pickCnFont(U8G2& g, uint16_t cp) {
  for (uint8_t i = 0; i < gCacheN; i++)
    if (gCache[i].cp == cp) return gCache[i].fi;
  int found = -1;
  for (int i = 0; i < gCnFontN; i++) {
    g.setFont(gCnFonts[i]);
    char buf[4]; int n = utf8enc(cp, buf); buf[n] = 0;
    if (n > 0 && g.getUTF8Width(buf) > 0) { found = i; break; }
  }
  if (gCacheN < (uint8_t)64) { gCache[gCacheN].cp = cp; gCache[gCacheN].fi = (int8_t)found; gCacheN++; }
  else                       { gCache[gCacheW].cp = cp; gCache[gCacheW].fi = (int8_t)found; gCacheW = (gCacheW + 1) % 64; }
  return found;
}

// ★繁 -> 简 兜底(2026-10-07)
// ----------------------------------------------------------------------------
// 现象(用户报): 电台名"怀旧音乐台"里的"旧"正常, 天气卡片里地名"旧金山"的"旧"不正常 ——
//   同一个意思的字, 一处好好的、一处是空心方框, 看着"好奇怪"。
// 根因: 两处根本不是同一个字。
//   · 台名来自 stations.json / CSV(GBK 来源), 走 icyToUtf8 转出来是【简体】旧 = U+65E7;
//   · 地名来自 ipwho.is 的 lang=zh-CN, 实测它返回的是【繁体】——
//       San Francisco -> 舊金山(U+820A)、San Jose -> 聖荷西(U+8056)、昆士兰 -> 昆士蘭州。
//   而屏幕字库 u8g2_font_unifont_t_gb2312 只覆盖 GB2312 = 简体 6763 字, 繁体字查不到
//   -> pickCnFont 返回 -1 -> drawGlyphAt 画【空心方框】。所以"方框"不是编码坏了, 是字库没这个字形。
// 对策: 渲染每个字之前, 先用 t2s_tab.h 的表把繁体换成简体写法再查字库。
//   ★为什么放在【渲染层】而不是转换层(icyToUtf8):
//     ① 这里是"字库到底画不画得出"的唯一收口 —— 不管数据从哪来(地名/曲名/台名/SD 文件名、
//        甚至以后新加的数据源), 只要字形缺失就自动兜底, 不会漏;
//     ② 表里只收"繁体在 GB2312 里没有、简体有"的字, 所以正常简体的字【查不到】-> 原样返回,
//        对现有内容零影响, 也不会去改用户存进去的原始文字(台名/文件名保持原样);
//     ③ 若放在 icyToUtf8 里, 台名会被就地改写成简体并落盘, 相当于替用户改了他自己的数据。
//   ★代价: 多一次二分查找(2505 项 => 约 12 次比较), 只在 cp >= 0x80 时做, 相对字体查表可忽略。
static uint16_t t2sFix(uint16_t cp) {
  int lo = 0, hi = T2S_N - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) >> 1;
    const uint16_t t = gT2S[mid * 2];
    if      (t < cp) lo = mid + 1;
    else if (t > cp) hi = mid - 1;
    else             return gT2S[mid * 2 + 1];
  }
  return cp;
}

// 单个字符的绘制宽度(不依赖具体字体, 仅供排版推进 x 用)
// ★2026-09-19 修正: 原判据是 `cp < 0x3400` 就走"取低字节当 ASCII"的捷径 —— 这是错的。
//   0x80~0x33FF 里全是【非 ASCII 但 < 0x3400】的码点, 取低字节后成了非法/错误字节:
//     · 中文标点与常用符号: 。(0x3002) 、(0x3001) 《(0x300A) …(0x2026)  —— 站点名/曲名里极常见
//     · 全角形式: ，！？：(0xFF00~0xFFEF)
//     · 拉丁重音(0xC0~0xFF, 由 icyToUtf8 从 Latin-1 转来)、希腊、西里尔(0x391/0x410 等)
//   后果: 这些字符整字消失(或宽度算成 0 导致后面的字错位)。判据改为 0x80 之后, 它们统一走
//   "UTF-8 编码 + pickCnFont 选字体"这条路 —— 含则正常显示, 不含则画占位框, 不再静默吞字。
static int glyphWidth(U8G2& g, uint16_t cp) {
  if (cp >= 0x80) cp = t2sFix(cp);       // ★繁体不成字形 -> 先换简体(见 t2sFix 注释)
  char buf[4];
  const int n = utf8enc(cp, buf); buf[n] = 0;
  if (cp < 0x80) {                       // ASCII / 数字 / 半角符号 -> 8x13(库无 8x16, 取最接近的 8 宽字体)
    g.setFont(u8g2_font_8x13_tr);
    return g.getUTF8Width(buf);
  }
  int fi = pickCnFont(g, cp);
  if (fi >= 0) {
    g.setFont(gCnFonts[fi]);
    return g.getUTF8Width(buf);
  }
  return 14;                            // 缺字占位框宽度
}

// 画单个字符: ASCII 用 6x10; 其余(中文标点/全角/重音拉丁/希腊/西里尔/CJK)逐字选字体; 全缺则画占位框
static void drawGlyphAt(U8G2& g, int x, int y, uint16_t cp) {
  if (cp >= 0x80) cp = t2sFix(cp);       // ★与 glyphWidth 同一步转换, 保证"测宽"和"绘制"永远同一个字形
  char buf[4];
  const int n = utf8enc(cp, buf); buf[n] = 0;
  if (cp < 0x80) {
    g.setFont(u8g2_font_8x13_tr);
    g.drawUTF8(x, y, buf);
    return;
  }
  int fi = pickCnFont(g, cp);
  if (fi >= 0) {
    g.setFont(gCnFonts[fi]);
    g.drawUTF8(x, y, buf);              // 与 glyphWidth 用同一份 UTF-8 编码, 测宽/绘制必然一致
    return;
  }
  // 真缺字: 空心方框占位, 避免被悄悄吞掉看不出
  g.setFont(u8g2_font_6x10_tr);
  g.drawFrame(x, y - 12, 12, 12);
}

// 逐字绘制 UTF-8 字符串, 返回绘制结束的 x
static int drawCN(U8G2& g, int x, int y, const char* s) {
  int cx = x;
  const uint8_t* p = (const uint8_t*)s;
  while (*p) {
    uint16_t cp;
    if (*p < 0x80)            { cp = *p;                         drawGlyphAt(g, cx, y, cp); cx += glyphWidth(g, cp); p += 1; }
    else if ((*p & 0xE0) == 0xC0) { cp = (((*p & 0x1F) << 6) | (p[1] & 0x3F));          drawGlyphAt(g, cx, y, cp); cx += glyphWidth(g, cp); p += 2; }
    else if ((*p & 0xF0) == 0xE0) { cp = (((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F)); drawGlyphAt(g, cx, y, cp); cx += glyphWidth(g, cp); p += 3; }
    else { p += 1; continue; }     // 非法 UTF-8 续字节, 跳过
  }
  return cx;
}

// 计算逐字渲染后的总宽度(用于跑马灯判断)
static int cnWidth(U8G2& g, const char* s) {
  int w = 0;
  const uint8_t* p = (const uint8_t*)s;
  while (*p) {
    uint16_t cp;
    if (*p < 0x80)            { cp = *p;                         w += glyphWidth(g, cp); p += 1; }
    else if ((*p & 0xE0) == 0xC0) { cp = (((*p & 0x1F) << 6) | (p[1] & 0x3F));          w += glyphWidth(g, cp); p += 2; }
    else if ((*p & 0xF0) == 0xE0) { cp = (((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F)); w += glyphWidth(g, cp); p += 3; }
    else { p += 1; continue; }
  }
  return w;
}

// ============================================================================
// 大屏时钟字体: 按【可用宽 + 可用高】自动挑最大的 logisoso 档位(2026-10-06)
// ----------------------------------------------------------------------------
// U8g2 的 logisoso 系列是等宽数字字体(0-9 与 ':'), 从 16px 一路到 92px 都有 ——
// 之前写死 42px 纯属保守。改成"有多宽用多大", 现场就不必再回来调字号:
//   · 横向先按 getUTF8Width 卡宽度(时钟不能压到右边天气卡片);
//   · 再按 getAscent-getDescent 卡高度(时钟不能压到下面的湿度行)。
// 返回挑中的字体(已 setFont); 一个都装不下时退回最小的 34px。
// ★注意: logisoso 是等宽数字字体, 只含数字和冒号, 所以用它打印的字符串
//   必须只有 [0-9:] 这些字符(时钟 "08:27" 正合适), 掺中文会整段不显示。
static const uint8_t* kStClockFonts[] = {
  u8g2_font_logisoso92_tn, u8g2_font_logisoso78_tn, u8g2_font_logisoso62_tn,
  u8g2_font_logisoso58_tn, u8g2_font_logisoso50_tn, u8g2_font_logisoso46_tn,
  u8g2_font_logisoso42_tn, u8g2_font_logisoso38_tn, u8g2_font_logisoso34_tn,
};
static const uint8_t* pickClockFont(U8G2& g, const char* s, int maxW, int maxH) {
  for (unsigned i = 0; i < sizeof(kStClockFonts) / sizeof(kStClockFonts[0]); i++) {
    g.setFont(kStClockFonts[i]);
    const int hgt = (int)g.getAscent() - (int)g.getDescent();   // getDescent() 是负值 ->
    if (g.getUTF8Width(s) <= maxW && hgt <= maxH) return kStClockFonts[i];
  }
  g.setFont(u8g2_font_logisoso34_tn);
  return u8g2_font_logisoso34_tn;
}

// 12 小时制时间串: 0 点 -> 12:xx, 13~23 点 -> 01~11:xx(保留前导 0, 这样 9:59 -> 10:00 宽度不变, 不会触发字号跳变)
static void fmtClock12(char* out, size_t n, int hour24, int minute) {
  int h = hour24 % 12;
  if (h == 0) h = 12;
  snprintf(out, n, "%02d:%02d", h, minute);
}

// 12 小时制大时钟 + 右下角小号 AM/PM: 先给 AM/PM 留出宽度, 再让时钟在剩余宽度里挑最大字号,
// 整组(时钟+AM/PM)一起居中; AM/PM 与数字同一条基线(底对齐)。
static void drawClock12(U8G2& g, const char* s, bool pm, int x0, int x1, int yTop, int yBot) {
  const char* ap = pm ? "PM" : "AM";
  g.setFont(u8g2_font_8x13_tr);
  const int apW = g.getUTF8Width(ap);
  const int gap = 3;
  const uint8_t* f = pickClockFont(g, s, (x1 - x0) - apW - gap, yBot - yTop);
  g.setFont(f);
  const int asc = (int)g.getAscent();
  const int dsc = (int)g.getDescent();               // 负
  const int baseY = (yTop + yBot) / 2 + (asc + dsc) / 2;
  const int w = g.getUTF8Width(s);
  const int left = x0 + (x1 - x0 - (w + gap + apW)) / 2;
  g.setCursor(left, baseY);
  g.print(s);
  g.setFont(u8g2_font_8x13_tr);
  g.drawUTF8(left + w + gap, baseY, ap);
}

// 以 (x 起, yTop..yBot 区间) 居中打印时钟 —— baseline 由字体真实的 ascent/descent 反推,
// 所以换任何字号都自动垂直居中, 不用再手调那个魔数 baseline。
static void drawClock(U8G2& g, const char* s, int x0, int x1, int yTop, int yBot) {
  const uint8_t* f = pickClockFont(g, s, x1 - x0, yBot - yTop);
  g.setFont(f);
  const int asc = (int)g.getAscent();
  const int dsc = (int)g.getDescent();               // 负
  const int baseY = (yTop + yBot) / 2 + (asc + dsc) / 2;
  const int w = g.getUTF8Width(s);
  g.setCursor(x0 + (x1 - x0 - w) / 2, baseY);
  g.print(s);
}


// ★列表行专用: 超长台名【直接截断】而不是跑马灯 —— 列表是"一眼扫"的界面, 滚动的字反而看不清;
//   顺带避免超长名字每帧白算几百个字形宽度(列表 7 行 × 每行上百字 = 每帧上千次字体查表, 会明显吃 CPU)。
static int drawCNLimit(U8G2& g, int x, int y, const char* s, int maxX) {
  int cx = x;
  const uint8_t* p = (const uint8_t*)s;
  while (*p && cx < maxX) {
    uint16_t cp;
    if (*p < 0x80)            { cp = *p;                         drawGlyphAt(g, cx, y, cp); cx += glyphWidth(g, cp); p += 1; }
    else if ((*p & 0xE0) == 0xC0) { cp = (((*p & 0x1F) << 6) | (p[1] & 0x3F));          drawGlyphAt(g, cx, y, cp); cx += glyphWidth(g, cp); p += 2; }
    else if ((*p & 0xF0) == 0xE0) { cp = (((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F)); drawGlyphAt(g, cx, y, cp); cx += glyphWidth(g, cp); p += 3; }
    else { p += 1; continue; }
  }
  return cx;
}

// ============================================================================
// 背光 PWM(LEDC) —— 亮度可调(2026-09-22)
// ----------------------------------------------------------------------------
// 两路背光脚: LCD_BL(ST7567) / LCD2_BL(ST7789), 都用 LEDC 输出 PWM, "亮度"= 占空比。
// ★为什么不能用 digitalWrite: 彩色 TFT(ST7789)没有亮度/对比度寄存器, 亮度只有背光占空比
//   这一个物理入口; 用 digitalWrite 就只能全亮/全灭, 后台"亮度"滑块会完全无效。
// ★★必须遵守的一条: 背光脚一旦 ledcAttach(), 就【绝不能再对该脚调用 pinMode()/digitalWrite()】——
//   那会把 LEDC 外设从引脚上摘掉(引脚回到普通 GPIO), 之后 ledcWrite 静默无效, 现象是
//   "改了亮度没反应 / 背光一直全亮"。所有背光输出必须走下面的 blPwm()。
// 极性: 高电平点亮的模块, 占空比 = 亮度; 低电平点亮的模块, 占空比取反(0% = 全亮输出)。
// ============================================================================
static bool gBlAttached = false;   // 两路背光脚是否已挂到 LEDC(幂等: 只挂一次)

static void blPwm(int pin, int pct, bool activeHigh) {
  if (pin < 0) return;
  if (pct < 0)   pct = 0;
  if (pct > 100) pct = 100;
  int d = (int)((uint32_t)pct * 255u / 100u);   // 0~100% -> 0~255
  if (!activeHigh) d = 255 - d;                 // 低电平点亮: 占空比取反
  ledcWrite(pin, (uint32_t)d);
}

// 把两路背光脚挂到 LEDC(只做一次)并全部置 0%(灭)。之后永久只用 blPwm 写这两根脚。
static void blPwmBegin(void) {
  if (gBlAttached) return;
  ledcAttach(LCD_BL,     LCD_BL_PWM_FREQ, LCD_BL_PWM_BITS);
  ledcAttach(LCD2_BL,    LCD_BL_PWM_FREQ, LCD_BL_PWM_BITS);
  ledcWrite(LCD_BL,  LCD_BL_ACTIVE_HIGH  ? 0 : 255);   // 0% 亮度(注意低电平点亮要写满占空比)
  ledcWrite(LCD2_BL, LCD2_BL_ACTIVE_HIGH ? 0 : 255);
  gBlAttached = true;
}

// ============================================================================
// 彩屏自定义 U8g2 设备(ST7789 / ILI9341 共用; 本机 U8g2 库无这两个驱动, 故走
//   "自定义设备 + 硬件 SPI")
// ----------------------------------------------------------------------------
// 思路: 用 u8g2_SetupBuffer() 注入自有 9600 字节 1bpp 全缓冲(=320*240/8),
//   ll_hvline_cb 用 vertical_top_lsb(与 U8g2 全缓冲单色设备一致)。
//   自定义 display_cb 的 DRAW_TILE 把「1bpp 比特图」逐像素转 16-bit RGB565,
//   经公共硬件 SPI(与 ST7567 共用总线, 与 OLED 软件 I2C 分离)推到屏上。
//   这样 100% 复用上层中文逐字渲染 / 4 行布局 / 整帧跳过代码, 仅在此做像素格式桥接。
// ★ST7789 与 ILI9341 的命令集(RESET/SLPOUT/DISPON/DISPOFF/INVON/COLMOD/MADCTL/CASET/PASET/RAMWR)
//   与玻璃尺寸(240x320)完全一致, 所以窗口/推屏/MADCTL/旋转/主题/背景图这条主链【一行都不用分叉】;
//   差别只在"上电初始化序列 / MADCTL 默认值 / 反显默认值", 由 gTftIli 分派(见 tft_hw_init)。
// 缓冲字节布局(已读 u8g2 源码确认): byte_index(x,y) = (y/8)*WIDTH + x,
//   WIDTH=320, 每 8px 高 band 占 320 字节, 共 30 个 band = 9600 字节。
// ============================================================================
// ★★横/竖屏运行期可切换(2026-10-06, 串口 V 命令; 存 NVS)。
//   为什么需要: ST7789 芯片的玻璃是【240 宽 × 320 高】的竖屏。"当 320x240 横屏用"要靠 MADCTL 的
//   MV 位交换行列 —— 于是同一个模组, 装成横的/竖的, 屏幕上看到的排版相差 90°。
//   与其猜用户的外壳怎么装, 不如两种都做, 现场按键切换并记忆。
//   ★两种方向的 1bpp 缓冲大小【恰好相等】(320*240/8 == 240*320/8 == 9600 B), 所以共用一块缓冲。
#define ST7789_W  320      // 横屏逻辑宽
#define ST7789_H  240      // 横屏逻辑高
#define ST7789_PW 240      // 竖屏逻辑宽(玻璃原生方向)
#define ST7789_PH 320      // 竖屏逻辑高
static bool gSt7789Portrait = (ST7789_ORIENT_DEFAULT != 0);   // false=横屏 320x240 / true=竖屏 240x320
// ★★2026-10-09: 本 TFT 驱动核心现在同时服务 ST7789 与 ILI9341(两者共用引脚/共用推屏路径)。
//   本标志区分当前驱动的是哪一颗芯片: false=ST7789 / true=ILI9341。
//   仅影响三处: ①上电初始化序列 ②MADCTL 默认值 ③反显默认值(见下); 其余窗口/推屏/旋转/主题完全共用。
//   由 DisplayUI::recreateU8g2() / setScreenType() / loadSettings() 按 mType 设置(切屏型时)。
static bool gTftIli = false;
// ★★2026-10-11: 再支持 240x240 方屏(1.3 寸) —— ST7789 的第二种玻璃。它【没有横竖之分】
//   (两个方向都是 240), 故不参与 gSt7789Portrait 那套"靠 MV 换行列"的机制, 单独用 gSt7789Square
//   标记; 方屏的缓冲只要 7200 B, 而 gSt7789Buf 已按 9600 B 开(取两种玻璃的较大者), 直接够用。
#define ST7789_SQ 240      // ★240x240 方屏(1.3寸)的逻辑宽高(两者相同)
// ★★2026-10-11: true = 当前这块 ST7789 是 240x240 方屏 —— 由 recreateU8g2() 按 mType 置位。
static bool gSt7789Square   = false;
static inline int stW() {
  if (gSt7789Square) return ST7789_SQ;                        // 240x240: 与方向无关
  return gSt7789Portrait ? ST7789_PW : ST7789_W;
}
static inline int stH() {
  if (gSt7789Square) return ST7789_SQ;
  return gSt7789Portrait ? ST7789_PH : ST7789_H;
}
// ★缓冲按【两种玻璃的较大者】开: 320x240 与 240x240 分别需要 9600 / 7200 字节, 取 9600。
static uint8_t   gSt7789Buf[ST7789_W * ST7789_H / 8];   // 9600 字节 1bpp 全缓冲(DRAM, CPU 频繁访问)
// ★双色调色板(2026-10-06 起「主题可选」): 本屏是 U8g2 1bpp 帧缓冲, 一帧【物理上只有两个颜色】:
//   bit=1 -> 亮色(FG), bit=0 -> 底色(BG)。所以“彩色”不是一项开关, 只能选“哪两个颜色”。
//   下面按 config.h 的 ST7789_THEME 选一套; 运行时还可用串口 Q 现场轮换(见 st7789ThemeCycle)。
//   ★ST7789 初始化里开了 INVON, 常见模组下 RGB565 值近似直接生效(想批量改色只改本表)。
// ★★fg/bg 必须是 16-bit RGB565, 不是 24-bit RGB888! (2026-10-06 修的坑)
//   旧版错写 0xE8F8FF 这种 8 位/通道的值, 赋给 uint16_t 会被【静默截断】成 0xF8FF ——
//   那是 R=31/G=7/B=31 的【品红】, 根本不是白色; 底色 0x0E3A6B 截成 0x3A6B = 灰蓝。
//   这就是"颜色不正常/发粉"的根因。换算: RGB565 = ((R>>3)<<11) | ((G>>2)<<5) | (B>>3)。
typedef struct { uint16_t fg; uint16_t bg; const char* name; } St7789Theme;
static const St7789Theme kSt7789Themes[] = {
  { (uint16_t)0xFFFF, (uint16_t)0x0000, "经典黑白(白字/黑底)" },        // 0  RGB(255,255,255) / RGB(0,0,0)
  { (uint16_t)0xEFDF, (uint16_t)0x09CD, "深蓝图片风(亮白青/深蓝底)" },  // 1  RGB(232,248,255) / RGB(14,58,107)
  { (uint16_t)0x7FF3, (uint16_t)0x00A1, "墨绿夜光(亮绿/近黑底)" },      // 2  RGB(124,255,158) / RGB(6,20,12)
};
static const int kSt7789ThemeCount = (int)(sizeof(kSt7789Themes) / sizeof(kSt7789Themes[0]));
static int      gSt7789ThemeIdx = (ST7789_THEME >= 0 && ST7789_THEME < (int)(sizeof(kSt7789Themes) / sizeof(kSt7789Themes[0]))) ? (int)ST7789_THEME : 0;
static uint16_t gSt7789Fg = kSt7789Themes[gSt7789ThemeIdx].fg;
static uint16_t gSt7789Bg = kSt7789Themes[gSt7789ThemeIdx].bg;

// ============================================================================
// 卡通配色(2026-10-16 由 FULL 整合工程 / FM 主线 v1.13 移植) —— 彩屏 bit=0 像素的底色
// ----------------------------------------------------------------------------
// ★为什么要它: 本屏是 U8g2 1bpp 帧缓冲, bit=0(背景)像素原来一律填【主题底色】(默认黑)。
//   于是"彩色屏"看着还是白字黑底的单色观感。改成: bit=0 填【卡通浅底色板】、bit=1 填
//   【各元素糖果色】(kCartoonElemPal) —— 整屏立刻是糖果/卡通配色, 而字体/排版一行不用改。
// ★语义(与 FULL 一致):
//   · bit=0(背景): 有背景图 -> 取图; 无图 -> 【gBgRgb 卡通色板】(不再是主题 gSt7789Bg);
//   · bit=1(界面): 元素配色 + 主题 fg 回落, 一帧不动;
//   · 主题表(Q 命令轮换)从此只决定 fg; bg 字段保留仅为兼容旧 NVS/串口打印。
#define BG_RGB(r,g,b)  (uint16_t)((((r)>>3)<<11)|(((g)>>2)<<5)|((b)>>3))
static const uint16_t kBgPalette[8] = {
  BG_RGB(150,200,235),  // 0 淡蓝天空(默认)
  BG_RGB(190,235,205),  // 1 薄荷绿
  BG_RGB(252,248,228),  // 2 奶油白
  BG_RGB(255,205,220),  // 3 樱花粉
  BG_RGB(218,210,245),  // 4 薰衣草紫
  BG_RGB(255,218,190),  // 5 蜜桃橙
  BG_RGB( 40, 52, 92),  // 6 深夜蓝(暗色卡通夜空)
  BG_RGB(255,255,255),  // 7 纯白(旧版观感)
};
static const int kBgPaletteN = 8;
// bit=0 像素(无背景图时)用的颜色 —— 就是卡通色板的那一项。DRAW_TILE 热路径只读它。
// ★v1.14: 去掉了 v1.13 试做的"内置程序生成壁纸"(实测观感更差), 显示逻辑回到与带 6686 的
//   FULL 完全一致 —— 无上传图就是一片干净柔和的卡通纯色, 而不是花哨色斑。
static uint16_t gBgRgb = kBgPalette[0];

// ★卡通默认元素色板: NVS 里 "ecol" 全为 0(从未改过颜色)时套用, 让新装/复位设备直接是卡通配色。
//   老用户若已手动设过颜色(非全零), 则保持原样, 不强制覆盖。
//   顺序必须对应 ELEM 枚举(见 display_ui.h 顶部), 与本工程 12 项完全一致:
//     TOPBAR, CLOCK, WEATHER, STATION, TITLE, STATUS, WIFI, VOLBAR, IP, BATT, LIST, FRAME
static const int8_t kCartoonElemPal[ELEM_COUNT] = {
  11,   // ELEM_TOPBAR  深蓝
  2,    // ELEM_CLOCK   红(大时钟, 在淡蓝底上最跳)
  3,    // ELEM_WEATHER 绿
  8,    // ELEM_STATION 橙
  7,    // ELEM_TITLE   洋红
  3,    // ELEM_STATUS  绿
  4,    // ELEM_WIFI    蓝
  13,   // ELEM_VOLBAR  紫
  6,    // ELEM_IP      青
  8,    // ELEM_BATT    橙
  4,    // ELEM_LIST    蓝
  13,   // ELEM_FRAME   紫(圆角框)
};
// 单个 8px 高 band 转 RGB565 后的组包缓冲: 320 列 × 8 行 × 2 字节 = 5120 字节。
// ★为什么不直接逐个 transfer16: 一整帧 320×240=76800 个像素, 逐个调用 transfer16 光是函数调用
//   开销就要几十毫秒(主循环被独占 -> 音频 I2S DMA 排空 -> 咔哒/断续)。先组包再一次 writeBytes,
//   整 band 只有 1 次调用, 由 SPI 硬件连续推, 速度快一个数量级。
static uint8_t   gSt7789Band[ST7789_W * 8 * 2];

// ============================================================================
// 元素配色(2026-10-10 由 DMradio-tyio / webradio-tyio-st7789 移植)
// ----------------------------------------------------------------------------
// 常规色板: 下标 0 = 白 = 默认(没改过颜色的用户看到的与旧固件一致, 零回归)。
// ★下标即 NVS 里存的值, 只能往后加, 不能重排; 且必须与 tools/web_page/color.js 的色板同序同数。
static const uint16_t kElemPalette[ELEM_PALETTE_N] = {
  (uint16_t)0xFFFF,  // 0  白     RGB(255,255,255)  ← 默认
  (uint16_t)0x0000,  // 1  黑     RGB(0,0,0)
  (uint16_t)0xF800,  // 2  红     RGB(255,0,0)
  (uint16_t)0x07E0,  // 3  绿     RGB(0,255,0)
  (uint16_t)0x001F,  // 4  蓝     RGB(0,0,255)
  (uint16_t)0xFFE0,  // 5  黄     RGB(255,255,0)
  (uint16_t)0x07FF,  // 6  青     RGB(0,255,255)
  (uint16_t)0xF81F,  // 7  洋红   RGB(255,0,255)
  (uint16_t)0xFD20,  // 8  橙     RGB(255,165,0)
  (uint16_t)0xBDF7,  // 9  浅灰   RGB(192,192,192)
  (uint16_t)0x7BEF,  // 10 深灰   RGB(128,128,128)
  (uint16_t)0x0010,  // 11 深蓝   RGB(0,0,128)
  (uint16_t)0x03E0,  // 12 深绿   RGB(0,128,0)
  (uint16_t)0x8010,  // 13 紫     RGB(128,0,128)
};
// 每个元素当前选中的色板下标 / 解析出的 RGB565。gElemRgb 由 elemSync() 从色板算出,
// 推屏(DRAW_TILE)只读它 —— 热路径里不再做任何查表与钳制。
static int      gElemPalS[ELEM_COUNT];
static uint16_t gElemRgb[ELEM_COUNT];
static void elemSync() {
  for (int i = 0; i < ELEM_COUNT; i++) {
    int p = gElemPalS[i];
    if (p < 0 || p >= ELEM_PALETTE_N) p = 0;
    gElemPalS[i] = p;
    gElemRgb[i]  = kElemPalette[p];
  }
}
// ---- 行 -> 元素 分区表(每帧由绘制代码登记, 推屏时按行查) ----
// 尺寸取两种方向里更大的高度(竖屏 320); 单色屏(ST7567/SSD1306)不走这套回调, 无影响。
static const int kZoneRows = 320;
static int8_t   gRowZoneA[kZoneRows];    // 每行主元素下标(-1 = 未登记, 回落旧版单一前景色)
static int8_t   gRowZoneB[kZoneRows];    // 每行"右半"元素下标(-1 = 该行不左右拆分)
static int16_t  gRowSplitX[kZoneRows];   // 左右拆分的分界 x(-1 = 不拆分)
// ★装饰线框(卡片边框 / 音量条外框 / 状态小框 / 分隔线)单独占 ELEM_FRAME 色, 不再被"所在行的
//   元素色"污染。机制: 竖框只钉【精确那一列】, 横框钉【那一行的 x0..x1 区间】——
//   所以绝不会误伤同行的其它元素。
// ★竖框必须支持【同一行多个线框】: 竖屏底部面板边框(x=6/234)与音量条外框(x=58/188)在
//   第 283~292 行重叠 —— 若每行只存一对左右列, 后登记的会把先登记的覆盖掉, 面板边框就"缺一截"。
static const int kFcMax = 4;
static int16_t  gRowFc [kZoneRows][kFcMax];  // 该行需要染成框色的列(追加式, -1=空)
static int16_t  gRowFx0[kZoneRows];      // 横框行: 此行 col∈[Fx0,Fx1] -> 框色, -1=无
static int16_t  gRowFx1[kZoneRows];
// ★【圆角】装饰框的角块: drawRFrame 的圆角是圆弧, 半径带内的像素既不在顶/底行(x0..x1)、
//   也不在左右精确那两列上 —— 旧机制兜不住, 屏幕上是"面板四个转角"露出一小截主题前景色(白)。
//   故再存【一对】第二横框区间, 只在半径带内把左右两个角块钉成框色。
static int16_t  gRowFx2[kZoneRows];      // 第二横框区间(圆角行: 右角块), -1=无
static int16_t  gRowFx3[kZoneRows];
// ★字形上下界(相对【基线】), 登记"一行文字"的元素色区时必须按它留足:
//   中文字体 u8g2_font_unifont_t_gb2312 的规格(max_char_height=16, y_offset=-2)
//   ⇒ 字形从【基线-14】探到【基线+2】。手拍行区间会把边界切进字形里 -> 同一行字两截两个色。
static const int kGlyphAsc  = 14;        // 基线上方(中文字形上探)
static const int kGlyphDesc = 2;         // 基线下方(中文字形下探)

// ============================================================================
// 彩色背景图(2026-10-06) —— 供 DRAW_TILE 逐像素取色用
// ----------------------------------------------------------------------------
// ★为什么这里能出【全彩色】: ST7789 本来就是 RGB565 面板, 是 U8g2 的 1bpp 缓冲把画面
//   限制成了两个颜色。而推屏时本来就要为每个像素算出一个 RGB565 值(见下面 DRAW_TILE),
//   所以只要把 "bit=0 -> 常量 gSt7789Bg" 换成 "bit=0 -> 查这张彩色图", 背景立刻全彩,
//   U8g2 的 1bpp 架构一行都不用改。
// 格式: RGB565 大端(高字节先发), 行优先, 像素(x,y) 在 offset (y*W + x)*2。
// 放在 PSRAM(153600B) 而不是 DRAM: 每帧 76800 次读取是顺序访问、命中友好, 实测多花的
// 2~4ms 相对推屏本身的 61ms(153600B @20MHz SPI) 可忽略; 而省下的 150KB DRAM 是实打实的。
// ============================================================================
static uint8_t* sBgBuf565  = nullptr;                // nullptr = 无背景图 -> 完全走原路径(两个主题色)
static uint8_t  sBgUiAlpha = BG_UI_ALPHA_DEFAULT;    // 界面不透明度 0~100(100=纯色不混合, 零开销)
static uint8_t  sBgBright  = BG_BRIGHT_DEFAULT;
// ★2026-10-11 前移到此: DRAW_TILE 要按它校验"已缓冲的这张图配不配当前屏"。
//   原来是 -1=无 / 0=横屏 / 1=竖屏, 定义在文件后半(bgLoad 那一节), C++ 文件作用域静态变量
//   不能先用后定义 —— 与 320x240 分支踩过的坑同源。
static int      sBgLoadedPort = -1;
// (v1.14: 原 v1.13 的"内置全彩壁纸"缓冲 sDefBg 已移除, 见下方说明。)
// 当前屏该用哪张背景图: 0 = 横屏槽(320x240), 1 = 竖屏槽(240x320)。
// ★240x240 方屏【借竖屏槽】: 它同样只有 240 列宽, 直接按 240 的行宽去索引这张图,
//   取到的就是顶部 240 行 —— 不额外占 flash/PSRAM, 网页端也不用手动传第三张。
static inline int bgSlotNow() { return (gSt7789Square || gSt7789Portrait) ? 1 : 0; }
// 已缓冲那张图的【每行像素数】: 横屏槽是 320, 竖屏槽是 240。用来校验"这张图配不配当前屏"。
static inline int bgLoadedStride() { return (sBgLoadedPort == 0) ? ST7789_W : ST7789_PW; }      // 背景图亮度 0~100(100=原图; <100 加载时压暗)

// 背景图按亮度缩放(只在 bgLoad 里对刚读进 PSRAM 的整张图调用一次, 不在推屏热路径)。
// ★每通道等比缩放, 不做整体 16-bit 乘 —— 否则 R/G/B 会串位(高通道溢出借位到低通道)。
//   b>=100 时直接返回(默认路径零开销); b==0 时全黑, 单独走 memset 更快。
static void bgApplyBright(uint8_t* buf, size_t bytes, uint8_t b) {
  if (!buf || b >= 100) return;
  if (b == 0) { memset(buf, 0, bytes); return; }
  const uint32_t k = b;                                // 0~99
  const size_t n = bytes / 2;                          // 像素数(大端, 2 字节/像素)
  for (size_t i = 0; i < n; i++) {
    const uint8_t* q = buf + i * 2;
    const uint16_t c = (uint16_t)((q[0] << 8) | q[1]);
    const uint32_t r = (((c >> 11) & 0x1F) * k) / 100;
    const uint32_t g = (((c >>  5) & 0x3F) * k) / 100;
    const uint32_t bl= (( c        & 0x1F) * k) / 100;
    const uint16_t o = (uint16_t)((r << 11) | (g << 5) | bl);
    uint8_t* w = buf + i * 2;
    w[0] = (uint8_t)(o >> 8); w[1] = (uint8_t)(o & 0xFF);
  }
}

// ============================================================================
// (v1.14 已移除"内置程序生成壁纸")
// ----------------------------------------------------------------------------
// v1.13 曾试做一张程序生成的多彩壁纸(色斑 / 渐变)在无上传图时兜底。实机观感反而更差
// (用户反馈"色彩斑块更多"), 且与带 6686 的 FULL 工程显示不一致。故 v1.14 全部撤掉:
// 无上传背景图时, bit=0 就是【gBgRgb 卡通纯色】—— 与 FULL 完全同一套渲染路径。
// ============================================================================

// RGB565 按百分比混合: a>=100 -> 全用 fg; a==0 -> 全用 bg。
// ★这是真正的线性混合(三个分通道各自算), 不是棋盘格抖动 —— 所以"界面半透明"是平滑的。
//   只在 sBgUiAlpha < 100 且真的有背景图时才会被调用; 默认路径一次都不进。
static inline uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t a) {
  if (a >= 100) return fg;
  if (a == 0)   return bg;
  const uint32_t na = (uint32_t)(100 - a);
  const uint32_t r = ((((uint32_t)(fg >> 11) & 0x1F) * a) + (((uint32_t)(bg >> 11) & 0x1F) * na)) / 100;
  const uint32_t g = ((((uint32_t)(fg >>  5) & 0x3F) * a) + (((uint32_t)(bg >>  5) & 0x3F) * na)) / 100;
  const uint32_t b = ((((uint32_t)(fg      ) & 0x1F) * a) + (((uint32_t)(bg      ) & 0x1F) * na)) / 100;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// ============================================================================
// 公共硬件 SPI 总线 —— ST7567 与 ST7789 共用一个 SPIClass 实例(2026-09-22)
// ----------------------------------------------------------------------------
// 两种屏都改/保持用硬件 SPI, 共用同一对外设 + 同一组脚(40/41), 靠"同一时刻只有一种屏工作"
// 天然互斥, 切换时只需换时钟频率:
//   · 引脚显式 begin(40,-1,41,-1) 指定, 不依赖全局 SPI 对象 / variant 默认脚;
//   · 切到【不用硬件 SPI】的屏之前应调用 hwspiRelease(), 把两根脚交还普通 GPIO。
//     ★2026-09-23: 现在唯一这种屏是 SSD1306(走独立 15/16 的硬件 I2C1, 已不占 40/41),
//     严格说它不与公共总线抢线, 该调用保留为"切到非 SPI 屏就归还总线"的对称动作。
// ★为什么不用 U8g2 自带的 _4W_HW_SPI 类: 它内部走全局 SPI(ESP32-S3 上 = FSPI/SPI2 外设),
//   且 INIT 时只传 (sck, MISO, mosi) —— MISO 取自 variant 默认脚(S3 上是 13, 正是我们的
//   LCD_CS!), 会把这根不相干的脚也路由进 SPI 外设。故 ST7567 的字节层改为自写
//   (见下面 st7567_hwspi_byte_cb), 引脚/频率/CS/DC 时序全部可控。
// ============================================================================
static SPIClass  gHwSpi;                  // 公共硬件 SPI(默认构造 = HSPI, 在 S3 上是 SPI3 外设)
static bool      gHwSpiReady = false;     // 是否已 begin(即是否"持有"SCK/MOSI 两根脚)
static uint32_t  gHwSpiHz    = 0;         // 当前已设时钟(相同则跳过 setFrequency, 省每帧开销)

// 取用公共总线并把时钟切到 hz(ST7567 与 ST7789 用不同档位)
static void hwspiAcquire(uint32_t hz) {
  if (!gHwSpiReady) {
    gHwSpi.begin(LCD_BUS_SCK, -1, LCD_BUS_MOSI, -1);   // MISO=-1: 两种屏都只写不读
    gHwSpi.setDataMode(SPI_MODE0);                     // 两种屏都是 mode0(上升沿采样, MSB 先)
    gHwSpiReady = true;
    gHwSpiHz    = 0;                                   // begin() 后频率回到默认, 下面强制设一次
  }
  if (gHwSpiHz != hz) { gHwSpi.setFrequency(hz); gHwSpiHz = hz; }
}

// 交还公共总线(切到不用硬件 SPI 的屏之前调用; 现为 SSD1306 的独立 I2C)
static void hwspiRelease(void) {
  if (!gHwSpiReady) return;
  gHwSpi.end();                        // detach SPI 外设并释放其 DMA 通道
  pinMode(LCD_BUS_SCK,  INPUT);        // 交还后置高阻, 免得留个输出一直驱动着别人的线
  pinMode(LCD_BUS_MOSI, INPUT);
  gHwSpiReady = false;
  gHwSpiHz    = 0;
}

// ---- ST7567 的 U8g2 字节层: 用公共硬件 SPI 推字节, CS/DC 自己控(不走 gpio 回调) ----
// U8g2 的 CAD→BYTE 消息在 u8x8.h 里是【宏别名】, 别被数值吓到:
//   BYTE_INIT == CAD_INIT == 20;  BYTE_SEND == CAD_SEND_DATA == 23;
//   BYTE_START/END_TRANSFER == CAD_START/END_TRANSFER == 24/25。故只需处理这 5 条。
// 复位脚(RST)不在这里动: U8g2 的 u8x8_d_helper_display_init() 会在 BYTE_INIT 之后
//   走 gpio 回调发一个标准的复位脉冲(1→0→1), 由保留的 u8x8_gpio_and_delay_arduino 负责。
extern "C" uint8_t st7567_hwspi_byte_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
  switch (msg) {
    case U8X8_MSG_BYTE_INIT:
      hwspiAcquire(LCD_BUS_SPI_HZ_ST7567);                     // 总线就绪(引脚由我们指定)
      pinMode(LCD_CS, OUTPUT); digitalWrite(LCD_CS, HIGH);     // 片选: 高 = 不选中
      pinMode(LCD_DC, OUTPUT); digitalWrite(LCD_DC, HIGH);
      return 1;
    case U8X8_MSG_BYTE_SET_DC:          digitalWrite(LCD_DC, arg_int ? HIGH : LOW); return 1;
    case U8X8_MSG_BYTE_START_TRANSFER:  digitalWrite(LCD_CS, LOW);  return 1;   // 低 = 选中
    case U8X8_MSG_BYTE_END_TRANSFER:    digitalWrite(LCD_CS, HIGH); return 1;
    case U8X8_MSG_BYTE_SEND:            gHwSpi.writeBytes((uint8_t *)arg_ptr, arg_int); return 1;
    default:                            return 0;
  }
}

// 三套设备描述: 横屏 320x240 / 竖屏 240x320 / 方屏 240x240。
// ★第 14~19 项(tile_width/height + pixel_width/height)必须与【运行时方向】一致 ——
//   U8g2 靠 pixel_width/height 算 getWidth()/getHeight(), 布局分支与 tile 寻址全看它。
static u8x8_display_info_t gSt7789InfoLand = {
  0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // 时序/模式字段(本自定义设备自行驱动 SPI, 多数无关)
  ST7789_W / 8, ST7789_H / 8, 0, 0,        // tile_width=40(320/8), tile_height=30(240/8), 无偏移
  ST7789_W, ST7789_H                         // pixel_width=320, pixel_height=240
};
static u8x8_display_info_t gSt7789InfoPort = {
  0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  ST7789_PW / 8, ST7789_PH / 8, 0, 0,      // tile_width=30(240/8), tile_height=40(320/8)
  ST7789_PW, ST7789_PH                       // pixel_width=240, pixel_height=320
};
// ★2026-10-11 新增: 240x240 方屏。tile 30x30 = 240/8, 缓冲用 7200 B(在 9600 B 的 buf 之内)。
static u8x8_display_info_t gSt7789InfoSq = {
  0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  ST7789_SQ / 8, ST7789_SQ / 8, 0, 0,      // tile_width=30, tile_height=30
  ST7789_SQ, ST7789_SQ                       // pixel_width=240, pixel_height=240
};
static inline u8x8_display_info_t* stInfo() {
  if (gSt7789Square) return &gSt7789InfoSq;
  return gSt7789Portrait ? &gSt7789InfoPort : &gSt7789InfoLand;
}

// ---- ST7789 硬件 SPI 底层(CS/DC/RST/BL 全部手写, 不走 U8g2 的 CAD 协议) ----
// ★CS 按"一次完整事务"控制: 命令字节 + 它的全部参数在 CS 保持低电平时连续发出, 结束才拉高。
//   中途不乱动 CS —— 部分 ST7789 模组在"命令+参数"之间遇到 CS 上升沿会丢掉参数(表现为初始化不生效/花屏)。
static inline void st7789_dc(uint8_t v) { digitalWrite(LCD2_DC, v); }
static inline void st7789_cs(uint8_t v) { digitalWrite(LCD2_CS, v); }

// 发一条命令 + n 个参数(n 可为 0)
static void st7789_tx(uint8_t cmd, const uint8_t* p, size_t n) {
  st7789_dc(LOW);  st7789_cs(LOW);
  gHwSpi.transfer(cmd);
  if (n) { st7789_dc(HIGH); gHwSpi.writeBytes(p, n); }
  st7789_cs(HIGH);
}
// 开像素写入窗口(RAMWR), 之后连续推 RGB565 —— CS 保持低, 由调用方用 st7789_end() 收尾
static void st7789_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  const uint8_t xs[4] = { (uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1 };
  const uint8_t ys[4] = { (uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1 };
  st7789_tx(0x2A, xs, 4);                    // CASET 列地址
  st7789_tx(0x2B, ys, 4);                    // PASET 行地址
  st7789_dc(LOW);  st7789_cs(LOW);
  gHwSpi.transfer(0x2C);                     // RAMWR 进 RAM 写
  st7789_dc(HIGH);                           // 其后全是像素数据, CS 继续拉低
}
static inline void st7789_end(void) { st7789_cs(HIGH); }

// 用当前底色铺满整屏(初始化 / 切 MADCTL / 切主题三处复用)。
// ★所有与方向相关的尺寸都走 stW()/stH(): 横屏 320x240、竖屏 240x320 共用这一份代码。
static void st7789_fillBg(void) {
  const int W = stW();
  st7789_window(0, 0, W - 1, stH() - 1);
  for (int i = 0; i < W * 8; i++) {                 // 组一个 band(8 行)的底色 RGB565
    // ★2026-10-16(卡通配色): 底色取【gBgRgb 卡通色板】而不是主题 gSt7789Bg ——
    //   与 DRAW_TILE 的 bit=0 落点保持同一个来源, 否则初始化瞬间会闪一下主题底色。
    gSt7789Band[i * 2]     = (uint8_t)(gBgRgb >> 8);
    gSt7789Band[i * 2 + 1] = (uint8_t)(gBgRgb & 0xFF);
  }
  for (int k = 0; k < stH() / 8; k++)               // 逐 band 推完整个高度
    gHwSpi.writeBytes(gSt7789Band, (size_t)W * 16);
  st7789_end();
}

// ★当前生效的 MADCTL(0x36) 值。【横屏/竖屏各存一份】—— 两者的正确值天然不同:
//     横屏(当 320x240 用)必须 MV=1 交换行列, 否则列窗口只能到 239 -> 花屏;
//     竖屏(玻璃原生 240x320)则必须 MV=0。
//   可由串口 K 命令现场轮换, 用于不重烧固件就试出本模组正确的"方向 + RGB/BGR 顺序"。
//   ★结果存 NVS, 重启后仍然生效(旧版只改内存, 一重启就回退, 现场很难受)。
static uint8_t gSt7789MadctlLand = ST7789_MADCTL_LAND;
static uint8_t gSt7789MadctlPort = ST7789_MADCTL_PORT;
// ★2026-10-11: 240x240 方屏单独一份(它的正确值天然不同 —— 方屏不靠 MV 换方向, 见 config.h)。
static uint8_t gSt7789MadctlSq   = ST7789_MADCTL_SQ;
// ★ILI9341 的 MADCTL 单独存一份: 它的正确值天然不同(多数模组 BGR 位必须置 1, 见 config.h)。
//   与 ST7789 分开保存 + 分开 NVS 键(madL/madP vs imadL/imadP), 这样两块屏来回切都各自记住。
static uint8_t gIliMadctlLand   = ILI9341_MADCTL_LAND;
static uint8_t gIliMadctlPort   = ILI9341_MADCTL_PORT;
// 基准值(串口 K 现场轮换的就是它; 横/竖/方屏各一份; 按当前芯片分套)
static inline uint8_t& stMadctl() {
  if (gTftIli) return gSt7789Portrait ? gIliMadctlPort : gIliMadctlLand;
  if (gSt7789Square) return gSt7789MadctlSq;
  return gSt7789Portrait ? gSt7789MadctlPort : gSt7789MadctlLand;
}
// ★实际下发给屏的值 = 基准 ^ (后台"旋转"带来的 180° 翻转)。
//   2026-10-06 加: 后台旋转 180°/270° 就是靠这个实现的 —— MADCTL 的 MX(bit6)/MY(bit7) 同时取反
//   等于把画面上下颠倒, 是【纯硬件翻转、零性能代价】, 而且完全不动 U8g2 的几何(不会分页错乱)。
static bool sStMadFlip180 = false;
static inline uint8_t stMadctlEff() {
  const uint8_t v = stMadctl();
  return sStMadFlip180 ? (uint8_t)(v ^ 0xC0) : v;
}
// ★当前反显状态(0x21 INVON / 0x20 INVOFF), 默认取 config.h 里对应芯片的宏。
//   模组有"常黑/常白"两种面板, 需要的反显相反; 若画面【整片发白、像底片】就是它设反了。
//   ★ST7789 与 ILI9341 各存一份: 前者默认 1(INVON)、后者默认 0(标准 ILI9341 不需要)。
static bool    gSt7789InvOn  = (ST7789_INVON != 0);
static bool    gIliInvOn     = (ILI9341_INVON != 0);
// 取"当前芯片"的反显状态引用(切屏型读写都走它, 保证两块屏各自独立记忆)
static inline bool& tftInv() { return gTftIli ? gIliInvOn : gSt7789InvOn; }
static void st7789SavePrefs();   // 前置声明(下面几个现场切换函数都要用)

// ---- 方向/反显/主题的 NVS 持久化(命名空间复用 "disp") ----
// ★为什么必须存: V(横竖屏)/K(方向)/J(反显)/Q(主题) 都是"现场一点点试出来的结果" ——
//   只改内存的话一断电就回退到 config.h 的默认值, 用户会以为"设置又丢了"。
static void st7789LoadPrefs() {
  Preferences p;
  p.begin("disp", true);
  gSt7789Portrait   = (p.getInt("land", (ST7789_ORIENT_DEFAULT != 0) ? 1 : 0) != 0);
  gSt7789MadctlLand = (uint8_t)p.getInt("madL", ST7789_MADCTL_LAND);
  gSt7789MadctlPort = (uint8_t)p.getInt("madP", ST7789_MADCTL_PORT);
  gSt7789MadctlSq   = (uint8_t)p.getInt("madS", ST7789_MADCTL_SQ);   // 240x240 方屏单独一份
  gSt7789InvOn      = (p.getInt("inv", ST7789_INVON) != 0);
  // ILI9341 单独一套(默认值不同: BGR 位必须置 1 / 通常不需要反显)
  gIliMadctlLand    = (uint8_t)p.getInt("imadL", ILI9341_MADCTL_LAND);
  gIliMadctlPort    = (uint8_t)p.getInt("imadP", ILI9341_MADCTL_PORT);
  gIliInvOn         = (p.getInt("iinv", ILI9341_INVON) != 0);
  gSt7789ThemeIdx   = p.getInt("thm", ST7789_THEME);
  // 界面不透明度(只对"有彩色背景图"那条路径有意义; 没上传背景图时这个值不参与任何计算)
  { int a = p.getInt("bga", BG_UI_ALPHA_DEFAULT);
    if (a < 0)   a = 0;
    if (a > 100) a = 100;
    sBgUiAlpha = (uint8_t)a; }
  // 背景图亮度(同样只在"有彩色背景图"时有意义; 100=原图)。★须在 .ino 的 bgLoad() 之前读到,
  // 否则首次加载会用默认 100 而漏掉压暗 —— st7789LoadPrefs 由 begin()->loadSettings() 调用, 早于 bgLoad。
  { int b = p.getInt("bgb", BG_BRIGHT_DEFAULT);
    if (b < 0)   b = 0;
    if (b > 100) b = 100;
    sBgBright = (uint8_t)b; }
  p.end();
  if (gSt7789ThemeIdx < 0 || gSt7789ThemeIdx >= kSt7789ThemeCount) gSt7789ThemeIdx = 0;
  gSt7789Fg = kSt7789Themes[gSt7789ThemeIdx].fg;
  gSt7789Bg = kSt7789Themes[gSt7789ThemeIdx].bg;
}
static void st7789SavePrefs() {
  Preferences p;
  p.begin("disp", false);
  p.putInt("land", gSt7789Portrait ? 1 : 0);
  p.putInt("madL", gSt7789MadctlLand);
  p.putInt("madP", gSt7789MadctlPort);
  p.putInt("madS", gSt7789MadctlSq);      // 240x240 方屏单独一份
  p.putInt("inv",  gSt7789InvOn ? 1 : 0);
  p.putInt("imadL", gIliMadctlLand);      // ILI9341 单独一套(与 ST7789 互不覆盖)
  p.putInt("imadP", gIliMadctlPort);
  p.putInt("iinv",  gIliInvOn ? 1 : 0);
  p.putInt("thm",  gSt7789ThemeIdx);
  p.end();
}

// ---- 上电初始化: 三个公共部件 + 两种面板各自的头部(2026-10-09 拆出, 供 ST7789/ILI9341 共用) ----
// 公共头部: 配引脚 + 取公共总线 + 硬复位
static void tft_hw_prepare(void) {
  pinMode(LCD2_CS,  OUTPUT); st7789_cs(HIGH);
  pinMode(LCD2_DC,  OUTPUT); st7789_dc(HIGH);
  pinMode(LCD2_RST, OUTPUT);
  // ★背光脚(LCD2_BL)不在这里碰: 它已挂 LEDC, 任何 pinMode/digitalWrite 都会把 PWM 打掉,
  //   背光由 DisplayUI::blInit()/blApply() 统一管理(见文件顶部"背光 PWM"一节)。
  hwspiAcquire(LCD_BUS_SPI_HZ_ST7789);   // 取用公共总线(两种面板同一档时钟)
  digitalWrite(LCD2_RST, LOW);  delay(10);  digitalWrite(LCD2_RST, HIGH);  delay(120);
}
// 公共尾部: 退休眠 -> 像素格式 -> MADCTL(方向) -> 反显 -> 铺底色 -> 开显示
//   ★顺序对两种面板都成立: MADCTL/COLMOD 允许在 SLPOUT 之后写(ST7789 官方序列即如此);
//     ILI9341 的电源/伽马序列在它之前(见 ili9341_hw_init)。
static void tft_init_tail(void) {
  st7789_tx(0x11, nullptr, 0);  delay(120);             // SLPOUT: 退出休眠
  { const uint8_t v = 0x55; st7789_tx(0x3A, &v, 1); }  delay(10);   // COLMOD: 16bit/pixel(RGB565)
  // ★MADCTL 必须开 MV(bit5) 交换行列 —— 否则芯片按原生 240 列扫描, 而下面窗口按 320 宽设置,
  //   CASET 0..319 会越界回绕 => 花屏。(横屏值见 config.h, ST7789/ILI9341 各一套)
  { const uint8_t v = stMadctlEff(); st7789_tx(0x36, &v, 1); }  delay(10);
  st7789_tx(tftInv() ? 0x21 : 0x20, nullptr, 0);  delay(10);        // INVON/INVOFF(按芯片默认)
  st7789_fillBg();                                    // 用当前底色铺满整屏(尺寸按当前方向)
  st7789_tx(0x29, nullptr, 0);  delay(150);            // DISPON: 开显示
}
// ST7789: 复位 -> SWRESET -> 公共尾部
static void st7789_hw_init(void) {
  tft_hw_prepare();
  st7789_tx(0x01, nullptr, 0);  delay(150);            // SWRESET
  tft_init_tail();
}
// ILI9341: 复位 -> SWRESET -> 【电源/时序/伽马序列】 -> 公共尾部
//   ★为什么 ILI9341 多这一大段: 它的电源电压(0xC0/0xC1)、VCOM(0xC5/0xC7)、帧率(0xB1)、
//     γ 校正(0x26/0xE0/0xE1)都要显式配置; 少了这些, 多数模块表现为【偏色/发暗/闪烁/对比度差】。
//     下面这几条就是最常见 ILI9341 模组的通用上电序列(照搬最广泛验证过的那一套)。
static void ili9341_hw_init(void) {
  tft_hw_prepare();
  st7789_tx(0x01, nullptr, 0);  delay(150);            // SWRESET
  { const uint8_t v[] = {0x03, 0x80, 0x02};           st7789_tx(0xEF, v, 3); }
  { const uint8_t v[] = {0x00, 0xC1, 0x30};           st7789_tx(0xCF, v, 3); }
  { const uint8_t v[] = {0x64, 0x03, 0x12, 0x81};     st7789_tx(0xED, v, 4); }
  { const uint8_t v[] = {0x85, 0x00, 0x78};           st7789_tx(0xE8, v, 3); }
  { const uint8_t v[] = {0x39, 0x2C, 0x00, 0x34, 0x02}; st7789_tx(0xCB, v, 5); }
  { const uint8_t v = 0x20;                           st7789_tx(0xF7, &v, 1); }
  { const uint8_t v[] = {0x00, 0x00};                 st7789_tx(0xEA, v, 2); }
  { const uint8_t v = 0x23;                           st7789_tx(0xC0, &v, 1); }  // Power control 1
  { const uint8_t v = 0x10;                           st7789_tx(0xC1, &v, 1); }  // Power control 2
  { const uint8_t v[] = {0x3E, 0x28};                 st7789_tx(0xC5, v, 2); }  // VCOM control 1
  { const uint8_t v = 0x86;                           st7789_tx(0xC7, &v, 1); }  // VCOM control 2
  { const uint8_t v[] = {0x00, 0x18};                 st7789_tx(0xB1, v, 2); }  // Frame rate: 全速
  { const uint8_t v[] = {0x08, 0x82, 0x27};           st7789_tx(0xB6, v, 3); }  // Display function
  { const uint8_t v = 0x00;                           st7789_tx(0xF2, &v, 1); }  // 3Gamma disable
  { const uint8_t v = 0x01;                           st7789_tx(0x26, &v, 1); }  // Gamma set
  { const uint8_t v[] = {0x0F,0x31,0x2B,0x0C,0x0E,0x08,0x4E,0xF1,0x37,0x07,0x10,0x03,0x0E,0x09,0x00};
    st7789_tx(0xE0, v, 15); }                                                   // 正伽马
  { const uint8_t v[] = {0x00,0x0E,0x14,0x03,0x11,0x07,0x31,0xC1,0x48,0x08,0x0F,0x0C,0x31,0x36,0x0F};
    st7789_tx(0xE1, v, 15); }                                                   // 负伽马
  tft_init_tail();
}
// 按当前芯片选初始化(由 u8x8 显示回调的 INIT 消息调用)
static void tft_hw_init(void) { if (gTftIli) ili9341_hw_init(); else st7789_hw_init(); }

// ---- MADCTL 现场切换(串口 K 命令) ----
// 不同厂家的 ST7789 模组, "横屏方向"与"RGB/BGR 顺序"这几位常不一样; 上板后若画面花/颠倒/红蓝反,
// 用串口 K 逐个试比"改宏+重烧"快得多。改完必须重设窗口 + 重画底色, 否则屏上还是旧排布的旧内容。
static void st7789_apply_madctl(uint8_t v) {
  stMadctl() = v;
  st7789_tx(0x36, &v, 1);  delay(10);
  st7789_fillBg();                                      // 用新方向/新底色铺满整屏
  st7789SavePrefs();                                    // ★存 NVS: 重启后仍然生效
}

void st7789MadctlCycle() {
  // 候选【按 芯片 x 方向 分组】, 只列"该方向物理上成立"的值 —— 越界的那种(横屏配 MV=0)会花屏,
  //   放进轮换里只会让用户白试一轮, 故剔除。
  //   ST7789 横屏(MV 必须=1): 0x60 / 0xA0 = 正/反两个横屏方向; 0xE0 / 0x20 = 其镜像(少数模组玻璃装反)
  //   ST7789 竖屏(MV 必须=0): 0x00 / 0xC0 = 正/反两个竖屏方向; 0x40 / 0x80 = 其镜像
  //   ILI9341 全部带 BGR(bit3), 否则红蓝互换:
  //     横屏(MV=1): 0x28(MV) / 0x68(MX|MV) / 0xA8(MY|MV) / 0xE8(MX|MY|MV)
  //     竖屏(MV=0): 0x48(MX) / 0x88(MY)  / 0x08(BGR)     / 0xC8(MX|MY)
  //   ★2026-10-11 方屏(240x240): 方屏没有"横竖之分", MV 只起"整体转 90°"的作用, 故四个值
  //     覆盖"原方向 / 转90° / 上下颠倒 / 左转90°"四种安装姿态, 与横竖屏的表无关。
  static const uint8_t stSq[]    = { 0x00, 0x60, 0xC0, 0xA0 };
  static const uint8_t stLand[]  = { 0x60, 0xA0, 0xE0, 0x20 };
  static const uint8_t stPort[]  = { 0x00, 0xC0, 0x40, 0x80 };
  static const uint8_t iliLand[] = { 0x28, 0x68, 0xA8, 0xE8 };
  static const uint8_t iliPort[] = { 0x48, 0x88, 0x08, 0xC8 };
  const uint8_t* seq = gTftIli ? (gSt7789Portrait ? iliPort : iliLand)
                     : gSt7789Square ? stSq
                               : (gSt7789Portrait ? stPort  : stLand);
  const int n = (int)(gTftIli ? sizeof(iliPort)
                            : gSt7789Square ? sizeof(stSq)
                            : (gSt7789Portrait ? sizeof(stPort) : sizeof(stLand)));
  int cur = -1;
  for (int i = 0; i < n; i++) if (seq[i] == stMadctl()) { cur = i; break; }
  const uint8_t nv = seq[(cur + 1) % n];
  st7789_apply_madctl(nv);
  Serial.printf("[SCR ] MADCTL = 0x%02X (MV=%d MX=%d MY=%d ML=%d BGR=%d) —— 铺满且不花 = 对了\n",
                nv, (nv >> 5) & 1, (nv >> 6) & 1, (nv >> 7) & 1, (nv >> 4) & 1, (nv >> 3) & 1);
  Serial.println("[SCR ] 已存 NVS, 重启后仍生效");
}
uint8_t st7789MadctlGet() { return stMadctlEff(); }

// ---- 反显开关(串口 J 命令 / 后台"反色"开关,共用同一份 NVS 键 "inv") ----
// 画面【整片发白、像照片底片】(底色变亮、文字变暗) = 反显设反了。串口 J 立即翻转, 免重烧;
// 后台也可点(2026-10-07 新增), 两者共用同一份状态与 NVS, 互相同步。
void st7789ToggleInvert() {
  st7789SetInvert(!tftInv());              // ★统一走 set: 保证"设值 + 发命令 + 存NVS"三步永远一致
}
bool st7789InvGet() { return tftInv(); }

// 设置反显并立即生效。★自由函数: 后台 WS 回调在 async_tcp 任务上下文, 拿不到 DisplayUI 实例;
//   而反显只发一条 SPI 命令 + 写 NVS, 不碰 u8g2 对象, 所以【并发安全】, 无需重建显示。
//   ★ST7789 / ILI9341 各存一份(tftInv()), 两块屏来回切各自记住自己的极性。
void st7789SetInvert(bool on) {
  tftInv() = on;
  st7789_tx(tftInv() ? 0x21 : 0x20, nullptr, 0);   // INVON / INVOFF
  delay(10);
  st7789SavePrefs();                                  // ★存 NVS: 重启后仍然生效
  Serial.printf("[SCR ] %s 反显 = %s —— 底色应是暗的、字是亮的; 已存 NVS(重启仍生效)\n",
                gTftIli ? "ILI9341" : "ST7789",
                tftInv() ? "ON(0x21)" : "OFF(0x20)");
}

// ---- 调色板主题轮换(串口 Q 命令) ----
// 1bpp 屏只有两个颜色; 本函数在三套预设间切换, 并【立即把整屏铺成新底色】(肉眼立刻可见)。
// 内容由主循环下一帧按新调色板重画 —— 调用方随后要调 display.invalidate() 强制重画。
void st7789ThemeCycle() {
  gSt7789ThemeIdx = (gSt7789ThemeIdx + 1) % kSt7789ThemeCount;
  gSt7789Fg = kSt7789Themes[gSt7789ThemeIdx].fg;
  gSt7789Bg = kSt7789Themes[gSt7789ThemeIdx].bg;
  st7789_fillBg();                                      // 立即把整屏铺成新底色(肉眼立刻可见)
  st7789SavePrefs();                                    // ★存 NVS: 重启后仍然生效
  Serial.printf("[SCR ] 调色板 -> %s (FG=0x%04X BG=0x%04X) 已存 NVS\n",
                kSt7789Themes[gSt7789ThemeIdx].name, (unsigned)gSt7789Fg, (unsigned)gSt7789Bg);
}
const char* st7789ThemeName() { return kSt7789Themes[gSt7789ThemeIdx].name; }
int  st7789ThemeGet() { return gSt7789ThemeIdx; }
bool st7789PortraitGet() { return gSt7789Portrait; }

// ---- 横竖屏切换(串口 O / 等价于后台"旋转"选 90° 或 270°) ----
// ★统一走"旋转"这一条通道: 竖屏 = 90°, 横屏 = 0°。
//   以前"方向(land)"与"旋转(rot)"各存一份状态, 两者会互相打架 —— 在后台改了旋转、串口再按 O,
//   屏上的实际方向就对不上了。现在 **rot 是唯一真源**, 方向和 180° 翻转都由它在 recreateU8g2()
//   里派生出来。
// ★这里只置 pending, 真正重建交给主循环 tick() 里的 recreateU8g2()(u8g2 非线程安全,
//   而本函数可能从 Web 异步任务(后台"旋转"下发)调用)。
void DisplayUI::setST7789Portrait(bool portrait) {
  if (!IS_TFT_SCREEN(mType)) {   // 只有彩屏用这套(其它屏按常规旋转走, 别误改)
    Serial.println("[SCR ] 当前屏不是彩屏(ST7789/ILI9341), 忽略横竖屏切换");
    return;
  }
  // ★2026-10-11: 240x240 方屏没有"横竖"之分(两个方向都是 240), 这条命令对它无意义 ——
  //   直接拒绝, 免得白白重建一次屏幕并打印一组误导性的 320x240 尺寸。
  if (gSt7789Square) {
    Serial.println("[SCR ] 当前是 240x240 方屏, 没有横/竖之分, 忽略(方屏请用后台旋转或串口 K 调 MADCTL)");
    return;
  }
  setRotation(portrait ? 90 : 0);        // 存 NVS + 置 pending
  Serial.printf("[SCR ] %s 方向 -> %s(%dx%d) MADCTL=0x%02X 已存 NVS, 重启仍生效\n",
                gTftIli ? "ILI9341" : "ST7789",
                portrait ? "竖屏 90°" : "横屏 0°",
                portrait ? ST7789_PW : ST7789_W, portrait ? ST7789_PH : ST7789_H,
                stMadctlEff());
}

// ★把"当前没选中的那一块屏"的片选【主动拉到无效电平】。
// 为什么需要: 三屏共用 CLK/DAT/DC/RST 后, 未选中屏的这几根线也在跟着跳 —— 只要它的 CS
//   恰好悬空/被拉到有效电平, 它就会把总线上的命令当成自己的命令, 于是【自己乱显示】
//   (液晶不会坏, 但观感差 + 白耗电)。把它的 CS 钉在无效电平(高)就彻底杜绝。
// 调用时机: 每次 recreateU8g2() 开头(切屏型/开机都走), 位于各屏自己的 begin() 之前。
static void deselectOtherCs(int activeType) {
  const bool tftActive = IS_TFT_SCREEN(activeType);  // 彩屏共用 LCD2_CS
  if (activeType != SCREEN_ST7567) { pinMode(LCD_CS,  OUTPUT); digitalWrite(LCD_CS,  HIGH); }  // ST7567: CS 高 = 不选中
  if (!tftActive)                  { pinMode(LCD2_CS, OUTPUT); digitalWrite(LCD2_CS, HIGH); }  // 彩屏: CS 高 = 不选中
}

// U8g2 显示回调: 仅 DRAW_TILE 做像素格式桥接, 其余消息走标准处理(横竖屏共用)
static uint8_t u8x8_d_st7789_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
  switch (msg) {
    case U8X8_MSG_DISPLAY_SETUP_MEMORY:
      u8x8_d_helper_display_setup_memory(u8x8, stInfo());   // 按当前横/竖屏选设备描述
      break;
    case U8X8_MSG_DISPLAY_INIT:
      tft_hw_init();   // 按当前芯片(ST7789/ILI9341)走各自的上电序列(自建复位, 不依赖 U8g2 的 GPIO 回调)
      break;
    case U8X8_MSG_DISPLAY_SET_POWER_SAVE:
      // arg_int=0 显示开; =1 省电。★背光不在这里动 —— 走 LEDC 的引脚只能用 blPwm 写,
      // 且"省电"语义由 DisplayUI 的背光超时/关机流程统一处理(避免两处抢同一根脚)。
      break;
    case U8X8_MSG_DISPLAY_SET_CONTRAST:
      break;   // 彩色 TFT 无对比度寄存器(这是 ST7789 的固有事实, 亮度只能靠背光 PWM)
    case U8X8_MSG_DISPLAY_DRAW_TILE: {
      u8x8_tile_t *tile = (u8x8_tile_t *)arg_ptr;
      const int x0 = tile->x_pos * 8;       // 全宽缓冲: x_pos=0
      const int y0 = tile->y_pos * 8;       // 本 band 顶部像素 y
      const int cols = tile->cnt * 8;       // 本 band 列数(整幅宽 = 240)
      st7789_window(x0, y0, x0 + cols - 1, y0 + 7);
      const uint8_t *src = tile->tile_ptr;  // 指向本 band 起始(共 cols 字节, 每字节=1 列 8 行)
      // 组包: 按「行优先」展开成 RGB565 字节流(MSB 先发), 一次 writeBytes 推出整个 band。
      uint8_t *dst = gSt7789Band;
      // ★彩色背景图(2026-10-06): 有图时 bit=0 的像素【取图里那一像素的颜色】而不是主题底色;
      //   bit=1(界面文字/边框)默认还是纯色 gSt7789Fg, 当界面不透明度 <100 时与背景做真正的
      //   RGB565 线性混合(不是抖动)。bgBase 为 nullptr 时下面这些分支都在循环外就判掉了,
      //   与原固件的逐像素代码完全等价 —— 这就是"没上传背景图时零影响"的实现保证。
      const uint8_t* bgBase = sBgBuf565;
      // ★2026-10-11 修: 必须校验"已缓冲那张图的每行像素数 == 当前屏宽", 否则图的行宽对不上,
      //   整屏底图会斜着错位/撕裂(例如从别的屏型/方向切回来, 内存里还是另一种几何的图)。
      if (bgBase && bgLoadedStride() != stW()) bgBase = nullptr;
      // (v1.14: 原"无上传图时用内置壁纸兜底"的分支已移除 —— 无图就是干净的卡通纯色, 与 FULL 一致。)
      const uint8_t  uiA    = sBgUiAlpha;
      const int      bgW    = stW();
      for (int row = 0; row < 8; row++) {
        // 本行在背景图里的起始地址(行优先, 2 字节/像素, 大端)
        const int yy = y0 + row;
        const uint8_t* bgRow = bgBase ? (bgBase + (((size_t)yy * (size_t)bgW + x0) * 2)) : nullptr;
        // ★元素配色: bit=1 的前景色【按行取该元素选的颜色】; 未登记的行回落 gSt7789Fg(旧行为)。
        //   同一行登记了左右两个元素时, 按分界 x 拆成两段各用自己的颜色。
        //   查表在【每行】做一次, 列循环里只是一次比较 —— 热路径零额外开销。
        const bool     zok  = (yy >= 0 && yy < kZoneRows);
        const int8_t   za   = zok ? gRowZoneA[yy]  : -1;
        const int8_t   zb   = zok ? gRowZoneB[yy]  : -1;
        const int16_t  sx   = zok ? gRowSplitX[yy] : -1;
        const uint16_t fgA  = (za >= 0) ? gElemRgb[za] : gSt7789Fg;
        const uint16_t fgB  = (zb >= 0) ? gElemRgb[zb] : fgA;
        const uint16_t fgF  = gElemRgb[ELEM_FRAME];   // 装饰线框专用色
        int splitI = -1;                       // <0 = 整行单色
        if (zb >= 0 && sx >= 0) {              // 左右拆分: 换算成本 tile 内的列下标
          splitI = sx - x0;
          if (splitI < 0) splitI = 0; else if (splitI > cols) splitI = cols;
        }
        // 本行的"框色列"表(可多个线框同行)与横框区间(顶/底边框 + 圆角角块)
        const int16_t* fc  = zok ? gRowFc[yy] : nullptr;
        const bool     hFx = zok && (gRowFx0[yy] >= 0);
        const int16_t  fx0 = hFx ? gRowFx0[yy] : 0;
        const int16_t  fx1 = hFx ? gRowFx1[yy] : 0;
        const bool     hFx2 = zok && (gRowFx2[yy] >= 0);
        const int16_t  fx2 = hFx2 ? gRowFx2[yy] : 0;
        const int16_t  fx3 = hFx2 ? gRowFx3[yy] : 0;
        for (int i = 0; i < cols; i++) {
          // bit0=顶部(vertical_top_lsb); 1->界面色 / 0->背景(有图取图, 无图取卡通底色 gBgRgb)
          const bool on = ((src[i] >> row) & 1) != 0;
          uint16_t c;
          if (on) {
            // ★线框优先(只对"亮点"判定, 暗色像素不必比框列 -> 省掉大部分无用的比较):
            //   竖框钉精确列(i==某一框列), 横框钉本行 x0..x1 区间; 命中即框色, 否则按元素色。
            uint16_t fg = (splitI < 0) ? fgA : ((i < splitI) ? fgA : fgB);
            bool fr = false;
            if (fc) {
              for (int k = 0; k < kFcMax; k++) {
                const int16_t fcv = fc[k];
                if (fcv < 0) break;
                if (i == fcv) { fr = true; break; }
              }
            }
            if (!fr && hFx && i >= fx0 && i <= fx1) fr = true;
            if (!fr && hFx2 && i >= fx2 && i <= fx3) fr = true;   // 圆角右角块
            if (fr) fg = fgF;
            c = fg;
            if (bgRow && uiA < 100) {          // 界面半透明: 与背景混合
              const uint16_t bgc = (uint16_t)(((uint16_t)bgRow[i * 2] << 8) | bgRow[i * 2 + 1]);
              c = blend565(fg, bgc, uiA);
            }
          } else if (bgRow) {
            c = (uint16_t)(((uint16_t)bgRow[i * 2] << 8) | bgRow[i * 2 + 1]);
          } else {
            c = gBgRgb;   // ★2026-10-16 卡通配色: 无背景图 -> 卡通色板, 不再是主题 gSt7789Bg
          }
          *dst++ = (uint8_t)(c >> 8);
          *dst++ = (uint8_t)(c & 0xFF);
        }
      }
      gHwSpi.writeBytes(gSt7789Band, (size_t)cols * 16);
      st7789_end();
      break;
    }
    default:
      return 0;
  }
  return 1;
}

// 自定义全缓冲设备构造(仿 U8G2_Setup_xxx_f): 注入自有缓冲 + 自定义回调。
// 三个编译期已踩的坑, 勿改回去:
//   1) u8g2_SetupDisplay 是 5 参宏(display_cb, cad_cb, byte_cb, gpio_and_delay_cb);
//      本设备自行驱动硬件 SPI, 不经过 u8x8 的 CAD 协议, 故 cad/byte 都传 empty。
//   2) u8g2_SetupBuffer 收的是 u8g2_t*, 必须用 u->getU8g2() 取内部对象(U8G2 本身不是 u8g2_t)。
//   3) U8G2 只有默认构造 U8G2(void), 没有带回调的构造函数 -> new U8G2() 后手动 setup。
static void u8g2_Setup_st7789_f(U8G2 *u, const u8g2_cb_t *rotation) {
  u8g2_t *p = u->getU8g2();
  u8g2_SetupDisplay(p, u8x8_d_st7789_cb, u8x8_cad_empty, u8x8_byte_empty, u8x8_gpio_and_delay_arduino);
  // ★tile_buf_height 必须 = 当前方向的高度/8(横屏 30 / 竖屏 40), 否则 U8g2 会按分页逻辑只发一部分
  u8g2_SetupBuffer(p, gSt7789Buf, stH() / 8, u8g2_ll_hvline_vertical_top_lsb, rotation);
}

// 旋转角度 -> U8g2 旋转回调(0/90/180/270°)。ST7789 正方屏用软件旋转: U8g2 在帧缓冲层
// 把内容转好, 自定义设备 DRAW_TILE 照常 1:1 推屏, MADCTL 保持 0x00 不必动。
// (U8G2_Rx 是回调指针, 不是数字; 三屏通用, 无需区分屏型。)
static const u8g2_cb_t* rotCbFor(int deg) {
  switch (deg) {
    case 90:  return U8G2_R1;
    case 180: return U8G2_R2;
    case 270: return U8G2_R3;
    default:  return U8G2_R0;
  }
}

// 旋转角度 -> 人类可读串(给日志/串口用)
static const char* rotStr(int deg) {
  switch (deg) {
    case 90:  return "90°";
    case 180: return "180°";
    case 270: return "270°";
    default:  return "0°";
  }
}

// ---- 整帧跳过用的内容指纹(FNV-1a) ----
// 比较几十字节字符串的成本几乎为 0; 一旦命中就省掉「逐字测宽 + 逐字查 GB2312 字体表绘制 +
// 软件 SPI 整屏发送」这一整套(实测 56~63ms)。字符串之间混入 0xFF 作分隔, 避免
// "ab"+"c" 与 "a"+"bc" 拼出同一个指纹。
static inline void mixStr(uint32_t& h, const String& s) {
  for (size_t i = 0; i < s.length(); i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
  h ^= 0xFFu; h *= 16777619u;
}
static const uint32_t FRAME_HASH_SEED = 2166136261u;   // FNV-1a 初值

// ============================================================================
// 元素配色 —— 分区登记 + 后台下发(见 display_ui.h 顶部说明)
// ----------------------------------------------------------------------------
// ★为什么"只按行分区"就够: 屏上每个元素本来就各占自己的行(台名行/信息行/状态行/底行…);
//   同一行里左右两个元素(码率 vs WiFi、IP vs 电量、时钟 vs 天气卡)用 Split 版按 x 拆开。
//   ⇒ 覆盖到"每一个元素", 而绘制代码只多出一行登记调用。
// ============================================================================
void DisplayUI::elemZoneReset() {
  for (int i = 0; i < kZoneRows; i++) {
    gRowZoneA[i] = -1; gRowZoneB[i] = -1; gRowSplitX[i] = -1;
    for (int k = 0; k < kFcMax; k++) gRowFc[i][k] = -1;
    gRowFx0[i] = -1; gRowFx1[i] = -1;
    gRowFx2[i] = -1; gRowFx3[i] = -1;
  }
}
void DisplayUI::elemZone(int idx, int y0, int y1) {
  if (y0 < 0) y0 = 0;
  if (y1 >= kZoneRows) y1 = kZoneRows - 1;
  for (int y = y0; y <= y1; y++) {
    gRowZoneA[y] = (int8_t)idx; gRowZoneB[y] = -1; gRowSplitX[y] = -1;
  }
}
void DisplayUI::elemZoneSplit(int idxL, int idxR, int sx, int y0, int y1) {
  if (y0 < 0) y0 = 0;
  if (y1 >= kZoneRows) y1 = kZoneRows - 1;
  for (int y = y0; y <= y1; y++) {
    gRowZoneA[y] = (int8_t)idxL; gRowZoneB[y] = (int8_t)idxR; gRowSplitX[y] = (int16_t)sx;
  }
}
// 登记"一行文字"的元素色区: 传基线, 自动按中文字形规格留足上下界(见 kGlyphAsc/kGlyphDesc 注释)。
void DisplayUI::elemZoneText(int idx, int yBase) {
  elemZone(idx, yBase - kGlyphAsc, yBase + kGlyphDesc);
}
// 把某一列追加进"该行框色列"表(去重; 满了就丢, 正常一块屏不会超过 kFcMax 个竖框同行)。
static void addFc(int y, int x) {
  for (int k = 0; k < kFcMax; k++) {
    if (gRowFc[y][k] == (int16_t)x) return;       // 已有
    if (gRowFc[y][k] < 0) { gRowFc[y][k] = (int16_t)x; return; }
  }
  // ★2026-10-11 越界告警: 同一行登记的竖框超过 kFcMax 个, 多余的列不会被染成框色(会露主题前景色)。
  //   正常一块面板一行不会超过 4 个竖框; 真出现就是新布局与 kFcMax 不匹配 -> 调大 kFcMax。
  static bool sFcWarned = false;
  if (!sFcWarned) { sFcWarned = true;
    Serial.printf("[LCD] ★elemFrameEdge: 第%d行竖框超过 kFcMax=%d 个, 多余列将露主题色\n", y, kFcMax); }
}
// 把矩形 [x0,y0]..[x1,y1] 的【边框】标记为 ELEM_FRAME 色: 顶/底两行钉整段 x0..x1(横框),
// 中间各行只钉左右两列 col==x0 / col==x1(竖框)。越界先夹紧; 内部像素仍归元素色, 不会被框色误伤。
// ★r = 该框的圆角半径(drawRFrame 的最后一个参数; drawFrame/drawHLine 传 0):
//   半径带内的【圆弧像素】不在 x0/x1 那两列上, 必须额外把左右两个【角块】也钉成框色,
//   否则四个转角会露出主题前景色(白)。见 gRowFx2/gRowFx3 的注释。
void DisplayUI::elemFrameEdge(int x0, int y0, int x1, int y1, int r) {
  if (y0 < 0) y0 = 0;
  if (y1 >= kZoneRows) y1 = kZoneRows - 1;
  const int w = stW();
  if (x0 < 0) x0 = 0;
  if (x1 >= w) x1 = w - 1;
  if (r < 0) r = 0;
  for (int y = y0; y <= y1; y++) {
    if (y == y0 || y == y1) {            // 顶 / 底横框: 这一行只有 x0..x1 之间是框色
      gRowFx0[y] = (int16_t)x0; gRowFx1[y] = (int16_t)x1;
    } else {                             // 左右竖框: 只钉那一列(可多个线框同行, 追加不覆盖)
      addFc(y, x0); addFc(y, x1);
      // 圆角行(距顶/底 <= r): 再钉左右【角块】[x0,x0+r] 与 [x1-r,x1]
      if (r > 0 && (y - y0 <= r || y1 - y <= r)) {
        const int rl = (x0 + r <= x1) ? x0 + r : x1;
        const int rr = (x1 - r >= x0) ? x1 - r : x0;
        gRowFx0[y] = (int16_t)x0; gRowFx1[y] = (int16_t)rl;   // 左角块
        gRowFx2[y] = (int16_t)rr; gRowFx3[y] = (int16_t)x1;   // 右角块
      }
    }
  }
}
void DisplayUI::setElemColor(int i, int palIdx) {
  if (i < 0 || i >= ELEM_COUNT) return;
  if (palIdx < 0 || palIdx >= ELEM_PALETTE_N) palIdx = 0;
  mElemPal[i] = palIdx; gElemPalS[i] = palIdx; gElemRgb[i] = kElemPalette[palIdx];
  saveSettings();
  mFrameValid = false;   // ★v1.13 修: 只改颜色不改内容时内容指纹不变 -> 整帧会被跳过, 必须显式重画
}
void DisplayUI::setElemColors(const int* v, int n) {
  if (n > ELEM_COUNT) n = ELEM_COUNT;
  for (int i = 0; i < n; i++) {
    int p = v[i];
    if (p < 0 || p >= ELEM_PALETTE_N) p = 0;
    mElemPal[i] = p; gElemPalS[i] = p;
  }
  elemSync();          // 一次性重算 RGB 表(热路径里不再查表)
  saveSettings();
  mFrameValid = false; // ★v1.13 修: 同 setElemColor —— 否则后台改元素配色"看不到变化"
}
// ★背景色(2026-10-16 由 FULL/FM 主线移植): 下标指向 kBgPalette[](0=淡蓝天空 … 7=纯白)。
//   存 NVS("bg")并立即生效(gBgRgb 是 DRAW_TILE 热路径唯一读取源, 下一帧整屏底色即变)。
void DisplayUI::setBgColor(int idx) {
  if (idx < 0 || idx >= kBgPaletteN) idx = 0;
  mBgIdx = idx;
  gBgRgb = kBgPalette[idx];
  saveSettings();
  mFrameValid = false;      // ★v1.13 修: 改底色同样要强制重画(否则整帧被跳过, 屏上还是旧底色)
}

// ============================================================================
// 显示设置持久化(NVS 命名空间 "disp": type / ctr / rot / bl / brg / ecol / bg)
// 屏幕类型/对比度/旋转(0/90/180/270°) 在 Web 后台可调。对比度与旋转为"立即生效",
// 但 WS 下发发生在异步任务上下文, 不能直接碰 u8g2(与主循环 show() 并发,
// u8g2 非线程安全) -> 先存 NVS + 置 pending 标志, 由主循环 tick() 统一应用。
// ============================================================================

void DisplayUI::loadSettings() {
  Preferences p;
  p.begin("disp", true);
  mType      = p.getInt("type", SCREEN_DEFAULT);
  mContrast  = p.getInt("ctr", LCD_CONTRAST);
  mRotation  = p.getInt("rot", 0);
  mBLTimeout = p.getInt("bl", LCD_BL_TIMEOUT_MS);
  mBright    = p.getInt("brg", LCD_BL_BRIGHT_DEFAULT);
  // ---- 元素配色(每个元素一个色板下标, 缺省全 0=白=旧行为) ----
  { uint8_t ec[ELEM_COUNT];
    const size_t ecN = p.getBytes("ecol", ec, ELEM_COUNT);
    bool allZero = true;                      // ★卡通配色: "从没改过颜色"的设备自动套卡通色板
    for (int i = 0; i < ELEM_COUNT; i++) {
      int v = (i < (int)ecN) ? (int)ec[i] : 0;
      if (v != 0) allZero = false;
      if (v < 0 || v >= ELEM_PALETTE_N) v = 0;
      mElemPal[i] = v; gElemPalS[i] = v;
    }
    // ★卡通风格(2026-10-16 由 FULL/FM 主线移植): 从未改过颜色的设备(ecol 全零)直接套用卡通
    //   默认色板, 让新装/复位设备一上来就是糖果配色; 老用户已手动设过则保持原样, 不强制覆盖。
    if (allZero) {
      for (int i = 0; i < ELEM_COUNT; i++) { mElemPal[i] = kCartoonElemPal[i]; gElemPalS[i] = kCartoonElemPal[i]; }
    } }
  // ---- 卡通背景色(bit=0 且无背景图时的底色; NVS "bg" = kBgPalette 下标) ----
  mBgIdx = p.getInt("bg", 0);
  if (mBgIdx < 0 || mBgIdx >= kBgPaletteN) mBgIdx = 0;
  gBgRgb = kBgPalette[mBgIdx];
  p.end();
  // ★必须重算 RGB 表: 静态 gElemRgb 初值是 0(黑), 不 sync 开机就全黑(本工程旧版漏了这一步,
  //   卡通色板也一并靠它落地)。幂等, 开销极小。
  elemSync();
  // 合法性钳制(NVS 可能被外部改坏)
  // ★2026-10-09: 新增第 4 种屏型 ILI9341(与 ST7789 共用同一组引脚与驱动核心)。
  if (!IS_TFT_SCREEN(mType) && mType != SCREEN_ST7567 && mType != SCREEN_SSD1306)
    mType = SCREEN_DEFAULT;
  gTftIli = (mType == SCREEN_ILI9341);   // 上电即让 stMadctl()/tftInv() 指向正确芯片那一套
  if (mContrast < 0 || mContrast > 255) mContrast = LCD_CONTRAST;
  if (mRotation == 1) mRotation = 180;          // 兼容旧版 NVS(曾用 0/1 表示是否旋转)
  if (mRotation != 0 && mRotation != 90 && mRotation != 180 && mRotation != 270) mRotation = 0;
  if (mBLTimeout < 0 || mBLTimeout > 3600000) mBLTimeout = LCD_BL_TIMEOUT_MS;
  if (mBright < 0 || mBright > 100) mBright = LCD_BL_BRIGHT_DEFAULT;   // 亮度 %(越界回落默认值)
  st7789LoadPrefs();   // ST7789 的横竖屏/方向/反显/主题(NVS "disp", 由串口 V/K/J/Q 写入)
}

void DisplayUI::saveSettings() {
  Preferences p;
  p.begin("disp", false);
  p.putInt("type", mType);
  p.putInt("ctr",  mContrast);
  p.putInt("rot",  mRotation);
  p.putInt("bl",   mBLTimeout);
  p.putInt("brg",  mBright);
  { uint8_t ec[ELEM_COUNT];
    for (int i = 0; i < ELEM_COUNT; i++) ec[i] = (uint8_t)mElemPal[i];
    p.putBytes("ecol", ec, ELEM_COUNT); }
  p.putInt("bg", mBgIdx);       // 卡通背景色(bit=0 且无背景图时的底色, kBgPalette 下标)
  p.end();
  elemSync();          // 颜色可能刚被改, 重算一次 RGB 表(幂等, 开销极小)
}

DisplayUI::DisplayUI() {}

// 按当前屏型/旋转/对比度重建 u8g2(旋转与屏型只能构造期设定, 故变更须整体重建); 仅主循环调用。
// 同时负责背光脚管理: 仅 ST7567 有背光脚, SSD1306 自发光。
// ---- SSD1306 硬件 I2C(Wire1): 自动探测地址 ----
// ★2026-09-23 修"滚动撕裂": 原来 1306 走 U8g2 软件 I2C, 位延时只有 2µs/5µs 两档, 整帧 1024B ≈60ms;
//   而 sendBuffer() 是"逐页(8 行)非原子提交", 一帧窗口内屏上"半新半旧" —— 跑马灯时跨页汉字会被
//   水平劈成几截(下面先动、再中间、最后上面)。换硬件 I2C 后整帧 ≈23ms, 撕裂大幅减轻。
// ★走 Wire1(I2C1): S3 有两个 I2C 控制器, 1306 挂 Wire1(15/16); 两条总线彻底分开。
//   硬件 I2C 由外设生成时序, 且【自动开内部上拉】—— 不再需要手工翻脚。
// U8G2 默认地址 0x3C, 但很多 SSD1306 模块实际是 0x3D; 地址错 = 永远无 ACK = 花屏, 故上电先扫真地址。
static uint8_t ssd1306_probeAddress() {
  uint8_t hit = 0;
  Wire1.begin((int)OLED_SDA, (int)OLED_SCL);         // 硬件 I2C1 挂 15/16
  Wire1.setClock(400000);
  delay(5);
  for (uint8_t a : { (uint8_t)0x3C, (uint8_t)0x3D }) {
    Wire1.beginTransmission(a);
    if (Wire1.endTransmission() == 0) { hit = a; break; }
    delay(1);
  }
  return hit;
}
// 硬件 I2C 字节层: 照 U8g2 官方 u8x8_byte_arduino_2nd_hw_i2c 改写, 但【显式指定 15/16 + 400kHz】。
// (U8g2 自带的 _2ND_HW_I2C 类内部只调 Wire1.begin() 无参, 用不到我们要的 15/16, 故自写字节层。)
static uint8_t ssd1306_hw_i2c_byte_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
  switch (msg) {
    case U8X8_MSG_BYTE_SEND:
      Wire1.write((const uint8_t *)arg_ptr, (int)arg_int);
      break;
    case U8X8_MSG_BYTE_INIT:
      Wire1.begin((int)OLED_SDA, (int)OLED_SCL);        // SDA=16, SCL=15
      Wire1.setClock(400000);
      break;
    case U8X8_MSG_BYTE_SET_DC:
      break;                                            // I2C 无 DC 脚
    case U8X8_MSG_BYTE_START_TRANSFER:
      Wire1.beginTransmission((uint8_t)(u8x8_GetI2CAddress(u8x8) >> 1));  // U8g2 存 8 位地址, Wire 要 7 位
      break;
    case U8X8_MSG_BYTE_END_TRANSFER:
      Wire1.endTransmission();
      break;
    default:
      return 0;
  }
  return 1;
}

// ★★切屏型前把【正在显示的旧屏】真正关掉(2026-10-07 修"切到别的屏后旧屏还挂着旧画面")。
//   根因: recreateU8g2() 原来只是 delete 掉旧 U8G2 对象 —— 删掉的是【驱动侧】,
//         而【面板侧】什么都没收到, 它会一直保持 GRAM 里的最后一帧:
//           · SSD1306 OLED : 自发光, 且【没有背光脚】可关 -> 不关它就一直亮着
//                            (用户报的就是这条: 切走后 1306 仍显示切换前的信息);
//           · ST7567 单色  : 非自发光 -> blApply() 会把它那路背光输出 0%, 屏幕自然全黑;
//           · ST7789 彩屏  : 背光同样会被关, 但像素仍在输出 -> 要补一条 DISPOFF 才彻底。
//           · ILI9341 彩屏 : 同 ST7789 —— 补 DISPOFF(0x28) 即可(两芯片此命令同码)。
//   ★必须在 delete 之前、且在 hwspiRelease()/deslectOtherCs() 之前调用: 关屏命令要用
//     旧屏自己的总线与片选, 顺序反了就发不出去。
//   ★为什么 ST7567 不用 setPowerSave(1): U8g2 给 ST7567 的 powersave1 序列是 0xAE + 0xA5,
//     而 0xA5 是"全屏点亮(all points ON)" —— 在被动屏上会留下整片黑底, 且该状态要等下次
//     init 的 0xA4 才复位。它本来就没背光可关, 清掉像素已经够了。
//   ★切回来不用手动恢复: 新对象的 begin() 内部就是 initDisplay()+clearDisplay()+setPowerSave(0);
//     ST7789 的自建 init 序列末尾也有 SLPOUT(0x11)+DISPON(0x29)。
void DisplayUI::blankActivePanel(int type) {
  U8G2* g = mDisp[0];
  if (!g) return;
  if IS_TFT_SCREEN(type) {
    // 彩屏: 让"像素停止输出"就够了 —— 它的背光本来就会被 blApply() 关到 0%,
    //   再整帧清一次要多花约 70ms 的全屏 SPI(~150KB @20MHz), 没必要。
    //   ★DISPOFF(0x28) 对 ST7789 / ILI9341 是同一条命令, 故不必区分旧屏是哪颗。
    hwspiAcquire(LCD_BUS_SPI_HZ_ST7789);   // 该屏的推屏/命令直写 gHwSpi, 不自己取总线
    st7789_tx(0x28, nullptr, 0);           // DISPOFF
    delay(20);
  } else {
    // 1306 / 7567: 这两块屏的"灭"只能靠像素本身 -> 把 GRAM 抹平(1KB, 很快)。
    g->clearBuffer();
    g->sendBuffer();
    // 1306 再补一条 0xAE(显示关)兜底: 个别兼容芯片功率下不去, 清完仍有微亮。
    if (type == SCREEN_SSD1306) g->setPowerSave(1);
  }
  Serial.printf("[LCD] 切屏: 已关闭旧屏 %s\n",
                type == SCREEN_SSD1306 ? "SSD1306 (清屏 + 0xAE 显示关)"
                : type == SCREEN_ST7789_SQ ? "ST7789-方屏 (0x28 DISPOFF)"
                : type == SCREEN_ST7789 ? "ST7789 (0x28 DISPOFF)"
                : type == SCREEN_ILI9341 ? "ILI9341 (0x28 DISPOFF)" : "ST7567 (清像素)");
}

void DisplayUI::recreateU8g2() {
  const u8g2_cb_t* rot = rotCbFor(mRotation);   // U8G2_Rx 是回调指针, 不是数字
  // ★先定"这颗芯片是谁": 后续 stMadctl()/tftInv()/上电序列都按它分叉。
  //   (必须在任何会用到它的调用之前设好; 关旧屏只用 DISPOFF, 两种芯片同码, 故不影响。)
  gTftIli = (mType == SCREEN_ILI9341);
  // ★★只在【真的换屏型】时关旧屏; 单纯改旋转(mBuiltType == mType)不关 —— 否则会白闪一下。
  if (mBuiltType >= 0 && mBuiltType != mType) blankActivePanel(mBuiltType);
  // 释放旧对象(单屏活动, 后台四选一; 不再强制叠 SSD1306 副屏)
  for (int i = 0; i < 2; i++) { if (mDisp[i]) { delete mDisp[i]; mDisp[i] = nullptr; } }
  mDispN = 0;
  // ★只有"主屏就是 SSD1306"时才交还硬件 SPI(SSD1306 现在走独立 15/16, 不再占用 40/41)。
  //   ST7567 与 ST7789 都用硬件 SPI(同一个 gHwSpi); 二者也只在 40/41 上 CS 互相隔开, 不能并存显示,
  //   但作为"主屏"切换时只需换时钟频率, 不必 end()/begin()。
  if (mType == SCREEN_SSD1306) hwspiRelease();
  // ★再把"未选中屏"的片选钉到无效电平, 免得它跟着公共总线上的命令自己乱显示。
  deselectOtherCs(mType);

  // ---- 主屏(按 NVS 选定类型) ----
  U8G2* prim = nullptr;
  if (mType == SCREEN_SSD1306) {
    // SSD1306 128x64 【硬件 I2C】(独立脚 15/16, Wire1); 自发光无背光
    // ★2026-09-23: 传输层由"软件 I2C"换成"硬件 I2C"(见 ssd1306_hw_i2c_byte_cb) —— 软件 I2C
    //   整帧 ≈60ms, 逐页非原子提交会让跨页汉字在跑马灯时被水平劈开(撕裂); 硬件 I2C 整帧 ≈23ms。
    //   地址仍先扫 0x3C/0x3D 取真值(地址错=花屏)。
    uint8_t ssdAddr = ssd1306_probeAddress();   // Wire1 扫 15/16
    prim = new U8G2_SSD1306_128X64_NONAME_F_2ND_HW_I2C(rot);
    prim->getU8x8()->byte_cb = ssd1306_hw_i2c_byte_cb;   // 字节层换成 Wire1 硬件 I2C(显式 15/16)
    if (ssdAddr) u8x8_SetI2CAddress(prim->getU8x8(), (uint8_t)(ssdAddr << 1));
    mHasBL = false;                // SSD1306 自发光, 亮度靠对比度寄存器
    blOn = false;
    Serial.printf("[LCD] SSD1306 I2C 探测: %s @0x%02X (硬件 I2C1 @400kHz, 15/16)\n",
                  ssdAddr ? "命中" : "无应答", ssdAddr);
  } else if (IS_TFT_SCREEN(mType)) {
    // 彩屏(ST7789 / ILI9341)硬件 SPI: 自定义 U8g2 设备 + 公共硬件 SPI(横/竖屏几何由 stW()/stH() 决定)
    //   ★两者唯一的差别在"上电初始化序列"(u8x8 回调的 INIT -> tft_hw_init 按 gTftIli 分叉),
    //     这里创建对象的方式完全一致 —— 因为窗口/推屏/MADCTL/反显/主题都是共用的。
    // ★★后台的"旋转"在彩屏上怎么落地(2026-10-06 修)——
    //   旧版这里【一刀切强制 U8G2_R0 并忽略旋转】, 结果后台选旋转完全没反应。但那条禁令
    //   其实只对 90/270 成立: 软件旋转会让 U8g2 把宽高对调(320x240 <-> 240x320), 而
    //   tile_buf_height 是按当前方向算的 -> 分页错乱、画面花掉。
    //   正确做法是【把角度翻译成硬件本来就会的两件事】, 全程不碰 U8g2 的几何:
    //     ① 换方向: 0°/180° = 横屏 320x240;  90°/270° = 竖屏 240x320
    //        (竖屏靠 MADCTL 的 MV 位交换行列; 同时 tile 数由 30 变 40 —— 跟缓冲一致, 不错乱);
    //     ② 上下颠倒: 180°/270° 时把 MADCTL 的 MX|MY 取反(^0xC0), 在 stMadctlEff() 里做。
    //   于是四个角度都有明确效果, 而 U8g2 这一层永远 U8G2_R0。
    // ★★2026-10-11 方屏(240x240): 宽高相同, "换方向"没有意义 —— 故在方屏上【不碰
    //   gSt7789Portrait】(几何恒为 240x240), 旋转只走②(180°/270° 由 MX|MY 取反表达);
    //   方屏能这么简化, 是因为转 90° 在正方形上等价于"水平+垂直同时镜像"。
    const bool square   = (mType == SCREEN_ST7789_SQ);
    gSt7789Square = square;
    const bool wantPort = square ? false : (mRotation == 90 || mRotation == 270);
    const bool wantFlip = (mRotation == 180 || mRotation == 270);
    if (!square && wantPort != gSt7789Portrait) {
      gSt7789Portrait = wantPort;       // 与串口 O 共用同一状态(谁后改谁生效)
      st7789SavePrefs();                // 存 NVS
      bgLoad();                         // ★换方向要换背景图(横竖各存一张, 否则底图会错位)
    }
    sStMadFlip180 = wantFlip;
    prim = new U8G2();
    u8g2_Setup_st7789_f(prim, U8G2_R0);
    mHasBL = true;                 // 彩屏有背光脚 LCD2_BL(亮度=PWM 占空比)
    blOn = true;
    blLastActive = millis();
  } else {
    // ST7567 128x64 硬件 SPI: 用 U8g2 现成类, 字节层换成我们自己的 st7567_hwspi_byte_cb
    prim = new U8G2_ST7567_JLX12864_F_4W_HW_SPI(rot, LCD_CS, LCD_DC, LCD_RST);
    prim->getU8x8()->byte_cb = st7567_hwspi_byte_cb;
    mHasBL = true;                 // ST7567 有背光脚 LCD_BL(亮度=PWM 占空比)
    blOn = true;
    blLastActive = millis();
  }
  mDisp[0] = prim; mDispN = 1; u8g2 = prim;   // u8g2 兼容别名 = 主屏(单屏活动, 后台四选一)

  if (prim) {
    prim->begin();
    prim->enableUTF8Print();        // 允许 UTF-8 中文
    if (mType == SCREEN_ST7567) prim->setContrast(mContrast);  // 被动LCD非自发光, 必须设对比度
    prim->clearBuffer();
    prim->sendBuffer();
  }
  // ★背光必须放在 begin() 之后: begin() 会走各屏的硬件初始化(可能 pinMode 一堆脚), 背光脚
  //   一旦被 pinMode/digitalWrite 碰过就不再是 LEDC 输出(见文件顶部"背光 PWM")。
  blInit();
  mFrameValid = false;   // 重建后画面已被清空; 声明"当前画面未知", 强制下帧重画
  mBuiltType = mType;    // ★记录"现在真正驱动着的是哪块屏", 供下次切屏时把它关掉
}    // recreateU8g2()

// 背光初始化: 两路背光脚挂 LEDC(幂等) + 按当前屏/亮度重新点亮
void DisplayUI::blInit() {
  blPwmBegin();
  blApply();
}

// 把 blOn / mBright 立刻刷成 PWM 输出; 同时保证【另一路背光恒为 0%】——
// 三屏共用同一组信号总线(CLK/DAT/DC/RST)、但背光脚各自独立, 所以这里必须显式把
// "当前没选中的那一块屏"的背光关掉, 否则它会亮着旧画面(耗电 + 观感差)。
void DisplayUI::blApply() {
  const int pin = blPin();
  const int pct = (mHasBL && blOn && !mPoweredOff) ? mBright : 0;
  blPwm(pin, pct, blActiveHigh());
  if (pin == LCD_BL) blPwm(LCD2_BL, 0, LCD2_BL_ACTIVE_HIGH != 0);
  else               blPwm(LCD_BL,  0, LCD_BL_ACTIVE_HIGH  != 0);
}

void DisplayUI::begin() {
  loadSettings();

  Serial.printf("[LCD] 屏幕类型: %s\n",
                mType == SCREEN_SSD1306 ? "SSD1306 (硬件 I2C1 15/16)"
                : mType == SCREEN_ST7789_SQ ? "ST7789 240x240 方屏 (硬件 SPI)"
                : mType == SCREEN_ST7789 ? "ST7789 320x240 (硬件 SPI)"
                : mType == SCREEN_ILI9341 ? "ILI9341 (硬件 SPI)" : "ST7567 (硬件 SPI 128x64)");
  recreateU8g2();   // 内部按屏型配置背光脚并设 mHasBL
  // 开机自检图案窗口(见 config.h 的 LCD_BOOT_TEST_MS; 0 = 关闭 -> 恒为 0 表示"永不在窗口内")
  mBootTestUntil = LCD_BOOT_TEST_MS ? (millis() + (uint32_t)LCD_BOOT_TEST_MS) : 0;
  mBootTestDrawn = false;

  if (mType == SCREEN_ST7567) {
    Serial.printf("[LCD] 初始化完成: SCK=%d MOSI(SDA)=%d CS=%d DC=%d RST=%d BL=%d(亮=%s) 对比度=%d 亮度=%d%% 旋转=%s SPI=%luHz(硬件)\n",
                  LCD_SCK, LCD_MOSI, LCD_CS, LCD_DC, LCD_RST, LCD_BL,
                  LCD_BL_ACTIVE_HIGH ? "HIGH" : "LOW", mContrast, mBright, rotStr(mRotation),
                  (unsigned long)LCD_BUS_SPI_HZ_ST7567);
  } else if IS_TFT_SCREEN(mType) {
    Serial.printf("[LCD] 初始化完成: SCK=%d MOSI=%d CS=%d DC=%d RST=%d BL=%d(亮=%s) 亮度=%d%%(PWM %dHz/%dbit) 旋转=%s SPI=%luHz(硬件)\n",
                  LCD2_SCK, LCD2_MOSI, LCD2_CS, LCD2_DC, LCD2_RST, LCD2_BL,
                  LCD2_BL_ACTIVE_HIGH ? "HIGH" : "LOW", mBright, LCD_BL_PWM_FREQ, LCD_BL_PWM_BITS, rotStr(mRotation),
                  (unsigned long)LCD_BUS_SPI_HZ_ST7789);
    // ★诊断花屏/方向问题就靠这一行: 打出当前芯片/方向的分辨率与 MADCTL。
    //   横屏 320x240 要求 MV=1; 竖屏 240x320 要求 MV=0; 240x240 方屏无"横竖"概念 —— 串口 V/K 现场调。
    Serial.printf("[LCD] %s 面板: %s %dx%d, MADCTL=0x%02X(MV=%d MX=%d MY=%d BGR=%d), 反显=%d, 调色板 FG=0x%04X BG=0x%04X\n",
                  gTftIli ? "ILI9341" : (gSt7789Square ? "ST7789-240x240" : "ST7789"),
                  gSt7789Square ? "方屏" : (gSt7789Portrait ? "竖屏" : "横屏"), stW(), stH(), st7789MadctlGet(),
                  (st7789MadctlGet() >> 5) & 1, (st7789MadctlGet() >> 6) & 1,
                  (st7789MadctlGet() >> 7) & 1, (st7789MadctlGet() >> 3) & 1,
                  st7789InvGet() ? 1 : 0,
                  (unsigned)gSt7789Fg, (unsigned)gSt7789Bg);
    Serial.println(String("[LCD] 调色板主题: ") + st7789ThemeName() +
                   (gSt7789Square ? "  方向: 方屏无横竖(几何恒 240x240) | K 换 MADCTL | J 翻反显 | Q 换主题"
                                  : "  方向: 串口 V 切横/竖屏(存NVS) | K 换 MADCTL | J 翻反显 | Q 换主题"));
  } else {
    Serial.printf("[LCD] 初始化完成: SDA=%d SCL=%d 旋转=%s (SSD1306 自发光, 无背光)\n",
                  OLED_SDA, OLED_SCL, rotStr(mRotation));
  }
}

// ---- 显示设置: 后台下发入口(存 NVS + 置 pending, 由 tick() 应用) ----

void DisplayUI::setScreenType(int t) {
  // ★2026-10-09: 第 4 种屏型 ILI9341(与 ST7789 共用同一组引脚), 其余非法值一律回落默认(ST7789)。
  // ★2026-10-11: 再加 5 = ST7789 240x240 方屏(1.3寸) —— 与 3/4 共用引脚与驱动核心。
  //   屏型合法性统一走 IS_TFT_SCREEN(见 config.h), 以后再加分辨率只改那一处。
  //   ★不在这里做"值没变就跳过": 串口 C 命令正是靠"设成当前值"来强制重建一次屏幕(花屏急救)。
  //     网页端已在 web_server.cpp 里判过"变更才发", 不会因此多重建。
  if (!IS_TFT_SCREEN(t) && t != SCREEN_ST7567 && t != SCREEN_SSD1306) t = SCREEN_DEFAULT;
  mType = t;
  gTftIli = (t == SCREEN_ILI9341);   // 后续 stMadctl()/tftInv()/上电序列按它分叉
  mPendingType = t;              // 不能在 WS 异步上下文重建 u8g2, 交主循环 tick() 执行
  saveSettings();
  Serial.println(String("[LCD] 屏幕类型 -> ") +
                 (t == SCREEN_SSD1306 ? "SSD1306(I2C1 15/16)"
                  : t == SCREEN_ST7789_SQ ? "ST7789(SPI 240x240 方屏)"
                  : t == SCREEN_ST7789 ? "ST7789(SPI 320x240)"
                  : t == SCREEN_ILI9341 ? "ILI9341(SPI 320x240)" : "ST7567(SPI)") + " (已存NVS, 待重建)");
}

void DisplayUI::setContrastVal(int v) {
  if (v < 0) v = 0;
  if (v > 255) v = 255;
  mContrast = v;
  mPendingContrast = v;
  saveSettings();
  Serial.println("[LCD] 对比度 -> " + String(v) + " (已存NVS)");
}

void DisplayUI::setRotation(int deg) {
  if (deg != 0 && deg != 90 && deg != 180 && deg != 270) deg = 0;  // 非法角度回落 0°
  mRotation = deg;
  mPendingRot = deg;          // >=0 即表示"有待应用值"(含 0°), 由 tick() 折叠进 recreateU8g2
  saveSettings();
  Serial.println("[LCD] 旋转 -> " + String(deg) + "° (已存NVS)");
}

// 顺时针转 90° 循环(0 -> 90 -> 180 -> 270 -> 0)。红外遥控"屏幕旋转"键 / 按键复用入口。
// ★为什么只认彩屏(ST7789/ILI9341): 横竖两套排版 + 横竖两张背景图都是为它们做的; 128x64 单色屏转 90° 没有意义。
// ★这里只改角度, 真正的重建由 tick() 完成 —— 它会 recreateU8g2(), 而彩屏分支里已带
//   "方向变了就 bgLoad() 换该方向的背景图", 所以旋转后底图也会跟着切, 不用在这里重复调用。
void DisplayUI::rotateStep() {
  if (!IS_TFT_SCREEN(mType)) {
    Serial.println("[LCD] 屏幕旋转键: 当前屏不是彩屏(ST7789/ILI9341), 忽略");
    return;
  }
  const int next = (mRotation + 90) % 360;
  setRotation(next);      // mRotation 立即更新 -> 连续按两次可靠地到 180(不必等 tick 追上)
  Serial.printf("[LCD] 旋转键 -> %d°\n", next);
}

// 屏幕类型循环切换(红外"切换屏幕"键): ST7567(1) -> SSD1306(2) -> ST7789(3) -> ILI9341(4) -> ST7567(1) ...
// 与 rotateStep 同策略: 只推进 mType 并置 mPendingType, 真正重建 u8g2 交主循环 tick(),
//   所以从 Web/红外回调调用都安全(不会在异步上下文碰 u8g2)。
// ★用固定 1->2->3->4 顺序而不是"数组里找下一个": 屏型是固定集合, 顺序循环即可。
// ★注意 ST7789 与 ILI9341 是"同位置换屏"(共用同一组引脚), 循环到另一颗时屏上会是花屏/无显示 ——
//   那是正常的: 选中的芯片型号与实际插着的模块不符。想换屏请在后台"显示设置"里选对型号。
void DisplayUI::screenStep() {
  int next = mType + 1;
  if (next > SCREEN_ILI9341) next = SCREEN_ST7567;     // 4 -> 1 回绕
  setScreenType(next);
  Serial.printf("[LCD] 切换屏幕键 -> %d (%s)\n", next,
                next == SCREEN_SSD1306 ? "SSD1306(I2C1 15/16)"
                : next == SCREEN_ST7789 ? "ST7789(SPI 320x240)"
                : next == SCREEN_ILI9341 ? "ILI9341(SPI 320x240)" : "ST7567(SPI 128x64)");
}

void DisplayUI::setBLTimeout(int ms) {
  if (ms < 0 || ms > 3600000) ms = LCD_BL_TIMEOUT_MS;   // 上限 1h, 越界回落默认
  mBLTimeout = ms;
  saveSettings();
  blLastActive = millis();   // 重置计时起点, 避免刚改完立刻熄灭/迟迟不灭
  Serial.println("[LCD] 背光熄灭时间 -> " + String(ms) + "ms (已存NVS, 0=常亮)");
}

// 背光亮度 0~100%(PWM 占空比)。拖动滑块时立即生效, 不必等主循环 tick()。
// ★注意: 亮度是【背光 LED 的电流】维度, 与 ST7567/SSD1306 的"对比度寄存器"不是一回事;
//   ST7789 没有对比度寄存器, 它的"亮度"就只有这一个入口。
void DisplayUI::setBrightVal(int pct) {
  if (pct < 0)   pct = 0;
  if (pct > 100) pct = 100;
  mBright = pct;
  saveSettings();
  if (!mHasBL) {   // SSD1306 自发光: 无背光可调, 只回一句免得用户以为没生效
    Serial.println("[LCD] 当前屏(SSD1306)无背光, 亮度设置已存但无效; 请用对比度调明暗");
    return;
  }
  if (blOn) blApply();   // 灭灯期间只记住数值, 下次点亮(noteActivity)按新亮度亮
  Serial.println("[LCD] 背光亮度 -> " + String(pct) + "% (已存NVS)");
}

// 主循环每圈调用: 应用 pending 设置 + 背光计时
void DisplayUI::tick() {
  if (u8g2) {
    // 屏型变更: 先把待应用的旋转/对比度折叠进当前值, 再整体重建
    if (mPendingType >= 0) {
      if (mPendingRot >= 0)      { mRotation = mPendingRot; mPendingRot = -1; }
      if (mPendingContrast >= 0) { mContrast = mPendingContrast; mPendingContrast = -1; }
      mType = mPendingType;
      mPendingType = -1;
      recreateU8g2();       // 重建为新型号(含背光脚配置差异)
    }
    if (mPendingContrast >= 0) {
      u8g2->setContrast(mPendingContrast);
      mPendingContrast = -1;
    }
    if (mPendingRot >= 0) {
      mRotation = mPendingRot;          // recreateU8g2 按 mRotation 选旋转回调(R0/R1/R2/R3)
      mPendingRot = -1;
      recreateU8g2();           // U8g2 旋转仅构造期可设 -> 整体重建(含 begin/对比度恢复)
      // 重建后已清屏, 下个 show 周期(<=300ms)按新方向重画
    }
  }
  if (mHasBL && !mPoweredOff && blOn && mBLTimeout > 0 &&
      (millis() - blLastActive > (uint32_t)mBLTimeout)) {
    blOn = false;
    blApply();       // 输出 0% 亮度
  }
}

// 任意按键 -> 点亮背光并重置计时(仅 ST7567/ST7789 有背光; 亮度用 NVS 里的 mBright)
void DisplayUI::noteActivity() {
  if (mPoweredOff) return;                       // 彻底关机后不再因任意输入点亮背光
  // 从"熄屏"恢复: 清标志 + 强制重画(否则整帧跳过会停在空白帧), 再点亮背光
  if (mScreenOff) {
    mScreenOff = false;
    mFrameValid = false;
    // ★2026-10-07 彩屏: screenOff() 发过 DISPOFF(0x28) 让像素停止输出,
    //   唤醒必须补 SLPOUT(0x11)+DISPON(0x29), 否则屏幕一直是黑的(背光回来了也没画面)。
    if IS_TFT_SCREEN(screenType()) {
      hwspiAcquire(LCD_BUS_SPI_HZ_ST7789);
      st7789_tx(0x11, nullptr, 0); delay(120);   // SLPOUT: 退出休眠
      st7789_tx(0x29, nullptr, 0); delay(120);   // DISPON: 恢复像素输出
      st7789_fillBg();                // 顺带铺回当前底色(否则底色是上次的残影)
    }
  }
  if (!mHasBL) return;                            // SSD1306 自发光: 无背光脚, 仅恢复绘制
  blOn = true;      // ★必须先置位再 blApply: blApply 按 blOn 决定输出 0 还是 mBright
  blApply();
  blLastActive = millis();
}

// 关机: 清屏 + 关背光 + 置"已断电"标志。深度休眠前调用, 让屏幕彻底无显示。
void DisplayUI::powerOff() {
  mPoweredOff = true;
  if (u8g2) { u8g2->clearBuffer(); u8g2->sendBuffer(); }
  blOn = false;
  blApply();   // 两路背光都输出 0%(含"未选中屏"那一路), 深睡/关机后不残留背光耗电
  mFrameValid = false;   // 已清屏, 画面不再是上一帧内容
}

// 熄屏待机(睡眠定时器到点 / 电源键双击): 清屏 + 关背光, 但【不置 mPoweredOff】,
// 故任意按键经 noteActivity 仍可重新点亮(睡眠是"熄屏待机", 不是断电/深睡)。
// show/showList/showMessage 在 mScreenOff 期间一律跳过绘制, 避免下一帧又点亮 OLED / 又把内容画出来。
//
// ★2026-10-07 各种屏的"真正关屏"手段不同(电源键双击关屏用):
//   · SSD1306 OLED : 自发光, 无背光脚 -> 必须靠 clearBuffer 把像素全灭(本函数已做)
//   · ST7567 单色  : 非自发光, 只关背光即可 -> 背光灭 = 全黑(本函数已做)
//   · ST7789 彩屏  : ★背光+像素都发光, 只灭背光【屏幕仍在亮、只是变暗】,
//                   必须再发 ST7789 的 DISPOFF(0x28) 让像素真正停止输出;
//                   唤醒时补发 SLPOUT(0x11)+DISPON(0x29) 恢复。
//   · ILI9341 彩屏 : 同 ST7789 —— DISPOFF(0x28) 同码, 一并处理。
void DisplayUI::screenOff() {
  mScreenOff = true;
  mFrameValid = false;   // 唤醒时强制下一帧重画(否则整帧跳过会停在空白帧)
  if (u8g2) { u8g2->clearBuffer(); u8g2->sendBuffer(); }  // 立即清屏(熄灭可见像素)
  if IS_TFT_SCREEN(screenType()) {
    hwspiAcquire(LCD_BUS_SPI_HZ_ST7789);        // 关屏要发 SPI 命令, 必须先取总线
    st7789_tx(0x28, nullptr, 0); delay(60);     // DISPOFF: 像素停止输出(只灭背光是不够的)
  }
  blOn = false;
  blApply();
  Serial.println("[LCD] 已关屏(任意按键可重新点亮)");
}

// 手动翻转背光(串口 L): 用于测试背光极性与接线
void DisplayUI::toggleBacklight() {
  if (!mHasBL) { Serial.println("[LCD] 当前屏幕无背光脚(SSD1306 自发光), 忽略"); return; }
  blOn = !blOn;
  blLastActive = millis();          // 手动切换也算一次操作, 重置自动熄灭计时
  blApply();
  Serial.println("[LCD] 背光手动切换 -> " + String(blOn ? "亮" : "灭") + " (亮度 " + String(mBright) + "%)");
}

// 全屏测试图案(串口 S): 满屏黑块 + 反白文字/边框/对角线。
// 绕开网络与播放状态, 单独验证接线与初始化; 全屏黑 = 哪怕对比度
// 偏低也能在自然光下看出屏幕"全暗"。
void DisplayUI::testPattern() {
  u8g2->clearBuffer();
  const int W = u8g2->getWidth(), H = u8g2->getHeight();
  u8g2->setDrawColor(1);
  u8g2->drawBox(0, 0, W, H);               // 满屏点亮(黑块)
  u8g2->setDrawColor(0);                    // 反白(黑底上抠出亮字)
  u8g2->setFont(u8g2_font_6x10_tr);
  u8g2->drawStr(6, 20, "DISPLAY TEST");
  u8g2->drawFrame(2, 2, W - 4, H - 4);
  u8g2->drawLine(2, 2, W - 3, H - 3);
  u8g2->drawLine(W - 3, 2, 2, H - 3);
  u8g2->sendBuffer();
  Serial.println("[LCD] 测试图案已发送(满屏黑+反白TEST+对角线)。若屏无任何变化=接线/模式有问题");
  mFrameValid = false;   // 图案绕过了 show() 的正常流程, 声明"当前画面未知", 下帧必须重画
}

void DisplayUI::drawPlayIcon(U8G2& g, int x, int y, bool playing) {
  if (playing) {
    // 播放: 实心三角
    g.drawTriangle(x, y - 8, x, y, x + 8, y - 4);
  } else {
    // 暂停: 双竖条
    g.drawBox(x, y - 8, 3, 8);
    g.drawBox(x + 5, y - 8, 3, 8);
  }
}

// 充电闪电图标: 两个填充三角形拼成经典闪电(1bpp 屏上实心清晰)。
// 坐标: (x, yTop)=左上角, h=图标高度(像素), 宽度约为 h*0.6。
void DisplayUI::drawLightning(U8G2& g, int x, int yTop, int h) {
  const int m   = yTop + (h * 5) / 9;        // 中部拐点
  const int wU  = (h * 4) / 9;               // 上半宽
  const int wL  = (h * 6) / 9;               // 下半宽(略宽, 让底部有"分叉"感)
  g.drawTriangle(x + wU, yTop,  x,            m,  x + wU, m);                 // 上半(尖朝上, 左斜)
  g.drawTriangle(x + (h*3)/9, m,  x,         yTop + h,  x + wL, m - 1);      // 下半(接到基线)
}

void DisplayUI::show(const String& station, const String& status,
                     int volume, int volMax, bool playing, const String& bitrate) {
  if (mScreenOff) return;     // 睡眠熄屏期间不绘制(清屏已由 screenOff() 完成, 任意按键经 noteActivity 恢复)
  (void)volMax;
  // 开机前 LCD_BOOT_TEST_MS 内强制显示满屏测试图案(硬件自检: 一开机就能看出接线/初始化对不对,
  // 不受 WiFi/播放状态影响)。★只画一次并直接 return, 防止被常规内容覆盖, 也避免每圈重画整屏。
  // 默认 0 = 关闭(见 config.h), 此时下面判断恒假, 没有任何常态开销。
  if (millis() < mBootTestUntil) {
    if (!mBootTestDrawn) { mBootTestDrawn = true; testPattern(); }
    return;
  }

  // ==========================================================================
  // ★整帧跳过(2026-09-18 修"断断续续")
  // 本函数是主循环里最贵的一段, 实测【单次 56~63ms】, 成本构成:
  //   clearBuffer + cnWidth 逐字测宽 + drawCN 逐字绘制(每个汉字都要查 GB2312 字体表) + sendBuffer
  //   (软件 SPI 整屏 1024 字节)。
  // 而实际上: 电台名几乎不变、曲名几分钟才换一次、IP 不变 —— 绝大多数刷新周期画出来的画面
  // 与上一帧【一模一样】, 却照样把这 60ms 烧掉, 而且每 300ms 一次。
  // 后果(实测): 该台是 96kHz 的 HE-AAC, I2S DMA 深度只有 170ms; 这 60ms 的 CPU 独占叠上同圈
  // 解码的 27ms = 87ms —— 旧 DMA 深度(85ms)时【每次刷新都恰好排空 DMA】, 于是听感就是
  // 约 3.3 次/秒的规律性"断断续续"。
  // 对策: 先算出"最终会画到屏上的字符串"做指纹(字符串比较几乎免费), 一个都没变就整帧 return,
  // 连测宽与 sendBuffer 都省掉。内容真变了才付这 60ms。
  // 例外: 内容超宽要跑马灯滚动时必须连续刷, 否则文字会卡住不动(mMarqueeRun)。
  // ==========================================================================

  // ---- 配网(AP)模式: 显示引导信息, 而不是空播放界面 ----
  if (gState.apMode) {
    uint32_t ha = FRAME_HASH_SEED;
    mixStr(ha, "AP"); mixStr(ha, gState.apName); mixStr(ha, gState.ip);
    if (mFrameValid && ha == mFrameHash) { mSkipCnt++; return; }   // 内容没变 -> 整帧跳过
    mFrameHash = ha; mFrameValid = true; mMarqueeRun = false;
    u8g2->clearBuffer();
    const bool big = (u8g2->getHeight() >= 200);   // 大屏(320×240)用更松的行距
    drawCN(*u8g2, 0, big ? 70 : 16, "配网模式");
    drawCN(*u8g2, 0, big ? 110 : 38, "连此热点");
    u8g2->setFont(big ? u8g2_font_8x13_tr : u8g2_font_6x10_tr);
    u8g2->setCursor(0, big ? 180 : 52);
    u8g2->print(gState.apName);
    u8g2->setCursor(0, big ? 214 : 62);
    u8g2->print("192.168.4.1");
    u8g2->sendBuffer();
    return;
  }

  // ---- 中部第2行内容: 有曲名/栏目名就显示; 没有时按状态显示 ----
  // 换台后库会先攒够输入缓冲再出声(见 config.h「起播预缓冲」), 这段是【故意不出声】,
  // 屏上写"缓冲中"比"播放中"更能让人明白不是在卡住。
  // ★2026-09-22: 调用方显式给了 status(非空)就【优先用它】—— 用于临时提示(如"网络收音机"、
  //   缓冲/连接提示)。网络态传空串, 行为与改前完全一致。
  const char* info = status.length()
                        ? status.c_str()
                        : (gState.title.length()
                             ? gState.title.c_str()
                             : (gState.playing ? (gState.buffering ? "缓冲中" : "播放中") : "停止"));

  // 码率量化到 8k 粒度(库给的是 VBR 瞬时值, 原样上屏会让指纹每帧都变, 整帧跳过失效)
  // ★2026-09-22: 调用方显式给了 bitrate(非空)就原样上屏; 网络态传空串,
  //   于是仍走下面的 gState 组装 + 量化, 指纹稳定性不受影响。
  String codecStr;
  if (bitrate.length()) {
    codecStr = bitrate;
  } else {
    String br = gState.bitrate;   // ★2026-10-11 去重复制: 原来 brRaw 还要再拷一份给 br, 每次刷新白白多一次 String 分配
    if (br.endsWith("k")) {
      const int kv = br.substring(0, br.length() - 1).toInt();
      br = String((kv + 4) / 8 * 8) + "k";
    }
    // 编码 + 码率(底部条左): 如 "MP3 128k" / "AAC 128k"
    codecStr = gState.codec.length() ? (gState.codec + " " + br) : br;
  }

  // 音量百分比 / WiFi 信号强度百分比
  const int volPct  = (volMax > 0) ? (volume * 100 / volMax) : 0;
  const int wifiPct = gState.wifiConnected ? constrain(2 * ((int)WiFi.RSSI() + 100), 0, 100) : 0;
  const String wifiPart = gState.wifiConnected ? (String(wifiPct) + "%") : String("--%");
  // 睡眠定时器倒计时(阶段①): 仅运行时显示, 向上取整到分钟(归零那秒屏上已熄灭, 不必显示 0)
  const int sleepMinDisp = (gState.sleepRemainSec > 0) ? (int)((gState.sleepRemainSec + 59) / 60) : 0;
  // ★2026-10-07 显示单位随长短自适应(用户要求):
  //   不足 1 小时 -> "30分" / "45分"; 满 1 小时 -> "1时" / "1.5时" / "12时"。
  //   半小时这种中间档必须能显示"1.5时", 所以不能只写"X时Y分"。下面统一成一个字符串,
  //   三处显示(ST7567 电量行 / ST7789 电量行 / 单色屏音量行)都用它, 避免各写一套格式。
  String sleepDisp;
  if (sleepMinDisp > 0) {
    if (sleepMinDisp < 60) {
      sleepDisp = String(sleepMinDisp) + "分";
    } else {
      const int h = sleepMinDisp / 60, m = sleepMinDisp % 60;
      sleepDisp = m ? (String(h) + "." + String(m) + "时")   // 90 -> "1.5时"
                    : (String(h) + "时");                  // 60 -> "1时", 720 -> "12时"
    }
  }

  // ---- 内容指纹(整帧跳过): 内容没变就整帧 return, 省掉下面全部「逐字测宽+绘制+SPI 发送」 ----
  // ★2026-09-21 布局改为 4 行, 指纹覆盖「台名/信息/格式码率/WiFi/IP/音量/播放态」全部字段
  uint32_t h = FRAME_HASH_SEED;
  mixStr(h, station); mixStr(h, info); mixStr(h, codecStr);
  mixStr(h, wifiPart); mixStr(h, gState.ip);
  // 电量% + 充电态也进指纹, 否则电压/充电变化后屏幕不刷新
  const int battPct = constrain((int)((gState.batteryV - BATT_EMPTY_V) / (BATT_FULL_V - BATT_EMPTY_V) * 100.0f), 0, 100);
  h ^= (uint8_t)battPct;            h *= 16777619u;
  h ^= (uint8_t)(gState.charging ? 1 : 0); h *= 16777619u;
  h ^= (uint8_t)volPct;             h *= 16777619u;
  h ^= (uint8_t)(playing ? 1 : 0);  h *= 16777619u;
  // 缓冲态进指纹: 大屏布局里"缓冲中/播放中"是两行不同文字, 不进指纹就切换时刷不出来
  h ^= (uint8_t)(gState.buffering ? 1 : 0); h *= 16777619u;
  h ^= (uint8_t)sleepMinDisp;            h *= 16777619u;   // 睡眠倒计时(按分钟刷新)
  // ★时钟/农历/天气也进指纹(320x240 大屏要显示): 分钟一变、天气每小时一换,
  //   不进指纹整帧跳过就会一直停在开机那一眼。这里只做几次乘加, 成本可忽略。
  {
    struct tm tmn; timeLocal(&tmn);
    h ^= (uint8_t)tmn.tm_hour; h *= 16777619u;
    h ^= (uint8_t)tmn.tm_min;   h *= 16777619u;
    h ^= (uint8_t)tmn.tm_mday;  h *= 16777619u;
    h ^= (uint8_t)(gState.timeValid ? 1 : 0); h *= 16777619u;   // 校时成功那一刻要重画(--:-- -> 真实时间)
    h ^= (uint8_t)(gState.wx.valid ? 1 : 0); h *= 16777619u;
    h ^= (uint8_t)(int8_t)gState.wx.tempC;    h *= 16777619u;
    h ^= (uint8_t)(int8_t)gState.wx.humidity; h *= 16777619u;
    h ^= (uint8_t)(int8_t)gState.wx.tmax2C;   h *= 16777619u;
    mixStr(h, gState.wx.city); mixStr(h, gState.wx.text);
    mixStr(h, gState.wx.windDir); mixStr(h, gState.wx.windScale);
  }
  if (mFrameValid && h == mFrameHash && !mMarqueeRun) { mSkipCnt++; return; }   // ★整帧跳过: 省掉下面全部工作
  mFrameHash = h; mFrameValid = true; mDrawCnt++;

  u8g2->clearBuffer();

  // ---- 屏幕背景图(2026-10-06): 这里【什么都不用做】----
  // ★彩色背景不是"垫"进帧缓冲的, 而是在推屏时(DRAW_TILE)由 bit=0 的像素去查图得到的。
  //   所以:
  //     · 界面绘制路径一行没改 —— 帧缓冲里只有界面(该亮的地方亮), 与原来完全一致;
  //     · 没上传背景图时, DRAW_TILE 走"两个主题色常量"的老路, 逐像素代码等价, 开销为零;
  //     · 有图时底色变成全彩, 而界面该不该半透明由 sBgUiAlpha 在 DRAW_TILE 里决定。
  //   一句话: 背景图是完全独立的一层, 与 U8g2 的 1bpp 缓冲互不干扰。

  // ---- 自适应布局: 按当前屏分辨率计算行基线与底部小字号 ----
  const int W = u8g2->getWidth();
  const int H = u8g2->getHeight();
  const bool big = (H >= 200);                 // 320×240 彩色屏(ST7789) vs 128×64 单色屏

  // 两套固定基线(比按比例均分好看): 都是「第1-2行一组(中文, 行距 40px) / 第3-4行一组(小字, 行距 40px)」,
  // 组内紧凑、组间留白, 与 128×64 的设计思路一致。
  //   128×64 : 中文占上半(y17/35, 行距18) + 小字占下半(y51/62, 行距11)
  //   320×240: 64/中文字高16px -> 基线上移留顶部边距; 上组 60/100, 下组 180/220, 组间留 80px, 距底 20px
  const int yStation = big ? 60  : 17;
  const int yInfo    = big ? 100 : 35;
  const int yLine3   = big ? 180 : 51;
  const int yLine4   = big ? 220 : 62;
  const uint8_t* smallFont = big ? u8g2_font_8x13_tr : u8g2_font_5x7_tr;  // 底部两行小字

  // ---- 跑马灯推进: 只按【真实流逝时间】记账, 与画面帧率彻底解耦(2026-09-23 改) ----
  // 旧做法 = "每帧固定 +3px" + 一个独立的 150ms 闸门; 而 .ino 的画面节流也是 150ms ——
  //   两个同周期计时器相位漂移, 于是"有时两帧都不动、有时一帧跳 6px", 肉眼就是【一卡一卡】。
  // 新做法 = 纯时间积分: 20px/s ⇒ 每 50ms 走 1px。帧率再怎么抖(60~200ms), 位移都随时间
  //   线性推进、步长恒为 1px ⇒ 顺滑, 且速度恒定(与帧率无关, 不会因屏/负载变化而变速)。
  // ★2026-09-23 按屏分档(现场反馈: 7567 滚起来"看着眼晕", 而 1306 已验收正常) ——
  //   根因不在"撕裂"(7567 早已是硬件 SPI, 2.2ms/帧), 而在【液晶本身的像素响应时间】:
  //   ST7567 是 FSTN 单色液晶, 一个像素"黑<->白"翻完要 100~200ms; 而 20px/s 的滚动
  //   (= 每秒 20 次整行位移)会让每一个像素【永远停在翻转的半路】——半黑半白 = 灰。
  //   于是整行汉字糊成一团灰影、还拖尾, 越盯着看越晕。OLED(1306, 像素 µs 级)没有这个问题。
  //   对策: 7567 把位移速度减半 -> 8.3px/s, 并且节拍取【画面周期 60ms 的整数倍(120ms)】——
  //     每 2 帧正好走 1px, 位移绝对规律(不会像"90ms/px"那样 1px / 空一帧 / 1px 地打摆子),
  //     每个像素也有 120ms 去把那一次翻转走完 ⇒ 字迹重新变实、不再糊。
  //   ★为什么"速度"是唯一有效杠杆: 每秒翻转的像素总数 ∝ 滚动速度(与每步多大无关) ——
  //     所以"放大步长"救不了糊, 只有放慢速度才真正减少每个像素的翻转次数。步长恒为 1px 最顺滑。
  //   1306(OLED) / 7789(TFT) 响应是 µs~ms 级, 保持 20px/s 细步不动(1306 已确认顺滑不撕裂)。
  // ★2026-09-23 二次优化(现场: "眼晕有所改善, 但还是晕") —— 只放慢不够, 再补上【周期性静止】。
  //   依据(网上查到的两条通行结论):
  //     ① 持续不断的滚动【本身】就是不适来源 —— 无障碍/前庭指南(WCAG 2.2.2 等)一致要求动效可暂停;
  //        对人眼来说,"一直在动"比"动得快"更累, 给一段完全静止的间歇能显著缓解眩晕。
  //     ② 小字号跑马的实测舒适速度是 15~25px/s, 且两遍之间要有【明显间隔/停顿】否则"糊成一片";
  //        低于 ~30px/s 就开始显得滞 —— 所以"放慢"已到收益拐点, 必须换招(加停顿)。
  //   做法: 7567 每滚完一轮 -> 在【原地】静止 MQ_DWELL_MS 再滚下一轮:
  //     滚动几秒 → 静止 1.5s(零位移, 眼睛可歇、也能从容读开头那几个字) → 再滚 …
  //     ★静止期不累加位移余额, 否则解除静止的那一帧会"窜一大步"(又是突兀运动)。
  //     ★静止期停在【原地】、不"跳回开头": 跳回本身就是一次突兀位移, 正是要避免的东西。
  //   OLED(µs 级)/TFT(ms 级)不需要, 保持连续滚动(1306 已验收顺滑)。
  //   ★2026-09-23 7567 的现场反馈链(一路在降速):
  //     120ms(8.3px/s) → "有所改善但还是晕"; 180ms(5.6px/s) + 每轮静止 1.5s → "还是晕";
  //     期间试过"翻页交替"(完全不滚动、离散换页) → 现按要求【改回连续滚动】, 并把速度再降一档到 240ms/px。
  const bool is7567 = (mType == SCREEN_ST7567);
  // ★7567(FSTN 慢速液晶): 240ms/px = 4.2px/s(比上一版 180 再慢三分之一)。
  //   为什么取 240 而不是 200: 240 是画面周期 60ms 的【整数倍(4×)】⇒ 每 4 帧正好走 1px, 位移绝对规律;
  //   非整数倍(如 200ms)会出现 "1px → 空两帧 → 1px" 的打摆子, 反而更难看。
  const int MQ_MS_PER_PX = is7567 ? 240 : 50;    // 7567: 4.2px/s | 1306(OLED)/7789(TFT): 20px/s 不变
  const int MQ_DWELL_MS  = is7567 ? 1500 : 0;    // 7567: 每轮之间原地静止 1.5s; 0 = 不停(连续滚)
  {
    const uint32_t mqNow = millis();
    uint32_t mqDt = (marqueeT == 0) ? 0 : (mqNow - marqueeT);   // 首帧(marqueeT=0)不补偿
    if (mqDt > 200) mqDt = 200;     // 长时间没画(列表/提示/休眠唤醒)后不一次性补一大段
    marqueeT = mqNow;
    if (MQ_DWELL_MS && (int32_t)(mqNow - mqDwellUntil) < 0) marqueeMs = 0;   // 静止期: 不攒位移余额
    else                                                    marqueeMs += (int)mqDt;
  }
  int pxStep = marqueeMs / MQ_MS_PER_PX;          // 本帧应前进的像素数(通常 1, 偶尔 2)
  marqueeMs -= pxStep * MQ_MS_PER_PX;

  // ==========================================================================
  // ★ST7789 专用布局 —— 图片风格, 但【横屏/竖屏各一套】(2026-10-06 起用串口 O 切换)。
  //   ★卡片一律【只描边不填底纹】: 1bpp 屏上底纹打密了就是一片白糊, 还把画在上面的同色文字盖掉;
  //     强调只用"反白块"(setDrawColor(1) 画实心块 + setDrawColor(0) 挖字) —— 1bpp 唯一可靠手段。
  //   (下面这份描述是初版横屏设计的来由, 竖屏分支见 if (vert) 内注释)
  // ★ST7789(320x240)横屏布局 —— 按"深蓝底时钟/播放器"图片风格重做(2026-10-04)
  //   图片元素 -> 本机真实数据, 一一对应:
  //     顶部细条 : ▶/⏸ + 编码码率(左) | WiFi 信号%(右) + 一条分隔线   <- 图片的日期行/横线
  //     左上焦点 : 电台名(16px 中文, 超宽无缝跑马灯, 限定在左栏宽度内)
  //                                                                   <- 图片巨大时钟 17:27 的位置
  //     焦点下方 : 节目信息/标题(超宽跑马灯)                            <- 图片"湿度 31%、北风 5级"
  //     右上卡片 : "电量"标签 + 电量大数字(对应图片的 22°C) + 闪电/充电状态  <- 图片天气盒
  //     底部面板 : 第1行 播放状态(左)|音量%(右)
  //                第2行 音量滑条(轨道+底纹填充+白色旋钮)
  //                第3行 IP(左) | 电量(右)   ←2026-10-07 去掉行首"配置"二字, 用户反馈这行太挤
  //   小屏(128x64 / 240 以下高)不进这里, 沿用下面原有的四行布局, 行为完全不变。
  // ==========================================================================
  if (big) {
    // ★联网数据只在这里【读】, 绝不在绘图路径发网络请求 ——
    //   采集由主循环 timeInit()/weatherTick() 负责(开机一次 + 每小时一次)。
    struct tm tmNow;
    timeLocal(&tmNow);
    const bool timeOk = gState.timeValid;        // ★NTP 没校上就不画假时间(开机初期显示 --:--)
    const LunarInfo lun = lunarFromSolar(tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday);
    const WeatherInfo& wx = gState.wx;
    const bool hasWx = wx.valid;
    const int  MGAP  = 8;                        // 跑马灯两遍之间的间隔(px)
    const bool vert  = (H > W);                  // true = 竖屏 240x320; false = 横屏 320x240
    elemZoneReset();    // ★每帧开头清空"行->元素"分区表(未登记的行回落主题前景色)

    // ---- 顶栏 ----
    // ★240x240 方屏也走下面"日期靠左 + 农历靠右"这一排, 但只有 240 宽 -> 按实际宽度自动降级。
    // ★竖屏(240 宽): 日期与农历挤不进同一排 -> 排成【整齐的两排, 且两排都居中】。
    //   旧版是"第1排靠左、第2排靠右", 视觉上就是错开的(用户实拍反馈"看着别扭")。
    //   选居中的理由: 下方时钟/湿度行/天气卡/底部面板全是居中的, 顶栏跟着居中才统一。
    // ★横屏(320 宽): 空间够, 保持"日期靠左 + 农历靠右"的同一排(原样不动)。
    char lb[40];
    // ★tm_mon 是 0~11(0=1月), 必须 +1 才是真实月份; 直接打印会少一个月(10月显示成9月)
    if (timeOk) snprintf(lb, sizeof(lb), "%d月%d日 %s", tmNow.tm_mon + 1, tmNow.tm_mday, WEEKDAY_CN[tmNow.tm_wday]);
    else        snprintf(lb, sizeof(lb), "时间未同步");   // 等 NTP 成功后自动变回日期
    drawCN(*u8g2, vert ? (W - cnWidth(*u8g2, lb)) / 2 : 6, 18, lb);
    if (timeOk && lun.ok) {                      // 未校时不显示农历(此刻的日期是假的)
      char rb[48];
      snprintf(rb, sizeof(rb), "%s%s年%s", lun.ganZhiYear, lun.zodiac, lun.monthDay);
      if (vert) drawCN(*u8g2, (W - cnWidth(*u8g2, rb)) / 2, 40, rb);
      else {
        // ★2026-10-11: 240x240 方屏放不下"日期 + 完整农历"同一排(两者相加约 232px, 只剩 8px 余量)。
        //   放不下就自动降级成只显示月日("八月二十"), 保证两段文字绝不叠在一起。
        const int dateR = 6 + cnWidth(*u8g2, lb) + 8;          // 日期右边界 + 至少 8px 间距
        if (W - 6 - cnWidth(*u8g2, rb) < dateR) snprintf(rb, sizeof(rb), "%s", lun.monthDay);
        const int rw = cnWidth(*u8g2, rb);
        if (W - 6 - rw >= dateR) drawCN(*u8g2, W - 6 - rw, 18, rb);   // 仍放不下就不画(绝不叠字)
      }
    }
    // ---- 睡眠倒计时徽标(★仅竖屏; 2026-10-07 由底部面板搬来) ----
    // ★为什么搬: 竖屏底部面板第 3 行是"电量:80%(左) + 本机 IP(右)"。实测竖屏(240 宽)
    //   面板内可用只有 204px(px0+12..px1-12), 而 "电量:80%"≈64px + IP(如 192.168.1.100)
    //   ≈104px, 加间距已用掉 ≈172px —— 只剩 ~32px。再往后缀挂 " 眠1.5时"(≈52px) 必然
    //   压到 IP 上(用户反馈"IP 和电量就已经占完位置了")。横屏(320 宽)内宽 276px, 后缀挂得下,
    //   所以【只改竖屏, 横屏保持原样】。
    // ★为什么放顶栏右上角: 顶栏日期是【居中】绘制, 最宽也只占中间 ≈100px(右边界 ≈170),
    //   右边还空 ≈64px; 徽标 "眠1.5时" 最长 ≈52px, 右对齐到 W-6 正好放得下, 且不会碰日期。
    //   视觉上就是个"状态角标", 与手机状态栏同理; 底部面板原有内容一行不动。
    if (vert && sleepMinDisp > 0) {
      const String sb = "眠" + sleepDisp;
      drawCN(*u8g2, W - 6 - cnWidth(*u8g2, sb.c_str()), 18, sb.c_str());
    }
    u8g2->drawHLine(0, vert ? 47 : 26, W - 1);

    // ==================== 竖屏 240x320 ====================
    // 竖直方向够长 -> 大时钟 + 天气卡片 + 播放面板 三段式, 每段一张【纯描边】卡片。
    if (vert) {
      // ---- 大时钟(居中; 字号按可用宽高自动挑, 实测能上到 62px) ----
      { char cb[16];
        if (timeOk) {
          fmtClock12(cb, sizeof(cb), tmNow.tm_hour, tmNow.tm_min);
          drawClock12(*u8g2, cb, tmNow.tm_hour >= 12, 8, W - 8, 50, 120);
        } else {
          drawClock(*u8g2, "--:--", 8, W - 8, 50, 120);          // 未校时: 占位, 不带 AM/PM
        } }
      // ---- 时钟下方: 湿度 / 风向风力 ----
      u8g2->setFont(u8g2_font_8x13_tr);
      if (hasWx) {
        String sc = wx.windScale;
        if (sc.length() && !sc.endsWith("级")) sc += "级";
        char wb[56];
        snprintf(wb, sizeof(wb), "湿度 %d%%、%s %s", wx.humidity, wx.windDir.c_str(), sc.c_str());
        drawCN(*u8g2, (W - cnWidth(*u8g2, wb)) / 2, 138, wb);
      } else {
        const char* wb = "湿度 --%、--风 --级";
        drawCN(*u8g2, (W - cnWidth(*u8g2, wb)) / 2, 138, wb);
      }
      // ---- 天气卡片(纯描边; 城市与天气并一行 + 大温度 + 明日温区) ----
      // ★2026-10-06 重排: 卡片只有 101px 高, 旧版把温度 baseline 放在 cy0+76(226),
      //   而"明天"行在 cy0+96(246) -> 字形顶 230 与温度的底 234 直接叠上。
      //   现在三行的字形区间是 156~172 / 188~220 / 224~240, 互不重叠且都在卡片(150~250)内。
      const int cx0 = 6, cy0 = 150, cx1 = W - 6, cy1 = 250;
      const int cxL = cx0 + 14, cxR = cx1 - 10;
      u8g2->drawRFrame(cx0, cy0, cx1 - cx0 + 1, cy1 - cy0 + 1, 8);
      if (hasWx) {
        { char t1[64];
          snprintf(t1, sizeof(t1), "%s %s", wx.city.c_str(), wx.text.c_str());
          drawCNLimit(*u8g2, cxL, cy0 + 22, t1, cxR); }
        char tb[16]; snprintf(tb, sizeof(tb), "%d", wx.tempC);
        u8g2->setFont(u8g2_font_logisoso32_tn);
        u8g2->setCursor(cxL, cy0 + 62); u8g2->print(tb);
        const int tw = u8g2->getUTF8Width(tb);
        u8g2->setFont(u8g2_font_8x13_tr);
        drawCN(*u8g2, cxL + tw + 1, cy0 + 62, "°C");
        char fb[32];
        snprintf(fb, sizeof(fb), "明天 %d/%d°C", wx.tmax2C, wx.tmin2C);
        drawCNLimit(*u8g2, cxL, cy0 + 90, fb, cxR);
      } else {
        drawCN(*u8g2, cxL, cy0 + 52, "天气未取");
      }
      // ---- 底部播放面板(纯描边) ----
      const int px0 = 6, py0 = 256, px1 = W - 6, py1 = 318;
      u8g2->drawRFrame(px0, py0, px1 - px0 + 1, py1 - py0 + 1, 8);
      // 第1行: 【电台名 + 节目信息(曲名) 合并成一条滚动】 | 播放状态(2 字, 右对齐)
      // ★2026-10-06 按要求合并: 以前本行只画 station, 于是竖屏下 title(曲名/节目名)根本没地方
      //   显示 —— 用户只看得见台名。现在拼成 "台名 - 曲名" 一起跑马灯; title 为空时退化成只有
      //   台名, 不会留一个孤零零的分隔符。
      // ★宽度是算好的(竖屏面板内宽只有 204px, 必须抠):
      //     面板 x 6..234 -> 内边界 18..222 = 可用 204px
      //     状态框 2 个汉字(32px) + 左右各 5px 内边距 = 42px, 右对齐到 222 -> 左沿 180
      //     台名右边界 = 180 - 6(间距) = 174   =>  ★跑马灯可用宽 = 174 - 18 = 156px(约 9.7 个汉字)
      //   ★状态文字由"播放中/缓冲中"缩成"播放/缓冲"就是为了多挤出这 16px; 语义不变(播放器语境下
      //     "播放"已经足够明确), 而跑马灯每屏能多显示一个字。
      const char* ss = playing ? (gState.buffering ? "缓冲" : "播放") : "停止";
      const int bxw = cnWidth(*u8g2, ss) + 10;                  // 状态框总宽(2 字 = 42px)
      const int SW  = (px1 - 12) - (px0 + 12) - bxw - 6;        // 跑马灯可用宽(156px)
      String line1 = station;
      if (gState.title.length()) { line1 += " - "; line1 += gState.title; }
      const int sw1 = cnWidth(*u8g2, line1.c_str());
      const int stMaxX = px0 + 12 + SW;                         // 硬右边界: 不许滚进状态框
      if (sw1 <= SW) drawCN(*u8g2, px0 + 12, py0 + 20, line1.c_str());
      else {
        const int period = sw1 + MGAP;
        marqueeOff = (marqueeOff + pxStep) % period;
        // ★2026-10-06 修"滚动时字从面板左边框外面露出来":
        //   drawCNLimit 只卡了右边界(stMaxX), 而跑马灯向左滚出时起点 (px0+12-marqueeOff) 会
        //   变成【负数】—— 负坐标经 u8g2_uint_t 转换后是个大数, 旧的 user 窗口是整屏, 于是
        //   第一个字被画到面板外面(用户实拍反馈)。
        //   这里给面板内部开一个【裁剪窗口】: U8g2 的 setClipWindow 会立即重算 user_x0/x1
        //   (u8g2_setup.c:u8g2_apply_clip_window), 之后所有绘制都被像素级裁在框内 ——
        //   而回绕后的大坐标会被 u8g2_clip_intersection2 的 "a >= d -> return 0" 干净拒掉,
        //   不会画出乱像素。左沿取 px0+1(压在边框内), 右沿用 stMaxX(不侵犯状态框)。
        u8g2->setClipWindow(px0 + 1, py0 + 1, stMaxX, py1 - 1);
        drawCNLimit(*u8g2, px0 + 12 - marqueeOff,          py0 + 20, line1.c_str(), stMaxX);
        drawCNLimit(*u8g2, px0 + 12 + period - marqueeOff, py0 + 20, line1.c_str(), stMaxX);
        u8g2->setMaxClipWindow();     // ★必须恢复: 否则紧接着的状态框/音量条/电量行也会被裁掉
      }
      // ★2026-10-06 按实拍反馈改: "播放中"原来的反白实心块=一块白底, 用户明确说【不要有背景】。
      //   改成空心描边 —— 既没有填充底色, 又和整屏"卡片只描边不填充"的风格统一。
      { const int sw = cnWidth(*u8g2, ss);
        const int sx = px1 - 12 - sw;
        if (playing) u8g2->drawRFrame(sx - 5, py0 + 4, sw + 10, 20, 3);
        drawCN(*u8g2, sx, py0 + 20, ss); }
      // 第2行: 音量滑条(实心填充 = 当前音量) + 音量%
      // ★行距是算过的: 第1行字底 py0+20(276), 滑条 py0+26..py0+38(282~294),
      //   第3行字顶 py1-22(296) —— 三行互不重叠(竖屏面板只有 62px 高, 挤不下就撞)。
      { const int sx0 = px0 + 52, sbw = px1 - 12 - 34 - sx0, sby = py0 + 26;
        drawCN(*u8g2, px0 + 12, sby + 11, "音量");
        u8g2->drawFrame(sx0, sby, sbw, 12);
        const int fw = (sbw - 6) * volPct / 100;
        if (fw > 0) u8g2->drawBox(sx0 + 3, sby + 3, fw, 6);      // 实心填充(不再用稀疏底纹)
        const String vs = String(volPct) + "%";
        u8g2->setFont(u8g2_font_8x13_tr);
        u8g2->setCursor(px1 - 12 - u8g2->getUTF8Width(vs.c_str()), sby + 11); u8g2->print(vs); }
      // 第3行: 电量(左, 充电中加闪电) | 本机 IP(右)
      // ★2026-10-07 此行【不再挂睡眠后缀】: 睡眠倒计时已搬到顶栏右上角(见上面注释)。
      //   竖屏 240 宽下 "电量:80%"+"IP" 已占满整行, 没有多余空间。
      { char eb[40]; snprintf(eb, sizeof(eb), "电量:%d%%", battPct);
        if (gState.charging) drawLightning(*u8g2, px0 + 12, py1 - 20, 13);
        drawCN(*u8g2, px0 + 12 + (gState.charging ? 18 : 0), py1 - 6, eb); }
      u8g2->setFont(u8g2_font_8x13_tr);
      u8g2->setCursor(px1 - 12 - u8g2->getUTF8Width(gState.ip.c_str()), py1 - 6);
      u8g2->print(gState.ip);

      // ---- 元素配色分区(竖屏大屏): 顶栏 / 大时钟 / 天气(湿度行+卡片) / 台名行 / 音量行 / 电量|IP ----
      elemZone(ELEM_TOPBAR,  0, 47);                       // 顶栏(日期/农历/睡眠徽标) + 分隔线
      elemZone(ELEM_CLOCK,   48, 120);                     // 大时钟(区间内居中, 不会压到湿度行)
      elemZone(ELEM_WEATHER, 121, 252);                    // 湿度风向行(基线138->124..140) + 天气卡片
      // ★文字行一律用 elemZoneText(传基线)自动按字形覆盖(边界-14..+2), 手拍行区间会把边界
      //   切进字形里 -> 同一行字上半截/下半截两个颜色(实测: "音量"两字顶 2 行串成台名色)。
      elemZoneText(ELEM_STATION, py0 + 20);                // 电台名 + 节目信息(基线 276 -> 262..278)
      elemZoneText(ELEM_VOLBAR,  py0 + 37);                // 音量滑条 + 音量%(基线 293 -> 279..295)
      { const int ipw = u8g2->getUTF8Width(gState.ip.c_str());
        elemZoneSplit(ELEM_BATT, ELEM_IP, px1 - 12 - ipw - 2, py1 - 22, py1 - 2); }  // 电量(左) | IP(右)

      // ---- 装饰线框统一归 ELEM_FRAME(不再被"所在行元素色"污染) ----
      // ★圆角框必须传半径 r(drawRFrame 末参), 否则四个转角的圆弧像素既不在顶/底行区间
      //   也不在左右竖列上 -> 露出一小截主题白。端点按 drawFrame/drawRFrame 的【端点】算。
      elemFrameEdge(6, 150, W - 6, 250, 8);                       // 天气卡片边框(r=8)
      elemFrameEdge(6, 256, W - 6, 318, 8);                       // 底部播放面板边框(r=8)
      elemFrameEdge(px0 + 52, py0 + 26, px1 - 47, py0 + 37);      // 音量条外框(drawFrame 直角, r=0)
      { const int sw2 = cnWidth(*u8g2, ss);                       // 播放/缓冲状态小框: 与 drawRFrame 同几何
        elemFrameEdge(px1 - 17 - sw2, py0 + 4, px1 - 8, py0 + 23, 3); }
      elemFrameEdge(0, 47, W - 1, 47);                            // 顶栏分隔线

      mMarqueeRun = (sw1 > SW);                        // 本帧在滚动? 是则下帧不可跳过
      u8g2->sendBuffer();
      return;
    }

    // ==================== 横屏 320x240 / 方屏 240x240 ====================
    // ★2026-10-11: 240x240 方屏【直接复用这套排版】—— 它竖直空间同样是 240, 与横屏一致,
    //   差别只在宽度(320 -> 240)。故下面所有"右边界"一律由 W 推导, 方屏自动收窄 80px,
    //   功能一项不减; 只有"天气卡宽度"和"湿度行"按可用宽自适应(见对应注释)。
    // ---- 右上天气卡片(纯描边, 不填底纹 —— 1bpp 上填密了就是一片白糊还盖掉文字) ----
    // ★2026-10-06 重排: 卡片只有 121x101, 旧版把"城市/天气"分成两行、温度顶到 cy0+82,
    //   最后一行 cy0+100(=130) 直接越过卡片下沿(126) —— 屏上就是"明天 0/0°C 跑出框外"。
    //   现在城市与天气并作一行, 三行字都在卡片内(50 / 64~96 / 102~118, 下沿 128)。
    //   ★末行一律走 drawCNLimit 兜右边界: 温度出现 -22 这种三位数也不会顶穿边框。
    //   ★卡片宽恒为 121(内容按这个宽度排过版, 别再改): 320 屏上落在 196..316, 方屏落在 116..236。
    const int cx1 = W - 4, cx0 = cx1 - 120, cy0 = 28, cy1 = 128;
    const int cxL = cx0 + 9, cxR = cx1 - 9;                      // 卡片内容左右安全边界
    const int colR = cx0 - 4;                                    // 左栏(大时钟/湿度)的右边界
    u8g2->drawRFrame(cx0, cy0, cx1 - cx0 + 1, cy1 - cy0 + 1, 8);
    if (hasWx) {
      { char t1[64];
        snprintf(t1, sizeof(t1), "%s %s", wx.city.c_str(), wx.text.c_str());  // 城市 + 天气并一行
        drawCNLimit(*u8g2, cxL, cy0 + 22, t1, cxR); }
      char tb[16];
      snprintf(tb, sizeof(tb), "%d", wx.tempC);
      u8g2->setFont(u8g2_font_logisoso32_tn);                   // 大数字(对应图片的 22°C)
      u8g2->setCursor(cxL, cy0 + 60); u8g2->print(tb);
      const int tw = u8g2->getUTF8Width(tb);
      u8g2->setFont(u8g2_font_8x13_tr);
      drawCN(*u8g2, cxL + tw + 1, cy0 + 60, "°C");
      char fb[32];
      snprintf(fb, sizeof(fb), "明天 %d/%d°C", wx.tmax2C, wx.tmin2C);
      drawCNLimit(*u8g2, cxL, cy0 + 90, fb, cxR);                // 90 -> 118 < 128, 回到框内
    } else {
      drawCN(*u8g2, cxL, cy0 + 46, "天气未取");                  // 没网 / 还没取到
    }

    // ---- 左侧大时钟(左栏 4..colR, 字号自动挑到最大; 320 屏实测 42px -> 62px, 方屏收窄到 ~26px) ----
    { char cb[16];
      if (timeOk) {
        fmtClock12(cb, sizeof(cb), tmNow.tm_hour, tmNow.tm_min);
        drawClock12(*u8g2, cb, tmNow.tm_hour >= 12, 4, colR, 30, 112);
      } else {
        drawClock(*u8g2, "--:--", 4, colR, 30, 112);             // 未校时: 占位, 不带 AM/PM
      } }
    // ---- 时钟下方: 湿度 / 风向风力(对应图片"湿度 31%、北风 5级") ----
    // ★2026-10-11: 240x240 方屏的左栏只有 108px 宽, 完整串(约 150px)会压到右边的天气卡上 ——
    //   故这里【按可用宽自动降级】: 放得下就整串, 放不下就只留湿度。320 屏行为完全不变。
    u8g2->setFont(u8g2_font_8x13_tr);
    if (hasWx) {
      String sc = wx.windScale;
      if (sc.length() && !sc.endsWith("级")) sc += "级";
      char wb[56];
      snprintf(wb, sizeof(wb), "湿度 %d%%、%s %s", wx.humidity, wx.windDir.c_str(), sc.c_str());
      if (cnWidth(*u8g2, wb) > (colR - 4)) snprintf(wb, sizeof(wb), "湿度 %d%%", wx.humidity);
      drawCN(*u8g2, (colR - cnWidth(*u8g2, wb)) / 2, 130, wb);
    } else {
      const char* wb = "湿度 --%、--风 --级";
      if (cnWidth(*u8g2, wb) > (colR - 4)) wb = "湿度 --%";
      drawCN(*u8g2, (colR - cnWidth(*u8g2, wb)) / 2, 130, wb);
    }

    // ---- 底部大面板(纯描边) ----
    const int px0 = 8, py0 = 140, px1 = W - 8, py1 = 238;
    u8g2->drawRFrame(px0, py0, px1 - px0 + 1, py1 - py0 + 1, 10);
    // 第1行: 【电台名 + 节目信息(曲名) 合并成一条滚动】(左) | 播放状态(2 字, 右)
    // ★2026-10-06 与竖屏同步合并(以前这里也只画 station, title 同样没显示)。
    // ★宽度: 面板 x 8..312 -> 内边界 22..298 = 可用 276px
    //         状态框 2 字 = 42px, 右对齐 298 -> 左沿 256
    //         ★跑马灯可用宽 = 256 - 6 - 22 = 228px(约 14 个汉字) —— 横屏宽裕, 通常不用滚
    const char* ss = playing ? (gState.buffering ? "缓冲" : "播放") : "停止";
    const int bxw = cnWidth(*u8g2, ss) + 10;                 // 状态框总宽(2 字 = 42px)
    const int SW  = (px1 - 14) - (px0 + 14) - bxw - 6;       // 跑马灯可用宽(228px)
    String line1 = station;
    if (gState.title.length()) { line1 += " - "; line1 += gState.title; }
    const int sw1 = cnWidth(*u8g2, line1.c_str());
    const int stMaxX = px0 + 14 + SW;                        // 硬右边界: 不许滚进状态框
    if (sw1 <= SW) drawCN(*u8g2, px0 + 14, py0 + 26, line1.c_str());
    else {
      const int period = sw1 + MGAP;
      marqueeOff = (marqueeOff + pxStep) % period;
      // ★2026-10-06 同竖屏: 跑马灯起点会变负 -> 只卡右边界不够, 文字会从左边框外面露出去。
      //   开裁剪窗口把整行关在面板内(原理与取值见竖屏那段注释)。
      u8g2->setClipWindow(px0 + 1, py0 + 1, stMaxX, py1 - 1);
      drawCNLimit(*u8g2, px0 + 14 - marqueeOff,          py0 + 26, line1.c_str(), stMaxX);
      drawCNLimit(*u8g2, px0 + 14 + period - marqueeOff, py0 + 26, line1.c_str(), stMaxX);
      u8g2->setMaxClipWindow();       // ★必须恢复, 否则后面的状态框/音量行一起被裁
    }
    // ★2026-10-06 按实拍反馈改: "播放中"原来的反白实心块=一块白底, 用户明确说【不要有背景】。
    //   改成空心描边 —— 既无填充底色, 又与整屏"卡片只描边不填充"的风格一致。
    { const int sw = cnWidth(*u8g2, ss);
      const int sx = px1 - 14 - sw;
      if (playing) u8g2->drawRFrame(sx - 5, py0 + 10, sw + 10, 20, 3);
      drawCN(*u8g2, sx, py0 + 26, ss); }
    // 第2行: 音量滑条(实心填充 = 当前音量) + 音量%
    // ★2026-10-11: 长度由"满宽 188"改成【按当前屏宽推导】—— 320 屏仍是 188(312-68-56),
    //   方屏自动收窄到 108, 右侧那句"音量%"始终贴右内边界, 不会被挤出面板。
    //   (音量的 vbx/vbw 提到块外: 下面登记 ELEM_FRAME 框色时还要用同一组几何。)
    const int vbx = px0 + 48, vbw = px1 - 68 - vbx, vby = py0 + 48;
    { drawCN(*u8g2, px0 + 14, vby + 13, "音量");
      u8g2->drawFrame(vbx, vby, vbw, 16);
      const int fw = (vbw - 8) * volPct / 100;
      if (fw > 0) u8g2->drawBox(vbx + 4, vby + 4, fw, 8);   // 实心填充(不再是稀疏底纹)
      const String vs = String(volPct) + "%";
      u8g2->setFont(u8g2_font_8x13_tr);
      u8g2->setCursor(px1 - 14 - u8g2->getUTF8Width(vs.c_str()), vby + 13); u8g2->print(vs); }
    // 第3行: IP(左) | 电量(右, 充电中加闪电)
    // ★2026-10-07 去掉行首的"配置"二字: 用户反馈这一行挤。原来是"配置 + IP"并排,
    //   两个汉字(2x14=28px)纯属冗余 —— 第4行已经是"IP + 电量", 这里再带个"配置"标签
    //   对读屏没有任何帮助, 却把 IP 往右挤。删掉后 IP 从行首起画, 左边多出 42px 余量。
    u8g2->setFont(u8g2_font_8x13_tr);
    u8g2->setCursor(px0 + 14, py1 - 12); u8g2->print(gState.ip);
    { char eb[40]; snprintf(eb, sizeof(eb), "电量:%d%%", battPct); if (sleepMinDisp > 0) { strncat(eb, (" 眠" + sleepDisp).c_str(), sizeof(eb) - strlen(eb) - 1); }
      const int ew = cnWidth(*u8g2, eb);
      // ★起点 py1-26: "电量"字形顶 = 基线(py1-12) - 14 = py1-26, 用 py1-24 会让顶 2 行串色。
      elemZoneSplit(ELEM_IP, ELEM_BATT, px1 - 14 - ew - 2, py1 - 26, py1 - 2);  // IP(左) | 电量+睡眠(右)
      if (gState.charging) drawLightning(*u8g2, px1 - 24 - ew, py1 - 24, 13);
      drawCN(*u8g2, px1 - 14 - ew, py1 - 12, eb); }

    // ---- 元素配色分区(横屏大屏) ----
    // ★★两份分区表: 竖屏一套 / 横屏一套, 同一块 UI【两边都要登记】—— 只登记竖屏的话,
    //   横屏那块会落到"未登记"用主题前景色(白), 表现为"元素全设黑色后仍有漏网之鱼"。
    elemZone(ELEM_TOPBAR,  0, 27);                            // 顶栏(日期 + 农历)
    // ★时钟与天气卡【同一段行区间、左右分列】, 故用 Split 按 x=196(卡片左沿)拆开各取各的颜色。
    elemZoneSplit(ELEM_CLOCK, ELEM_WEATHER, cx0, cy0, cy1);   // 左=大时钟(4..190) | 右=天气卡(196..316)
    // ★湿度行基线 130 -> 字形 116..132; 必须在"时钟/天气卡拆分"【之后】登记, 把 116~128 改回整行天气色
    //   (否则那两行会按 x<196 取【时钟色】—— 屏上就是"湿度字头是黑的、身子是红的")。
    elemZoneText(ELEM_WEATHER, 130);                          // 湿度/风向行(在时钟下方, 整行归天气)
    elemZone(ELEM_STATION, py0 + 8,  py0 + 28);               // 电台名+节目信息(合并跑马灯)与状态框
    elemZone(ELEM_VOLBAR,  py0 + 40, py0 + 66);               // 音量滑条 + 音量%

    // ---- 装饰线框统一归 ELEM_FRAME ----
    // ★圆角框必须把【圆角半径】传进来(r = drawRFrame 末参); 端点按 drawFrame 端点算:
    //   音量条 bx=56 bw=188 -> 右沿 243(不是 244); 状态小框右沿 px1-10、底沿 py0+29。
    elemFrameEdge(cx0, cy0, cx1, cy1, 8);                      // 天气卡片边框(r=8)
    elemFrameEdge(px0, py0, px1, py1, 10);                     // 底部播放面板边框(r=10)
    elemFrameEdge(vbx, vby, vbx + vbw - 1, vby + 15);          // 音量条外框(drawFrame 直角, r=0)
    { const int sw2 = cnWidth(*u8g2, ss);                      // 播放/缓冲状态小框: 与 drawRFrame 同几何
      elemFrameEdge(px1 - 19 - sw2, py0 + 10, px1 - 10, py0 + 29, 3); }
    elemFrameEdge(0, 26, W - 1, 26);                           // 顶栏分隔线

    mMarqueeRun = (sw1 > SW);                        // 本帧在滚动? 是则下帧不可跳过
    u8g2->sendBuffer();
    return;                                         // 大屏专用布局结束; 小屏继续走下面原有四行布局
  }

  // ==========================================================================
  // 四行布局(按屏分辨率自适应): 电台名 / 节目信息 / 状态+格式+码率+WiFi / IP+音量
  // ==========================================================================

  // 长文本(第1/2行): 无缝跑马灯 —— 把"文字 + 间隔"重复画两遍, 偏移按周期取模循环; 第一遍滚出左边界之前,
  //   第二遍已经从右侧紧跟着进入, 所以永远连续、不会"滚完归零、第一字闪一下看不见"。
  //   (旧逻辑只画一遍 + 归零重置: 滚到末尾整段消失再跳回开头, 视觉上第一个字就丢了。)
  // ★2026-09-23 撕裂修复: 跑马灯"汉字被上中下劈开"的错位量, 恰好等于"每步像素"——
  //   步长 6px 时跨页(8 行一条带)的上下两截会错开 6px, 肉眼就是"断成几截"; 改成 3px 后错位减半。
  //   步长减半会让滚动变慢, 故"上屏节拍"一并减半(tick 220->150 + .ino 画面节流 300->150)维持原速。
  // ★2026-09-23: 期间试过"翻页交替"(7567 完全不滚动、离散换页), 现按要求【改回连续滚动】+ 再降一档速度。
  const int MQ_GAP  = 8;          // 两遍之间的间隔(像素)(24->8: 原来 1.5 个汉字宽的明显空档)
  elemZoneReset();    // ★小屏四行布局同样要清分区表(大屏分支已 return, 不会走到这里)
  int w1 = cnWidth(*u8g2, station.c_str());
  if (w1 <= W) drawCN(*u8g2, 0, yStation, station.c_str());
  else {
    const int period = w1 + MQ_GAP;
    const int prev1 = marqueeOff;
    marqueeOff = (marqueeOff + pxStep) % period;                       // 按时间推进(见上方"跑马灯推进")
    if (MQ_DWELL_MS && marqueeOff < prev1) mqDwellUntil = millis() + MQ_DWELL_MS;   // 滚完一轮 -> 原地静止一会儿
    drawCN(*u8g2, -marqueeOff,           yStation, station.c_str());   // 第一遍
    drawCN(*u8g2, period - marqueeOff,   yStation, station.c_str());   // 第二遍(无缝衔接)
  }

  // ===== 第2行: 电台信息(中文 16×16) =====  无缝跑马灯(同第1行)
  int w2 = cnWidth(*u8g2, info);
  if (w2 <= W) drawCN(*u8g2, 0, yInfo, info);
  else {
    const int period = w2 + MQ_GAP;
    const int prev2 = marqueeOff2;
    marqueeOff2 = (marqueeOff2 + pxStep) % period;      // 按时间推进(与第1行同一节拍)
    if (MQ_DWELL_MS && marqueeOff2 < prev2) mqDwellUntil = millis() + MQ_DWELL_MS;  // 同第1行: 滚完一轮静止一会儿
    drawCN(*u8g2, -marqueeOff2,         yInfo, info);   // 第一遍
    drawCN(*u8g2, period - marqueeOff2, yInfo, info);   // 第二遍(无缝衔接)
  }

  // ===== 第3行: 播放状态(图标) + 格式 + 码率(左) | WiFi 信号(右) =====
  u8g2->setFont(smallFont);
  drawPlayIcon(*u8g2, 2, yLine3, playing);                // ▶ 播放 / ⏸ 暂停(左侧状态)
  u8g2->setCursor(12, yLine3);
  u8g2->print(codecStr);                                // 如 "MP3 128k" / "AAC 128k"
  {
    const String wifiStr = "WiFi:" + wifiPart;
    const int ww = u8g2->getUTF8Width(wifiStr.c_str());
    elemZoneSplit(ELEM_STATUS, ELEM_WIFI, W - ww - 2, yLine3 - 13, yLine3);  // 左=播放图标+格式码率 | 右=WiFi
    u8g2->setCursor(W - ww, yLine3);                     // 右对齐, 与左侧图标+格式永不碰撞
    u8g2->print(wifiStr);
  }

  // ===== 第4行: 本机 IP(左) | [闪电]电量% 音量%(右) =====
  // 充电时电量%左侧画闪电图标。
  // ★2026-10-07 重排降级优先级(修"设了睡眠定时器后 IP 就不显示"):
  //   原降级链是【先丢 IP -> 再去 V -> 再去睡眠】, 结果"去 V"与"去睡眠"两段做的事完全一样
  //   (都是 volStr = 百分比), 睡眠后缀根本去不掉 —— 而 IP 早在第1 步就被永久丢掉了。
  //   实测(小字5x7, 128px 屏): IP 65px + "80% 眠1.5时" 72px + 间距 6px = 143px > 126px
  //   ⇒ 设睡眠定时器后必然超宽 ⇒ IP 消失; 而"无睡眠"时 65+37+6=108px 刚好放得下。
  //   这正是用户看到的现象, 与"旋转"无关(旋转只是换了屏/重画, 让它更容易被注意到)。
  //   正确优先级: 睡眠后缀(最次要) -> "V"标签 -> IP -> 音量%(电量% 永远保留)。
  u8g2->setFont(smallFont);
  String battStr = String(battPct) + "%";                 // 电量百分比(如 "80%")
  const int bw = u8g2->getUTF8Width(battStr.c_str());
  const int lgw = gState.charging ? (big ? 14 : 9) : 0;   // 闪电图标宽(含右侧间距)
  const int gap = 2;
  // 三段可选内容各自的宽度(先算好, 降级时只需重新组合, 不必反复测宽)
  const int ipFullW = u8g2->getUTF8Width(gState.ip.c_str());
  const String pctStr = String(volPct) + "%";         // 音量数字部分
  const int pctW    = u8g2->getUTF8Width(pctStr.c_str());
  const int sleepW  = (sleepMinDisp > 0)
                    ? u8g2->getUTF8Width((" 眠" + sleepDisp).c_str()) : 0;

  bool showIp = (ipFullW > 0);
  bool showVTag = true;
  bool showSleep = (sleepMinDisp > 0);
  // 逐级降级: 只要"IP + 右侧"放不下就砍掉当前最次要的一项, 重新判断。
  // 用循环而不是三段 if: 保证"砍掉睡眠后缀后能放得下"时立刻停手, 不会多砍。
  for (int guard = 0; guard < 4; guard++) {
    const int vw = pctW + (showVTag ? u8g2->getUTF8Width("V") : 0) + (showSleep ? sleepW : 0);
    const int rightW = lgw + (lgw ? gap : 0) + bw + gap + vw;
    if (!showIp || ipFullW + 4 + rightW <= W - 2) break;// 放得下(或已无 IP 可丢)即完成
    if (showSleep)     { showSleep = false; continue; }  // ① 睡眠后缀最次要, 先砍
    if (showVTag)      { showVTag  = false; continue; }  // ② 再砍 "V" 标签
    if (showIp)        { showIp    = false; continue; }  // ③ 最后才砍 IP
    break;                                              // ④ 只剩电量%, 无可再砍
  }
  const int vw = pctW + (showVTag ? u8g2->getUTF8Width("V") : 0) + (showSleep ? sleepW : 0);
  const int rightW = lgw + (lgw ? gap : 0) + bw + gap + vw;
  elemZoneSplit(ELEM_IP, ELEM_BATT, W - rightW - 2, yLine4 - 13, yLine4);   // 左=IP | 右=电量/音量/睡眠

  if (showIp) { u8g2->setCursor(0, yLine4); u8g2->print(gState.ip); } // 画 IP(左)
  // 画右侧(从右往左: 音量% -> 电量% -> 闪电)
  int x = W - vw;
  u8g2->setCursor(x, yLine4);
  if (showVTag) u8g2->print("V");
  u8g2->print(pctStr);
  if (showSleep) u8g2->print(" 眠" + sleepDisp);
  x -= gap + bw;
  u8g2->setCursor(x, yLine4); u8g2->print(battStr);
  if (gState.charging) {
    const int bh = big ? 12 : 8;                            // 闪电高度(≈宽)
    drawLightning(*u8g2, x - 2 - bh, yLine4 - bh, bh);
  }

  elemZone(ELEM_STATION, yStation - 16, yStation);      // 第1行 电台名(跑马灯)
  elemZone(ELEM_TITLE,   yInfo - 16,    yInfo);         // 第2行 节目信息(跑马灯)

  mMarqueeRun = (w1 > W) || (w2 > W);   // 本帧在滚动? 是则下一帧不可跳过(否则文字卡住)
  u8g2->sendBuffer();
}

// ============================================================================
// 电台列表界面(2026-09-21): 长按中键 / 长按红外"播放键"进入的"快速选台"
// ----------------------------------------------------------------------------
// 与主界面共用同一套「中文逐字渲染 + 整帧跳过」, 差异只在本函数自己算布局:
//   · 按屏高定向数与行高(见下), 128×64 与 320×240 各自铺满;
//   · 选中行【反白】: 先铺满一行实心块(1 色), 再把该行文字用 0 色"挖"出来 —— 1bpp 屏上
//     就是整行黑白反转, 远看清清楚楚(单色屏只有这一个强调手段);
//   · "正在播放的那台"用行首 '*' 标记 —— 与反白是两回事: 光标可能停在别的台上,
//     用户需要同时知道"选中了谁"和"正在放谁";
//   · 超宽台名按屏宽截断(见 drawCNLimit 说明), 不做跑马灯。
// 参数: names/n = 整份电台名(调用方持有, 本函数不复制), cursor = 光标序号, current = 正在播的序号(-1=无)。
// 滚动窗口 mListTop 是本对象持有的纯视图状态, 每次调用按 cursor 自动跟随并钳制。
// ============================================================================
// ★测一段 UTF-8 文本的绘制宽度 —— 必须与 drawCNLimit/glyphWidth 用【同一套】规则逐字累加。
//   【为什么不能用 u8g2->getUTF8Width()】: 它按"当前 setFont 的字体"算, 而本文件的绘制是
//   逐字选字体的(ASCII 走 8x13, 中文走 GB2312 各号字体)。两者不一致时中文会被算窄 ——
//   后果就是"标签底框比文字窄, 文字溢出压到隔壁标签上", 看上去全堆在一起。
static int textWidthCN(U8G2& g, const char* s) {
  int w = 0;
  const uint8_t* p = (const uint8_t*)s;
  while (*p) {
    uint16_t cp;
    if (*p < 0x80)                   { cp = *p;                                                        p += 1; }
    else if ((*p & 0xE0) == 0xC0)    { cp = (uint16_t)(((p[0] & 0x1F) << 6) | (p[1] & 0x3F));            p += 2; }
    else if ((*p & 0xF0) == 0xE0)    { cp = (uint16_t)(((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6)
                                                  | (p[2] & 0x3F));                                    p += 3; }
    else { ++p; continue; }                       // 非法首字节: 跳过, 与 drawCNLimit 一致
    if (cp) w += glyphWidth(g, cp);
  }
  return w;
}

void DisplayUI::showList(const String* names, int n, int cursor, int current,
                         const String* groups, int ng, int sel) {
  if (mScreenOff) return;     // 睡眠熄屏期间不绘制
  if (!u8g2 || !names || n <= 0) return;
  elemZoneReset();            // ★元素配色: 每帧清空分区表

  const int  W   = u8g2->getWidth();
  const int  H   = u8g2->getHeight();
  const bool big = (H >= 200);          // 320×240 彩色(ST7789) vs 128×64 单色

  // ---- 布局常量 ----
  //   128×64 : 行高 18, 首行基线 15 (字占 1~17), 底部第 62 行放计数(5×7)
  //   320×240: 行高 30, 首行基线 24 (字占 10~26), 底部第 232 行放计数(8×13)
  // ★2026-10-06: 顶部多一行【分类标签栏】(有分类时才占), 列表整体下移。
  const int rowH  = big ? 30 : 18;
  const int footH = big ? 14 : 11;      // 底部计数行预留高度
  // 标签数上限 24(见下面 tabW 数组): 选中项必须钳进实际会画的范围内, 否则 tabW[sel] 越界
  const int MAX_TAB  = 24;
  const int nTabAll = (ng < MAX_TAB) ? ng : MAX_TAB;   // ng>=0 由调用方保证
  const bool hasTabs = (groups != nullptr) && (nTabAll > 0);
  const int  tabH   = hasTabs ? rowH : 0;      // 标签栏高度(与行同高, 视觉整齐)
  const int  y0     = (big ? 24 : 15) + tabH;  // 列表首行基线
  int rows = (H - y0 - footH) / rowH + 1;
  if (rows < 1) rows = 1;

  if (cursor < 0)      cursor = 0;
  if (cursor > n - 1)  cursor = n - 1;
  if (sel < 0)         sel = 0;
  if (nTabAll > 0 && sel > nTabAll - 1) sel = nTabAll - 1;

  // ---- 滚动窗口跟随光标(光标永远可见) ----
  if (cursor < mListTop)               mListTop = cursor;
  else if (cursor >= mListTop + rows)  mListTop = cursor - rows + 1;
  const int maxTop = (n > rows) ? (n - rows) : 0;
  if (mListTop > maxTop) mListTop = maxTop;
  if (mListTop < 0)      mListTop = 0;
  const int visN = ((n - mListTop) < rows) ? (n - mListTop) : rows;

  // ---- 分类标签的像素宽度(必须用 textWidthCN 逐字累加, 见该函数注释: 用 getUTF8Width 会算窄) ----
  // ★留白: pad 是标签文字左右的内边距, gap 是标签之间的间距。128x64 上中文标签约 16px/字,
  //   pad 3 + gap 3 才不会"挤成一坨"; 彩屏上按比例放大。
  int tabW[24];                       // 标签数上限 24(分类多了也只画得下这几个)
  const int nTab = nTabAll;
  const int pad  = big ? 8 : 4;       // 标签左右内边距
  const int gap  = big ? 6 : 4;       // 标签之间的间距
  for (int k = 0; k < nTab; k++)
    tabW[k] = textWidthCN(*u8g2, groups[k].c_str()) + pad * 2;

  // ---- 整帧跳过指纹: 可见行 + 光标 + 窗口 + 总数 + 正在播序号 + 分类标签(名字与选中项) ----
  // 首字节混入 'L' 与主界面指纹空间隔开, 避免两个界面的指纹偶然相撞而漏画一帧。
  uint32_t h = FRAME_HASH_SEED;
  h ^= 0x4Cu;                          h *= 16777619u;   // 'L'
  h ^= (uint8_t)cursor;                h *= 16777619u;
  h ^= (uint8_t)mListTop;              h *= 16777619u;
  h ^= (uint8_t)(n & 0xFF);            h *= 16777619u;
  h ^= (uint8_t)((current < 0) ? 255 : (current & 0xFF)); h *= 16777619u;
  h ^= (uint8_t)sel;                   h *= 16777619u;   // 换分类必须重画
  for (int k = 0; k < nTab; k++) mixStr(h, groups[k]);
  for (int r = 0; r < visN; r++) mixStr(h, names[mListTop + r]);
  if (mFrameValid && h == mFrameHash) { mSkipCnt++; return; }   // 内容没变 -> 整帧跳过
  mFrameHash = h; mFrameValid = true; mMarqueeRun = false; mDrawCnt++;
  elemZone(ELEM_LIST, 0, H - 1);   // ★元素配色: 整个列表界面(含分类标签栏)用"列表"这一档颜色

  u8g2->clearBuffer();

  // ---- 顶部: 分类标签栏(选中项反白高亮; 放不下时横向滚动保证选中项完整可见) ----
  if (hasTabs) {
    const int ty = (big ? 24 : 15);
    // ① 横向滚动: 夹紧 mTabTop, 保证【选中标签】完整落在屏内
    int sum = 0; for (int k = 0; k < nTab; k++) sum += tabW[k] + gap;
    const int total = sum > 0 ? sum - gap : 0;            // 末项后面不加 gap
    int selStart = 0;                                    // 选中标签的起始 x(未滚动坐标系)
    for (int k = 0; k < sel; k++) selStart += tabW[k] + gap;
    const int selEnd = selStart + tabW[sel];
    if (total <= W) {
      mTabTop = 0;                                       // 全部放得下 -> 不滚
    } else {
      if (mTabTop > selStart)          mTabTop = selStart;              // 选中项左边界别被切掉
      if (mTabTop < selEnd - W)        mTabTop = selEnd - W;             // 右边界也别被切掉
      if (mTabTop > total - W)         mTabTop = total - W;              // 别滚过头留空白
      if (mTabTop < 0)                 mTabTop = 0;
    }
    int cx = -mTabTop;
    // 标签底框比行高【上下各缩 1px】, 视觉上比满格高度更透气, 不会与下方列表行连成一片
    const int boxTop = ty - (big ? 20 : 14) + 1;
    const int boxH   = rowH - (big ? 6 : 2) - 2;
    for (int k = 0; k < nTab && cx < W; k++) {
      const bool on = (k == sel);
      if (on) {                                          // 选中项画实心底块, 文字用 0 色"挖"出来
        u8g2->setDrawColor(1);
        u8g2->drawBox(cx, boxTop, tabW[k], boxH);
        u8g2->setDrawColor(0);
      }
      drawCNLimit(*u8g2, cx + pad, ty, groups[k].c_str(), cx + tabW[k] - pad + 1);
      if (on) u8g2->setDrawColor(1);
      cx += tabW[k] + gap;
    }
    // 右侧若还有标签没画完, 画个小三角提示"右边还有" —— 让用户知道能横向翻
    if (cx < W) u8g2->drawTriangle(W - 6, ty - 8, W - 2, ty - 4, W - 6, ty);
    // 标签栏与列表之间的分隔线: 让"标签区 / 电台区"一眼分得开(不画的话两区视觉上糊成一片)
    const int sepY = ty - (big ? 20 : 14) + rowH - (big ? 6 : 2) + 1;
    if (sepY < H - footH) u8g2->drawHLine(0, sepY, W);   // U8g2 API: drawHLine(x, y, w)
  }

  for (int r = 0; r < visN; r++) {
    const int  i   = mListTop + r;
    const int  y   = y0 + r * rowH;
    const bool sel = (i == cursor);

    if (sel) {                                   // 选中行反白
      u8g2->setDrawColor(1);
      u8g2->drawBox(0, y - (big ? 20 : 14), W, rowH - (big ? 6 : 2));
      u8g2->setDrawColor(0);                     // 之后的文字用 0 色从实心块里"挖"出来
    }
    // 行首 "[*]序号." —— '*' 表示这一台正在播放; 序号右对齐到 2 位, 保证各行文字左边缘对齐
    char pre[12];
    snprintf(pre, sizeof(pre), "%c%2d.", (i == current) ? '*' : ' ', i + 1);
    const int x = drawCNLimit(*u8g2, 1, y, pre, W - 2);
    drawCNLimit(*u8g2, x + 2, y, names[i].c_str(), W - 2);

    if (sel) u8g2->setDrawColor(1);              // 恢复默认绘制色, 别影响后面的行与底部计数
  }

  // ---- 底部: 光标位置 / 总数("我在第几个、一共几台") ----
  u8g2->setFont(big ? u8g2_font_8x13_tr : u8g2_font_5x7_tr);
  const String pos = String(cursor + 1) + "/" + String(n);
  const int pw = u8g2->getUTF8Width(pos.c_str());
  u8g2->setCursor(W - pw - 1, H - (big ? 8 : 2));
  u8g2->print(pos);

  u8g2->sendBuffer();
}

void DisplayUI::showMessage(const String& line1, const String& line2) {
  if (mScreenOff) return;     // 睡眠熄屏期间不绘制
  // ★2026-10-11: 提示页不做元素分区登记, 必须清表 —— 否则提示文字会继承上一帧(播放页/
  //   列表页)登记的元素颜色, 出现"提示字带色"的残留。03 层三个工程本就有这一行, 这里补齐。
  elemZoneReset();
  u8g2->clearBuffer();
  const int H = u8g2->getHeight();
  const bool big = (H >= 200);
  drawCN(*u8g2, 0, big ? 105 : 28, line1.c_str());
  if (line2.length()) {
    drawCN(*u8g2, 0, big ? 145 : 50, line2.c_str());
  }
  u8g2->sendBuffer();
  // 提示信息不在 show() 的指纹体系内 —— 不置无效的话, 下帧若内容与上次一致就会被
  // "整帧跳过", 这条提示会残留在屏上不消失。
  mFrameValid = false;
}

// ---- 诊断: 显示帧的绘制/跳过比例 ----
// 跳过率就是省下来的 CPU: 每跳一帧省掉一次「中文逐字渲染 + 软件 SPI 整屏发送」(实测 55~60ms)。
// 这些时间直接变成 I2S DMA 的安全余量 —— 96kHz 的 HE-AAC 台只有 170ms DMA。
// 若跳过率长期偏低(说明画面内容一直变), 就要往"码率/RSSI 抖动导致指纹失效"方向查。
void DisplayUI::printFrameStats() {
  const uint32_t total   = mDrawCnt + mSkipCnt;
  const uint32_t skipPct = total ? (mSkipCnt * 100 / total) : 0;
  Serial.printf("[LCD] 显示帧: 绘制 %lu / 跳过 %lu (跳过率 %lu%%)\n",
                (unsigned long)mDrawCnt, (unsigned long)mSkipCnt, (unsigned long)skipPct);
  mDrawCnt = 0; mSkipCnt = 0;
}

// ============================================================================
// 字库覆盖自检(串口 F) —— 回答"这个字为什么是方框"
// ============================================================================
// 屏上中文用的是 U8g2 的 u8g2_font_unifont_t_gb2312(297.8KB 字模), 它是 U8g2 内置
// 【覆盖最广】的中文档, 范围正好是【GB2312 全集】: 6763 个汉字 + 682 个符号
// (中文标点/全角字符/希腊字母/俄文字母/日文假名/制表符)。由此:
//   · GB2312 之内的字(日常台名/曲名/栏目名的 99%+) -> 正常显示;
//   · GB2312 之外的字 -> 空心方框(不是乱码!): 典型是 GBK 扩展区生僻字(囧/烜/燚/喆/堃/犇)、
//     繁体字(臺/灣)、韩文、emoji、CJK 扩展 A/B。
// ★必须分清两件事:
//   · "方框"   = 字库没有这个字形(本函数量的就是它);
//   · "乱码"   = 显示成【别的汉字或一串拉丁字母】, 那是【编码判定错】, 不是字库问题
//                —— 修法见 audio_player.cpp 的 icyToUtf8()(UTF-8/GB2312/Latin-1 判定),
//                   串口发 D 看「标题编码=」那一段就能确认判定对不对。
// 判定手法与绘制路径完全一致: U8g2 对字体里没有的码点返回 0 宽度 -> pickCnFont 返回 -1。
int DisplayUI::fontCheck(const char* s, String& missing) {
  if (!u8g2 || !s) return 0;
  int miss = 0;
  const uint8_t* p = (const uint8_t*)s;
  while (*p) {
    uint16_t cp; int step;
    if (*p < 0x80)                { p += 1; continue; }   // ASCII 走 6x10 字体, 必定有
    else if ((*p & 0xE0) == 0xC0) { cp = (uint16_t)(((*p & 0x1F) << 6) | (p[1] & 0x3F)); step = 2; }
    else if ((*p & 0xF0) == 0xE0) {
      if (!p[1] || !p[2]) break;                          // 被截断的多字节序列(防越界读)
      cp = (uint16_t)(((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F)); step = 3;
    }
    else { p += 1; continue; }                            // 非法 UTF-8 续字节
    if (pickCnFont(*u8g2, cp) < 0) {
      miss++;
      if (missing.length() < 160) {
        char h[10];
        snprintf(h, sizeof(h), "U+%04X ", cp);
        missing += h;
      }
    }
    p += step;
  }
  return miss;
}

void DisplayUI::fontCheckBuiltin() {
  static const struct { const char* name; const char* s; } kCases[] = {
    { "ASCII/数字/符号",  "ABC abc 0123 -_.:/%()" },
    { "中文常用字",       "中央人民广播电台音乐之声新闻经济交通" },
    { "中文标点",         "，。、；：？！《》（）【】“”‘’——……·～" },
    { "全角字符",         "ＡＢＣａｂｃ１２３＋－＝％" },
    { "日文假名",         "こんにちはカタカナひらがな" },
    { "希腊/俄文字母",    "αβγΩ ПРИВЕТ" },
    { "生僻字(预期方框)", "囧烜燚喆堃犇甯玥" },
    { "繁体字(多数方框)", "臺灣語言電腦學生" },
  };
  Serial.println("[FONT] 屏上中文字体 = u8g2_font_unifont_t_gb2312, 覆盖 GB2312 全集(6763 汉字 + 682 符号)");
  Serial.println("[FONT] ★方框=字库没这个字形; 若显示成别的汉字/拉丁字母=编码问题(见 D 的「标题编码」)");
  for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
    String miss;
    const int n = fontCheck(kCases[i].s, miss);
    if (n) Serial.printf("[FONT] %-18s 缺 %2d 字: %s\n", kCases[i].name, n, miss.c_str());
    else   Serial.printf("[FONT] %-18s 全部有字形\n", kCases[i].name);
  }
}

// ============================================================================
// 屏幕背景图(2026-10-06 新增) —— ★全彩色 RGB565, 不是黑白
// ----------------------------------------------------------------------------
// 数据流:
//   浏览器(canvas 缩放/裁剪 -> 转 RGB565 大端 -> 上传)
//     -> POST /api/bg/l 或 /api/bg/p(流式收进 PSRAM)
//     -> 主循环落盘 LittleFS(/bg_l.rgb 或 /bg_p.rgb, 各 153600 字节)
//     -> bgLoad() 读进 PSRAM, 供 DRAW_TILE 逐像素取色
//
// ★★和"1bpp 帧缓冲"的关系(最容易搞混的一点, 别再弄错):
//     帧缓冲(9600B) 只决定【界面画在哪】—— 它相当于一张"模板", 每个 bit = 这里要不要画界面;
//     背景图(153600B) 决定【底色是什么】—— DRAW_TILE 里 bit=0 的像素去查它, 所以底色是全彩的。
//   两者尺寸、布局、用途都不同, 唯一的交汇点就是 u8x8_d_st7789_cb 的 DRAW_TILE 那几行。
//   所以"这块屏只能显示两个颜色"的说法是错的 —— 准确说法是【U8g2 的界面层只有两态】,
//   底色完全可以是 65K 色。ST7789 是彩色面板, 这一点没有被浪费。
//
// 取舍:
//   ① 解码/缩放全在浏览器做 —— 固件不引入 PNG/JPEG 解码器, 避免大块连续内存与 CPU 峰值;
//   ② 图放 PSRAM(150KB) 而非 DRAM —— DRAM 是这块板子更稀缺的资源(音频/AAC/Web 都在抢);
//   ③ 没有背景图时 DRAW_TILE 走的还是原来那两行常量查表, 一字节都没变。
// ============================================================================
// ★sBgLoadedPort 已在文件上方(sBgBuf565 旁边)声明 —— DRAW_TILE 要用它做"图配不配当前屏"的校验。
// "某个方向到底有没有图"的缓存。[0]=横屏 / [1]=竖屏。
// ★为什么要缓存: /api/state 每 3 秒被拉一次, 而它跑在【Web 异步任务】上下文 ——
//   若在那里调 LittleFS.exists() 就是"在异步任务里碰文件系统"(本工程明令避免的事)。
//   所以在主循环这边探测好、缓存成两个 bool, 异步任务只读内存。
static bool sBgExist[2] = { false, false };

static const char* bgPath(bool portrait)    { return portrait ? BG_FILE_PORT : BG_FILE_LAND; }
static const char* bgTmpPath(bool portrait) { return portrait ? "/bg_p.tmp"  : "/bg_l.tmp";  }

// 探测两个方向各有没有背景图(★仅主循环调用: 会碰文件系统)
static void bgRefreshExist() {
  sBgExist[0] = LittleFS.exists(BG_FILE_LAND);
  sBgExist[1] = LittleFS.exists(BG_FILE_PORT);
}

bool bgHas(bool portrait) { return sBgExist[portrait ? 1 : 0]; }   // 纯内存读, 异步任务里安全
bool bgActive()           { return sBgBuf565 != nullptr; }

// 界面不透明度 0~100(100 = 界面纯色、与背景零混合, 也就是完全走原路径)。
// ★persist=true 会写 NVS, 因此【只能在主循环调用】(Web 侧走 pending 标志, 见 web_server.cpp)。
uint8_t bgUiAlphaGet() { return sBgUiAlpha; }
void    bgUiAlphaSet(uint8_t a, bool persist) {
  if (a > 100) a = 100;
  sBgUiAlpha = a;
  if (persist) { Preferences p; p.begin("disp", false); p.putInt("bga", (int)a); p.end(); }
  Serial.printf("[BG  ] 界面不透明度 -> %u%%%s\n", (unsigned)a, persist ? " (已存 NVS)" : "");
}

// 背景图亮度 0~100(100=原图)。★压暗是"加载时一次性算"的, 所以改值后若当前方向有图,
// 必须【丢弃已缓冲的那份并重读】才会看到效果 —— 这里就地把 sBgLoadedPort 置 -1 再 bgLoad()。
// 只会重读 150KB(几十 ms), 且用户是点一次按钮改一次, 不在热路径上, 代价可接受。
uint8_t bgBrightGet() { return sBgBright; }
void    bgBrightSet(uint8_t b, bool persist) {
  if (b > 100) b = 100;
  const uint8_t old = sBgBright;
  sBgBright = b;
  if (persist) { Preferences p; p.begin("disp", false); p.putInt("bgb", (int)b); p.end(); }
  Serial.printf("[BG  ] 背景图亮度 -> %u%%%s\n", (unsigned)b, persist ? " (已存 NVS)" : "");
  if (b != old && sBgBuf565) {           // 值真变了且当前有上传图 -> 重读并重算亮度
    free(sBgBuf565); sBgBuf565 = nullptr; sBgLoadedPort = -1;
    bgLoad();
  }
}

bool bgLoad() {
  bgRefreshExist();                                       // 顺带刷新"有无"状态(供 Web 回显)
  // ★用 bgSlotNow(): 横屏=横屏槽, 竖屏与 240x240 方屏都=竖屏槽(三者里只有横屏是 320 宽)。
  const bool portSlot = (bgSlotNow() == 1);
  const int  want = bgSlotNow();
  if (sBgBuf565 && sBgLoadedPort == want) return true;    // 已经是这个槽的图, 不必重读 flash
  if (sBgBuf565) { free(sBgBuf565); sBgBuf565 = nullptr; sBgLoadedPort = -1; }

  const char* path = bgPath(portSlot);
  if (!LittleFS.exists(path)) return false;               // 这个方向没上传过 -> 正常情况, 静默返回

  uint8_t* p = (uint8_t*)ps_malloc(BG_RGB_BYTES);
  if (!p) {
    Serial.printf("[BG  ] PSRAM 分配失败(需 %d B), 背景图本次不启用\n", (int)BG_RGB_BYTES);
    return false;
  }
  File f = LittleFS.open(path, "r");
  if (!f) { free(p); Serial.printf("[BG  ] 打开 %s 失败\n", path); return false; }
  size_t off = 0;                                         // 分块读: 便于给出明确的短读诊断
  while (off < (size_t)BG_RGB_BYTES) {
    const size_t chunk = ((size_t)BG_RGB_BYTES - off > 4096) ? 4096 : ((size_t)BG_RGB_BYTES - off);
    const size_t r = f.read(p + off, chunk);
    if (r == 0) break;
    off += r;
  }
  f.close();
  if (off != (size_t)BG_RGB_BYTES) {                      // 尺寸不符宁可不用, 也不能拿半张图去取色
    Serial.printf("[BG  ] %s 实际 %u 字节 != %d, 已忽略\n", path, (unsigned)off, (int)BG_RGB_BYTES);
    free(p);
    return false;
  }
  sBgBuf565 = p; sBgLoadedPort = want;
  bgApplyBright(p, BG_RGB_BYTES, sBgBright);            // 背景图亮度(100=原图, 此时函数直接返回)
  Serial.printf("[BG  ] 已加载彩色背景 %s (%d B, %s) 亮度=%u%% 内部堆=%uKB\n", path, (int)BG_RGB_BYTES,
                gSt7789Square ? "方屏(取竖屏图顶部 240 行)" : (gSt7789Portrait ? "竖屏" : "横屏"),
                (unsigned)sBgBright,
                (unsigned)(ESP.getFreeHeap() / 1024));
  return true;
}

// 落盘 + 立即生效。★只允许主循环调用: LittleFS 写入会短暂禁用 flash cache 冻核,
//   放在 Web 异步任务里会拖累同核的其它任务(与 station_store.cpp 的取舍一致)。
bool bgSave(const uint8_t* data, bool portrait) {
  if (!data) return false;
  const char* path = bgPath(portrait);
  const char* tmp  = bgTmpPath(portrait);

  File f = LittleFS.open(tmp, "w");
  if (!f) {
    Serial.printf("[BG  ] 打开 %s 失败(文件系统满?) 已用=%u/%uKB\n", tmp,
                  (unsigned)(LittleFS.usedBytes() / 1024), (unsigned)(LittleFS.totalBytes() / 1024));
    return false;
  }
  // 分块写(4KB): 单次 write 在极端情况下可能短写, 这里按返回值推进, 保证 150KB 全部落盘。
  size_t off = 0;
  while (off < (size_t)BG_RGB_BYTES) {
    const size_t chunk = ((size_t)BG_RGB_BYTES - off > 4096) ? 4096 : ((size_t)BG_RGB_BYTES - off);
    const size_t w = f.write(data + off, chunk);
    if (w == 0) break;
    off += w;
  }
  f.close();      // ★必须先 close 再 stat: VFS 有 stdio 缓冲, 不 close 时 size() 会读到 0(见 station_store 注释)
  if (off != (size_t)BG_RGB_BYTES) {
    LittleFS.remove(tmp);
    Serial.printf("[BG  ] 写入不足 %u/%d, 已放弃\n", (unsigned)off, (int)BG_RGB_BYTES);
    return false;
  }
  { File r = LittleFS.open(tmp, "r");                     // 落盘校验
    const size_t sz = r ? r.size() : 0;
    if (r) r.close();
    if (sz != (size_t)BG_RGB_BYTES) {
      LittleFS.remove(tmp);
      Serial.printf("[BG  ] 落盘校验失败(%u 字节), 已放弃\n", (unsigned)sz);
      return false;
    } }
  if (LittleFS.exists(path)) LittleFS.remove(path);
  if (!LittleFS.rename(tmp, path)) {
    LittleFS.remove(tmp);
    Serial.printf("[BG  ] rename -> %s 失败\n", path);
    return false;
  }
  Serial.printf("[BG  ] 已保存 %s (%d B, %s) 已用=%u/%uKB\n", path, (int)BG_RGB_BYTES,
                portrait ? "竖屏" : "横屏",
                (unsigned)(LittleFS.usedBytes() / 1024), (unsigned)(LittleFS.totalBytes() / 1024));

  sBgExist[portrait ? 1 : 0] = true;                      // 更新"有无"缓存(供 /api/state 回显)
  // ★判"是不是当前屏正在用的槽": 方屏借的是竖屏槽(见 bgSlotNow), 故不能直接比 gSt7789Portrait。
  if ((portrait ? 1 : 0) == bgSlotNow()) {                // 正好是当前槽 -> 立刻换图生效
    sBgLoadedPort = -1;                                   // 让 bgLoad() 重新读(不认内存里那份旧的)
    if (sBgBuf565) { free(sBgBuf565); sBgBuf565 = nullptr; }
    bgLoad();
  }
  return true;
}

void bgClear(bool portrait) {
  const char* path = bgPath(portrait);
  if (LittleFS.exists(path)) {
    LittleFS.remove(path);
    Serial.printf("[BG  ] 已删除 %s\n", path);
  }
  sBgExist[portrait ? 1 : 0] = false;                     // 更新"有无"缓存
  if ((portrait ? 1 : 0) == bgSlotNow() && sBgBuf565) {   // 当前槽被删 -> 立刻取消背景(方屏同竖屏槽)
    free(sBgBuf565); sBgBuf565 = nullptr; sBgLoadedPort = -1;
  }
}
