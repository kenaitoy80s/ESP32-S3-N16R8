#include "sd_card.h"
#include <vector>
#include <algorithm>   // std::sort
#include <Preferences.h>

// ★只在主循环调用, 见 sd_card.h 的线程模型说明。
#include <SD_MMC.h>

// 1-bit 模式下只接 CLK/CMD/D0 三根线; D1~D3 在 setPins 的 3 参重载里被固定为 -1(不占用)。
// 必须在 begin() 之前调用, 且要在 SPI.begin() 之后 —— 本工程没有初始化 SPI(SD_MMC 不需要)。
SdCard gSd;

bool SdCard::isPlayableExt(const String& filename) {
  // ★★必须只看【最后一个点之后】的部分, 且不能被目录名里的点干扰。
  //   SD_MMC 的 File::name() 返回的是【带目录前缀的完整路径】(如 "/music/我的歌.mp3"),
  //   不是纯文件名 —— 所以这里要:
  //     ① 先截掉最后一个 '/' 之后的部分(纯文件名), 再找扩展名;
  //     ② lastIndexOf('.') 只在文件名里找(目录名带点时不会误判)。
  //   少了 ① 的话, 目录名带点的卡(如 "/my.mp3.folder/歌")会把扩展名判成 ".mp3.folder/歌" 而全部漏掉。
  const int slash = filename.lastIndexOf('/');
  const String base = (slash >= 0) ? filename.substring(slash + 1) : filename;

  int i = base.lastIndexOf('.');
  if (i < 0) return false;                // 没有扩展名 -> 不收
  if (i == 0) return false;               // 隐藏文件(如 ".mp3" 这种点开头) -> 不收
  String e = base.substring(i);           // 含点的扩展名, 如 ".mp3"
  e.toLowerCase();
  // ★白名单只收本板解码器真能解的格式(与 config.h 的 CODEC_ALLOW_* 一致, 且 1-bit SD 上
  //   绝大多数都是 MP3)。刻意不收 .m3u/.pls/.aacp 等: 前两个是播放列表不是音频文件,
  //   .aacp 是 ADTS AAC 裸流(本项目按 radio/aacp 走网络流, 本地裸流兼容性未验证)。
  return e == ".mp3" || e == ".aac" || e == ".m4a" || e == ".flac"
      || e == ".wav"  || e == ".ogg" || e == ".opus";
}

bool SdCard::begin() {
  if (mMounted) return true;

  SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);

  // ★第三个参数 format_if_mount_failed 故意传 false:
  //   挂载失败就报错给用户, 绝不自动格式化 —— 格式化会抹掉卡里的歌, 这个决定必须由用户自己做。
  if (!SD_MMC.begin(SD_MOUNT, /*mode1bit=*/true, /*format_if_mount_failed=*/false)) {
    Serial.printf("[SD  ] 挂载失败: 请检查接线(CLK=GPIO%d CMD=GPIO%d D0=GPIO%d)与 FAT32 格式\n",
                  SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);
    return false;
  }
  mMounted = true;
  // 卡容量: cardSize() 返回 MB。顺带打印已用/总量, 让"卡认出来了"这件事在串口上一眼可见。
  // ★core 3.3.11 的 SDMMCFS 只有 cardSize()/totalBytes()/usedBytes(),
  //   【没有】totalFiles() —— 文件数要自己遍历数(见 scan())。
  //
  // ★★顺手打印卡型/文件系统: 用户报"卡里有文件却一个都扫不到"时, exFAT 是最常见的元凶 ——
  //   FFat 驱动虽然支持 exFAT, 但个别 exFAT 卡的目录项兼容性有问题, 表现就是
  //   "挂载成功(容量读得出来)但列不出文件"。打印出来一眼就能判断要不要先格成 FAT32。
  const char* ct = "未知";
  switch (SD_MMC.cardType()) {
    case CARD_MMC:  ct = "MMC";  break;
    case CARD_SD:   ct = "SD";   break;
    case CARD_SDHC: ct = "SDHC"; break;
    default: break;
  }
  Serial.printf("[SD  ] 已挂载(1-bit): 卡类型 %s, 卡容量 %llu MB, 已用 %llu MB / 共 %llu MB\n",
                ct,
                (unsigned long long)SD_MMC.cardSize(),
                (unsigned long long)(SD_MMC.usedBytes() / (1024ULL * 1024ULL)),
                (unsigned long long)(SD_MMC.totalBytes() / (1024ULL * 1024ULL)));
  return true;
}

void SdCard::end() {
  if (!mMounted) return;
  SD_MMC.end();
  mMounted = false;
  mFiles.clear();
  mFiles.shrink_to_fit();
  mNames.clear();
  mNames.shrink_to_fit();
  Serial.println("[SD  ] 已卸载");
}

// ---- 上次播放位置记忆(2026-10-07) ----
// 只存序号, 不存文件名 —— 见 sd_card.h 的理由。
#define SD_NVS_NS "sd"
#define SD_NVS_KEY_LAST "last"

void SdCard::loadLast() {
  Preferences p;
  // ★只读不写, 用 ro=true: 万一 NVS 分区还没初始化, 不会顺手创建/擦写它。
  //   (显示设置那边用 read-write 读, 是因为它随后就要写。)
  if (p.begin(SD_NVS_NS, true)) {
    mLastIndex = p.getInt(SD_NVS_KEY_LAST, -1);
    p.end();
  }
  Serial.printf("[SD  ] 记忆的播放位置: %s\n",
                mLastIndex >= 0 ? (String(mLastIndex + 1) + " 号").c_str() : "无(首次使用)");
}

void SdCard::saveLast() {
  // 去重: 同一首反复切换时不重复写 NVS —— putInt 会触发一次 flash 擦写,
  // 而"切来切去播同一首"是很常见的操作, 不去重等于每次都擦一下 flash。
  if (mLastIndex == mSavedIndex) return;
  Preferences p;
  if (p.begin(SD_NVS_NS, false)) {
    p.putInt(SD_NVS_KEY_LAST, mLastIndex);
    p.end();
    mSavedIndex = mLastIndex;
    Serial.printf("[SD  ] 已记忆播放位置 -> %d 号\n", mLastIndex + 1);
  }
}

int SdCard::resolveLast() const {
  const int n = (int)mFiles.size();
  if (n <= 0) return -1;                       // 卡里没文件
  if (mLastIndex < 0) return 0;                // 没记忆 -> 从第一首开始(合理默认)
  if (mLastIndex >= n) return n - 1;           // 记忆越界(换了卡/删过文件) -> 钳到最后一首
  return mLastIndex;
}

// ★★★ 路径前缀: 【必须用 "/" 而不是 SD_MOUNT("/sdcard")】★★★
//   这是本模块最容易踩的坑, 症状是"挂载成功(容量都读得出来)但打开根目录失败"。
//   原因: SD_MMC.begin("/sdcard", ...) 里的 "/sdcard" 只是给 esp_vfs_fat_sdmmc_mount()
//   注册挂载点用的【标签】, 而 arduino-esp32 3.x 的 FS 层(VFSImpl::open, vfs_api.cpp:25)
//   是把调用者给的路径【原样】丢给 VFS 的, 【不会自动拼接挂载点前缀】——
//   即挂载后根目录就是 "/", 去访问 "/sdcard" 相当于访问一个不存在的子目录, 必然失败。
//   (这与老的 SD_MMC 2.x 不同: 老版本 FS 对象自己持有挂载点路径, 能直接 open("/sdcard")。
//    core 3.3.11 换成 VFS 实现后语义变了 —— 旧写法照搬过来就会报"打开根目录失败"。)
//   同理, scan() 里 openNextFile() 返回的 name() 已是 "/xxx.mp3" 这种【不带挂载点】的路径,
//   可以直接交给 audio.connecttoFS() 打开, 无需再做任何拼接/裁剪。
#define SD_ROOT_PATH "/"

int SdCard::scan() {
  mFiles.clear();
  mNames.clear();
  if (!mMounted) return -1;

  File root = SD_MMC.open(SD_ROOT_PATH);   // ★ "/", 不是 SD_MOUNT
  if (!root || !root.isDirectory()) {
    // 走到这里说明"卡挂载成功但目录打不开" —— 最常见就是路径前缀写错(见上面的宏),
    // 其次才是卡本身损坏/文件系统异常。两种都打印, 便于现场分辨。
    Serial.printf("[SD  ] 打开根目录失败 (路径=%s): 请确认这是 core 3.x 的 VFS 路径语义(\"/\"),"
                  "以及卡是否已损坏\n", SD_ROOT_PATH);
    if (root) root.close();
    return -1;
  }

  // 平铺扫描根目录(不递归子目录)。mFiles 先收集, 最后再排序 ——
  //   FAT 的目录项顺序是物理写入顺序, 几乎总是"创建顺序", 对用户没有意义;
  //   排序后才能保证"屏上第 N 行"与"按 next 播放的下一首"始终一致。
  //
  // ★★诊断(用户报"卡里有文件但提示找不到音频"时, 这一段就是答案):
  //   把扫到的【每一个】目录项都打出来 —— 哪怕扩展名不认、哪怕它在子目录里。
  //   以前只打印"认到的几个", 一旦结果是 0, 串口上什么线索都没有, 只能靠猜;
  //   现在即使一个都没匹配上, 也能从这行看出到底是
  //     ①卡里确实是空的 / ②文件在子目录里(当前只扫根目录) / ③扩展名不在白名单 / ④中文名乱码。
  int totalEntries = 0, totalDirs = 0, playable = 0;
  File f;
  while (true) {
    f = root.openNextFile();
    if (!f) break;                       // 扫完了
    totalEntries++;
    if (f.isDirectory()) {
      totalDirs++;
      // 子目录也打出来: 用户最常见的误会就是"文件放进了文件夹里"
      Serial.printf("        [目录] %s/\n", f.name());
      f.close();
      continue;
    }
    const String full = f.name();        // ★带目录前缀的完整路径, 如 "/abc.mp3"
    if (isPlayableExt(full)) playable++;
    // 前 25 个条目无论是否匹配都打印(够用又不刷屏)
    if (totalEntries <= 25)
      Serial.printf("        [文件] %s  (%s)\n", full.c_str(),
                    isPlayableExt(full) ? "可播" : "不识别扩展名");
    f.close();
    if (!isPlayableExt(full)) continue;

    SdAudioFile a;
    // 去掉扩展名做显示名: 屏幕只有十几个字符宽, ".mp3" 纯属浪费; path 保留原样。
    // ★先截掉目录前缀再找点, 否则目录名带点时会把点找错位置(见 isPlayableExt 的注释)。
    int slash = full.lastIndexOf('/');
    const String base = (slash >= 0) ? full.substring(slash + 1) : full;
    int dot = base.lastIndexOf('.');
    a.name = (dot > 0) ? base.substring(0, dot) : base;
    a.path = full;
    mFiles.push_back(std::move(a));
  }
  root.close();

  //★诊断汇总行: 把"扫到几个条目 / 几个目录 / 几个可播"一次说清。
  //   判读: totalEntries>0 但 playable==0 -> 看上面逐条打出的扩展名是什么(多半是白名单没有的格式)。
  //         totalEntries==0 -> 卡是空的, 或根目录打不开(不是"格式不对", 格式不对会挂载失败)。
  Serial.printf("[SD  ] 目录项共 %d 个(其中目录 %d 个), 识别为可播音频 %d 个\n",
                totalEntries, totalDirs, playable);
  if (totalEntries > 0 && playable == 0)
    Serial.println("[SD  ] ★卡里有条目但一个都不匹配 —— 请看上面逐条扩展名; "
                   "注意只扫【卡根目录】, 放进文件夹里的不会被扫到");

  // 按显示名自然序(数字按数值比, 所以 song2 < song10)
  std::sort(mFiles.begin(), mFiles.end(),
            [](const SdAudioFile& x, const SdAudioFile& y) {
              return strcasecmp(x.name.c_str(), y.name.c_str()) < 0;
            });

  // ★排序【之后】再重建扁平显示名数组 —— 若在排序前填, 两者顺序会不一致,
  //   表现为"屏上第 N 行的字"与"按确定播放的文件"对不上(这类错最难查, 必须从根上避免)。
  mNames.reserve(mFiles.size());
  for (const auto& f : mFiles) mNames.push_back(f.name);

  Serial.printf("[SD  ] 扫描到 %d 个音频文件\n", (int)mFiles.size());
  for (int i = 0; i < (int)mFiles.size() && i < 12; i++)
    Serial.printf("        %2d. %s\n", i + 1, mFiles[i].name.c_str());
  if ((int)mFiles.size() > 12) Serial.printf("        ... 共 %d 个\n", (int)mFiles.size());
  return (int)mFiles.size();
}