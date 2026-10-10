# 进度条卡在 50% —— 复现与根因（2026-10-08 追加）

设备：emulator-5554（sdk_gphone64_x86_64，1080×2400，density 2.625，深色主题）。
本报告为只读排查 + 模拟器复现，**未改任何源码**。
起因：实机反馈「200 帧会卡在 50 进度」。

## 结论（一句话）

输入**带音轨**时，native 在视频解码中途上报了一条**硬编码 50%** 的「audio 阶段」占位值
（`app/src/main/cpp/transcode.cpp:2564`、`:2892`、`:2913` 的 `vp.rep(50, "audio", vp.frames);`），
而 `app/src/main/java/com/proresforge/app/engine/TranscodeService.kt:585-588` 又把百分比单调化
（`maxPct = maxOf(maxPct, pct.coerceIn(0, 100))`）⇒ 显示值 = max(50, decode 曲线)。
decode 曲线是全程 0→100，所以**前半程一直被 50 钉住**，过了半数帧才开始爬升 —— 200 帧就是「卡在 50%」。

## 一、复现（两次独立运行，结果一致）

素材：宿主 ffmpeg 8.1.2 生成 `t200a.mp4` = **1920×1080 / 25 fps / 200 帧 / 8 s / H.264 + AAC 48 kHz 单声道**，
push 到 `/sdcard/Download/t200a.mp4`。

```powershell
ffmpeg -y -f lavfi -i "testsrc2=size=1920x1080:rate=25:duration=8" `
       -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=8" `
       -c:v libx264 -preset veryfast -pix_fmt yuv420p -c:a aac -b:a 128k -shortest t200a.mp4
adb push t200a.mp4 /sdcard/Download/
```

步骤：Home → Pick files → 选 `t200a.mp4` → Start（输出 MP4 · H.264 / High / Original + 25）→ Jobs 页，
设备侧循环 `uiautomator dump /sdcard/q$i.xml`（约 1 s 一次）。

**运行 #1**（04:08:09 起，总耗时 0:24，5.1 MB → 2.5 MB）：

| 采样 | 卡片文案 | 百分比 |
|---|---|---|
| 1 | `Transcoding · 2/200` | **50%** |
| 2 | `Transcoding · 21/200` | **50%** |
| 3 | `Transcoding · 43/200` | **50%** |
| 4 | `Transcoding · 58/200` | **50%** |
| 5 | `Transcoding · 73/200` | **50%** |
| 6 | `Transcoding · 87/200` | **50%** |
| 7 | `Transcoding · 102/200` | **50%** |
| 8 | `Transcoding · 119/200` | 59% |
| 9 | `Transcoding · 137/200` | 68% |
| 10 | `Transcoding · 159/200` | 79% |
| 11 | `Transcoding · 181/200` | 90% |
| 12 | `Done` | — |

**运行 #2**（04:11:07 起，0:19，5.1 MB → 2.7 MB）：`53/200 → 50%`；`181/200 → 90%`；`Done`。

要点：**第 2 帧就已经是 50%**（第一条音频包几乎立刻到达），一直钉到 ~102/200 才开始爬升；
爬升后百分比正好 = frame/total。

## 二、对照：无音轨素材没有 50% 平台

| 素材 | 采样 |
|---|---|
| 375 帧 H.264 **无音轨**（`long30.mp4`，上一轮） | 91/375 → 24%、256/375 → 68%、363/375 → 96% |
| DNG 序列 zip 4 帧（无音轨，上一轮） | 2/4 → 0%、3/4 → 25%、4/4 → 50%、4/4 → 100% |

⇒ 触发条件是**输入有音频流**，与帧数无关；帧数只决定这个平台持续多久（前半程）。

## 三、根因（4 条）

1. **三处硬编码占位值**（`app/src/main/cpp/transcode.cpp`）：

   全文共三处，都是 `if (!audio_reported) { audio_reported = true; vp.rep(50, "audio", vp.frames); }`：
   `:2562-2565`（读包分发 lambda，音频 copy 分支 `pump_audio_dec` 之后）、
   `:2890-2893`（音频 **copy** 分支，`av_interleaved_write_frame` 之后）、
   `:2911-2914`（音频 **AAC 重编码** 分支，`adec send / pump` 之后）。
   由 `audio_reported` 守卫 ⇒ 每作业只发一次。音频包与视频包是**交错**解复用的，
   所以这条「audio 阶段」上报落在**视频解码中途**（实测第 2 帧），并不是真的阶段切换。

2. **百分比被单调化**（`app/src/main/java/com/proresforge/app/engine/TranscodeService.kt:578-588`）：

```kotlin
// 单调化：audio/finalize 是固定占位值，decode 曲线与它们交错，
// 直接透传会让进度条 100→50→0 来回跳；这里只进不退。
maxPct = maxOf(maxPct, pct.coerceIn(0, 100))
val c = maxPct
```

   注释本身承认 audio/finalize 是「固定占位值」。单调化挡住了回跳，却也把 50 变成了**地板**。

3. **decode 曲线的定义**（`app/src/main/cpp/transcode.cpp:1344-1362`）：
   `pct = 已输出帧 pts / duration_us`（DNG zip 走 `frame_count / fps`，普通容器用 `nb_frames` / duration），
   整段 0→100 ⇒ 显示 = max(50, 0→100) = 前半程恒定 50%。

4. **为什么纯视频 / DNG zip 不受影响**：这三处都在 `si == astream` 的音频分支里，
   没有音频流就永不触发（与 §二 实测一致）。

## 四、附带影响

- **前台通知与卡片同值**：`app/src/main/java/com/proresforge/app/engine/TranscodeService.kt:589-599`
  把同一个 `c` 交给 `JobBus.progress(id, c, stage, frame, totalFrames)` 与
  `Notifications.progress(...)` ⇒ 通知栏同样钉在 `Transcoding 50%`。
- 与上一份《进度条问题检查（2026-10-08）》一致的其它毛病：百分比取 **sink pts**、帧计数取 **push 计数**
  （实测差 1–2 帧）；`rep(0, "finalize")` 被 max 吞掉；100% 出现在 moov 写完之前；
  「排队中」被画成确定的 0% 空条。

## 五、建议修复

**A（最小、推荐）** —— `app/src/main/cpp/transcode.cpp:2564`、`:2892`、`:2913` 三行：

```cpp
vp.rep(50, "audio", vp.frames);   →   vp.rep(-1, "audio", vp.frames);
```

- `pct = -1` 是这条管线既有的「无有效进度」约定（见 `:1350` 的兜底判断与 `:3235 vp.rep(-1, "error", ...)`）。
- `app/src/main/java/com/proresforge/app/engine/JobBus.kt:190-203` 会把 -1 夹到 0，
  于是 `maxPct = maxOf(maxPct, 0)` 保持不变 ⇒ 界面继续显示上一次 decode 的真实百分比，
  只是 stage 会短暂变成 `Audio`；50% 地板消失。（如按 C 改，-1 会原样透传成不确定条。）
- 若确实想让 audio 阶段也有真实进度：得改成按音频 pts 计算，并把 decode 曲线压到 0–90%、
  audio/finalize 占 90–100%（改动较大，不推荐现在做）。

**B（可选，治「100% 未完成」）**：非终态阶段把显示值封顶 99%，只有 `done` 才允许 100%。

**C（可选，治「排队 0% 空条」）**：排队态 `pct = -1` 原样透传（现在被
`app/src/main/java/com/proresforge/app/engine/JobBus.kt:66`、`:173` 的 `pct = 0` 夹成 0），
`app/src/main/java/com/proresforge/app/ui/components.kt:154-195` 的不确定分支才能生效；
同时把 `app/src/main/res/values-en/strings.xml:100` 的 `progress_indeterminate`（当前是「…」）换成可读文案。

## 六、顺带发现（新，已复现）：入队成功的 Snackbar 永不消失

- `app/src/main/java/com/proresforge/app/ui/HomeScreen.kt:142-152`：
  `snackbarHostState.showSnackbar(msg, actionLabel = viewJobsLabel)` —— Material3 的默认规则是
  **带 `actionLabel` 时 duration = Indefinite**，这条 `Queued 1 job(s)` + `View jobs` 不会自动消失；
  且 `vm.consumeMessage()` 只在它 resolve 之后才调用（`:151`）⇒ `state.message` 一直非空，
  **每次回到 Transcode 页都会重新弹出**。
- 实测：该提示在作业完成 4 分钟后仍在屏幕上（`Queued 1 job(s)` bounds [116,1963][380,2016]、
  `View jobs` bounds [794,1963][953,2016]），切到 Jobs 再切回仍在。
- 修法：显式 `duration = SnackbarDuration.Long`（或 Indefinite + `withDismissAction = true`），
  并在送出提示后立即 `consumeMessage()`。

## 七、复现配方

```powershell
# 宿主机：造素材（见 §一），推入设备
& $adb push t200a.mp4 /sdcard/Download/
# 应用内：Pick files → t200a.mp4 → Start → 切 Jobs 页
& $adb shell 'force-stop com.proresforge.app; am start -n com.proresforge.app/.MainActivity'
& $adb shell 'for i in 1 2 3 4 5 6 7 8 9 10; do uiautomator dump /sdcard/q$i.xml >/dev/null 2>&1; done'
& $adb pull /sdcard/q3.xml .
```

解析（注意：设备侧 `grep | tr | cut` 长管道在该模拟器上会 `Segmentation fault`；一律拉回主机解析）：

```powershell
$d = New-Object System.Xml.XmlDocument
$d.Load($path)
$d.SelectNodes("//*[@text!='']") | ForEach-Object { $_.GetAttribute('text') + '  ' + $_.GetAttribute('bounds') }
```

## 八、未做 / 未验证

- 未改任何源码；A / B / C 均未实施，等确认。
- 真机（用户设备）未复测；native 改动需要重新 `assembleDebug` 并装机。
- 「无音轨素材不出现 50% 平台」只在模拟器上对照过，真机未验。
- 运行中的**通知栏**实时样本没采到（该模拟器上 `dumpsys notification --noredact > file` 与长管道会把脚本打断）；
  不过通知与卡片同源（同一个 `c`），卡片钉住即通知钉住。

## 九、设备善后

- Jobs 页两张 Done 卡已用 `Clear finished` 清掉（页面现为 `No transcode jobs`）；格式复位为 `MP4 · H.264`。
- `/sdcard/Download/t200a.mp4` 与 `/sdcard/Movies/t200a_PF_h264.mp4`、`t200a_PF_h264 (2).mp4` 保留作复现素材。
- 之前的临时 dump（ui*/q*/p*/f*/g*/c*/h*/s*/n* 等）与 notif*.txt 待清理。
## 十、修复与验证（2026-10-08 12:18，已实施）

### 改动

1. **A（核心，3 行 native）** —— `app/src/main/cpp/transcode.cpp` 的 `:2564`、`:2892`、`:2913`：

```cpp
vp.rep(50, "audio", vp.frames);   →   vp.rep(-1, "audio", vp.frames);
```

   `-1` 沿用它在这条管线里的既有含义（无有效进度）；`app/src/main/java/com/proresforge/app/engine/JobBus.kt:190-203`
   会把它夹到 0，`maxPct` 不变 ⇒ 50% 地板消失，界面保留上一次 decode 的真实百分比。

2. **B（终态封顶）** —— `app/src/main/java/com/proresforge/app/engine/TranscodeService.kt:576-591`：

```kotlin
// 终态封顶：decode/audio 也会报 100（早于收尾写盘），只有 done 允许满格
val shown = if (stage == "done") pct.coerceIn(0, 100) else pct.coerceIn(0, 99)
```

   `worthIt` / `lastPct` / `maxPct` 全部改用 `shown` ⇒ 只有 native 的 `done` 阶段能报 100%。

### 构建与安装

- `P:\gradlew.bat :app:assembleDebug` → **BUILD SUCCESSFUL in 1m39s**（native 重新编译：mergeDebugNativeLibs / stripDebugDebugSymbols / packageDebug；唯一 warning 是既有的 `transcode.cpp:622 av_stream_get_side_data is deprecated`）。
- APK：`app/build/outputs/apk/debug/app-debug.apk`，2026-10-08 12:18:14，62,342,722 B，
  SHA256 `B67EAC39DAA1ABD6873A49E0E0D51F80BC146C07C05BA3D4B30D419DEF1AFA59`；`adb install -r` 到 emulator-5554 → Success。

### 验证 1：200 帧 + 音轨（修复前的症状素材）

`t200a.mp4`（1920×1080 / 25 fps / 200 帧 / H.264 + AAC），输出 MP4 · H.264，Jobs 页逐秒采样：

| 采样 | 修复前 | 修复后 |
|---|---|---|
| 1 | `2/200` + **50%** | `21/200` + **10%** |
| 2 | `21/200` + 50% | `78/200` + 38% |
| 3 | `43/200` + 50% | `189/200` + 94% |
| … | 一直 50% 到 102/200 才开始爬 | 10 → 17 → 24 → 32 → 38 → 45 → 53 → 60 → 67 → 74 → 80 → 87 → 94% 单调爬升 |
| 末 | — | 94% → `Done`（0:32，5.1 MB → 2.3 MB） |

百分比与帧计数一致（21/200 = 10.5%、78/200 = 39%、189/200 = 94.5%）⇒ 50% 地板消失。

### 验证 2：4 帧 DNG zip（验证 B 的封顶）

| 采样 | 卡片文案 |
|---|---|
| r1–r3 | `Preparing · -` + 0% |
| r4 | `Transcoding · 3/4` + 25% |
| r5–r6 | `Transcoding · 4/4` + 50% |
| **r7** | `Transcoding · 4/4` + **99%**，卡片仍是 `Running · current job`（修复前这里会是 100%） |
| r8 | 卡片消失 → `Done`（0:17，60.0 MB → 762.1 KB） |

⇒ 收尾阶段不再提前报满格；100% 只出现在 JobBus 置 DONE 之后（那时卡片已是 Done 分支，不再画进度条）。

### 仍未修（另案，未获授权）

- **短素材的「帧计数领先、百分比滞后」**（上表 r4 的 `3/4` + 25%、r5 的 `4/4` + 50%）：pts（已输出帧）与 push 计数不同源，
  需要给 `Pipe::total` 赋值或改成同源公式 —— 即上一份报告的建议 A，本轮未动。
- 排队态 0% 空条（建议 C）。
- 入队 Snackbar 永不消失（`app/src/main/java/com/proresforge/app/ui/HomeScreen.kt:142-152`）。
- 真机（用户设备）未复测。
### 交付产物

- `dist/ProResForge-1.0.0-debug.apk` 已替换为本次构建（2026-10-08 12:18:14，62,342,722 B，
  SHA256 `B67EAC39DAA1ABD6873A49E0E0D51F80BC146C07C05BA3D4B30D419DEF1AFA59`）。
  被替换的旧调试包：2026-10-06 22:48:17，64,009,808 B，SHA256 `F2692CFF925FFDC969AAA5E48711AC3051D772C28F53B944AB6A965F0FA421CF`。