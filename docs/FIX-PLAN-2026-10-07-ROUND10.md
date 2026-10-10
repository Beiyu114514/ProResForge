# Round 10 分批修复方案

日期：2026-10-07。输入：`docs/REVIEW-REPORT-2026-10-07-ROUND10.md`。本文件只排批次和验收，不改源码。

第九轮点名的功能项大多数已经落地，这里不再打开。本方案只处理第十轮仍开放的项和新问题。

## 为什么不按报告第 1 条先重编 FFmpeg

报告把完整重跑 `scripts/build-ffmpeg-android.sh` 放在第一位，因为它是**发布阻塞**（S-1：二进制里的发行串是 `n6.1`，不是 pin 上的 `n6.1.6`）。它不是功能缺陷的修复顺序。

重编会换掉整套 prebuilt，一次失败或一次验证不过，会把后面的逻辑修复绑在同一次不可单独回滚的二进制更换上。功能批次在现有二进制上就能验证。S-1 / S-2 / B-09 单独作为最后一批发布闸门，不挡批次 1–7。

## 分批原则

1. 按用户能看见的损害排，不按文件类型排。会删掉已经转完的文件，优先于预览挂起，优先于很少走到的降级。
2. 一批只守一个不变量。同一文件上互相打架的改动合成一批；低优先级的顺手修改不塞进这一批。
3. 不放宽已经有的终态守卫。`JobBus.finish` 在记录已是终态时不再改写（`JobBus.kt:216`）。用户转码过程中点取消，不能再被一次 `rc == 0` 盖成完成。
4. 不在功能批次里重编 FFmpeg、打开 R8、或生成 keystore。

## 总览

| 批次 | 守什么 | 项 | 主要文件 | 依赖 |
|---|---|---|---|---|
| 1 | 成功成品不被销毁路径改成取消并删掉 | R10-9、R10-3、R10-10 | `TranscodeService.kt`，必要时 `JobBus.kt` | 无 |
| 2 | 建档失败不留后缀空文件；旧探测不盖新列表 | R10-11、K-07 | `OutputDir.kt`、`HomeViewModel.kt` | 无，可与 1 并行 |
| 3 | 转码收尾不会双 EAGAIN 空转，且仍能取消 | R10-5 | `transcode.cpp` 两段 flush | 无，可与 1、2 并行 |
| 4 | 坏掉的中央目录后半段不当成功 | R10-4 | `dngzip.cpp` | 无 |
| 5 | 预览循环会停，坏帧不会扫完整包 | R10-1、R10-2、R10-6，顺手 `av_frame_ref` | `preview.cpp` | 无 |
| 6 | 预览按长边缩小，DNG 预览不先解全幅 | N-07、N-08 | `preview.cpp`，`dng_decode_to_rgb48` 只加默认关闭的预览参数 | 批次 5 先落地 |
| 7 | 异常不穿 JNI、不漏 fd、线程全失败不降级 | R10-7、R10-8、N-05、N-06 | `preview.cpp`、`native-lib.cpp`、`dngframeq.h` | 批次 5、6 之后（都碰 `preview.cpp`） |
| 8 | 发布闸门，不是功能修复 | S-1、S-2、B-09 | 重编产物、`MIT.txt`、打包核对 | 不阻塞 1–7 |

低优先级见文末，不进上面任何一批。

---

## 批次 1 — 服务终态

### 不变量

native 已经返回 0 的任务，销毁路径不得把它写成 `CANCELLED`，也不得删输出。native 还在跑时被销毁，仍然取消并在写完之后删半成品。用户在转码过程中点的取消，仍然赢过之后到来的 `finish(DONE)`。

### 现在为什么会删成品

`runJob` 包在 `withContext(NonCancellable + Dispatchers.IO)` 里（`TranscodeService.kt:248-249`），所以 `serviceScope.cancel()` 拦不住它。它把 `JobOutcome` 交回 `runQueue` 之后，`JobBus.finish` 在 `:173`，已经离开 NonCancellable。

`onDestroy`（`:117-137`）在 `currentId` 还在时调用 `finalizeInterrupted`。`:426-428` 对非终态直接 `finish(CANCELLED)`。`:445` 在 `state != DONE` 时删文件。终态守卫让后面的 `finish(DONE)` 改不回来。

`run.done.countDown()` 在 `runJob` 的 `finally`（`:396`）。native 已经返回时，销毁侧一等闸门就打开，然后按刚才写上的 `CANCELLED` 把成品删掉。`statSize`（`:377-382`）把这个窗口拉长，但窗口的本质是「结果已经算出来，`finish` 还在可取消的 `runQueue` 里」。

同一段还会漏队尾。`runJob` 返回后若抛出取消，`:217` 的补录执行不到。`finalizeInterrupted` 只处理当前这一条。还没 `begin` 的 `QUEUED` 任务不进历史，入队时建的 0 字节文档也没人删。

R10-3 是这个窗口的另一侧，优先级低，但策略必须同一条：native 返回 0 之后、`finish(DONE)` 之前用户点取消，历史记取消，文件留下。把 `finish` 挪进 NonCancellable、且放在 `statSize` 之前，取消在 `statSize` 期间看到的已经是 `DONE`。`JobBus.cancel`（`:100` 附近）对非 `RUNNING` / `QUEUED` 直接返回，不会把完成改回取消。

### 做法

1. 在 `runJob` 的 NonCancellable 块里，native 返回并映射完状态之后、`statSize` 之前，就调用 `JobBus.finish`。
   - 此时若 JobBus 里已经是 `CANCELLED`（用户在 native 运行期间点的取消）：不要写 `DONE`。沿用取消，并删输出。这是旧守卫要保住的情况。
   - 若仍是 `RUNNING` 且 `rc == 0`：立刻 `finish(DONE)`。然后再 `statSize`。终态守卫会挡住第二次 `finish`，所以补文件大小要用一个只改 `outSize` 的更新，不能借 `finish` 再写一遍状态。
2. `ActiveRun` 增加在 `countDown()` 之前写好的 outcome。`finalizeInterrupted` **先等闸门，再读 outcome，最后才决定状态和删不删**。现在的顺序是先 `finish(CANCELLED)` 再等（`:426-444`），写错之后改不回来。
   - outcome 已是 `DONE`：按 `DONE` 落库，不删。
   - outcome 是 `FAILED` 或 `CANCELLED`：按该状态落库，非 `DONE` 才删。
   - outcome 仍空（native 没返回）：维持 `CANCELLED`，等写盘停止后再删。
3. 历史改到闸门之后再写。晚写换来的是「不把成品标错并删掉」。进程若在等待期间被杀，这一条历史可能缺，文件还在。这比文件被删、历史写成取消好。不要先写一条临时的 `CANCELLED` 当保险。
4. 销毁时不要再依赖 `runQueue` 能走到 `:217`。在 `finalizeScope` 里扫一遍 JobBus：已经是终态但没落库的补 `recordFinish`；仍是 `QUEUED` 的，删输出文档并 `removeQueued`（与 K-04 相同：没开始过，不留 0 字节文件，也不编造成完成）。
5. R10-10：`promoteToForeground`（`:500-516`）改成返回是否成功。失败时已经 `stopSelf()`，`onCreate`（`:89-99`）不得再拿 wakelock，也不得 `launch { runQueue() }`。`enqueueStart` 没抛，ViewModel 不会走 K-04 回滚，所以服务自己把当时仍是 `QUEUED` 的任务做 `removeQueued` + 删文档。回滚放在 `onCreate` 的失败分支里做完，不要指望随后的 `onDestroy`。

### 不要做

- 不要让 `finish(DONE)` 覆盖已经写成的 `CANCELLED`。那会把「运行中点取消，native 仍返回 0」又变回完成。
- 不要为了测 Service 去加 Robolectric。把「已知 outcome 时提交什么状态、删不删」抽成纯函数，用现有 JVM 测试覆盖：`DONE` 不删，`FAILED` / `CANCELLED` / outcome 空都删，已是 `DONE` 的记录销毁路径不改状态。
- 不要改 `clearPendingCancel` 的语义。`g_transcode_active` 时不清 pending，这是第十轮已经排除的误报。

### 验收

- native 已返回 0、`finish(DONE)` 尚未被 `runQueue` 执行时销毁：历史是完成，输出还在。
- native 仍在跑时销毁：取消，半成品在 native 停止写之后被删。
- 运行中点取消且 native 返回取消码：仍然是取消，输出被删，不会被完成覆盖。
- `startForeground` 抛异常：队列不启动，已入队的 0 字节文档被删，不留下取消记录冒充跑过。

---

## 批次 2 — 建档回滚与探测代际

与批次 1 无文件重叠，可以并行。不要和批次 1 混成一个提交：一个是终态状态机，一个是 SAF / 列表。

### R10-11

`OutputDir.outputDocsFor`（`OutputDir.kt:124-128`）对整批 `names.map { createInTree }`，前面失败不阻止后面创建。`HomeViewModel.startTranscode`（`:451-456`）遇到第一个 null 就 `deleteCreated(createdDocs)`，而 `createdDocs` 只含这个下标之前的文档。失败项之后已经建成的 0 字节文档留在 SAF 里，名字也留在 `pendingNames`。

两处一起改，缺一不可：

- `outputDocsFor` 在第一次创建失败后停止，后面的名字不再 `createDocument`。返回列表仍与 `names` 等长，未创建的位置是 null。
- 调用方回删**返回列表里每一个非 null**，不只回删前缀累加器。这样以后若有人又改成「失败继续创建」，回滚仍然盖得住。
- 改 `OutputDir.kt:121` 的注释。现在写的是「调用方回删前面已建成功的」，与真实义务不符。

### K-07

`HomeViewModel.onFilesPicked`（`:190-196`）在 `isCurrentProbe` 通过之后才 `_state.update { files = picked }`，lambda 里不再读 generation。`Dispatchers.IO` 是多线程的，取消要到下一个挂起点才生效。旧批次可以在新批次写入之后把列表盖回去，然后因 generation 不符退出。`finally` 不把列表改回来。界面停在过期文件上且 `probed = false`，开始按钮一直不可用。不会拿过期列表真正开转。

做法：每一处写 `files` 或 `busy` 的 `update` lambda 内部再读 generation，过期则原样返回旧状态。`finally` 只清自己这一代的 busy，不清、也不恢复文件列表。检查放在 `update` 外面不够。

### 验收

- 批量建档第 2 个失败：第 3 个及之后的文档不被创建；已创建的被删；`pendingNames` 里没有它们。
- 连续选两批：慢的那批在快的那批写入之后返回时，列表仍是新的一批，且新的一批能探测完，开始按钮可用。

---

## 批次 3 — 转码 flush 不得空转

只改 `transcode.cpp` 视频 flush（`:2932-2950`）和音频 AAC flush（`:2978-2996`）。两段是同一形状：`send_frame(nullptr)` 返回 `EAGAIN` 就去 `receive_packet`；receive 也是 `EAGAIN` 时内层 break，外层再送一次 NULL。没有次数上限，也不读 `g_cancel`。主循环的取消点已经走完。硬错误会 `goto done`，不会把截断文件报成成功。触发条件是两边同时 `EAGAIN`。硬件编码器异步取缓冲时，这条比预览更优先，因为取消按不下去。

做法，两段相同：

- 连续「send NULL 得 `EAGAIN` 且紧接着的 receive 也是 `EAGAIN`、没有写出任何包」计一次。上限 **8**。符合约定的编码器不应停在这种零进展状态；8 次够吸收抖动，又不会把取消拖成长时间空转。
- 触顶：`result` 设为真实负错误，走现有 `goto done`。不得保持 `result == 0`。
- 每一轮读 `g_cancel`。置位则走主循环已经在用的取消返回码，不要新造一个码，也不要当成功。

不要在这一批改 `pwrite64` 返回 0 的空转（`:337-345`）。那是另一条路径，混进来无法从 flush 的验收里分开。

验收：构造或在注释里固定「双 EAGAIN 连续 8 次必须离开循环且 `result != 0`」；取消标志在 flush 中置位时，返回码与主循环取消一致。不要求为此新造 FFmpeg 测试框架。

---

## 批次 4 — 坏中央目录必须失败

`dngzip.cpp:387-392`：中央目录项签名不符时，若已经收集到至少一帧就 `break`，函数末尾仍 `return 0`。转码按截短后的帧表继续，用户看到成功。两个表都空才返回 -1。损坏在第一帧之前会失败，发生在中途会被静默丢掉后半段。

做法：签名不符一律 `return -1`，错误字符串与现在两表都空时相同（`zip: 中央目录项签名不符(不是 zip 或被破坏)`）。不要 `break` 后落到成功返回。已经收进表里的帧不作为成功的理由。

不要在这一批改 EOCD 窗口（`:107-108` 的 64KiB 窗口比 APPNOTE 最大注释短 21 字节，`:285-290` 不核对 `comment_len`）。MotionCam 的包几乎碰不到，而且扫尾窗口改错会把合法 zip 拒掉。与「目录中途坏掉必须失败」不是同一个不变量。

验收：中央目录前若干项合法、之后签名被改坏的 zip，`open` 返回 -1，帧表不得被当成完整序列拿去转码。

---

## 批次 5 — 预览循环会停

只处理控制流。缩放和全幅解码留在批次 6。都在 `preview.cpp`。

### R10-1

`:296-338`。两处都没有计数：

1. `have_packet` 为真，`avcodec_send_packet` 返回 `EAGAIN`，紧接着 `avcodec_receive_frame` 也返回 `EAGAIN`。包不丢、`flushed` 仍为 false，`:338` 不退出。
2. `av_read_frame` 已 EOF，flush（`send_packet(nullptr)`）返回 `EAGAIN`，receive 也是 `EAGAIN`。`:303-305` 只有非 EAGAIN 才 `break`，下一轮再次读到 EOF。

连续零进展上限同样是 **8**。触顶则退出循环：已有帧就用已有帧，没有则走现有的失败（空预览）。不要无限循环。预览在 `Dispatchers.IO` 上，协程取消中断不了已经进入的 JNI，所以上限必须在 native 里。

### R10-2

`:499-512`。`flushed` 只在 `send_frame(nullptr)` 返回 `>= 0` 或 `EOF` 时置真。flush 自己返回 `EAGAIN` 时 `flushed` 仍为 false，函数 `continue`。下一轮若 receive 仍是 `EAGAIN`，又走「尚未 flush」分支，`:511` 的 `enc_ok = false` 到不了。注释写的「flush 后再 EAGAIN 就停」与代码不一致。

连续 flush `EAGAIN` 且没有收到包，计进同一个上限。触顶：`enc_ok = false`，`:516` 已有的 `result.clear()` 会把截断 JPEG 丢掉。不要把一次 `EAGAIN` 就当成失败。

`:482` 把第一次 `avcodec_send_frame` 的任何 `< 0`（含 `EAGAIN`）直接 `goto cleanup`。空的 MJPEG 编码器首帧通常不会 `EAGAIN`。这一批可以一并改成「`EAGAIN` 则进入上面的收包循环」，但不要为此改质量或像素格式。

### R10-6

`:208` 的循环是 `for (i = idx; i < frame_count(); ++i)`，从目标帧一直扫到包尾。注释（`:179-180`）写「顺序后退几帧」，方向和上限都不符。探测路径 `native-lib.cpp:320` 最多 8 帧，预览没有上限。

保持向后找（不要改成向前，那是另一项行为变更）。尝试次数上限 **8**，与探测一致。注释改成「从目标帧向后最多 8 帧」。批次 6 之前每一帧仍可能是全幅解码，所以上限必须先于半尺寸落地。

### `av_frame_ref`

`:331` 不看返回值。失败后 `cand` 已被 `unref`，该帧被跳过，可能落到更晚的帧或最终无预览。同一循环里补上：返回值 `< 0` 则跳过这一帧，不用已经 unref 的 `cand`。

### 验收

- 解码环连续 8 次双 `EAGAIN` 后函数返回，不挂在 JNI 里。
- 编码 flush 连续 8 次 `EAGAIN` 后预览结果为空，而不是半截 JPEG。
- DNG 目标帧坏掉时，最多再试 7 帧，不会把 `frame_count()` 跑完。

---

## 批次 6 — 预览尺寸

依赖批次 5。否则「向后扫」仍可能把全幅解码乘上很多帧。

### N-07 按长边缩小

`preview.cpp:351-360` 只在旋转后的宽 `dw > max_w` 时缩小，高不参与。`HomeViewModel` 的 `PREVIEW_MAX_W = 640`（`:583`），注释已经写的是「预览图最大边」。宽 ≤ 640、高很大的素材不会缩小；JPEG 顶到 64MB 后被判失败，用户看到的是没有预览。

旋转之后用 `max(dw, dh)` 和 `max_w` 比。长边超过 640 才等比缩小，宽高仍取偶数（mjpeg 4:2:0）。数值 640 不改。JNI 参数名这一批不改，避免签名抖动；把 `preview.cpp:351` 附近「按宽度」的注释改成「按长边」，与 Kotlin 注释一致。

### N-08 预览不要先解全幅

`preview.cpp:208-218` 对候选帧 `read_frame` + `dng_decode_to_rgb48` 出全幅 RGB48，之后才缩放。4096×3072 一帧 RGB48 约 72MB（`4096 * 3072 * 3 * 2`）。

`apply_libraw_params`（`dngzip.cpp:862-872`）只在 `DngQuality::FAST` 置 `half_size = 1`。BALANCED / FINE 不置。预览档位已经跟转码 spec 对齐，不能为了预览把用户选的档位改成 FAST，也不能在共享的 `apply_libraw_params` 里让 BALANCED / FINE 默认半尺寸 —— 转码会跟着变。

还有复用陷阱：`dngzip.cpp:935-938` 写明 `half_size` 只在 FAST 置 1，BALANCED / FINE 不显式置 0；线程局部 LibRaw 若不先恢复构造默认 params，FAST 之后的帧会残留半尺寸。所以预览降采样必须是**调用方显式传入、默认关闭**的参数，并且仍然走每帧先恢复 `tlr_params0` 再 `apply` 的路径。转码的三处调用（`transcode.cpp:1728`、`:2579`，以及探测 `native-lib.cpp:328`）保持默认，不传这个开关。

预览侧：在现有质量 / 色彩 / 映射之外要求半尺寸解算。2048×1536 的 RGB48 约 18MB，然后再按批次 6 的长边缩到 640。半尺寸改变的是预览分辨率，不是档位枚举；预览和成片的像素不会逐点相同，这是这一批接受的代价。转码成片分辨率不变。

不要用「预览失败就静默改成 FAST」这种旁路。档位参数仍然照传。

### 验收

- 宽 640、高远大于 640 的帧，输出 JPEG 的长边 ≤ 640，而不是直接失败。
- 预览 BALANCED / FINE 的 DNG 时，解码输出宽高约为全幅的一半，而不是 4096×3072。
- 同一进程里先预览再转码一帧 BALANCED：转码输出尺寸仍是全幅。这是为了守住 `:935-938` 的复用约定。

---

## 批次 7 — 异常、fd、线程失败

放在批次 5、6 之后，因为 R10-7 也要动 `preview.cpp` 的出口。

### R10-7 fd

契约是 native 拥有 fd，正常路径在 `preview.cpp:544-545` `close(fd)`。`mem_write` 的分配或解码若抛出，`goto cleanup` 走不到，`native-lib.cpp:696-700` 把异常收成返回 null。Kotlin `MediaProbe` 只在 Java 异常时关 fd。这条路径 Java 侧看到的是正常 null，fd 留下。

在 `render_preview_jpeg` 里用会在栈展开时 `close` 的守卫，覆盖异常出口。JNI 的 catch 仍然只返回 null，**不要在 catch 里再 close 一次**（正常路径已经关过）。

`GetStringUTFChars` 失败的早退（`native-lib.cpp:684`）不是这条漏关：JNI 会带着 pending `OutOfMemoryError` 回到 Java，`MediaProbe` 的 catch 会关 fd。不要改这一行的 fd 所有权。

### R10-8 分配失败不得穿出 JNI

`native-lib.cpp:614` 直接调 `run_transcode`，没有 try。`bad_alloc` 会跳过 `:620` 的 `end_transcode()` 并穿过 JNI。Android 上未捕获的 C++ 异常通常直接 abort 进程。worker 路径已有 catch，这条主线程路径没有。`nativeInfo`（`:667`）同样没有 try。

`run_transcode` 外包 try。捕获后：若已经进入 lifecycle（`owns_lifecycle`），调用 `end_transcode()`，再返回一个 **非 0** 的负码，让 Kotlin 现有的 `else` 分支把它当成失败（`err_ffmpeg`）。不得返回 0。`nativeInfo` 捕获后返回空串（现有空结果已经是失败），不要新开一条 JNI 错误通道。

### N-05 零线程不得降级

`dngframeq.h:275` 在 `pthread_create` 失败时 `break`。`:292-300` 在线程数为 0 时置 `m_no_workers = true` 并 `return true`。`next()` 随后在消费线程上解码。这和 `:260-261`「不要降级到不受控栈」相反。LibRaw 已经在堆上，不能再用「栈上 767KB 必 SIGSEGV」当作这条路径已经证实的崩溃；调用链栈深仍然没有 8MB 的保证。

只改「一个线程都没有」：`thread_count() == 0` 时 `return false` 并写上错误，**不要**置 `m_no_workers`。至少创建起 1 条时仍 `return true`（`:250` 的注释：有一条就能跑）。`#else` 的 `std::thread` 分支同样处理。不要 `pthread_self()` 再自 join。

### N-06 `m_started`

`:249` 在 attr 失败（`:267`）和 init 失败（`:281`）的 `return false` 之前就把 `m_started` 置真。当前调用方看到 false 会走开，不会再 `next()`。`start()` 开头会 `stop()`，同一对象重试能恢复。若失败后误调 `next()`，未启动防护失效。

把 `m_started = true` 挪到线程创建成功之后；每条 `return false` 之前保持 false。和 N-05 是同一个函数，一起改。

### 验收

- 预览路径抛 `std::bad_alloc` 时，fd 被关，JNI 返回 null，且不会 double-close。
- `run_transcode` 抛异常时进程不 abort，`g_transcode_active` 被清掉，Kotlin 得到失败而不是成功。
- `pthread_create` 全部失败时 `start()` 返回 false，消费线程不会自己跑 `produce_one()`。

---

## 批次 8 — 发布闸门

不阻塞 1–7，也不要和它们放在同一个提交里。功能修复不依赖新的 prebuilt。

### S-2 可以先做的文本

`app/src/main/assets/licenses/MIT.txt:3` 仍是 `Copyright (c) <year> <copyright holders>`。仓库根 `LICENSE:3` 是 `Copyright (c) 2025 ProResForge contributors`。把 assets 里这一行换成仓库根的版权行，正文许可条款保持 MIT，不要把 GPL 说明写进这个文件（GPL 边界已经在 `LICENSE` 末尾和 `NOTICE.md`）。

现有 `dist/ProResForge-1.0.0-debug.apk`（2026-10-06，报告核对过没有 `licenses` / `NOTICE` / `LICENSE` 条目）是许可文件进树之前打的。源码树已经有这些文件。下一次 debug 打包后核对 APK 内有 `assets/licenses/`。不要把「这份旧 APK 里没有许可证」当成还要改打包脚本；`app/build.gradle.kts` 排除的是 jar 的 `META-INF/{AL2.0,LGPL2.1}`，不删 assets。

### S-1 完整重编，单独做

`scripts/build-ffmpeg-android.sh` pin 的是 `n6.1.6`，但 `prebuilt/arm64-v8a/include/libavutil/ffversion.h` 是 `FFMPEG_VERSION "n6.1"`，`libavcodec.so` 里有 `FFmpeg version n6.1` 和 `Lavc60.31.102`，没有 `n6.1.6`。版本头数字（60.31.102、x264 165、x265 216、libvpx 1.17.0）和脚本 SHA-256 `683d34540d3847231c73547fde51bf8be36e112379c00c648b17abdc02e2bb4b` 只能证明清单抄了当前脚本，不能证明这份脚本产出了这些二进制。`libavcodec.so` 时间戳是 1981-01-01，同目录其他库是 2026-10-04。

验收：

- 重跑脚本，不手改 `ffversion.h`，不在二进制里搜替换字符串。
- `libavcodec.so` 中出现 `FFmpeg version n6.1.6`。
- `verify_pin` 把二进制和 NOTICE 里的 commit 对上之后，才允许 NOTICE 把该 commit 写成随包源码。对不上之前，不要加强 GPL offer 的措辞。
- offer 仍然缺具体 URL、要约人和至少三年的期限。那是发布材料，不是这一批代码能编出来的。重编完成前不要假装 offer 已经成立。

工作目录是 UNC。`gradlew` 若要在本机跑，需要映射盘符后在盘符根目录执行，不要在 `\\192.168.100.232\win\ProResForge` 上直接开 `gradlew.bat`。重编脚本本身走它现有的构建路径，不要为了这一批改 pin。

### B-09 本方案不实施

报告所记：`isMinifyEnabled = false`，仓库没有 `signingConfig`，也没有 keystore。这是发布决策。打开 R8 可能碰 JNI，没有单独的混淆冒烟之前不打开。keystore 不进仓库。没有签名配置之前不产出可分发的 release 包。

---

## 明确不进上述批次

| 项 | 原因 |
|---|---|
| `transcode.cpp:337-345` `pwrite64` 返回 0 时空转 | 普通文件很少返回 0。和批次 3 的 flush 不是一条路径，单独的小补丁。 |
| `dngzip.cpp:107-108`、`:285-290` EOCD 窗口短 21 字节、不核对 `comment_len` | MotionCam 几乎碰不到。和批次 4 的「中途坏签名」分开，避免合法 zip 被误拒。 |
| N-09 CMake 没有 `-Wall` / `-Wextra` / `-fvisibility=hidden` / `-fstack-protector` 字面量 | 不是功能缺陷。工具链默认值这份 CMake 证明不了。 |
| `native-lib.cpp:655` `initNativeLogging` 在 `GetStringUTFChars` 失败时没有显式失败路径 | 失败时 JNI 已挂起异常且后面没有 JNI 调用。预览路径已经修过。 |
| `JobStore` 坏档有备份、无恢复 UI | 不再是静默清空。恢复入口是产品行为，不是这一轮的正确性修复。下一次成功的 `recordFinish` 不会把备份合并回来，这点保留在 backlog。 |
| 已关闭项（K-01 至 K-06、K-08、K-11、N-01 至 N-04、N-10、L-17、32 位 `off_t`、LibRaw 堆分配） | 第十轮已对源码复核。不重做。 |

## 建议落地顺序

单人：1 → 2 → 3 → 4 → 5 → 6 → 7，批次 8 在要打开发行包时再做。

两人：1 与 3 可以同时做（Kotlin 服务 / `transcode.cpp`）。2 与 4 也可以同时做。5、6、7 按顺序，因为都碰到 `preview.cpp`。不要把批次 6 的半尺寸参数和批次 7 的 fd 守卫放进同一次未完成的 5。
