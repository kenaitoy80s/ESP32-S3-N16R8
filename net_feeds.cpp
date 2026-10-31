#include "../player_state.h"   // 联网数据层声明(原 net_feeds.h)已并入此头
#include "../config.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <sys/time.h>
#include <Preferences.h>   //运行时改天气地址的 NVS 存取
#include "audio_player.h"  // icyToUtf8(): 城市名等外部数据统一转 UTF-8(见fetchKeyless 里注释)

// ============================================================================
// 中文星期名(索引 = tm_wday: 0=周日)
// ============================================================================
const char* const WEEKDAY_CN[7] = { "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六" };

// ============================================================================
// 一、时间(网络授时 + 断电兜底)
// ----------------------------------------------------------------------------
// 为什么不用 configTzTime("America/Los_Angeles"): ESP32 固件里没有完整 tzdata 文件,
// 那条路会静默退回 UTC。改用 POSIX TZ 串 "PST8PDT,M3.2.0,M11.1.0"(旧金山, 含夏令时规则),
// 这种写法是 libc 内置支持的, 不需要任何数据文件。
// 开机时区 = config.h 的 TZ_STR(旧金山), 与天气定位无关, 所以不会再先跳出北京时间。
// ============================================================================
static bool s_timeSynced = false;     // 网络校时是否成功过
static bool s_timeInitDone = false;
// 当前生效的 POSIX TZ 串, 默认 = TZ_STR(旧金山, 夏令时由规则自动处理)。
static long s_tzOffsetSec = -8 * 3600;   // 仅作记录: 旧金山标准时间 UTC-8
static char s_tzName[48] = TZ_STR;       // 供串口打印核对; 带夏令时规则较长, 缓冲放大到 48
// 地理接口(ipwho.is)返回的时区, 在 fetchKeyless 里抓geo 时填好; open-meteo 的更优先。
[[maybe_unused]] static long s_geoOffsetSec  = 0;      // 0 = 未取到
[[maybe_unused]] static char s_geoUtcStr[16] = "";     // "+08:00" 形式, offset 缺失时的兜底

// 把 "UTC+HH:MM" / "UTC-05:30" 解析成秒; 也接受裸的 "+08:00" 形式。
// open-meteo 与 ipwho.is 都是这种格式(+HH:MM / +H:MM / +HH), 解析成 POSIX TZ 串也更稳。
[[maybe_unused]] static long parseUtcOffset(const String& s, bool& ok) {
  ok = false;
  int i = 0;
  while (i < s.length() && (s[i] == ' ')) i++;
  if (i < s.length() && (s[i] == 'U' || s[i] == 'u')) {
    while (i < s.length() && s[i] != '+' && s[i] != '-') i++;   // 跳过 "UTC"
  }
  if (i >= s.length() || (s[i] != '+' && s[i] != '-')) return 0;
  const int sign = (s[i] == '-') ? -1 : 1;
  i++;
  int hh = 0, mm = 0, digits = 0;
  while (i < s.length() && s[i] >= '0' && s[i] <= '9' && digits < 2) { hh = hh * 10 + (s[i] - '0'); i++; digits++; }
  if (i < s.length() && s[i] == ':') {
    i++;
    while (i < s.length() && s[i] >= '0' && s[i] <= '9' && digits < 4) { mm = mm * 10 + (s[i] - '0'); i++; digits++; }
  } else if (digits <= 2) {
    // 无冒号且只给了 1~2 位: 视作小时(ipwho.is 的 offset 是秒, 不会走到这里)
  }
  ok = true;
  return sign * (hh * 3600L + mm * 60L);
}

// ★按地理定位结果设置时区。地理接口(ipwho.is)已经返回了该处的 UTC 时差,
//   用它替代"写死 UTC+8" —— 设备在海外/跨时区出差时, 时钟与天气时间才对得上。
//   posixTzStr() 生成 POSIX TZ 串: POSIX 的约定是"符号与 UTC 偏移相反"
//   (UTC+8 写作 CST-8; UTC-5 写 EST5), 搞反了会让时间差整整反一个符号。
[[maybe_unused]] static void applyTimezone(long offsetSec, const char* label) {
  char buf[24];
  const int offH = (int)(offsetSec / 3600);
  const int offM = (int)(labs((long)offsetSec % 3600) / 60);
  // ★★★ POSIX TZ 串的偏移【必须取负】★★★(2026-10-07 修, 这个符号搞反过一次)
  //   规范: TZ 串里的偏移是"本地时间 + 它 = UTC", 即与 UTC 偏移【方向相反】。
  //     UTC+8(成都) 必须写 "CST-8"  -> 本地 = UTC - (-8) = UTC+8   ✔
  //     若写成 "CST8"(漏了负号) -> 本地 = UTC - 8 = UTC-8, 整整差反 16 小时。
  //   实际症状(用户实测): 成都显示 21:14 而实际 13:13、日期还退到前一天 ——
  //     UTC+8 被当成 UTC-8, 再叠加"显示的是 UTC+8 应有的 8 小时差" = 差反 16 小时。
  //   带分钟的同理: UTC+5:30 -> "IST-5:30"; UTC-3:30 -> "NST3:30"。
  if (offM == 0) snprintf(buf, sizeof(buf), "%s%d", label, -offH);
  else           snprintf(buf, sizeof(buf), "%s%d:%02d", label, -offH, offM);
  const bool same = (s_tzOffsetSec == offsetSec) && (strcmp(s_tzName, buf) == 0);
  s_tzOffsetSec = offsetSec;
  strncpy(s_tzName, buf, sizeof(s_tzName) - 1);
  s_tzName[sizeof(s_tzName) - 1] = 0;
  setenv("TZ", s_tzName, 1);
  tzset();                      // 立刻生效, 否则 mktime/localtime 仍按旧 TZ
  (void)same;                   // 重复设置也无害, tzset 很便宜
  // 日志同时打出"POSIX 串"与"真实 UTC 偏移"两者的对照 —— 符号写反时一眼能看出来。
  Serial.printf("[TIME] 时区已按定位设置: TZ=%s (真实偏移 UTC%+03d:%02d, 标签 %s)\n",
                s_tzName, offH, offM, label);
}

static void applyFallbackTime() {
  // 兜底: 把【编译那一刻的本机时间】当成开机起点。
  // __DATE__ " " __TIME__ 形如 "Oct  4 2026 22:21:53"; strptime 能吃多空格。
  struct tm t;
  memset(&t, 0, sizeof(t));
  if (strptime(TIME_FALLBACK_STR, "%b %d %Y %H:%M:%S", &t) == nullptr) return;
  t.tm_isdst = 0;
  // 此刻 TZ 已是当前生效时区, mktime 会把"本地时间"换算成 epoch, 正合我们要把系统钟设成
  // "本地时间的某刻"这件事; 之后本地时间继续按该时区走。
  time_t v = mktime(&t);
  if (v > 0) { struct timeval tv; tv.tv_sec = v; tv.tv_usec = 0; settimeofday(&tv, nullptr); }
}

// 按 POSIX TZ 串(含夏令时规则)设置时区并立刻生效。开机固定调一次, 之后与天气无关。
static void applyPosixTz(const char* posixTz) {
  strncpy(s_tzName, posixTz, sizeof(s_tzName) - 1);
  s_tzName[sizeof(s_tzName) - 1] = 0;
  setenv("TZ", s_tzName, 1);
  tzset();                              // 立刻生效, 否则 localtime 仍按旧 TZ
  Serial.printf("[TIME] 开机时区: TZ=%s\n", s_tzName);
}

void timeInit() {
  // ★开机第一步就设成旧金山时区(来自 config.h 的 TZ_STR), 不再先用 UTC+8。
  //   这样即使天气/定位一直失败, 时钟也始终是旧金山时间。
  applyPosixTz(TZ_STR);
  applyFallbackTime();            // 先兜底, 保证断网也有个能看的时间(NTP 一到就会被校准)
  // ★configTzTime: 一次设好 TZ(含夏令时规则) + 三个海外 NTP 服务器。
  //   不用 configTime(偏移秒数): 它只能给固定偏移, 而且会把我们设好的 TZ 串覆盖掉。
  configTzTime(TZ_STR, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
  s_timeInitDone = true;
  s_timeSynced = false;           // 等首次同步成功再置位
}

bool timeSynced() {
  if (s_timeSynced) return true;
  // 主动探一次: configTime 是后台任务在同步, 这里只在"尚未成功过"时查系统时间是否已合理
  // (S3 上 RTC 也会走网络, 开机几秒内通常已拿到时间)。
  if (!s_timeInitDone) return false;
  time_t now = time(nullptr);
  // 2024-01-01 之后的都算"像样"; 早于它说明还没校上(仍是 1970 纪元)
  if (now > 1704067200) { s_timeSynced = true; return true; }
  return false;
}

void timeLocal(struct tm* out) {
  if (!out) return;
  time_t now = time(nullptr);
  localtime_r(&now, out);
}

// ============================================================================
// 二、农历(公历 -> 干支年 + 生肖 + 农历月日)
// ----------------------------------------------------------------------------
// 表: 通行 lunarInfo 压缩表(1900-2100, 每年 1 个 32bit)。编码含义:
//   bit16      = 闰月是大月(30天)
//   bit15..bit4= bit(15-i) 为 1 表示第 i+1 个月是 30 天(否则 29 天)
//   bit3..bit0 = 闰月月份(0 = 无闰月)
// 基准: 1900-01-31 == 农历 1900 年正月初一。
// ============================================================================
static const uint32_t sLunarInfo[201] = {  // 1900 .. 2100
0x04bd8,0x04ae0,0x0a570,0x054d5,0x0d260,0x0d950,0x16554,0x056a0,0x09ad0,0x055d2,
0x04ae0,0x0a5b6,0x0a4d0,0x0d250,0x1d255,0x0b540,0x0d6a0,0x0ada2,0x095b0,0x14977,
0x04970,0x0a4b0,0x0b4b5,0x06a50,0x06d40,0x1ab54,0x02b60,0x09570,0x052f2,0x04970,
0x06566,0x0d4a0,0x0ea50,0x06e95,0x05ad0,0x02b60,0x186e3,0x092e0,0x1c8d7,0x0c950,
0x0d4a0,0x1d8a6,0x0b550,0x056a0,0x1a5b4,0x025d0,0x092d0,0x0d2b2,0x0a950,0x0b557,
0x06ca0,0x0b550,0x15355,0x04da0,0x0a5b0,0x14573,0x052b0,0x0a9a8,0x0e950,0x06aa0,
0x0aea6,0x0ab50,0x04b60,0x0aae4,0x0a570,0x05260,0x0f263,0x0d950,0x05b57,0x056a0,
0x096d0,0x04dd5,0x04ad0,0x0a4d0,0x0d4d4,0x0d250,0x0d558,0x0b540,0x0b6a0,0x195a6,
0x095b0,0x049b0,0x0a974,0x0a4b0,0x0b27a,0x06a50,0x06d40,0x0af46,0x0ab60,0x09570,
0x04af5,0x04970,0x064b0,0x074a3,0x0ea50,0x06b58,0x055c0,0x0ab60,0x096d5,0x092e0,
0x0c960,0x0d954,0x0d4a0,0x0da50,0x07552,0x056a0,0x0abb7,0x025d0,0x092d0,0x0cab5,
0x0a950,0x0b4a0,0x0baa4,0x0ad50,0x055d9,0x04ba0,0x0a5b0,0x15176,0x052b0,0x0a930,
0x07954,0x06aa0,0x0ad50,0x05b52,0x04b60,0x0a6e6,0x0a4e0,0x0d260,0x0ea65,0x0d530,
0x05aa0,0x076a3,0x096d0,0x04afb,0x04ad0,0x0a4d0,0x1d0b6,0x0d250,0x0d520,0x0dd45,
0x0b5a0,0x056d0,0x055b2,0x049b0,0x0a577,0x0a4b0,0x0aa50,0x1b255,0x06d20,0x0ada0,
0x14b63,0x09370,0x049f8,0x04970,0x064b0,0x168a6,0x0ea50,0x06b20,0x1a6c4,0x0aae0,
0x0a2e0,0x0d2e3,0x0c960,0x0d557,0x0d4a0,0x0da50,0x05d55,0x056a0,0x0a6d0,0x055d4,
0x052d0,0x0a9b8,0x0a950,0x0b4a0,0x0b6a6,0x0ad50,0x055a0,0x0aba4,0x0a5b0,0x052b0,
0x0b273,0x06930,0x07337,0x06aa0,0x0ad50,0x14b55,0x04b60,0x0a570,0x054e4,0x0d160,
0x0e968,0x0d520,0x0daa0,0x16aa6,0x056d0,0x04ae0,0x0a9d4,0x0a2d0,0x0d150,0x0f252,
0x0d520
};

static const char* const GAN[10] = { "甲","乙","丙","丁","戊","己","庚","辛","壬","癸" };
static const char* const ZHI[12] = { "子","丑","寅","卯","辰","巳","午","未","申","酉","戌","亥" };
static const char* const ZODIAC[12] = { "鼠","牛","虎","兔","龙","蛇","马","羊","猴","鸡","狗","猪" };
static const char* const LMONTH[13] = { "", "正月","二月","三月","四月","五月","六月",
                                         "七月","八月","九月","十月","冬月","腊月" };

static inline int leapMonthOf(int y) { return (int)(sLunarInfo[y - 1900] & 0xf); }
static inline int leapDaysOf (int y) {
  if (!leapMonthOf(y)) return 0;
  return (sLunarInfo[y - 1900] & 0x10000u) ? 30 : 29;
}
static inline int monthDaysOf(int y, int m) {
  return (sLunarInfo[y - 1900] & (0x10000u >> m)) ? 30 : 29;   // m=1..12
}
static int lunarYearDays(int y) {
  int sum = 348;                                   // 12 * 29
  for (int i = 0x8000; i > 0x8; i >>= 1) sum += (sLunarInfo[y - 1900] & i) ? 1 : 0;
  return sum + leapDaysOf(y);
}

// 公历 -> 距 1970-01-01 的天数(Howard Hinnant 的 days_from_civil, 无需查表/无闰年判断)
static long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= (m <= 2);
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097L + (long)doe - 719468L;
}

static void lunarDayName(int d, char* out) {
  // ★2026-10-06 修 —— 这是"农历日期显示成乱码/方框"的真正根因, 与字库无关:
  //   旧版写的是 NUM[..][0] 和 DIG[..] ——那是【UTF-8 的单个首字节】而不是整个汉字。
  //   "廿"=E5 BB BF 只取了 0xE5, "四"=E5 9B 9B 也只取了 0xE5, 拼出来是 {E5,E5,0}
  //   这种非法 UTF-8 序列。drawCN() 按 UTF-8 解码时把它当成一个不存在的汉字,
  //   只能画占位框 -> 屏上"廿四"就变成方框乱码(只有 11/21/22…29 这些日用得上"廿/三"开头,
  //   所以是"有时正常有时乱")。必须整字 strcpy/strcat。
  //   合法组合: 1~9 初X / 10 初十 / 11~19 十X / 20 二十 / 21~29 廿X / 30 三十。
  static const char* NUM[4] = { "初", "十", "廿", "三" };
  static const char* DIG[10] = { "", "一", "二", "三", "四", "五", "六", "七", "八", "九" };
  if (d < 1 || d > 30) { out[0] = 0; return; }
  if (d == 10) { strcpy(out, "初十"); return; }
  if (d == 20) { strcpy(out, "二十"); return; }
  if (d == 30) { strcpy(out, "三十"); return; }
  strcpy(out, NUM[(d / 10) % 4]);
  strcat(out, DIG[d % 10]);
}

LunarInfo lunarFromSolar(int y, int m, int d) {
  LunarInfo r;
  if (y < 1900 || y > 2100) return r;                    // 表范围外
  r.ok = true;
  // 距 1900-01-31(= 农历 1900 正月初一)的天数
  long off = daysFromCivil(y, (unsigned)m, (unsigned)d) - daysFromCivil(1900, 1, 31);
  if (off < 0) { r.ok = false; return r; }
  // 逐年扣减, 定位农历年
  int ly = 1900;
  while (ly <= 2100) {
    int yd = lunarYearDays(ly);
    if (off < yd) break;
    off -= yd;
    ly++;
  }
  if (ly > 2100) { r.ok = false; return r; }
  // 逐月扣减, 定位农历月/日(闰月插在该月号之后)
  int lm = 1;
  bool isLeap = false;
  for (;;) {
    int len = monthDaysOf(ly, lm);
    if (off < len) break;
    off -= len;
    if (leapMonthOf(ly) == lm) {         // 该月号后面可能跟一个闰月
      int llen = leapDaysOf(ly);
      if (off < llen) { isLeap = true; break; }
      off -= llen;
    }
    lm++;
    if (lm > 12) break;                  // 理论上到不了(上面已按整年天数扣过)
  }
  int ld = (int)off + 1;
  r.monthNum = lm;
  r.dayNum   = ld;
  // 干支年 + 生肖
  snprintf(r.ganZhiYear, sizeof(r.ganZhiYear), "%s%s", GAN[(ly - 4) % 10], ZHI[(ly - 4) % 12]);
  snprintf(r.zodiac,     sizeof(r.zodiac),     "%s", ZODIAC[(ly - 1900) % 12]);
  // 月 + 日
  char dn[8];
  lunarDayName(ld, dn);
  if (isLeap) snprintf(r.monthDay, sizeof(r.monthDay), "闰%s%s", LMONTH[lm], dn);
  else        snprintf(r.monthDay, sizeof(r.monthDay), "%s%s",     LMONTH[lm], dn);
  snprintf(r.dayName, sizeof(r.dayName), "%s", dn);
  return r;
}
// ============================================================================
// 三、天气 —— 默认【免密钥】: 用公网 IP 反查所在地, 再按经纬度取天气
// ----------------------------------------------------------------------------
// ① 地理: ipwho.is        GET https://ipwho.is/  -> { city, latitude, longitude }
// ② 天气: open-meteo.com  GET .../v1/forecast?latitude=..&longitude=..
//                         -> current{温度/湿度/体感/WMO天气码/风速/风向} + daily{今明最高最低温}
// 两次请求都在主循环里串行发(实测 1~3s), 每小时才一次。音频泵是独立任务, 不受这里阻塞影响。
// ★"把本地外网地址放网上就能搜到本地天气"就是你要的效果: 设备在家里的 WiFi 上,
//   服务端看到的是你家出口 IP, 于是返回你家所在城市与该地天气。
// 备选: config.h 里 WX_PROV_KEYLESS=0 切和风天气(需 Key, 国内更快更稳)。
//
// ★两个地址都能在【运行时】改(串口 X / Y, 存 NVS), 不用重新编译 ——
//   万一这两个海外服务在某个网络下慢/不通, 换成别的镜像或国内接口即可。
// ============================================================================
static NetworkClientSecure s_wxClient;
static bool s_wxClientReady = false;

// 运行时可改的地址(NVS "wx"); 空串 = 用 config.h 的编译期默认值
static char s_geoUrl[128] = {0};
static char s_fcUrl[128]  = {0};
static bool s_urlLoaded = false;

static void loadWxUrls() {
  if (s_urlLoaded) return;
  Preferences p;
  p.begin("wx", true);
  p.getString("geo", s_geoUrl, sizeof(s_geoUrl));
  p.getString("fc",  s_fcUrl,  sizeof(s_fcUrl));
  p.end();
  s_urlLoaded = true;
}
const char* weatherGeoUrl() { loadWxUrls(); return s_geoUrl[0] ? s_geoUrl : WX_GEO_URL; }
const char* weatherFcUrl()  { loadWxUrls(); return s_fcUrl[0]  ? s_fcUrl  : WX_FC_URL;  }

// 运行时改地址(串口 X/Y); 传空串 = 恢复默认。改完立即生效(下次取数就用新地址)。
void weatherSetUrl(bool isGeo, const char* url) {
  loadWxUrls();
  char*  dst = isGeo ? s_geoUrl : s_fcUrl;
  size_t cap = sizeof(s_geoUrl);
  strncpy(dst, url ? url : "", cap - 1);
  dst[cap - 1] = 0;
  Preferences p;
  p.begin("wx", false);
  if (isGeo) p.putString("geo", s_geoUrl); else p.putString("fc", s_fcUrl);
  p.end();
  s_urlLoaded = false;          // 置无效, 下次读时重新从 NVS 取(顺带确认刚写进去)
  weatherInit();                // 作废旧 TLS 会话, 换地址后重新握手
}

void weatherInit() {
  loadWxUrls();
  if (s_wxClientReady) { s_wxClient.stop(); s_wxClientReady = false; }
}

// ---- WMO 天气码(国际标准 0~99) -> 中文; 屏上天气文字就是查这张表 ----
static const char* wmoText(int c) {
  switch (c) {
    case 0:  return "晴";
    case 1:  return "晴间多云";
    case 2:  return "多云";
    case 3:  return "阴";
    case 45: return "雾";
    case 48: return "雾凇";
    case 51: case 52: case 53: case 54: case 55: return "毛毛雨";
    case 56: case 57: return "冻毛毛雨";
    case 61: return "小雨";
    case 63: return "中雨";
    case 65: return "大雨";
    case 66: case 67: return "冻雨";
    case 71: return "小雪";
    case 73: return "中雪";
    case 75: return "大雪";
    case 77: return "米雪";
    case 80: return "小阵雨";
    case 81: return "阵雨";
    case 82: return "强阵雨";
    case 85: return "小阵雪";
    case 86: return "大阵雪";
    case 95: return "雷阵雨";
    case 96: case 99: return "雷阵雨伴冰雹";
    default: return "未知天气";
  }
}
// 风向度数 -> 16 方位中文(每格 22.5°)
static const char* windDirCN(int deg) {
  static const char* d[16] = { "北风","北东北风","东北风","东东北风","东风","东东南风","东南风","南东南风",
                               "南风","南西南风","西南风","西西南风","西风","西西北风","西北风","北西北风" };
  int i = ((deg % 360) + 360) % 360;
  return d[(i + 11) / 22 & 15];
}
// 风速 m/s -> 蒲福风级 0~12
static uint8_t beaufort(float ms) {
  static const float lim[12] = { 0.3f,1.6f,3.4f,5.5f,8.0f,10.8f,13.9f,17.2f,20.8f,24.5f,28.5f,32.7f };
  for (uint8_t i = 0; i < 12; i++) if (ms < lim[i]) return i;
  return 12;
}

// 一次 HTTPS GET; 成功把响应体放进 body, 返回 true
static bool httpGet(const char* url, String& body) {
  if (!s_wxClientReady) {
    s_wxClient.setTimeout(WEATHER_HTTP_TIMEOUT);
    // arduino-esp32 3.x 没有内置根证书符号(2.x 的 MBEDTLS_ROOT_CA 已删): 粘了证书就校验, 否则不校验
    if (strlen(WEATHER_CA_PEM) > 16) s_wxClient.setCACert(WEATHER_CA_PEM);
    else                             s_wxClient.setInsecure();
    s_wxClientReady = true;
  }
  s_wxClient.stop();
  HTTPClient http;
  http.setTimeout(WEATHER_HTTP_TIMEOUT);
  http.setConnectTimeout(WEATHER_HTTP_TIMEOUT);
  http.setReuse(false);
  if (!http.begin(s_wxClient, url)) return false;
  const int code = http.GET();
  if (code != 200) { http.end(); return false; }
  body = http.getString();
  http.end();
  return body.length() > 10;
}

// ---- 免密钥链路: ipwho.is 定位 -> open-meteo 取天气 ----
static bool fetchKeyless(WeatherInfo& out) {
#if WX_GEO_FIXED
  // ★固定旧金山: 不再请求 ipwho.is(省一次网络往返, 也不会被 VPN/出口 IP 带偏)。
  const double lat = HOME_LAT;
  const double lon = HOME_LON;
  WeatherInfo w;
  w.city = HOME_CITY;               // 简体 UTF-8 常量, 无需转码
#else
  String geo;
  if (!httpGet(weatherGeoUrl(), geo)) return false;
  JsonDocument g;
  if (deserializeJson(g, geo)) return false;
  if (!(g["success"] | false)) return false;              // ipwho.is 用 success 表示成败
  const double lat = g["latitude"]  | 0.0;
  const double lon = g["longitude"] | 0.0;
  if (lat == 0.0 && lon == 0.0) return false;
  WeatherInfo w;
  // ★转 UTF-8: 城市名来自地理接口, 可能是 GBK/双重编码 —— 不转的话屏上就是空方框
  //   (drawUTF8 遇非法 UTF-8 会整字跳过), 后台 JSON 也会乱码。
  //   注意 ArduinoJson 的 `|` 返回 const char*, 不能直接 .c_str(), 要先落成 String。
  w.city = icyToUtf8(String(g["city"] | "").c_str());

  // 先记下 ipwho.is 的时区(offset 优先, 其次 utc 串), 供下面 open-meteo 失败时兜底。
  s_geoOffsetSec = 0; s_geoUtcStr[0] = 0;
  {
    JsonObject tz = g["timezone"].as<JsonObject>();
    if (!tz.isNull()) {
      if (tz.containsKey("offset")) s_geoOffsetSec = (long)(tz["offset"] | 0);
      const char* u = tz["utc"] | "";
      strncpy(s_geoUtcStr, u, sizeof(s_geoUtcStr) - 1);
      s_geoUtcStr[sizeof(s_geoUtcStr) - 1] = 0;
    }
  }
#endif

  String url(weatherFcUrl());
  url.replace("{lat}", String(lat, 4).c_str());
  url.replace("{lon}", String(lon, 4).c_str());
  url += (url.indexOf('?') >= 0) ? "&" : "?";
  url += "latitude="  + String(lat, 4) + "&longitude=" + String(lon, 4)
      + "&current="   + WEATHER_FIELDS
      + "&daily="     + WEATHER_DAILY
      + "&timezone=auto&forecast_days=2";
  String body;
  if (!httpGet(url.c_str(), body)) return false;

  JsonDocument d;
  if (deserializeJson(d, body)) return false;
  JsonObject cur = d["current"].as<JsonObject>();
  if (cur.isNull()) return false;                          // 出错时只有 error 字段

  w.tempC    = (int)round(cur["temperature_2m"]        | 0.0);
  w.feelsC   = (int)round(cur["apparent_temperature"] | 0.0);
  w.humidity = cur["relative_humidity_2m"] | 0;
  w.text     = wmoText(cur["weather_code"] | 0);
  w.windDir  = windDirCN(cur["wind_direction_10m"] | 0);
  w.windScale = String(beaufort(cur["wind_speed_10m"] | 0.0));   // 只存级数, 显示层补"级"
  // ★2026-10-06 修 —— "明天 0/0°C"的根因(和风那家格式不同, 别照抄):
  //   open-meteo 返回的 daily 是【对象】而不是数组 ——
  //     "daily":{"time":[...],"temperature_2m_max":[...],"temperature_2m_min":[...]}
  //   旧版写 d["daily"].as<JsonArray>() 永远得到空数组, 于是"今日/明日温区"恒为 0,
  //   屏上就是那句"明天 0/0°C"。正确姿势: 取对象 -> 按字段名各取一条【并行数组】-> 取下标。
  JsonObject daily = d["daily"].as<JsonObject>();
  if (!daily.isNull()) {
    JsonArray mx = daily["temperature_2m_max"].as<JsonArray>();
    JsonArray mn = daily["temperature_2m_min"].as<JsonArray>();
    if (mx.size() > 0 && mn.size() > 0) {
      w.tmaxC = (int)round(mx[0] | 0.0);
      w.tminC = (int)round(mn[0] | 0.0);
      if (mx.size() > 1 && mn.size() > 1) {          // forecast_days=2 -> 下标 1 就是明天
        w.tmax2C = (int)round(mx[1] | 0.0);
        w.tmin2C = (int)round(mn[1] | 0.0);
      }
    }
  }
#if !WX_GEO_FIXED   // 固定旧金山时不按定位改时区 —— 时区只由开机的 TZ_STR 决定, 天气成败都不影响时钟
  // ★时区跟着定位走(2026-10-07): 时区不再写死 UTC+8。放在【天气解析之后】(要用 open-meteo 的
  //   utc_offset_seconds)。三个来源, 按可靠性排序:
  //   ① open-meteo 的 utc_offset_seconds —— 带符号的整数秒(如纽约 -14400), 最不易解析错,
  //      且它与天气是同一份数据, 天气时间与显示的时钟天然一致。
  //   ② ipwho.is 的 timezone.offset(秒) / timezone.utc("+08:00" 串)。
  //   ③ 都没有 -> 保持开机默认时区。
  //   ★夏令时: 两个接口都只给"当前偏移"不给规则; 实测 is_dst 为真时 offset 已含夏令时
  //     (纽约 UTC-4), 所以【不要】再额外 +3600 —— 那是重复加。代价: 跨夏令时的那段时间
  //     不会自动切换, 下一个刷新周期(每小时)会纠正回来。
  {
    long off = 0; bool ok = false;
    if (d.containsKey("utc_offset_seconds")) {
      off = (long)d["utc_offset_seconds"].as<long long>();
      ok = true;                                   // 0 就是 UTC 本身, 也是有效值
    }
    if (!ok && s_geoOffsetSec != 0) { off = s_geoOffsetSec; ok = true; }
    if (!ok) off = parseUtcOffset(s_geoUtcStr, ok);
    if (ok) {
      // 合理性闸门: 真实时区在 UTC-12 ~ UTC+14(含 +14 的新西兰); 越界说明字段被误读, 别乱改。
      if (off >= -12 * 3600L && off <= 14 * 3600L) applyTimezone(off, "LST");
      else Serial.printf("[TIME] 定位返回的时区偏移越界(%ld 秒), 保持原时区\n", off);
    }
  }
#endif

  w.valid = true;
  w.updMs = millis();
  out = w;
  return true;
}

// ---- 和风天气 QWeather(需 Key; 国内更快更稳) ----
static bool fetchQWeather(WeatherInfo& out) {
  const char* host = strlen(WEATHER_HOST) ? WEATHER_HOST : WEATHER_API_BASE;
  String url = String("https://") + host + "/v7/weather/now?location="
             + WEATHER_LOCATION + "&key=" + WEATHER_API_KEY;
  String body;
  if (!httpGet(url.c_str(), body)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  JsonObject now = doc["now"].as<JsonObject>();
  if (now.isNull()) return false;
  WeatherInfo w;
  w.city      = icyToUtf8(String(doc["location"]["name"] | "").c_str());   // 同上: 外部数据要转 UTF-8
  w.text      = now["text"]      | "";
  w.tempC     = now["temp"]      | 0;
  w.feelsC    = now["feelsLike"] | 0;
  w.humidity  = now["humidity"]  | 0;
  w.windDir   = now["windDir"]   | "";
  w.windScale = now["windScale"] | "";
  JsonArray daily = doc["daily"].as<JsonArray>();
  if (daily.size() > 0) { w.tminC  = daily[0]["tempMin"] | 0; w.tmaxC  = daily[0]["tempMax"] | 0; }
  if (daily.size() > 1) { w.tmin2C = daily[1]["tempMin"] | 0; w.tmax2C = daily[1]["tempMax"] | 0; }
  w.valid = true;
  w.updMs = millis();
  out = w;
  return true;
}

bool weatherFetch(WeatherInfo& out) {
  if (!gState.wifiConnected) return false;                    // 没网就别试
#if WX_PROV_KEYLESS
  return fetchKeyless(out);                                    // 免密钥: 开机即用, 无需任何注册
#else
  if (strcmp(WEATHER_API_KEY, E_WX_NO_KEY) == 0) return false; // 没配 Key 就不发请求
  return fetchQWeather(out);
#endif
}

// 主循环每圈调; 内部按节流决定这圈要不要真的发请求。
// 规则: 成功后每 WEATHER_POLL_MS(1h) 取一次;
//       ★失败后【退避重试】: 8s→16s→32s→…封顶 WEATHER_RETRY_MAX_MS(5 分钟), 成功即清零。
//       (旧版成功过一次后, 下一次失败要干等整整 1 小时; 现在失败会很快重试, 但不会猛刷接口。)
// 时钟只靠 NTP + TZ_STR, 这里无论成败都不碰时区, 所以重试期间时间始终正确。
void weatherTick() {
  gState.timeValid = timeSynced();
  static uint32_t lastTry = 0;
  static uint32_t gap = (uint32_t)WEATHER_BOOT_TRY_MS;   // 距下次尝试的间隔
  static uint8_t  fails = 0;                             // 连续失败次数
  if ((uint32_t)(millis() - lastTry) < gap) return;
  lastTry = millis();
  if (!gState.wifiConnected) {                           // 没连上 WiFi: 不算失败, 短间隔等着
    gap = (uint32_t)WEATHER_BOOT_TRY_MS;
    return;
  }
  WeatherInfo w;
  if (weatherFetch(w)) {
    gState.wx = w;
    fails = 0;
    gap = (uint32_t)WEATHER_POLL_MS;                     // 成功: 回到每小时一次
  } else {
    if (fails < 16) fails++;
    uint32_t g = (uint32_t)WEATHER_BOOT_TRY_MS;
    for (uint8_t i = 1; i < fails && g < (uint32_t)WEATHER_RETRY_MAX_MS; i++) g *= 2;   // 指数退避
    if (g > (uint32_t)WEATHER_RETRY_MAX_MS) g = (uint32_t)WEATHER_RETRY_MAX_MS;
    gap = g;
    Serial.printf("[WX] 取天气失败(连续 %u 次), %lu 秒后重试\n", (unsigned)fails, (unsigned long)(g / 1000));
  }
}
