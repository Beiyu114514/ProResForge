# ProResForge 审查报告与整改方案

审查日期：2026-10-06  
审查范围：当前工作区源码、native C++、Kotlin/Compose、Gradle 构建脚本、Manifest、接口文档、已有测试产物。  
审查方式：以当前文件为准进行静态复核，并检查已有构建/测试结果；不把历史审查记录直接当作当前结论。  

## 结论

项目的分层和接口设计基本清晰，SAF 文件访问、JNI fd 所有权、DNG ZIP 解析、LibRaw 堆分配、DNG 帧队列死锁修复、历史记录单例和 FFmpeg runtime 打包白名单等问题已经处理得比较完整。

当前仍有几项会影响用户可见结果或数据正确性的缺陷：

1. 连续选择文件时，旧批次的探测结果可能写入新批次，造成媒体信息串文件。
2. 取消和成功收尾之间存在状态竞态，用户取消的任务可能最终显示为 DONE 并发送完成通知。
3. `buildSpec()` 异常时会使用不完整的默认 spec 继续转码，输出可能与用户选择不一致。
4. 音频 FIFO、BSF 和 flush 错误存在静默丢失或被当作正常结束的路径。
5. DNG 预览固定使用 FAST/CAMERA 参数，与实际转码所选参数不一致。
6. DNG 内存实现、注释和验收标准不一致，4K 实测内存已经超过项目当前的 400 MB 口径。
7. native 依赖没有完整固定版本，构建结果不可重复。

因此，当前版本适合继续开发和修复，完成 P1 项前不建议作为稳定版发布；完成 P1 后仍需要 arm64 真机回归，尤其是 DNG、音频、取消和长队列路径。

## 验证边界

当前目录不是 Git 仓库，无法进行提交差异审查：

```text
fatal: not a git repository (or any of the parent directories): .git
```

本轮尝试执行：

```text
./gradlew.bat --no-daemon :app:testDebugUnitTest
```

在工作区盘符和已有本地副本上都在真正编译前失败，环境错误为：

```text
java.io.IOException: Unable to establish loopback connection
java.net.SocketException: Invalid argument: connect
```

因此，本报告没有把这次 Gradle 启动失败解释成源码编译失败。

工作区已有的测试 XML 来自 2026-10-05，结果为：

| 测试类 | 用例数 | failures | errors | skipped |
|---|---:|---:|---:|---:|
| `DngSpecMappingTest` | 4 | 0 | 0 | 0 |
| `FormatCatalogTest` | 12 | 0 | 0 | 0 |
| `HistoryItemSerializationTest` | 4 | 0 | 0 | 0 |
| 合计 | 20 | 0 | 0 | 0 |

这些结果只能证明已有数据层测试曾经通过，不能替代当前源码的重新构建，也没有覆盖下面列出的并发、状态机、音频错误和真实 arm64 native 路径。

## 问题清单

### P1-01：文件探测批次竞态，facts 可能串到另一批文件

位置：[HomeViewModel.kt](../app/src/main/java/com/proresforge/app/ui/HomeViewModel.kt:160-196)

`onFilesPicked()` 每次调用都会启动新的 IO 协程。上一批探测没有取消，也没有 generation/token。新批次会先整体替换 `state.files`，旧批次完成后仍然按旧的 `idx` 使用 `mapIndexed` 更新当前列表。

典型时序：

1. 用户选择 A 批文件，A 开始探测。
2. 用户马上选择 B 批文件，B 替换当前 `files`。
3. A 的结果返回。
4. A[0] 的 facts 被写到 B[0]，A[1] 被写到 B[1]。

另外，代码在探测循环开始前就执行 `busy=false`，所以用户可以在事实信息尚未完成时点击开始。服务随后会再次探测输入，增加 I/O，并扩大旧批次回调覆盖新状态的窗口。

影响：错误的分辨率、编码器、帧率、HDR 标记或 DNG 判断可能被展示和用于入队。

整改：

- 保存当前探测 `Job`，新批次到来时取消上一批。
- 为每批选择分配递增 generation，回写前检查 generation 仍然匹配。
- 按 `Uri` 回写结果，不按列表下标回写。
- 增加 `probing` 或 `pendingProbeCount` 状态，整批探测完成后才解除 busy。
- `canStart` 应要求所有文件 `probed == true` 且 `facts != null`。

验收：连续快速选择两批不同格式、不同数量的文件，最终列表只能显示第二批，且每个文件的 facts 必须对应自己的 Uri。

### P1-02：取消状态可能被成功收尾覆盖

位置：[JobBus.kt](../app/src/main/java/com/proresforge/app/engine/JobBus.kt:78-100)、[JobBus.kt](../app/src/main/java/com/proresforge/app/engine/JobBus.kt:157-173)、[TranscodeService.kt](../app/src/main/java/com/proresforge/app/engine/TranscodeService.kt:132-156)

当前取消流程会先把 RUNNING 任务改成 `CANCELLED`，随后 native 可能恰好完成并返回 `RC_OK`。`runQueue()` 无条件调用 `JobBus.finish(..., DONE, ...)`，而 `finish()` 没有终态守卫，因此 `CANCELLED` 会被写回 `DONE`，随后还会发送完成通知。

影响：用户点击取消后看到“已完成”，通知栏出现 Done；历史记录也可能记成 DONE。

整改：

- 为状态转换定义明确的合法边：`QUEUED → RUNNING → DONE/FAILED/CANCELLED`。
- `finish()` 若记录已经是 `CANCELLED`，不得被 `DONE` 覆盖。
- 服务在落库和发通知前重新读取 JobBus 中的最终状态。
- 取消动作和 `_currentId` 更新应使用同一个受保护的状态转换，避免取消发生在 `begin()` 更新任务与设置 currentId 的间隙。

验收：在 native 即将返回成功的时刻点击取消，重复测试至少 100 次；最终状态必须遵循产品定义，不能出现 CANCELLED 被 DONE 覆盖。

### P1-03：`buildSpec()` 异常时继续使用不完整 spec

位置：[TranscodeService.kt](../app/src/main/java/com/proresforge/app/engine/TranscodeService.kt:209-220)

当前异常处理会把完整 spec 替换为：

```kotlin
"vcodec=" + FormatCatalog.spec(spec.format).vcodec + "\n"
```

这会丢失尺寸、帧率、画质、音频、LUT、色彩标签、faststart，以及 DNG zip 的 `src=dngzip`、`dngq`、`dngcolor`、`dngmap` 等关键参数。

影响：任务可能成功生成一个与用户选择不一致的文件；DNG zip 可能进入普通容器路径而失败或产生错误输出。

整改：

- `buildSpec()` 异常直接转为 FAILED。
- 删除已经创建的空输出文档。
- 写入可操作的错误信息并跳过 nativeRun。
- 为 `buildSpec()` 增加每种格式、DNG zip、LUT、音频和 10-bit 组合的单测。

验收：人为注入 buildSpec 异常时，不得调用 nativeRun，不得留下空输出文件，任务必须进入 FAILED。

### P1-04：音频和解码错误存在静默丢数据路径

位置：[transcode.cpp](../app/src/main/cpp/transcode.cpp:979-1005)、[transcode.cpp](../app/src/main/cpp/transcode.cpp:1833-1878)、[transcode.cpp](../app/src/main/cpp/transcode.cpp:1904-1925)

发现的具体路径：

- `av_audio_fifo_write()` 返回值没有检查。
- `av_bsf_send_packet()` 失败时只记录 warning 并丢弃当前包，转码继续。
- 视频 `avcodec_send_packet()` 失败时直接 `continue`。
- 编码器 flush 阶段没有区分 `EAGAIN`、`EOF` 和真正错误。

影响：损坏输入或内存压力下可能生成可播放但缺音频、缺视频帧或尾帧不完整的文件，任务仍显示 DONE。

整改：

- FIFO 写入必须确认写入样本数等于请求值，否则返回错误。
- BSF、解码器和编码器只允许 `EAGAIN`/`EOF` 作为非致命控制流；其他负值必须进入错误收尾。
- flush 阶段显式检查 send/receive 返回值。
- 错误信息应带流类型、帧或包位置，并映射为 FAILED。
- 增加含 AAC 音频、需要 BSF 的音频、截断包和损坏视频包的端到端样本。

验收：对损坏音频包、损坏视频包和写满存储空间的场景，任务不得报告成功且输出不得被当作完整成品保留。

### P1-05：DNG 预览参数与实际转码不一致

位置：[preview.cpp](../app/src/main/cpp/preview.cpp:167-228)、[FFmpeg.kt](../app/src/main/java/com/proresforge/app/engine/FFmpeg.kt:210-222)

预览路径固定调用：

```cpp
dng_decode_to_rgb48(..., DngQuality::FAST, DngColorMode::CAMERA, ...)
```

实际转码则根据 spec 使用用户选择的 DNG 质量、白平衡和色彩映射。

影响：用户选择 BALANCED/FINE、AUTO/D65 或 Apple Log 时，预览图不代表最终输出。

整改：

- 扩展 preview spec，使 nativePreview 接收 DNG 质量、白平衡和映射参数。
- 让预览使用与转码一致的 LibRaw 参数和 Apple Log 曲线。
- 若出于速度必须固定 FAST/CAMERA，应在 UI 明确标注“预览为快速近似”，不能暗示与成片一致。

验收：同一个 DNG 帧分别走预览和单帧转码，使用相同参数时两者颜色映射和白平衡必须一致；Apple Log 预览不得显示为普通 sRGB。

### P1-06：DNG 内存预算、注释和验收口径不一致

位置：[transcode.cpp](../app/src/main/cpp/transcode.cpp:100-150)、[INTERFACES.md](../docs/INTERFACES.md:414-422)、[INTERFACES.md](../docs/INTERFACES.md:493)、[README.md](../README.md:158)

代码注释仍写默认预算 768 MB，但实际代码是 1024 MB。文档记录 4K DNG 多线程实测 VmHWM 约 496 MB，而验收标准仍要求全程不超过 400 MB。

影响：实现已经不满足项目当前自定义验收标准；4K 和低内存设备存在被系统杀进程的实际风险。

整改方案 A：按分辨率收紧预算，4K 自动限制解码线程数和队列深度。  
整改方案 B：修订验收口径，例如 1080p ≤ 400 MB、4K ≤ 500 MB，并同步 README、INTERFACES 和 UI 提示。

无论采用哪种方案，都应：

- 统一代码注释和实际常量；
- 记录 arm64 真机的 RSS/VmHWM，而不是只使用主机数据；
- 用 4K、6K/8K、900 帧样本验证 OOM 和取消；
- 对 `dngthreads` 进行上限和低内存设备保护。

### P2-01：构建依赖未完全固定，无法保证可重复构建

位置：[build-ffmpeg-android.sh](../scripts/build-ffmpeg-android.sh:41-65)、[build-ffmpeg-android.sh](../scripts/build-ffmpeg-android.sh:220-252)、[build-ffmpeg-android.sh](../scripts/build-ffmpeg-android.sh:293-307)

FFmpeg 使用 `n6.1` 分支，x264、x265、libvpx 使用默认分支的浅克隆。上游变化会改变生成的 native 二进制。x265 的 pkg-config 文件还手写了 `Version: 4.1`，不一定对应实际源码。

整改：

- 所有上游固定 tag 或 commit SHA；
- 构建日志记录每个依赖的 commit；
- 删除假版本 pkg-config，改为使用真实生成的 metadata 或由脚本从源码读取；
- 为 ABI 和依赖版本生成 manifest，随构建产物保存。

### P2-02：依赖缺失只 warning，可能生成能力不完整的 APK

位置：[build-ffmpeg-android.sh](../scripts/build-ffmpeg-android.sh:498-516)

FFmpeg `.so` 缺失会最终失败，但 x264/x265/vpx 缺失只输出 warning。这样可能构建出一个能安装、但实际缺少 README 宣称编码器的 APK。

整改：

- 将必需的 `libx264.a`、`libx265.a`、`libvpx.a` 缺失改为 `die`；
- 构建结束后调用 FFmpeg/符号检查，确认 `libx264`、`libx265`、`libvpx-vp9` 编码器存在；
- 将支持的编码器写入构建 manifest；
- 在 README 中区分“构建配置支持”和“设备硬编可用”。

### P2-03：批量完成/失败通知复用同一个 ID

位置：[TranscodeService.kt](../app/src/main/java/com/proresforge/app/engine/TranscodeService.kt:143-156)、[TranscodeService.kt](../app/src/main/java/com/proresforge/app/engine/TranscodeService.kt:369-375)

所有终态通知都使用 `TERMINAL_NOTIF_ID = 1002`。批量任务完成时，后一个通知会覆盖前一个通知。

整改：

- 用稳定的任务 ID 生成通知 ID；
- 完成通知的 PendingIntent request code 也使用任务 ID；
- 或改为一个摘要通知，显示完成数、失败数和最近一条任务。

### P2-04：备份恢复后 SAF 输出目录可能失效

位置：[AndroidManifest.xml](../app/src/main/AndroidManifest.xml:14)

Manifest 设置 `android:allowBackup="true"`，但没有数据提取规则。`SharedPreferences` 中的 `output_tree_uri` 和 `filesDir/jobs.json` 可能被恢复到另一台设备，而原 SAF 持久化权限并不一定有效。

整改：

- 若不支持跨设备迁移，设置 `allowBackup="false"`；
- 或增加 `dataExtractionRules`，排除 SAF URI 和不应跨设备恢复的状态；
- 启动时验证 URI 权限，失效则清除并提示用户重新选择目录。

### P2-05：接口契约文档与代码存在漂移

位置：[INTERFACES.md](../docs/INTERFACES.md:26-29)、[INTERFACES.md](../docs/INTERFACES.md:152-179)、[INTERFACES.md](../docs/INTERFACES.md:233-240)、[INTERFACES.md](../docs/INTERFACES.md:276-280)

当前文档仍有以下不一致：

- CPP 文件清单、CMake 实际 `.cpp` 源文件和 header-only 文件的描述不一致；
- 文档写 4 个 JNI 函数，当前实际有 5 个 JNI 入口；
- 文档仍写 ProRes `profile=2`，代码已经使用具名 profile；
- 文档描述的 CMake `file(COPY)` 兜底已不存在；
- FFmpeg runtime 列表有遗漏 `postproc` 的旧表述；
- 普通 FpsMode 和 DNG 参数表没有完全跟随当前 model.kt。

整改：

- 以当前源码生成 JNI、CMake 源文件、runtime 库和 spec 键的清单；
- 每次契约变更同时更新 README、INTERFACES 和构建脚本注释；
- 在 CI 中加入简单一致性脚本，检查 CMake 源文件、JNI 声明和文档清单。

### P3-01：ZIP64 元数据错误处理仍不完整

位置：[dngzip.cpp](../app/src/main/cpp/dngzip.cpp:277-305)、[dngzip.cpp](../app/src/main/cpp/dngzip.cpp:355-381)

ZIP64 extra 读取失败时目前只是跳过解析，没有立即记录条目名、偏移和所需字段。对于使用 ZIP64 哨兵值的条目，错误会延迟到 `read_frame()`，诊断信息不够明确。

整改：

- `z64_off`、central directory offset/size、local header offset 做完整范围校验；
- 需要 ZIP64 字段但 extra 读取失败时立即失败；
- 对条目名称、entry offset、required field 输出结构化错误日志；
- 增加损坏 ZIP64 extra 和截断 central directory 的测试样本。

### P3-02：线程栈配置返回值未检查

位置：[dngframeq.h](../app/src/main/cpp/dngframeq.h:190-208)

代码调用 `pthread_attr_setstacksize()`，但没有检查返回值。若设置失败，线程可能使用系统默认栈，代码注释却仍声称使用 8 MB。

整改：

- 检查 `pthread_attr_setstacksize()` 返回值；
- 失败时记录错误并选择明确的 fail-fast 或可靠降级策略；
- 在真机日志中打印实际线程策略；
- 保留当前 LibRaw 堆分配作为额外防护。

### P3-03：入队成功后立即解除 busy，允许后台运行期间重复提交

位置：[HomeViewModel.kt](../app/src/main/java/com/proresforge/app/ui/HomeViewModel.kt:407-415)

入队成功后 HomeViewModel 立即 `busy=false`。因此用户可以在上一批还在后台转码时再次提交同一批文件，输出会通过 `(2)`、`(3)` 去重。

这可能是产品允许的行为，但需要明确选择：

- 若允许追加队列，应在 UI 说明“可继续添加任务”；
- 若不允许重复提交，应根据 JobBus 中是否存在 QUEUED/RUNNING 任务禁用按钮；
- 若只禁止同一批重复，应计算输入 Uri 集合和任务参数的重复键。

## 整改优先级与实施计划

### 第一阶段：状态和数据正确性

目标：避免错误 facts、错误状态和错误 spec 产生实际输出。

1. 修复 `onFilesPicked()` 批次取消、generation 和 Uri 匹配。
2. 重写 JobBus 状态转换，补充取消/收尾终态守卫。
3. 删除 `buildSpec()` 的残缺 spec fallback，异常直接失败。
4. 为以上三项增加单元测试和并发时序测试。

完成标准：

- 快速连续选取文件不会串 facts；
- 取消不能被 DONE 覆盖；
- spec 生成失败不会调用 nativeRun；
- 输出文档不会遗留空文件。

### 第二阶段：native 数据完整性

目标：确保“成功”意味着音视频数据完整，预览与成片一致。

1. 修复音频 FIFO、BSF、解码器和编码器 flush 的错误处理。
2. 使 DNG 预览接收并使用质量、白平衡、色彩映射参数。
3. 增加损坏包、截断文件、BSF 失败、音频 FIFO 分配失败的测试。
4. 比较 DNG 预览帧与单帧转码输出的颜色和方向。

完成标准：

- 非 EAGAIN/EOF 的 native 错误会使任务 FAILED；
- 音频包不被静默丢弃；
- DNG 预览和实际转码使用相同参数；
- 失败输出被删除或明确保留为失败残片，不被标记为成品。

### 第三阶段：性能、构建和发布合规

目标：让构建可重复，资源预算可解释，文档可作为真实契约。

1. 固定 FFmpeg、x264、x265、libvpx 和 LibRaw 版本。
2. 将必需依赖缺失从 warning 改成构建失败。
3. 决定并落实 DNG 内存验收口径。
4. 修正 Manifest 备份策略。
5. 同步 INTERFACES、README、prebuilt README 和脚本注释。
6. 增加 arm64 真机回归矩阵。

## 建议测试矩阵

### Kotlin/JVM

- 快速连续选择两批文件，验证 generation 和 Uri 匹配；
- 探测失败、探测取消、旧批次晚到结果；
- JobBus：QUEUED 取消、RUNNING 取消、取消与 finish 并发；
- `buildSpec()`：四种输出格式、DNG zip、LUT、音频、10-bit、Apple Log；
- buildSpec 异常时不入队、不调用 nativeRun；
- OutputDir 命名、重名、超长 Unicode 和 provider 改名。

### Native/主机

- FIFO 写入短写和重新分配失败；
- AAC、BSF、flush 的正常和异常返回值；
- 视频损坏包不能静默跳过；
- ZIP64 extra 截断、偏移越界、central directory 截断；
- DNG 帧尺寸不一致、单帧解码失败、取消和线程创建失败。

### arm64 真机

- 普通 `.mov` / `.mxf`：ProRes 422、422 HQ、音频 copy/AAC/drop；
- DNG zip：FAST/BALANCED/FINE、CAMERA/AUTO/D65、普通色彩/Apple Log；
- 1080p、4K、6K/8K 内存和耗时；
- 转码中取消、服务被杀、通知、历史记录和残片清理；
- 无通知权限、SAF provider 不支持持久权限、输出目录权限失效；
- 批量任务每个终态通知和历史记录是否独立正确。

## 发布前验收清单

- [ ] 当前源码可在干净环境完成 `assembleDebug`。
- [ ] `testDebugUnitTest` 在当前源码上重新执行并全绿。
- [ ] arm64 真机完成普通 ProRes 端到端转码。
- [ ] arm64 真机完成 DNG zip 端到端转码。
- [ ] 取消不会被 DONE 覆盖。
- [ ] 任务失败时不会静默丢音频或视频帧。
- [ ] DNG 预览参数与实际转码一致，或 UI 明确标注近似预览。
- [ ] DNG 内存预算和文档验收标准统一，并有真机数据。
- [ ] 所有 native 上游依赖固定到 tag/commit。
- [ ] 必需 native 依赖缺失会使构建失败。
- [ ] `allowBackup`/`dataExtractionRules` 策略已经确定并验证。
- [ ] `README.md`、`LICENSE`、`INTERFACES.md`、`prebuilt/README.md` 与代码一致。

