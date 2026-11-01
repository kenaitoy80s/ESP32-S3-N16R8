// ============================================================================
// sys_monitor —— 监测层: Wi-Fi 信号(RSSI) / ESP32-S3 芯片内部温度 / CPU 频率 / 运行时间
// ----------------------------------------------------------------------------
// 设计原则(与"网台稳定第一"一致):
//   · 只在【主循环/显示线程】里采样, 不进音频泵任务, 不新增任务, 不写 Flash;
//   · 采样限频: 每 SYS_SAMPLE_MS(5 秒)才真正读一次, 其余调用立即返回;
//   · 显示层只读 sysSnapshot() 这一份快照, 不直接调 Wi-Fi/温度接口;
//   · 屏上用的数值做了【平滑 + 滞回量化】(rssiShown/tempShown), 避免数值每秒抖动
//     把"内容没变就整帧跳过"的指纹反复打破、白白多画帧(省下的 CPU 就是音频 DMA 的安全余量)。
// 全部功能都有开关, 置 0 即回到原来的样子(见下面的宏)。
// ============================================================================
#pragma once
#include <Arduino.h>

// ===================== 开关(可直接改这里, 也可用编译参数 -D 覆盖) =====================
#ifndef SYS_MONITOR_ENABLE
#define SYS_MONITOR_ENABLE   1     // 总开关: 0 = 整个监测层不编译、不运行
#endif
#ifndef SYS_SHOW_ON_SCREEN
#define SYS_SHOW_ON_SCREEN   1     // 屏上显示总开关: 0 = 屏幕布局与改动前完全一样(音量条也恢复原长)
#endif
#ifndef SYS_SHOW_RSSI
#define SYS_SHOW_RSSI        1     // 显示 Wi-Fi 信号(格数图标 + dBm 数值)
#endif
#ifndef SYS_SHOW_TEMP
#define SYS_SHOW_TEMP        1     // 显示芯片温度(注意是 S3 芯片内部温度, 不是室温)
#endif
#ifndef SYS_SHOW_UPTIME
#define SYS_SHOW_UPTIME      1     // 显示运行时间(时:分)
#endif
#ifndef SYS_TEMP_ENABLE
#define SYS_TEMP_ENABLE      1     // 是否读取芯片温度。★若编译报 temperatureRead 相关错误, 把它改成 0 即可
#endif
#ifndef SYS_SAMPLE_MS
#define SYS_SAMPLE_MS        5000  // 采样间隔(毫秒)
#endif
#ifndef SYS_LOG_INTERVAL_S
#define SYS_LOG_INTERVAL_S   60    // 串口 [SYS] 日志间隔(秒); 0 = 不打印
#endif

// ===================== 只读快照(显示层/未来的自适应层都读它) =====================
struct SysSnapshot {
  bool     valid;        // 至少采样过一次
  bool     wifiOk;       // Wi-Fi 已连接(STA)
  int      rssiRaw;      // 最近一次 RSSI 读数(dBm), 未连=0
  float    rssiAvg;      // 指数平滑后的 RSSI
  int      rssiShown;    // ★屏显用: 滞回量化后的 RSSI(变化 >=3dBm 才更新), 未连=0
  bool     tempOk;       // 温度读数有效
  float    tempC;        // 芯片温度(°C), 最近一次
  float    tempMax;      // 开机以来最高温度(°C)
  int      tempShown;    // ★屏显用: 整数温度(变化 >=1.5°C 才更新)
  uint16_t cpuMhz;       // 当前 CPU 频率(MHz)
  uint32_t uptimeSec;    // 开机至今(秒), 采样时刻的值
  uint32_t wifiDisc;     // 开机以来 Wi-Fi 断线次数(STA_DISCONNECTED 事件计数)
};

void sysMonitorTick();                          // 主循环/显示里反复调用都行, 内部限频
const SysSnapshot& sysSnapshot();               // 只读快照
void sysFormatUptime(char* out, size_t n);      // 运行时间 "时:分"(如 "3:07"、"123:45"), 用 64 位计时器, 不会 49 天回绕
