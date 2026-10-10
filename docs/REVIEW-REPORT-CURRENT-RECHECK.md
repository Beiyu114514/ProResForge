# ProResForge 当前只读复审报告

复审时间：2026-10-06（Asia/Shanghai）  
复审方式：当前工作区文件级静态复核。  
范围：Kotlin/JNI/native、FFmpeg 状态机、DNG ZIP、Service/队列、Release 构建门禁、预编译供应物、测试与文档契约。  
限制：本轮没有修改源码，没有运行 Gradle、CMake、shell 脚本或任何构建/测试；工作目录不是 Git 仓库，不能做提交差异审查。

## 结论摘要

当前源码已经闭环了上一轮最严重的普通媒体错误收尾、DNG ZIP 边界、队列终态锁和 Service 正常销毁路径。当前仍值得处理的主要问题集中在预览路径的资源上限和 FFmpeg 错误语义、Release 供应物门禁的完整性、生成元数据漂移，以及缺少 native/端到端/真机验证证据。

本报告只把能从当前文件直接证明的事项列为发现；“没有测试证明”单独标为验证缺口，不把历史报告中已经修复的事项重新计入缺陷。

## Findings

### P2-01 预览输出没有 native 侧容量上限，JNI 还存在 `size_t` 到 `jsize` 的未校验转换

证据：

- `app/src/main/cpp/preview.cpp:94-104` 的 `mem_write()` 对 `std::vector` 直接 `insert`，没有最大 JPEG 大小、容量预检或 `std::bad_alloc` 转换。
- `app/src/main/cpp/preview.cpp:197-240` 的 DNG 预览先把原始帧解码到完整 `rgb` 和 `AVFrame`，`app/src/main/cpp/preview.cpp:319-336` 才按 `max_w` 缩放；因此 `max_w` 不能限制前置解码峰值内存。
- `app/src/main/cpp/native-lib.cpp:640-661` 接受任意 `jint maxW`，不验证范围；`jpeg.size()` 直接转换为 `jsize` 后调用 `NewByteArray` 和 `SetByteArrayRegion`。
- 当前 UI 调用点 `app/src/main/java/com/proresforge/app/ui/HomeViewModel.kt:379-383` 传入固定 `PREVIEW_MAX_W`，常量在 `app/src/main/java/com/proresforge/app/ui/HomeViewModel.kt:542-543` 为 640。这降低了正常 UI 路径的风险，但不能替代 JNI/native 边界校验，也不能限制完整 DNG 解码的峰值。
- `app/src/main/java/com/proresforge/app/engine/FFmpeg.kt:95` 把 `maxW` 文档定义为“最长边”；但 `app/src/main/cpp/preview.cpp:319-336` 只比较 `dw > max_w` 并限制宽度。未旋转的竖屏素材高度大于宽度时，输出最长边仍可远超 640。

影响：异常大图、恶意或损坏输入、未来其他调用方传入超大 `maxW` 时，native 可能持续扩大输出 vector 或在完整 DNG 解码阶段产生过高内存压力。超出 `jsize` 可表示范围时，窄化转换会得到错误长度；分配异常也没有被转换成可控的失败返回。异常跨 JNI 边界传播的行为不能作为可靠错误处理。

建议：在 nativePreview 入口对 `maxW` 设置合理上下限；在 `mem_write()` 中以明确的 JPEG 上限做 checked-add/容量检查，失败返回 `AVERROR(ENOMEM)`；捕获分配异常并统一返回空结果；在 JNI 侧先验证 `jpeg.size() <= INT_MAX`，再创建数组。对 DNG 预览增加输入尺寸/解码预算或在解码前做可拒绝的资源检查。

需要验证：超大分辨率 DNG、超大但合法的 JPEG、截断媒体、内存紧张设备，以及 JNI 返回失败时 Java 层的行为。

### P2-02 预览解码和 MJPEG flush 把所有负返回都当作普通结束，且可能丢弃 `EAGAIN` 对应的 packet

证据：

- `app/src/main/cpp/preview.cpp:285-310` 中 `avcodec_send_packet(dec, pkt)` 的任意负值都会 `unref` packet 后 `continue`，没有区分 `EAGAIN`、`EOF` 和真实错误；若是 `EAGAIN`，按 FFmpeg send/receive 契约应先排空并重送同一 packet，不能直接丢弃。
- 同一段 `app/src/main/cpp/preview.cpp:298-309` 对 `avcodec_receive_frame()` 的所有负值只 `break`，真实解码错误不会被记录或区分为失败原因。
- `app/src/main/cpp/preview.cpp:451-463` 忽略 `avcodec_send_frame(enc, nullptr)` 的返回值，并把 `avcodec_receive_packet()` 的所有负值都当作结束；只有 `out_pb->error` 被检查。

影响：普通素材通常可能仍能得到预览，但特定解码器、截断包或编码器延迟场景可能丢帧、返回旧候选帧、静默生成不完整 JPEG，或者把真实错误误报为正常结束。该路径与主转码路径已经采用的 EAGAIN/EOF/真实错误分层不一致。

建议：复用主转码路径的 send-and-drain 语义：只把允许的 `EAGAIN`/`EOF` 当控制流，真实负值记录上下文并失败；`send_packet` 返回 `EAGAIN` 时排空并重送原 packet；flush send 和 receive 都显式处理返回值。为预览补充截断视频包、损坏压缩帧和编码器 flush 的回归样本。

需要验证：不同 FFmpeg 视频解码器的 delayed-frame 行为、截断/损坏视频包，以及 MJPEG encoder 的 flush 返回值。

### P2-03 Release/CMake 门禁未复核 manifest 的摘要和来源 pin

证据：

- `app/build.gradle.kts:254-288` 检查必填字段、`manual`、ABI、target、`LIBRAW_NOTHREADS` 字符串和 `libraw.a` 文件 SHA；但没有把 `api_level`、`ndk_revision`、`libraw_tag`、`libraw_cmake_sha` 或 `script_sha256` 与当前构建配置/脚本比较。
- `app/build.gradle.kts:289-290` 对 `symbols_sha256` / `members_sha256` 只检查长度或非空，不运行 `llvm-nm` / `llvm-ar` 复算，也不比较复算结果。
- `app/src/main/cpp/CMakeLists.txt:201-261` 虽解析上述来源字段，但主要要求非空并核 ABI、raw_r/nothreads/flags 与 `libraw.a` SHA；不复算 symbol/member 摘要，也不核对来源 pin。
- `scripts/verify-prebuilt.sh:77-89`、`scripts/verify-prebuilt.sh:130-157`、`scripts/verify-prebuilt.sh:269-297` 已实现摘要复算和期望 pin 对账，但该独立校验没有成为 Gradle Release 门禁的一部分。

影响：当前两份 manifest 的字段和 `libraw.a` SHA 与当前脚本/文件相符，但 Release/CMake 构建路径本身不能证明供应物来自当前 LibRaw tag、NDK/API 和构建脚本，也不能证明 symbol/member 摘要仍与库内容相符。携带自洽旧来源字段及其对应库的 prebuilt 可能通过现有门禁。

建议：让发布门禁执行与 `verify-prebuilt.sh` 完全一致的 pin 对账及 symbol/member 复算，或在可用 NDK 工具链的 CI/release job 中强制运行该脚本并把结果作为构建前置条件。若 Windows Gradle 不适合直接调用 shell，应增加等价的受控 host 校验任务，而不是只检查字段非空或摘要字符串形状。

### P2-04 BUILDINFO、构建错误提示和文档仍描述旧的手工替换状态

证据：

- 当前 `app/src/main/cpp/prebuilt/BUILDINFO:5` 记录脚本 SHA 为 `8fd70f1e8874219c62d9f18c6b6453c695388aebdad92f02f0f8569d20fadca3`；当前 `scripts/build-ffmpeg-android.sh` 的只读 SHA 核对值为 `8339285ff2c739e5ce81dbb760192b982921dc2bedc3fe879411814a1925e8bb`（大小写不影响比较，摘要值不同）。
- `BUILDINFO:19-26` 一方面说明库已改为线程安全 `raw_r`，另一方面仍写着“本次为手工替换(未重跑整个脚本)”。当前两个 per-ABI manifest 的 `manual=false`，并且 `libraw_sha256` 与当前两个库逐字节 SHA 相符：arm64 为 `566fc1edd6fa85b506f3747f8c7855b62d7ef966a94a952e74ed89dd70f486b6`，armeabi-v7a 为 `7013419593ded6f698a1045fa6243d70d0797e9f24d6fdf9910f1eba1e5235c0`。
- `app/src/main/cpp/prebuilt/README.md:181-187` 仍称当前副本没有 manifest、是手工替换并应预期被拒绝；`docs/INTERFACES.md:210-213` 也保留同样的“当前副本状态”；`app/build.gradle.kts:302-304` 的门禁失败信息固定写着当前 prebuilt 没有有效 manifest。

影响：当前实际 provenance 已比这些文字新，但发布人员可能依据旧 BUILDINFO/README 误判构建不可复用，或忽略真正的脚本 SHA/构建时间；错误提示在发生其他门禁失败时也会给出不正确的根因。供应物、脚本与文档之间失去单一可信来源。

建议：用当前脚本在有效产物生成后重新生成 BUILDINFO，并同步 README、INTERFACES 和 Gradle 错误提示；构建门禁应从 manifest/BUILDINFO 动态报告实际状态，不要把“当前副本是手工替换”硬编码进通用失败信息。把 BUILDINFO 的脚本 SHA、每 ABI manifest 和库 SHA 一并作为发布输入检查。

### P2-05 缺少 native/Android 端到端测试和 CI 门禁

证据：

- 当前 `app/build.gradle.kts:479-481` 只声明纯 JVM 的 JUnit 依赖。
- 当前 `app/src/test` 仅发现 `FormatCatalogTest.kt`、`HistoryItemSerializationTest.kt`、`DngSpecMappingTest.kt` 三个数据/序列化类测试；未发现 `.github/workflows/*`。
- 因此没有仓库内可见的 native 测试、instrumentation 测试、真实 SAF fd 流程、预览编码回归或媒体损坏样本的自动执行门禁。

影响：本轮确认的 FFmpeg 预览状态机、DNG ZIP 解码、JNI 资源边界、Service 取消/销毁竞态以及双 ABI 真实加载均没有自动回归证据。后续修改很容易只在人工设备上发现问题，或因没有 NDK/prebuilt 的 CI 复验而产生供应物漂移。

建议：建立最小 CI：JVM 单测、契约检查、有效 manifest/SHA 校验、至少一个 ABI 的 Debug/Release configure（使用有效 prebuilt）；再增加 host native 测试或 instrumentation/e2e，覆盖截断包、DNG ZIP 边界、预览、取消、输出 fd 和队列历史。真实 arm64 设备验收应单独覆盖多线程 DNG、内存预算、色彩元数据和强制停止。

### P2-06 Service 销毁时可能跳过已取消排队任务的历史补录

证据：

- `app/src/main/java/com/proresforge/app/engine/TranscodeService.kt:98` 在 `serviceScope` 中启动 `runQueue()`；`app/src/main/java/com/proresforge/app/engine/TranscodeService.kt:116-133` 的 `onDestroy()` 取消该 scope，并只对 `currentId ?: activeRun.id` 调用 `finalizeInterrupted()`。
- `app/src/main/java/com/proresforge/app/engine/TranscodeService.kt:198-205` 对已终态（包括排队期 CANCELLED）的任务补写历史，位于 `runQueue()` 主循环之后。
- 当前运行任务路径 `app/src/main/java/com/proresforge/app/engine/TranscodeService.kt:222-223` 使用可取消的 `withContext(Dispatchers.IO)`；若销毁导致当前 runQueue 子协程取消，控制流可能在主循环退出前取消，或在抵达循环后的补录前终止。独立的 `finalizeInterrupted()` 只写当前任务，不遍历其他已取消排队任务（`app/src/main/java/com/proresforge/app/engine/TranscodeService.kt:386-418`）。

影响：Service 生命周期结束但进程仍存活时，如果队列中已有被取消的任务而另一个任务仍在执行，当前任务的 finalizer 不覆盖这些排队项，runQueue 的尾部补录也可能因 scope 取消而未执行；这些取消结果可能没有写入历史。

建议：将 runQueue 的终态历史收尾放进 `finally`，并从 `finalizeScope` 或 `NonCancellable + Dispatchers.IO` 执行；销毁时快照所有终态但尚未记录的任务，避免只收尾 `currentId`。用“一个任务运行、另一个排队任务已取消、随后销毁 Service”的测试覆盖。

### P2-07 发布包的第三方许可证/源码提供物在仓库中缺少可核对的独立文件

证据：

- `LICENSE:23-32` 只在顶层许可证中说明 FFmpeg/x264/x265 与 LibRaw 义务，并要求 LibRaw 许可证文本、声明和源码获取方式随二进制提供。
- `README.md:160-169` 说明当前构建启用 GPL、静态链接 LibRaw，并要求发布时随附许可证文本、版权声明和源码获取方式。
- 当前工作区文件巡查只发现顶层 `LICENSE`，没有独立的 `NOTICE` / `COPYING` 文件可供发布流程核对；`app/build.gradle.kts:445-447` 只配置了 META-INF 资源排除，没有看到第三方 notice 打包或源码 offer 产物生成规则。

影响：源码仓库的说明已经意识到许可证义务，但不能从当前发布流程证明 APK 或发行目录实际包含完整许可证文本、版权声明和对应源码 offer。该项是发布合规证据缺口，不能仅靠 README 文字替代。

建议：建立可审计的第三方 notices/licenses 目录或发布任务，明确 GPL/LGPL/LibRaw/x264/x265/libvpx 的文本、版权、对应源码/offer URL 与 APK 发布物之间的关系；在 CI 中检查其存在并在发布包验收中确认可访问。

## 已确认修复或当前不再重报

- `app/src/main/cpp/transcode.cpp:2180-2313` 已将普通输入结束限定为 `AVERROR_EOF`，对 decoder/BSF 的 EAGAIN 泵送和真实负错误进入失败收尾；`:2317-2425` 的 filter/encoder flush 和 `:2480-2591` 的错误收尾也已检查。
- `app/src/main/cpp/dngzip.cpp:315-430`、`:591-630`、`:693-778` 已有 ZIP64、中央目录、local header、数据范围、声明尺寸和实际消费量检查；旧报告的边界缺陷不能按当前代码重报。
- `app/src/main/java/com/proresforge/app/engine/JobBus.kt:90-123,134-201` 的 cancel/begin/finish/clearFinished 使用同一锁，终态保护已存在。
- `app/src/main/java/com/proresforge/app/engine/TranscodeService.kt:145-205,381-418` 已补齐队列终态历史、ActiveRun 完成闸门和正常销毁时的输出删除顺序；当前保留的是 Service 销毁时排队取消任务的历史补录窗口，而不是普通路径锁缺失。
- 两个当前 manifest 都记录 `raw_r_target=raw_r`、`libraw_nothreads=absent`、`manual=false`，且记录的库 SHA 与当前文件相符。因此本轮不把“缺 manifest/仍是单线程 LibRaw”列为当前缺陷；遗留问题是 BUILDINFO/文档滞后和 Release 门禁未复算 symbol/member 摘要。

## 未验证范围

1. 未运行 Gradle、CMake、`scripts/verify-prebuilt.sh`、契约脚本或任何测试，不能声称构建/门禁实际通过。
2. 未在真机加载 APK，未验证两个 ABI 的 JNI 符号、动态库打包、FFmpeg 运行时加载、DNG 并发稳定性、内存峰值或 Apple Log 色彩元数据。
3. 未构造截断容器、损坏视频、超大 DNG/JPEG、写满存储，以及“活动任务期间 Service 销毁但进程继续存活”的队列场景；P2-01、P2-02、P2-06 的运行时触发条件仍需这些样本确认。
4. 当前工作区不是 Git 仓库，无法判断上述文件是否已在某个提交中、也无法做本轮与基线的差异审查。

## 建议顺序

1. 先给 preview native/JNI 加输出和尺寸预算，并统一 send/receive/flush 错误语义。
2. 把 symbol/member 摘要复算接入 Release/CI 门禁，同时重新生成 BUILDINFO 并清理旧文档口径。
3. 建立最小 CI 与 native/Android 媒体回归矩阵，再做 arm64 真机、Service 销毁队列历史和进程强杀恢复验收。
4. 在发布任务中固化第三方许可证、版权、源码 offer 和 notice 文件的可核对产物。
