# ProResForge 第十一轮复审

日期：2026-10-11。范围：Round10 报告（`REVIEW-REPORT-2026-10-07-ROUND10.md`）之后发生的全部改动——10-08 凌晨的批次 1–7 整改（无 FIXLOG，只有 `build-r10-*.log`）、10-08 的 UI 轮（U-01…U-18，见 `FIXLOG-2026-10-08-UI.md`）、进度条修复（A/B，见 `PROGRESS-BAR-STUCK-50-2026-10-08.md` §十）、10-09 的 PCM→MP4 降级（`FIX-PCM-MP4-2026-10-09.md`）。逐段亲读源码核对，JVM 单测在当前树上重跑。本轮未改任何源码。

结论先说：Round10 点名的全部 16 项（R10-1…R10-11、K-07、N-05/06/07/08）已经**全部落地且落点正确**；进度条 A/B 修复与代码一致；10-09 的 PCM 降级逻辑本身正确但**设备验证仍未做**。本轮新发现 1 个功能性缺口（PCM 范围检查漏 PCM_F24LE，低）、1 个已记录未修的 UI 缺陷仍在（入队 Snackbar 永不消失）、2 处重复注释行。发布闸门（S-1 完整重编 / B-09 签名混淆）依旧未动。

## 已关闭（本轮亲读核对）

### Round10 批次 1 — 服务终态（R10-9 / R10-3 / R10-10）

| 项 | 证据 |
|---|---|
| R10-9 成品被标取消并删 | `TranscodeService.kt:415-422`（runJobInner 在 NonCancellable 内、statSize 之前就 `JobBus.finish`）；`:277-284` outcome 先登记再开闸门；`finalizeInterrupted`（`:475-511`）改为「等闸门 → 读 outcome → 才决定状态与删不删」；判定抽成纯函数 `resolveInterruptedOutcome`（`JobBus.kt:279-288`，DONE 不删，其余删，记录已终态不覆盖）。 |
| R10-9 队尾漏补录 | `finalizeRemainingJobs`（`TranscodeService.kt:520-542`）：已终态未落库的补 `recordFinish`，仍 QUEUED 的 `removeQueued` + 删 0 字节文档。 |
| 大小回填通道 | `JobBus.updateOutSize`（`JobBus.kt:239-250`）只改 outSize 不碰状态，绕开 finish 的终态守卫。 |
| R10-10 前台失败仍跑队列 | `promoteToForeground` 返回 Boolean（`:615-637`）；`onCreate` 失败分支 `rollbackQueuedJobs()` 后 return（`:104-107`、`:549-565`），不再拿 wakelock、不再 `launch { runQueue() }`。 |
| R10-3 | 同一 finish-提前 窗口：`JobBus.cancel`（`:96-119`）对非 RUNNING/QUEUED 直接 return，完成不被改回取消；取消在 native 运行期落下的 CANCELLED 由 finish 守卫保持。 |

### Round10 批次 2 — 建档回滚与探测代际（R10-11 / K-07）

| 项 | 证据 |
|---|---|
| R10-11 | `OutputDir.outputDocsFor`（`OutputDir.kt:130-141`）第一次创建失败即停、返回列表与 names 等长（失败位及之后为 null）；调用方回删 `docs.filterNotNull()`（`HomeViewModel.kt:487`），不再依赖前缀累加器；注释与义务一致（`:122-128`）。 |
| K-07 | `HomeViewModel.kt:201-202/213-214/236-239/256-257` 每一处写 `files`/`busy` 的 update lambda **内部**再读 generation，过期原样返回；finally 只清自己代际的 busy。 |

### Round10 批次 3 — flush 零进展封顶（R10-5）

`kFlushStallLimit = 8`（`transcode.cpp:123`）。视频 flush（`:2954-3021`）与音频 AAC flush（`:3022-3059`）同形：每轮先读 `g_cancel`（置位 → `result = 1`，与主循环取消点 1715/2661/2675/2794 同码）；「send NULL→EAGAIN 且 receive 无产出」计一次零进展，触顶 `result = AVERROR(EAGAIN)`（真实负码，不再冒充成功）走 done；有产出清零重计。`loop_error`（`:3136-3139`）不覆盖已写错误。

### Round10 批次 4 — 坏中央目录（R10-4）

`dngzip.cpp:388-397`：中央目录项签名不符一律 `return -1`（错误串与两表全空一致），不再「已收帧就 break 后 return 0」。

### Round10 批次 5 — 预览循环会停（R10-1 / R10-2 / R10-6 / av_frame_ref）

| 项 | 证据 |
|---|---|
| R10-1 解码零进展 | `preview.cpp:313-396`：`kDecodeStallLimit = 8`，两种双 EAGAIN 场景（包在手 send/receive 全 EAGAIN；EOF 后 flush EAGAIN+receive EAGAIN）都计 stall，触顶退出（有帧用帧、没帧走空预览）；`progressed` 语义正确（跳过别的流/读到包/收到帧都算推进）。 |
| av_frame_ref | `:374-377` 查返回值，失败丢该帧继续，不再用已 unref 的 cand。 |
| R10-2 编码 flush | `:548-597`：首帧 send EAGAIN 不再 `goto cleanup`（`:549`）；「flush EAGAIN 且无包」计 stall，`kEncFlushStallLimit = 8` 触顶 `enc_ok=false` → `result.clear()`；flush 后仍 EAGAIN 直接失败（`:592`）；`!flushed` 也判失败（`:597`）。 |
| R10-6 向后扫封顶 | `:197` `kPreviewBackscan = 8`，循环 `:225` 上限 `i - idx < 8`；注释改为「从目标帧向后最多 8 帧」。 |

### Round10 批次 6 — 预览尺寸（N-07 / N-08）

| 项 | 证据 |
|---|---|
| N-07 长边 | `preview.cpp:408-431`：旋转后 `dw > max_w || dh > max_w` 按长边等比缩小，宽高取偶；`HomeViewModel.kt:616` 注释已是「预览图最大边」。 |
| N-08 半尺寸 | `dng_decode_to_rgb48` 增显式 `want_half_size` 尾参（默认关）；预览传 `true`（`preview.cpp:240-241`），转码两处（`transcode.cpp:1737/2603`）与探测（`native-lib.cpp:328`）显式传 `false`；`apply_libraw_params`（`dngzip.cpp:863-876`）**false 也写 0**，堵住 FAST 档 half_size=1 在线程局部 LibRaw 上的残影；`tlr_params0` 快照每帧复位（`:961-968`）。 |

### Round10 批次 7 — 异常 / fd / 线程失败（R10-7 / R10-8 / N-05 / N-06）

| 项 | 证据 |
|---|---|
| R10-7 fd 漏关 | `preview.cpp:168-172` `FdReleaser` 栈守卫，异常展开时 close 且置 -1；正常路径 `:629` close 后解除，互斥不 double-close。 |
| R10-8 异常穿 JNI | `native-lib.cpp:620-638` run_transcode 包 try/catch，异常经 owns_lifecycle 调 `end_transcode()`、返回 -12（非 0）；`nativeInfo` 同样有 catch（`:684-692`）。 |
| N-05 零线程降级 | `dngframeq.h:300-311`：`thread_count()==0` → `m_err` + `return false`，不再 `m_no_workers`；消费线程亲自解码的分支已删（`:384` 注释）。 |
| N-06 m_started | `dngframeq.h:313`：线程至少建成 1 条之后才置位；attr/init 失败路径保持 false。 |

### 进度条修复（A/B）与 UI 轮抽查

- A：`vp.rep(-1, "audio", …)` ×3（`transcode.cpp:2578/2906/2927`），50% 地板消失。
- B：`makeProgress` 的 `shown` 封顶（`TranscodeService.kt:576-591`）——非 `done` 阶段一律 `coerceIn(0,99)`，`worthIt`/`lastPct`/`maxPct` 全用 `shown`；`JobBus.progress` 仍有 RUNNING 守卫（`JobBus.kt:193`），早 finish 后的迟到进度不污染状态。
- U-06：`zipFacts` 取「任一文件是 DNG zip」（`HomeScreen.kt:411`），混批不再丢 DNG 档位，zip 帧率档无 ORIG。
- U-11/U-12/U-14：Snackbar 带动作、双通知渠道（LOW 进度 / DEFAULT 结果）、通知理由框 dismiss 不再静默开转——代码与 FIXLOG 一致；`OutputActions.kt` 消除了 Jobs/History 的 Intent 重复。
- 资源：`values`/`values-en` 各 164 键，键集合一致；两份 dist APK（10-08 12:18 与 10-09 14:36）内均含 9 个 `assets/licenses/` 条目；`MIT.txt` 占位版权已换成仓库根正文。

### 构建与测试复证

- JVM 单测当前树重跑：6 套件 **58 tests, 0 failures, 0 errors**（`JobBusTest` 17 含 resolveInterruptedOutcome 用例、`FFmpegBuildSpecTest` 13、`FormatCatalogTest` 12、`DngSpecMappingTest` 6、`JobStoreCompatTest` 6、`HistoryItemSerializationTest` 4）。
- 批次 1–7 实施轮的验证证据：`build-r10-verify.log`（10-08 07:03）为 clean + 双 ABI native 重编 + assembleDebug + testDebugUnitTest 全绿（4m23s）。
- 产物 SHA 复算一致：`dist/ProResForge-1.0.0-debug.apk` = `B67EAC39…`（进度条修复构建），`dist/ProResForge-pcm-fix-20261009-143628.apk` = `213DC9FA…`（PCM 修复构建），与各自文档相符。

## 新问题

### N11-1 PCM 降级范围检查漏 PCM_F24LE / PCM_VIDC / PCM_SGA（低）

`transcode.cpp:2224`：`is_pcm = (aid >= AV_CODEC_ID_FIRST_AUDIO && aid <= AV_CODEC_ID_PCM_F16LE)`。本仓库 FFmpeg 6.1 的 `codec_id.h:363-366` 里 `PCM_F16LE` 之后还有 `PCM_F24LE`、`PCM_VIDC`、`PCM_SGA` 三个 PCM 族 id——这些（罕见）音频 copy 进 MP4 时仍会撞 `Tag sowt incompatible…` 硬失败而不是降级 AAC。范围末段还把 `S302M`（`:356`，在 F16LE 之前）划进「PCM」，但它走 AAC 降级是安全的（有解码器），无副作用。建议把上界改成 `aid < AV_CODEC_ID_ADPCM_IMA_QT`（0x11000，PCM 块整体），一劳永逸。

### N11-2 入队 Snackbar 永不消失（中低，已记录未修）

`HomeScreen.kt:142-152`：`showSnackbar(msg, actionLabel = viewJobsLabel)` 在 Material3 下带 actionLabel 默认 **Indefinite**；`vm.consumeMessage()` 又只在 resolve 后调用 ⇒ `Queued N job(s)` 永不自动消失，且 `message` 一直非空，每次回到 Transcode 页重新弹出。这在 `PROGRESS-BAR-STUCK-50-2026-10-08.md` §六已复现并给出修法（显式 `duration = SnackbarDuration.Long` 或 Indefinite+dismiss action，送出后立即 consume），至今未实施。

### N11-3 重复注释行（nit）

`HomeScreen.kt:139-140` 与 `Notifications.kt:35-36` 各有一行注释原样重复两次。无害，顺手清理即可。

### PCM 修复的设备验证缺口（流程）

`verify-pcm-fix.md` 与 `FIX-PCM-MP4-2026-10-09.md` 的验证清单全部未勾选（「验证状态：待设备测试」）。代码逻辑本轮已核对成立（降级只动 `audio_mode`；aac 链路任一步失败都落到 `drop` 不留半初始化；MOV/ProRes 输出的 `is_mp4=false` 不受影响），但「PCM→MP4 实际出 AAC、AAC→MP4 保持 copy、DNG zip→ProRes 保持 PCM」三条回归仍需装机跑一遍才能闭合。

## 仍开放（与 Round10 一致，未回归）

| 项 | 状态 |
|---|---|
| S-1 发行串 | 两 ABI `ffversion.h` 仍是 `FFMPEG_VERSION "n6.1"`，FFmpeg 未完整重编；NOTICE 的 GPL offer 依旧缺 URL/要约人/三年期限。**发布阻塞**。 |
| B-09 | `isMinifyEnabled = false`、无 signingConfig，release 不可分发。 |
| S-2 余项 | MIT.txt 与 APK 打包已闭合；LicensesScreen 行内 Markdown 记号未剥（U-17 遗留，已知）。 |
| N-09 | CMakeLists 仍无 `-Wall`/`-Wextra`/`-fvisibility=hidden`/`-fstack-protector` 字面量（信息性）。 |
| pwrite64==0 空转 | `transcode.cpp:346-356` `fd_io_write` 对 `n == 0` 仍无 break（普通文件几乎不会发生；FIX-PLAN 已明确另案）。 |
| EOCD 窗口 | `dngzip.cpp` 64KiB 窗口短 21 字节、不核对 `comment_len` —— 按计划保持不改。 |
| JobStore 坏档 | 有备份无恢复 UI（backlog）。 |
| 短素材帧数/百分比不同源、排队态 0% 空条 | 进度条报告建议 A/C，未实施（另案）。 |

## 流程备注

10-08 凌晨的批次 1–7 整改轮**没有 FIXLOG**——整改计划（`FIX-PLAN-2026-10-07-ROUND10.md`）、代码内 `R10-*` 注释与 `build-r10-*.log` 是仅有的痕迹。建议补一份简短 FIXLOG（逐项指向落点行号 + 验证日志名），与本仓库其余轮次的可追溯性对齐。
