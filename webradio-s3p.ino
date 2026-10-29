
#include "src/config.h"
#include "rom/rtc.h"        // rtc_get_reset_reason: 打印上次复位原因(定位死机类型)
#include "esp_system.h"     // esp_reset_reason: 复位原因(可读文本)
#include "src/player_state.h"
#include "src/ui/display_ui.h"
#include "src/ui/input_btn.h"
#include "src/core/audio_player.h"
#include "src/core/station_store.h"
#include "src/core/sd_card.h"    // 阶段②: 本地 SD 卡播放(SD_MMC 1-bit; 与电台列表同构的浏览界面)
#include "src/core/alarm.h"      // 定时开机(闹钟): 到点自动切台播放 / 到点自动停播
#include <SD_MMC.h>     // 阶段②: sdTick() 里查文件大小(SD_MMC 驱动只在主循环上下文被访问)
#include "src/net/wifi_portal.h"
#include "src/net/web_server.h"
#include "src/ui/ir_learn.h"   // 红外遥控学习(RMT 接收, 学任意遥控器)
#include "esp_sleep.h"   // 深度休眠: esp_deep_sleep_start / esp_sleep_enable_ext1_wakeup
#include "esp_timer.h"   // 启动看门狗: 卡在"开机中"时自动重启(见 bootWdtStart)
#include "esp_ota_ops.h" // ★启动分区自检 + 启动即确认(见 setup 开头; 排查"刷了却不生效")
#include "esp_wifi.h"    // esp_wifi_stop: 关机时彻底关闭 WiFi 射频
#include "esp_heap_caps.h" // heap_caps_get_free_size: 只看内部堆(不含 PSRAM)的内存兜底判据
#include "driver/gpio.h" // gpio_reset_pin: 关机前把红外脚从 RMT 占用改回纯输入

// 串口命令 X/Y 用: 读一行地址(等回车)后写进 NVS。前置声明放在这里, 定义见文件后部。
static void readWxUrlLine(bool isGeo);
extern bool btStop();   // Arduino-ESP32 核心: 关闭蓝牙(关机省电); 声明于 esp32-hal-bt.h

DisplayUI   display;
InputBtn    btn;
StationStore store;
AudioPlayer player;
WiFiPortal  portal;
WebServerSvc web;
bool gAmpOn = false;    // 功放使能脚当前状态(updateAmp 维护, 供 /api/state 诊断)
bool gNetDacOn = false; // 网络侧 TDA1387 供电开关状态(供 /api/state 诊断; web_server.cpp 用 extern 引用)
// 双通道日志实例: config.h 里把全局 Serial 重定向到这里(见 config.h 顶部的 TeeLog)。
// 作用: ① UART0 与原生 USB 口都能看到日志; ② 写超时=0, 串口输出绝不阻塞主循环。
TeeLog LogOut;
#if WR_HAS_USB_CDC && WR_USB_NEEDS_BEGIN
// 内核未开 "USB CDC On Boot" 时 core 不提供全局 USB CDC 实例, 这里自建一个给日志用。
// (native USB = USB-Serial-JTAG, 就在 GPIO19/20, 与 UART0 的 GPIO43/44 是两条独立通路)
HWCDC wrUsbCdc;
#endif

void handleButtons();
void updatePlaybackWatchdog();
void updateDisplay();
void updateAmp();            // 功放使能: 真正出声时拉高, 停止/待机/连接中拉低
void checkHeapSafety();      // 24h 兜底: 内存过低自动重启
void heartbeat();            // 心跳日志(每分钟)
static bool gAutoPlayPending = false;   // 开机自动续播待触发标志(等 WiFi 真正连上再播)
static uint32_t gAutoPlayAt = 0;        // 开机续播的"最早开播时刻"(millis); 0 = 未排期
// 关机请求: 由硬件入口(电源键 GPIO3 长按 / 已学 "power" 遥控键 / 串口 Z)置位, 真正写 SPIFFS + 深睡
// 由主循环执行(绝不能在异步回调/中断上下文里直写文件系统 / 进深睡)。
// ★2026-09-23: 后台"关机"按钮(WS poweroff)已移除 —— 深度休眠后 Web 服务停止, 无法从后台开机,
//   点了就回不来。故本变量现在只由硬件入口置位(web_server.cpp 已不再 extern 引用它)。
//   仍保留为全局(非 static), 便于 .ino 内各函数共用。
bool gShutdownPending = false;

// ---- 主界面临时提示(2026-09-22 加) ----
// 由来: 某些操作(如网络侧 DAC 供电通断)会在合流点产生瞬态, 或某些键按下后屏上不会立刻变化,
// 用户可能以为"键坏了"。所以关键操作都给回执: 屏上第2行显示 2.6s 的提示文字 + 串口一行日志。
static String   gNoticeMsg;
static uint32_t gNoticeUntil = 0;
void uiNotice(const String& msg, uint32_t ms = 2600) {
  gNoticeMsg   = msg;
  gNoticeUntil = millis() + ms;
  display.invalidate();          // ★必须: 否则整帧跳过关掉这次提示(内容指纹算的是主界面字段)
  Serial.printf("[UI] 提示: %s\n", msg.c_str());
}

// ============ 开关机(深度休眠) + 功放开关 ============
// 关机 = 停播 + 关 WiFi/蓝牙 + 清屏 + 背光灭 + 功放脚拉低 + ESP32 进 deep sleep(见 enterDeepSleep)。
// 唤醒 = ①按电源键(POWER_BTN_PIN 拉低); ②按【已学成 "power" 动作的那一个遥控键】(VS1838B 输出拉低)。
//   注意②: 硬件 EXT1 唤醒只看 IR 脚是否拉低(任何遥控码都会让 VS1838B 拉低), 无法在深睡期区分具体键,
//   故唤醒后由 ir_confirm_power() 软件确认 —— 只认已学的 power 码, 其它遥控键/遥控器的信号会立刻回深睡。
// 深度休眠中无 Web/串口, 故"开机"只能靠硬唤醒; 关机能从按键或遥控触发。功放脚由本模块独占控制。
// ★2026-10-08 补充"熄屏待机"这一态(别与深睡混淆):
//   面板/Web 的【睡眠定时器】到点后设备只是"停播 + 熄屏", 仍然通电(Web/串口都还在) ——
//   这一态记为 gState.sleepStandby。此时【电源键】(面板短按 / 遥控 power 键)的语义反转为
//   "开机"(亮屏 + 恢复播放), 而不是继续往下关机。原因: 用户把"睡眠"当"待机", 想用遥控叫醒,
//   旧实现却会让这一按直接进深睡, 再从深睡唤醒还须在窗口内再按一次 —— 表现成"遥控开机有问题"。
//   红外【定时关机】(IR_TIMER)到点才是真关机, 走 enterDeepSleep() 进深睡, 语义不变。
void enterDeepSleep();                  // 关机: 进深度休眠(不返回)
static void goToSleep();                // 仅配置唤醒源 + 进深睡(不返回); enterDeepSleep / 唤醒确认失败共用
void handlePowerBtn();                 // 电源键扫描: 单击=返回(待机态=开机) 双击=关闭屏幕 长按=关机
// ---- 供电与静音(主循环上下文执行) ----
void uiNotice(const String& msg, uint32_t ms);   // 主界面第2行临时提示
void muteAmpFor(uint32_t ms);             // 临时静音功放(供电通断的瞬态不进扬声器)
static void setNetDacPower(bool on);      // 网络侧 TDA1387-A 供电开关(-1 = 未接则自动跳过)

// ============ 复位原因(中文) ============
// 判断"重启"是软件崩溃、看门狗、还是供电不足的第一步 —— 三类问题修法完全不同。
static const char* resetReasonText(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:    return "上电/EN键复位 (正常)";
    case ESP_RST_EXT:        return "外部复位引脚";
    case ESP_RST_SW:         return "软件重启 ESP.restart()/OTA (正常)";
    case ESP_RST_PANIC:      return "★程序崩溃 异常/断言失败 -> 看上一行 Guru Meditation 回溯";
    case ESP_RST_INT_WDT:    return "★中断看门狗 有代码长时间关中断(常见于 Flash 写)";
    case ESP_RST_TASK_WDT:   return "★任务看门狗 某任务霸占CPU久不让出";
    case ESP_RST_WDT:        return "★看门狗(其它)";
    case ESP_RST_DEEPSLEEP:  return "深度睡眠唤醒";
    case ESP_RST_BROWNOUT:   return "★供电跌落 Brownout 电流拉低电压(功放/USB供电), 非软件问题";
    case ESP_RST_SDIO:       return "SDIO 复位";
    case ESP_RST_USB:        return "USB 复位";
    case ESP_RST_JTAG:       return "JTAG 复位";
    case ESP_RST_EFUSE:      return "eFuse 错误";
    case ESP_RST_PWR_GLITCH: return "★电源毛刺 Power glitch";
    case ESP_RST_CPU_LOCKUP: return "★CPU 死锁 双核都卡住";
    default:                 return "未知";
  }
}

// ============ 崩溃面包屑 ============
// 存在 RTC 不掉电内存: 软复位/看门狗复位后仍保留, 只有彻底断电才丢。
// 崩溃时串口往往来不及看, 靠它能在下次启动时知道"死在哪一步"。
RTC_NOINIT_ATTR static uint32_t gStage;        // 不初始化: 该段不受加载器管理, 初值无意义
RTC_NOINIT_ATTR static uint32_t gStageMagic;
#define STAGE_MAGIC 0x5A5A2026u

const char* stageText(uint32_t s) {
  switch (s) {
    case 1:  return "1 setup 开始";
    case 2:  return "2 存储(LittleFS)已挂载";
    case 3:  return "3 屏幕/按键已就绪";
    case 4:  return "4 音频已初始化(setPinout/setBufsize)";
    case 5:  return "5 WiFi/Web 服务已就绪";
    case 6:  return "6 主循环运行中(空闲/播放)";
    case 11: return "11 正在解析电台地址(resolveStreamUrl)";
    case 12: return "12 正在连流(audio.connecttohost)";
    case 13: return "13 正在停止播放(audio.stopSong)";
    case 14: return "14 正在 I2S 蜂鸣自检(beep)";
    case 21: return "21 正在写 LittleFS(stations.json 落盘/导入)";
    default: return "未知";
  }
}

// 供 audio_player.cpp / station_store.cpp 标记"即将进入的危险操作"
void markStage(uint32_t s) { gStage = s; }
uint32_t lastStage() { return gStage; }   // 供 Web 诊断页读取(设备不接串口也能看状态)

// ============ 启动看门狗: 卡在"开机中"时自动重启 ============
// 用户实测过: 关机(深睡)再开机后卡在"开机中"界面, 只能拔电。为避免必须人工断电,
// 这里用 esp_timer 做一个启动兜底 —— 选 esp_timer 而不是任务看门狗, 是因为它的回调跑在
// 独立的 esp_timer 任务上下文, 即使主任务(loopTask)被 setup 里的某个阻塞等待彻底卡死,
// 回调依然会执行, 从而能自己重启救回来。
// 只在启动阶段生效: 一进主循环就停表(否则正常运行也会被误重启)。
static esp_timer_handle_t gBootWdt = nullptr;
static volatile bool      gBootOk  = false;

static void bootWdtCb(void*) {
  if (gBootOk) return;
  Serial.printf("[Boot] ★启动 %llus 仍未进入主循环 -> 自动重启(上次停在阶段 %u: %s)\n",
                (unsigned long long)BOOT_WDT_MS / 1000ULL,
                (unsigned)gStage, stageText(gStage));
  delay(50);            // 给 USB 通道一点发送窗口(通道本身已是 0 超时, 不会卡住)
  esp_restart();
}

static void bootWdtStart() {
  esp_timer_create_args_t args = {};     // ★IDF 结构体必须整体清零后再赋值
  args.callback = bootWdtCb;
  args.name     = "bootwdt";
  if (esp_timer_create(&args, &gBootWdt) != ESP_OK) {
    Serial.println("[Boot] 启动看门狗创建失败(不影响正常运行)");
    gBootWdt = nullptr;
    return;
  }
  esp_timer_start_once(gBootWdt, (uint64_t)BOOT_WDT_MS * 1000ULL);
}

static void bootWdtMarkOk() {
  if (gBootOk) return;
  gBootOk = true;
  if (gBootWdt) { esp_timer_stop(gBootWdt); esp_timer_delete(gBootWdt); gBootWdt = nullptr; }
}

// ============ 失速看门狗(主循环 + 音频泵): 让"必须拔电"从此绝迹 ============
// 背景: 音频库把"拉流 + 解码 + 写 I2S"交给音频泵任务执行(见 config.h 的「音频泵任务」段),
//   其中任何一步被永久卡住 —— I2S DMA 停摆、网络库内部长等待、文件系统长操作 —— 整机就僵住:
//   屏幕停在最后一帧(可能是"开机中", 也可能是"播放中"), 按键/Web 全无响应, 用户只能断电。
//   这不是能靠"改一行代码"消除的风险(它发生在库内部), 但可以让设备【自己重启自救】。
// 做法: 一个独立小任务(prio 2 > 主循环的 1)同时盯【两个】心跳 ——
//   · gLoopTick       —— 主循环(UI/按键/Web/落盘)是否还在转; 停 LOOP_WDT_STUCK_S 秒即重启;
//   · player.pumpTick —— 音频泵是否还在喂流(方案B 新增)。★必须单独盯: 泵任务若卡在库内部,
//                        主循环照样活得好好的 —— 表现为"界面正常但彻底没声", 光靠主循环心跳
//                        永远发现不了, 用户只能人工断电。
//                        其阈值取得更长(AUDIO_WDT_STUCK_S), 因为换台时泵会阻塞在 audioMutex 上等
//                        connecttohost 完成(可能数秒)。
// 两个阈值都比任何合法长阻塞更长(HLS 分片边界、WiFi 建连 10s、文件系统格式化数秒),
// 所以只会对真正的永久僵死生效。
#define LOOP_WDT_STUCK_S 15
static volatile uint32_t gLoopTick = 0;

static void loopWatchdogTask(void*) {
  uint32_t lastLoop = 0, lastPump = 0;
  int      stuckLoop = 0, stuckPump = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    // ① 主循环心跳
    if (gLoopTick == lastLoop) {
      if (++stuckLoop >= LOOP_WDT_STUCK_S) {
        Serial.printf("[WDG] ★主循环已停 %ds(最后阶段 %u: %s) -> 自动重启自救\n",
                      stuckLoop, (unsigned)gStage, stageText(gStage));
        delay(30);
        esp_restart();
      }
    } else {
      stuckLoop = 0;
      lastLoop  = gLoopTick;
    }
    // ② 音频泵心跳(方案B): 泵停 = 没声。它的阈值更长, 以免换台时误判
    // ★闸门: 只有泵任务【确实在喂流】才监测(pumpFeeding = 开关打开 且 任务已创建)。
    //   否则: setup 早期(WiFi 建连可能耗时数十秒才轮到 player.begin())、泵任务创建失败、
    //   或用户用串口 P 把喂流切回主循环时, pumpTick 会不推进 -> 被误判成"泵停"而反复重启,
    //   甚至把"创建失败"这个真正原因掩盖成重启死循环。
    if (!player.pumpFeeding()) {
      lastPump  = player.pumpTick;      // 跟随即可, 不计数
      stuckPump = 0;
    } else if (player.pumpTick == lastPump) {
      if (++stuckPump >= AUDIO_WDT_STUCK_S) {
        Serial.printf("[WDG] ★音频泵已停 %ds(不再喂流; 最后阶段 %u: %s) -> 自动重启自救\n",
                      stuckPump, (unsigned)gStage, stageText(gStage));
        delay(30);
        esp_restart();
      }
    } else {
      stuckPump = 0;
      lastPump  = player.pumpTick;
    }
  }
}

static void loopWdtStart() {
  xTaskCreatePinnedToCore(loopWatchdogTask, "loopwdt", 3072, nullptr, 2, nullptr, 1);
}

// ============================================================================
// 电台列表(快速选台) —— 2026-09-21 新增
// ----------------------------------------------------------------------------
// 进入: 长按中键 BTN_LIST_HOLD_MS(800ms) / 长按红外"播放键"(靠重复帧判定, 见 ir_learn.cpp)
// 列表内三个键的语义【整体切换】, 实体键与红外一一对应:
//   上一台键 / 红外 prev  = 光标上移(按下即动; 按住连续上移)
//   下一台键 / 红外 next  = 光标下移(同上)
//   中  键 / 红外 toggle  = 单击【确定】并立即播放选中的台; 长按 = 收起列表回播放界面
// 浏览期间【当前台照常播放】, 只有"确定"才真正换台 —— 这样翻列表不会打断正在听的内容。
// 列表不自动收起(规格要求), 所以"长按中键"是主要出口; 确定播放也会自动收起。
//
// ★为什么台名要【快照】而不是每帧现取: store.name(i) 每次都要拿递归锁 + 造一个新的 String,
//   7 行 × 3fps 白白几百次分配; 快照只在"进入列表"与"发现条数被后台改过"时重建一次。
// ★为什么用 std::vector<String> 而不是固定数组: 电台数由用户在后台决定(可能上百个), 开固定数组太冒险。
// ★列表模式【不写 gState】(除了 player 命令投递): 屏幕内容全部来自本块的状态, 因此不会与
//   播放状态机互相干扰 —— 这也是"浏览时不换台"能成立的原因。
// ============================================================================
static bool gListMode = false;              // 当前是否处于电台列表界面
static int  gListCursor = 0;                // 光标所在电台序号(0 基, 在"当前分类过滤后的列表"里)
static std::vector<String> gListNames;      // 全量台名快照(序号与 store 一一对应)
static std::vector<String> gListView;       // ★当前分类过滤后台名(真正喂给 showList 的数组)
static std::vector<int>     gListIdx;       // ★过滤后列表第 i 行 -> store 里的真实下标
static std::vector<String> gListGroups;     // 全部分类名(去重, 按 store 出现顺序)
static int  gListGroup = 0;                 // 当前分类在 gListGroups 的下标(0=第一个分类)

// 重建台名快照(进入列表时 / 发现条数变化时调用)
static void listBuildSnapshot() {
  gListNames.clear();
  const int n = store.count();
  if (n > 0) gListNames.reserve((size_t)n);
  for (int i = 0; i < n; i++) gListNames.push_back(store.name(i));
}

// 重建【当前分类】的过滤结果: gListNames 只留该分类的台, gListIdx 存它们在 store 的真实下标。
// ★为什么要过滤而不是全列+插分类标题: 128x64 去掉标题行只剩 2 行, 插标题会把列表撑到
//   几十屏才看得完一个分类; 而"只列当前分类"翻两下就到头, 128x64 上 2 行也够用。
static void listApplyGroup() {
  gListIdx.clear();
  gListView.clear();
  const int n = store.count();
  if (n <= 0) return;
  const String want = (gListGroup >= 0 && gListGroup < (int)gListGroups.size())
                    ? gListGroups[gListGroup] : String();
  gListIdx.reserve((size_t)n);
  for (int i = 0; i < n; i++) {
    if (want.isEmpty() || store.group(i) == want) {
      gListIdx.push_back(i);
      // ★2026-10-11 边界保护: gListNames(快照) 与 store.count() 可能因 Web 任务并发增删而短暂不等,
      //   直接 [i] 会越界读(vector operator[] 不做检查)。填空串而非跳过, 以保持与 gListIdx 一一对应。
      gListView.push_back(i < (int)gListNames.size() ? gListNames[i] : String());
    }
  }
  if (gListCursor >= (int)gListIdx.size()) gListCursor = (int)gListIdx.size() - 1;
  if (gListCursor < 0) gListCursor = 0;
}

// 扫描 store 收集全部分类(去重, 保持出现顺序); 当前分类若已不存在则回落到第一个
static void listBuildGroups() {
  gListGroups.clear();
  const int n = store.count();
  for (int i = 0; i < n; i++) {
    const String g = store.group(i);
    bool seen = false;
    for (int k = 0; k < (int)gListGroups.size(); k++)
      if (gListGroups[k] == g) { seen = true; break; }
    if (!seen) gListGroups.push_back(g);
  }
  if (gListGroup >= (int)gListGroups.size()) gListGroup = 0;
  if (gListGroup < 0) gListGroup = 0;
}

// 切换分类(d=+1 下一个 / -1 上一个, 到头回绕)。音量键在列表里 = 换分类。
static void listCycleGroup(int d) {
  if (gListGroups.size() <= 1) {          // 只有一个分类(或没有) -> 无从切换
    Serial.println("[LIST] 只有一个分类, 无需切换");
    return;
  }
  const int m = (int)gListGroups.size();
  gListGroup = ((gListGroup + d) % m + m) % m;   // 双侧回绕, 负数也安全
  listApplyGroup();                            // 新分类 -> 光标复位到 0(第 1 台)
  display.noteActivity();
  Serial.printf("[LIST] 切换分类 -> %s (该类 %d 台)\n",
                gListGroups[gListGroup].c_str(), (int)gListIdx.size());
}

// 统一的"退出列表"收尾: 置位 + 释放快照 + 声明画面未知(强制下帧重画主界面)
static void listRelease() {
  gListMode = false;
  gListNames.clear();
  gListNames.shrink_to_fit();
  gListView.clear();
  gListView.shrink_to_fit();
  gListIdx.clear();
  gListIdx.shrink_to_fit();
  display.invalidate();
}

void listEnter() {
  const int n = store.count();
  if (n <= 0) { Serial.println("[LIST] 电台表为空, 不进入列表"); return; }
  listBuildSnapshot();
  listBuildGroups();                          // 先收集分类
  listApplyGroup();                          // 再按当前分类过滤出可见台
  // 光标默认落在"正在播放的那台"(在当前分类内), 否则落在该分类第 1 台
  gListCursor = 0;
  const int cur = gState.index;
  for (int i = 0; i < (int)gListIdx.size(); i++)
    if (gListIdx[i] == cur) { gListCursor = i; break; }
  gListMode = true;
  display.invalidate();                       // 主界面 -> 列表, 强制重画
  Serial.printf("[LIST] 进入电台列表: 分类 %s, 该类 %d/%d 台, 光标 #%d\n",
                (gListGroup >= 0 && gListGroup < (int)gListGroups.size())
                  ? gListGroups[gListGroup].c_str() : "?",
                (int)gListIdx.size(), n, gListCursor + 1);
}

void listExit() {
  if (!gListMode) return;
  listRelease();
  Serial.println("[LIST] 收起列表(未换台)");
}

// 光标上下移动(首尾环绕)。d=+1 向下 / d=-1 向上。范围是【当前分类过滤后】的列表
void listMove(int d) {
  const int n = (int)gListIdx.size();
  if (n <= 0) return;
  gListCursor += d;
  if (gListCursor < 0)  gListCursor = n - 1;        // 从第 1 台再往上 -> 跳到最后一台
  if (gListCursor >= n) gListCursor = 0;
  display.noteActivity();                           // 有操作 -> 保持背光
}

// 确定: 播放光标所指的台并收起列表(这是列表里【唯一】真正换台的动作)
void listConfirm() {
  const int n = (int)gListIdx.size();
  if (n <= 0 || gListCursor < 0 || gListCursor >= n) { listExit(); return; }
  const int idx = gListIdx[gListCursor];             // ★映射回 store 真实下标(分类过滤后不能直接用 gListCursor)
  Serial.printf("[LIST] 确定播放 #%d %s\n", idx + 1, store.name(idx).c_str());
  listRelease();
  player.requestPlayIdx(idx);   // 只投递命令, 由主循环 processCmd 执行(与音频泵/异步任务的边界)
}

// ★2026-10-07 前移到此处: listToggle() 现在要读 gSdMode(判断长按该进哪个列表),
//   而 listToggle 定义在本函数之前 —— 变量必须先声明, 否则编译报"未声明"。
static bool gSdMode   = false;      // 当前是否处于 SD 文件列表界面
static int  gSdCursor = 0;          // 光标所在文件序号

// 主体开关: 按键与红外的"长按播放键"共用
void listToggle() {
  // ★2026-10-07 用户指定: 中键长按 = "进入电台列表 / SD 卡音乐列表" ——
  //   哪个音源就进哪个列表(在 SD 就进 SD 列表, 在电台就进电台列表)。
  //   两个列表互斥: 进 SD 列表时若电台列表开着, 先关掉电台列表, 避免两套界面叠加。
  if (gSdMode) {
    sdExit();
    return;
  }
  if (gListMode) { listExit(); return; }       // 已在电台列表 -> 长按=收起
  if (gState.sdActive) { sdEnter(); return; }  // 当前音源是 SD -> 长按进 SD 文件列表
  listEnter();                                 // 当前音源是电台 -> 进电台列表
}

// ===================== 本地 SD 卡浏览(阶段②) =====================
// 与电台列表【完全同构】的一套装模界面, 刻意不新造渲染路径:
//   · 复用 display.showList() 画文件列表(128x64 只有两行, 与电台列表一样的排版);
//     传 groups=nullptr/ng=0 即可不画顶部分类标签栏(该参数有 hasTabs 守卫, 安全)。
//   · 用独立的一套布尔/游标(gSdMode/gSdCursor), 与电台列表互不干扰, 可各自进出。
//
// ---- 进入方式(★为什么实体键用"中键双击")----
//   3 个实体键的所有手势在【电台列表模式】里已被占满(上下移/双击换分类/确定/长按退出),
//   而红外"播放键长按"也已给了电台列表(listToggle)。唯一空闲且不冲突的手势是
//   【中键双击】= 音源切换(SD⇄网络, 见 sdToggleSource); 【中键长按】= 进本列表(见 listToggle)。
// 另外两个入口: 串口 G / Web 后台"本地SD卡"页。
// 浏览期间【当前音源照常播放】(和电台列表一样), 只有"确定"才真正切到 SD 文件。
// ★gSdMode / gSdCursor 已前移到 listToggle() 之前声明(那里要用它们)。
void sdEnter() {
  if (!gSd.mounted() && !gSd.begin()) {
    uiNotice("SD 卡挂载失败, 见串口日志");
    return;
  }
  const int n = gSd.scan();         // 扫描在主循环上下文, 符合 sd_card.h 的线程约定
  if (n <= 0) {
    //★区分"卡真的空"与"有文件但没匹配上" —— scan() 的串口日志里有逐条明细,
    //   这里只需给用户一句能行动的提示(最常见: 文件被放进了子文件夹)。
    uiNotice(n == 0 ? "SD卡: 只扫根目录, 请把文件移到卡根目录(详情看串口日志)"
                    : "SD 卡已挂载但打不开目录, 请看串口日志");
    return;
  }
  // ★光标落在"上次播到的那首"(2026-10-07 记忆功能):
  //   优先当前正在播的(本来就在 SD 模式), 否则用 NVS 里记住的序号(已钳到有效范围)。
  //   之前这里是硬编码 0 —— 意味着每次切回 SD 都从第 1 首开始, 与"接着上次听"的直觉不符。
  if (gState.sdActive && gState.sdIndex >= 0) gSdCursor = gState.sdIndex;
  else                                          gSdCursor = gSd.resolveLast();
  if (gSdCursor >= n) gSdCursor = n - 1;
  if (gSdCursor < 0)  gSdCursor = 0;
  gSdMode = true;
  display.invalidate();             // 主界面 -> 列表, 强制重画
  Serial.printf("[SD  ] 进入文件列表: %d 个文件, 光标 #%d (上次播到第 %d 首)\n",
                n, gSdCursor + 1, gSd.resolveLast() + 1);
}

// ★切回 SD 音源时【直接接着上次那首播】(2026-10-07):
//   红外"音源切换"/后台从电台切到 SD 时用这个 —— 用户表达的意图是"我要听 SD 里那首歌",
//   让他再在列表里点一次"确定"是多余的一步。
//   与 sdEnter()(只开列表不播)分工: 切换键 = 直接播; 面板中键双击 = 只看列表。
void sdEnterAndResume() {
  if (gSdMode) return;               // 已经在列表里了(说明用户是在浏览), 不要抢
  sdEnter();                         // 挂载 + 扫描 + 定位光标(失败时内部会提示并返回)
  if (!gSdMode) return;              // 进入失败 -> 不要继续播
  const int n = gSd.count();
  if (n <= 0) return;
  gSdCursor = gSd.resolveLast();     // 再取一次(此时已 clamp 过)
  if (gSdCursor >= n) gSdCursor = n - 1;
  if (gSdCursor < 0)  gSdCursor = 0;
  gSdMode = false;                   // 不进列表界面, 直接播
  display.invalidate();
  const int i = gSdCursor;
  Serial.printf("[SD  ] 直接续播上次那首: #%d %s\n", i + 1, gSd.file(i)->name.c_str());
  player.playSdFile(i);              // 投递命令, 主循环 processCmd 执行
}

void sdExit() {
  if (!gSdMode) return;
  gSdMode = false;
  display.invalidate();
  Serial.println("[SD  ] 收起文件列表(未换文件)");
}

// 光标上下移动(首尾环绕)。语义与 listMove 完全一致。
void sdMove(int d) {
  const int n = gSd.count();
  if (n <= 0) return;
  gSdCursor += d;
  if (gSdCursor < 0)  gSdCursor = n - 1;
  if (gSdCursor >= n) gSdCursor = 0;
  display.noteActivity();
}

// 确定: 播放光标所指的文件并收起列表(与 listConfirm 同构, 只是把"换电台"换成"播本地文件")
void sdConfirm() {
  const int n = gSd.count();
  if (n <= 0 || gSdCursor < 0 || gSdCursor >= n) { sdExit(); return; }
  Serial.printf("[SD  ] 确定播放 #%d %s\n", gSdCursor + 1, gSd.file(gSdCursor)->name.c_str());
  gSdMode = false;
  display.invalidate();
  player.playSdFile(gSdCursor);   // 只投递命令, 由主循环 processCmd 执行
}

// 退出 SD 音源(回网络电台)。Web 后台/串口用。
void sdExitSource() {
  if (!gState.sdActive && !gSdMode) return;
  gSdMode = false;
  display.invalidate();
  player.exitSdMode();            // 投递命令: 停 SD + 回到进 SD 前的那台电台
}

// 主体开关: 中键双击(唯一空闲手势)。与实体键/红外无冲突, 详见上面的说明。
void sdListToggle() {
  if (gSdMode) sdExit();
  else         sdEnter();
}

// ★音源切换(遥控 IR_SD 键用): 网络电台 <-> 本地 SD 卡。
//   【只置标志, 不在这里做重活】—— 挂载卡 + 扫描目录要几百毫秒, 且 SD 驱动只在主循环
//   上下文被访问(见 sd_card.h)。真正的动作在 sdTick() 里做。
//   两个方向:
//     · 正在放 SD / 正看着 SD 列表 -> 退出 SD 音源, 回到进 SD 前的那台电台;
//     · 否则 -> 进 SD: 挂载 + 扫描 + 打开文件列表(光标落在正在播的那首)。
void sdToggleSource() {
  if (gState.sdActive || gSdMode) {
    gSdMode = false;                 // 顺手把 SD 列表也收掉, 免得留在屏幕上
    display.invalidate();
    sdReqExit();                     // 主循环会 stopSong + 回到电台
    Serial.println("[SD  ] 音源切回网络电台");
  //   ★2026-10-07: 切过去时【直接接着上次播的那首播】(sdEnterAndResume), 不必再在列表里点确定。
  } else {
    sdEnterAndResume();          // 等价于"切到 SD 并续播上次那首"
    Serial.println("[SD  ] 音源切到本地 SD 卡(续播上次那首)");
  }
}

// ===================== 定时关机(红外 IR_TIMER 键) =====================
// 单键循环: 关闭 -> 1h -> 2h -> ... -> 12h -> 关闭 -> ...
// ★为什么用【12 个固定档】而不是每按一次 +1 小时:
//   - 跳着选(比如从 12h 一下跳到"关闭")时, 逐小时要点很多次, 遥控体验很差;
//   - 12 档正好一屏/一排能数清, 用户心里有明确预期"按 N 下就是几小时"。
// 档位表用分钟表示, 与 gState.sleepMin 同单位。
static const int kTimerStepsMin[] = {0, 60, 120, 180, 240, 300, 360, 420, 480, 540, 600, 660, 720};
static const int kTimerStepsNum = (int)(sizeof(kTimerStepsMin) / sizeof(kTimerStepsMin[0]));

void timerCycle() {
  // 找到当前档位下标。sleepMin 若不在表内(例如后台设了 45 分钟), 就退化成"下一档 >= 45"。
  int cur = gState.sleepMin;
  int idx = 0;
  for (int i = 0; i < kTimerStepsNum; i++) {
    if (kTimerStepsMin[i] == cur) { idx = i; break; }
    if (kTimerStepsMin[i] > cur) { idx = i; break; }   // 表里没有该值 -> 取第一个更大的
  }
  const int next = (idx + 1) % kTimerStepsNum;         // 末档后回"关闭"

  if (kTimerStepsMin[next] == 0) {
    player.setSleepTimerAt(0, true);  // 取消(执行器里会把 sleepToPowerOff 复位)
    uiNotice("定时关机: 已关闭");
    Serial.println("[SLEEP] 定时关机: 已关闭(循环回到起点)");
    return;
  }
  const int h = kTimerStepsMin[next] / 60;
  player.setSleepTimerAt(kTimerStepsMin[next], true);
  uiNotice(String("定时关机: ") + h + " 小时后关机");
  Serial.printf("[SLEEP] 定时关机 -> %d 小时后关机 (第 %d/%d 档)\n",
                h, next + 1, kTimerStepsNum - 1);
}

// ---- Web 后台桥接(阶段②): 给 /api/sd/* 用 ----
// ★线程模型(与 bgTick / gStoreChanged 同一手法): 异步任务【绝不直接碰 SD 卡】, 只投递请求,
//   真正的挂载/扫描/序列化都在主循环里做。
static volatile bool     gSdReqScan = false;   // 异步任务置位 -> 主循环挂载+扫描+重建 JSON
static volatile bool     gSdReqEnter = false;  // 异步任务置位 -> 主循环进入 SD 文件列表界面
static volatile int8_t   gSdReqExit = -1;      // 0 = 请求退出 SD 音源(回电台)
static String            gSdJson;              // 主循环构建的文件列表 JSON(Web 只读)
static SemaphoreHandle_t gSdJsonMutex = nullptr;

// 主循环调用: 消费 Web 的 SD 请求。放在主循环是为了让 SD 卡驱动只在一个上下文里被访问。
void sdTick() {
  if (!gSdJsonMutex) gSdJsonMutex = xSemaphoreCreateMutex();

  if (gSdReqExit == 0) {
    gSdReqExit = -1;
    if (gState.sdActive || gSdMode) {
      gSdMode = false;
      display.invalidate();
      player.exitSdMode();        // 投递命令: 停 SD + 回进 SD 前的那台电台
    }
  }

  // 请求"进入 SD"(遥控/后台的音源切换): 走与红外"音源切换"完全相同的路径
  // —— 挂载+扫描+【直接续播上次那首】(不是只开列表)。
  if (gSdReqEnter) {
    gSdReqEnter = false;
    if (!gSdMode && !gState.sdActive) sdEnterAndResume();
  }

  if (gSdReqScan) {
    gSdReqScan = false;
    const bool okMount = gSd.mounted() || gSd.begin();
    const int  n = okMount ? gSd.scan() : -1;
    // ★在主循环里序列化(Web 异步任务只读结果字符串), 避免它直接读 gSd 的 vector。
    String j;
    j = "{\"ok\":";        j += okMount ? "true" : "false";
    j += ",\"mounted\":";  j += okMount ? "true" : "false";
    j += ",\"count\":";    j += String(n < 0 ? 0 : n);
    j += ",\"playing\":";  j += gState.sdActive ? "true" : "false";
    j += ",\"cur\":";      j += String(gState.sdIndex);
    j += ",\"name\":\"";    j += gState.sdName;
    j += "\",\"files\":[";
    for (int i = 0; i < (int)(okMount ? n : 0); i++) {
      if (i) j += ",";
      const SdAudioFile* f = gSd.file(i);
      j += "{\"i\":";  j += String(i);
      j += ",\"n\":\""; j += f->name;
      // ★文件大小用 fs::File::size() 现开现关: core 3.3.11 的 SDMMCFS 【没有】fileSize()。
      //   这里是主循环上下文(SD 驱动只在本上下文被访问), 逐个 open/size/close 是安全的。
      j += "\",\"sz\":";
      File fp = SD_MMC.open(f->path);
      j += String(fp ? fp.size() : 0);
      if (fp) fp.close();
      j += "}";
    }
    j += "]}";
    xSemaphoreTake(gSdJsonMutex, portMAX_DELAY);
    gSdJson = j;
    xSemaphoreGive(gSdJsonMutex);
    Serial.printf("[SD  ] Web 列表已重建: %d 个文件\n", n);
  }
}

// Web 异步任务读文件列表快照(零拷贝: 锁内 send_P, 同 /api/state 的做法)。
bool sdGetJson(String& out) {
  if (!gSdJsonMutex) return false;
  xSemaphoreTake(gSdJsonMutex, portMAX_DELAY);
  out = gSdJson;               // ★拷进调用方缓冲(同 buildStateInto 的约定), 避免悬空指针
  xSemaphoreGive(gSdJsonMutex);
  return true;
}

// Web 请求扫描(异步任务可调: 只置标志, 不碰 SD 卡)
void sdReqScan() { gSdReqScan = true; }
void sdReqExit() { gSdReqExit = 0; }
// ★Web 请求"进入 SD"(异步任务可调)。
//   与"扫描"分开: 扫描只要刷新文件清单给网页看, 而"进入"还要把屏幕切到 SD 文件列表
//   (含光标定位 + invalidate 重画), 语义不同, 故用独立标志。
void sdReqEnter() { gSdReqEnter = true; }

void setup() {
  Serial.begin(115200);
  // ★启动后第一条输出: 只要这一行能在打开监视器后出现, 就证明串口通道(原生USB/UART0 双通道)真正通了。
  // 若连它都看不到, 说明监视器接的口不对(应接"原生USB烧录口"), 或固件根本没跑起来。
  Serial.println("[Boot] === 固件已启动, 串口通道就绪 (若看不到此行=监视器接错口/波特率非115200) ===");
  // ★★★ 启动分区自检 (2026-10-16, 排查"固件刷了却一点效果都没有") ★★★
  //   本机是【双 app 分区】: ota_0@0x10000 / ota_1@0x3F0000, 出厂走"整片线刷(0x0 起一个 bin)"。
  //   引导器到底启动哪一块, 由 otadata(0xE000, 来自 Arduino 的 boot_app0.bin)决定 ——
  //   ★Arduino 自带的 boot_app0.bin 里 ota_seq=1 ⇒ 指向 ota_1! 而整片镜像的 app 段写在 ota_0。
  //   一旦设备以前做过一次"网页 OTA"(把旧固件写进 ota_1), 之后每次线刷都启动 ota_1 里那份【旧固件】,
  //   现象就是"改了半天代码, 屏幕上一点变化都没有" —— 这一行直接把它暴露出来。
  //   (根治办法: tools/make_full_bin.py 已把同一份 app 同时写进 ota_0 与 ota_1, 选哪块都是新固件。)
  {
    const esp_partition_t* rp = esp_ota_get_running_partition();
    esp_ota_img_states_t st; bool hasSt = (esp_ota_get_state_partition(rp, &st) == ESP_OK);
    Serial.printf("[Boot] 固件版本=%s | 运行分区=%s @0x%06X | otadata状态=%s\n",
                  FW_VERSION, rp ? rp->label : "?", rp ? (unsigned)rp->address : 0,
                  !hasSt ? "无(未走OTA)" : (st == ESP_OTA_IMG_VALID ? "VALID"
                        : st == ESP_OTA_IMG_NEW ? "NEW(待确认)"
                        : st == ESP_OTA_IMG_PENDING_VERIFY ? "PENDING_VERIFY" : "其它"));
    // ★启动成功即"自我确认": 本工程 CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y,
    //   若新镜像以 NEW/PENDING_VERIFY 状态启动却不自我确认, 下一次复位会被引导器判为"启动失败"
    //   而【自动切回另一块分区】—— 表现为"重启一次就变回旧固件/旧界面"。
    //   这里显式确认(只有 PENDING/NEW 状态时才真正写 otadata, 已 VALID 时是空操作, 无磨损)。
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
      Serial.println("[Boot] 已确认本分区 VALID (不再自动回退)");
  }
  bootWdtStart();          // 启动兜底: BOOT_WDT_MS 内没进主循环就自动重启(防"卡开机中"必须拔电)
  // 【启动期最优先】红外 + 深睡唤醒确认: RMT 武装得越早, 越可能抓到"唤醒这次"的遥控帧。
  // 深睡唤醒 -> setup 之间有 ROM/加载时延(数百 ms), 而一次遥控帧仅约 110ms, 可能已结束;
  // 故把 ir_init 提到最前 + 确认窗口给足(IR_WAKE_CONFIRM_MS), 用户再按一下即可命中。
  // 仅"红外唤醒(EXT1)"才需确认; 电源键唤醒(EXT0)与上电/复位直接放行。
  ir_init();
  {
    esp_sleep_wakeup_cause_t wc = esp_sleep_get_wakeup_cause();
    if (wc == ESP_SLEEP_WAKEUP_EXT1 && !ir_confirm_power(IR_WAKE_CONFIRM_MS)) {
      Serial.println("[PWR] 红外唤醒但非 power 码 -> 回深睡(该键不开机)");
      goToSleep();             // 仅配置唤醒源 + 深睡(不返回)
    }
    else if (wc == ESP_SLEEP_WAKEUP_EXT0) {
      // ★双段唤醒: 电源键唤醒后, 量按住的时长 —— 短按(<POWER_LONG_MS)不开机直接回深睡,
      //   长按(>=阈值, 用户仍按住)才真正开机。等价"只有长按才开机", 防误触。
      pinMode(POWER_BTN_PIN, INPUT_PULLUP);
      uint32_t t0 = millis();
      bool longEnough = false;
      while (digitalRead(POWER_BTN_PIN) == LOW) {
        if (millis() - t0 >= POWER_LONG_MS) longEnough = true;
        delay(20);
      }
      if (!longEnough) {
        Serial.println("[PWR] 深睡唤醒: 短按(未达长按阈值) -> 回深睡");
        goToSleep();
      }
      Serial.println("[PWR] 深睡唤醒: 长按达到阈值 -> 正常开机");
    }
  }
  // 日志通道: 双通道同时输出(UART0=GPIO43/44 / 原生USB=USB-Serial-JTAG), 写超时 0 不阻塞主循环。
  // 排障时两个 USB 口都能看到日志, 不必猜插哪个口。
  Serial.printf("[Main] 日志通道: UART0(GPIO43/44)%s | 写超时=0(绝不阻塞主循环)\n",
#if WR_HAS_USB_CDC
                " + 原生USB(USB-Serial-JTAG)"
#else
                ""
#endif
                );
  // 注: 旧版(3.0.13/faad2)曾在此预占 AAC 缓冲池以对抗堆碎片化; 现用 2.0.6(Helix)一次性
  // 申请 28.8KB 后全程零分配, 不需要也不该再预占 —— 故该池机制已整体删除。
  // 复位原因 = 死机类型第一判据:
  //   "TG1WDT_SYS_RESET"/"Task watchdog" -> 某段代码长时间不让出 CPU(忙等)
  //   "Brownout reset"                   -> 供电不足(与软件无关)
  //   "Software reset CPU"/"Panic"       -> 崩溃(LoadProhibited/Stack overflow 等)
  Serial.println(String("[Main] 复位原因: ") + resetReasonText(esp_reset_reason()) +
                 " (code=" + String((int)esp_reset_reason()) + ")");
  // 崩溃面包屑: 仅当上次异常结束(RTC 内存里 magic 仍在)才有意义
  if (gStageMagic == STAGE_MAGIC) {
    Serial.println(String("[Main] ★上次运行最后阶段: ") + stageText(gStage) +
                   " (stage=" + String(gStage) + ")");
  } else {
    Serial.println("[Main] 上电冷启动(无历史阶段)");
  }
  gStageMagic = STAGE_MAGIC;   // 之后若发生软复位, 下次启动即可读出阶段
  // 唤醒源: 排查"关机后开不了机"的第一手信息 —— 区分"深睡被按醒"与"上电/复位"
  {
    esp_sleep_wakeup_cause_t wc = esp_sleep_get_wakeup_cause();
    const char* wcTxt = (wc == ESP_SLEEP_WAKEUP_EXT0) ? "深睡唤醒 EXT0(电源键)"
                      : (wc == ESP_SLEEP_WAKEUP_EXT1) ? "深睡唤醒 EXT1(红外)"
                      : (wc == ESP_SLEEP_WAKEUP_UNDEFINED) ? "非深睡唤醒(上电/复位/烧录)"
                      : "其它唤醒源";
    Serial.printf("[Main] 本次启动: %s (cause=%d)\n", wcTxt, (int)wc);
  }
  markStage(1);
  display.begin();
  display.showMessage("   大猫收音机", "    启动中...");  //开机屏幕显示内容

  btn.begin();
  markStage(3);                        // 屏幕/按键就绪

  // 功放使能脚(PA_ENABLE): 输出, 初始"静音"(低=关断)。真正出声后由 updateAmp() 拉高,
  // 停止/待机/连接中自动拉低; 关机时由 enterDeepSleep() 拉低。
  pinMode(AMP_EN_PIN, OUTPUT);
  digitalWrite(AMP_EN_PIN, AMP_EN_ACTIVE == HIGH ? LOW : HIGH);
  // 独立电源键(开关机): 外接瞬时按键, INPUT_PULLUP(按下=低电平)。深度休眠中按下可唤醒。
  pinMode(POWER_BTN_PIN, INPUT_PULLUP);
  // 电池电压检测: ADC 输入, 不要上拉/下拉(否则污染 100k 分压)。中点 = Vbat/2, 接 BATT_ADC_PIN。
  pinMode(BATT_ADC_PIN, INPUT);
  analogSetPinAttenuation(BATT_ADC_PIN, ADC_11db);   // 满量程 ~3.3V, 覆盖中点最高 2.1V(1S 锂电)
  // 充电状态检测: 接 TP4056 类 CHRG 脚(漏极开路, 充电中拉低)。INPUT_PULLUP 已含上拉, 无需外置电阻。
  pinMode(CHG_STAT_PIN, INPUT_PULLUP);
  // 网络授时(时钟/日期/星期/农历都靠它): 先按编译期时间兜底, 再起 SNTP 等 WiFi 就绪后自动校准。
  //   放在这里而不是等 WiFi 连上: 断网/AP 配网时屏上也有个(不准的)时间可看。
  timeInit();
  weatherInit();
  if (!store.begin()) {
    Serial.println("[Main] 存储挂载失败! 电台/音量配置将不持久(重启即丢)");
  }
  // 屏幕背景图(Web 后台上传的 1bpp 位图): ★必须在这里读 ——
  // display.begin() 发生在 store.begin() 之前, 那时 LittleFS 还没挂载, 读不到。
  // 没有背景图时静默返回, 显示路径与原固件逐字节相同。
  bgLoad();
  markStage(2);                        // SPIFFS 挂载 + 配置加载完成
  gState.volume = store.getVolume();   // 加载已记忆音量(默认12)
  // 读回"上次播到 SD 卡第几首"(2026-10-07)。只读 NVS, 不在这里挂载卡——
  // 开机就把SD 挂上会拖慢启动, 而且没插卡时只会刷一串失败日志。
  // 真正的挂载+定位发生在切到 SD 音源时(sdEnter/sdEnterAndResume)。
  gSd.loadLast();
  // 定时开机(闹钟)配置: 同样是"读 NVS", 和上面几项聚在一起。只载入不触发,
  // 真正的判定在主循环(见 loop() 里的闹钟块)。
  alarmBegin();
  player.begin();                      // 用加载的音量初始化音频
  // 网络侧 TDA1387-A 的供电脚: 初始化并置于"断电", 随后统一打开(仅网络收音机, 网络侧 DAC 常通电)
  if (NET_DAC_PWR_PIN >= 0) {
    pinMode(NET_DAC_PWR_PIN, OUTPUT);
    digitalWrite(NET_DAC_PWR_PIN, NET_DAC_PWR_ON_LEVEL == HIGH ? LOW : HIGH);
  }
  setNetDacPower(true);   // 仅网络音源: 网络侧 DAC 通电
  player.attachStore(&store);
  markStage(4);                        // 音频(I2S)初始化完成

  portal.begin();                 // 连接WiFi 或 启动AP配网
  web.begin(&store, &player, &portal, &display);
  // ★"有遥控活动"回调(2026-10-08): 收到任何"像样的红外帧"就点亮背光, 与"这个键有没有学过/
  //   这次有没有命中"无关。屏熄灭(睡眠待机 / 电源键双击关屏)后, 用户按遥控上任意一个键
  //   —— 包括从没学过的键、包括正好撞上动作冷却被丢弃的那次 —— 屏都该亮。
  //   ★这里只做"亮屏"这一件事: 不碰 gState.sleepStandby(那是下面 action 回调的职责,
  //     且 IR_POWER 需要读"按之前"的值来判断是"开机"还是"关机", 提前清掉会判错)。
  ir_set_activity_cb([]() { display.noteActivity(); });
  // 命中某动作 -> 触发对应播放控制(主循环上下文调用, 直接调 player 安全)
  // ★电台列表模式下前三个动作的语义整体切换(与实体键一一对应, 见文件上方"电台列表"块)
  ir_set_action_cb([](IRAction a) {
    display.noteActivity();          // 任意遥控键 -> 点亮背光 + 重置熄灭计时(按键已有, 这里补齐遥控)
    // ★2026-10-08: 先把"按之前是否处于熄屏待机"取走, 再清标志。
    //   顺序不能反 —— IR_POWER 分支要靠 wasStandby 决定是"开机"还是"关机"(见下)。
    //   "清"这个动作本身表示: 任何一次有效的遥控动作 = 用户已把设备叫醒。
    const bool wasStandby = gState.sleepStandby;
    (void)wasStandby;   // STANDBY_POWER_OPENS=0 时下面的分支不读它, 加这行避免"未使用变量"警告
    gState.sleepStandby = false;
    if (gListMode) {
      switch (a) {
        case IR_PREV:   listMove(-1);   break;   // 列表里 = 光标上移
        case IR_NEXT:   listMove(+1);   break;   // 列表里 = 光标下移
        case IR_TOGGLE: listConfirm();  break;   // 列表里"播放键" = 确定并播放
        // ★音量键在列表里 = 切换分类(浏览时最常做的事就是"换一类看看", 不必退出列表)
        case IR_VOLUP:   listCycleGroup(+1); break;
        case IR_VOLDOWN: listCycleGroup(-1); break;
        default: break;                          // 其余键在列表里不生效
      }
      return;
    }
    // ★SD 文件列表模式(阶段②): 与电台列表同一套语义 —— 上下移 / 播放键=确定 / 音量键换分类没有,
    //   所以音量键在这里不做"换类"(SD 是平铺列表, 本来就没有分类), 留空即可。
    if (gSdMode) {
      switch (a) {
        case IR_PREV:   sdMove(-1);   break;   // 列表里 = 光标上移
        case IR_NEXT:   sdMove(+1);   break;   // 列表里 = 光标下移
        case IR_TOGGLE: sdConfirm();  break;   // 列表里"播放键" = 确定并播放该文件
        default: break;
      }
      return;
    }
    switch (a) {
      case IR_PREV:    player.prev();        break;
      case IR_NEXT:    player.next();        break;
      case IR_TOGGLE:  player.togglePlay();  break;
      case IR_VOLUP:   player.setVolume(gState.volume + 1); break;
      case IR_VOLDOWN: player.setVolume(gState.volume - 1); break;
      // ★遥控"电源"键(2026-10-08 修正): 语义取决于"按之前设备是不是熄屏待机态"。
      //   · 熄屏待机(睡眠定时器到点, 已停播+熄屏但仍在通电) -> 【开机】: 亮屏 + 恢复播放。
      //     旧实现无条件 enterDeepSleep(), 于是"定时睡眠后用遥控开机"反而把设备关进深睡
      //     (黑到底), 再从深睡唤醒还要在 IR_WAKE_CONFIRM_MS 窗口内【再按一次】 ——
      //     用户看到的就是"遥控开机有问题"。这是本次要修的核心。
      //   · 其它情况(正常播放中 / 暂停中 / 双击关屏但还在播) -> 照旧【关机】进深睡, 语义不变。
      case IR_POWER:
#if STANDBY_POWER_OPENS
        if (wasStandby) {
          Serial.println("[IR] 电源键: 熄屏待机中 -> 开机(亮屏 + 恢复播放)");
          if (!gState.playing) player.togglePlay();   // 续播当前台(睡眠到点时已把台名/序号留住)
        } else
#endif
        {
          enterDeepSleep();     // 遥控"关机"键 -> 进深度休眠
        }
        break;
      case IR_ROTATE:      display.rotateStep(); break;  // 屏幕旋转: 每按一次顺时针 90°(0->90->180->270->0)
      case IR_SCREEN:      display.screenStep(); break;  // 切换屏幕: ST7567->SSD1306->ST7789->ILI9341 循环(存 NVS, 2~4s 后生效)
      // ★音源切换(阶段②): 网络电台 <-> 本地 SD 卡。两个方向都是"切到另一个"，
      //   所以这里不用判断当前是哪个源 —— sdExitSource() 内部自己判:
      //     · 正在放 SD(或正看着 SD 列表) -> 退出 SD, 回到进 SD 前的那台电台;
      //     · 否则 -> 进入 SD: 挂载 + 扫描 + 打开文件列表。
      //   两个函数都只是【置标志/投递命令】, 重活留给主循环, 与其它遥控键同一手法。
      case IR_SD:      sdToggleSource();    break;
      // ★定时关机(阶段③配套): 每按一次在 关闭/1h/…/12h 之间循环一档。
      //   到点会【真正关机】(停播+关无线+进深睡), 只剩电源键/红外开机键能唤醒 ——
      //   与"播放键"的播放/暂停是两回事, 所以放在主界面(列表里不响应, 见上面的分支)。
      case IR_TIMER:  timerCycle();       break;
      default: break;
    }
  });
  // "长按"(由重复帧判定, 见 ir_learn.cpp): 播放键长按 = 进入 / 退出电台列表。
  // 关机键长按不做特殊处理 —— 它的单击语义就是关机, 长按仍按单击即关机(防呆, 免得长按没反应)。
  ir_set_hold_cb([](IRAction a) {
    display.noteActivity();
    if (a == IR_TOGGLE) listToggle();
  });
  // 允许"重复帧连续重发"的动作: 仅【列表模式下】的 prev/next —— 实现按住连续上下选择。
  // 非列表模式返回 false -> 按住 prev/next 不会连续跳台(保持原有"一步一台"的手感, 也不会误跳一串台)。
  ir_set_repeat_cb([](IRAction a) {
    return gListMode && (a == IR_PREV || a == IR_NEXT);
  });
  markStage(5);                        // 网络/Web 就绪

  // 不在 setup 当场续播: 此时 WiFi 可能还没连上(异步/慢网),
  // 交由 loop() 在 "WiFi 真正连上" 时触发, 保证开机即续播上次电台
  gAutoPlayPending = true;
  Serial.println("[Main] 启动完成");
  Serial.println("[Main] 按键: 左=上一台(双击音量-/长按连续减) | 右=下一台(双击音量+/长按连续加) | 中=播放/停止(长按800ms=进出电台列表)");
  Serial.println("[Main] 列表内: 左/右=上下选择(按住连续) | 中单击=确定并播放 | 中长按=收起列表 ｜ 红外同义(长按播放键进出)");
  Serial.println("[Main] 注意: 中键长按已不再是\"重置配网\" —— 重置配网只在 Web 后台(WS reset / POST /api/reset)");
  Serial.println("[Main] 串口诊断命令: T=I2S蜂鸣自检(无声) | M=切换节目信息连接(卡顿) | D=打印诊断 | F=字库自检 | S=屏幕测试图案 | L=翻转背光 | B=起播预缓冲开关 | R=恢复出厂电台表(清残留+播第1台) | W=原始抓流测试(区分网络/库) | I=红外学习菜单 | U=睡眠定时器(30分) u=取消 | Z=关机(深睡: 按电源键/已学 power 遥控键唤醒, 其它遥控键不唤醒)");
  Serial.printf("[Main] 节目信息连接: %s (发 M 可切换; 缓冲持续欠载会自动降级关闭)\n",
                player.metaEnabled ? "开" : "关");
}

// ---- 分阶段耗时探针(段号含义见 player_state.h 的 PH_* 枚举) ----
// 用法: PH(表达式, 段号) —— 把该段【本周期的最大耗时】记进 g_phMaxUs[], 由 printDiag() 打印并清零。
// 存在意义: 只测"主循环整体卡了 116ms"无法定位; 分段后能一眼看出是 Flash 落盘、屏幕全刷,
// 还是解码/切台本身 —— 这三种的修法完全不同。
#define PH(block, idx) do { const uint32_t _phT0 = micros(); block; \
                            const uint32_t _phD  = micros() - _phT0; \
                            if (_phD > g_phMaxUs[idx]) g_phMaxUs[idx] = _phD; } while (0)

void loop() {
  // 面包屑: 首次进入主循环标为"运行中"; 崩溃后可读到最后阶段(见 markStage)定位死因。
  static bool s_firstLoop = true;
  if (s_firstLoop) {
    s_firstLoop = false;
    markStage(6);
    bootWdtMarkOk();      // 已进主循环 -> 撤掉启动兜底
    loopWdtStart();       // 起主循环失速看门狗(卡死 15s 自动重启自救, 免得用户拔电)
  }
  gLoopTick++;            // 心跳: 供上面的失速看门狗判断主循环是否还在转

  // 关机(深度休眠)待执行: 由主循环统一在这里安全地写 SPIFFS + 深睡
  // (Web 异步回调只置 gShutdownPending 标志, 绝不在其上下文直写 SPIFFS/深睡)
  if (gShutdownPending) {
    gShutdownPending = false;
    enterDeepSleep();   // 内部已先做 store.saveNow(), 不返回
  }

  // 主循环侧音频管理: 曲名落地 + 取换台命令(processCmd) + ICY 元数据 + 健康监测 + 静音恢复。
  // ★喂流(audio.loop)已由【音频泵任务】独占执行, 不在这里 —— 这就是方案B 的全部意义:
  //   下面那些重活(显示整屏刷新 56~63ms / 落盘 / 网页请求)再怎么耗时, 都推迟不了喂流。
  PH(player.loop(), PH_PLAYER);
  // 睡眠定时器到点: 音频模块置 sleepJustFired, 这里关屏(显示层不在音频模块内)。
  // 任意按键经 display.noteActivity() 仍可重新点亮(睡眠=熄屏待机, 非断电)。
  if (gState.sleepJustFired) {
    gState.sleepJustFired = false;
    // ★同时标记"熄屏待机态"(2026-10-08): 设备已停播+熄屏但仍在通电, 此时【电源键语义反转】
    //   —— 单击/遥控电源键 = 开机(亮屏 + 恢复播放), 而不是继续往下关一层。
    //   详见 player_state.h 的 sleepStandby 字段与下面的消费处。
    gState.sleepStandby = true;
    display.screenOff();
  }
  // ★"熄屏待机"期间【重新设定睡眠定时器】-> 自动亮屏并退出待机(2026-10-08)。
  //   理由: 会去设睡眠时间的人必然就在设备旁, 屏不该继续黑着; 而且这是他"要用设备"的明确信号。
  //   放在这里(而不是各设定入口)是因为设定路径有三条(面板 U 键 / Web 后台 / 红外 IR_TIMER),
  //   统一在主循环判一次, 免得三处各写一遍还容易漏。
  //   ★判据必须带 sleepStandby: 去掉它的话, "用户双击关屏后屏被自己点亮"(定时器可能还在跑)。
  if (gState.sleepRemainSec > 0 && gState.sleepStandby) {
    gState.sleepStandby = false;
    display.noteActivity();          // 亮屏 + 强制下一帧重画
    Serial.println("[SLEEP] 重新设定睡眠定时器 -> 退出熄屏待机, 屏幕点亮");
  }
  // ★定时关机到点(红外 IR_TIMER 设的): 真正关机 —— 停播+关无线+屏灭+功放关+进深睡。
  //   ★这个函数【不返回】, 所以必须放在 sleepJustFired 之后: 先把"熄屏"做完再关机,
  //   顺序反了的话 screenOff() 就白做了(反正屏也要断电, 但保持逻辑顺序清晰)。
  //   注意必须在 player.loop() 之后调用 —— 此刻音频泵仍可能在跑, 由 enterDeepSleep()
  //   内部统一收尾(挂起泵任务 + 关 WiFi), 见 webradio-s3p.ino 的 enterDeepSleep。
  if (gState.sleepShutdownFired) {
    gState.sleepShutdownFired = false;
    gState.sleepStandby = false;     // 真关机: 待机标志一并作废(深睡唤醒后状态已重置)
    enterDeepSleep();
  }
  PH(web.loop(),    PH_WEB);      // 主线程: 修正 gState(列表被Web改过) + 重建状态快照
  // 屏幕背景图落盘(2026-10-06): 上传的数据由 Web 异步任务的 onBody 收在 PSRAM 里, 落盘必须
  // 留在主循环 —— LittleFS 写入会短暂冻结 flash cache, 放异步任务里会拖累同核任务。
  // 没有待处理的上传时这里只是几个 bool 判断, 开销可忽略, 所以不进性能探针。
  web.bgTick();
  PH(ir_poll(),     PH_IR);       // 红外: 处理收到的波形 / 比对 / 触发动作(无码时极轻量)
  PH(store.loop(),  PH_STORE);    // 延迟落盘: 在 WiFi/重启处理前先持久化配置(避免删改丢)
  PH(portal.loop(), PH_PORTAL);   // DNS(captive) + 断网重连
  // 开机后待 WiFi 真正连上再自动续播【上次关机时的电台】(setup 时可能尚未就绪)。
  // 2026-09-19 修正: 不再因"上次停在 HLS 台"就强制改播第 1 台 —— 用户要求重启后续播真正的上次电台。
  // 改用"连上 WiFi 后再延迟 AUTO_RESUME_DELAY_MS"再开播, 让 Web 首屏先加载, 含 HLS 也能续播。
  if (gAutoPlayPending) {
    if (gState.wifiConnected && store.count() > 0) {
      if (gAutoPlayAt == 0) gAutoPlayAt = millis() + AUTO_RESUME_DELAY_MS;
      if (millis() >= gAutoPlayAt) {
        gAutoPlayPending = false;
        gAutoPlayAt = 0;
        player.playCurrent();       // 内部走 store.getLast() -> 续播上次电台(含 HLS, 不再跳第1台)
      }
    } else if (gState.apMode) {
      gAutoPlayPending = false;     // 进入 AP 配网模式则不自动播放(无外网)
      gAutoPlayAt = 0;
    }
  }
  // ── 定时开机(闹钟, 2026-10-08) ─────────────────────────────────────────────
  // 判定本身在 alarm 模块里(内置 1s 节拍 + 当日去重), 这里只执行它不该碰的"音频+显示+UI"。
  // ★前提: 设备必须【通电】—— 熄屏待机可以(那样 CPU/WiFi/钟都还在跑), 深度休眠不行
  //   (CPU 已停机, 只能靠电源键/红外唤醒)。本功能不做 RTC 定时唤醒。
  {
    const AlarmEvent aev = alarmTick();
    if (aev == ALARM_START) {
      const AlarmCfg ac = alarmGet();
      // 先退掉那两个"浏览界面": 否则屏上还停在列表上, 用户看不到已在播放。
      if (gListMode) listExit();
      if (gSdMode)   sdExit();
      gState.sleepStandby = false;   // 明确退出熄屏待机(此刻电源键应恢复"关机"语义)
      display.noteActivity();        // 亮屏 + 重置熄灭计时
      // 未连网时 execPlayUrl 会拒绝并置 status="未连网", 属预期(不重试; 设备常态是长通电待机)
      player.play(ac.url);
      uiNotice("定时开机: " + (ac.name.length() ? ac.name : String("已开播")), 4000);
      Serial.printf("[ALARM] 已触发开播 -> %s\n", ac.url.c_str());
    } else if (aev == ALARM_STOP) {
      // ★复用"睡眠定时器到点"的成熟路径, 而不是直接调 player.stop():
      //   execStop() 内部会清掉 sleepStandby(它把停播理解为"用户手动操作, 退出待机"),
      //   而闹钟停播恰恰【要】进入熄屏待机。故把剩余时间设成 1 秒 -> 下一圈由音频模块
      //   走标准停播路径并置 sleepJustFired, 由上面的主循环分支关屏 + 置 sleepStandby。
      if (gState.playing) {
        gState.sleepToPowerOff = false;   // 别误触"定时关机"(两个定时器共用这组字段)
        gState.sleepMin        = 0;
        gState.sleepRemainSec  = 1;       // 约 1 秒后到点
      } else {
        // 本来就没在播(比如已手动停掉): 不必走停播路径, 直接把屏关掉并标记待机态
        gState.sleepStandby = true;
        display.screenOff();
        Serial.println("[ALARM] 到点停播(当前本就未播放) -> 直接进入熄屏待机");
      }
    }
  }
  // 串口命令: T=蜂鸣自检 M=节目信息开关 D=诊断(含电池[BATT]/充电[CHG]/时间[TIME]/天气[WX]/接口地址[SRC]/★内存[MEM]) F=字库自检 P=切换喂流模式(泵/主循环) B=起播预缓冲开关 S=屏测 L=翻转背光 R=恢复出厂 W=抓流测试 I=红外学习菜单 Z=关机(深度休眠) X=改地理接口地址 Y=改天气接口地址 C=轮换屏幕类型 O=彩屏(ST7789/ILI9341)横竖屏切换 K=轮换彩屏显示方向(MADCTL) J=翻转彩屏反显 Q=轮换彩屏调色板主题
  if (Serial.available()) {
    char c = Serial.read();
    static bool gIrMenu = false;
    if (gIrMenu) {
      // 红外学习子菜单: 上一字符已选 'I'/'i' 进入。★这一段各分支必须【互斥】——
      //   旧写法把 T/Z 放进 else-if 链、而 M/D/S/L/R/W 用独立 if, 结果在菜单里敲 M 会
      //   "既取消菜单又切换节目信息开关", 语义混乱。现在统一成一条链: 菜单期间只认
      //   1~6(学某个动作) 与 c(清除全部), 其它任何键都只是"取消"。
      gIrMenu = false;
      if (c >= '1' && c <= '9') {           // 1~9 -> 动作 0~8
        IRAction a = (IRAction)(c - '1');
        ir_begin_learn(a);                  // ★进入学习即清除该动作原有码(见 ir_begin_learn 注释)
        Serial.printf("[IR] 重学 %s (原有码已清除) : 请按遥控器对应按键(%.0fs 内)\n",
                      ir_action_name(a), (float)IR_LEARN_MS / 1000.0f);
      } else if (c == '0') {
        // ★第 10 个动作(定时关机)没有对应的数字键 —— '1'~'9' 已经被 0~8 占满。
        //   这里把 '0' 单独映射给它, 不要写成 (c-'1')(那是 -1, 会得到非法枚举)。
        ir_begin_learn(IR_TIMER);
        Serial.printf("[IR] 重学 %s (原有码已清除) : 请按遥控器对应按键(%.0fs 内)\n",
                      ir_action_name(IR_TIMER), (float)IR_LEARN_MS / 1000.0f);
      } else if (c == 'c' || c == 'C') {
        ir_clear_all(); Serial.println("[IR] 已清除全部学习码");
      } else {
        Serial.println("[IR] 取消");
      }
    } else {
      switch (c) {
        case 'I': case 'i': {
          gIrMenu = true;
          int learned = 0; for (int a = 0; a < IR_COUNT; a++) if (ir_has_code((IRAction)a)) learned++;
          Serial.printf("[IR] 模块状态: %s | 已学习 %d/%d | 接收脚 GPIO%d\n",
                        ir_is_ready() ? "RMT就绪" : "未初始化(检查VS1838B接线)",
                        learned, IR_COUNT, IR_RX_PIN);
          Serial.println("[IR] 红外学习菜单: 1上一台 2下一台 3播放/暂停 4音量+ 5音量- 6关机 7屏幕旋转(仅彩屏) 8切换屏幕(ST7567/SSD1306/ST7789/ILI9341循环) 9音源切换(网络电台<->本地SD卡) 0定时关机(单键循环 关闭/1h../12h) c清除全部 (其它键取消)");
          Serial.println("[IR] ★ 选数字即【重学】: 原有码立即清除, 按新键则替换; 15秒内不按则保持已清除。");
          break;
        }
        case 'T': case 't': player.beepTest(); break;
        case 'M': case 'm': player.setMetaEnabled(!player.metaEnabled); break;
        case 'D': case 'd':
          player.printDiag();
          // ---- 内存体检(2026-10-06 加) ----
          // ★背景图那 150KB 能不能分配出来全看这几项。判读要点:
          //   ① psramSize 应为 8192KB(N16R8 的 8MB OPI PSRAM)。若是 0 -> PSRAM 根本没启用,
          //      不只是背景图不可用, audio 的 512KB 输入缓冲也已静默退回 DRAM 的 16KB;
          //   ② "最大连续块"比"空闲总量"更重要 —— 分配 150KB 要的是一整块连续的,
          //      空闲很多但最大块偏小就是碎片化。
          //   ★另注意 ps_malloc() 内部本来就会降级到 DRAM(heap_caps_malloc_prefer),
          //     所以它失败 = PSRAM 与 DRAM 都拿不出这么大一块, 而不是"只是 PSRAM 不够"。
          Serial.printf("[MEM ] PSRAM 总%uKB 空闲%uKB 最大连续块%uKB | 内部RAM 空闲%uKB 最大连续块%uKB | 背景图需 %dKB\n",
                        (unsigned)(ESP.getPsramSize() / 1024),
                        (unsigned)(ESP.getFreePsram() / 1024),
                        (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024),
                        (unsigned)(ESP.getFreeHeap() / 1024),
                        (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                        (int)(BG_RGB_BYTES / 1024));
          if (ESP.getPsramSize() == 0) {
            Serial.println("[MEM ] ★★PSRAM 未启用! N16R8 本该有 8192KB —— 背景图无法工作, "
                           "且音频输入缓冲已退回 DRAM 小缓冲(可能是历史卡顿的隐藏原因)");
          } else if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < (size_t)BG_RGB_BYTES) {
            Serial.println("[MEM ] ★PSRAM 已启用但最大连续块不足以放下 150KB 背景图(碎片化)");
          } else {
            Serial.println("[MEM ] PSRAM 正常, 可容纳 150KB 背景图");
          }
          Serial.printf("[BATT] 电池电压: %.2fV (ADC GPIO%d, 分压比 %.1f, 采样%d次)\n",
                        gState.batteryV, BATT_ADC_PIN, BATT_DIVIDER, BATT_SAMPLES);
          Serial.printf("[CHG ] 充电状态: %s (GPIO%d, %s有效)\n",
                        gState.charging ? "充电中" : "未充电",
                        CHG_STAT_PIN, CHG_ACTIVE_LOW ? "低电平" : "高电平");
          // 联网数据(时钟/农历/天气)的状态, 方便一眼看出"是没配 Key / 网络不通 / 接口挂了"
          { struct tm tmn; timeLocal(&tmn);
            Serial.printf("[TIME] %02d:%02d:%02d %04d-%02d-%02d %s (网络校时: %s)\n",
                          tmn.tm_hour, tmn.tm_min, tmn.tm_sec, tmn.tm_year + 1900, tmn.tm_mon + 1, tmn.tm_mday,   // tm_mon 0~11, +1
                          WEEKDAY_CN[tmn.tm_wday], gState.timeValid ? "已校准" : "未校准(用编译期兜底时间)"); }
          if (gState.wx.valid)
            Serial.printf("[WX  ] %s %s %d℃(体感%d) 湿度%d%% %s %s级 | 今日 %d~%d℃ 明日 %d~%d℃ | %u分钟前\n",
                          gState.wx.city.c_str(), gState.wx.text.c_str(), gState.wx.tempC, gState.wx.feelsC,
                          gState.wx.humidity, gState.wx.windDir.c_str(), gState.wx.windScale.c_str(),
                          gState.wx.tminC, gState.wx.tmaxC, gState.wx.tmin2C, gState.wx.tmax2C,
                          (unsigned)((millis() - gState.wx.updMs) / 60000UL));
          else
            Serial.println("[WX  ] 天气未取到(检查网络; 串口 X/Y 可现场换接口地址)");
          Serial.printf("[SRC ] %s | 地理 %s | 天气 %s\n",
                        WX_PROV_KEYLESS ? "免密钥" : "和风",
                        weatherGeoUrl(), weatherFcUrl());
          break;
        // F = 字库覆盖自检: 打印内置测试串的缺字情况, 回答"这个台名/曲名为什么显示成方框"
        case 'F': case 'f': display.fontCheckBuiltin(); break;
        // P = 切换喂流模式(现场 A/B): 音频泵任务(方案B) <-> 主循环(方案A 回退)。
        //     用途: 一旦怀疑"方案B 的泵任务"有问题, 不用重新烧录, 发个 P 立刻退回旧喂流方式对比。
        case 'P': case 'p': player.setPumpEnabled(!player.pumpEnabled); break;
        case 'B': case 'b': player.togglePrebuf(); break;  // 切换起播预缓冲(现场 A/B, 免重烧)
        // K = 轮换彩屏(ST7789/ILI9341)的 MADCTL(0x36), 现场试"方向 + RGB/BGR 顺序", 免重烧。
        //     用途: 画面上下颠倒 / 镜像时, 连按 K 试到正常为止。★结果自动存 NVS, 重启仍生效
        //           (不用再回填 config.h)。候选值按 芯片×当前横/竖屏 分组, 只列物理上成立的那些。
        case 'K': case 'k':
          if IS_TFT_SCREEN(display.screenType()) {
            st7789MadctlCycle(); display.invalidate();
          }
          else Serial.println("[SCR ] 当前屏型不是彩屏(ST7789/ILI9341), K 命令无效(先到后台把屏幕类型切成彩屏)");
          break;
        // J = 翻转彩屏反显(INVON 0x21 <-> INVOFF 0x20)。ST7789 / ILI9341 各存一份极性。
        //     用途: 画面"整片发白、像照片底片"(底色变亮、文字变暗)就是反显设反了, 按 J 立即翻转。
        case 'J': case 'j':
          if IS_TFT_SCREEN(display.screenType()) st7789ToggleInvert();
          else Serial.println("[SCR ] 当前屏型不是彩屏(ST7789/ILI9341), J 命令无效");
          break;
        // Q = 轮换彩屏调色板主题(经典黑白 -> 深蓝图片风 -> 墨绿夜光)。两种彩屏共用同一套主题。
        //     1bpp 屏只有两个颜色, 颜色不满意时按 Q 现场换一套看效果(免重烧); 满意后回填 config.h。
        case 'Q': case 'q':
          if IS_TFT_SCREEN(display.screenType()) {
            st7789ThemeCycle(); display.invalidate();
          }
          else Serial.println("[SCR ] 当前屏型不是彩屏(ST7789/ILI9341), Q 命令无效");
          break;
        // O = 切换彩屏横屏 320x240 / 竖屏 240x320(存 NVS, 立即整体重建)。
        //     ★这块玻璃原生是 240x320(竖), 当 320x240 横屏用要靠 MADCTL 交换行列 ——
        //       所以同一个模组装成横的/竖的, 屏幕上看到的排版相差 90°。按 O 切到与外壳一致的方向。
        //       两个方向各有一套专用排版(横屏: 时钟在左+天气卡在右上; 竖屏: 时钟居中+卡片竖排)。
        case 'O': case 'o':
          if IS_TFT_SCREEN(display.screenType())
            display.setST7789Portrait(!st7789PortraitGet());
          else Serial.println("[SCR ] 当前屏型不是彩屏(ST7789/ILI9341), O 命令无效");
          break;
        // C = 轮换屏幕类型(ST7567 -> SSD1306 -> ST7789 -> ILI9341 -> 循环), 存 NVS, 主循环立即重建 u8g2。
        //     ★花屏/白屏第一件事就是确认这里: NVS 里存的屏型若不是你实际接的那块
        //       (本工程出厂默认 = ST7789, 见 config.h 的 SCREEN_DEFAULT), 固件就没用对驱动
        //       -> 屏当然不正常。这条命令免进后台、免重烧即可切对。
        //     ★注意 ST7789 与 ILI9341 是"同位置换屏"(共用同一组引脚): 选中的型号与实插模块不符时
        //       会花屏/无显示, 属正常 —— 要选对芯片型号。
        case 'C': case 'c': {
          static const int seq[4] = { SCREEN_ST7567, SCREEN_SSD1306, SCREEN_ST7789, SCREEN_ILI9341 };
          const int cur = display.screenType();
          int nx = SCREEN_ST7567;
          for (int i = 0; i < 4; i++) if (seq[i] == cur) { nx = seq[(i + 1) % 4]; break; }
          display.setScreenType(nx);
          Serial.printf("[LCD] 屏幕类型 -> %s (已存NVS, 主循环重建)\n",
                        nx == SCREEN_ST7789 ? "ST7789" : nx == SCREEN_ILI9341 ? "ILI9341"
                        : nx == SCREEN_SSD1306 ? "SSD1306" : "ST7567");
          break; }
        // X / Y = 运行时改天气接口地址(存 NVS, 免重烧)。
        //   发 X(地理接口) 或 Y(天气接口) 后, 接着把地址敲进去回车; 单独发 X/Y 不带地址 = 恢复默认。
        //   用途: 默认走免密钥的 ipwho.is + open-meteo(海外服务), 某个网络下慢/不通时可换成镜像/国内源。
        case 'X': case 'x': readWxUrlLine(true);  break;
        case 'Y': case 'y': readWxUrlLine(false); break;
        case 'S': case 's': display.testPattern(); break;
        case 'L': case 'l': display.toggleBacklight(); break;
        // R = 一键恢复出厂电台表(清掉残留的 SomaFM 国外源等) + 立即播放第 1 台
        case 'R': case 'r': store.resetToDefaults(); player.playCurrent(); break;
        // W = 原始抓流测试(绕过音频库, 用本机自己的 TCP 数 3 秒内收到多少音频字节)
        //     作用: 把"服务器/网络没推音频"与"库侧失败"彻底分开 —— 这是唯一可信的网络判据
        //     (开发机上 curl 会被代理截断, 结论不可用)。没有当前地址时用 store 里记住的那台。
        case 'W': case 'w': {
          String u = gState.url;
          if (u.length() == 0 && store.count() > 0) u = store.url(store.getLast());
          if (u.length() == 0) Serial.println("[RAW] 没有可用地址(先播放一个电台再发 W)");
          else                 player.rawStreamTest(u);
          break;
        }
        // Z = 关机(深度休眠): 停播 + 关无线 + 屏灭 + 功放关 + 深睡。★本函数不返回。
        case 'Z': case 'z': enterDeepSleep(); break;
        // U = 睡眠定时器: 设 30 分钟(纯软件倒计时, 到点停播+熄屏, 不进深睡); u = 取消。
        //   任意分钟数请用 Web 后台"睡眠定时器"或串口 WS/HTTP 接口。
        case 'U': case 'u': player.setSleepTimer((c == 'u') ? 0 : 30); break;
        // G = 本地 SD 卡(阶段②)。G = 进入/退出 SD 文件列表; H = 立刻扫描并打印文件清单;
        //     V = 退出 SD 音源回到网络电台。
        case 'G': case 'g': sdListToggle(); break;
        case 'H': case 'h': {
          const bool ok = gSd.mounted() || gSd.begin();
          const int  n  = ok ? gSd.scan() : -1;
          Serial.printf("[SD  ] 手动扫描: %s, %d 个文件 | 播放源=%s 目录=%s\n",
                        ok ? "已挂载" : "挂载失败", n,
                        gState.sdActive ? "SD" : "网络电台", SD_MOUNT);
          break;
        }
        case 'V': case 'v': sdReqExit(); Serial.println("[SD  ] 已请求退出 SD 音源"); break;
        // E = 定时关机循环档位(与遥控 IR_TIMER 同一入口, 便于没遥控时验证)。
        //   ★刻意不用 T —— T 早已是"蜂鸣测试"(player.beepTest), 复用会把那个功能顶掉。
        //   快捷键字母已基本用完, 新增功能请继续挑没被占用的字母(先 grep 现有 case 列表)。
        case 'E': case 'e': timerCycle(); break;
        // A = SBR 开关(2026-10-07): a = 开(HE-AAC 输出 96kHz) / A = 关(推荐, 输出 48kHz)。
        //   用于现场对比"发颤"是否消失。★换台后生效(解码器状态在建流时就定了)。
        case 'A': case 'a': player.setSbrEnabled(!player.sbrEnabled()); break;
        default: break;   // 未定义字符忽略(避免串口噪声/误触触发动作)
      }
    }
  }
  PH({
    btn.update();                 // 按键扫描
    handleButtons();              // 按键事件(左/中/右)
    handlePowerBtn();             // 电源键扫描: 单击关机
    sdTick();                     // 阶段②: 消费 Web 的 SD 挂载/扫描/退出请求(主循环独占 SD 卡驱动)
    updateAmp();                  // 功放使能: 真正出声才开(连接中/停止/待机静音)
    updatePlaybackWatchdog();     // 重连 / 跳台(24h 自愈核心)
    checkHeapSafety();            // 内存过低自动重启兜底
    heartbeat();                  // 心跳日志(每分钟)
  }, PH_BTN);

  PH(updateDisplay(), PH_DISPLAY); // 屏刷新(ST7567 或 SSD1306; 内部限流 ~3fps, 软件 bit-bang 很吃 CPU)

  // 电池电压 + 充电状态(节流刷新): 主循环写 gState.batteryV / gState.charging, 供 /api/state 与 D 诊断读取
  static uint32_t battMs = 0;
  if (millis() - battMs > BATT_POLL_MS) {
    battMs = millis();
    gState.batteryV = readBatteryVoltage();
    gState.charging  = readChargeState();
  }
  // 天气/温湿度: 开机一次 + 每小时一次(节流在 net_feeds.cpp 的 weatherTick 内部)
  weatherTick();

  // 流畅度诊断(定期)。缓冲填充率持续偏低 = 网络喂不饱; 持续满 = CPU 解不动。
  static uint32_t diagMs = 0;
  if (AUDIO_DIAG_INTERVAL_MS > 0 && millis() - diagMs > AUDIO_DIAG_INTERVAL_MS) {
    diagMs = millis();
    player.printDiag();
    display.printFrameStats();   // 显示帧绘制/跳过比例(跳过率 = 省下的 CPU, 见该函数注释)
  }

  // 只让出 1 个 tick。原为 delay(2) —— 主循环每圈白等 2ms, 库就少两次机会去
  // TCP 里取音频数据。注意不能彻底去掉: 完全不让出 CPU 会饿死 IDLE 任务,
  // 触发 task_wdt 重启。
  delay(1);
}

// 关机(进深度休眠)。停播 + 关 WiFi/蓝牙 + 屏灭 + 功放关 + ESP32 deep sleep。
// 不返回。唤醒: 电源键(POWER_BTN_PIN, EXT0)直接开机; 红外(EXT1)唤醒后须在 setup() 里
// 经 ir_confirm_power() 确认"收到的是已学 power 码"才真开机, 否则立刻回深睡。
void enterDeepSleep() {
  Serial.println("[PWR] 关机: 停播 + 关 WiFi/蓝牙 + 屏灭 + 功放关 + 进深度休眠");
  // 0) ★先挂起音频泵任务(方案B): 紧接着要断 WiFi, 若泵任务此刻正卡在 audio.loop() 的 socket
  //    读里, 网络被抽走会让库内部状态异常。挂起后就没有任何人在碰 audio 对象了。
  player.suspendPumpTask();
  // 1) 先持久化"当前电台序号 + 音量": 否则关机瞬间尚未落盘的改动会丢失,
  //    下次开机就播回旧台/旧音量(用户要求"重启后续播关机时的电台和音量")。
  //    必须在主循环上下文调用(本函数只经 gShutdownPending 从主循环进入), 落盘安全。
  store.saveNow();                  // 写 stations.json(含 lastIndex + volume), 原子 rename 不丢文件
  // 2) 停播(网络/解码都省了)。泵任务已挂起, 这条命令不会被处理 —— 但紧接着就深睡断电,
  //    无需收尾(解码器内存/套接字随深睡一并消失)。
  player.stop();
  // 3) 关无线(BT 先停, 再停 WiFi 射频)
  btStop();
  WiFi.disconnect(true, false);   // 断网但不清凭据(下次开机自动重连)
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
  // 4) 屏灭: 清屏 + 背光灭 + 置已断电标志(tick/noteActivity 不再点亮背光)
  display.powerOff();
  // 5) 功放关(与 AMP_EN_ACTIVE 相反电平)
  digitalWrite(AMP_EN_PIN, AMP_EN_ACTIVE == HIGH ? LOW : HIGH);
  // 5.5) 关机时网络侧 DAC 供电断开 —— 否则深睡后 TDA1387 仍通电耗电(十几 mA), 深睡电流压不下去。
  //      深睡期间 ESP32 引脚转为高阻, 由 MOSFET 栅极的 10k 下拉保证维持关断(硬件前提)。
  setNetDacPower(false);
  // 6) 配置唤醒源(电源键 EXT0 + 红外 EXT1) 并进深睡 —— 细节见 goToSleep()
  goToSleep();              // 不返回
}

// 仅"配置唤醒源 + 进深睡", 不做任何外设关闭/落盘。两类调用方共用:
//   ① enterDeepSleep()           : 用户主动关机(先停播/关无线/屏灭/功放关再进来);
//   ② setup() 里唤醒确认失败回睡 : 深睡被红外唤醒但收到的不是 power 码, 立刻回睡(此时尚未初始化外设)。
// 唤醒源: EXT0(单脚, 低)接电源键 + EXT1(单脚接红外, ALL_LOW) —— 本 IDF 的 ext1 只支持
//   ALL_LOW / ANY_HIGH(无 ANY_LOW), 故两路分开使能, 任一触发即复位开机。
//   ⚠ 红外这路是"任何遥控码都会唤醒"(硬件层做不到按码过滤), 真正的按键过滤在 setup() 里
//     由 ir_confirm_power() 完成: 非已学 power 码 -> 立即再回深睡(相当于该键不开机)。
static void goToSleep() {
  // ★关键(S3 深睡唤醒后卡死的根因): 保持 VDD_SPI(VDDSDIO) 供电域在深睡期间不掉电。
  //   ESP32-S3 的 VDD_SPI 同时给 flash 与 PSRAM(本板为八线 OPI) 供电; 若深睡把它关掉,
  //   唤醒时 ROM 引导程序无法重新初始化 PSRAM/Flash -> 表现为"开机卡在开机中, 必须断电才能恢复"。
  //   本调用于是必须在 esp_deep_sleep_start() 之前执行。
  esp_sleep_pd_config(ESP_PD_DOMAIN_VDDSDIO, ESP_PD_OPTION_ON);
  pinMode(POWER_BTN_PIN, INPUT_PULLUP);
  gpio_reset_pin((gpio_num_t)IR_RX_PIN);   // 此刻红外脚可能被 RMT 占用, 复位回普通输入
  pinMode(IR_RX_PIN, INPUT_PULLUP);        // 上拉: 空闲为高, 不误唤醒
  esp_sleep_enable_ext0_wakeup((gpio_num_t)POWER_BTN_PIN, 0);                 // 0 = 低电平唤醒(电源键按下)
  esp_sleep_enable_ext1_wakeup((1ULL << IR_RX_PIN), ESP_EXT1_WAKEUP_ALL_LOW); // 红外收到信号(输出拉低)即唤醒
  // 防自唤醒: 等电源键松开(高), 再静候 300ms 让遥控脉冲结束, 避免"按关机键瞬间又被同一次按下唤醒"。
  uint32_t t0 = millis();
  while (digitalRead(POWER_BTN_PIN) == LOW && millis() - t0 < 2000) delay(20);
  delay(300);
  Serial.println("[PWR] 已进入深度休眠 (按电源键 / 已学 power 遥控键唤醒)");
  esp_deep_sleep_start();   // 不返回
}

// ============ 网络侧 DAC 供电 / 功放静音(主循环上下文执行) ============

// 仅网络收音机: 网络侧 TDA1387-A DAC 由 setNetDacPower() 控制供电, 切台/音量由 AudioPlayer 内部完成,
// 不再有 6686(FM/AM/WM)音源, 故无音源切换/频段/频率相关逻辑。

// ============ 网络侧 DAC 供电 / 功放静音(2026-09-22 新增) ============
// 背景: 网络侧的 TDA1387-A 原先没有任何电源管理 —— 靠"停止播放 = 停喂 I2S = 数字静音"糊过去。
//   这个假设站不住(见 config.h 里 NET_DAC_PWR_PIN 的三条理由), 且深睡时它还在通电耗电。
// 做法: 播放时由 setNetDacPower(true) 给网络侧 DAC 通电, 深睡时 setNetDacPower(false) 断电。
static uint32_t gAmpMuteUntil = 0;   // 强制静音截止时刻(millis); updateAmp() 会尊重它

// 临时静音功放。用途: DAC 供电通断 / I2C 探测导致的"模块侧 DAC 瞬间上电"都会在合流点产生
// 一个瞬态, 功放开着就是扬声器里一声"咔哒"。先静音, 稳定后再由 updateAmp() 自动恢复。
void muteAmpFor(uint32_t ms) {
  digitalWrite(AMP_EN_PIN, AMP_EN_ACTIVE == HIGH ? LOW : HIGH);
  gAmpOn = false;
  gAmpMuteUntil = millis() + ms;
}

static void setNetDacPower(bool on) {
  if (NET_DAC_PWR_PIN < 0) { gNetDacOn = false; return; }   // 该路未接 -> 保持旧行为, 不碰任何 GPIO
  digitalWrite(NET_DAC_PWR_PIN,
               on ? NET_DAC_PWR_ON_LEVEL
                  : (NET_DAC_PWR_ON_LEVEL == HIGH ? LOW : HIGH));
  gNetDacOn = on;
}


// 第4键(电源键)扫描(三态 FSM, 复用 GPIO3 不改脚):
//   短按 = 返回(退出电台列表)
//   双击 = 仅网络收音机, 无音源可切换(原双击为音源/频段循环, 已随 6686 移除)
//   长按(>=POWER_LONG_MS) = 关机(深度休眠)
// 详见开发文档「六、控制手势」。单击有 POWER_DBL_MS 延迟(等双击窗口), 用户已确认可接受。
void handlePowerBtn() {
  static uint8_t  st = HIGH;
  static uint32_t tChg = 0;
  static bool     pressed = false;
  static uint32_t pressStart = 0;
  static uint32_t lastReleaseMs = 0;
  static bool     pendingShort = false;   // 单击待定(等双击窗口过完再执行"返回")
  static uint32_t shortMs = 0;

  uint8_t raw = digitalRead(POWER_BTN_PIN);   // 高=未按下
  if (raw != st) {
    if (millis() - tChg >= DEBOUNCE_MS) {     // 稳定超过去抖窗才认
      if (raw == LOW && st == HIGH) {
        // 刚按下
        uint32_t now = millis();
        // ★任意按下 -> 先点亮背光(2026-10-08): 电源键是【独立扫描】的, 不在 btn.anyActivity()
        //   覆盖范围内, 故屏熄灭(睡眠待机 / 双击关屏)后按电源键原本不会亮屏。
        //   放在"按下沿"而非"单击派发"是为了零延迟 —— 单击要等双击窗口(约 300ms)才派发。
        //   双击时本行先亮、紧接着下面的 screenOff() 再灭, 终态仍是"关屏", 不冲突。
        display.noteActivity();
        if (lastReleaseMs && now - lastReleaseMs <= POWER_DBL_MS) {
          // 双击(2026-10-07 用户指定): 关闭屏幕。
          //   · SSD1306 OLED: 自发光 -> 清屏即全黑(display.screenOff 已做)
          //   · ST7567 单色  : 非自发光 -> 只关背光就是全黑
          //   · ST7789/ILI9341 彩屏: 背光+像素都发光 -> display.screenOff() 里补发 DISPOFF(0x28)
          //   再按一下任意键即可重新点亮(见 display.noteActivity)。
          pendingShort = false;
          lastReleaseMs = 0;
          display.screenOff();
          Serial.println("[PWR] 双击 -> 关闭屏幕(任意按键重新点亮)");
        } else {
          pressed = true;
          pressStart = now;
        }
      } else if (raw == HIGH && st == LOW && pressed) {
        // 刚松开
        pressed = false;
        uint32_t held = millis() - pressStart;
        lastReleaseMs = millis();
        if (held >= POWER_LONG_MS) {
          pendingShort = false;
          enterDeepSleep();          // 长按 = 关机
        } else {
          pendingShort = true;       // 短按: 等双击窗口过完再"返回"
          shortMs = millis();
        }
      }
      st = raw;
    }
    tChg = millis();
  }
  // 单击延迟派发: 双击窗口已过且确实无第二击 -> 执行"返回"
  if (pendingShort && millis() - shortMs > POWER_DBL_MS) {
    pendingShort = false;
#if STANDBY_POWER_OPENS
    // ★熄屏待机(睡眠定时器到点)时, 电源键短按 = 【开机】: 亮屏 + 恢复播放(2026-10-08)。
    //   与遥控电源键同一语义(见 ir_set_action_cb 的 IR_POWER 分支), 保证"面板/遥控一致"。
    //   ★为什么放在"确认非双击"之后而不是按下沿: 双击的第一击也走这里的话, 就会先开播;
    //     紧接着第二击只关屏 -> 变成"屏幕黑的、音乐在响", 用户会以为设备疯了。
    if (gState.sleepStandby) {
      gState.sleepStandby = false;
      Serial.println("[PWR] 短按: 熄屏待机中 -> 开机(亮屏 + 恢复播放)");
      if (!gState.playing) player.togglePlay();
      return;                            // 待机开机是一次性动作, 不再兼"返回/退列表"
    }
#endif
    // ★两个列表都要能退(阶段② 之后有"电台列表"和"SD 文件列表"两套浏览界面)。
    //   电源键的语义本就是"返回/退出当前界面", 只退一半会让用户在 SD 列表里按电源键没反应。
    if (gListMode) listExit();        // 退出电台列表
    if (gSdMode)   sdExit();          // 退出 SD 文件列表
    // 非列表态: 短按"返回"暂无其它出口(网络主界面无上级), 留空
  }
}

void handleButtons() {
  // 按键映射(2026-10-07 按用户指定重排):
  //   电源(GPIO3, 独立扫描): 单击=返回(退列表) / 双击=关闭屏幕 / 长按=开关机(进深睡)
  //     ★2026-10-08: 熄屏待机态(睡眠定时器到点)下单击 = 开机(亮屏+恢复播放), 见下面单击派发处。
  //   左(BTN_LEFT)  : 单击=上一台/上一曲 / 双击=音量-1 / 长按=音量连续减
  //   右(BTN_RIGHT) : 单击=下一台/下一曲 / 双击=音量+1 / 长按=音量连续加
  //   中(BTN_CENTER): 单击=播放/暂停 / 双击=音源切换(SD⇄网络) / 长按=进入电台列表或SD列表
  //   列表内: 左/右双击 = 电台列表切分类 / SD 列表切循环方式
  // 注意: 事件必须【各自求值】再分别判定 —— 不能写成 `a() || b()`,
  // 短路求值会跳过后者, 让事件残留到下一帧。
  // 音量落盘无压力: saveVolume() 只置脏标, store.loop() 合并落盘一次, 连续调节不刷 flash。
  if (btn.anyActivity()) {
    display.noteActivity();          // 任意按键点亮背光 + 重置熄灭计时
    // ★任意按键 = 用户在用设备 -> 退出"熄屏待机"(2026-10-08), 见 player_state.h 该字段注释。
    //   注意这里【只清标志不自动开播】: 用户按的是哪个功能键就执行哪个功能(比如按"下一台"
    //   本身就会开播)。要"一键开机"请按电源键(见 handlePowerBtn)。
    gState.sleepStandby = false;
  }

  // ---- 【SD 文件列表】模式 ----
  // 上下=移动光标 / 中键单击=确定播放 / 中键长按=收起列表 /
  // ★左键双击=循环方式反向一档, 右键双击=正向一档(2026-10-07 新增, 用户要求)。
  if (gSdMode) {
    if (btn.downPressed(BTN_LEFT))    sdMove(-1);
    if (btn.downPressed(BTN_RIGHT))   sdMove(+1);
    if (btn.repeatPressed(BTN_LEFT))  sdMove(-1);
    if (btn.repeatPressed(BTN_RIGHT)) sdMove(+1);
    if (btn.singlePressed(BTN_CENTER)) sdConfirm();  // 单击 = 确定并播放该文件
    if (btn.longPressed(BTN_CENTER))   sdExit();     // 长按 = 收起列表(不换文件)
    if (btn.doublePressed(BTN_LEFT))   player.cycleSdLoop(-1);
    if (btn.doublePressed(BTN_RIGHT))  player.cycleSdLoop(+1);
    // ★排空本模式用不到的事件: 否则退出后攒下的"双击"会误触发"切音源"。
    btn.clearEvents();
    return;
  }

  // ---- 【电台列表】模式: 三键语义整体切换(见文件上方"电台列表"块) ----
  // 上下用【按下沿 downPressed】而不是"单击": 单击要等 BTN_DOUBLE_CLICK_MS(300ms)双击窗口
  // 才派发, 翻列表会明显顿一下; 按下沿是零延迟。长按连续由 repeatPressed 接续。
  if (gListMode) {
    if (btn.downPressed(BTN_LEFT))    listMove(-1);
    if (btn.downPressed(BTN_RIGHT))   listMove(+1);
    if (btn.repeatPressed(BTN_LEFT))  listMove(-1);
    if (btn.repeatPressed(BTN_RIGHT)) listMove(+1);
    if (btn.doublePressed(BTN_LEFT))  listCycleGroup(-1);   // 双击左 = 上一个分类
    if (btn.doublePressed(BTN_RIGHT)) listCycleGroup(+1);   // 双击右 = 下一个分类
    if (btn.singlePressed(BTN_CENTER)) listConfirm();       // 单击 = 确定(唯一换台入口)
    if (btn.longPressed(BTN_CENTER))   listExit();          // 长按 = 收起列表(不换台)
    btn.clearEvents();
    return;
  }

  // ---- 主界面 ----
  // 左键: 单击=上一台/上一曲(音源决定) / 双击=音量-1 / 长按=音量连续减
  if (btn.singlePressed(BTN_LEFT))   player.prev();
  if (btn.doublePressed(BTN_LEFT))   player.setVolume(gState.volume - 1);
  if (btn.repeatPressed(BTN_LEFT))   player.setVolume(gState.volume - 1);

  // 右键: 单击=下一台/下一曲 / 双击=音量+1 / 长按=音量连续加
  if (btn.singlePressed(BTN_RIGHT))  player.next();
  if (btn.doublePressed(BTN_RIGHT))  player.setVolume(gState.volume + 1);
  if (btn.repeatPressed(BTN_RIGHT))  player.setVolume(gState.volume + 1);

  // 中键: 单击=播放/暂停 / 双击=音源切换(SD⇄网络) / 长按=进入电台列表
  if (btn.singlePressed(BTN_CENTER)) player.togglePlay();
  if (btn.doublePressed(BTN_CENTER)) sdToggleSource();   // ★双击 = SD 卡 ⇄ 网络收音机 来回切
  if (btn.longPressed(BTN_CENTER))   listToggle();       // 长按 800ms = 进入/退出电台列表
}

// 功放使能(PA_ENABLE): 按 S3 规格 —— 播放且【真正出声】时拉高, 待机/停止/连接中拉低。
// 判据不用 gState.playing(连接成功即置位, 连接中也会是 true), 而用"最近是否真有音频数据在流":
// gState.lastDataMs 由主循环按"音频输入缓冲是否还在增长"推进(见 audio_player.cpp 的 loop()),
// 数据断供 >1.5s 即视为未出声 -> 关功放, 从而覆盖"连接中 / 卡死 / 停止"三类静音场景,
// 且不会在切台间隙出噪。
void updateAmp() {
  // 仅网络收音机: 播放且最近 1.5s 内确有音频数据在流 -> 功放开; 否则静音
  bool sounding = gState.playing && (millis() - gState.lastDataMs < 1500);
  // ★供电刚切换过(muteAmpFor 设的窗口内) -> 保持静音: 压掉 DAC 上电 / MOSFET 通断 / I2C 探测
  //   带来的合流点瞬态, 否则扬声器会"咔哒"一声。用带符号差比较, 天然兼容 millis() 回绕。
  if ((int32_t)(millis() - gAmpMuteUntil) < 0) sounding = false;
  gAmpOn = sounding;
  // 状态变化时打一行日志: 排查"屏显播放中却没有声音"时, 一眼看出是功放没开还是别的原因
  static int8_t lastState = -1;
  if ((int8_t)sounding != lastState) {
    lastState = sounding;
    Serial.printf("[AMP] 功放%s | 播放=%d 距上次音频数据=%ums 音量=%d/%d 缓冲=%u\n",
                  sounding ? "开(出声)" : "关(静音)",
                  gState.playing ? 1 : 0, (unsigned)(millis() - gState.lastDataMs),
                  gState.volume, VOLUME_MAX, (unsigned)player.inBufferFilled());
  }
  digitalWrite(AMP_EN_PIN, sounding ? AMP_EN_ACTIVE : (AMP_EN_ACTIVE == HIGH ? LOW : HIGH));
}

// 电池电压检测(2026-09-28): 两个 100k 串联分压, 中点 = Vbat/2 接 BATT_ADC_PIN(ADC2_CH6)。
// S3 上 ADC2 与 WiFi 不冲突(经典 ESP32 才有 ADC2 被 WiFi 占用的坑), 联网播电台时也能正常采样。
// 转换: raw(0~4095) / 4095 * BATT_VREF = 分压中点电压; * BATT_DIVIDER(=2) = 电池电压; * BATT_CAL 校准。
float readBatteryVoltage() {
  uint32_t sum = 0;
  for (int i = 0; i < BATT_SAMPLES; i++) {
    sum += (uint32_t)analogRead(BATT_ADC_PIN);
    delay(2);
  }
  float raw  = (float)sum / (float)BATT_SAMPLES;
  float vmid = raw / 4095.0f * BATT_VREF;          // 分压中点电压(0~3.3V)
  return vmid * BATT_DIVIDER * BATT_CAL;            // 还原电池电压(V)
}

// 充电状态检测(2026-09-28): CHRG 脚漏极开路, 充电中拉低; INPUT_PULLUP 已上拉。
// 极性由 CHG_ACTIVE_LOW 决定(默认低有效=TP4056 类; 高有效时改 0)。
bool readChargeState() {
  int lv = digitalRead(CHG_STAT_PIN);
  return CHG_ACTIVE_LOW ? (lv == LOW) : (lv == HIGH);
}

// 串口命令 X/Y 的实现: 读一行(等回车, 3s 超时)当作接口地址写进 NVS。
// 直接回车(空串) = 恢复 config.h 里的编译期默认地址。
// ★为什么做成串口而不是网页后台: 网页 app.js 的 gzip 已 4704B / 单次响应安全水位 4800B,
//   只剩 96B —— 再加任何设置项都会超限, 网页后台会重新出现"打不开/一直转圈"。
//   串口命令零体积, 且这个项目本来就有一整套现场调试命令(T/D/P/B/S...)。
static void readWxUrlLine(bool isGeo) {
  Serial.printf("[WX] %s接口地址(直接回车=恢复默认, 3s 内有效): ", isGeo ? "地理" : "天气");
  char     buf[128];
  uint8_t  n = 0;
  bool     done = false;
  const uint32_t t0 = millis();
  while (!done && (millis() - t0) < 3000) {
    const int ch = Serial.read();
    if (ch < 0) { delay(5); continue; }                 // 没数据就短睡, 别空转烧 CPU
    if (ch == '\r' || ch == '\n') done = true;
    else if (n < sizeof(buf) - 1) buf[n++] = (char)ch;
  }
  buf[n] = 0;
  weatherSetUrl(isGeo, buf);
  Serial.printf("[WX] %s接口已设为: %s\n", isGeo ? "地理" : "天气",
                isGeo ? weatherGeoUrl() : weatherFcUrl());
  Serial.println("[WX] 下一次取数(最多 8s 后)即用新地址; 串口 D 可看结果");
}

// 无音频数据超时 -> 重连当前台; 连续多次失败 -> 跳下一台
// 关键: 重连用 reconnect()(不清 failCount), 这样累计失败到 SKIP_AFTER_FAILS 才切下一台。
void updatePlaybackWatchdog() {
  if (!gState.playing) { gState.failCount = 0; return; }
  // ★★SD 本地播放不走这个看门狗(阶段②):
  //   ① 断网期间不计数 —— 对本地文件毫无意义(SD 播放根本不需要网络);
  //   ② "放完一首"在 audio_player::loop() 里已识别为正常结束并自动续播下一首, 不会走到这里;
  //   ③ 若这里继续按"无数据 -> 重连/跳台"处理, 会把 SD 播放误判成断流并跳去网络电台。
  if (gState.sdActive) return;
  if (!gState.wifiConnected) {            // 断网期间不计数(无外网重连无意义), 恢复后再算
    gState.failCount = 0;
    return;
  }
  // 区分"正在建连"(用更长宽限, 防重连风暴) 与 "曾出声后卡死"(用短超时)
  uint32_t to = gState.audioEverFlowed ? RECONNECT_TIMEOUT_MS : CONNECT_GRACE_MS;
  if (millis() - gState.lastDataMs > to) {
    gState.failCount++;
    gState.lastDataMs = millis();         // 给本次重连一个观察窗口, 防止忙等
    if (gState.failCount >= SKIP_AFTER_FAILS) {
      gState.failCount = 0;
      Serial.println("[WD] 连续失败 " + String(SKIP_AFTER_FAILS) + " 次, 跳到下一台");
      player.next();                      // 当前台已确认不行, 换下一台(play() 会重置计数)
    } else {
      Serial.println("[WD] 无音频数据, 重连当前台 (第 " + String(gState.failCount) + " 次)");
      player.reconnect();                 // 同台重连, 不清 failCount
    }
  }
}

// 24h 运行兜底: 内存过低持续过久 -> 主动重启, 避免库内部堆碎片致死机。
// ★判据必须用【内部堆】(MALLOC_CAP_INTERNAL), 不能用 ESP.getFreeHeap():
//   后者把 8MB PSRAM 也算进"空闲堆", 于是 20KB 这个门限在本板上永远不可能被触发 ——
//   兜底形同虚设。而真正会 OOM 的恰恰是内部堆(音频解码器要的连续块只能来自内部堆,
//   任务栈/网络 pbuf 也都在内部堆)。改用内部堆后这个保命机制才真正起作用。
void checkHeapSafety() {
  static uint32_t lowSince = 0;
  const size_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  if (internalFree < 20000) {
    if (lowSince == 0) lowSince = millis();
    else if (millis() - lowSince > 60000) {   // 持续 60s 低于 20KB
      Serial.printf("[Heap] ★内部堆持续过低(剩 %uB), 自动重启以保护 24h 播放\n",
                    (unsigned)internalFree);
      ESP.restart();
    }
  } else {
    lowSince = 0;
  }
}

// 心跳日志: 便于远程确认设备还活着 + 状态/内存/网络
// 【为什么改成 10 秒】"后台打不开"有三种互斥成因, 必须靠这行区分:
//   - 反复出现 "[Main] 复位原因" -> 设备在重启循环(后台当然打不开; 看复位原因判类型)
//   - "运行时间" 归零/不涨          -> 同上(重启)
//   - WiFi=未连/热点AP             -> 网络层问题(IP 变了或在配网热点, 串口里的 IP 才是真的)
//   - HTTP请求= 数字不涨(浏览器正在访问时) -> 请求没到设备(WiFi/路由/lwIP), 与 Web 代码无关
//   - HTTP请求= 在涨但浏览器仍打不开 -> 请求到了设备, 故障在响应侧(看同行的 堆/最大块)
// 判读"内存是否够"必须看【最大块】(最大连续可分配块): 堆总量够但最大块很小 = 碎片化,
// 这才是本项目解码器/响应分配失败的真正形态。
void heartbeat() {
  static uint32_t t = 0;
  if (millis() - t < 10000) return;       // 每 10 秒一次(排障期加密; 稳定后可改回 60000)
  t = millis();
  // 主循环栈余量: 持续下降说明有深层嵌套/大局部数组, 逼近 0 会栈溢出重启
  // ★音频泵状态(方案B 新增): 「泵 0Hz」= 没有任务在喂流 -> 必然没声, 这是远程判断"没声"最快的
  //   判据(比看缓冲更快); 「泵栈余」逼近 0 则迟早栈溢出重启。
  Serial.printf("[HB] 运行%lus | WiFi=%s IP=%s RSSI=%lddBm | 播放=%d 编码=%s %ubps | 堆=%uKB 最大块=%uKB | HTTP请求=%lu WS=%d | 主栈余=%u 音频泵=%luHz 泵栈余=%u\n",
                (unsigned long)(millis() / 1000),
                gState.wifiConnected ? "已连" : (gState.apMode ? "★热点AP" : "★未连"),
                gState.ip.c_str(),
                (long)WiFi.RSSI(),
                gState.playing ? 1 : 0, player.audioCodec(), (unsigned)player.audioBitrate(),
                (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getMaxAllocHeap() / 1024),
                (unsigned long)web.httpReqCount(), web.wsClients(),
                (unsigned)uxTaskGetStackHighWaterMark(nullptr),
                (unsigned long)player.pumpHz, (unsigned)player.pumpStackFree());
}

void updateDisplay() {
  static uint32_t lt = 0;
  // 每圈都跑: 背光计时 + 应用后台下发的对比度/旋转(pending), 不依赖下面的 300ms 限流
  display.tick();

  // 限流 ~16.7fps: 各屏都走硬件外设(ST7567 4MHz / ST7789·ILI9341 20MHz 硬件 SPI, SSD1306 硬件 I2C),
  // 单帧传输 ≤ ~23ms。★2026-09-23: 150->60 —— 跑马灯改成"按时间积分、步长恒 1px"后,
  // 帧率决定的是【顺滑度】而不是速度(速度由 50ms/px 决定), 故把节拍加密到 60ms 消除顿挫。
  // 代价: 1306/7789 各帧约 23ms, 滚动时约占主循环 1/3 时间; 音频喂流是【更高优先级】的泵任务,
  // 会抢占主循环, 故不受影响。限流的目的仍是"别让 UI 把 CPU 吃光、拖慢按键/Web 响应"。
  if (millis() - lt < 60) return;
  lt = millis();

  // ---- SD 文件列表界面(阶段②): 与电台列表同构, 复用 showList 的渲染路径 ----
  if (gSdMode) {
    const int n = gSd.count();
    if (n <= 0) { sdExit(); return; }   // 卡被拔了/文件没了 -> 收起列表
    if (gSdCursor >= n) gSdCursor = n - 1;
    if (gSdCursor < 0)  gSdCursor = 0;
    // 第4个参数 current = 正在播的序号(加 '*' 标记); SD 模式没在播时传 -1。
    display.showList(gSd.names(), n, gSdCursor, gState.sdActive ? gState.sdIndex : -1);
    return;
  }

  // ---- 电台列表界面: 走独立渲染路径(内部自管行数/滚动窗口/整帧跳过) ----
  if (gListMode) {
    // 后台可能增删过电台: 条数变了就重建快照+分类+过滤, 保证"屏上第 N 行"与"确定后播第 N 台"始终对应
    if ((int)gListNames.size() != store.count()) {
      listBuildSnapshot();
      listBuildGroups();
      listApplyGroup();
    }
    if (gListIdx.empty()) { listExit(); return; }   // 列表被后台清空/当前分类已无台 -> 收起
    if (gListCursor >= (int)gListView.size()) gListCursor = (int)gListView.size() - 1;
    if (gListCursor < 0) gListCursor = 0;
    display.showList(gListView.data(), (int)gListView.size(), gListCursor,
                     gState.index,
                     gListGroups.data(), (int)gListGroups.size(), gListGroup);
    return;
  }

  // ---- 仅网络收音机: 第2行传临时提示(有 notice 时)或空串, 第3行传空串码率, 由 display_ui 按 gState 组装 ----
  const bool noticeOn = (gNoticeUntil != 0) && ((int32_t)(millis() - gNoticeUntil) < 0);
  String st, name, br;
  st   = noticeOn ? gNoticeMsg : String();
  name = gState.stationName;
  br   = String();
  display.show(name, st, gState.volume, VOLUME_MAX,
               gState.playing, br);
}
