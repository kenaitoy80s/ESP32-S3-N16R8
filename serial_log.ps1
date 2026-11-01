# 串口日志抓取工具 —— Windows 自带 PowerShell 即可, 不需要 Arduino IDE / Python
# 用法: 双击 serial_log.cmd (或  powershell -File serial_log.ps1 -Port COM5)
# 功能: 把板子串口输出实时显示, 同时保存到本目录 logs\serial_时间.txt; 板子重启/重新插拔会自动重连。
#       在窗口里按键可向板子发送单个字符命令(例如 H = 列出 SD 卡里每个条目, G = 打开列表)。
# 退出: Ctrl+C
param(
  [string]$Port = "",
  [int]$Baud = 115200,     # 本板是 USB 原生串口(HWCDC), 波特率随便填, 不影响
  [switch]$Dtr             # 若连上了却完全没有输出, 再加 -Dtr 试一次(默认不拉 DTR, 避免板子被复位)
)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$utf8 = New-Object System.Text.UTF8Encoding($false)

$dir = Join-Path $PSScriptRoot "logs"
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
$logFile = Join-Path $dir ("serial_{0}.txt" -f (Get-Date -Format "yyyyMMdd_HHmmss"))

# ---- 选串口: 只有一个(排除板载 COM1)就自动选, 多个就让你选 ----
function Select-SerialPort {
  while ($true) {
    $names = @([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object)
    $cand  = @($names | Where-Object { $_ -ne "COM1" })
    if ($cand.Count -eq 1) { return $cand[0] }
    if ($cand.Count -gt 1) {
      Write-Host "检测到多个串口: $($cand -join ', ')"
      $in = Read-Host "请输入要用的串口名(例如 COM5)"
      if ($names -contains $in.Trim().ToUpper()) { return $in.Trim().ToUpper() }
      continue
    }
    Write-Host "没有检测到串口, 请用 USB 线连接板子(要接板上的 USB 口)... 2 秒后重试"
    Start-Sleep -Seconds 2
  }
}

if ([string]::IsNullOrWhiteSpace($Port)) { $Port = Select-SerialPort }
Write-Host "串口: $Port   日志文件: $logFile"
Write-Host "提示: 想看开机时的挂载/扫描日志, 连上后按一下板子的 RESET 键(或重新插拔 USB)。"
Write-Host "-----------------------------------------------------------------"

while ($true) {
  $sp = $null
  try {
    if ([System.IO.Ports.SerialPort]::GetPortNames() -notcontains $Port) {
      Write-Host "[等待串口 $Port 出现...]"
      Start-Sleep -Seconds 1
      continue
    }
    $sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
    $sp.Encoding    = $utf8
    $sp.ReadTimeout = 200
    if ($Dtr) { $sp.DtrEnable = $true }
    $sp.Open()
    Write-Host "[已连接 $Port]"
    while ($sp.IsOpen) {
      $t = $sp.ReadExisting()
      if ($t.Length -gt 0) {
        [Console]::Write($t)
        [System.IO.File]::AppendAllText($logFile, $t, $utf8)
      }
      if ([Console]::KeyAvailable) {
        $k = [Console]::ReadKey($true)
        $sp.Write([string]$k.KeyChar)
      }
      Start-Sleep -Milliseconds 20
    }
  } catch {
    Write-Host "[串口异常: $($_.Exception.Message)]"
  } finally {
    if ($sp) { try { $sp.Close() } catch {} ; $sp.Dispose() }
  }
  Write-Host "[串口已断开或被占用(请关闭 Arduino 串口监视器等其它占用程序), 2 秒后重连...]"
  Start-Sleep -Seconds 2
}
