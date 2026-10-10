# 2026-10-07 Round9 审查整改落地记录（FIXLOG）

> 输入基线：`docs/REVIEW-REPORT-2026-10-07-ROUND9.md`。本文件记录 Round9 优先级整改、代码侧复核以及 JVM/native/prebuilt/模拟器验证；上一轮历史见 `docs/FIXLOG-2026-10-06.md`。

## 1. 整改总览

| 项 | 整改内容 | 结果 |
|---|---|---|
| K-01 | `JobBus.begin()` 成功后清理 stale pending cancel，避免上一任务取消请求污染下一任务 | 已修 |
| N-01/N-02/N-03 | preview send/receive/flush 状态机区分 EAGAIN、EOF、真实错误；不丢 packet；MJPEG flush 检查；单帧 JPEG 缓冲上限 64 MiB | 已修 |
| S-3 | `JobStore` 坏档解析失败时保留 `jobs.json.corrupt-<timestamp>` 备份并记录告警，从空历史恢复 | 已修 |
| S-1 | FFmpeg/x264/x265/libvpx pin 与现存版本头对齐；BUILDINFO、per-ABI manifest、脚本 SHA 同步 | 已修 |
| S-2 | `assets/licenses/` 随 APK 分发完整许可证/归因文本；History 增加 Licenses 入口和等宽文本弹窗 | 已修 |
| K-03 | `startForeground` 受异常保护，失败记录、`stopSelf()`，避免前台服务启动异常导致进程崩溃 | 已修 |
| K-02/P2-06 | 任务终态落库、通知和队列尾部补录放入 `NonCancellable`；`runJob` 终态清理不因取消丢失 | 已修 |
| K-04 | 服务入队/启动失败时只回滚仍为 `QUEUED` 的 Job，并回删已创建 SAF 文档 | 已修 |
| K-05 | 输入文件行增加 Remove，可从当前批次移除不可处理文件 | 已修 |
| K-06 | 仅在 native 仍活动时等待停止窗口，已完成终态不再无意义等待 | 已修 |
| K-08 | LUT 预览增加 250 ms 去抖，减少滑动时重复 native 解码 | 已修 |
| K-11 | SAF 批量输出先取目录快照，再按批次解析递增命名，避免每个文件重复枚举 | 已修 |
| L-新B | SAF 半批创建失败时回删本批已创建文档并清理 pending 名称 | 已修 |
| L-17 | HDR 提示改用 `media_hdr_hint` 资源 | 已修 |
| B-06/B-07 | manifest 必填字段、`threads_lib`、BUILDINFO/provenance 文案和当前状态同步 | 已修 |
| 契约/测试 | 修订 `crfQuality`、`vp9Cq`、`JobBus.enqueue`、`OutputDir` 等契约；新增纯 Kotlin SpecBuilder、JobBus、JobStore 兼容性测试 | 已修 |
| 清理 | 移除无效临时 APK、重复 dist/tmp/prebuilt 副本和无调用死代码；`.gitignore`/`.zcodeignore` 覆盖 `.kotlin/` | 已完成 |

## 2. Provenance 状态

当前脚本 SHA-256：

```text
683d34540d3847231c73547fde51bf8be36e112379c00c648b17abdc02e2bb4b
```

`app/src/main/cpp/prebuilt/BUILDINFO` 与两个 ABI manifest 均已同步该值。两个 manifest 均复核为：

```text
manual=false
raw_r_target=raw_r
libraw_nothreads=absent
threads_lib=pthread
libraw_tag=0.21.4
libraw_cmake_sha=eb98e4325aef2ce85d2eb031c2ff18640ca616d3
tls_probe=absent
```

FFmpeg/x264/x265/libvpx 现存产物的版本头分别对应 n6.1.6、X264_BUILD 165、X265_BUILD 216、libvpx 1.17.0；`BUILDINFO` 明确保留“版本级对应已核对、commit 级归属需下一次完整重建”的边界。`tls_probe=absent` 是信息性静态探针结果，不单独判死 raw_r；随后补做的 host `raw_r` 多线程同帧行为探针已通过（见 §3）。

## 3. 验证结果

| 验证 | 实测结果 |
|---|---|
| `:app:testDebugUnitTest` | **52 tests completed, 0 failed** |
| `:app:assembleDebug` | **BUILD SUCCESSFUL** |
| `:app:prfVerifyLibRawManifests` | **通过** |
| `:app:prfWriteNativeFlagsManifest` | **通过** |
| `:app:configureCMakeRelease[arm64-v8a]` | **通过** |
| `scripts/verify-prebuilt.sh` | `arm64-v8a 17/17`、`armeabi-v7a 17/17`，均 `GATE_PASS` |
| Round9 APK | `tmp/prf-round9.apk`，SHA-256 `fe549f56544d965d0c7a96686a57775e97fc84e0ccfe19377cc93e743ecffde9` |
| LibRaw HOST_TEST/F | **通过**：同一 `ref_frame000000.dng`，host `raw_r`、`PRF_LIBRAW_REUSE=1`，2T×5 轮（10 次）与 4T×5 轮（20 次）全部输出 SHA 一致；尺寸 `4096×3072`，RGB48 字节数 `75497472`，SHA-256 `bad07fa30bcbcbb55491fb7295594acdd7799cef15afa532c4b8dee9fd2a5e56` |

## 4. Android 模拟器冒烟（AVD `medium_phone`, `emulator-5554`）

- APK 安装和应用启动成功。
- DNG zip 探测成功：4 帧、4096×3072、rgb48le；preview JPEG 路径成功。
- 连续 DNG zip 转码成功，输出名正确递增（含 `(2)`…`(8)`），native fallback 从 `hevc_mediacodec` 到 `libx265` 正常，任务完成并显示 `Done · 4/4`。
- Home 输入行的 Remove 按钮实测可用。
- History → Licenses 打开成功，NOTICE 与许可证文本可在应用内查看。
- Cancel UI 实测成功：运行中的 `prf-smoke-4f.zip → prf-smoke-4f_PF_h265 (7).mp4` 显示 `Cancelled · 4/4`。
- **K-01 闭环**：取消后不清除输入，重新 Start 同一输入生成 `prf-smoke-4f_PF_h265 (8).mp4`；Jobs 页面显示该新任务 `Done · 4/4`、`100%`，没有被旧 pending cancel 立即取消。
- 当前 MCP 日志输出有截断，未将某一条 `nativeRun rc=1` 或 `job end` 原始行作为独立证据；上述结论以 Jobs UI 的终态和连续输出结果为准。

## 5. 边界与后续

1. LibRaw 行为探针使用 WSL x86_64 host `raw_r` archive，而非直接执行 Android arm64 archive；它验证了 formal `dngzip.cpp` 解码路径与并发行为，但 ABI 特定执行仍未覆盖。manifest 的 `tls_probe=absent` 仅为符号级信息，不覆盖这项行为证据。
2. prebuilt 的版本级 pin↔版本头已核对；若要求 commit 级可追溯性，必须重新运行 `scripts/build-ffmpeg-android.sh` 并重新生成全部二进制、BUILDINFO 和 manifest，不能手工改写 provenance。
3. 本轮未声称 arm64 真机、4K 长时内存/耗时、无通知权限降级和所有 SAF provider 变体已覆盖。
