# FFmpeg 错误码 22（EINVAL）实机转码失败 — 检查结论与修复方案

> 日期：2026-10-07（Asia/Shanghai）
> 范围：只读检查（源码 / 产物 / FFmpeg 6.1 源码），本文件只给结论 + 方案，**未改任何源码**
> 触发问题：「项目编译后上实机验证出现问题，使用 prores 导 h265 等格式会出现 ffmpeg 错误码 22 报错」

## 0. 结论先行（TL;DR）

1. **UI 上的「ffmpeg 错误码 22」= native 返回 `-22` = `AVERROR(EINVAL)`**
   （契约：`docs/INTERFACES.md:48` — `<0 = ffmpeg 错误码的相反数`）。
2. **上一轮（10-07 00:49）的修复（候选编码器 + open2 回退）只覆盖"编码器打开失败"这一种情况，
   而且它的立论（"hevc_mediacodec 会在 avcodec_open2 阶段以 EINVAL 拒绝"）在 FFmpeg 6.1 源码里
   找不到支撑**：mediacodec 编码器的 configure/start 失败一律返回 `AVERROR_EXTERNAL`
   （`libavcodec/mediacodec_wrapper.c:2300-2310` "Encoder configure failed, %d"），
   它在 init 里返回 `AVERROR(EINVAL)` 只有 4 处，且与本工程参数不相交（见 §2.3）。
   ⇒ 真正的 -22 出处目前**仍未定位**，需要设备日志（一条日志即可定位到具体行）。
3. **必须先确认实机装的是哪一份 APK**：`dist/ProResForge-1.0.0-debug.apk`（10-06 22:48）里
   **没有**回退代码；`app/build/outputs/apk/debug/app-debug.apk`（10-07 00:52）才有（判据见 §1.2）。
   若实机装的是 22:48 那份，则 22 就来自"第一个候选（mediacodec）打开失败后直接终止"。
4. **取证通道已经现成，不需要 adb**：历史页有「日志」按钮（`app/src/main/java/com/proresforge/app/ui/HistoryScreen.kt:235-250`），
   分享出 `filesDir/logs/app.log`；native 侧 `pf_av_log`（`app/src/main/cpp/native-lib.cpp:95-150`）以
   `AV_LOG_INFO` 等级把 **FFmpeg 自己的 ERROR 行**一并落盘 ⇒ 失败阶段在日志里是白纸黑字。

---

## 1. 现场与产物事实（已核对）

### 1.1 时间线
| 时间 | 事件 | 证据 |
|---|---|---|
| 10-06 22:48 | `dist/` 两份 APK（均 64,009,808 B） | 文件时间戳 |
| 10-07 00:49:29 | `app/src/main/cpp/transcode.cpp` 被改（+diagnostic +候选回退） | mtime、与 `tmp/merge-cpp/transcode.cpp`（10-06 19:30，109,650 B）diff 314 行 |
| 10-07 00:52:52 | `app/build/outputs/apk/debug/app-debug.apk`（60,747,762 B） | mtime 晚于源码改动 |
| 10-07 02:58 | 用户报告：ProRes→H.265 等格式实机导出报错误码 22 | 本次会话 |

### 1.2 怎么判断实机装的是哪一份（已用 `strings` 证实）
在同一路径 `lib/arm64-v8a/libffmpeg-transcode.so` 上：

| 字符串 | app-debug.apk（00:52，**有回退**） | dist/*.apk（22:48，**无回退**） |
|---|---|---|
| `video encoder selected=` | 1 | 0 |
| `video encoder open failed` | 1 | 0 |
| `trying fallback` | 1 | 0 |
| `no fallback available` | 1 | 0 |
| `video encode %s failed` | 1 | 0 |
| `video mux write failed` | 1 | 0 |
| .so 大小 | 1,934,728 B | 1,920,032 B |

复现命令（Windows/WSL 均可）：
```bash
unzip -p <apk> lib/arm64-v8a/libffmpeg-transcode.so > /tmp/t.so
for s in "trying fallback" "no fallback available" "video encoder selected="; do
  printf '%-28s %s\n' "$s" "$(grep -a -c "$s" /tmp/t.so)"
done
```

### 1.3 其它已排除的"旧账"（避免重复怀疑）
| 怀疑项 | 结论 | 依据 |
|---|---|---|
| SAR mismatch（`mux.c:275`） | **不可达** | 全工程只设 `enc->sample_aspect_ratio={1,1}`（transcode.cpp:1650），`v_ost->sample_aspect_ratio` 从未设置 ⇒ `st->sar.num==0` 短路 |
| 时间基未设置（`encode.c:586`） | **不可达** | transcode.cpp:1572-1584 保证 `fps_num/fps_den>0` 才进候选循环；`enc->time_base={fps_den,fps_num}` |
| 尺寸未设置（`encode.c` `dimensions not set` / `mux.c:260`） | **不可达** | transcode.cpp:1536-1566 归一：源尺寸兜底、`round_down_even`、夹到 `[2,8192]` |
| `max_pixels` / `av_image_check_size2`（`encode.c:386`） | **不可达** | `max_pixels` 未设 = INT_MAX（`options_table.h:399`），8192²≪INT_MAX |
| PCM 音频进 MP4 | **合法，不是 -22 来源** | `movenc.c:7871` `PCM_S24LE→ipcm`；movenc 的 EINVAL 只在 AC-3/E-AC-3/TrueHD 路径 |
| ADTS/AAC bsf、PCM 长度断言 | 不涉 EINVAL | `mov_write_pcmc_tag` 用 `av_assert0`（断言≠错误码） |
| mediacodec 的 `max_b_frames>0` EINVAL（mediacodecenc.c:307） | 本工程 `max_b_frames=(is_x264\|\|is_x265)?2:0` ⇒ mediacodec 恒 0 | transcode.cpp:1641 一带 |
| 硬件解码路径 | **不存在** | 全工程解码只用 `avcodec_find_decoder()`（transcode.cpp:1514、preview.cpp:254），mediacodecdec 的 EINVAL 不可达 |
| faststart | 失败不改 `result`，且成功/失败都打日志 | transcode.cpp:2499-2520、:2592-2610 |
| 文档化的设备证据 | **无**：docs/ 全库没有 22 的设备/素材记录；`FIXLOG-2026-10-06.md` 也没写这次回退 | grep `错误码 22\|EINVAL` 全库 |

---

## 2. `-22` 的全部可达来源（按"本工程可达性"排序）

### 2.1 本工程自己的静默 EINVAL（最可疑：无日志、与格式无关）
| 位置 | 触发条件 | 日志 |
|---|---|---|
| `transcode.cpp:315` | `fd_io_read(buf_size<=0)` | 无 |
| `transcode.cpp:334` | `fd_io_write(buf_size<=0)` | 无 |
| **`transcode.cpp` fd_io_read/write 的 `return AVERROR(errno)`** | `pread64/pwrite64` 返回 EINVAL(22)（SAF/MediaStore 的 fd 在部分机型上不支持某些偏移/加密/代理写） | **无**（原样上抛，用户只看到 22） |
| `transcode.cpp:370/373` | seek 未知 whence / 目标偏移 <0 | 无 |
| `transcode.cpp:1178` | `av_audio_fifo_read` 短读 | 无（部分已补） |
| `transcode.cpp:1246/1273` | `av_audio_fifo_write` 短写 | 无（部分已补） |
| `transcode.cpp:2481` | `!oc` 守卫 | 有 INFO |

### 2.2 通用层检查（有 FFmpeg 自己的 ERROR 行，会落 app.log）
| 位置 | 触发条件 | FFmpeg 日志文本 |
|---|---|---|
| `libavformat/mux.c:567-576`（mp4/mov **严格**，movenc 未设 `AVFMT_TS_NONSTRICT`） | 每流 dts 非严格递增 | `Application provided invalid, non monotonically increasing dts to muxer in stream %d: %s >= %s` |
| `libavformat/mux.c:577-582` | `pts < dts` | `pts (%s) < dts (%s) in stream %d` |
| `libavformat/mux.c:797-825` | 同上（另一份实现）+ 时间戳未设 | `Timestamps are unset in a packet for stream %d` |
| `libavformat/mux.c:771-781` | stream_index 越界 / attachment 流 | `Invalid packet stream index: %d` |
| `libavformat/mux.c:210/232/260` | 无流 / 音频采样率未设 / 尺寸未设 | 同名文本 |
| `libavcodec/encode.c:386`、`:473/477`、`:622/634` | 尺寸检查 / 音频 frame_size 语义 / 像素格式不支持 | `Specified pixel format %s is not supported by the %s encoder.` 等 |
| `libavfilter/buffersrc.c` `CHECK_AUDIO_PARAM_CHANGE` | 运行期音频帧属性变化 | `Changing audio frame properties on the fly is not supported.` |

### 2.3 编码器 open2 的 EINVAL（**回退已覆盖，但需注意**）
| 编码器 | EINVAL 条件 | 本工程可达性 |
|---|---|---|
| `hevc_mediacodec`/`h264_mediacodec` | 只有 mediacodecenc.c:224/235/244（surface 模式缺 `hw_device_ctx` 等）与 :307（`max_b_frames>0` 且 `strict>EXPERIMENTAL`） | **都不可达**（本工程 yuv420p、非 surface、max_b_frames=0）⇒ 回退到软件编码时的 `last_open_ret` 不可能是 mediacodec 给的 22 |
| `libx265` | `libx265.c:244` `Image size is too small (%dx%d).`（**<16**）、`:348` `Invalid crf`、`:161/176` `Invalid value for param`、`:462` profile、`:214` tune | 可达**当且仅当** `out_w/out_h` <16（当前只保证 ≥2）或 spec 的 crf/profile 非法 |
| `libx264` | `libx264.c:812-826` `bad option/value`、`:922-925` bit_rate>INT_MAX、`:1118` profile | 参数固定为 preset=medium + crf/profile ⇒ 需 spec 非法 |
| `libvpx-vp9` | `libvpxenc.c:456-501` codec control、`:993-996` config_default、`:1005-1006` set_pix_fmt | 需构建/参数不支持 |
| `prores_ks` | `proresenc_kostya.c:1215` mbs_per_slice 非 2 的幂、`:1239` alpha bits | 本工程不设 `slices`/无 alpha ⇒ **不可达** |

**⇒ 若实机装的是"带回退"的包，媒体编码器的 open2 已基本排除；剩下的高概率是 §2.1（本工程静默点）与 §2.2（mux 时间戳/音频属性）。**
**⇒ 若实机装的是 dist 的 22:48 包，则 22 来自 `pick_video_encoder` 单选取后 `avcodec_open2` 的返回值**
（此时需要日志确认是哪一个候选、什么返回码）。

### 2.4 顺带查出的两个真实缺陷（与 22 无关，但会制造"设备相关差异"）
1. **`gop_size` 对 NTSC 帧率算错 1001 倍**：`enc->gop_size = fps_num * gop_seconds`
   （transcode.cpp:1600-1730 一带，且注释写了"mediacodec 给 2 秒"）。
   例：29.97fps（`fps_num=30000,fps_den=1001`）→ `gop_size=60000`（应为 59.9），
   传给 MediaCodec 的 `i-frame-interval = round(60000/29) = 2069`（单位：秒）——
   部分机型的 `MediaCodec.configure()` 会直接抛异常（→ AVERROR_EXTERNAL → 回退软编）。
   正确写法：`av_rescale(fps_num * gop_seconds, 1, fps_den)`。
2. **10bit 请求会被静默降为 8bit**：`FormatCatalog.kt:22` 的 HEVC 声明 `pix_fmt="yuv420p"`、
   `supports10Bit=true`；用户勾 10bit 时 `FFmpeg.kt:201` 发 `pixfmt=yuv420p10le`，
   但 `hevc_mediacodec` 的 `avc_pix_fmts[]` 只有 `{MEDIACODEC, YUV420P, NV12}`（`mediacodecenc.c:93-98`），
   第一轮 `want_fmt` 匹配失败 → 第二轮 `codec_best_fmt` 把 mediacodec 作为 **8bit** 候选塞进来
   ⇒ 结果：用户要 10bit，实际出 8bit，且没有任何提示。

---

## 3. 修复方案

### P0 — 先定位（0 行代码，今天就能做完）
- **P0-1** 确认实机装的哪份包：按 §1.2 的判据查（或直接换装 `app/build/outputs/apk/debug/app-debug.apk`，10-07 00:52）。
- **P0-2** 用**同一素材、同一设置**复现一次失败（ProRes → H.265），然后：
  历史页 →「日志」→ 分享 `app.log`（可微信/邮件发回）。日志里重点看：
  | 关键字 | 说明 |
  |---|---|
  | `transcode: video encoder selected=` | 选定的是 hevc_mediacodec 还是 libx265 |
  | `transcode: video encoder open failed` … `trying fallback` | 回退是否真的发生过、返回码多少 |
  | `transcode: video encode send_frame/receive_packet failed` | 运行期编码失败（带 codec/pix_fmt/size/pts/ret） |
  | `transcode: video mux write failed ret=…` | 写 mux 失败 |
  | `Application provided invalid, non monotonically increasing dts` / `pts (…) < dts (…)` | mux 时间戳类 EINVAL |
  | `Encoder configure failed` / `MediaCodec configure failed` | 设备拒绝参数（会是 EXTERNAL，不是 22） |
  | `Specified pixel format … is not supported` / `Image size is too small` / `Invalid crf` | 软编 open2 的 EINVAL |
  | `Changing audio frame properties on the fly is not supported` | 滤镜图音频属性变化 |
- **P0-3** 记录：设备型号 + ABI（`getprop ro.product.cpu.abi`）、Android 版本、10bit 开关状态、
  失败的输出格式清单（H.264/H.265/VP9/ProRes 各试一次）、素材特征（分辨率/帧率/时长/有无音轨）、
  失败后输出文件是否存在（部分文件 = 已写到一半）。

### P1 — 诊断强化（小改：`transcode.cpp` + `FFmpeg.kt`/`JobsScreen.kt`）
- **P1-1 失败出口带 stage 标签**：给每个 `result = …` 出口加一个短标签（枚举字符串），
  返回给 Kotlin 时用已有的负数编码之外再落一行日志；历史/通知文案显示成
  `错误码 22 (mux-ts)` 这样的形式。标签建议：`open-input/decode/filter/enc-open/enc-send/enc-recv/mux-ts/mux-write/audio/avio/faststart/finalize`。
  收益：**下一次用户只需截图**就能定位，不必再取日志。
- **P1-2 补全静默点日志**：§2.1 的 6 处 + `AVERROR(errno)` 分支全部打 `av_log(AV_LOG_ERROR, …)`
  （带 errno 名/数值、偏移、长度、fd）。特别是 `fd_io_read/write` 的 `n<0` 分支。
- **P1-3 作业开头打环境行**：ABI/设备、spec **全部 27 键**、源流信息（codec/尺寸/fps/pix_fmt/time_base/bit_rate/rotation/音轨）、
  输出容器、候选清单（每个候选的 codec+fmt+open 返回值）。
- **P1-4（推荐）编码自检回环**：正式开跑前，用**同一编码器+参数**对 5 帧合成帧做一次
  `buffersrc → format → enc → mp4(自定义内存 AVIO)` 回环，把成败与 FFmpeg 日志作为候选排序依据。
  成本 ~10-100ms；收益：把"设备上这个编码器/码率/尺寸组合是否能用"提前到开跑前，
  且自检失败时**输出文件还没被写过**，回退是干净的。

### P2 — 结构性修复（中改）
- **P2-1 运行期回退**（把 00:49 的半成品补完）：视频编码/mux 写在前 `N=8` 帧（或前 2 秒）内失败时，
  记 stage → `ftruncate(output_fd, 0)` → 以 `force_vcodec=<下一候选>` 重启整条管线（最多 2 次）。
  输入是文件（可 seek），zip 源在内存 ⇒ 重启可行。
- **P2-2 mediacodec 像素格式兼容**：候选去重键从 `codec` 改为 `(codec, fmt)`，
  给 h264/hevc_mediacodec 加 **NV12（`COLOR_FormatYUV420SemiPlanar=0x15`）优先**、再 YUV420P
  —— 大量机型只保证 SemiPlanar。
- **P2-3 mediacodec 码率阶梯**：`bitrate_mode=1`（CBR）下码率过高是常见 configure 失败原因；
  open2 失败时把 `bit_rate` 减半重试同一候选（≤2 次）并打日志；同时把
  `estimate_mc_bitrate` 的 80 Mbps 上限改为按分辨率/帧率收敛（先记录实测再定值）。
- **P2-4 10bit 语义修正**：want 10bit 时只保留声明支持 10bit 的编码器；
  链里只剩软编就走软编（libx265 main10），**不再静默 8bit**；做不到时 UI 明确提示。
- **P2-5 gop 修正**：`gop_size = av_rescale(fps_num * gop_seconds, 1, fps_den)`；
  mediacodec 的 `i-frame-interval` 夹到 `[1,10]` 秒。

### P3 — 加固（可选）
- **P3-1** mux 前时间戳守卫：保证每流 dts 严格递增且 `pts>=dts`，违反则修正并打日志，
  把 `mux.c:567/582` 的 EINVAL 变成可恢复。
- **P3-2** 输出 fd 不可写时区分 `EINVAL/ENOSPC/EPIPE`，UI 给明确文案（"输出位置不可写/空间不足"），
  不再统一显示"错误码 22"。
- **P3-3** 把 stage 标签表与错误码语义写回 `docs/INTERFACES.md`；给 native 烧一个构建标识
  （构建脚本注入宏：时间戳/内容哈希），作业日志开头打印 —— 这次"哪份包"的歧义就不该再出现。

---

## 4. 验证矩阵（改完后照此跑）

| 维度 | 取值 | 期望 |
|---|---|---|
| 设备 | 用户实机（arm64/v7a 各一）+ Windows AVD 冒烟 | 两个 ABI 都过 |
| 素材 | `tmp/out-linear.mov`、`tmp/out-applelog.mov`（ProRes 4.3/4.4MB）；另加 4K/29.97fps/带 PCM 音轨的真实 ProRes | 全部成功 |
| 输出 | MP4·H.264、MP4·H.265（8bit/10bit）、WebM·VP9、MOV·ProRes | 全部成功；10bit 勾选后确实是 10bit |
| 选项 | faststart 开/关；缩放 ORIG/1080p/720p/50%/25%；音轨 copy/aac/drop | 全部成功 |
| 异常注入 | 故意把输出 fd 设为不可写；人为把 crf 置非法值 | 失败时 UI 显示 `错误码 22 (stage)`，日志有对应 ERROR 行 |

**验收标准**
- AC1：真机 ProRes → H.265（含 PCM 音轨）连续 3 次成功；
- AC2：`app.log` 里无 `E` 行（或仅剩可解释的告警）；
- AC3：任何失败都能在 UI/历史里看到 `错误码 NN (stage)`；
- AC4：AVD 冒烟（DNG zip → H.264 MP4 DONE）不回归；
- AC5：`:app:assembleDebug` + `:app:testDebugUnitTest` 通过（双 ABI 产物含 7 个 FFmpeg .so + `libffmpeg-transcode.so`）。

---

## 5. 需要用户确认的问题（阻塞项）

1. 实机现在装的是哪一份 APK？（`dist/ProResForge-1.0.0-debug.apk` 22:48 / `app-debug.apk` 00:52）
2. 能否在**失败后**点历史页「日志」把 `app.log` 发回？（这一条能直接把结论坐实）
3. "等格式"具体指哪些？（H.264 / VP9 / ProRes 是否也报 22；是否都带音轨）
4. 10bit 开关当时是否打开？
5. 设备型号 + ABI + Android 版本；素材的分辨率/帧率/时长/有无音轨？
6. 失败后输出文件是否残留（部分写入）、能否播放？

> 本文件为方案，**未改动任何源码/资源**。P0 完成后（拿到日志）可 5 分钟内把范围收窄到具体一行，
> 再按 P1→P2 落地；如需我先做 P1（诊断强化）或 P1-4（自检回环），请明确指示。


---

## 6. 【新增·2026-10-07 03:40 实机日志】另一条确定性缺陷：faststart 读回失败被当成任务失败（错误码 5）

### 6.1 新证据（用户提供的 app.log 片段，原文照录）

```
10-07 03:40:18.170 I/TranscodeService: job begin: 261007_033849_VIDEO_25mm_Apple Log_Apple-Log2.zip → ..._PF_prores.mov (PRORES)
10-07 03:40:19.257 I/FFmpeg: transcode: video encoder selected=prores_ks pix_fmt=yuv422p10le size=4096x3072 fps=25/1
10-07 03:40:19.268 I/FFmpeg: transcode: dngzip 并行解码 45 帧:4 线程 / 预取深度 2(源 4096x3072)
10-07 03:40:45.837 E/FFmpeg: transcode: faststart 失败(stage=moov-move, point=scan-io, errno=9(Bad file descriptor), out_len=256621583, out_ctx.size=256621583):按失败收尾,不伪装完成
10-07 03:40:45.867 I/TranscodeService: nativeRun rc=-5
10-07 03:40:46.058 I/TranscodeService: job end: ... state=FAILED dur=27886ms in=992438012 out=0 err=FFmpeg 错误码 5
```

判读：
1. 实机装的是 **00:52 那份包**（`transcode: video encoder selected=` 这条 INFO 只存在于 00:52 构建，见 §1.2）。
2. 这次跑的是 **DNG zip(45 帧/4096x3072/Apple Log2) → ProRes .mov**，不是出问题诉求里的 ProRes→H.265。
3. **编码本身完全成功**：成品已写 256,621,583 B（245 MB）、`av_write_trailer` 已成功（否则不会走到 faststart）。
4. 爆点在 faststart（moov 前移，纯优化步骤）的**第一次读**：`point=scan-io` + `errno=9(EBADF)`。
   按 `transcode.cpp:626-664` 的 `scan_top_level()` 语义，`scan-io` = 第一次 `pread_exact()` 就失败，
   **此时文件一个字节都没被改动**（搬移的第一次写盘发生在 `shift-A-write`）。
5. 当前代码把 faststart 的任何失败都升级为任务失败（`transcode.cpp:2631-2633` `if (result == 0) result = AVERROR(EIO);`
   → `rc=-5` → Kotlin 按失败收场并删除输出）⇒ **一个完整、可播放的 245 MB 成品被删掉，原因只是一个 0.2 秒的优化步骤失败。**
6. **本次错误码是 5(EIO)，不是 22**：用户诉求里的 -22 在这一份日志里没有出现（-22 的实机证据仍然缺，见 §5）。

### 6.2 根因

输出 fd **不可读**。Kotlin 侧已经按「faststart 需要读回」用 `"rwt"` 打开
（`app/src/main/java/com/proresforge/app/engine/FFmpeg.kt:269-276`，注释原文：「必须带读权限("rwt" 而非 "wt"):faststart(moov 前移)会用 pread 读回」），
但设备上这个 SAF 输出 provider **没有兑现读权限**：
- 写：正常（`pwrite64` 写了 256 MB）；
- 定位：正常（`lseek64(fd,0,SEEK_END)` 量出 256,621,583）；
- 读：**第一次 `pread64` 就 EBADF** ⇒ 拿到的实际是 `O_WRONLY`（少数第三方/网络类 DocumentsProvider 会把不认识的 mode 降级成只写）。

native 侧对这类 fd 没有任何防备：`faststart_move_moov()` 只做 `pread_exact/pwrite_exact`，
失败即 `PRF_FS_FAIL` 上报（`transcode.cpp:682-751`）。

### 6.3 方案（P0-0，优先级高于 §3 全部）

原则：**faststart 是纯优化，不能因为优化失败丢掉已经成功的成品**；只有"文件可能已被改坏"的失败才允许让任务失败。

1. **失败分级**（`transcode.cpp:2610-2635` 调用点）：
   - **无害阶段**（文件分毫未动）：`bad-arg`、`bad-size`、`scan-io`、`scan-structure`、`too-few-boxes`、`no-moov`、`moov-too-large`、`read-moov`、`bad-layout`
     → `result` 保持 0，只打 `AV_LOG_WARNING`（文案建议：`transcode: faststart 跳过(point=%s, errno=%s):文件未改动,保留成品(moov 在尾部)`），任务按成功收尾。
   - **危险阶段**（可能半搬移）：`shift-A-read`、`shift-A-write`、`write-moov`、`verify-*`
     → 维持现有语义：ERROR + `result = AVERROR(EIO)`（Kotlin 删残文件，不留下不可播的成品）。
2. **搬移前先验可读性**：`faststart_move_moov()` 入口加一次 `pread(fd, &b, 1, 0)`（或 `fcntl(fd, F_GETFL)` 看 `O_ACCMODE`）；
   不可读 ⇒ 直接归入"无害阶段"，跳过搬移 + WARNING（连搬移都不尝试，省掉 245 MB 的无效 IO 风险）。
3. **给 faststart 一条可读通道（可选 B，配合 1/2 才治本）**：Kotlin 侧 `createOutFd` 额外开一个只读 fd
   （`resolver.openFileDescriptor(uri, "r")`，`TranscodeService.kt:347` 已在用同一手法量尺寸），
   native 接口多收一个 `read_fd` 专供 faststart 读回；若 provider 依然不给读，则退回第 1/2 条的"跳过"路径。
4. **补日志**（同 §3-P1，本次现场暴露的盲区）：`fd_io_read`/`fd_io_write` 里
   `if (n < 0) return AVERROR(errno);`（`transcode.cpp:322`、`:341`）加一行 `av_log`（errno 文本 + 当前 pos + 请求长度），
   把"静默 errno 冒泡"变成可定位的 ERROR 行。
5. **需要用户点头的语义**：接受「faststart 跳过时输出 moov 在尾部、仍可播放」这一降级吗？
   （绝大多数播放器/剪辑软件可播；仅个别要求 moov 在前端的极旧播放器受影响。）

### 6.4 与 -22 的关系

两者机制不同，但**暴露的是同一类盲区**：fd/provider 层失败被静默地转成错误码 + 失败语义过度放大。
- 本次（5/EIO）：faststart 读了不可读的 fd。
- -22 待查：仍缺实机日志（§5 问题 2 依然是最短路径）。

建议复现取证顺序：① 同一素材 → 输出 **H.265/MP4** 跑一次；② 失败后点历史页「日志」把 `app.log` 发回（不需要 adb）。

### 6.5 追加问题（并入 §5）

7. 输出目录在哪？（本机存储 / SD 卡 / U 盘 / NAS-SMB / 云盘）——用于确认不可读 fd 的来源。
8. 这次任务是否勾选了 faststart？（若是关闭状态仍有此日志，请一并说明。）

---

## 7. -22 的最后一段：候选链/预检的精确行为与「日志指纹」表（2026-10-07 复核 00:52 版源码）

### 7.1 候选链怎么搭出来的（`app/src/main/cpp/transcode.cpp:1598-1620`）
- 第一轮（满足请求格式）：`want_fmt` 来自 spec 的 `pixfmt`（`engine/FFmpeg.kt:201`：H265+10bit → `yuv420p10le`，否则 FormatCatalog 的 `pixFmt`），chain 里每个编码器 `codec_supports(c, want_fmt)` 为真才入列。
- 第二轮（保出片）：对 chain 里每个编码器追加 `codec_best_fmt(c)`（优先级表 `transcode.cpp:822-833`：yuv420p → yuv420p10le → yuv422p → yuv422p10le → yuv444p10le → nv12 → yuvj420p）。
- **去重只按编码器**（`add_candidate` 里 `if (candidates[i].first == c) return;`）⇒ H265+10bit 的实际候选 = `[(libx265, yuv420p10le), (hevc_mediacodec, yuv420p)]`（10bit 请求被 mediacodec 候选静默降为 8bit，见 §2.4）。
- `candidates.empty()` → `result = 2`（不是 22）；`int last_open_ret = AVERROR(EINVAL)`（`transcode.cpp:1628`）在 candidates 非空时**不可达**（循环里每条失败路径都写它）⇒ 排除「初值泄露成 22」这条。
- ⇒ **只有当最后一个候选也失败、且其返回码是 -22 时，UI 才会显示 22**；此时日志里必定留下该候选的 ERROR（预检）或 WARNING（open / 预检被拒）行。

### 7.2 mediacodec 预检（`transcode.cpp:1704-1780`）
mediacodec 候选不直接开正式上下文，而是先在临时上下文上 open2 + 造探测帧 + send_frame + drain：
- 失败 → `E/FFmpeg: transcode: video encoder preflight failed codec=<名> pix_fmt=<fmt> size=WxH fps=N/D stage=open|frame-buffer|send-frame|receive-packet|receive-after-send-eagain ret=<码> (<strerror>)`，随后 `W/FFmpeg: … preflight rejected … ; trying fallback` 或 `; no fallback available`。
- 成功 → `I/FFmpeg: transcode: video encoder preflight ok …`。
⇒ H.265 任务里只要日志出现 `preflight` 字样，就说明走到了 mediacodec 分支（00:52 包独有）。

### 7.3 「22」的日志指纹表（拿到日志后对号入座）

| 日志里的行 | 出处 | 含义 |
|---|---|---|
| `… preflight failed … stage=open ret=-22` | `transcode.cpp:1710-1724` | mediacodec open2 期 EINVAL |
| `… preflight failed … stage=frame-buffer ret=-22` | 同上 | 探测帧分配失败（尺寸/格式不被接受）|
| `… video encoder open failed … ret=-22 (Invalid argument)` | `transcode.cpp:1824-1838` | 软编 open2 期 EINVAL（§2.3：libx265 的 crf/preset/tune/profile/尺寸<16；libx264 的 bad option、bit_rate>INT_MAX、profile；libvpx 的 config）|
| `Specified pixel format %s is not supported by the %s encoder.` | libavcodec `encode.c`（ff_encode_preinit）| 请求的 pix_fmt 不在该编码器列表里 |
| `Application provided invalid, non monotonically increasing dts to muxer in stream %d: … >= …` | `libavformat/mux.c:567-576`、`:797-825` | MP4/MOV 严格时间戳检查（movenc 未设 AVFMT_TS_NONSTRICT）|
| `pts (%s) < dts (%s) in stream %d` | 同上 | 同上 |
| `Timestamps are unset in a packet for stream %d` | `mux.c:803-807` | 同上（!FF_API_COMPUTE_PKT_FIELDS2 分支）|
| `Picture size %ux%u is invalid`、`The encoder timebase is not set` | `libavcodec/encode.c` | 尺寸/时基类通用检查 |
| `Invalid crf: %2.2f.`、`Error setting preset/tune %s/%s.`、`Cannot open libx265 encoder.`、`Image size is too small (%dx%d).` | `libavcodec/libx265.c:244/:348`、`:462/:468` | libx265 参数/尺寸拒绝 |
| `Encoder configure failed, %d`、`Failed to create codec` | `libavcodec/mediacodec_wrapper.c`、`mediacodecenc.c` | 设备编解码器拒绝（返回 AVERROR_EXTERNAL，**不是** 22）|
| **完全没有相关行** | 本工程自己的静默点 | `fd_io_read/fd_io_write` 的 `AVERROR(errno)`（`:322/:341`，无日志）、`aac_emit_fifo` 短读（`:1178`）、`av_audio_fifo_write` 短写（`:1246/:1273`）、`if (!oc)`（`:2593`）|

### 7.4 取证请求（一次就能定性）
1. 历史页「日志」按钮把 **app.log 与 app.log.1 两个文件**都发回（2MB×2 轮转；H.265 那次失败很可能还在较早的 app.log.1 里，别让它被轮转掉）。
2. H.265 复现时开/关 10bit 各跑一次；**若两次都成功也请把日志发回**——那说明 22 已被回退掩盖，且要顺手确认成品到底是不是 8bit（§2.4 的静默降级）。
