# ProResForge 第十轮只读复审

日期：2026-10-07。范围：对照 `docs/REVIEW-REPORT-2026-10-07-ROUND9.md` 与 `docs/FIXLOG-2026-10-07-ROUND9.md`，只读核对当前源码。不改源码，不跑会改写产物的构建或测试。

结论先说：第九轮点名的功能性缺陷，绝大多数已经在源码里落地，而且落点和注释对得上。还没关的是预览缩放/内存、DNG 队列极端降级，以及发布与 GPL 对应源码。对应源码比 FIXLOG 写的更硬：版本号对得上 n6.1.6，编进 `libavcodec.so` 的发行串是 `n6.1`。现有 debug APK 里没有 `assets/licenses/`。本轮新看到的功能问题是：服务在转码已经成功返回、尚未记完成时被销毁，会把成品标成取消并删掉；另外还有几处无上限忙等、坏 zip 中央目录被当成短序列成功，以及预览里被 JNI 吃掉的 C++ 异常会漏关 fd。

## 已关闭（本轮亲读）

| 项 | 证据 |
|---|---|
| K-01 取消残留级联删下一任务 | `TranscodeService.kt:165` 在 `JobBus.begin` 之后、`runJob` 之前调 `clearPendingCancel()`；`nativeRun` 返回后再次清除。`native-lib.cpp:633-642` → `clear_cancel_pending()`，仅在 `!g_transcode_active` 时清 pending，不会把正在跑的取消撤掉。 |
| K-02 / P2-06 正常跑完时历史被取消吃掉 | `TranscodeService.kt:189-207`、`:217-224` 用 `NonCancellable` 包住单任务的 `recordFinish` 和结束通知。销毁窗口仍会删成品，见 R10-9。 |
| K-03 `startForeground` 打成进程 FATAL | `TranscodeService.kt:508-515` 捕获后 `stopSelf()`，进程不再 FATAL。队列仍会启动，见 R10-10。 |
| K-04 入队失败只撤 QUEUED | `JobBus.kt:139` `removeQueued`；`HomeViewModel` 失败路径先撤队列再 `deleteCreated`。 |
| K-05 坏文件无法从批次剔除 | `HomeScreen.kt:238-240` → `HomeViewModel.kt:418-419` `removeFile`。 |
| K-06 销毁时误等已经结束的 native | `TranscodeService.kt:137` 把 `nativeActive` 传进 `finalizeInterrupted`；`:454` 仅在仍 active 时等待。 |
| K-08 拖滑条风暴式重预览 | `HomeScreen.kt:152-167` 先 `delay` 再进 `Dispatchers.IO`。 |
| K-11 批量建输出重复枚举 SAF | `OutputDir.kt:124` `outputDocsFor` 一次快照目录名。 |
| L-新B 失败前缀留下 0 字节输出 | `OutputDir.kt:134` `deleteCreated` 走 `DocumentsContract.deleteDocument`。失败项之后已经建成的文档仍会留下，见 R10-11。 |
| L-17 HDR 无提示 | `HomeScreen.kt:326` `media_hdr_hint`。 |
| N-01 预览解码把 EAGAIN 当丢包 | `preview.cpp:286-338` send/receive 分开，EAGAIN 保留同一包。 |
| N-02 预览编码失败仍当成功 JPEG | `preview.cpp:489-516` 分层 flush；失败 `result.clear()`。截断防护本身成立，见下方新问题。 |
| N-03 预览内存无上限 | `preview.cpp:57-60` `kPreviewMaxBytes = 64MB`；`native-lib.cpp:688-700` JNI 边界接住 `std::exception`。 |
| N-04 空预览与失败返回值 | `native-lib.cpp:703-708` 空 vector 返回 null，并拒绝超过 `INT32_MAX`。 |
| N-10 预览 LUT 路径 OOM 被当成「无 LUT」 | `native-lib.cpp:683-684` `GetStringUTFChars` 失败直接返回 null。 |
| S-2 源码树缺许可证文本 | 源码树 `app/src/main/assets/licenses/` 已有 NOTICE 与 Apache-2.0 / BSD-3 / CDDL / GPL-2.0 / GPL-3.0 / LGPL-2.1 / MIT / Zlib。这只说明树里有文件。随包与 offer 见下方，S-2 不能算关闭。 |
| S-3 `jobs.json` 损坏被静默清空 | `JobStore.kt:106-127` 先 `copyTo(jobs.json.corrupt-<时间戳>)` 再记 `AppLog.w`。历史仍从空列表重建，没有恢复入口，这是残留，不是静默丢失。 |
| 32 位 `off_t` 截断（既有根因） | `dngzip.cpp:38-51` 与 `transcode.cpp:309-312` 明确不定义 `_FILE_OFFSET_BITS=64`，zip/长度探测走 `pread64` / `lseek64` / `off64_t`。本轮只核对源码，没有重编 armeabi-v7a 产物。 |
| LibRaw 栈上巨对象 | `dngzip.cpp` 解码实例改为 `thread_local unique_ptr<LibRaw>`，不再把约 750KB 的 `LibRaw` 放在栈上。 |

版本头与 pin 文本一致（本轮重读）：`libavcodec/version_major.h` `LIBAVCODEC_VERSION_MAJOR 60`，`version.h` minor 31 / micro 102；`x264.h` `X264_BUILD 165`；`x265_config.h` `X265_BUILD 216`；`vpx.pc` `Version: 1.17.0`。两边 ABI 相同。`scripts/build-ffmpeg-android.sh` 的 SHA-256 为 `683d34540d3847231c73547fde51bf8be36e112379c00c648b17abdc02e2bb4b`，与 `prebuilt/BUILDINFO:7` 相同。

## 仍开放

### S-1 发行串是 n6.1，不是 pin 上的 n6.1.6（合规）

版本数字对齐，发行身份不对齐。

- `scripts/build-ffmpeg-android.sh:68` pin 为 `n6.1.6`。`:62-67` 自己写明：这是按现存产物的版本头去贴 tag；二进制由更早的脚本打出，commit 归属等下次完整重建。
- 随包头文件 `prebuilt/arm64-v8a/include/libavutil/ffversion.h:4` 是 `#define FFMPEG_VERSION "n6.1"`。`ffversion.h` 由构建时的 `version.sh` 生成，不在 FFmpeg git 树里。干净的 n6.1.6 构建应写入 `n6.1.6`。
- 本轮把 `prebuilt/arm64-v8a/lib/libavcodec.so` 拷到本地后搜字符串：有 `FFmpeg version n6.1` 和 `Lavc60.31.102`，没有 `n6.1.6`。该文件时间戳是 1981-01-01，同目录其他库是 2026-10-04，内容仍是这套版本串。
- `libavcodec/version.h` 的 60.31.102、`X264_BUILD 165`、`X265_BUILD 216`、`vpx.pc` 1.17.0 与 pin 的版本号一致。脚本 SHA-256 `683d34540d3847231c73547fde51bf8be36e112379c00c648b17abdc02e2bb4b` 与 `BUILDINFO` 相同，只能证明清单抄了当前脚本的哈希，不能证明这份脚本产出了这些二进制。

`assets/licenses/NOTICE.md:17-47` 把 `n6.1.6@f1e3a2bf…` 写成随包二进制的对应源码，并以此做 GPL-3.0 §6 书面 offer。offer 指向「发布渠道提供的源码包」，没有具体 URL、要约人或至少三年的期限。指向的 tag 与二进制里的发行串不是同一个身份。完整重建并用 `verify_pin` 复核之前，这是发布阻塞。

### S-2 现有 APK 没有许可证，MIT 文本是模板（合规）

源码树有 9 个许可文件，界面也有入口。`dist/ProResForge-1.0.0-debug.apk`（64009808 字节，2026-10-06 22:48，945 个条目）里没有路径匹配 `licenses` / `NOTICE` / `LICENSE` 的条目。没有 release APK。`app/build.gradle.kts:458` 排除的是 jar 里的 `META-INF/{AL2.0,LGPL2.1}`，不删 assets；这份包只是在许可文件放进源码树之前打的。

`assets/licenses/MIT.txt:3` 仍是 `Copyright (c) <year> <copyright holders>`。应用自己的 MIT 在仓库根 `LICENSE`，不在 assets。应用内许可页会把占位符列出来。

### B-09 release 仍不可分发

`app/build.gradle.kts:429` `isMinifyEnabled = false`。`:495-497` 说明仓库没有 `signingConfig`，也没有 keystore。release native 优化参数有清单闸门，但包本身未混淆、未签名。

### N-07 预览只按宽度封顶

`preview.cpp:351-360`：旋转后的宽 `dw > max_w` 才缩小，高不参与比较。`HomeViewModel` `PREVIEW_MAX_W = 640`。宽 ≤ 640、高很大的素材不会缩小；JPEG 顶到 64MB 后被判失败，用户看到的是「没有预览」而不是一张缩小图。

### N-08 DNG 预览先全尺寸解码

`preview.cpp:175-242` 对目标帧 `read_frame` + `dng_decode_to_rgb48` 出全幅 RGB48，之后才进 `max_w` 缩放。4096×3072 一帧 RGB48 约 72MB，再加 LibRaw 峰值。预览档位已经跟转码 spec 对齐（N-7 已关），但没有半尺寸/预览专用上限。

### N-05 DNG 队列「全线程创建失败」仍降级到消费线程

`dngframeq.h:260-267` 在 `pthread_attr_setstacksize` 失败时正确返回 false。`transcode.cpp:2605-2612` 也把这次失败当成 IO 错误，不进主循环。

但 `dngframeq.h:275` 在 `pthread_create` 失败时直接 `break`，`:292-300` 在线程数为 0 时置 `m_no_workers = true` 并 **return true**。`next()` `:372-374` 随后在消费线程上 `produce_one()`。这和 `:260-261`「不要降级到不受控栈」的注释相反。LibRaw 对象已经上堆，旧的「栈上 767KB 必 SIGSEGV」不能再当成已证实；调用链栈深仍不受 8MB 保证。概率低（rlimit 把线程打光），路径还在。

### N-06 `m_started` 在失败返回前已置位

`dngframeq.h:249` `m_started = true` 在 attr 失败的 `:267` / `:281` `return false` 之前。当前调用方看到 false 就 `goto done`，不会再 `next()`。`start()` 开头会 `stop()`，同一对象重试能恢复。若失败后误调 `next()`，`:367` 的「未启动」防护失效，且 `m_no_workers` 仍为 false，会每 15 秒打一次无进展日志并一直等。残留，不是当前主路径。

## 新问题

### R10-1 预览解码忙等没有次数上限（M，条件触发）

`preview.cpp:296-338`。两处都会把循环条件保持为真，且没有任何计数：

1. `have_packet` 为真，`avcodec_send_packet` 返回 `EAGAIN`，紧接着 `avcodec_receive_frame` 也返回 `EAGAIN`。包不丢、`flushed` 仍为 false，`:338` 不退出。
2. `av_read_frame` 已 EOF，flush（`send_packet(nullptr)`）返回 `EAGAIN`，receive 也是 `EAGAIN`。`:303-305` 只有非 EAGAIN 才 `break`，`flushed` 保持 false，下一轮再次读到 EOF。

符合约定的解码器在 send EAGAIN 时 receive 应吐出一帧。这不是「每次预览都挂」。缺的是次数上限：一旦两边同时 EAGAIN，循环不会停。预览跑在 `Dispatchers.IO`（`HomeScreen.kt:161`），取消协程也取消不了已经进入的 JNI。

### R10-2 预览编码的防死循环守卫盖不住 flush EAGAIN（M，条件触发）

`preview.cpp:499-512`。`flushed` 只在 `send_frame(nullptr)` 返回 `>= 0` 或 `EOF` 时置真。flush 自己返回 `EAGAIN` 时 `flushed` 仍为 false，函数 `continue`。下一轮若 receive 仍是 `EAGAIN`，又走「尚未 flush」分支，`:511` 的 `enc_ok = false` 永远到不了。注释写的「flush 后再 EAGAIN 就停」与代码不一致。

另外 `:482` 把第一次 `avcodec_send_frame` 的任何 `< 0`（含 EAGAIN）直接 `goto cleanup`。空的 MJPEG 编码器首帧通常不会 EAGAIN，这是次要的。

### R10-3 成功返回后、`finish` 前的取消会留下文件但记成取消（L）

`runJob` 按 `nativeRun` 的返回码决定保留或删除输出；`JobBus.finish`（`TranscodeService.kt:173`）带终态守卫，不会把已经写成 `CANCELLED` 的状态盖回 `DONE`。取消若落在 native 已经 `end_transcode`、Kotlin 还没 `finish` 的窗口（含 SAF 上 stat/删文件），历史是取消，成功文件还在。K-01 的「下一任务开局就被删」已经堵住；这是同一窗口的另一侧，不级联。

### 其它低优先级

- `preview.cpp:331` `av_frame_ref` 的返回值未查。失败后 `cand` 已被 `unref`，该帧被跳过，可能落到更晚的帧或最终无预览。
- `native-lib.cpp:655` `initNativeLogging` 在 `GetStringUTFChars` 失败时没有显式失败路径（预览路径已修）。失败时 JNI 已挂起异常且后面没有 JNI 调用，实际风险低。
- `JobStore` 坏档有备份、无恢复 UI。下一次成功的 `recordFinish` 不会把备份合并回来。

## 本轮排除的误报

`onDestroy` 里 `nativeCancel()` 之后再 `clearPendingCancel()`，看起来像把刚发出的取消清掉。`clear_cancel_pending()` 在 `g_transcode_active` 时不动 pending，所以不会撤销正在进行的转码取消。

## 原生路径补核

下面几条是同一轮里按行号再对过的，不是第九轮清单的重复。

### R10-4 坏的中央目录后半段被当成成功（M）

`dngzip.cpp:387-392`：中央目录项签名不符时，如果已经收集到至少一帧 DNG，就 `break` 而不是失败。函数末尾 `:578` 仍 `return 0`。转码按截短后的帧表继续，用户看到的是成功。两个表都空才返回 -1。损坏发生在第一帧之前会失败，发生在中途会被静默丢掉后半段。

### R10-5 转码收尾的编码器 flush 双 EAGAIN 空转，且不再看取消（M）

`transcode.cpp:2932-2950`（视频）和 `:2978-2996`（音频 AAC）：`send_frame(nullptr)` 返回 `EAGAIN` 就去 `receive_packet`；receive 也是 `EAGAIN` 时内层 break，外层再送一次 NULL。没有次数上限，也不读 `g_cancel`。主循环里的取消点已经走完。硬错误会 `goto done`，不会把截断文件报成成功。触发条件同样是「两边同时 EAGAIN」。硬件编码器异步取缓冲时这比预览路径更值得先堵，因为取消按不下去。

### R10-6 DNG 预览失败会往后扫完整包（M）

`preview.cpp:179-180` 的注释是「顺序后退几帧」。循环是 `:208` `for (i = idx; i < frame_count(); ++i)`，方向是往后，而且没有帧数上限。目标帧坏掉时，调用线程上对后续每一帧做全尺寸 `read_frame` + LibRaw。预览在 `Dispatchers.IO` 上，不是主线程 ANR，但可以拖很久或把内存打满。探测路径另有最多 8 帧的限制，预览没有。

### R10-7 预览里被接住的 C++ 异常会漏关 fd（M，仅 OOM/异常）

契约是 native 拥有 fd，`preview.cpp:544-545` 在正常 `cleanup` 里 `close(fd)`。`mem_write` 的 `vector::insert` 或解码分配若抛出，`goto cleanup` 走不到，`native-lib.cpp:696-700` 把异常收成「返回 null」。Kotlin `MediaProbe.kt:76-78` 只在 Java 异常时 `adoptFd` 再关。这条路径 Java 侧看到的是正常 null，fd 留下。

`GetStringUTFChars` 失败的早退（`native-lib.cpp:684`）不是这条漏关：JNI 会带着 pending `OutOfMemoryError` 回到 Java，`MediaProbe` 的 catch 会关 fd。不要把这一行算成泄漏。

### R10-8 `nativeRun` 没有接住分配失败（M，表现是进程 abort）

`native-lib.cpp:614` 直接调 `run_transcode`，没有 try。首帧 `transcode.cpp:1727` 的 `read_frame` / `:1738` 的 `read_audio` 会按条目声明分配，单帧上限 512MB（`dngzip.cpp:91-94`）。`bad_alloc` 跳过 `:620` 的 `end_transcode()`，并穿过 JNI。Android 上未捕获的 C++ 异常通常直接 abort 进程，不是「下次调用永久 EBUSY」。EBUSY 只在进程没死、而 `g_transcode_active` 仍为真时才成立。worker 路径有 catch，这条主线程路径没有。`nativeInfo`（`:667`）同样没有 try。

### 低优先级补记

- `transcode.cpp:337-345`：`pwrite64` 返回 0 时不像 `pread`（`:326`）那样结束，`total` 不增加，写回调空转。普通文件很少返回 0，但是个不对称。
- `dngzip.cpp:107-108`：注释写「64KB + EOCD 22 字节」，常量是 `64 * 1024`（65536）。APPNOTE 最大注释 65535 时，EOCD 签名距文件尾 65557 字节，窗口短 21 字节；注释长于 65514 会报找不到 EOCD。`:285-290` 从窗口右端取第一个 `PK\x05\x06`，不核对 `comment_len`，注释里的伪签名会抢走真 EOCD。MotionCam 的包几乎碰不到。
- N-09：`app/src/main/cpp/CMakeLists.txt` 没有 `-Wall` / `-Wextra` / `-fvisibility=hidden` / `-fstack-protector` 这些字面量。工具链是否默认带 protector，这份 CMake 证明不了。不是功能缺陷。
- N-07 的注释也不一致：`preview.cpp` 按宽度缩，`HomeViewModel.kt:582` 附近仍把 `PREVIEW_MAX_W` 说成最大边。

## Kotlin 路径补核

### R10-9 销毁窗口把已成功的成品标成取消并删除（M）

`TranscodeService.kt:117-137`：`onDestroy` 在 `currentId` 还在时调用 `finalizeInterrupted`。`:426-428` 对非终态任务 `JobBus.finish(CANCELLED)`。`:445` 在 `finalRec.state != DONE` 时 `deleteOutputDocument`。

`runJob` 对 native 返回 0 只统计大小、不删文件；`JobBus.finish(DONE)` 在 `runQueue` 里更晚才发生。`runJob` 包在 `NonCancellable` 里，父 scope 已取消时块仍会跑完，然后向 `runQueue` 抛 `CancellationException`，`:173` 的 `finish(DONE)` 走不到。终态守卫也不会把 CANCELLED 改回 DONE。

于是：native 已成功返回、SAF 文件已写完、服务在 `finish(DONE)` 之前被销毁，历史记取消，成品被删。`statSize` 把这个窗口拉长。这和「用户点取消后保留成品」不是同一条：用户取消不走这里的删除，销毁路径会主动删。

同一次取消还会漏掉队尾：`:217` 的补录在 `runJob` 抛出取消之后执行不到。`finalizeInterrupted` 只处理当前这一条。尚未 `begin` 的 QUEUED 任务不进历史，入队时建的 0 字节文档也没人删。

### R10-10 `startForeground` 失败后队列仍然启动（M）

`promoteToForeground`（`:508-515`）捕获异常后只 `stopSelf()`。`onCreate`（`:89-99`）随后无条件 `serviceScope.launch { runQueue() }`。`stopSelf()` 不会阻止这次 `onCreate` 继续往下走。

失败后要么在没进前台时开始转码，要么马上走进 R10-9，把本批空文档删掉或留下取消记录。`enqueueStart` 本身没抛，所以没有 K-04 那条 ViewModel 回滚。用户看到的是点了开始就没了。

### R10-11 批量建档失败只回删前缀（M）

`OutputDir.kt:127` 对整批 `names.map { createInTree }`，前面失败不阻止后面继续创建。`HomeViewModel.kt:451-456` 遇到第一个 null 时 `deleteCreated(createdDocs)`，而 `createdDocs` 只含这个下标之前加进去的文档。失败项之后已经建成的 0 字节文档不删，名字也留在 `pendingNames`。注释要求调用方回删已建文档，回滚只覆盖了前缀。

### K-07 旧一批探测仍能盖掉新文件列表（M）

`HomeViewModel.kt:190-196`：`isCurrentProbe` 通过之后，`_state.update { files = picked }` 里不再读 generation。`update` 本身不挂起，但 `Dispatchers.IO` 是多线程的，取消要到下一个挂起点才生效。旧批次可以在新批次写入之后把文件列表盖回去，然后因 generation 不符退出。`finally` 不把列表改回来。界面停在过期文件上且 `probed=false`，开始按钮一直不可用。不会拿过期列表真正开转。

## 建议顺序

1. 完整重跑 `scripts/build-ffmpeg-android.sh`。重建后 `libavcodec.so` 里应出现 `FFmpeg version n6.1.6`，而不是现在的 `n6.1`；再用 `verify_pin` 把二进制和 NOTICE 里的 commit 对上。重新打包后确认 APK 含 `assets/licenses/`，并把 `MIT.txt` 的占位版权换成仓库根 `LICENSE` 的正文。签名与 minify 同属发布闸门，不是功能修复。
2. 给预览两处循环和 `transcode.cpp` 视频/音频 flush（R10-5）加连续双 EAGAIN 上限，并在 flush 里读 `g_cancel`。预览触发时返回空图；转码触发时按失败或取消收尾，不要空转。
3. `dngzip.cpp:387` 中途签名不符应失败，不要带着半截帧表 `return 0`（R10-4）。预览找替代帧加上限，不要从目标帧扫到包尾（R10-6）。
4. N-07/N-08：预览缩放改为限制长边；DNG 预览不要先解全幅再缩小。
5. 预览的 C++ 异常路径要 `close(fd)`（R10-7）。`nativeRun` 的分配失败应变成错误码并调用 `end_transcode()`，不要让异常穿过 JNI（R10-8）。
6. N-05：`pthread_create` 全失败应和 attr 失败一样 `return false`，不要 `m_no_workers = true`。
7. `onDestroy` 不要把「native 已返回 0、尚未 `finish(DONE)`」的任务标成取消再删文件（R10-9）。`startForeground` 失败后不要再 `launch { runQueue() }`（R10-10）。`outputDocsFor` 中途失败应停掉后续创建，或把失败下标之后的文档也放进回删列表（R10-11）。
