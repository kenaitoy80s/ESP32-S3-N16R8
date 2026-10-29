#include "audio_player.h"
#include "station_store.h"
#include "sd_card.h"     // 阶段②: 本地 SD 卡播放(SD_MMC 1-bit; 见 sd_card.h 的线程模型)
#include <SD_MMC.h>     // SD_MMC 全局实例(1-bit, 挂载点 SD_MOUNT)
// ★AACSetSbrEnabled: 本工程给第三方库加的 SBR 运行时开关。
//   ⚠ include 路径要带aac_decoder/ 前缀 —— 该头文件在库的 src/aac_decoder/ 子目录下,
//     不在对外入口(Audio.h -> Audio_nopsram.h)里, 直接 #include "aac_decoder.h" 会找不到。
//     库自己的 Audio_nopsram.cpp 也是这么写的(见其第 79 行)。
#include "aac_decoder/aac_decoder.h"
#include <WiFi.h>
// 方案B: 音频泵任务(见 config.h 的「音频泵任务」段 / 头文件的线程模型说明)。
// Arduino.h 通常已间接引入 FreeRTOS 头, 但跨核任务 API(xTaskCreatePinnedToCore / vTaskDelay /
// vTaskSuspend)是关键路径, 这里显式引入, 不依赖间接包含。
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
// GB2312 区位 -> Unicode 映射表(7445 字符, 编译后 17.7KB flash)。
// 由 tools/gen_gb2312_table.py 生成, 用来把【GBK 编码的 ICY 标题】转成 UTF-8(见 icyToUtf8)。
#include "../data/gb2312_tab.h"
// 由 tools/gen_gbk_rev_tab.py 生成: Unicode码点 -> GBK双字节码(反查), 用于还原"双重编码"乱码
// (部分上游把 UTF-8 字节按 GBK 两两切开解码再发出, 见 undoDoubleEncode 注释)。约 85KB, 放 flash。
#include "../data/gbk_rev_tab.h"

PlayerState gState;   // 全局状态实体(其余文件 extern 引用)

// ---- 断音定位探针(2026-09-18, 2026-09-19 改挂到音频泵任务) ----
// 2.0.6 是单线程库: audio.loop() 一个人包办"拉流+解码+写 I2S", 所以【两次调用之间的间隔】
// 直接决定音频连续性。间隔超过 I2S DMA 深度就会排空 -> 听感"断断续续"。
// DMA 深度: 32(buf_count) x 512(len) = 16384 帧 => 96000Hz 下 170ms / 44100Hz 下 372ms。
// ★方案B 后这个探针挂在【音频泵任务】里(测两次 audio.loop() 的间隔)。这才是喂流节奏的真实指标:
//   它若稳定在几毫秒, 说明无论主循环在刷屏还是写盘, 音频喂流都照样按时进行。
static uint32_t s_loopMaxGapUs = 0;   // 本统计周期内最大间隔(micros)
static uint32_t s_lastLoopUs   = 0;   // 上次 audio.loop() 调用时刻

// ---- ★"本台是否已经真的开始出声"的锁存标志(2026-09-19 晚加) ----
// 为什么必须"锁存"而不能每圈现算: 屏上第 2 行的文字是由这面旗子决定的(见 display_ui.cpp 的
// show()), 而 show() 靠"最终上屏字符串的指纹"做【整帧跳过】来省掉 55~60ms 的软件 SPI 整屏刷新。
// 一旦这面旗子会来回翻, 上屏字符串就每帧都不一样 -> 指纹每帧都变 -> 整帧跳过永远命中不了 ->
// 每帧都真刷一遍屏 -> 主循环每 ~300ms 卡 60ms -> **刚好把 I2S DMA 排空 -> 听感"一直卡卡卡"**。
// (这个"刷屏卡顿吃掉 DMA"的因果链在本项目已被实测确认过, 见 display_ui.cpp 的注释。)
// 所以判据必须单调: 换台时清 0, 只要观察到一次"库在跑且码率已识别"就永久置 1, 本台内不再翻转。
static bool s_outStarted = false;

// ---- ★跨任务曲名交接(方案B 关键) ----
// audio_showstreamtitle 是【库在 audio.loop() 内部】回调的, 也就是说它现在跑在【音频泵任务】里;
// 而 gState(含 String)只允许【主循环】写 —— 两个任务同时碰同一个 Arduino String 会 double-free
// 直接堆损坏。所以这里用"无堆分配的暂存缓冲 + 代次号"做交接: 泵任务只写字节, 主循环取走落地。
// 代次号奇偶约定: 奇数 = 正在写; 偶数 = 写完。主循环只接受"偶数且未取过"的代次。
static char              s_titlePending[192];
static volatile uint32_t s_titleGen     = 0;
static uint32_t          s_titleApplied = 0;

// 主循环分阶段最大耗时(微秒, 索引见 player_state.h 的 PH_* 枚举)。
// 由 webradio-s3p.ino 的 loop() 逐段写入, printDiag() 打印并清零。
// 用途: 把"主循环卡了 116ms"进一步钉到具体哪一段(落盘? 显示全刷? 切台?), 而不是只报总数。
uint32_t g_phMaxUs[PH_COUNT] = {0};

// ============ 音频库回调(方案B: 一律【不写 gState】) ============
// 这些回调由库在 audio.loop() 内部触发 —— 也就是说它们跑在【音频泵任务】上, 而 gState 只允许
// 【主循环】写(跨任务写 Arduino String 会 double-free 堆损坏)。因此这里全部只做
// "打印 / 丢弃 / 暂存到无堆缓冲":
//   · audio_info              -> 仅打印;
//   · audio_showstreamtitle   -> 唯一有内容的一个: 存进暂存缓冲 + 代次号, 由主循环取走落地(见其实现);
//   · 其余(bitrate/station/eof/id3…) -> 全部空实现, 数据由主循环主动读库访问器获取。
// 回调 API 来自本工程安装的 ESP32-audioI2S 2.0.6 fork(nopsram), 勿升级到 3.x。

// ICY StreamTitle 编码不统一: 中文电台多为 UTF-8, 西文电台常为 Latin-1, 个别老中文台为 GBK。
// U8g2 的 drawUTF8(ST7567 同样走 U8g2)只认 UTF-8, 遇到非 UTF-8 字节会直接跳过 → 曲名整行空白/乱码。
// 判别顺序(先严后宽):
//   ① 合法 UTF-8          -> 先尝试"双重编码还原"(见 undoDoubleEncode): 有些上游把 UTF-8 字节
//                            按 GBK 两两切开解码再发出, 收到的是一串"合法的错字"。还原成功用还原结果,
//                            否则原样放行。还原带严格判据, 正常标题不受影响。
//   ② 像 GB2312 双字节序列 -> 查 gGb2312Uni 表转 UTF-8。表覆盖区 1~9 符号 + 16~87 汉字(6763 字),
//                            与屏上字体 u8g2_font_unifont_t_gb2312 的范围一致 ⇒ 表里有的屏上必画得出;
//   ③ 其余                -> 按 Latin-1 逐字节升位(西文重音电台)。
// 判定结果记在 gLastTitleEnc, 串口发 D 可见"当前标题被判成什么编码"(含"UTF-8(已还原)"), 用于排查乱码。
// note: GBK 与 UTF-8 存在极小概率的字节歧义(某串 GBK 恰好也是合法 UTF-8), 此时按 UTF-8 处理 ——
//       实测中文电台标题极少踩中, 真踩中也只是"那一条乱码", 不影响其它标题。
static const char* gLastTitleEnc = "未判";

static bool isUtf8(const uint8_t* p) {
  int i = 0;
  while (p[i]) {
    uint8_t c = p[i];
    if (c < 0x80) { i++; }
    else if ((c & 0xE0) == 0xC0) { if ((p[i+1] & 0xC0) != 0x80) return false; i += 2; }
    else if ((c & 0xF0) == 0xE0) { if ((p[i+1]&0xC0)!=0x80 || (p[i+2]&0xC0)!=0x80) return false; i += 3; }
    else if ((c & 0xF8) == 0xF0) { if ((p[i+1]&0xC0)!=0x80||(p[i+2]&0xC0)!=0x80||(p[i+3]&0xC0)!=0x80) return false; i += 4; }
    else return false;
  }
  return true;
}
// 粗略判断是否为 GB2312 双字节序列: 每个双字节都落在合法区位内、且多数能在表里查到。
// 要求"至少有双字节 且 过半能查到", 以免把垃圾字节误当 GBK 转出一串 '?'。
static bool looksGb2312(const uint8_t* p) {
  int pairs = 0, ok = 0;
  for (int i = 0; p[i]; ) {
    const uint8_t c = p[i];
    if (c < 0x80) { i++; continue; }                                     // ASCII: 跳过
    if (c < 0xA1 || c > 0xF7 || p[i+1] < 0xA1 || p[i+1] > 0xFE) return false;  // 非法区位 -> 判非 GBK
    if (gGb2312Uni[(c - 0xA1) * 94 + (p[i+1] - 0xA1)] != 0) ok++;
    pairs++;
    i += 2;
  }
  return pairs > 0 && ok * 2 >= pairs;
}

// 码点 -> UTF-8 字节(返回长度, 不含终止符)。与 display_ui.cpp 的同名函数各自 static, 互不影响。
static int utf8enc(uint16_t cp, char* b) {
  if (cp < 0x80)  { b[0] = (char)cp; return 1; }
  if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
  b[0] = (char)(0xE0 | (cp >> 12));
  b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
  b[2] = (char)(0x80 | (cp & 0x3F));
  return 3;
}

// ============================================================================
// 双重编码(双重 mojibake)还原 —— 2026-10-06
// ----------------------------------------------------------------------------
// 现象: 后台/屏上显示 "寮犵幃浼? - 鎬庝箞璇存垜涓嶇埍浣?" 这类字符。
//   它们【不是】设备解错编码 —— 抓流实测(218.6.164.56:7999, /01 与 /04 两个频道),
//   上游发来的字节本身就是坏的: "张玮伦" 的 UTF-8 是 E5 BC A0 E7 8E AE E4 BC A6,
//   上游按 GBK 每 2 字节切开解码 -> 寮 犵 幃 浼 + 1 个残字节, 再以 UTF-8 发出。
//   ⇒ 这是"UTF-8 被当 GBK 解码"造成的双重编码, 信息在【上游】就已部分丢失。
//
// 还原思路(把上面那条链反着走):
//   B2(收到的 UTF-8) --UTF-8解码--> 字符序列 --逐字符按GBK编码--> B1(原始UTF-8字节)
//   --UTF-8解码--> 原文
// 关键判据(为什么不会误伤正常标题):
//   ★正常中文标题的字符按 GBK 编码出来的字节, 几乎不可能凑成合法 UTF-8 ⇒ 还原失败 ⇒ 保留原文。
//   ★再加一层: 还原结果【必须全部落在 U+2E80 以上】(CJK/全角区), 否则视为误判。
//     (实测误判产物是 Ⱥ è ֵ ԡ ʫ 这类 U+00xx~U+05xx 的拉丁/希腊/西里尔字符, 一律被拦下)
//   实测: 3 个正常频道(大猫轻音乐电台/王娜 - 借酒封喉/34.浴火成诗)全部原样通过;
//         2 个坏频道还原出 "张玮" / "怎么说我不爱" / "流星"。
// 局限: 上游切字节时【奇数长度会丢 1 个字节】, 所以还原后可能少一两个字(救不回来)。
// 串口 D 可见 gLastTitleEnc = "UTF-8(已还原)", 用于现场确认是否触发。
// ============================================================================

// Unicode 码点 -> GBK 双字节码(二分; 命中返回 0xHHLL, 未收录返回 0)
static uint16_t uniToGbk(uint16_t cp) {
  int lo = 0, hi = GBK_REV_N - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) >> 1;
    const uint16_t u = gGbkRev[mid * 2];
    if      (u < cp) lo = mid + 1;
    else if (u > cp) hi = mid - 1;
    else             return gGbkRev[mid * 2 + 1];
  }
  return 0;
}

// 从 UTF-8 串 pos 处取一个码点; 成功则推进 pos。
// ★4 字节序列(emoji / CJK 扩展 B 等, 首字节 0xF0~0xF4)在此【刻意】返回 false:
//   cp 是 uint16_t, 装不下; 而返回 false 会让调用方把它当"分隔符"处理
//   (undoDoubleEncode 里 j = save 回退 / allCjkish 直接判否), 于是含 emoji 的标题只是
//   "按 emoji 分段还原", 绝不会错位解码 —— 降级是安全的。
//   (isUtf8() 认 4 字节, 那是"整串是否合法 UTF-8"的判据, 与此处口径不同, 两者不是一回事。)
static bool nextCp(const String& s, int& pos, uint16_t& cp) {
  const int n = s.length();
  if (pos >= n) return false;
  const uint8_t c = (uint8_t)s[pos];
  if (c < 0x80) { cp = c; pos += 1; return true; }
  if ((c & 0xE0) == 0xC0 && pos + 1 < n && ((uint8_t)s[pos + 1] & 0xC0) == 0x80) {
    cp = (uint16_t)(((c & 0x1F) << 6) | ((uint8_t)s[pos + 1] & 0x3F)); pos += 2; return true;
  }
  if ((c & 0xF0) == 0xE0 && pos + 2 < n && ((uint8_t)s[pos + 1] & 0xC0) == 0x80
      && ((uint8_t)s[pos + 2] & 0xC0) == 0x80) {
    cp = (uint16_t)(((c & 0x0F) << 12) | (((uint8_t)s[pos + 1] & 0x3F) << 6)
                    | ((uint8_t)s[pos + 2] & 0x3F));
    pos += 3; return true;
  }
  return false;
}

// 结果是否"可信": 非空, 且每个码点都 >= 0x2E80(CJK 部首起点) —— 这是防误伤的核心闸门
static bool allCjkish(const String& s) {
  int pos = 0;
  if (!s.length()) return false;
  while (pos < s.length()) {
    uint16_t cp;
    if (!nextCp(s, pos, cp)) return false;
    if (cp < 0x2E80) return false;
  }
  return true;
}

// 把一段 GBK 字节当 UTF-8 解码。返回空串 = "这段不像双重编码", 调用方应保留原文。
static String decodeGbkAsUtf8(const uint8_t* b, int n, int segChars) {
  { String t; t.reserve(n);                       // ① 严格: 整段必须是合法 UTF-8
    int pos = 0; bool ok = true;
    while (pos < n) {
      const uint8_t c = b[pos];
      if      (c < 0x80) { t += (char)c; pos++; }
      else if ((c & 0xE0) == 0xC0 && pos + 1 < n && (b[pos + 1] & 0xC0) == 0x80) {
        t += (char)b[pos]; t += (char)b[pos + 1]; pos += 2; }
      else if ((c & 0xF0) == 0xE0 && pos + 2 < n && (b[pos + 1] & 0xC0) == 0x80
               && (b[pos + 2] & 0xC0) == 0x80) {
        t += (char)b[pos]; t += (char)b[pos + 1]; t += (char)b[pos + 2]; pos += 3; }
      else { ok = false; break; }
    }
    if (ok && allCjkish(t)) return t;
  }
  { String t; t.reserve(n);                       // ② 容错: 丢掉错位字节(上游奇数长度留下的)
    int pos = 0;
    while (pos < n) {
      const uint8_t c = b[pos];
      if      (c < 0x80) { t += (char)c; pos++; continue; }
      if ((c & 0xE0) == 0xC0 && pos + 1 < n && (b[pos + 1] & 0xC0) == 0x80) {
        t += (char)b[pos]; t += (char)b[pos + 1]; pos += 2; continue; }
      if ((c & 0xF0) == 0xE0 && pos + 2 < n && (b[pos + 1] & 0xC0) == 0x80
          && (b[pos + 2] & 0xC0) == 0x80) {
        t += (char)b[pos]; t += (char)b[pos + 1]; t += (char)b[pos + 2]; pos += 3; continue; }
      pos++;                                        // 非法字节 -> 丢
    }
    // 长度下限: n 个字符恢复出 2n 字节 UTF-8, 正常应解出约 2n/3 个汉字
    if (allCjkish(t) && (int)t.length() >= segChars * 2 / 3 - 1) return t;
  }
  return String();
}

// 主函数: 扫描整串, 对"连续可 GBK 编码的非 ASCII 字符"段尝试还原。
// 返回空串 = 没做任何改动(调用方用原文, 保证正常标题零影响)。
static String undoDoubleEncode(const String& s) {
  const int n = s.length();
  String out; out.reserve(n);
  bool changed = false;
  int i = 0;
  while (i < n) {
    // ---- 收集一个候选段: 连续、非 ASCII、且能在 GBK 表里查到 ----
    int j = i, blen = 0, segChars = 0;
    uint8_t buf[384];                              // 标题上限 192B -> 段最多 128B GBK, 384 足够
    while (j < n) {
      const int save = j;
      uint16_t cp;
      if (!nextCp(s, j, cp))          { j = save; break; }
      if (cp < 0x80)                  { j = save; break; }   // ASCII 作分隔符
      const uint16_t g = uniToGbk(cp);
      if (!g)                         { j = save; break; }   // 不在 GBK 里 -> 分隔
      if (blen + 2 > (int)sizeof(buf)){ j = save; break; }
      buf[blen++] = (uint8_t)(g >> 8);
      buf[blen++] = (uint8_t)(g & 0xFF);
      segChars++;
    }
    if (segChars == 0) {                           // 非候选字符: 原样搬过去
      const int save = i;
      uint16_t cp;
      if (nextCp(s, i, cp)) out += s.substring(save, i);
      else                { out += s[save]; i = save + 1; }
      continue;
    }
    // 段尾若紧跟 '?'(上游丢掉残字节后留下的标记), 且字节数暗示有残字节 -> 一并吞掉
    if (j < n && s[j] == '?' && (blen % 3) != 0) j++;
    const String r = decodeGbkAsUtf8(buf, blen, segChars);
    if (r.length()) { out += r; changed = true; }
    else            { out += s.substring(i, j); }  // 不像双重编码 -> 原样保留
    i = j;
  }
  return changed ? out : String();
}

// ★2026-10-07 由static 改为公开: 台名(stations.json)、SD 文件名、城市名等
//   【从外部数据源进来的字符串】同样可能是 GBK/双重编码, 之前只有曲名走了这里,
//   导致后台乱码 + 屏上画方框(见 station_store.cpp 的同名调用)。
//   判别逻辑本身没变, 只是让别的模块也能用同一套判别(避免两套规则各修一半)。
String icyToUtf8(const char* s) {
  if (!s || !*s) return String();
  const uint8_t* p = (const uint8_t*)s;
  if (isUtf8(p)) {                                 // ① 已是 UTF-8
    // ★注意: "合法 UTF-8" 并不等于"内容正确" —— 双重编码的乱码本身也是合法 UTF-8,
    //   所以这里再走一道还原; 还原失败(返回空)就用原文, 对正常标题完全无影响。
    const String raw(s);
    const String fixed = undoDoubleEncode(raw);
    if (fixed.length()) { gLastTitleEnc = "UTF-8(已还原)"; return fixed; }
    gLastTitleEnc = "UTF-8";
    return raw;
  }
  if (looksGb2312(p)) {                                          // ② GBK -> 查表转 UTF-8
    String out;
    out.reserve(strlen(s) * 3 / 2 + 1);
    for (int i = 0; p[i]; ) {
      const uint8_t c = p[i];
      if (c < 0x80) { out += (char)c; i++; continue; }
      if (c >= 0xA1 && c <= 0xF7 && p[i+1] >= 0xA1 && p[i+1] <= 0xFE) {
        const uint16_t u = gGb2312Uni[(c - 0xA1) * 94 + (p[i+1] - 0xA1)];
        if (u) { char b[4]; const int n = utf8enc(u, b); b[n] = 0; out += b; }
        else   { out += '?'; }                                   // GBK 扩展区生僻字(表外)
        i += 2;
        continue;
      }
      out += '?'; i++;                                           // 残留非法字节
    }
    gLastTitleEnc = "GB2312";
    return out;
  }
  String out;                                                     // ③ 其余: 按 Latin-1 升位
  for (int j = 0; p[j]; j++) {
    const uint8_t c = p[j];
    if (c < 0x80) out += (char)c;
    else { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
  }
  gLastTitleEnc = "Latin-1";
  return out;
}

void audio_info(const char* info) {
  // 只打印库的 AUDIO_INFO 自述(带 [LIB] 前缀), 绝不写 gState(可能在库任务线程执行)。
  if (info && *info) Serial.printf("[LIB] %s\n", info);
}
void audio_bitrate(const char* info) {
  (void)info;
  // 码率改由主循环 updateAudioHealth() 通过 audio.getBitRate() 读取并显示,
  // 此处不写 gState(避免跨线程写 String)。
}
// ============ 节目信息(曲名/栏目) 2026-09-18 启用 ============
// 之前这里是个空实现(注释说"2.0.0 不回调它"), 于是屏幕上第 2 行永远只显示"播放中"。
// 读了库源码后确认: 2.0.6 一定会在【音频连接内】解析 ICY 元数据并回调本函数
//   Audio_nopsram.cpp:3911  AUDIO_INFO("%s", sTit);            <- 串口那行 [LIB] StreamTitle=…
//   Audio_nopsram.cpp:3915  audio_showstreamtitle(sTit + pos); <- 就是这里回调我们
// 而且只在 title 的 hash 变化时才触发(换曲才一次), 开销可忽略。
// ★关键: 这条路径【不额外开第二条连接】, 带宽/内存零成本 —— 与本工程里那套
//   "独立 ICY 读取器"(startMeta/updateMeta, 受 META_ENABLED_DEFAULT 控制, 会再拉一路完整流)
//   是两回事。所以即使 META_ENABLED_DEFAULT=0, 节目信息也照常有。
// ★线程安全(方案B 2026-09-19 已改): 本回调在【音频泵任务】里执行(库在 audio.loop() 内部回调),
//   而 gState 只允许【主循环】写 —— 跨任务写 Arduino String 会 double-free 造成堆损坏。
//   故这里【绝不碰 gState】, 只把原始字节拷进无堆分配的暂存缓冲 + 递增代次号;
//   真正的落地(转 UTF-8 + 写 gState.title)由主循环的 AudioPlayer::loop() 完成。
void audio_showstreamtitle(const char* info) {
  if (!info || !*info) return;
  s_titleGen++;                                    // -> 奇数: 标记"写入中"
  strncpy(s_titlePending, info, sizeof(s_titlePending) - 1);
  s_titlePending[sizeof(s_titlePending) - 1] = 0;  // 超长时兜底加终止符
  s_titleGen++;                                    // -> 偶数: 标记"写入完成"
}
void audio_showstation(const char* info) {
  (void)info;
  // 始终以用户在列表中保存的电台名为准; 不写 gState(避免跨线程竞争)。
}
void audio_eof_mp3(const char* info) {
  (void)info;
  // 流结束由主循环看门狗基于缓冲增长判定重连/跳台; 此处不写 gState(避免跨线程竞争)。
}

// ---- ESP32-audioI2S 还会调用的其他回调: 补齐桩函数, 否则链接报错 ----
// ★audio_id3data 在阶段②(SD 卡播放)里【被真正使用了】:
//   本地 MP3/AAC 文件的曲名就在这里(ID3v2 的 TIT2)。网络电台走 ICY 的 audio_showstreamtitle,
//   两者写的是同一个暂存缓冲 s_titlePending + 代次号 s_titleGen, 由主循环 applyPendingTitle 取走,
//   所以屏幕上第2行对本地文件同样能显示曲名, 且完全遵守"gState 只由主循环写"的铁律。
// 回调给的是整个 ID3 帧的文本, 里面按 "Title: xxx" / "Artist: xxx" / "Album: xxx" 分段,
// 我们只取 Title 段(没有 Title 时退回取 Artist), 免得整段乱码一样的东西被塞进屏幕。
void audio_id3data(const char* info) {
  if (!info || !*info) return;
  const char* t = strstr(info, "Title:");
  if (!t) t = strstr(info, "Artist:");
  if (!t) return;                       // 没有可用字段就丢弃(总比显示一整段 ID3 帧好)
  t += (t[0] == 'T') ? 6 : 7;           // 跳过 "Title:"(6) 或 "Artist:"(7)
  while (*t == ' ') t++;                // 跳过冒号后的空格(ID3 里常有若干个)
  if (!*t) return;
  // 原地剔除换行/制表: ID3 文本里常见 \r\n, 直接显示会把屏幕排版搞乱
  char clean[192];
  size_t n = 0;
  for (const char* p = t; *p && n < sizeof(clean) - 1; p++) {
    if (*p == '\r' || *p == '\n' || *p == '\t') continue;
    clean[n++] = *p;
  }
  clean[n] = 0;
  if (n == 0) return;

  s_titleGen++;                          // 奇数: 写入中(与 ICY 标题同一套交接协议)
  strncpy(s_titlePending, clean, sizeof(s_titlePending) - 1);
  s_titlePending[sizeof(s_titlePending) - 1] = 0;
  s_titleGen++;                          // 偶数: 写完, 等主循环取
}
void audio_icyurl(const char* info)    { (void)info; }
void audio_icyname(const char* info)   { (void)info; }
void audio_lasthost(const char* info)  { (void)info; }
void audio_commercial(const char* info){ (void)info; }
void audio_eof_speech(const char* info){ (void)info; }
void audio_process_time(uint32_t a, uint32_t b) { (void)a; (void)b; }

// ============ M3U/M3U8/PLS 解析 ============
String AudioPlayer::resolveStreamUrl(const String& url) {
  // ---- 兜底清洗(2026-09-18) ----
  // 导入的 TSV 曾被旧解析器把后面几列粘进地址(如 "...:7999/01\t0"), 库拿这种地址建连必然失败,
  // 表现就是"导入的电台全部无声"。数据侧已在 StationStore::cleanUrl 修好, 这里再兜一道 ——
  // 只要地址走到播放路径, 它就一定是干净的。
  // ★注意是"遇到空白就截断", 不是"删掉空白": 后者会把 "/01\t0" 粘成 "/010"(仍是错地址)。
  String u;
  u.reserve(url.length());
  for (size_t i = 0; i < url.length(); i++) {
    const char c = url[i];
    if ((uint8_t)c <= 0x20 || c == '"') break;   // 空格/Tab/CR/LF/控制字符/引号 -> 地址到此为止
    u += c;
  }
  // 协议归一: 把 https 强制降级为 http。
  // 根因(实测): HTTPS 流要额外做 TLS 握手 + 加密解密, 占内部堆与 CPU; 4M 版(无 PSRAM)上
  // 处理 https 流不稳定甚至完全起不来。国内电台 CDN 通常 http 同样可达且稳定, 故统一降级。
  // (S3 版对照实测: CNR 的 https 版 m3u8 不可播、http 版 200 可播 —— 降级在本板同样是正解。)
  if (u.startsWith("https://")) u = "http://" + u.substring(8);
  // 直连流地址(如 http://183.220.53.251:7999/01)无需任何预解析, 原样交给库即可。
  // HLS(.m3u8) 也原样交给库 —— 2.0.6 库自带 HLS 流程(GET m3u8→解析 .ts→processWebStreamTS
  // 解封装)。注意库在每个分片边界会同步阻塞着去拉下一个 .ts, 故 HLS 台对"喂流节奏"要求更高
  // (方案B 的音频泵任务正是为此; 4M 版无此任务, 故 4M 上 HLS 台更吃力)。
  // 上方已把 https 降级 http, 正好规避每个 .ts 分片重走 TLS 握手拖垮小板的问题。
  return u;
}

// ============ 播放前编码探测(本板可播性闸门) ============
// 放行 MP3/WAV 与各解码档(AAC / AAC+ / FLAC / VORBIS / OPUS 各自受 config.h 开关控制);
// HLS(m3u8) 现已放行(kind=1), 交给库内 HLS 流程处理(见下方 mpegurl 分支)。
// 2026-09-16: FLAC/OGG/OPUS/VORBIS 从"一律判死"改为可放行 —— 库里的 PSRAM 硬依赖已解开
// (详见 config.h 的 CODEC_ALLOW_* 说明与库文件顶部补丁注释)。
int AudioPlayer::probeCodec(const String& url) {
  String u = url; u.trim();
  if (u.startsWith("https://")) u = "http://" + u.substring(8);   // 本板统一走 http
  int p = u.indexOf("://");
  if (p < 0) return 0;
  String rest = u.substring(p + 3);
  int s = rest.indexOf('/');
  String host = (s < 0) ? rest : rest.substring(0, s);
  String path = (s < 0) ? String("/") : rest.substring(s);
  int port = 80;
  int c = host.indexOf(':');
  if (c >= 0) { port = host.substring(c + 1).toInt(); host = host.substring(0, c); }
  if (host.length() == 0) return 0;

  WiFiClient cli;
  cli.setTimeout(2);                       // 秒: 连接/读取超时
  if (!cli.connect(host.c_str(), (uint16_t)port)) { cli.stop(); return 0; }
  // 只取前 1KB(Range), 很多流服务器不支持 HEAD, 但都支持 GET+Range
  cli.print(String("GET ") + path + " HTTP/1.1\r\nHost: " + host +
            "\r\nRange: bytes=0-1023\r\nUser-Agent: WebRadio\r\nConnection: close\r\n\r\n");

  String hdr = "";
  uint32_t t = millis();
  while (millis() - t < 2500 && hdr.length() < 2048) {
    bool got = false;
    while (cli.available()) { hdr += (char)cli.read(); got = true; if (hdr.endsWith("\r\n\r\n")) break; }
    if (hdr.endsWith("\r\n\r\n")) break;
    if (!got) delay(1);                    // 关键: 空转必须让出 CPU, 否则饿死 IDLE -> task_wdt 重启
  }
  cli.stop();
  uint32_t usedMs = millis() - t;

  // ---- 排障: 把服务器"真实回给本机"的响应头原样打出来 ----
  // 这一步的价值: 它是【本固件自己】发起的请求, 不经过音频库, 因此不受 Core Debug Level 影响,
  // 能独立回答三个问题:
  //   ① 服务器到底给没给 Content-Type? 给的是什么(决定本板能不能播)?
  //   ② 响应头有没有收全(以 \r\n\r\n 结束)? 还是收到一半就没数据了(= 服务器侧问题)?
  //   ③ 有没有 Location(重定向)/WWW-Authenticate(要认证)/Transfer-Encoding(chunked)?
  {
    String shown = hdr;
    shown.replace("\r\n", " | ");
    shown.replace("\n",   " | ");
    if (shown.length() > 300) shown = shown.substring(0, 300) + "...";
    Serial.printf("[PROBE] %s\n", u.c_str());
    Serial.printf("[PROBE]   耗时 %ums, 收到头 %u 字节%s: %s\n",
                  (unsigned)usedMs, (unsigned)hdr.length(),
                  hdr.endsWith("\r\n\r\n") ? "(完整)" : "(★不完整=未读到空行, 服务器提前断了)",
                  shown.c_str());
  }

  String h = hdr; h.toLowerCase();
  int i = h.indexOf("content-type:");
  if (i < 0) {
    Serial.println("[PROBE]   判定: 未取到 Content-Type -> kind=0(交给库按老路尝试)");
    return 0;                              // 没拿到 => 未知, 交给库按老路尝试
  }
  String ct = h.substring(i + 13);
  ct.trim();
  int e = ct.indexOf('\n'); if (e > 0) ct = ct.substring(0, e);
  ct.replace("\r", "");
  ct.trim();

  // ---- 判定表(2026-09-16: 全格式放行, 逐档开关) ----
  // kind=1 放行 / kind=2 判死并自动跳台 / kind=0 未知(交给库嗅探)。
  // 档次顺序有讲究:
  //   ① opus 必须排在 ogg/vorbis 之前 —— "audio/ogg; codecs=opus" 两种子串都含, 先判更准。
  //   ② aacp 必须排在 aac 之前 —— "aacp" 里含子串 "aac", 否则 HE-AAC 会被当成 AAC-LC 放行。
  int kind;
  const char* tag;
  if      (ct.indexOf("mpegurl") >= 0) { kind = 1; tag = "HLS(m3u8)播放列表(走库内HLS流程)"; }
  else if (ct.indexOf("flac")    >= 0) { kind = CODEC_ALLOW_FLAC   ? 1 : 2; tag = "FLAC"; }
  else if (ct.indexOf("opus")    >= 0) { kind = CODEC_ALLOW_OPUS   ? 1 : 2; tag = "OPUS"; }
  else if (ct.indexOf("vorbis")  >= 0 ||
           ct.indexOf("ogg")     >= 0) { kind = CODEC_ALLOW_VORBIS ? 1 : 2; tag = "VORBIS / OGG"; }
  else if (ct.indexOf("aacp")    >= 0 ||
           ct.indexOf("aac+")    >= 0 ||
           ct.indexOf("he-aac")  >= 0 ||
           ct.indexOf("heaac")   >= 0) { kind = CODEC_ALLOW_AACPLUS ? 1 : 2;
                                         tag  = "AAC+/HE-AAC(★本板 SBR 已启用, 输出 96kHz)"; }
  else if (ct.indexOf("aac")     >= 0 ||
           ct.indexOf("mp4")     >= 0 ||
           ct.indexOf("m4a")     >= 0) { kind = CODEC_ALLOW_AAC ? 1 : 2; tag = "AAC-LC / M4A"; }
  else if (ct.indexOf("mpeg")    >= 0 ||
           ct.indexOf("mp3")     >= 0 ||
           ct.indexOf("wav")     >= 0) { kind = 1; tag = "MP3 / WAV"; }
  else                                 { kind = 0; tag = "未知(交给库嗅探)"; }
  Serial.printf("[PROBE]   判定: Content-Type=%s -> kind=%d [%s] %s\n",
                ct.c_str(), kind,
                kind == 2 ? "判死-自动跳台" : (kind == 1 ? "放行" : "交给库"), tag);
  return kind;
}

// ============ 播放控制(命令投递 + 主循环执行) ============
void AudioPlayer::begin() {
  cmdMutex   = xSemaphoreCreateMutex();
  audioMutex = xSemaphoreCreateRecursiveMutex();   // 控制面锁(泵任务与主循环共用)
  // ★2026-10-15 顺序铁律: 必须先 audio.begin() 把 I2S 驱动装好, 再 setPinout()。
  //   共享库(ESP32-audioI2S-nopsram)自 2026-10-12 起把 i2s_driver_install() 从【构造函数】
  //   整体搬到了 Audio::begin() —— 起因是经典 ESP32(无 PSRAM)那台: Audio 是全局对象,
  //   构造函数跑在调度器起来之前, 内部 DRAM 只剩 ~27KB 而 DMA 要 32KB, 分配必失败 ->
  //   上电无限重启。搬到 setup() 阶段装就没这个问题。
  //   而 setPinout() 内部只调 i2s_set_pin(), 驱动没装就返回 false, 引脚压根不生效 => 没声音。
  //   反过来也安全: 若将来库改回构造函数安装, begin() 会因 m_f_i2sInstalled=true 提前返回。
  audio.begin();
  bool ok = audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  // ★关键(修"断断续续"): 必须把第二个参数给成 >0, 库才会把输入缓冲放进 PSRAM。
  // 库默认输入缓冲仅 8KB; 第二个参数写 0 会退回 DRAM 的小缓冲(并且日志会误报 "PSRAM not found")。
  // 本板 S3 有 8MB OPI PSRAM, 缓冲给到 AUDIO_PSRAM_BUFSIZE(512KB):
  //   128kbps 台 ≈ 32 秒余量; 200kbps 的 HE-AAC 台 ≈ 20 秒余量 —— 主循环偶发长卡顿也吃得下。
  // 必须早于第一次 connecttohost(): 缓冲一旦 init 后再改无效(库会 log_e 并忽略)。
  audio.setBufsize(AUDIO_RAM_BUFSIZE, AUDIO_PSRAM_BUFSIZE);
  // PSRAM 状态显式上报。库那句 "PSRAM not found" 会把"参数传 0"也算进去, 容易被误判成硬件没有
  // PSRAM —— 这里给硬数据: 只有 psramFound()=0 才真的是 fqbn 漏了 PSRAM=opi。
  Serial.printf("[Audio] PSRAM: %s | 总 %uKB 空闲 %uKB | 输入缓冲 RAM %uB + PSRAM %uB\n",
                psramFound() ? "已启用" : "★未启用(fqbn 缺 PSRAM=opi?)",
                (unsigned)(ESP.getPsramSize() / 1024), (unsigned)(ESP.getFreePsram() / 1024),
                (unsigned)AUDIO_RAM_BUFSIZE, (unsigned)AUDIO_PSRAM_BUFSIZE);
  audio.setVolume(volToLib(gState.volume));
  // 诊断输出: setPinout 返回 false 表示 i2s_set_pin 失败(驱动未就绪), 引脚不会生效 -> 必然无声
  Serial.print("[Audio] setPinout(BCLK=" + String(I2S_BCLK) +
               ", LRC="  + String(I2S_LRC) +
               ", DOUT=" + String(I2S_DOUT) + ") -> ");
  Serial.println(ok ? "OK" : "失败!(I2S驱动未就绪, 引脚未生效)");
  Serial.println("[Audio] 音量 = " + String(gState.volume) + "/" + String(VOLUME_MAX) +
                 " (0=静音)");
  // 2026-09-16 切回 2.0.6: 该库【单线程】—— audio.loop() 一个人包办"拉流 + 解码 + 写 I2S",
  // 没有独立解码任务, 也【没有任何内部锁】。于是有两条硬约束:
  //   ① 喂流节奏 = 调用间隔(间隔 > I2S DMA 深度就断音);
  //   ② 库绝不可被并发调用。
  // 2026-09-19 方案B: 把 audio.loop() 交给一个【独占的高优先级泵任务】(见 pumpBody)高频调用,
  //   主循环只负责取命令 / ICY 元数据 / 健康监测。这样主循环在软件 SPI 整屏刷新(56~63ms)
  //   或 Flash 落盘时, 泵任务会【抢占】它继续喂流 —— 喂流间隔不再被 UI 拖累,
  //   "整帧跳过"由"救命补丁"降级为"省 CPU 的优化"。
  // ⚠ 曾有一版注释写"解码由库自带 audioTask 负责"——那是 3.0.13 的模型, 本版 2.0.6 fork
  //   里根本不存在, 留着会严重误导并发改造, 已删。
  // 启动泵任务: 必须放在上面所有 audio 初始化(setPinout / setBufsize / setVolume)之后。
  startPumpTask();
}

// ============ 音频泵任务(方案B, 2026-09-19) ============
// 职责单一: 高频调用 audio.loop()(拉流 + 解码 + 写 I2S), 别的什么都不干。
// 为什么值得单开一个任务 —— 库是单线程的, "audio.loop() 的调用间隔"就是音质本身:
//   主循环里一次整屏刷新 56~63ms、一次 LittleFS 落盘、一次网页请求处理, 都会把喂流推迟
//   同样长的时间。泵任务优先级(AUDIO_TASK_PRIO=3) > loopTask(1), 主循环不管在忙什么, 泵任务都能抢占它按时喂流。
// 并发边界(必须严格遵守):
//   · 泵任务只碰 audio 对象, 且全程持 audioMutex —— 与主循环侧的 stopSong / connecttohost /
//     beep 严格互斥, 保证库【永不被并发调用】(库里没有任何锁, 全靠这里);
//   · 泵任务【绝不碰 gState】(跨任务写 Arduino String 会 double-free 堆损坏);
//     曲名交接走文件顶部的 s_titlePending + 代次号, 由主循环取走落地。
void AudioPlayer::startPumpTask() {
  if (pumpTaskHandle) return;                       // 幂等: 重复调用不重复创建
  // ★创建梯子(2026-09-19 加): FreeRTOS 的任务栈必须是【内部 DRAM 里的一块连续内存】,
  //   而这块内存要在 WiFi + lwIP + TLS 都吃过后还拿得到。栈要得越大越容易失败, 而失败
  //   以前的后果是"静默回退成主循环喂流"—— 音频解码于是和 LCD 整屏刷新挤在一条线程上,
  //   现象是"一直卡卡卡"(实测过, 比改造前还差), 却完全看不出根因。
  //   所以: 先按配置值要, 拿不到就退一半再要一次, 两次都失败才认输(此时主循环会接管喂流,
  //   至少不会没声)。真实生效的栈大小打在下面这行日志里。
  const uint32_t want[2] = { (uint32_t)AUDIO_TASK_STACK, (uint32_t)(AUDIO_TASK_STACK / 2) };
  uint32_t got = 0;
  for (int i = 0; i < 2 && !pumpTaskHandle; i++) {
    if (want[i] < 2560) break;                      // 再小就没有意义了(idle 任务才 1KB 级)
    BaseType_t ok = xTaskCreatePinnedToCore(
        pumpEntry, "audiopump", want[i], this, AUDIO_TASK_PRIO,
        &pumpTaskHandle, AUDIO_TASK_CORE);
    if (ok == pdPASS) { got = want[i]; break; }
    pumpTaskHandle = nullptr;
    Serial.printf("[Audio] 音频泵任务创建失败(要 %uB 连续内部DRAM拿不到) -> 退一档重试\n",
                  (unsigned)want[i]);
  }
  if (!pumpTaskHandle) {
    // 创建失败 = 没有任务喂流。这里必须显式报出来, 不要让故障表现成"莫名其妙变卡/没声音"。
    Serial.printf("[Audio] ★★音频泵任务创建失败(内部堆不足) -> 已回退主循环喂流: 声音能出, 但会受 UI 卡顿影响\n");
    Serial.printf("[Audio] ★★当前 内部堆%uKB 最大块%uKB —— 请把这行发出来\n",
                  (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getMaxAllocHeap() / 1024));
    return;
  }
  Serial.printf("[Audio] 音频泵任务已启动: core%d 优先级%d 栈%uB(实配) —— 喂流不再受 UI/落盘影响\n",
                AUDIO_TASK_CORE, AUDIO_TASK_PRIO, (unsigned)got);
}

// 挂起泵任务。★关机/深睡前必须调用: 紧接着要 WiFi.disconnect() / esp_wifi_stop(),
// 若此刻泵任务正卡在 audio.loop() 的 socket 读里, 网络被抽走会让库内部状态异常
// (这是方案B 引入的新并发面, 旧单线程结构天然不存在)。
void AudioPlayer::suspendPumpTask() {
  if (!pumpTaskHandle) return;                      // setup 早期(尚未创建)调用时是安全的 no-op
  vTaskSuspend(pumpTaskHandle);
  Serial.println("[Audio] 音频泵任务已挂起(关机/深睡前, 不再碰网络)");
}

void AudioPlayer::pumpEntry(void* arg) {
  static_cast<AudioPlayer*>(arg)->pumpBody();
}

// 切换喂流模式(串口 P)。★这是"防呆"的现场开关: 一旦怀疑方案B(泵任务)有问题, 不用重新烧录,
// 发个 P 就能退回"主循环喂流"(方案A, 与改造前完全一致), 立刻判断问题是否出在泵任务上。
// 只改一个 volatile bool, 不需要挂起/恢复任务 ⇒ 不存在"挂起时正持锁"导致的死锁风险。
void AudioPlayer::setPumpEnabled(bool on) {
  pumpEnabled = on;
  if (on) {
    Serial.printf("[Audio] 喂流模式 -> 音频泵任务(方案B) | 泵任务%s | 泵 %luHz\n",
                  pumpTaskHandle ? "已创建" : "★未创建(会回退到主循环喂流)",
                  (unsigned long)pumpHz);
  } else {
    Serial.println("[Audio] 喂流模式 -> 主循环(方案A 回退) —— 泵任务保留但不再喂流");
  }
}

// 泵任务的栈余(字节)。FreeRTOS 的 uxTaskGetStackHighWaterMark 以"字"为单位, 乘 StackType_t 换字节。
// 为什么单独提供: 泵任务里跑的 audio.loop() 会做深层嵌套的流解析/解码, 而整个固件的音质就押在
// 这个任务上 —— 它的栈水位是最该盯的一个数(栈溢出会直接重启, 现象与"死机"一样)。
uint32_t AudioPlayer::pumpStackFree() {
  return pumpTaskHandle
       ? (uint32_t)uxTaskGetStackHighWaterMark(pumpTaskHandle) * sizeof(StackType_t) : 0;
}

void AudioPlayer::pumpBody() {
  uint32_t cnt = 0, hzMs = millis();
  for (;;) {
    pumpTick++;                                     // 心跳: 失速看门狗据此判断"喂流是否还在转"
    // ---- 间隔探针: 两次 audio.loop() 之间最长隔了多久 ----
    // 这是方案B 下判定音频健康的第一指标: 它稳定在几毫秒 = 喂流没被任何东西拖住。
    {
      const uint32_t us = micros();
      if (s_lastLoopUs) {
        const uint32_t gap = us - s_lastLoopUs;     // unsigned 差值, 溢出安全
        if (gap > s_loopMaxGapUs) s_loopMaxGapUs = gap;
      }
      s_lastLoopUs = us;
    }
    // ---- 持锁喂流 ----
    // 锁竞争时本任务【阻塞等待】(不忙等): IDLE 得以运行, 不会触发 task_wdt。
    // 最坏情况是主循环正在 connecttohost(换台, 可能数百 ms ~ 数秒), 本任务等它做完 ——
    // 这正是期望的顺序: 换台期间本就不该继续喂旧流。
    // ★pumpEnabled=false 时本任务【不喂流】(仅维持心跳), 由主循环接管 —— 见 config.h 的「防呆设计」。
    //   这样做的价值: 万一泵任务在某个板子/某版库上有问题, 声音不会跟着一起没, 而是自动退回旧方案。
    if (pumpEnabled) {
      const uint32_t t0 = micros();
      lockAudio();
      audio.loop();
      unlockAudio();
      const uint32_t d = micros() - t0;
      if (d > g_phMaxUs[PH_AUDIOLOOP]) g_phMaxUs[PH_AUDIOLOOP] = d;
    }
    // 泵频率统计(诊断: 喂流节奏的真实指标)
    cnt++;
    uint32_t now = millis();
    if (now - hzMs >= 1000) {
      pumpHz = cnt * 1000 / (now - hzMs);
      cnt = 0; hzMs = now;
    }
    // 让出 1 个 tick(本工程 CONFIG_FREERTOS_HZ=1000, 即 1ms)。
    // 不能省: 否则 loopTask 与 IDLE 会被饿死(task_wdt 重启), UI 也会完全卡死。
    vTaskDelay(1);
  }
}

// ============ I2S 输出自检(诊断"无声"用) ============
// 绕过网络与解码器, 直接向 I2S 写 1kHz 方波, 用于二分定位故障层级:
//   有声 -> I2S 引脚 + 功放硬件正常, 问题在网络/解码/音量
//   无声 -> I2S 引脚配置未生效 或 功放硬件问题(尤其 MAX98357 的 SD_MODE 未拉高)
// 注意: I2S 通道由【库自己】安装并持有(本工程用的 2.0.6 fork 走 IDF 的旧 I2S 驱动
//       i2s_driver_install), 所以自检必须走库的 audio.beep()。
//       ★本固件任何地方都不得 include driver/i2s.h 或自建 I2S 通道 —— 两套 I2S 驱动同时存在会 abort() 重启。
void AudioPlayer::beepTest() {
  // 若切台静音 ramp 进行中, 先结束并恢复原音量, 否则 beep 会因 DOUT=0 而无声(自检失效)
  if (m_muteRampStep) { audio.setVolume(volToLib(gState.volume)); m_muteRampStep = 0; }
  Serial.println("[BEEP] 开始 I2S 自检: 1kHz 方波, 约1.5秒 (请贴近喇叭听)");
  // 整段自检独占 I2S: 泵任务被挡在 audioMutex 外, 不会与本自检的 i2s_channel_write 抢 DMA
  markStage(14);                           // 面包屑: I2S 自检
  lockAudio();
  audio.stopSong();
  audio.beep(1000, 1500);                  // 走 NEW 驱动, 与库一致
  unlockAudio();                           // 自检结束, 放泵任务继续解码
  Serial.println("[BEEP] 自检结束。听到\"嘟\"声=I2S+功放正常; 无声=查 I2S 接线/SD_MODE");
  if (gState.playing) execReconnect();   // 自检后立即恢复播放, 免得看门狗误判成断流而跳台
}

void AudioPlayer::setVolume(int v) {
  gState.volume = constrain(v, 0, VOLUME_MAX);
  audio.setVolume(volToLib(gState.volume));   // 库实现只写单个 int 变量, 无锁调用线程安全
  if (store) store->saveVolume(gState.volume);   // 记忆音量, 重启后恢复
}

// ---- 命令投递(任何线程可调; 真正的音频操作在主循环 processCmd 执行) ----

void AudioPlayer::issueCmd(uint8_t c, int idx) {
  xSemaphoreTake(cmdMutex, portMAX_DELAY);
  pendingCmd = c;
  pendingIdx = idx;
  xSemaphoreGive(cmdMutex);
}

void AudioPlayer::play(const String& url) {
  xSemaphoreTake(cmdMutex, portMAX_DELAY);
  pendingUrl = url;
  pendingIdx = -1;
  pendingCmd = CMD_PLAYURL;
  xSemaphoreGive(cmdMutex);
}

void AudioPlayer::reconnect()   { issueCmd(CMD_RECONNECT); }
void AudioPlayer::playCurrent() { issueCmd(CMD_PLAYIDX, -1); }
void AudioPlayer::requestPlayIdx(int i) { issueCmd(CMD_PLAYIDX, i); }
void AudioPlayer::stop()        { issueCmd(CMD_STOP); }
void AudioPlayer::togglePlay()  { issueCmd(CMD_TOGGLE); }
void AudioPlayer::next()        { issueCmd(CMD_NEXT); }
void AudioPlayer::prev()        { issueCmd(CMD_PREV); }
void AudioPlayer::setSleepTimer(int min) { issueCmd(CMD_SLEEP, min); }
//定时关机: 独立命令而非给 setSleepTimer 加参数 —— 见audio_player.h 的说明。
void AudioPlayer::setSleepTimerAt(int min, bool toPowerOff) {
  issueCmd(toPowerOff ? CMD_SLEEP_PWROFF : CMD_SLEEP, min);
}
// ---- SD 本地播放(阶段②): 同样只是投递命令, 不碰 SD 卡也不碰 audio 对象 ----
void AudioPlayer::playSdFile(int idx) { issueCmd(CMD_SD_PLAY, idx); }
void AudioPlayer::playSdNext()       { issueCmd(CMD_SD_NEXT); }
void AudioPlayer::exitSdMode()       { issueCmd(CMD_SD_EXIT); }

// ---- stopSong 事故源: 库内部【没有任何锁】, stopSong() 与 audio.loop() 并发即损坏 ----
// 实测库的 stopSong(): 直接 memset(m_outBuff,0) + i2s_zero_dma_buffer()。若它与音频泵任务
// 正在跑的 audio.loop() 并发 -> 输出缓冲/DMA 状态损坏 -> 换台太快直接死机。
//
// 历史上被否决的解法: "置 stopReq 让泵代执行 + 2 秒超时兜底裸调 stopSong()"。
// 因为泵任务的 audio.loop() 内部会阻塞 —— HLS(m3u8)分支里它自己去拉下一个 .ts 分片,
// 网络慢时数秒不返回, stopReq 得不到响应 -> 超时 -> 主循环裸调 stopSong()
// -> 恰恰在最危险的时刻(泵卡在网络 IO、解码状态未落定)造成并发损坏。这就是
// "后台切台仍偶发死机"的残留点。
//
// 现行解法(方案B 沿用, 并成为并发安全的唯一支柱): 一把递归锁 audioMutex。
// 泵任务每圈 audio.loop() 持锁, 主循环侧所有碰 audio 对象的操作
// (stopSong / connecttohost / beep)也持同一把锁, 二者严格互斥;
// 等待发生在信号量上(任务阻塞, IDLE 得以运行, 不会触发 task_wdt),
// 不存在"超时退化为不安全调用"的可能。
void AudioPlayer::lockAudio()   { if (audioMutex) xSemaphoreTakeRecursive(audioMutex, portMAX_DELAY); }
void AudioPlayer::unlockAudio() { if (audioMutex) xSemaphoreGiveRecursive(audioMutex); }

void AudioPlayer::stopSongSafe() {
  lockAudio();
  audio.stopSong();     // 此刻泵任务绝不在 audio.loop() 内部
  unlockAudio();
}

// ---- 切台静音: 立刻把音量压到 0(库 Gain() 在 m_vol=0 时强制 DOUT=0 = 真静音),
// 灭掉 2.0.6 库 stopSong() 只清 DMA 不停 I2S 带来的间隙嘶声 + 新流起播 0→首样本爆破音。
// 不写 store、不改动 gState.volume(用户保存的音量), 仅临时压低库内增益。
void AudioPlayer::beginSwitchMute() {
  if (gState.volume <= 0) return;             // 本来就是静音, 无需处理
  m_muteTarget = (int8_t)gState.volume;       // 记住用户音量, 待恢复
  m_muteCur   = 0;
  audio.setVolume(0);                         // 立即压到 0(库内部 m_vol=0 -> Gain 输出恒 0)
  m_muteRampStep = 1;                         // 进入"等待新流出声"阶段
  m_muteRampMs = millis();
  Serial.println("[Audio] 切台静音: 音量压到0, 待新流出声后 ramp 恢复");
}

// ---- 切台静音后的音量平滑恢复(主循环每圈调用) ----
// step1: 等【库真的开始出声了】再恢复音量 —— 判据是"库在跑 且 码率已识别"(= HTTP 头已解析、
//        解码链已建立)。
// ★★2026-09-19 晚 修正(这是一次实打实的误改): 今天早些时候把这里的判据改成了
//   "输入缓冲 >= AUDIO_PREBUF_BYTES(32KB)", 想法是"静音窗口与库故意不出声的窗口重合"。
//   但那只有【闸门开着】时才成立! 闸门一关(库按上游行为、攒一帧就出声并持续消耗), 输入缓冲
//   就是"进多少出多少"的真实水位, **永远攒不到 32KB** ⇒ 判据永不满足 ⇒ 每次换台音量都被压在 0,
//   一直等 8 秒兜底才抬起来 —— 用户体感就是"换台后先哑十来秒", 比改动前更差。
//   已改回可达判据(等价于最初的"缓冲>256 字节"意图, 只是要求库确实已进入运行态)。
//   8s 兜底也收紧到 2.5s: 它只该是"这台真连不上"时的逃生, 不该变成常态等待。
// step2: 每 ~15ms 抬一档(共约 8 档), 灭 0→原值的起播爆破音。
// ★注意: 本判据只负责"何时恢复音量", 不解决"库刚开始出声那几秒缓冲低位导致的卡顿" ——
//   那属于起播预缓冲(闸门)的职责; 现已重新开启(见 config.h), 换台后头几秒由预缓冲静默吃掉,
//   出声即流畅, 故此处无需再处理起播卡顿。
void AudioPlayer::rampSwitchVolume() {
  if (m_muteRampStep == 0) return;
  uint32_t now = millis();
  if (m_muteRampStep == 1) {
    // 可达判据: 库已运行 + 码率已识别(HTTP 头解析完成) + 首声已开始(prebufferDone)。
    // ★最后一项让静音精确覆盖"预缓冲静默期": 若提前解除静音, 待首声到来时音量已到顶,
    //   起播爆破音风险上升。prebufferDone 在预缓冲关闭时恒为 true, 不影响原行为。
    bool flowing = audio.isRunning() && audio.getBitRate() > 0 && audio.prebufferDone();
    if (flowing || (now - m_muteRampMs > 2500)) {   // 2.5s 兜底(该台真连不上时不能一直哑着)
      m_muteRampStep = 2;
      m_muteRampMs = now;
    }
    return;
  }
  if (now - m_muteRampMs < 15) return;       // 每档间隔 ~15ms
  m_muteRampMs = now;
  int stepUp = max(1, (int)m_muteTarget / 8);
  m_muteCur += (int8_t)stepUp;
  if (m_muteCur >= m_muteTarget) {
    m_muteCur = m_muteTarget;
    m_muteRampStep = 0;
    Serial.println("[Audio] 切台静音恢复完成");
  }
  audio.setVolume(volToLib(m_muteCur));       // 仅改库内增益, 不写 store
}

// ---- 起播预缓冲现场开关(串口 B) ----
// 在 0(关, 纯上游行为)与 AUDIO_PREBUF_BYTES(32KB)之间切换; 只作用于下一台, 不打断当前台。
// 用途: 现场 A/B 对比"开预缓冲(换台静默稍久但出声即流畅)" vs "关预缓冲(出声最快但起播略卡)"。
void AudioPlayer::togglePrebuf() {
  uint32_t cur  = audio.getPrebufBytes();
  uint32_t next = (cur > 0) ? 0 : (uint32_t)AUDIO_PREBUF_BYTES;
  audio.setPrebufBytes(next);
  Serial.printf("[Audio] 起播预缓冲 %s (%uKB) —— 下一台起生效, 当前台不打断\n",
                next > 0 ? "开" : "关", (unsigned)(next / 1024));
}

uint32_t AudioPlayer::prebufBytes() const { return audio.getPrebufBytes(); }

// ---- 命令执行器(只在主循环线程运行) ----

void AudioPlayer::processCmd() {
  if (pendingCmd == CMD_NONE) return;      // 快路径: 无锁读一次
  uint8_t cmd; int idx; String url;
  xSemaphoreTake(cmdMutex, portMAX_DELAY);
  cmd = pendingCmd; idx = pendingIdx; url = pendingUrl;
  pendingCmd = CMD_NONE;
  xSemaphoreGive(cmdMutex);

  switch (cmd) {
    case CMD_PLAYURL:  execPlayUrl(url); break;
    case CMD_PLAYIDX:  execPlayIdx(idx); break;
    case CMD_STOP:     execStop(); break;
    case CMD_TOGGLE:   execToggle(); break;
    case CMD_RECONNECT: execReconnect(); break;
    case CMD_SLEEP:     execSleep(idx); break;
    case CMD_SLEEP_PWROFF: execSleepPowerOff(idx); break;
    case CMD_SD_PLAY:  execSdPlay(idx); break;
    case CMD_SD_NEXT:  execSdNext(); break;
    case CMD_SD_EXIT:  execSdExit(); break;
    case CMD_NEXT: case CMD_PREV: {
      // ★SD 模式下 next/prev 走"下一个本地文件", 不动电台表(两种音源的"下一首"含义不同)
      if (gState.sdActive) { execSdNext(); break; }
      // 连发合并: 快速连按 N 次切台只做一次"跳 |N| 台", 避免排队重复连接
      int dir = (cmd == CMD_NEXT) ? 1 : -1;
      xSemaphoreTake(cmdMutex, portMAX_DELAY);
      while (pendingCmd == CMD_NEXT || pendingCmd == CMD_PREV) {
        dir += (pendingCmd == CMD_NEXT) ? 1 : -1;
        pendingCmd = CMD_NONE;
      }
      xSemaphoreGive(cmdMutex);
      execStep(dir);
      break;
    }
    default: break;
  }
}

void AudioPlayer::execPlayUrl(const String& url) {
  if (!gState.wifiConnected) { gState.status = "未连网"; return; }
  // ★从 SD 模式切回网络电台: 清掉音源标志, 否则看门狗会继续按"本地文件"处理(见 loop())
  gState.sdActive = false;
  gState.sdIndex  = -1;
  gState.sdName   = "";
  // 切台前先压音量到 0(灭间隙嘶声 + 起播爆破音), 等出新流后由 loop() 的 rampSwitchVolume 平滑恢复
  beginSwitchMute();
  // 切台前先主动 stopSong(), 尽早释放解码器/TLS 缓冲(防堆累积耗尽)
  stopSongSafe();
  gState.title = "";                 // 切换电台: 清掉旧曲名(新台连上后会刷新)
  gState.audioEverFlowed = false;    // 本次连接尚未出声, 看门狗用更长建连宽限
  markStage(11);                     // 面包屑: 即将解析/清洗电台地址(resolveStreamUrl: 只做清洗+协议归一)
  String real = resolveStreamUrl(url);
  if (real == "") { gState.status = "读取失败"; markStage(6); return; }
  gState.url = real;                 // 记住真实流地址
  // ---- 本板可播性闸门 ----
  // 探一次 Content-Type: 已知解不动的编码判死并自动跳下一台, 避免送进解码器 OOM 无声(见 probeCodec)。
  int codecKind = probeCodec(real);
  if (codecKind == 2) {
    int total = (store && store->count() > 0) ? store->count() : 1;
    if (total > 1 && ++unsupportedStreak < total) {
      Serial.println("[Audio] 跳过: 已知本板不支持的编码 -> " + real);
      gState.title  = "该编码本板不支持";
      gState.status = "跳过";
      // ★置标志而不是 execStep(1) 直接递归 —— 递归会随"连续跳过台数"加深,
      //   几十层就能撑爆 loopTask 的 8KB 栈, 且递归期间不喂失速看门狗(>15s 即被重启)。
      //   交给 loop() 下一圈处理: 栈深恒为 1, 每圈喂一次看门狗, UI/Web 也不会被长时间冻结。
      skipNext = +1;
    } else {
      unsupportedStreak = 0;
      gState.playing = false;
      gState.status  = "无可播台";
      gState.title   = "无可播电台";
      Serial.println("[Audio] 一圈 " + String(total) +
                     " 台全部为本板不支持的编码 —— 已停止");
    }
    return;
  }
  unsupportedStreak = 0;             // 遇到可播台: 连续跳过计数归零
  skipNext = 0;                      // 已成功落到可播台 -> 撤销任何待执行的跳过
  startMeta(real);                   // 独立连接跟踪 ICY 节目信息(主循环线程, 与 updateMeta 同线程)
  markStage(12);                     // 面包屑: 即将进入库的连流(阻塞, 崩溃高发点)
  // ★必须持 audioMutex(方案B 新增): connecttohost 会长时间(可能数百 ms~数秒, HLS 分片更久)
  //   改写库内部状态, 而音频泵任务正在另一个核上跑 audio.loop() —— 两者并发就是未定义行为。
  //   本调用期间泵任务在锁上阻塞等待(不喂流), 但此刻本就处于"刚停掉旧流、正在建新流"的切换窗口,
  //   属预期行为。
  lockAudio();
  audio.connecttohost(real.c_str());
  unlockAudio();
  markStage(6);                      // 连流返回, 回到常规运行
  // ---- 建连结果立即回报(不等下一个 DIAG 周期) ----
  // 库内部"建连成功但解码器初始化失败"时会静默 stopSong() 把 m_f_running 置 0;
  // 若不在此查, 现象就是界面一直"连接中"却永远没声。判读:
  //   运行=0            -> 库已放弃, 看上面 [LIB:E]/[LIB:I] 行定位(超时/解码器没内存/连不上)
  //   编码=unknown      -> 连上了但 HTTP 头没解析出类型
  //   编码=AAC 且运行=1 -> 链路正常却无声, 才需往流格式(HE-AAC 无 SBR)或 I2S 方向查
  Serial.printf("[Audio] 建连返回: 运行=%d 编码=%s 码率=%ubps 解码错=%d | 堆 %uKB 最大块 %uKB | %s\n",
                audio.isRunning() ? 1 : 0, audio.getCodecname(), (unsigned)audio.getBitRate(),
                (int)audio.getDecodeError(),
                (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getMaxAllocHeap() / 1024),
                audio.isRunning() ? "已建连" : "★失败(库已内部放弃, 查上面 [LIB:E] 行)");
  // ★回退(2026-09-19 晚): 这里【不再】用 audio.isRunning() 决定 playing。
  //   原因: connecttohost() 返回的那一瞬间, 库的 m_f_running 未必已置位(尤其 HLS 台要先取
  //   playlist + 首个分片), 一旦此刻读到 0 就会把 playing 永久钉成 false, 而【全固件再没有
  //   任何地方会把它改回 true】。后果是连锁的、且都很难查:
  //     · rampSwitchVolume() 不再认为"在播放" -> 换台静音永不恢复 -> 看起来像没声音;
  //     · updateAudioHealth() 的欠载统计被关掉 -> 卡顿没人管;
  //     · 失速看门狗 if(!gState.playing) return -> 死了的流永远不自愈;
  //     · 屏上一直"停止"。
  //   playing 的语义就是"用户意图是播放", 由命令/状态机驱动最稳; 建连成不成功交给下面这行日志
  //   和 [WD] 自愈去表达。★这类"用瞬时状态去改写长期状态"的改动最危险, 已记入项目记忆。
  gState.playing = true;
  gState.status  = "连接中";
  if (!audio.isRunning())
    Serial.println("[Audio] ★建连返回时库仍未进入运行态(见上一条日志; 库可能稍后才起播)");
  s_outStarted = false;              // 新台: 重新开始等"库真的出声"(见 loop() 里的锁存判据)
  gState.lastDataMs = millis();
  gState.failCount = 0;              // 用户主动操作/切台 -> 失败计数归零
  prevInFill = 0;                    // 缓冲基线归零, 避免旧值误判"缓冲在增长"
  Serial.println("[Audio] 播放: " + real);
}

void AudioPlayer::execPlayIdx(int idx) {
  if (!store || store->count() == 0) { gState.status = "没电台"; return; }
  if (idx < 0) idx = store->getLast();          // -1 = 播上次电台
  gState.index = idx % store->count();
  store->setLast(gState.index);
  gState.stationName = store->name(gState.index);
  execPlayUrl(store->url(gState.index));
}

void AudioPlayer::execStop() {
  // 若切台静音 ramp 进行中, 立即结束并恢复原音量(避免停播后音量永远停在 0)
  if (m_muteRampStep) { audio.setVolume(volToLib(gState.volume)); m_muteRampStep = 0; }
  markStage(13);
  stopSongSafe();
  markStage(6);
  stopMeta();                // 关闭独立元数据连接
  gState.playing = false;
  gState.status = "停止";
  gState.url = "";           // 停播后清空地址
  gState.title = "";         // 停播后清空节目信息
  gState.sleepMin = 0; gState.sleepRemainSec = 0;   // 手动停播 = 取消睡眠定时器
  gState.sleepStandby = false;                      // ★同时退出"熄屏待机"(见 player_state.h 该字段注释)
  prevInFill = 0;
  skipNext = 0;              // 用户明确"停止" -> 撤销待执行的自动跳台
  // ★SD 模式保留(不退出): 用户按停止后源仍是 SD, 再按一次播放键应该接着放同一首,
  //   而不是跳去网络电台。sdActive / sdIndex 保留, 由 execToggle 判断该重播还是重连电台。
  if (gState.sdActive) {
    gState.status = "SD 停止";
    gState.url    = "";      // 地址已无意义(停播), 诊断页不显示半截路径
  }
}

void AudioPlayer::execReconnect() {
  // ★SD 源的"重连"语义 = 重播当前这首(没有服务器会断, 只有文件读完/解码失败才需要重来)
  if (gState.sdActive) { execSdPlay(mSdLastIdx); return; }
  if (!gState.wifiConnected) { gState.status = "未连网"; return; }
  if (gState.url == "") { execPlayIdx(-1); return; }   // 没有记忆地址则退回播放上次电台
  // 重连前先压音量到 0(重连也是一次停旧流+起新流的切换, 同样会嗤嗤), 等出新流后 ramp 恢复
  beginSwitchMute();
  // 重连前先主动停一遍旧流, 尽早释放解码器/TLS 缓冲(防反复重连吃光堆)
  stopSongSafe();
  gState.title = "";
  gState.audioEverFlowed = false;    // 重新建连, 此次尚未出声
  s_outStarted = false;              // 同上: 重新等"库真的出声"
  startMeta(gState.url);
  lockAudio();                       // ★同 execPlayUrl: 与音频泵任务的 audio.loop() 互斥
  audio.connecttohost(gState.url.c_str());
  unlockAudio();
  gState.playing = true;
  gState.status = "重连中";
  gState.lastDataMs = millis();      // 给本次重连一个观察窗口, 避免忙等
  prevInFill = 0;
}

void AudioPlayer::execToggle() {
  if (gState.playing) execStop();
  // ★SD 源暂停后再按播放 = 重播同一首(不是去播电台), 见 execStop 末尾的说明
  else if (gState.sdActive) execSdPlay(mSdLastIdx);
  else execPlayIdx(-1);
}

void AudioPlayer::execStep(int dir) {
  if (!store || store->count() == 0) return;
  int n = store->count();
  int i = gState.index + dir;
  i = ((i % n) + n) % n;             // 支持 ±N(连发合并), C 的 % 对负数不友好故双重取模
  gState.index = i;
  store->setLast(i);
  gState.stationName = store->name(i);
  execPlayUrl(store->url(i));
}

// ---- 睡眠定时器(阶段①: 纯软件) ----
// min=0 -> 取消(清掉剩余与设定); 否则夹紧到 [1, SLEEP_TIMER_MAX_MIN] 并启动倒计时。
// ★仅在主循环调用: gState 的 int 字段在此线程写安全(异步 WS/HTTP 只经 issueCmd 投递到这里)。
// ★每次都显式写 sleepToPowerOff —— 两套定时器共用同一组计数字段, 不清就会串味:
//   用户先用面板设了"定时关机", 再用后台设"睡眠定时器"时, 若这里不清标志,
//   后者到点会继续去关机(与用户刚设的意图相反)。后设的赢, 所以这里必须覆盖。
void AudioPlayer::execSleep(int min) {
  gState.sleepToPowerOff = false;
  // ★注意: 这里【不】清 gState.sleepStandby —— "重新设定时器 = 退出熄屏待机并点亮屏"这件事
  //   统一由主循环处理(它才持有显示层), 判据 = sleepRemainSec>0 && sleepStandby。
  //   音频模块只负责把倒计时设起来, 不碰显示。
  if (min <= 0) {
    if (gState.sleepRemainSec > 0 || gState.sleepMin > 0)
      Serial.println("[SLEEP] 已取消睡眠定时器");
    gState.sleepMin = 0;
    gState.sleepRemainSec = 0;
    return;
  }
  if (min > SLEEP_TIMER_MAX_MIN) min = SLEEP_TIMER_MAX_MIN;
  gState.sleepMin       = min;
  gState.sleepRemainSec = min * 60;
  mSleepMs = millis();             // 对齐到当前时刻, 避免首秒提前跳变
  Serial.printf("[SLEEP] 睡眠定时器设定 %d 分钟 (剩余 %d 秒) —— 到点仅停播+熄屏\n",
                min, gState.sleepRemainSec);
}

// ---- 定时关机(红外 IR_TIMER 专用): 与上面同一套倒计时, 但到点【真正进深睡关机】----
// 单独一个执行器而不是给 execSleep 加参数: 让"到点关机"这件事在代码里是显式的一行,
// 读日志的人一眼能看出这次设定的是关机而不是熄屏。
void AudioPlayer::execSleepPowerOff(int min) {
  gState.sleepToPowerOff = true;
  // 同 execSleep: 不清 sleepStandby, 由主循环统一处理"重设定时器 -> 亮屏 + 退出待机"。
  if (min <= 0) {
    if (gState.sleepRemainSec > 0 || gState.sleepMin > 0)
      Serial.println("[SLEEP] 已取消定时关机");
    gState.sleepMin = 0;
    gState.sleepRemainSec = 0;
    gState.sleepToPowerOff = false;   // 取消时也要把标志复位, 否则下次归零会误关机
    return;
  }
  // 上限对齐 SLEEP_TIMER_MAX_MIN(720 分 = 12 小时), 与红外循环的最后一档一致
  if (min > SLEEP_TIMER_MAX_MIN) min = SLEEP_TIMER_MAX_MIN;
  gState.sleepMin       = min;
  gState.sleepRemainSec = min * 60;
  mSleepMs = millis();
  Serial.printf("[SLEEP] ★定时关机设定 %d 分钟 (%d 小时) —— 到点将【真正关机进深睡】\n",
                min, min / 60);
}

// ===================== 本地 SD 卡播放(阶段②) =====================
// 三条铁律(与网络电台侧完全一致, 不另立一套):
//   ① 只在主循环执行(命令由 playSdFile/playSdNext/exitSdMode 投递过来);
//   ② 碰 audio 对象必须持 audioMutex(与音频泵任务的 audio.loop() 互斥);
//   ③ gState 只在主循环写 —— 这里正是主循环, 所以可以随便写。
//
// ---- 为什么用 connecttoFS 而不是 connecttoSD ----
//   库的 connecttoSD() 内部写死了全局 SPI `SD` 对象, 而本工程挂的是 SD_MMC(1-bit),
//   两者不是同一个 FS -> 必须用通用接口 connecttoFS(SD_MMC, path)。

void AudioPlayer::execSdPlay(int idx) {
  // 首次进入(或卡被拔插过)时确保已挂载
  if (!gSd.mounted() && !gSd.begin()) { gState.status = "SD 未挂载"; return; }
  const int n = gSd.count();
  if (n <= 0) { gState.status = "SD 无音频"; return; }
  if (idx < 0) idx = (mSdLastIdx >= 0) ? mSdLastIdx : 0;
  if (idx >= n) idx = 0;
  mSdLastIdx = idx;
  // ★记忆"播放到哪一首"(2026-10-07): 写 NVS, 内部对同一首去重不重复擦写。
  //   作用: 下次用红外/后台切回 SD 音源时, 从这首继续(而不是回到第 1 首)。
  gSd.setLast(idx);
  gSd.saveLast();

  const SdAudioFile* f = gSd.file(idx);
  if (!f) { gState.status = "SD 文件不存在"; return; }

  // 进 SD 模式前先记住"该回哪台电台", 退出时原样恢复(gState.index 未被 SD 改动, 这里只记"有效")
  if (!gState.sdActive) mSdReturnArmed = (store && store->count() > 0);

  beginSwitchMute();     // 切源静音: 与切电台同一手法, 避免 SD/网络 之间爆音
  stopSongSafe();        // 停掉上一首(网络流或上一个本地文件)
  gState.title = "";                 // 清掉上一首的 ID3/ICY 曲名
  gState.audioEverFlowed = false;
  s_outStarted = false;

  gState.sdActive = true;            // ★音源标志: 让看门狗别把"放完"当断流(见 loop())
  gState.sdIndex  = idx;
  gState.sdName   = f->name;
  gState.stationName = icyToUtf8(f->name.c_str());   // ★转 UTF-8: 卡内文件名可能是 GBK(老中文名), 否则屏上画方框
  gState.status   = "SD 播放";
  gState.url      = f->path;         // 诊断页显示真实路径
  gState.playing  = true;
  gState.lastDataMs = millis();
  gState.failCount = 0;
  prevInFill = 0;

  markStage(12);                     // 与网络连流同一个"进入库"面包屑阶段
  lockAudio();
  audio.connecttoFS(SD_MMC, f->path.c_str());   // ★SD_MMC 是 fs::SDMMCFS, 派生自 fs::FS
  unlockAudio();
  markStage(6);

  Serial.printf("[Audio] SD 播放 [%d/%d] %s -> 运行=%d 编码=%s 码率=%ubps\n",
                idx + 1, n, f->name.c_str(),
                audio.isRunning() ? 1 : 0, audio.getCodecname(),
                (unsigned)audio.getBitRate());
}

void AudioPlayer::execSdNext() {
  if (!gSd.mounted()) { gState.status = "SD 未挂载"; return; }
  const int n = gSd.count();
  if (n <= 0) { gState.status = "SD 无音频"; return; }
  int next = (mSdLastIdx < 0) ? 0 : (mSdLastIdx + 1) % n;
  // ★随机(3): 用简单伪随机挑一首, 避免整点走C 库 rand 的长耗时(音频泵附近别做重活)。
  //   用线性同余(种子固定)而不是 millis() —— 结果可复现, 便于"同一首重播"复现问题。
  if (gState.sdLoop == SD_LOOP_RANDOM && n > 1) {
    mSdRand = mSdRand * 1103515245UL + 12345UL;      // 32 位 LCG, 溢出是预期行为
    next = (int)((mSdRand >> 16) % (uint32_t)n);
    if (next == mSdLastIdx) next = (next + 1) % n;     // 别连着两首都一样(至少推1)
  }
  execSdPlay(next);
}

// 循环方式切换(2026-10-07): dir=-1 反向一档 / +1 正向一档。
// 由 SD 列表内的左/右双击调用(在 .ino 里), 只改状态不发命令, 因此可在任意上下文安全调用。
void AudioPlayer::cycleSdLoop(int dir) {
  const int N = 4;
  int v = gState.sdLoop % N;
  v = ((v + (dir >= 0 ? 1 : N - 1)) % N);             // C++ 取模可能为负, 故手工补 N
  gState.sdLoop = v;
  Serial.printf("[SD  ] 循环方式 -> %s\n",
                v == 0 ? "顺序(放完停止)" : v == 1 ? "循环全部"
                : v == 2 ? "单曲循环"    : "随机播放");
}

void AudioPlayer::execSdExit() {
  if (!gState.sdActive) return;
  stopSongSafe();
  gState.sdActive = false;
  gState.sdIndex  = -1;
  gState.sdName   = "";
  gState.title    = "";
  gState.playing  = false;
  gState.status   = "停止";
  gState.url      = "";
  // 回到进 SD 前的那台电台(电台表的 lastIndex 全程没被动过, 直接按它续播)
  if (mSdReturnArmed && store && store->count() > 0) {
    mSdReturnArmed = false;
    execPlayIdx(-1);                 // -1 = 用 store 的 lastIndex
  }
}

// 把音频泵任务暂存的曲名落地到 gState —— ★只能由主循环调用(见文件顶部交接说明)。
// 代次号协议: 奇数 = 泵任务正在写; 偶数 = 写完。只在"偶数 且 未取过"时取走,
// 从而不会读到半截字符串(最坏情况只是这一圈跳过, 下一圈再取)。
void AudioPlayer::applyPendingTitle() {
  const uint32_t g = s_titleGen;
  if ((g & 1u) != 0 || g == s_titleApplied || g == 0) return;  // 写入中 / 已取过 / 还没内容
  // 先把字节拷到本地再复核代次号: 若拷的过程中泵任务又开了新一轮写(代次号已变),
  // 本地这份可能是"旧+新"拼接的半截串, 直接丢弃 —— 下一圈重取干净的那份。
  char tmp[192];
  memcpy(tmp, s_titlePending, sizeof(tmp));
  if (s_titleGen != g) return;                 // 拷期间被改写 -> 丢弃本轮
  s_titleApplied = g;
  gState.title      = icyToUtf8(tmp);          // UTF-8 原样 / GB2312 查表转换 / Latin-1 升位
  gState.titleEnc   = gLastTitleEnc;           // 记下判定结果(诊断用)
  gState.lastMetaMs = millis();
}

void AudioPlayer::loop() {
  // 喂流由谁做 —— 由 pumpFeeding() 决定(见 config.h「防呆设计」与 setPumpEnabled):
  //   · 泵任务正常(开关开 且 任务已创建) -> 本函数【不碰 audio.loop()】, 只做管理;
  //   · 泵缺失 或 开关关              -> 本函数【直接喂流】(方案A 回退), 保证"泵坏了也不会没声"。
  // ★两条路径互斥: 只有 pumpFeeding() 为假时下面这段才执行 ⇒ 任何时刻只有一条在调 audio.loop()。
  if (!pumpFeeding()) {
    const uint32_t t0 = micros();
    lockAudio();
    audio.loop();
    unlockAudio();
    const uint32_t d = micros() - t0;
    if (d > g_phMaxUs[PH_AUDIOLOOP]) g_phMaxUs[PH_AUDIOLOOP] = d;
    s_lastLoopUs = micros();          // 让"喂流最大间隔"指标在回退模式下同样有效
  }

  // ① 先落地泵任务送来的曲名(库回调在泵任务里执行, 不能直接写 gState)
  applyPendingTitle();

  // ② 执行排队的换台/停止/重连命令(见类注释线程模型; 按键/WS/看门狗都只是投递命令)
  processCmd();

  // ③ 执行待办的"跳过不支持编码的台"(见 skipNext 注释: 用迭代替代递归, 保护 8KB 主循环栈
  //    并保证每圈都喂失速看门狗)。放在 processCmd 之后: 用户新下的命令优先, 执行完再看要不要跳。
  if (skipNext != 0) {
    const int d = skipNext;
    skipNext = 0;
    execStep(d);
  }

  updateMeta();                 // 非阻塞推进 ICY 元数据读取

  // 存活判定: 由音频输入缓冲是否真在增长判定(不依赖库回调, 其只在建连时触发一次)。
  // 同时捕获库"运行标志" 1->0 的瞬间, 配合 [LIB] 行点名"建连即死/出声后死"。
  static bool s_lastRun = false;
  bool runNow = audio.isRunning();
  if (s_lastRun && !runNow && gState.playing) {
    // ★★SD 本地播放(阶段②)的"文件放完"走这里 —— 必须和网络流分开处理:
    //   一个本地文件播完时, 库会正常地停掉解码器(isRunning() 归 0), 这【不是】故障。
    //   若按下面的电台逻辑处理, 会先 execReconnect() 把同一首重播一遍, 或干脆跳台 ——
    //   表现为"一首播完就莫名重播/乱跳"。所以这里识别为"放完了" -> 顺序下一首(自动续播)。
    if (gState.sdActive) {
      const int nSd = gSd.count();
      const int cur = gState.sdIndex;
      gState.playing = false;         // 先落"停止", 免得下面切下一首期间又被判成断流
      gState.lastDataMs = millis();
      Serial.printf("[SD  ] 第 %d 首放完 (循环=%d)\n", cur + 1, gState.sdLoop);
      // ★2026-10-07 按循环方式决定"放完怎么办"(原来只有"顺序下一首", 且单首时一律停住)。
      //   判定用【当前序号】而不是"下一首序号": cur 是放完的那首, 语义最明确。
      if (nSd <= 0) {
        gState.status = "SD 无音频";
      } else if (gState.sdLoop == SD_LOOP_ONE) {              // 单曲循环: 重播本首
        execSdPlay(cur >= 0 ? cur : 0);
      } else if (gState.sdLoop == SD_LOOP_ORDER) {            // 顺序: 到最后一首就停
        if (cur >= nSd - 1) { gState.status = "SD 播放完"; s_lastRun = false; }
        else                execSdPlay(cur + 1);
      } else if (nSd == 1) {
        execSdPlay(0);                                       // 循环全部/随机 + 单曲 = 一直重播
      } else {
        execSdNext();                // 循环全部 -> 顺序接下一首; 随机 -> 由 execSdNext 随机挑
      }
      s_lastRun = runNow;
      updateMeta();
      prevInFill = inBufferFilled();
      return;                         // 本圈已处理完, 不再往下走电台的看门狗逻辑
    }
    // ★自愈(2026-09-18 加): 库自己把流停了(解码失败/服务器断开/内部错误), 而 gState.playing 仍为 true。
    //   只靠"输入缓冲里还有数据"判活着的话, 缓冲里的残留数据会让看门狗永远不触发 ->
    //   表现为"屏显播放中却永久无声", 只能人工断电。这里直接按一次流故障处理, 自愈:
    //   先同台重连, 连续失败 SKIP_AFTER_FAILS 次则跳到下一台。
    Serial.printf("[WD] ★库运行标志 1->0: 流被库内部停止 -> 自动重连/跳台 | 编码=%s 码率=%ubps 缓冲=%u | 阶段 %u %s\n",
                  audio.getCodecname(), (unsigned)audio.getBitRate(), (unsigned)inBufferFilled(),
                  (unsigned)lastStage(), stageText(lastStage()));
    gState.failCount++;
    gState.lastDataMs = millis();          // 给本次重连/切台一个观察窗口, 防忙等
    if (gState.failCount >= SKIP_AFTER_FAILS) {
      gState.failCount = 0;
      execStep(+1);                        // 连坏多次 -> 换下一台
    } else {
      execReconnect();                     // 同台重连
    }
  }
  s_lastRun = runNow;

  uint32_t filled = inBufferFilled();
  bool grew = filled > prevInFill;            // 本圈有新数据写入(网络在推)
  bool substantial = filled > 256;            // 缓冲里还有可播数据(未彻底干涸)
  if (gState.playing && (grew || substantial)) {
    gState.lastDataMs       = millis();
    gState.audioEverFlowed  = true;           // 连接确实有数据在流
  }
  prevInFill = filled;

  // ---- "缓冲中" 的判据(2026-09-19 晚改回锁存式) ----
  // ★绝不能用"缓冲 < 某水位"来判断: 稳态播放时输入缓冲本来就可能停在低位(进多少出多少),
  //   那样屏上会【一直】显示"缓冲中" —— 明明声音正常却看着像卡死。
  // 真正该问的是"库有没有开始解码输出": 已建连 + 已识别码率(>0) => 解码链跑起来了。
  // ★但这面旗子【必须锁存】(见 s_outStarted 处的长篇说明): 让它单调, 屏上文字才不会每帧抖,
  //   否则整帧跳过失效 -> 主循环每次刷屏都卡 60ms -> 排出 DMA -> 一直卡卡卡。
  // ★解码/出声判据: 必须"库在跑 + 码率已识别 + 已过起播预缓冲"。最后一项让屏上"缓冲中"
  //   只在【预缓冲期间】显示(预缓冲是故意不出声), 不会在稳态播放时误显(见 s_outStarted 锁存)。
  const bool decoding = audio.isRunning() && audio.getBitRate() > 0 && audio.prebufferDone();
  if (decoding) s_outStarted = true;                 // 一旦出声过, 本台内不再回退为"缓冲中"
  gState.buffering = gState.playing && !s_outStarted;

  // ---- "迟迟不出声"诊断(每 2s 一行) ----
  // 现象"没有声音"时, 这一行直接回答四个关键问题, 不用再猜:
  //   库建连了吗(运行) / 编码认出来了吗 / 缓冲在长还是原地不动 / 到底谁在喂流。
  {
    static uint32_t s_lastQuietMs = 0;
    static bool     s_wasDecoding = false;
    const uint32_t  nowMs = millis();
    if (gState.playing && decoding && !s_wasDecoding) {
      Serial.printf("[Audio] 开始出声: 编码=%s 码率=%ubps 缓冲=%uKB | 采样率=%uHz\n",
                    audio.getCodecname(), (unsigned)audio.getBitRate(),
                    (unsigned)(filled / 1024), (unsigned)audio.getSampleRate());
    }
    s_wasDecoding = decoding;
    if (gState.playing && !decoding && (nowMs - s_lastQuietMs >= 2000)) {
      s_lastQuietMs = nowMs;
      const uint32_t cap = bufferCapacity();
      Serial.printf("[Audio] ★未出声诊断: 库运行=%d 编码=%s 缓冲=%uB/%uB(%u%%) 喂流=%s(泵%luHz) | 堆 %uKB 最大块 %uKB | 阶段 %u %s\n",
                    audio.isRunning() ? 1 : 0, audio.getCodecname(),
                    (unsigned)filled, (unsigned)cap,
                    (unsigned)(cap ? filled * 100 / cap : 0),
                    pumpFeeding() ? "泵任务" : "主循环",
                    (unsigned long)pumpHz,
                    (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getMaxAllocHeap() / 1024),
                    (unsigned)lastStage(), stageText(lastStage()));
    }
  }

  // 主循环频率统计(诊断用; 过低说明主循环被阻塞)
  loopCnt++;
  uint32_t now = millis();
  if (now - loopHzMs >= 1000) {
    loopHz = loopCnt * 1000 / (now - loopHzMs);
    loopCnt = 0; loopHzMs = now;
  }

  updateAudioHealth();          // 欠载监测 + 元数据连接自适应降级
  rampSwitchVolume();           // 切台静音后的音量平滑恢复(灭起播爆破音)

  // ---- 睡眠定时器 / 定时关机 倒计时(共用同一组计数字段) ----
  // 主循环每圈都跑, 但只在"剩余>0"且"每过 1s"时减一; 归零即触发。
  //   sleepToPowerOff=false(睡眠定时器) -> 停播 + 置 sleepJustFired 让主循环关屏,
  //                                    设备保持通电, 任意按键可重新点亮(熄屏待机)。
  //   sleepToPowerOff=true (定时关机)   -> 额外置 sleepShutdownFired 让主循环【真正关机】。
  if (gState.sleepRemainSec > 0) {
    uint32_t now = millis();
    if ((int32_t)(now - mSleepMs) >= 1000) {
      mSleepMs = now;
      gState.sleepRemainSec--;
      if (gState.sleepRemainSec <= 0) {
        gState.sleepRemainSec = 0;
        gState.sleepMin = 0;
        const bool pwrOff = gState.sleepToPowerOff;
        // ★停播要【先于】置标志: execStop() 内部会清 sleepMin/sleepRemain(防重复触发),
        //   所以先把需要的信息取出来再调它。
        if (gState.playing) execStop();
        gState.sleepJustFired = true;              // 主循环据此关屏(显示模块不在音频侧)
        if (pwrOff) {
          gState.sleepShutdownFired = true;        // 主循环据此真正关机(不返回)
          Serial.println("[SLEEP] ★定时关机到点 -> 即将停播并进入深度休眠");
        } else {
          Serial.println("[SLEEP] 睡眠定时器到点 -> 已停播, 屏幕由主循环关闭");
        }
      }
    }
  }
}

// ============ 流畅度健康监测 + 自适应降级 ============
// 音频输入缓冲的填充率是判断卡顿成因的金标准:
//   持续接近 0  -> 网络喂不饱(带宽不足 / WiFi信号弱 / 服务器慢 / 主循环被阻塞)
//   持续接近满  -> 喂得进但解不动(CPU 算力不足, 检查 CPU_FREQ_MHZ)
// 而"独立元数据连接"是最大的资源竞争者(下行带宽翻倍 + 占全局共享的 LWIP pbuf 池),
// 所以持续欠载时优先牺牲它 —— 先保声音, 节目信息是次要功能。
void AudioPlayer::updateAudioHealth() {
  uint32_t now = millis();
  if (now - healthMs < 1000) return;      // 每秒采样一次
  healthMs = now;

  // 码率: 由主循环读取库内部值显示(读 int 在 ESP32 上原子, 不跨线程写 String)
  if (gState.playing) {
    uint32_t br = audio.getBitRate();   // bps
    gState.bitrate = (br >= 1000) ? (String(br / 1000) + "k") : String(br);
    // 编码名: 库识别出编码后返回 "AAC" / "MP3" / "FLAC" / …; 未识别时是 "unknown",
    // 这时留空不上屏(免得显示个 unknown 反而更让人困惑)。
    const char* cn = audio.getCodecname();
    gState.codec = (cn && *cn && strcmp(cn, "unknown") != 0) ? String(cn) : String();
  }

  // 只在"播放中且曾出过声"时统计: 建连阶段缓冲本就是空的, 不该算作欠载
  if (!gState.playing || !gState.audioEverFlowed) { lowWaterCnt = 0; return; }

  // ★用真实缓冲容量算填充率(缓冲已改到 PSRAM, 再除以 AUDIO_RAM_BUFSIZE 会永远 >100%,
  // 欠载检测就永久失效了)
  const uint32_t cap = bufferCapacity();
  uint32_t pct = cap ? (inBufferFilled() * 100 / cap) : 0;
  if (pct < META_LOW_WATER_PCT) {
    lowWaterCnt++;
    if (lowWaterCnt >= META_LOW_WATER_TIMES && metaEnabled && !metaAutoOff) {
      metaAutoOff = true;
      setMetaEnabled(false);      // 自动降级: 关掉节目信息连接, 把带宽/pbuf 还给音频
      Serial.println("[HEALTH] 音频缓冲持续欠载 -> 已自动关闭节目信息连接以优先保声音");
      Serial.println("[HEALTH] (此举省下一半下行带宽与 pbuf 占用; 串口发 M 可手动切回)");
    }
  } else {
    lowWaterCnt = 0;
  }
}

uint32_t AudioPlayer::inBufferFilled() { return audio.inBufferFilled(); }

// 输入缓冲的真实总容量。库的 AudioBuffer::init() 会把两个尺寸各扣掉一个保留区:
//   PSRAM 分支: m_buffSize = m_buffSizePSRAM - m_resBuffSizePSRAM  (4096*4 = 16384)
//   DRAM  分支: m_buffSize = m_buffSizeRAM   - m_resBuffSizeRAM    (1600)
// 实测核对: 传 (16384, 524288) 时库打印 "inputBufferSize: 507903" = 524288-16384-1 ✓
uint32_t AudioPlayer::bufferCapacity() {
  return psramFound() ? (uint32_t)(AUDIO_PSRAM_BUFSIZE - 16384)
                      : (uint32_t)(AUDIO_RAM_BUFSIZE   - 1600);
}

// 节目信息连接开关。关闭后不再建立第二条流, 带宽/pbuf/CPU 全部让给音频。
// ---- SBR 开关(2026-10-07) ----
// 直通第三方库的运行时开关(该库已打补丁, 见 aac_decoder.cpp 的 AACSetSbrEnabled 注释)。
// ★为什么能运行时切: 开关加在"识别到 SBR 数据的那一刻", 关掉时 sbrEnabled 恒为 0,
//   于是 AACGetSampRate() 自然返回核心采样率、AACGetOutputSamps() 也随之减半,
//   下游(I2S 采样率 / 补静音长度)全部自动跟随 —— 不需要重建解码器。
// ★必须在换台前调用: 已建流的台解码器状态已定, 要等下一次 connect 才会用新设置。
void AudioPlayer::setSbrEnabled(bool on) {
  mSbrEnabled = on;
  AACSetSbrEnabled(on ? 1 : 0);
  Serial.printf("[Audio] SBR(高频重建) = %s —— %s\n",
                on ? "开(HE-AAC 输出 96kHz, 但 96kHz 下 I2S DMA 仅 170ms 余量, 突发流易发颤)"
                   : "关(输出为核心采样率 48kHz, DMA 余量 372ms, 推荐)",
                "换台后生效");
}

void AudioPlayer::setMetaEnabled(bool on) {
  metaEnabled = on;
  if (on) {
    metaAutoOff = false;
    metaRetryMs = 0;                    // 清冷却, 允许立刻重连
    if (gState.playing && gState.url.length()) startMeta(gState.url);
  } else {
    stopMeta();
    gState.title = "";                  // 关掉后不再有节目信息, 清掉避免残留旧曲名
  }
  Serial.println(String("[META] 节目信息连接: ") + (on ? "开启" : "关闭"));
}

void AudioPlayer::printDiag() {
  uint32_t filled = inBufferFilled();
  const uint32_t cap = bufferCapacity();          // 真实容量(PSRAM 分支 ≠ AUDIO_RAM_BUFSIZE)
  uint32_t pct    = cap ? (filled * 100 / cap) : 0;
  long rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  uint32_t age  = gState.playing ? (millis() - gState.lastDataMs) : 0;  // 距上次真有数据流入的时长
  // 栈水位: 低于 ~1KB 就很危险(栈溢出会直接重启, 且现象与"死机"一样)
  uint32_t stkMain = uxTaskGetStackHighWaterMark(nullptr);
  // 音频泵任务的栈余(方案B 新增): 泵任务里的 audio.loop() 会做深层嵌套的流解析/解码,
  // 而整个固件的音质就押在这个任务上 —— 见 AudioPlayer::pumpStackFree()。
  uint32_t stkPump = pumpStackFree();
  // audio.isRunning()  = 库 connecttohost 是否成功建连(m_f_running);
  // audio.getCodecname() = 已识别的编码(MP3/AAC/.../NONE); NONE=连接建了但编码未识别(HTTP头解析卡住)。
  // 码率 = 库从流头部解析出的名义码率(bps): >0 说明"编码已识别且解码链已启动",
  // 是区分"连上了但没解"与"解码器正常工作"的最强信号(比 编码= 更早出现)。
  uint32_t br = audio.getBitRate();
  // 显示运行时实际生效值(串口 B 可改写): 闸门开时打真实水位, 关时说明库按上游行为。
  static char prebufTxt[40];
  {
    uint32_t pb = audio.getPrebufBytes();
    if (pb > 0)
      snprintf(prebufTxt, sizeof(prebufTxt), "开,%uKB", (unsigned)(pb / 1024));
    else
      snprintf(prebufTxt, sizeof(prebufTxt), "★关(库攒到一帧就出声)");
  }
  Serial.printf("[DIAG] 缓冲 %u/%u(%u%%) 起播预缓冲=%s | 喂流=%s | 音量 %d/%d | 运行=%d 编码=%s 码率=%ubps 解码错=%d | 数据age %ums | 主循环 %luHz 音频泵 %luHz | 堆 %uKB(最大块 %uKB) | 栈余 主%u/泵%u | WiFi %lddBm | 阶段 %u %s | %s | 标题编码=%s\n",
                (unsigned)filled, (unsigned)cap, (unsigned)pct,
                prebufTxt,
                pumpFeeding() ? "泵任务" : "主循环",
                gState.volume, VOLUME_MAX,
                audio.isRunning() ? 1 : 0, audio.getCodecname(), (unsigned)br, (int)audio.getDecodeError(),
                (unsigned)age,
                (unsigned long)loopHz, (unsigned long)pumpHz,
                (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getMaxAllocHeap() / 1024),
                (unsigned)stkMain, (unsigned)stkPump,
                rssi,
                (unsigned)lastStage(), stageText(lastStage()),
                gState.buffering ? "等待出声" : (gState.playing ? "播放中" : "停止"),
                gState.titleEnc.length() ? gState.titleEnc.c_str() : gLastTitleEnc);
  // 关键判读(重要): 「运行=0 且 编码=unknown 且 缓冲=0」是"库连流失败"的特征形态, 只有三种成因 ——
  //   (a) HTTP 响应头 4.5s 读不全 -> log_e("timeout") -> parseHttpResponseHeader 的 exit: stopSong()
  //   (b) MP3 解码器内存分配失败 -> "The MP3Decoder could not be initialized" -> initializeDecoder 的 exit: stopSong()
  //   (c) TCP 根本连不上 -> connecttohost 里 m_f_running=false
  // 三者都会把 m_codec 复位成 NONE、m_f_running 置 0; 而 m_bitRate 不被 stopSong 重置 ->
  // 「码率=192000」只证明【历史某次读到过 icy-br:192】, 不能证明"现在还连着"。
  // 「最大块」是判定 (b) 的直接证据: 它 < 20KB 就说明堆碎片化, 解码器那 9 个连续分配必然失败。
  // 真正点名成因的是本次新增的 [LIB] / [LIB:E] 行(库自己的 nararation)。
  // 当前播放的电台名 + 真实流地址(排查"音量/地址/是否还在播默认台")
  Serial.println("[DIAG] 电台: [" + gState.stationName + "] " + gState.url);
  // ---- 断音判读(2026-09-18 新增; 2026-09-19 随方案B 改挂音频泵任务) ----
  // 采样率: 库里报的是【实际输出】采样率 = 核心采样率 x (SBR 启用 ? 2 : 1)。
  //   96000 表示这是一个 HE-AAC(audio/aacp) 流且 SBR 正在工作 —— 输出数据率高达 384KB/s,
  //   对 I2S 喂数节奏的要求远高于 44.1kHz 的 MP3 台, 是"断断续续"的高发场景。
  // 音频泵喂流最大间隔 = 两次 audio.loop() 之间最长的间隔。超过 DMA 深度
  //   (96000Hz 下 170ms / 44100Hz 下 372ms) 就必然听到断音。
  //   · 方案B 后这个值只反映【泵任务被什么挡住了】: 若出现 100ms+ 尖峰, 看是否与
  //     "喂流"段(锁等待 connecttohost 换台)或 Flash 落盘(禁用 cache, 两个核一起冻结)重合;
  //   · 常年 < 20ms 却仍断续 -> 不是喂流节奏问题, 应怀疑 CPU 解不动 / 网络突发。
  Serial.printf("[DIAG] 采样率 %uHz | 音频泵喂流最大间隔 %lums (DMA深度 96000Hz->170ms / 44100Hz->372ms)\n",
                (unsigned)audio.getSampleRate(), (unsigned long)(s_loopMaxGapUs / 1000));
  s_loopMaxGapUs = 0;                 // 每个统计周期清零

  // ---- 分阶段最大耗时: 把"卡了多久"钉到"卡在哪一段"(索引见 player_state.h 的 PH_*) ----
  // 判读(方案B 后各段的含义):
  //   喂流  偏大 -> 泵任务里的 audio.loop() 本身慢(96kHz HE-AAC 属高负载)或被 audioMutex 挡住(正在换台);
  //   库循环偏大 -> 主循环侧的取命令/元数据/健康监测慢(正常应只有个位数 ms);
  //   落盘  偏大 -> ★Flash 落盘会禁用 flash cache, 两个核一起被冻结(泵任务也躲不掉), 应降低落盘频率;
  //   显示  偏大 -> ST7567 软件 SPI 整屏刷新慢 —— 方案B 后它已不再推迟喂流, 只影响 UI 流畅度。
  Serial.printf("[DIAG] 阶段最大耗时ms: 喂流%lu 库循环%lu Web%lu 落盘%lu 门户%lu 红外%lu 按键%lu 显示%lu\n",
                (unsigned long)(g_phMaxUs[PH_AUDIOLOOP] / 1000),
                (unsigned long)(g_phMaxUs[PH_PLAYER]  / 1000),
                (unsigned long)(g_phMaxUs[PH_WEB]     / 1000),
                (unsigned long)(g_phMaxUs[PH_STORE]   / 1000),
                (unsigned long)(g_phMaxUs[PH_PORTAL]  / 1000),
                (unsigned long)(g_phMaxUs[PH_IR]      / 1000),
                (unsigned long)(g_phMaxUs[PH_BTN]     / 1000),
                (unsigned long)(g_phMaxUs[PH_DISPLAY] / 1000));
  for (uint8_t i = 0; i < PH_COUNT; i++) g_phMaxUs[i] = 0;
}

// ============ 原始抓流测试(绕过音频库, 串口发 W) ============
// 为什么必须有它: 排查"无声"时最核心的分叉是 ——
//   ① 服务器/网络根本没有把音频数据推过来  -> 与音频库无关, 改代码永远修不好
//   ② 数据推过来了, 但库没解/没写 I2S       -> 库侧问题
// 此前判断 ① 只能靠开发机 curl, 但实测开发机的网络会被代理截断(同一 URL 三次分别收到
// 5600/2800/5600 字节, 几 MB 的普通文件也只收到 4096 字节), 结论完全不可用。
// 唯一可信的判据是【这块板自己】的 TCP。本函数就是干这个: 不和库共用任何状态,
// 自己连、自己发请求、自己数 3 秒内到手的音频字节数。
void AudioPlayer::rawStreamTest(const String& url) {
  Serial.println("[RAW] ===== 原始抓流测试(绕过音频库, 用本机自己的 TCP) =====");
  String u = url; u.trim();
  if (u.startsWith("https://")) u = "http://" + u.substring(8);   // 与播放路径保持一致
  int p = u.indexOf("://");
  if (p < 0) { Serial.println("[RAW] 地址无效: " + u); return; }
  String rest = u.substring(p + 3);
  int s = rest.indexOf('/');
  String host = (s < 0) ? rest : rest.substring(0, s);
  String path = (s < 0) ? String("/") : rest.substring(s);
  int port = 80;
  int c = host.indexOf(':');
  if (c >= 0) { port = host.substring(c + 1).toInt(); host = host.substring(0, c); }
  if (host.length() == 0) { Serial.println("[RAW] 地址解析失败"); return; }

  WiFiClient cli;
  cli.setTimeout(3);
  uint32_t t0 = millis();
  if (!cli.connect(host.c_str(), (uint16_t)port)) {
    Serial.printf("[RAW] ★TCP 连接失败 %s:%d (耗时%ums) -> 网络/服务器不可达\n",
                  host.c_str(), port, (unsigned)(millis() - t0));
    cli.stop();
    return;
  }
  Serial.printf("[RAW] TCP 已连 %s:%d (耗时%ums)\n", host.c_str(), port, (unsigned)(millis() - t0));

  // 与库完全相同的请求头: 只有用同样的头才可能复现库看到的响应
  // (Audio.cpp connecttohost 里的 rqh: Icy-MetaData ×2 / Accept:*/* / VLC UA / identity)
  cli.print(String("GET ") + path + " HTTP/1.1\r\nHost: " + host +
            "\r\nIcy-MetaData:1\r\nIcy-MetaData:2\r\nAccept:*/*\r\n"
            "User-Agent: VLC/3.0.21 LibVLC/3.0.21\r\n"
            "Accept-Encoding: identity;q=1,*;q=0\r\nConnection: keep-alive\r\n\r\n");

  // ---- 阶段1: 读响应头(最多 4.5s, 与库的 timeout 一致, 便于对比) ----
  String hdr = "";
  uint32_t th = millis();
  while (millis() - th < 4500 && hdr.length() < 3000) {
    bool got = false;
    while (cli.available()) { hdr += (char)cli.read(); got = true; }
    if (hdr.endsWith("\r\n\r\n")) break;
    if (!got) delay(1);                 // 空转必须让出 CPU, 否则饿死 IDLE 触发 task_wdt
  }
  uint32_t hdrMs = millis() - th;
  int he = hdr.indexOf("\r\n\r\n");
  String line1 = hdr.substring(0, hdr.indexOf('\n')); line1.replace("\r", "");
  Serial.printf("[RAW] 响应头 %u 字节 耗时%ums %s\n", (unsigned)hdr.length(), (unsigned)hdrMs,
                (he >= 0) ? "(完整, 收到空行)" : "(★不完整: 4.5s 内没收到结尾空行 -> 这就是库里 log_e(\"timeout\") 的成因)");
  Serial.println("[RAW] 状态行: " + line1);
  // 只挑关键行打印(与库解析逻辑一一对应)
  {
    String h = hdr; h.toLowerCase();
    int i1 = h.indexOf("content-type:");
    if (i1 >= 0) Serial.println("[RAW] " + hdr.substring(i1, hdr.indexOf('\n', i1)));
    int i2 = h.indexOf("icy-br:");
    if (i2 >= 0) Serial.println("[RAW] " + hdr.substring(i2, hdr.indexOf('\n', i2)));
    int i3 = h.indexOf("icy-metaint:");
    Serial.println(String("[RAW] ") + (i3 >= 0 ? "有 icy-metaint(带元数据)" : "无 icy-metaint(纯音频流)"));
    int i4 = h.indexOf("transfer-encoding:");
    if (i4 >= 0) Serial.println("[RAW] " + hdr.substring(i4, hdr.indexOf('\n', i4)));
    int i5 = h.indexOf("location:");
    if (i5 >= 0) Serial.println("[RAW] ★有重定向: " + hdr.substring(i5, hdr.indexOf('\n', i5)));
  }

  // 头之后残留在缓冲区的字节就是音频数据的开头, 一并计入
  uint32_t audio = 0;
  uint8_t first[8] = {0}; int fi = 0;
  if (he >= 0) {
    for (int k = he + 4; k < (int)hdr.length() && fi < 8; k++) first[fi++] = (uint8_t)hdr[k];
    audio = hdr.length() - (he + 4);
  }

  // ---- 阶段2: 读 3 秒音频, 统计真实吞吐 ----
  uint32_t t1 = millis();
  while (millis() - t1 < 3000) {
    int n = cli.available();
    if (n > 0) {
      uint8_t buf[512];
      int want = (n > (int)sizeof(buf)) ? (int)sizeof(buf) : n;
      int r = cli.read(buf, want);
      if (r > 0) {
        for (int i = 0; i < r && fi < 8; i++) first[fi++] = buf[i];
        audio += r;
      }
    } else {
      if (!cli.connected()) { Serial.printf("[RAW] 连接在 %ums 处被服务器关闭\n", (unsigned)(millis() - t1)); break; }
      delay(1);
    }
  }
  uint32_t ms = millis() - t1;
  cli.stop();

  uint32_t kbps = (ms > 0) ? (uint32_t)((uint64_t)audio * 8ULL / ms) : 0;   // 字节*8/毫秒 = kbps
  Serial.printf("[RAW] 3秒内收到音频 %u 字节 (耗时%ums) ≈ %u kbps\n", (unsigned)audio, (unsigned)ms, (unsigned)kbps);
  Serial.print("[RAW] 音频前几个字节: ");
  for (int i = 0; i < fi; i++) Serial.printf("%02X ", first[i]);
  if (fi >= 2 && first[0] == 0xFF && (first[1] & 0xE0) == 0xE0) Serial.println(" <- MP3 帧同步(0xFFEx) 正常");
  else if (fi == 0) Serial.println(" <- ★一个音频字节都没收到");
  else Serial.println(" <- ★首字节不是 MP3 帧同步");
  // 判读表(按 128kbps≈16KB/s, 192kbps≈24KB/s 对照)
  Serial.println("[RAW] 结论判据:");
  Serial.println("[RAW]   ≈0 kbps 或连接立刻被关 -> 服务器/网络没推音频, 与音频库无关(换源/查服务器)");
  Serial.println("[RAW]   ≈100~200 kbps 且有 MP3 帧同步 -> 网络侧完全正常, 无声必在库侧(看 [LIB] 行点名)");
  Serial.println("[RAW] ===== 抓流测试结束 =====");
}

// ============ 独立 ICY 元数据读取器 ============
// 另开一个轻量连接, 只解析 icy-metaint 之后的元数据块、丢弃音频字节,
// 把 StreamTitle 写进 gState.title(本函数在主循环上下文运行, 与本文件的库回调交接互不冲突)。
// ★2026-09-19 更正: 旧注释写"实测本机 ESP32-audioI2S 2.0.0 不会回调 audio_showstreamtitle,
//   导致节目信息永远为空" —— 那是对 2.0.0 的结论; 本工程用的 2.0.6 一定会回调(见文件顶部),
//   零带宽即可拿到曲名。故本"独立读取器"是【可选的老方案】, 默认关闭(META_ENABLED_DEFAULT=0),
//   留着是为了对比排障: 当库回调路径取不到曲名时, 可用它交叉验证是流没有 ICY 还是解析问题。
void AudioPlayer::startMeta(const String& streamUrl) {
  if (!metaEnabled) return;     // 开关关闭时不得建立第二条流(省带宽/pbuf 给音频)
  // HLS(.m3u8) 流本身不携带 ICY 元数据, 且库在每个分片边界都要重新建连抓取下一分片,
  // 没必要再开第二条独立连接去抢宝贵的 pbuf 池 / 带宽 / CPU —— 关掉它, 把资源全部让给音频,
  // 能缩小分片切换时的卡顿窗口。节目信息(StreamTitle)对 HLS 源本就取不到, 关掉无功能损失。
  if (streamUrl.indexOf(".m3u8") >= 0) return;
  stopMeta();
  String u = streamUrl; u.trim();
  int p = u.indexOf("://");
  if (p >= 0) u = u.substring(p + 3);
  int s = u.indexOf('/');
  metaHost = (s < 0) ? u : u.substring(0, s);
  metaPath = (s < 0) ? String("/") : u.substring(s);
  metaPort = 80;
  int c = metaHost.indexOf(':');
  if (c >= 0) { metaPort = metaHost.substring(c + 1).toInt(); metaHost = metaHost.substring(0, c); }
  if (!metaClient.connect(metaHost.c_str(), metaPort)) { metaState = 0; return; }
  metaClient.print(String("GET ") + metaPath + " HTTP/1.1\r\nHost: " + metaHost +
                   "\r\nIcy-MetaData: 1\r\nUser-Agent: WebRadio\r\nConnection: keep-alive\r\n\r\n");
  metaState = 1; metaMetaint = 0; metaCount = 0; metaPos = 0; metaBuf[0] = 0; metaHdr = "";
}

void AudioPlayer::stopMeta() {
  if (metaClient.connected()) metaClient.stop();
  metaState = 0; metaMetaint = 0; metaCount = 0; metaPos = 0; metaBuf[0] = 0; metaHdr = "";
  // 注意: 不再在此清空 gState.title —— 曲名由 play()/stop() 显式管理,
  // 否则元数据 socket 一抖就把第2行曲名清掉(音频其实好好的)。
}

void AudioPlayer::updateMeta() {
  if (!metaEnabled) return;     // 已关闭(手动 M 或自动降级): 不再拉第二条流

  // 元数据自愈: 播放中但元数据连接已断, 等冷却后自动重连(不依赖音频库)
  if (metaState == 0) {
    if (gState.playing && gState.url.length() && (millis() - metaRetryMs > 10000)) {
      metaRetryMs = millis();
      startMeta(gState.url);
    }
    return;
  }
  if (!metaClient.connected()) {
    // 仅断开元数据 socket: 保留 gState.title(音频可能还正常), 等上面自愈重连
    metaClient.stop(); metaState = 0; metaMetaint = 0; metaCount = 0; metaPos = 0; metaBuf[0] = 0; metaHdr = "";
    return;
  }

  if (metaState == 1) {                                   // 读响应头, 找 icy-metaint
    while (metaClient.available()) {
      metaHdr += (char)metaClient.read();
      if (metaHdr.endsWith("\r\n\r\n")) {
        int i = metaHdr.indexOf("icy-metaint:");
        if (i >= 0) metaMetaint = metaHdr.substring(i + 12).toInt();
        if (metaMetaint > 0) { metaState = 2; metaCount = metaMetaint; }
        else                 { stopMeta(); }
        metaHdr = "";
        return;
      }
      if (metaHdr.length() > 4096) { stopMeta(); return; } // 异常保护
    }
    return;
  }

  // 每帧处理上限: 由 2048 降到 256, 避免元数据读取独占主循环、饿死音频解码造成断音
  int guard = 256;
  while (guard > 0 && metaClient.available()) {
    if (metaState == 2) {                                 // 丢弃音频字节, 直到下一个元数据块
      // 批量丢弃: 逐字节 read() 每次都是一次函数调用, 开销很大, 会明显抢占解码时间。
      // 顺带效果: 读得慢 -> TCP 窗口收紧 -> 服务器推得慢, 省下带宽给真正的音频流。
      int n = metaCount;
      int avail = metaClient.available();
      if (n > avail)                 n = avail;
      if (n > guard)                 n = guard;
      if (n > (int)sizeof(metaDrop)) n = (int)sizeof(metaDrop);
      if (n <= 0) break;
      metaClient.read(metaDrop, n);
      metaCount -= n;
      guard     -= n;
      if (metaCount <= 0) { metaState = 3; metaPos = 0; metaBuf[0] = 0; }
    }
    else if (metaState == 3) {                            // 读 1 字节长度指示
      guard--;
      int b = metaClient.read();
      metaCount = b * 16;
      if (metaCount <= 0) { metaState = 2; metaCount = metaMetaint; }   // 本块无元数据
      else                { metaState = 4; metaPos = 0; }
    }
    else if (metaState == 4) {                            // 读元数据块
      guard--;
      if (metaPos < (int)sizeof(metaBuf) - 1) metaBuf[metaPos++] = metaClient.read();
      else                               metaClient.read();            // 超长则丢弃
      if (--metaCount <= 0) {
        metaBuf[metaPos] = 0;
        String m = String(metaBuf);
        int a = m.indexOf("StreamTitle='");
        if (a >= 0) {
          int b = m.indexOf("'", a + 13);
          if (b > a + 13) {
            gState.title = icyToUtf8(m.substring(a + 13, b).c_str());
            gState.lastMetaMs = millis();   // 仅更新元数据时间戳, 不碰音频看门狗的 lastDataMs
            Serial.print("[META] 节目信息: ");
            Serial.println(gState.title);
          }
        } else {                                          // 兼容双引号写法
          int a2 = m.indexOf("StreamTitle=\"");
          if (a2 >= 0) {
            int b2 = m.indexOf("\"", a2 + 13);
            if (b2 > a2 + 13) {
              gState.title = icyToUtf8(m.substring(a2 + 13, b2).c_str());
              gState.titleEnc = gLastTitleEnc;                      // 记录编码判定(诊断用)
              gState.lastMetaMs = millis();
            }
          }
        }
        metaState = 2; metaCount = metaMetaint;           // 进入下一块
      }
    }
  }
}
