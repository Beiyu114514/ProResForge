# ProResForge 第 9 轮代码审查报告

- **审查日期**:2026-10-07(Asia/Shanghai)
- **审查方式**:纯静态只读审查(read / grep / glob / 文件哈希实测 + 上游官网版本核对)。**未修改任何源码,未运行 gradle 构建、未运行测试、未运行设备端验证。**
- **审查范围**:一级源码 = 10 个 C/C++ 文件(`app/src/main/cpp/`)+ 23 个 Kotlin 文件(`app/src/main/java/com/proresforge/app/`),外加构建脚本(`scripts/`、`app/build.gradle.kts`、`CMakeLists.txt`)、prebuilt 目录 provenance、`docs/` 契约文档一致性、测试覆盖。
- **与既往报告的关系**:本报告为第 9 轮增量。沿用 2026-10-06 至 2026-10-07 既有编号体系(B2/b3 基线已固化于会话记忆)。编号规则:S = 阻断发布/合规/数据损坏级,M = 应尽快修,L = 建议修。带「N-」前缀为 native 层,「K-」为 Kotlin/Android 层,「B-」为构建/供应链层,P2-nn 为跨轮遗留编号。
- **产出文件**:本文件(`docs/REVIEW-REPORT-2026-10-07-ROUND9.md`),未触碰任何其它文件。

---

## 0. 结论速览

### 0.1 总体判断

项目工程质量**显著高于平均水平**:上轮(第 8 轮)标记的 M-3(选取竞态)、M-5(busy 卡死)、P2-03(finish 终态守卫)、JobSpec 契约、allowBackup 等项经本轮逐行复核**均已真实闭环**,证据见 §5。native 主链路(31 处 `avcodec_send/receive` + 3 处 bitstream filter 调用点的错误分层)在第 8 轮后已彻底重写为 EAGAIN 泵送 / EOF 收工 / 真错误跳转的统一状态机,这是本轮最大的正面变化。

剩余风险集中在**四处收尾语义**(并发取消窗口、Service 生命周期、历史落盘、SAF 空文件),而非转码算法本身。

### 0.2 问题统计

| 严重度 | 数量 | 编号 |
|---|---|---|
| **S(阻断/合规/数据损坏)** | 3 | S-1 产物与 pin 不符、S-2 无许可文本随包、S-3 历史静默清空 |
| **M(应尽快修)** | 18 | K-01…K-06、P2-06、N-01…N-03、B-03…B-09、B-16 |
| **L(建议修)** | 24 | K-07…K-13、L-06、L-17、L-新B、N-04…N-10、B-10…B-15、B-17 |
| 文档 nit | 2 | strspec.h:38 注释、docs/ 计数 |

### 0.3 修复优先级排序(按性价比,非按严重度)

| 序 | 动作 | 预计工作量 | 收益 |
|---|---|---|---|
| 1 | K-01 `clearPendingCancel()` 补在 `begin()` 后 | 1 行 | 消掉「取消即误删 + 下一任务连带失败」的级联 |
| 2 | N-01/N-02 preview.cpp send/receive 分层 | ~20 行 | 消掉截断 JPEG 被判成功 |
| 3 | S-3 JobStore 坏档备份 + 日志 | ~6 行 | 止住「枚举改个名 → 用户历史全没」 |
| 4 | S-1/S-2 刷新 BUILDINFO + 随包许可资产 | 半天 | GPLv3 源码 offer 与产物对齐 |
| 5 | K-03 `startForeground` 加 runCatching | 3 行 | 消掉 Android 15 配额 FATAL |
| 6 | §8.1 契约修正 `crfQuality`→`crfHigh`、`vp9Cq`→三档 | 文档 2 行 | 契约从「照抄编译不过」回到可用 |
| 7 | L-17 改 1 行接 `media_hdr_hint` 资源 | 1 行 | 本地化漏网清零 |
| 8 | K-04/K-05/K-06/JobBus 通知与补录 | 各 ~10 行 | 收尾语义收口 |
| 9 | 补 3 个纯 JVM 测试类(buildSpec / JobBus / jobs.json 兼容) | 半天 | 覆盖当前 0 覆盖的三个高风险区 |

---

## 1. 核查基线(可复核事实)

### 1.1 源码文件与行数(实测)

| 层 | 文件 | 行数 |
|---|---|---|
| native | `app/src/main/cpp/transcode.cpp` | 2928 |
| native | `app/src/main/cpp/dngzip.cpp` | 985 |
| native | `app/src/main/cpp/dngframeq.h` | 464 |
| native | `app/src/main/cpp/native-lib.cpp` | 662 |
| native | `app/src/main/cpp/preview.cpp` | 493 |
| native | `app/src/main/cpp/strspec.h` | 319 |
| native | `app/src/main/cpp/transcode.h` | 126 |
| native | `app/src/main/cpp/dngzip.h` | 126 |
| native | `app/src/main/cpp/CMakeLists.txt` | 273 |
| native | `app/src/main/cpp/preview.h` | 34 |

- **TODO/FIXME/HACK 全仓 178 命中,100% 位于 prebuilt 的 FFmpeg/vpx 头文件**;上述 10 个一级 C++ 文件与全部 23 个 Kotlin 文件**零命中** → 无遗留标注型技术债。
- `app/src/test/` 仅 3 个 Kotlin 测试类:`FormatCatalogTest`(12 例)、`HistoryItemSerializationTest`(4 例)、`DngSpecMappingTest`(6 例),合计 **22 个 `@Test`**(grep 实测)。**无 `androidTest/`、无 native 测试、无 `.github/workflows/`。**
- `docs/` 共 **19** 份 md(既往报告误记为 21,本轮 glob 更正)。

### 1.2 关键 SHA 实测(本轮由主代理本机计算)

| 对象 | 实测值 | 对照 |
|---|---|---|
| `scripts/build-ffmpeg-android.sh` | `8339285ff2c739e5ce81dbb760192b982921dc2bedc3fe879411814a1925e8bb` | 与 `libraw-manifest-arm64-v8a:12` 一致;与 `BUILDINFO:5` 记的 `8fd70f1e…` **不一致** |
| `prebuilt/arm64-v8a/lib/libraw.a` | `566fc1edd6fa85b506f3747f8c7855b62d7ef966a94a952e74ed89dd70f486b6` | 与 manifest `:13` 一致 |
| `prebuilt/armeabi-v7a/lib/libraw.a` | `7013419593ded6f698a1045fa6243d70d0797e9f24d6fdf9910f1eba1e5235c0` | 与 manifest 一致 |

### 1.3 APK 清单实测

仓库内共 **14 个 APK,约 824 MB**,其中 12 个为 debug;`dist/` 树(含 `dist/obsolete/`)10 个约 590 MB **全为 debug、0 个 release**(含两份 SHA 前缀同为 `f2692cff` 的 61.04 MB 重复副本);根目录 1 个 57.65 MB debug + 2 个失控脚本重定向产生的乱码名文件;`tmp/prf-current.apk` 61.06 MB(第三个未跟踪构建,sha `3ca9f376…`);`app/build/outputs/apk/` 下 debug 61.16 MB(sha `b454499e`)、`app-release-unsigned.apk` 52.20 MB。release APK 解包 942 条目,`lib/` 仅 18 项(7 个 `.so` × 2 ABI + 其它),**无冗余 `.so`**(白名单同步机制有效)。

---

## 2. S 级问题(3 项)

### S-1 prebuilt 产物与文档/脚本 pin 不符 — GPLv3 源码 offer 落空【新增,本轮最重】

**证据**(除标注外均为主代理本轮亲读/实测):
- 产物头文件版本:`app/src/main/cpp/prebuilt/arm64-v8a/include/libavcodec/version.h:32-33` = `60.31.102`(armeabi-v7a 相同),对应 FFmpeg 6.1.6 线(ffmpeg.org/download.html:6.1.6 "Heaviside"(2026-06-20),libavcodec = 60.31.102)。
- 脚本与文档 pin:`scripts/build-ffmpeg-android.sh:62` `FFMPEG_TAG="${FFMPEG_TAG:-n6.1.1}"`(注释还写着「libavcodec 60.31.100」)、`docs/INTERFACES.md:161` 同写 60.31.100、`prebuilt/README.md:149` 同。
- `prebuilt/BUILDINFO:10` 记 `FFmpeg tag n6.1.1 @ d4ff0020b40b524a490cf62eccbd3a318f4c0e58`,与产物版本(6.1.6 线)矛盾。据构建子代理核对上游,n6.1.1 的真实 commit 为 `e38092ef9395d7049f871ef4d5411eb410e283e0`(annotated tag `6f4048827982a8f48f71f551a6e1ed2362816eec`,2023-12-31),即 BUILDINFO 的 commit **既对不上产物版本,也对不上它自己声称的 tag**。→ 该 commit 级结论建议下次有网络时按上游 tag 对象最终确认(见附录 C)。
- 同类矛盾共四处:`BUILDINFO:11` x264 `0480cb05…` vs 脚本 pin `b35605ace3ddf7c1a5d67a2eb553f034aef41d55`(`build-ffmpeg-android.sh:64`);`:12` x265 `b81f650e` vs `:66` `1d117bed…`;`:13` libvpx `0a6f769e` vs `:68` `9f9b7e9b…`;**`prebuilt/arm64-v8a/lib/pkgconfig/vpx.pc:9` `Version: 1.17.0` vs pin v1.15.0**(主代理本轮实测)。仅 `:15` 的 LibRaw-cmake `eb98e43` 与脚本 pin(`:69`)一致 → BUILDINFO 生成于改 pin 之前的旧脚本。
- 其它佐证:实测 `include/libavutil/ffversion.h` 全文仅 5 行(`#define FFMPEG_VERSION "n6.1"`),**无 GIT_VERSION** → 二进制无法反推 rev;头文件侧另有 `X264_BUILD 165`(`include/x264.h:48`)、`X265_BUILD 216`(`include/x265_config.h:32`)、libavformat 60.16.100 / libavutil 58.29.100,与 libavcodec 60.31.102 同属一次构建产物。
- `prebuilt/` 被 `.gitignore:4` 忽略,当前副本又无 `.git`/`.gitmodules` → 无 git 历史可作旁证。
- **合规后果**:`README.md:163-165` 在 `--enable-gpl --enable-version3` 下承诺开放「对应 native 源码」,当前既指不出对应 commit,BUILDINFO 与 manifest 又互相矛盾(见 B-03)→ **源码 offer 对不上真实二进制**,GPLv3 源码供给义务实质未履行。

### S-2 无任何许可文本随包【新增】

- **正面先记**:`LICENSE:23-32` 已写入 FFmpeg/x264/x265 → GPLv2+/LGPLv3+、LibRaw 0.21.4 → LGPL-2.1-or-later/CDDL-1.0 双许可 + tag URL + LibRaw-cmake + NDK sysroot zlib 的获取方式;`README.md:160-170` §6 同;prebuilt 内 LibRaw 头文件许可块完整(`include/libraw/libraw_version.h:9-18`、`libraw.h:9-17`、`libraw_config.h:9-18` + 版权行 `libraw_version.h:3` "Copyright 2008-2025 LibRaw LLC")。**声明层面已闭环。**
- **缺口**:全仓 glob 仅根目录 1 个 `LICENSE`(MIT),**无任何 GPL-2.0/GPL-3.0/LGPL-2.1/CDDL-1.0/zlib 全文、无 NOTICE/THIRD-PARTY/COPYING 文件、无 LICENSES/ 目录**;`app/src/main/assets/` 不存在;release APK 解包 942 条目**无 license 类资产**(仅 `assets/dexopt/baseline.prof`);`*.kt` grep `Licen|关于|third-party` 0 命中 → **APK 内无 attribution 界面**。
- zlib 的**使用**已在 `README.md:76`/`:169` 与 `LICENSE:32` 提及(NDK sysroot zlib,且 `libraw_config.h:24` `LIBRAW_USE_DNGDEFLATECODEC 1` 证明实际依赖),但 **zlib 许可证文本未随包**;LibRaw 含 dcraw 衍生物却无 dcraw/CDDL 归属声明(`docs/INTERFACES.md:196-198` 反而提醒了此事);libpostproc 的 LGPL 归属未点名;AndroidX/Compose/Material/kotlinx 的 Apache-2.0 通知未收集。
- `app/build.gradle.kts:445-447` 的 `resources.excludes += "/META-INF/{AL2.0,LGPL2.1}"` 有剥掉依赖许可文本的风险;`README.md:166-168` 自述「发布二进制时应随附许可证文本、版权声明和源码获取方式」→ **自述承诺与实际产物不符**。
- **应随附文本清单(当前全缺)**:① MIT 全文 + "Copyright (c) 2025 ProResForge contributors";② FFmpeg GPL-3.0 全文 + copyright 汇总 + 书面源码 offer(x264/x265 使整体为 GPLv3,`--enable-version3`);③ x264 GPL-2.0 + copyright + 源码 URL/commit(须先修掉 S-1 的 SHA 冲突);④ x265 GPL-2.0 + copyright + URL;⑤ libvpx BSD-3-Clause + copyright;⑥ LibRaw 0.21.4 的 LGPL-2.1 或 CDDL-1.0 完整文本 + LibRaw LLC copyright + dcraw/CDDL 归属 + tag URL;⑦ zlib license;⑧ AndroidX/Compose/Material/kotlinx Apache-2.0 NOTICE;⑨ 集中的开放源代码许可界面(建议进 Kotlin)。

### S-3 JobStore 反序列化失败静默清空历史【新增;触发条件已被代码自证】

- `data/JobStore.kt:93-103` `loadLocked()`:`:96` `readText` IOException 静默 `return emptyList()`;`:97-101` `json.decodeFromString` catch **静默 `return emptyList()`** —— 无日志、无坏档备份、不抛给调用方。
- 随后 `:106-119` `writeLocked()` 在下次 `recordFinish` 时把空列表原样写回 `jobs.json`(`:113-115` `atomicWrite` 覆盖)→ **坏档彻底消失,人工恢复机会都没有**。
- 触发条件已被代码自证:`data/model.kt:206-208` kdoc 明确记载「**枚举改名会导致反序列化失败 → 静默清空历史**」(`HistoryItem.state` 存枚举名 `JobState`)。这不是理论风险,是一次重构就能踩到的雷。
- 用户表现:历史记录全没,无任何提示、无 "history load failed" 态。
- 修复建议:(a) catch 内 `AppLog.w(TAG, "jobs.json 解析失败,备份后重建", e)` + `f.copyTo(..., overwrite = true)` 保留坏档;(b) 长期把 `state`/`stateName` 改为 `String` + `enumValues` 容错解析,从根上断掉枚举改名触发;(c) 空历史且文件非空时给 UI 一个 error 态。

---

## 3. Kotlin / Android 层问题

### 3.1 M 级(K-01…K-06、P2-06)

### K-01 [M] stale pending-cancel 泄漏到下一任务(本轮最重的功能缺陷,已亲读确认)

证据链(全部主代理逐行亲读):
- `engine/JobBus.kt:90-113` `cancel()`:对 RUNNING 记录先 `FFmpeg.nativeCancel()`(`:97`),再置 CANCELLED(`:102-111`)→ **pending 标志此时仍为 true**。
- `clearPendingCancel()` 的全部调用点只在 `engine/TranscodeService.kt:312`(nativeRun 返回后)、`:284`(预检取消分支)、`:303`(异常分支)→ 每轮只可能消费一次。
- `runJob` 的状态映射/落盘在 `TranscodeService.kt:316-366`,失败/取消分支 `:357-364` 执行 `DocumentFile.fromSingleUri(...)?.delete()`(`:359`)**删除输出**。

**两种失效路径**:
1. 用户取消落在 `nativeRun` 返回(`:296`/`:312`)到 `finish()`(`:163`)之间的窗口 → 当前任务被误标 CANCELLED,**已成功的产出被删**(`:359`)。
2. pending 标志残留 → 下一任务 `begin()` 后,预检(`:282`)看到的是 RUNNING 状态直接放行 → `nativeRun` 开局即消费残留 pending → 返回 RC_CANCELLED → **又被误标取消 + 删输出**,级联失败。

**修复**:`JobBus.begin(nextId)`(`:155`)成功后、`runJob` 前补一次 `FFmpeg.clearPendingCancel()`。

### K-02 [M] onDestroy 丢掉终端通知与尾部补录(主代理已修正子代理的「历史双丢」说法)

- `TranscodeService.kt:120-122` 先读 `currentId`/`activeRun`,`:129` 才 `serviceScope.cancel()`;此时 `activeRun` 尚未清(`:195` finally 在 `:175` recordFinish 之后)→ `finalizeId` 非空 → `finalizeInterrupted()`(`:392-424`)会 `JobBus.finish()`(`:398`/`:402`)+ 在独立 `finalizeScope` 里 `recordFinish(finalRec)`(`:410-413`)→ **历史不丢**。
- 真正丢失的是 `:178-191` 的 DONE/FAILED **终端通知**(协程在 `:175` 挂起点被 cancel,通知块被跳过)与 `:198-204` 的 **runQueue 尾部补录**。
- 复合风险:`recordFinish` 首挂起点是 `withContext(IO)`(`JobStore.kt:56-57`),本身非 NonCancellable;若 serviceScope 被取消时 onDestroy 未捕获该 id,历史会全丢。

### K-03 [M] onCreate 未保护 startForeground

`TranscodeService.kt:88` + `:467-474`:Android 15 的 `mediaProcessing` 前台服务有 6h/日配额,超额抛 `ForegroundServiceStartNotAllowedException` → **进程 FATAL**。建议 `runCatching { startForeground(...) }` + 失败 `stopSelf()`。

### K-04 [M] enqueue 与 startForegroundService 之间无回滚

`ui/HomeViewModel.kt:457-458` 先 `JobBus.enqueue(g)`,`:534-539` 再 `startForegroundService`(无 try/catch)→ 启动失败则 specs 永久 QUEUED(`JobBus` 无 remove API),且 L-新B 的空文档残留。

### K-05 [M] 单个不可解析文件阻断整批(已亲读确认)

- `ui/HomeViewModel.kt:87-88` `fun canStart(): Boolean = probeState.probed && probeState.facts != null` —— **不校验 `err`**。
- `data/MediaProbe.kt:83-126` `parseInfo` 对不可解析输入**返回 null 不抛异常**(`:84` info 空、`:95` 无视频流);`err_unprobeable` 资源在 `res/values/strings.xml:174`、`res/values-en/strings.xml:163`。
- `preflightError`(`:403-409`)只校验 tier/format/audio/fps/faststart/scale,**不校验 facts**;`ui/HomeScreen.kt:156-184` 文件列表又无移除按钮 → 一个坏文件卡住整批。

### K-06 [M] onDestroy 主线程有界等待最长 2 s → ANR 风险

`TranscodeService.kt:406-423` `NATIVE_STOP_WAIT_MS=2000` 叠加 recordFinish 的 IO。建议改 `goAsync()` 或整体留在 finalizeScope。

### P2-06 [M] runQueue 尾部补录不是 NonCancellable

`TranscodeService.kt:198-204`:在被取消的 `serviceScope` 内做普通 suspend 落库(`JobStore.kt:56-57` 首挂起点 `withContext(IO)`),无 `finally` + `NonCancellable` 保护(运行中任务的 ActiveRun 闸门 `:57-61`/`:158-159`/`:367-371` 与 `:192-196` 双 countDown 已闭环,仅尾部补录未闭环)。

### 3.2 L 级(K-07…K-13、L-06、L-17、L-新B)

**K-07 [L] `onFilesPicked` 窄窗口**:`ui/HomeViewModel.kt:188-195` 校验与写入之间无挂起点,协程 cancel 拦不住。

**K-08 [L] LUT 预览拖滑条每档一次阻塞 JNI 解码,且 before 图也照样解码**:`ui/HomeScreen.kt:106` LaunchedEffect 的 key 含 `spec.lutIntensity` → 每动一格重启协程 → `:112` `MediaProbe.previewJpeg(fd, len, 0L, spec.lutPath, spec.lutIntensity, 1200)` 一次完整 nativePreview(缩放 + 解首帧 + lut3d + JPEG 编码);滑条 `:455-465` `steps=19`(20 档);LUT 关闭时 `data/MediaProbe.kt:58-80` 仍走无 LUT 原图解码路径 → before/after 各 N 次。建议去抖 + before 图缓存复用。

**K-09 [L] `AppLog.init` 非线程安全双检**:`AppLog.kt:41-51`。

**K-10 [L] 三处死代码**:`data/LutFile.kt:82-89`(`cachedPathFor` 反查)、`data/OutputDir.kt:264-265`、`engine/TranscodeService.kt:541-551`(`enqueueStop` 无 UI 入口)。

**K-11 [L] OutputDir 批量命名重复查询(子代理 A1 详尽取证)**:`ui/HomeViewModel.kt:428-456` 循环每文件调一次 `outputDir.outputUriFor(outName)`(`:431`)→ `OutputDir.kt:114-143` → `resolveName`(`:217-232`)→ `existingNames`(`:194-214`),其中 `:202-206` `resolver.query(children, arrayOf(DISPLAY_NAME), …)` **一次枚举输出目录全部条目** → **N 个文件 = N 次 children 全量枚举**,且结果集随输出目录体积线性增长;而 `OutputDir.kt:110-113` 的 kdoc 自己承认「批内 query 与 createDocument 之间无变化」。建议把 taken 集合提到循环外算一次,或在 OutputDir 内加批内缓存。另注:`:211-213` `catch (e: Exception) { /* ignore */ }` 吞异常返回空集合。

**K-12 [L] JobStore 静默清空**:已升级为 **S-3**(触发条件被 `model.kt:206-208` kdoc 自证)。

**K-13 [L] 契约漂移**:详见 §8 矩阵(其中 `crfQuality`/`vp9Cq` 两条会造成「照文档写编译不过」)。

**L-06 [L] `defaultSubDir()` 死代码**:`data/OutputDir.kt:263-265`,全仓 grep 仅 1 命中即定义本身(0 调用点),删除安全;`mediaStoreTreeUri`(`:64-86`)用的是字面量 `"ProResForge"`,不依赖该函数。

**L-17 [L] HDR 提示硬编码(资源已存在,改 1 行即可)**:`ui/HomeScreen.kt:261-267` 三处硬编码(`:263` 判定、`:265` 文案「HDR 输入可能偏色,建议先转 SDR 或烘焙 tonemap」),而 `res/values/strings.xml:95` 的 `media_hdr_hint`(`values-en/strings.xml:90` 对应英文串)**已存在但从未被引用**。

**L-新B [L] SAF 输出文档半批泄漏**:`ui/HomeViewModel.kt:428-456` 两个退出点(`:431-435` `doc == null` 只报错;`:463-467` catch 异常)**均不回删本次已创建的空 SAF 文档**(第 1..k-1 个 createDocument 已在 SAF 里落成 0 字节文件);全仓删除 API 只在 `TranscodeService.kt`(`DocumentFile.delete` `:359`/`:377`;`deleteOutputDocument` 调用 `:237`/`:252`/`:270`/`:287`/`:415`),`DocumentsContract.deleteDocument` **全仓 0 命中**;Service 只删它认识到的 spec.outputUri,enqueue 前失败的那批文档无人回收。预检失败退出点 `:423-426` 在创建循环之前,无泄漏(干净)。建议维护 `created` 列表,两个 exit 前回删。

---

## 4. native 层问题

### 4.1 M 级(N-01…N-03)

### N-01 [M] preview.cpp send_packet 不分层且丢 packet

`preview.cpp:295` `if (avcodec_send_packet(dec, pkt) < 0) { av_packet_unref(pkt); continue; }` —— EAGAIN(应排空后再重送)与真错误同路,且直接丢弃 packet;`:289` EOF flush 未查返回值。

### N-02 [M] MJPEG 编码侧 flush 未分层,EAGAIN 被当结束

`preview.cpp:454` `avcodec_send_frame(enc, nullptr);` flush 未查返回值;`:457-461` `if (avcodec_receive_packet(enc, opkt) < 0) break;` → 截断的 JPEG 可能被判成功(仅 `:463` `out_pb->error` 兜底)。

### N-03 [M] IOContext 增长无上限、无 bad_alloc 捕获

`preview.cpp:98-104` `c->sink->insert(c->sink->end(), buf, buf + buf_size);` 无累计上限;全文 493 行**无任何 try/catch** → 恶意/异常输入可致 bad_alloc 直接崩 native。

### 4.2 L 级(N-04…N-10)

**N-04 [L,主代理降级] nativePreview 失败返回 null**:`native-lib.cpp:656-660` `if (jpeg.empty()) return nullptr;` + `NewByteArray((jsize)jpeg.size())` 窄化。亲读三方契约后判定**返回 null 符合跨语言契约**:`preview.h:17`「失败返回空 vector」、`docs/INTERFACES.md:53`「失败返回 null」、`engine/MediaProbe.kt:71-79` Kotlin 侧 `ByteArray?` 且判 null 属正常路径。待修的只是 C++ 头注释与 jsize 窄化校验;`maxW` 唯一来源是 `ui/HomeViewModel.kt:542-543` `PREVIEW_MAX_W = 640`,故窄化实际不可达。

**N-05 [L] `dngframeq.h` 安静降级与自述矛盾**:`:275` `break; // 安静降级`;`:292-301` `m_no_workers = true; … return true` 与 `:95-100` 自述「绝不能带着没线程的队列进主循环」矛盾。

**N-06 [L] `m_started` 早置导致防护失效**:`dngframeq.h:249` `m_started = true` 置于 attr 失败 `return false` 之前(`:262-268`/`:279-282`)→ `next()` 的 `:367-370` 防护失效、每 15s 诊断日志永不返回。

**N-07 [L] `max_w` 只比宽度,与注释「最大边」不符**:`preview.cpp:326-332` 只比(旋转后)宽度,而 `ui/HomeViewModel.kt:542-543` 注释写「最大边」(竖屏 1080×1920 会输出 640×1137)。

**N-08 [L] DNG 预览不看 max_w**:`preview.cpp:210`/`:232-236` 全帧 LibRaw 解码 + 手工逐行 memcpy,未按 `max_w` 降采样。

**N-09 [L] 无告警/加固编译选项**:`CMakeLists.txt` 全文无 `-Wall`/`-Wextra`/`-fstack-protector`(`-fstack-protector` 实际由 NDK toolchain 默认兜底);`-fvisibility=hidden` 与 `-Wl,--exclude-libs` 缺位 → 7 个 wrapper target + `libraw.a` 的静态符号留在 `.dynsym`。

**N-10 [L] JNI 字符串无异常检查**:`native-lib.cpp:647`/`:624` `GetStringUTFChars` 无 `ExceptionCheck`;`graded=1` 时 LUT 加载失败会静默按「无 LUT」传递。

## 5. 构建 / 供应链层

### 5.1 M 级(B-03…B-09、B-16)

### B-03 [M] provenance 双轨矛盾:BUILDINFO 说手工替换,manifest 说自动

`prebuilt/BUILDINFO:5` 记脚本 sha256 `8fd70f1e…`(已失效,当前脚本实测 `8339285f…`),`:19-26` 自述「LibRaw 本次为手工替换(未重跑整个脚本)」;而 `libraw-manifest-arm64-v8a:12` `script_sha256=8339285f…`、`:18` `manual=false`,`libraw_sha256` 与实测 libraw.a 完全一致;`:17` `build_time_utc=2026-10-06T14:30:07Z` 晚于 BUILDINFO 的 08:20:00Z。同一批字节,人读的文档与机器门禁的文档**行为相反**。

### B-04 [M] Release 门禁校验强度低于自述

`app/build.gradle.kts:289` 对 `symbols_sha256`/`members_sha256` 只查格式(长度 64/非空),**不复算 `llvm-nm`/`llvm-ar`**;`:254-255` 对 `script_sha256`/`ndk_revision` 等只查必填非空,**不与实际脚本 SHA、`ndkVersion`(`:374`=27.0.12077973)对账**;`scripts/verify-prebuilt.sh` 有复算能力(可离线跑,`--no-symbols` 可跳过符号复算)但只被 build 脚本末尾自检调用,**未接入 Gradle**,且它读的是脚本 pin 表、**不校验 BUILDINFO 里的 x264 commit**,抓不到 S-1 的冲突。

### B-05 [M] 门禁可绕过

`app/build.gradle.kts:361-369` 用 `tasks.matching { name.startsWith("buildCMakeRelease") || ... }` 挂 `prfReleaseNativeGate` → 可被 `-x` 排除、AGP 任务改名即静默 no-op、未挂 `assembleRelease`、无 CI 强制。

### B-06 [M] manifest 字段口径三处不一

`docs/verify-prebuilt.sh`(16 必填,含 `threads_lib`)、`app/build.gradle.kts:78-83`(15 字段,不含 `threads_lib`,而 manifest 实际 18 字段含 `:11` `threads_lib=pthread`)、`CMakeLists.txt:189-211`(只取 11 键)→ `app/build.gradle.kts:76-77` 注释「三者保持一致」失实。

### B-07 [M] 文档仍写「无 manifest / 手工替换 / 失败是预期」

`docs/INTERFACES.md:210-213`、`prebuilt/README.md:87-98`/`:181-187`、`app/build.gradle.kts:297-306` 的失败文案硬编码「当前 prebuilt 是 2026-10-06 手工替换产物」→ 与 manifest `manual=false` 的现实相反。

### B-08 [M] 无 NOTICE/COPYING 随包(合规细节见 S-2)

除 S-2 清单外,`app/build.gradle.kts:445-447` 的 `resources.excludes += "/META-INF/{AL2.0,LGPL2.1}"` 可能剥掉依赖许可文本;dependencies 段也无任何 license/attribution 依赖。

### B-09 [M] Release 配置未达分发标准

`isMinifyEnabled=false`(`:420` 显式)、`shrinkResources` 全文 0 命中、无 `signingConfigs`(仅 `:483-485` 注释)、`versionCode=1`(`:386`)、dex 约 22 MB 未收缩;`dist/` 内 **0 个 release 包**(100% debug 残留)。这是产品化前必做项,不阻断本次审查结论。

### B-16 [M] LibRaw 线程安全性仅有 manifest 自证,符号级未验证

- 两 manifest 记 `raw_r_target=raw_r`(`:8`)、`libraw_nothreads=absent`(`:9`)、`tls_probe=absent`(`:16`)。
- 但头文件里 TLS 结构确实存在:`include/libraw/libraw_internal.h:29` `class LibRaw_TLS`、`include/libraw/libraw.h:407` `LibRaw_TLS *tls;`(主代理本轮实测)。
- `scripts/verify-prebuilt.sh:307-322` 对 `tls_probe` **只 warn 不 FAIL** → 「tls_probe=absent」到底是「未探测」还是「确实无 TLS」无法从现状判定;若 libraw.a 实际编成了 NOTHREADS 版,而代码按线程安全使用(`dngframeq`/`dngzip` 三路径 thread_local 复用),则是并发隐患。
- **待办**:在有 NDK `llvm-nm`/`llvm-ar` 的机器上跑 `scripts/verify-prebuilt.sh app/src/main/cpp/prebuilt/arm64-v8a`,反查 `LibRaw_TLS::` 符号定论。

### 5.2 L 级(B-10…B-17)

### B-10 [L] flags 自检把 SKIPPED/UNDECIDED 记为通过

`scripts/check-native-flags.sh:98-143` 中 SKIPPED(无工具链)与 UNDECIDED(无法判定)都不计失败 → 若 flags 被污染成 `-O0`,脚本仍可能整体「看起来 PASS」。建议 UNDECIDED 单独分类并要求显式 `--allow-undecided`。

### B-11 [L] 内存预算口径三处不一致

`README.md:192-193` 与 `docs/INTERFACES.md:790` 写 400/500 MB,而 `REVIEW-REPORT-2026-10-06.md:175` 写 768/1024 MB。另:4K 四线程实测 `VmHWM` 496 MB 距 500 MB 上限仅 4 MB,文档未说明这个余量是否刻意。建议统一口径并记录实测余量设计意图。

### B-12 [L] manifest 泄漏构建机路径

`libraw-manifest-arm64-v8a:1` 注释含 `/home/ubuntu/prf/…`、`prebuilt/BUILDINFO:6` 含 `/home/ubuntu/android-ndk-r27` → 产物内容随构建机漂移,不利于复现。建议改为相对路径或脱敏占位。

### B-13 [L] `.gitignore` 不完整 + dist/ 100% debug + tmp/ 死副本

- `.gitignore` 漏 `.kotlin/`(该目录已生成)与两个坏名散落文件——本轮查明其真身是失控脚本 stdout 重定向产物:`I,h[4])[0]…struct.unpack` 与 `I,h[4] typ=h[4:8]…print(moov`(均 10/6 ~16:51)。
- **`dist/`(含 `dist/obsolete/`)10 个 APK 全为 debug,0 个 release**;其中 `ProResForge-1.0.0-debug.apk` 与 `-20261006-2248.apk` 均为 61.04 MB 且 **SHA-256 前缀同为 `f2692cff`(重复副本)**;根目录另有 `ProResForge-1.0.0-debug-dngzipfix.apk` 57.65 MB。
- `tmp/prf-current.apk`(61.06 MB,sha `3ca9f376…`)是**第三个未跟踪构建**,与 dist 的 `f2692cff`、`app-debug.apk` 的 `b454499e` 均不同,且全仓 grep 无任何引用 → ad-hoc「当前安装目标」副本。
- **provenance 歧义**:`tmp/` 下存在 4 份 libraw.a 死副本(`tmp/merge-cpp/prebuilt/<abi>/lib/libraw.a` ×2、`tmp/ad-stub/baseline-libraw.a`、`tmp/ad-cmake/prebuilt/arm64-v8a/lib/libraw.a`,大小/mtime 与正式产物相同)→ 「哪份 libraw.a 参与过构建」不可判定。
- 清理建议:删 `dist/obsolete/`、dist 重复副本、根目录乱码名文件与 debug 包、`tmp/prf-current.apk`;把 tmp 死副本移出仓库。

> 注:既往报告记录的「仓库 7 个 APK」有误,本轮实测为 14 个(见 §1.3)。

### B-14 [L] 当前副本不可自建、无版本历史

工作区为 UNC 路径(`\\192.168.100.232\win\ProResForge`),`BUILDINFO:3` 显示产物构建于 WSL2 Linux 6.6.87.2,而本机非 Linux → 该副本**无法自行重跑 prebuilt 构建链**;仓库无 `.git`,无历史可回溯。`README.md:59-61`/`:83` 未提示这一风险。建议在 README 明确「prebuilt 产物来源与再生成前置条件」。

### B-15 [L] CMake 侧多处未规范化(构建子代理全文 273 行取证 + 主代理采信)

- `CMakeLists.txt:24-26` 默认 Release;`:27` `if(CMAKE_BUILD_TYPE STREQUAL Debug)` 分支**不做任何标准化**(-O/-g/strip/-fvisibility 均不设,完全吃 toolchain 默认);`:28-33` Release/RelWithDebInfo 下 `-ffast-math`/`-Ofast` 直接 FATAL_ERROR;`:38-47` 清掉优化开关后强制 `-O3 -DNDEBUG`。
- **只动 `CMAKE_CXX_*`,`CMAKE_C_FLAGS` 从不规范化**;`CMAKE_CXX_FLAGS_MINSIZEREL` 漏处理(当前 4 个源均为 .cpp,暂未暴露)。
- CMakeLists 内 **`getenv`/`$ENV{}` 0 命中** —— PRF_* 编译宏只在 `app/build.gradle.kts:153-170`(PRF_PROTOTYPE_THREADS)/`:173-177`(PRF_AVFRAME_POOL)桥接为 `-D`,release 侧经 `:437-441` `prfReleaseCmakeArgs` 下发;源码默认值在 `transcode.cpp:19-20`(PRF_PRORES_THREADS=3)、`:218-219`(PRF_AVFRAME_POOL=0)。该机制本身自洽。
- **无 IMPORTED GLOBAL**(7 个 SHARED IMPORTED `:97` + `ffmpeg-raw` `:264` 均目录作用域);`find_package`/`add_subdirectory` 0 处。
- 预编译二进制(FFmpeg/x264/x265/vpx)的编译参数不受本仓库控制(仅 libraw.a 有 SHA 对账),建议在 `prebuilt/README.md` 记录上游构建参数快照。

### B-17 [L] versionCode/versionName 硬编码与脚本自身注释矛盾

`app/build.gradle.kts:2` 明写「版本/插件别名全部来自 `gradle/libs.versions.toml`,勿在此处写死版本号」,而 `:386-387` 就是写死的 `versionCode = 1` / `versionName = "1.0.0"`。建议移到版本目录或由 CI 注入,兑现注释意图。

---

## 6. 本轮确认已闭环的项(附证据)

| 既往编号 | 内容 | 闭环证据 |
|---|---|---|
| M-3 | `onFilesPicked` 选取竞态 | `ui/HomeViewModel.kt:162-235` generation 递增 + `probeJob?.cancel()` + 按 uri 回写校验 |
| M-5 | busy 状态卡死 | `HomeViewModel.kt:414-419` CAS + `:420-472` try/catch + finally 复位 |
| P2-03 | finish 终态守卫 | `engine/JobBus.kt:183-202` `isTerminal` 守卫;`:116-124` `clearFinished` 同锁 |
| — | `nativeCancel` 先于 `serviceScope.cancel()` | `TranscodeService.kt:126` vs `:129`(顺序正确) |
| §8.1 | JobSpec 契约 | `data/model.kt:121-142` 与 `docs/INTERFACES.md:249-258` 逐字段吻合(仅 dng* 三字段未列,见 B3) |
| — | `allowBackup=false` 全域排除 | `AndroidManifest.xml` + `res/xml/data_extraction_rules.xml:12-35` |
| 既往 P1-01/02/03 | FFmpeg 主循环错误处理 | `transcode.cpp` 31 处 send/receive/bsf 全部分层;`encode_out` EAGAIN 时先 receive 排空再重送同一帧(`:1181-1200`,注释明确「不能丢帧」) |
| 既往 P1-04 | trailer 写入 | `transcode.cpp:2768-2773` 只调一次;faststart 失败置 `result=AVERROR(EIO)`(`:2783-2809`) |
| 既往 P1-05 | fd 泄漏 | `done:` 标签统一释放 + `close(fd_in/fd_out)`(`:2901-2902`) |
| 既往 P1-06 | DNG ZIP 边界 | `checked_add_u64`、EOCD/ZIP64/CD 校验、`csize>avail` 拒(`:604-610`)、`usize>512MB` 拒(`:612-618`)、resize 在 method 与 `0x7FFFFFFF` 检查后(`:658-673`)、`fw*fh` 溢出检查(`:945-957`) |
| 既往 P1-07 | LibRaw 线程安全/栈 | 三路径全部堆分配(`dngzip.cpp:879-888` thread_local unique_ptr + `:889-893` `LibRawReuseGuard` + `:896` make_unique 回退);`dngframeq.h:44-48` 注明 `sizeof(LibRaw)=767,320` 且 `:251` 显式 8 MB 栈。**代码侧用法闭环;但 manifest 的 `tls_probe=absent` 与头文件 `LibRaw_TLS` 并存,符号级未证(见 B-16)** |
| 既往 P2-04 | 预览内存写 mem | `preview.cpp:224-243` 逐行校验 dst 容量后写 |
| — | 首帧解两次 | `transcode.cpp:1617-1634` seed_frame swap + `:2382-2387` `pool.seed`,worker 从 seq=1 起,重复解码已消除 |
| — | 队列取消语义 | `dngframeq.h:107-111` `kNextOk/Cancelled/Drained` 三分,`transcode.cpp:2424-2444` 消费 |
| — | 预览参数越界 | `preview.cpp:177-183` dng_quality/dng_color 越界退回默认 |
| — | spec 解析 weekday 注入 | `transcode.cpp:1531-1573` weekday 字段拒绝非常白名单值 |
| L-22(部分) | zipOptions 测试副本 | `DngSpecMappingTest.kt:65-67` 仍是 `HomeScreen.kt:369-376` 的手工副本,未消除(见 §7) |

---

## 7. 测试覆盖缺口

当前 22 个 `@Test` 全部是纯 JVM 数据层小测,**零覆盖以下高风险区**:

| 未覆盖区 | 位置 | 风险 |
|---|---|---|
| `FFmpeg.buildSpec` 整段 | `engine/FFmpeg.kt:147-248` | spec 拼装规则错误直接产出不可播文件;无任何测试 |
| OutputDir 命名去重 | `data/OutputDir.kt:114-232` | 含 TOCTOU 与 K-11 的 N 次查询问题 |
| `LutFile.parseHeader` | `data/LutFile.kt:96-130` | 1D/3D 混淆、size 越界只靠运行时判 |
| `MediaProbe.parseInfo` | `data/MediaProbe.kt:83-126` | null 返回值语义(K-05 所在) |
| **JobBus 状态机** | `engine/JobBus.kt` | **K-01 / K-02 都在这里,却是 0 测试** |
| HomeViewModel 预检 | `ui/HomeViewModel.kt:87-88`、`:403-409` | K-05 所在 |
| 旧 jobs.json 兼容 | `data/JobStore.kt:93-119` | S-3 所在 |
| Service 生命周期 | `engine/TranscodeService.kt` | K-03/K-06/K-02 |

**建议优先补三个纯 JVM 测试类**(不依赖 Android 框架,成本最低):① `buildSpec` 参数表驱动;② JobBus 状态机(重点覆盖「cancel 后立刻 begin 下一任务」);③ 旧版 jobs.json / 坏档 jobs.json 反序列化兼容。

另:L-22 残留 —— `DngSpecMappingTest.kt:65-67` 的 `zipOptions` 与 `HomeScreen.kt:369-376` 是同一段逻辑的手工副本,应提取共享常量后删除副本。

---

## 8. 文档一致性矩阵(docs/INTERFACES.md vs 代码)

| # | 文档 | 代码实际 | 处置 |
|---|---|---|---|
| B1 | `:242` `crfQuality` | `model.kt:35-37` `crfHigh/crfMedium/crfLow`,`crfQuality` 全仓 0 命中 | **必须改文档** |
| B2 | `:243` `vp9Cq`(单数) | `model.kt:38-40` `vp9CqHigh/vp9CqMedium/vp9CqLow`(三档) | **必须改文档** |
| B3 | §8.1 JobSpec 未列 dng 三字段 | `model.kt:136-142` 有 `dngQuality/dngColor/dngMap`+默认值 | 补文档 |
| B4 | §8.1 MediaFacts 是占位注释 | `model.kt:172-200` 含 14+ 字段 | 补字段表 |
| B5 | LUT 契约(头部键/4096 行/size∈2..256) | `LutFile.kt:96-130`、`:185`、`:188`、`:122/:126` | 大体一致,补两条实现细节 |
| B6 | LUT 上限 16MB | `LutFile.kt:191` | 一致 |
| B8 | `outputUriFor(name): OutputDoc?` | `OutputDir.kt:114` `suspend fun outputUriFor(name: String): OutputDoc?` | 补 suspend + OutputDoc 结构 |
| B11 | `lutint` 总写 | `FFmpeg.kt:233-237` 仅 `lutPath != null` 时写 lut/lutint | 改文档语义 |
| B12 | 过滤器链顺序 | `FFmpeg.kt:147-248`(:241-245 applelog 插入位置与文档一致) | 一致 |
| B14 | SettingsStore 存 intensity | `LutFile.kt:75-76` 只存 uri+name,刻意不存 intensity(`:28` 注释) | 改文档(`:283`/`:288`) |
| nit | `strspec.h:38` 注释「n<=22」 | `strspec.h:306-315` 实际 27 键 `kAll[27]` | 改注释 |

**结论**:`docs/INTERFACES.md` §8.1 FormatSpec 的字段名已与代码脱节到「照文档写编译不过」的程度(B1/B2),这是文档侧最高优先级修复项。

---

## 9. 修复路线图

### 第一批(1-2 小时,消掉 S/M 级数据风险)

1. **K-01**:`TranscodeService.kt` `runJob` 前补 `FFmpeg.clearPendingCancel()`(位置建议 `:155` `begin(nextId)` 成功后)。
2. **S-3**:`JobStore.kt:97-101` catch 内加 `AppLog.w` + 坏档 `copyTo` 备份;长期把 `HistoryItem.state` 迁移为 `String` + 容错解析。
3. **K-03**:`TranscodeService.kt:88`/`:467-474` `startForeground` 包 `runCatching`,失败 `stopSelf()`。
4. **N-01/N-02**:`preview.cpp` send/receive/flush 分层(EAGAIN / EOF / 真错误),`:454` flush 查返回值。
5. **L-17**:`HomeScreen.kt:265` 改 `stringResource(R.string.media_hdr_hint)`(资源已存在于 `values/strings.xml:95`、`values-en/strings.xml:90`)。

### 第二批(半天,合规与契约)

6. **S-1**:重跑或修订 `scripts/build-ffmpeg-android.sh` 使产物与 pin 一致(或把 pin 改成 6.1.6 并刷新 `BUILDINFO`),恢复 README `:163-165` 的源码 offer 可指认性。必须修正的三处:`BUILDINFO:10` FFmpeg commit(`d4ff0020` → n6.1.1 真实 commit 或直接改 6.1.6 线)、`:11` x264(`0480cb05` vs `b35605ac`)`:12` x265、`:13` libvpx,外加 `lib/pkgconfig/vpx.pc` 的 1.17.0 vs pin v1.15.0;在此之前任何依赖 BUILDINFO 的 provenance 结论都不可信。
7. **S-2**:新增 `app/src/main/assets/NOTICE`(或 `licenses/` 目录),按 S-2 的 9 项清单随 release APK 打包许可证文本与源码 offer(FFmpeg GPLv3 / x264 / x265 / libvpx / LibRaw LGPL-CDDL 二选一 / zlib / AndroidX Apache-2.0);Kotlin 侧加 attribution 界面;移除或收窄 `app/build.gradle.kts:446` 对 `/META-INF/{AL2.0,LGPL2.1}` 的排除。
8. **B-07/B-06**:刷新 `prebuilt/README.md:181-187` 与 `app/build.gradle.kts:297-306` 中「手工替换/失败是预期」文案,使与 `manual=false` 的 manifest 一致;把 `docs/verify-prebuilt.sh`、`app/build.gradle.kts:78-83`、`CMakeLists.txt:189-211` 三处 manifest 字段口径统一(含 `threads_lib`)。
9. **文档**:改 B1/B2,补 B3/B4/B8,改 B11/B14;顺手修 `strspec.h:38` 注释。
9b. **B-16**:在有 NDK `llvm-nm`/`llvm-ar` 的机器上执行 `scripts/verify-prebuilt.sh app/src/main/cpp/prebuilt/arm64-v8a`(该脚本可离线跑,`--no-symbols` 可跳过符号复算),反查 `LibRaw_TLS::` 符号,判定 NOTHREADS 疑云。

### 第三批(收尾语义与可观测性,1-2 天)

10. **K-02/P2-06**:`runJob` 尾部补录与通知包 `finally` + `withContext(NonCancellable)`;`onDestroy` 遍历所有终态未落库任务。
11. **L-新B**:`HomeViewModel.kt:428-456` 维护 `created` 列表,两个 exit 前回删。
12. **K-04**:`enqueueStart`(:534-539)包 try/catch,失败回滚 `JobBus` 与已创建文档。
13. **K-05**:`canStart` 追加 `probeState.err == null`。
14. **K-06**:`onDestroy` 的 2 s 等待改 `goAsync()` 或整体留 `finalizeScope`。
15. **K-08**:预览 LaunchedEffect 去抖 + LUT 关闭时复用无 LUT 预览缓存。
16. **K-11**:`existingNames/taken` 提到 `HomeViewModel.kt:428` 循环外算一次。

### 第四批(质量基建)

17. 补 §7 建议的三个纯 JVM 测试类;抽取 `zipOptions` 共享常量消除 L-22 副本。
18. 删除死代码:L-06(`OutputDir.kt:263-265`)、`LutFile.kt:82-89`、`OutputDir.kt:264-265`、`TranscodeService.kt:541-551`(`enqueueStop` 无 UI 入口)。
19. 门禁加固(B-04/B-05):Gradle 门禁复算 `llvm-nm`/`llvm-ar`,把 `verify-prebuilt.sh` 接进 `prfReleaseNativeGate`,并改为不可 `-x` 跳过的形式(或加 CI)。
20. 工程整理:B-09(R8/signing/versionCode + `dist/` 补 release 产物)、B-10、B-11、B-12、**B-13(删 `dist/obsolete/`、dist 重复副本、根目录乱码名文件与 57.65 MB debug 包、`tmp/prf-current.apk`;把 `tmp/` 下 4 份 libraw.a 死副本移出仓库;`.gitignore` 补 `.kotlin/`)**、B-14、B-15、B-17(versionCode 移出版本目录);同时核查为何 `.git` 缺失。

---

## 附录 A:本报告引用的全部证据文件

- native:`app/src/main/cpp/{transcode.cpp, dngzip.cpp, dngzip.h, dngframeq.h, native-lib.cpp, preview.cpp, preview.h, strspec.h, transcode.h, CMakeLists.txt}`
- Kotlin:`app/src/main/java/com/proresforge/app/{engine/{FFmpeg.kt, JobBus.kt, MediaProbe.kt, TranscodeService.kt}, data/{JobStore.kt, LutFile.kt, OutputDir.kt, model.kt}, ui/{HomeScreen.kt, HomeViewModel.kt}, AppLog.kt}`
- 构建/供应链:`scripts/build-ffmpeg-android.sh`、`scripts/check-native-flags.sh`、`app/build.gradle.kts`、`prebuilt/BUILDINFO`、`prebuilt/README.md`、`prebuilt/{arm64-v8a,armeabi-v7a}/libraw-manifest-*`、`prebuilt/*/include/{libavcodec/version.h, libavformat/version.h, libavutil/{version.h,ffversion.h}, x264.h, x265_config.h, libraw/{libraw.h, libraw_internal.h, libraw_config.h, libraw_version.h}}`、`prebuilt/*/lib/pkgconfig/*.pc`、`gradle/libs.versions.toml`、`.gitignore`
- 合规:`LICENSE`、`README.md:140-198`
- 契约:`docs/INTERFACES.md`
- 清单/权限:`app/src/main/AndroidManifest.xml`、`res/xml/data_extraction_rules.xml`、`res/values/{strings.xml,values-en/strings.xml}`
- 测试:`app/src/test/java/com/proresforge/app/data/{FormatCatalogTest, HistoryItemSerializationTest, DngSpecMappingTest}.kt`

## 附录 B:复核方法与可信度分级

| 结论级别 | 取证方式 |
|---|---|
| A(主代理亲读) | 主代理逐行 read 原文 + grep 全文确认,如 K-01 全链路、K-02 判定、S-1 版本头、S-2 APK 解包、§1.2 SHA 实测、§5 闭环证据 |
| B(子代理详尽取证,主代理抽样核) | A1-A5、B 组契约表、N-07…N-10 |
| C(子代理报告,主代理未逐行复核) | K-07(窄窗口)、K-09(AppLog 双检)、K-10(OutputDir 查询重复)中的细节、K-12、K-13 |

四处子代理误报已被主代理剔除/修正:**B-02**(prebuilt 缺 `.a` → glob 证实全部存在)、**N-04**(返回 null 违约 → 符合跨语言契约,降级)、**K-02**(历史双丢 → 仅通知与尾部补录丢失,历史不丢)、**zlib 未声明**(`README.md:76`/`:169` 与 `LICENSE:32` 已提及 NDK sysroot zlib → 修正为「zlib 许可证文本未随包」)。

## 附录 C:未验证边界(本报告结论的适用范围)

1. **未执行**:gradle 构建/测试、CMake 编译、`scripts/verify-prebuilt.sh`、`scripts/check-native-flags.sh`、真机转码、内存/性能实测。
2. **未联网核对**上游 tag ↔ commit 对应关系(仅核对了 ffmpeg.org 版本表中 `libavcodec` 版本号);x264/x265/libvpx 的 SHA 差异结论基于本地 BUILDINFO 与脚本 pin 的**文本比对**,未拉取上游仓库验证该 commit 对应的版本号。
3. `check-contracts.sh` / `check-native-flags.sh` 的 rc 结论引自既往报告,未复跑。
4. 内存(400/500 MB)与性能数字(4K VmHWM 496 MB)引自既往报告,本轮未复测。
5. 因此:本报告的**代码级结论可信度高**,**构建产物级结论(尤其是 S-1 的上游版本归属)建议在下一次有网络/构建环境时用一次真实 `build-ffmpeg-android.sh` 复跑来最终确认**。

---

*报告完。本次为纯只读审查,除本报告外未创建、修改或删除任何文件,未执行任何构建/测试命令。*
