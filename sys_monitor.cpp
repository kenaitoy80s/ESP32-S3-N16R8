// sys_monitor.cpp —— 监测层实现(说明见 sys_monitor.h)
#include "sys_monitor.h"
#include <math.h>
#include "esp_timer.h"

// 运行时间格式化: 与监测开关无关, 始终提供(用 esp_timer 的 64 位微秒计时, 不依赖 millis 的 49 天回绕)
void sysFormatUptime(char* out, size_t n) {
  const uint32_t totalMin = (uint32_t)(esp_timer_get_time() / 60000000ULL);
  snprintf(out, n, "%u:%02u", (unsigned)(totalMin / 60), (unsigned)(totalMin % 60));
}

#if SYS_MONITOR_ENABLE

#include <WiFi.h>
#include "../player_state.h"

static SysSnapshot        s_sy = {};          // 对外快照
static bool               s_inited     = false;
static uint32_t           s_lastSample = 0;   // 上次采样时刻(millis)
static uint32_t           s_lastLog    = 0;   // 上次打印 [SYS] 的时刻(millis)
static volatile uint32_t  s_disc       = 0;   // Wi-Fi 断线累计(在 Wi-Fi 事件任务里自增, 主循环里读)
static bool               s_rssiHave   = false;

// Wi-Fi 事件回调: 只数"断线"次数, 不做任何别的事, 不影响原有的重连逻辑(这只是多挂一个监听)
static void onWifiEvt(arduino_event_id_t ev) {
  if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) s_disc++;
}

void sysMonitorTick() {
  const uint32_t now = millis();
  if (s_inited && (uint32_t)(now - s_lastSample) < (uint32_t)SYS_SAMPLE_MS) return;   // 限频: 绝大多数调用在这里就返回
  if (!s_inited) {
    WiFi.onEvent(onWifiEvt);       // 只登记一次
    s_inited = true;
  }
  s_lastSample = now;

  s_sy.uptimeSec = (uint32_t)(esp_timer_get_time() / 1000000ULL);
  s_sy.cpuMhz    = (uint16_t)getCpuFrequencyMhz();
  s_sy.wifiDisc  = s_disc;

  // ---- Wi-Fi 信号: 读最近一次连接的 RSSI(便宜的变量读取); 未连接就清零 ----
  s_sy.wifiOk = gState.wifiConnected;
  if (s_sy.wifiOk) {
    const int r = WiFi.RSSI();
    if (r < 0 && r > -127) {
      s_sy.rssiRaw = r;
      s_sy.rssiAvg = s_rssiHave ? (0.7f * s_sy.rssiAvg + 0.3f * (float)r) : (float)r;   // 指数平滑
      s_rssiHave   = true;
      const int avgI = (int)lroundf(s_sy.rssiAvg);
      // 滞回量化: 与当前显示值差 >=3dBm 才更新显示值 -> 屏上数字不抖, 指纹不会被反复打破
      if (s_sy.rssiShown == 0 || abs(avgI - s_sy.rssiShown) >= 3) s_sy.rssiShown = avgI;
    }
  } else {
    s_rssiHave = false;
    s_sy.rssiRaw = 0;
    s_sy.rssiShown = 0;
  }

  // ---- 芯片内部温度(ESP32-S3 片内传感器, 不是室温) ----
#if SYS_TEMP_ENABLE
  {
    const float t = temperatureRead();
    if (!isnan(t) && t > -40.0f && t < 150.0f) {
      s_sy.tempC = t;
      if (!s_sy.tempOk || t > s_sy.tempMax) s_sy.tempMax = t;
      if (!s_sy.tempOk || fabsf(t - (float)s_sy.tempShown) >= 1.5f) s_sy.tempShown = (int)lroundf(t);
      s_sy.tempOk = true;
    } else {
      s_sy.tempOk = false;
    }
  }
#else
  s_sy.tempOk = false;
#endif
  s_sy.valid = true;

  // ---- 串口 [SYS] 日志(观察几天用; SYS_LOG_INTERVAL_S=0 关闭) ----
  if (SYS_LOG_INTERVAL_S > 0 && (uint32_t)(now - s_lastLog) >= (uint32_t)SYS_LOG_INTERVAL_S * 1000UL) {
    s_lastLog = now;
    const uint32_t m = s_sy.uptimeSec / 60;
    char tb[24];
    if (s_sy.tempOk) snprintf(tb, sizeof(tb), "%.1f(最高%.1f)", s_sy.tempC, s_sy.tempMax);
    else             snprintf(tb, sizeof(tb), "--");
    Serial.printf("[SYS] 运行 %u:%02u | WiFi=%s RSSI=%ddBm(平滑%.0f) | 芯片温度=%s°C | CPU=%uMHz | 断线累计=%lu | 堆=%uKB\n",
                  (unsigned)(m / 60), (unsigned)(m % 60),
                  s_sy.wifiOk ? "已连" : "未连", s_sy.rssiRaw, (double)s_sy.rssiAvg,
                  tb, (unsigned)s_sy.cpuMhz, (unsigned long)s_sy.wifiDisc,
                  (unsigned)(ESP.getFreeHeap() / 1024));
  }
}

const SysSnapshot& sysSnapshot() { return s_sy; }

#else   // SYS_MONITOR_ENABLE == 0: 空实现, 让别处的调用仍能链接通过

void sysMonitorTick() {}
const SysSnapshot& sysSnapshot() { static SysSnapshot z = {}; return z; }

#endif
