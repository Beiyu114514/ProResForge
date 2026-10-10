# ProResForge 只读审查报告

审查日期：2026-10-07  
审查范围：当前工作区的 Kotlin/Compose、Android Service/数据层、native C++、DNG ZIP 解析、Gradle/构建脚本、契约文档与测试布局。  
审查方式：静态逐文件复核，并与 2026-10-06 两份审查报告对照；本轮不修改源码、不执行构建或测试。

## 结论

项目的 JNI fd 所有权、SAF 输出、任务终态守卫、DNG 预览参数传递和 DNG 队列等关键路径已有明显整改。当前仍不建议把本工作区作为稳定版发布，主要原因是 native 转码在普通容器路径仍可能把解码或 flush 错误当成成功，以及 DNG ZIP 解析对恶意/损坏元数据的整数和范围校验尚未闭环。

最高优先级是修复音视频数据完整性问题，并用损坏媒体做端到端验证。该问题不是本轮新发现：它已经出现在 `docs/REVIEW-REPORT-2026-10-06.md:126-147` 和 `docs/AUDIT-2026-10-06.md:93`，本轮复核确认当前实现仍未闭环。

## 严重问题

### P1-01：普通容器解码错误仍会静默丢帧/丢音频

位置：[`transcode.cpp`](../app/src/main/cpp/transcode.cpp:1845-1916)

证据：

- EOF 冲刷视频和音频解码器时，`avcodec_send_packet(dec, nullptr)` 与 `avcodec_send_packet(adec, nullptr)` 的返回值未检查（:1858-1859）。
- 视频包发送失败后直接 `continue`（:1874-1878）：`ret < 0` 的所有返回值都会被当作当前包可丢弃。
- AAC 解码路径有同样行为（:1910-1916）。
- 后续只要剩余数据还能编码，主流程仍可继续写 trailer 并走成功收尾（:1928-2014）。

损坏包、解码器内部错误或异常输入因此可能生成缺视频帧或缺音频的可播放文件，而 Kotlin 侧仍收到成功结果。`EAGAIN`/`EOF` 可以作为解码状态机控制流处理，但真正的负错误必须带流类型和包位置进入失败收尾；EOF flush 的 send 也必须明确处理。

建议：统一封装视频/AAC 的 send-and-drain 逻辑，区分 `EAGAIN`、`EOF` 与真实错误；真实错误设置非零结果、删除或明确标记残片，并补充截断视频包、损坏 AAC 包和存储写满样本。

### P1-02：编码器 flush 的 send 错误被忽略

位置：[`transcode.cpp`](../app/src/main/cpp/transcode.cpp:1928-1981)

视频和 AAC 编码器 flush 时分别调用 `avcodec_send_frame(..., nullptr)`（:1943、:1965），但返回值完全未检查。receive 阶段虽然把除 `AVERROR_EOF` 外的负值转为失败（:1945-1971），但 send 已经失败时可能没有正确排空编码器，造成尾帧/尾音频缺失。

建议：对 flush send 检查返回值；仅按 FFmpeg 状态机允许的 `EAGAIN`/`EOF` 处理，其他错误直接失败。为视频编码延迟、AAC 尾帧和 flush 写盘失败增加 native 回归测试。

### P1-03：AAC BSF 的 receive 错误被当作正常结束

位置：[`transcode.cpp`](../app/src/main/cpp/transcode.cpp:1883-1905)

AAC 直通路径已经对 `av_bsf_send_packet()` 的真实错误跳转失败（:1884-1891），但 `av_bsf_receive_packet()` 的任意负返回都只触发 `break`（:1893-1900）。这无法区分正常的 `EAGAIN`、正常结束的 `EOF` 和过滤器损坏/参数错误。

影响是需要 `aac_adtstoasc` 的音频可能在中途被截短，而任务仍继续完成。建议只把明确允许的控制返回当作循环结束，其余返回值进入失败收尾，并记录 BSF 名称、流索引和错误码。

## 中等严重问题

### P2-01：DNG ZIP64 和条目边界校验仍不完整

位置：[`dngzip.cpp`](../app/src/main/cpp/dngzip.cpp:277-421、:454-518)

本轮确认旧 P3-01 只部分闭环：ZIP64 extra 读取失败现在会记录并返回错误（:367-379），无名中央目录项也已正确推进 `pos` 后再继续（:353-361）；但以下边界缺口仍存在：

- ZIP64 EOCD 检查使用 `z64_off + 56 > m_len`（:293），加法本身可能在 `uint64_t` 上溢出。
- ZIP64 解析后只检查 `cd_off > m_len`（:311-314），没有验证 `cd_off + cd_size <= m_len`。
- 中央目录循环用 `pos += 46 + nlen + xlen + clen`（:419-421），没有 checked addition；`cd_size` 也没有作为解析上限使用。
- 本地头数据偏移用 `ent.lho + 30 + nlen + xlen`（:483-488），没有逐项检查加法溢出。
- stored 条目按 `ent.usize` 读取（:503-518），没有同时约束 `ent.csize` 与 `avail`（:490-491）。损坏条目声明的长度可能使读取跨越条目边界并消费后续 ZIP 区域；`kMaxFrameUsize`（:72-74、:493-499）降低了内存 DoS，但没有替代完整范围校验。

目前更可能表现为拒绝解析或错误解压，未据此断言存在已证明的任意代码执行；但这是文件解析器的健壮性和资源边界问题。建议加入 checked-add/helper、以 `cd_off` 和 `cd_size` 限制中央目录、验证 `csize <= avail`（stored 还应要求 `usize == csize`），并增加 ZIP64 偏移溢出、截断目录、伪造 stored 尺寸和超长 extra 的测试样本。

### P2-02：Service 销毁时历史落库仍是异步 best-effort

位置：[`TranscodeService.kt`](../app/src/main/java/com/proresforge/app/engine/TranscodeService.kt:106-124、:349-365)

`onDestroy()` 先调用 `nativeCancel()`，再取消 `serviceScope`，然后调用 `finalizeInterrupted()`（:106-116）。终态在 JobBus 中会被标为 `CANCELLED`，输出会删除，但 `jobStore.recordFinish(...)` 通过独立的 `finalizeScope.launch` 异步执行；如果进程在协程落盘前被系统杀死，历史记录可能丢失。此前的“幽灵 RUNNING”问题已增加补偿收尾，当前残留是落库可靠性问题，而不是旧问题原样复现。

建议：为中断收尾提供同步、可确认的持久化路径，或把待落库的取消记录写入可恢复的 journal，再由下次启动补偿；补充 Service 被销毁/进程被杀后的历史一致性测试。另需确认 nativeCancel 返回前 native 是否已停止写输出，避免删除输出与 native 写盘并发。

### P2-03：`clearFinished()` 没有使用 JobBus 状态机锁

位置：[`JobBus.kt`](../app/src/main/java/com/proresforge/app/engine/JobBus.kt:36-37、:90-121、:132-199)

`cancel()`、`begin()`、`finish()` 都在 `lock` 内完成状态转换和 `_currentId` 更新，但 `clearFinished()` 直接执行 `_jobs.update`（:115-121）。UI 清理历史与 Service 收尾并发时，清理可能基于旧快照重建 map，覆盖另一条刚完成的更新，或导致终态记录未按预期移除/保留。

这是并发一致性缺口，当前没有测试证明其必现，定级低于 native 数据完整性问题。建议让 `clearFinished()` 使用同一把锁，并增加 cancel/finish/clearFinished 交错测试；同时保持状态转换和 `_currentId` 更新的原子边界。

### P2-04：预览缓冲缺少明确的容量/分配失败边界

位置：[`preview.cpp`](../app/src/main/cpp/preview.cpp:98-112)

`mem_write()` 将输入追加到 `std::vector`，没有最大输出限制，也没有捕获分配异常。异常输入或超大预览输出可能导致异常向 JNI 边界传播，或产生过高内存压力。预览 fd cleanup 会关闭 fd（:466-490），这部分当前正常。

建议按预览尺寸和格式设置上限，在追加前做 checked size 计算；将分配失败转为明确的 native 错误码并在 JNI 层映射为失败，而不是依赖异常跨越 C/JNI 调用边界。

## 构建、测试与发布风险

### P2-05：行为测试无法覆盖当前高风险 native 路径

位置：[`FormatCatalogTest.kt`](../app/src/test/java/com/proresforge/app/data/FormatCatalogTest.kt:1-141)、[`check-contracts.sh`](../scripts/check-contracts.sh:1-135)

当前只有三个 JVM 测试类：`FormatCatalogTest`、`HistoryItemSerializationTest`、`DngSpecMappingTest`；没有 `androidTest` 或 native 测试。代表性的 `FormatCatalogTest` 主要验证格式枚举、码率档位、容器扩展名和音频选项（:14-141），不执行 FFmpeg、LibRaw、JNI、Service 状态机或 ZIP 解析。契约脚本只检查 JNI/CMake/runtime/spec 文本和清单一致性，不能证明媒体行为正确。

本轮未执行 build/test，因此没有把历史测试 XML 当作当前源码验证证据。建议最少增加：

- native 主机测试：ZIP64/截断/溢出边界、DNG 条目尺寸、解码错误与 flush 返回值；
- Android/instrumentation 测试：Service 销毁、取消、历史落库、输出残片清理；
- 端到端媒体样本：损坏视频包、损坏 AAC/ADTS、BSF、编码延迟尾帧、DNG FAST/BALANCED/FINE 与不同色彩模式；
- CI 中执行 JVM 单测、契约检查、native 测试和至少一个可用 ABI 的 assemble 检查。

### 仍需闭环的既有发布项

以下条目在现有报告中仍标为未完成，本轮没有将它们伪装成已解决：

- native 依赖和预编译产物的可重复性、构建脚本的 upstream pin 与产物 manifest：[`docs/AUDIT-2026-10-06.md`](AUDIT-2026-10-06.md:39-46、:70-73)。
- 缺少 CI、wrapper 完整性校验和仓库卫生：[`docs/AUDIT-2026-10-06.md`](AUDIT-2026-10-06.md:70-73)。当前工作区也不是 Git 仓库，无法进行提交差异审查。
- 契约/README/prebuilt 文档漂移：[`docs/AUDIT-2026-10-06.md`](AUDIT-2026-10-06.md:98-113)。
- release 未启用 minify/R8：`app/build.gradle.kts:101-109`；这不是当前 native 数据缺陷，但应在发布前决定并验证。
- DNG 内存预算与验收口径：[`docs/REVIEW-REPORT-2026-10-06.md`](REVIEW-REPORT-2026-10-06.md:171-187)。需要用 arm64 真机数据统一实现、注释和验收阈值。

## 已复核并排除的旧问题

以下项目在当前源码中已有对应整改，本轮不再列为现状缺陷：

- 探测批次已使用 generation/URI 回写和 `probed && facts != null` 的启动条件：`HomeViewModel.kt:157-235`。
- 取消终态已有守卫，`finish()` 不再把 `CANCELLED` 改回 `DONE`：`JobBus.kt:175-199`；Service 通知/历史读取最终 JobBus 状态：`TranscodeService.kt:143-180`。
- `buildSpec()` 异常直接失败、关闭 fd 并删除输出：`TranscodeService.kt:237-253`。
- DNG 预览已传入质量、色彩和映射参数；不再固定使用旧的 FAST/CAMERA：`FFmpeg.kt` 与 `preview.cpp` 当前接口。
- Manifest 已采用 `allowBackup=false` 和数据提取规则；Service 为非 exported：`app/src/main/AndroidManifest.xml:12-47`。
- 无名中央目录项已先推进目录位置再继续：`dngzip.cpp:353-361`。

## 修复优先级

1. P1：修复普通容器解码、BSF、EOF flush 和编码器 flush 的错误状态机；错误必须使任务 FAILED，不能只生成“尽量可播”的成功文件。
2. P2：完成 DNG ZIP checked arithmetic、中央目录范围和 stored 条目尺寸校验。
3. P2：把 Service 中断收尾的历史记录变成可确认/可恢复的持久化操作，并用同一锁保护 `clearFinished()`。
4. P2：建立 native/Android/端到端媒体测试矩阵和 CI；之后再处理可重复构建、文档漂移、R8 与内存口径。

## 审查边界

- 本轮只读审查，未修改源码、配置或既有文档；本文件是唯一新增文档。
- 本轮未执行 Gradle build、单元测试、instrumentation、native 测试或 arm64 真机测试。
- 工作区没有 `.git`，无法审查提交差异或确认文件是否来自当前分支。
- 因此，报告中的 native 结论是静态代码证据，尚未被运行时样本验证；发布前必须补齐上述测试。

---

## 第七轮复审:整改复证 + 运行时验证(2026-10-06 午间补记)

本节是对上文静态报告的第七轮复审补记:上午 09:29–10:37 的整改轮(4 个源码文件)经逐行亲读核实,并补上静态报告缺失的构建、单测与模拟器端到端验证。

### 上午整改轮逐项复证(全部落地)

- **P1-01 ✓** 视频/AAC send 循环:EAGAIN 泵帧、`AVERROR_EOF` continue、真实负错误 `goto loop_error`(`transcode.cpp:1958-2030`);EOF 冲刷 send 检查非 EOF 负值(`:1908-1926`);`av_read_frame` 仅 `AVERROR_EOF` 才进冲刷,其余 I/O 错误直接失败(`:1899-1903`)。
- **P1-02 ✓** 视频/AAC 编码器 flush:send EAGAIN 先排空再重送,最终返回非 EOF 负值失败;receive 非 EAGAIN/EOF 与写盘失败均置非零 result(`transcode.cpp:2056-2146`)。
- **P1-03 ✓** BSF drain 区分 EAGAIN/EOF/真实错误(`transcode.cpp:1936-1952`、`:1979-2000`)。
- **P2-01 ✓** ZIP64 EOCD 范围用减法判上溢(`dngzip.cpp:302-309`);`cd_off+cd_size` 越界与 checked_add 双保险(`:321-337`);CD 循环受 `[cd_off, cd_end)` 闭区间约束、条目长度/extra/filename 偏移全 checked(`:347-505`);本地头 data_off checked、`csize > avail` 拒绝、stored 要求 `csize == usize` 且不超 avail(`:574-614`)。
- **P2-02 ✓** `ActiveRun` CountDownLatch 完成闸门:nativeRun 返回/抛异常后才放行删除输出,消除写删竞态;`finalizeInterrupted` 历史先落独立 `finalizeScope`(不随 serviceScope 取消),onDestroy 有界等待 2s(`TranscodeService.kt:57-67`、`:116-140`、`:380-412`)。
- **P2-03 ✓** `clearFinished()` 已入 `synchronized(lock)`(`JobBus.kt:116-124`)。

### 第七轮新整改(本轮落地)

- **M-新A(第六轮遗留,一行级)✓** `runJob` 在 `nativeRun` 返回后统一补 `clearPendingCancel()`(`TranscodeService.kt:307-311`):堵住「nativeRun 成功返回 → finish 未调」窗口内用户取消把 pending 留给下一任务(开局即取消)的竞态。此前所有 clear 都只在 nativeRun 之前的早退路径。
- **L-新A(第六轮遗留)✓** `INTERFACES.md §8.1` JobSpec 声明补 `dngQuality/dngColor/dngMap` 三字段,与 `model.kt:121-142` 对齐。

### 构建与测试证据(本轮独立执行)

- `:app:assembleDebug` + `:app:testDebugUnitTest`(WSL Ubuntu-24.04,ubuntu 用户):**BUILD SUCCESSFUL**,单测 **22/22 绿**(DngSpecMappingTest 6 + FormatCatalogTest 12 + HistoryItemSerializationTest 4,0 失败 0 错误)。
- host `dngframeq` 测试(直连 `app/src/main/cpp` 编译):**5 场景 PASS**(next-before-start / drained / next-after-stop / cancel / decode-fail)。
- 交付物:`dist/ProResForge-1.0.0-debug-20261006-1100.apk`(62,081,324 字节),晚于最后一次源码改动(10:37)。
- **Windows AVD 端到端冒烟(medium_phone,google_apis x86_64,ARM 转译)**:安装 APK → SAF 选 `prf-smoke-4f.zip`(4 帧 DNG)→ 探测正确(4096×3072 · rgb48le · 4 帧 · bt709)→ MP4·H.264 转码 **DONE,6.7s,62.9MB → 3.1MB** → 输出 MP4 box 结构完整(ftyp/free/mdat/moov)→ 历史落库显示 Done → 服务正常销毁。全程 logcat 无错误。

### 仍欠(按优先级)

1. **P2-04 未修**:preview `mem_write` 仍无容量上限、无分配异常防护(`preview.cpp:98-104`)。
2. **P2-05**:native/端到端媒体测试矩阵与 CI 仍缺(本轮冒烟是手工执行)。
3. 第六轮遗留:M-新B(prebuilt 仍为 pin 前构建,按计划与下次 FFmpeg 升级合并重建)、L-新B(startTranscode 中途失败已建空输出文档不回收)、L-新C(dngframeq pthread_create 全失败仍降级 m_no_workers,与 L-6「拒绝降级」矛盾)、L-新D(runQueue 尾部 recordFinish 为 suspend,在已取消协程中抛 CancellationException → 排队取消任务历史丢失)。
4. 旧欠:L-17("HDR" 硬编码)、release minify/R8 未启用、arm64 真机端到端(含 Apple Log colr 9-2-9 目检)仍未做。

### 复审边界(本节)

- M-新A/L-新A 为本轮唯一源码/文档改动;其余均为只读复证。
- 冒烟未覆盖:取消路径(M-新A 修复的行为需「下一任务」才能观察到)、AAC 音频路径、Apple Log 映射输出(沿用 2026-10-06 凌晨验证结论)。
