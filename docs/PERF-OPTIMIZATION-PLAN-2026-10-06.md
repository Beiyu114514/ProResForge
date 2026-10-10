# ProResForge 性能优化方案

- 方案类型：只读审查方案
- 适用范围：DNG ZIP → LibRaw → filtergraph → ProRes/MOV 主链
- 方案日期：2026-10-06
- 当前状态：**仅新增本方案文档；未修改源码、构建配置或既有文档，未执行构建和测试**
- 主要依据：[PERF-RAW-TO-PRORES-VERIFIED-2026-10-06.md](PERF-RAW-TO-PRORES-VERIFIED-2026-10-06.md)

## 1. 执行摘要

当前最值得优先处理的不是 ZIP inflate，也不是强制给 filtergraph 设置线程数，而是以下两项：

1. **为 `prores_ks` 设置有限的帧级编码线程**：当前应用只设置 ProRes profile，没有设置 `thread_count`；FFmpeg 编码器默认 `thread_count=1`。同源实验显示，编码器是 raw→ProRes 端到端耗时的约 94–95%，编码器从 1 线程改为 2 线程已有约 1.77×的隔离加速，4 线程约 2.97×。
2. **所有性能基线改用 Release native 构建**：当前 app 自有 native 产物存在 Debug/`-O0` 证据；同源 4→3 通道微核从 `36.1 ms` 降至 `5.6 ms`。在 Release 基线建立之前，不应继续使用历史 Debug 包的绝对耗时判断优化收益。

编码器优化完成后，瓶颈预计转移到 DNG 流水线和内存交接：

- `DngFrameQueue` 的 `depth=2` 限制了在飞解码帧数；
- 每帧存在约 72 MB AVFrame 分配/清零及约 72 MB RGB memcpy；
- 每个 DNG 帧都会重新创建 LibRaw 实例和大型内部缓冲；
- faststart 可能在长 4K 输出结束时产生一次近全文件读写。

## 2. 约束与不应重复处理的事项

### 2.1 本方案的工作边界

本轮只交付方案，不执行以下动作：

- 不修改 `app/src/main/cpp/`、Kotlin、Gradle、CMake 或脚本；
- 不运行 Gradle、CMake、native 测试、instrumentation 或真机测试；
- 不重建 FFmpeg/LibRaw 预编译产物；
- 不改写既有性能报告。

### 2.2 已落地、不是本方案待办项的内容

以下事项已有实现或修复，不应在优化计划中重复当作新优化：

- DNG ZIP 帧级并行队列；
- POSIX worker 的显式 8 MB 栈；
- `DngFrameQueue::thread_count()` 同时统计 `m_workers` 和 `m_threads`；
- 队列 15 秒停滞诊断；
- 帧序、取消、失败传播和槽位 buffer 复用的既有语义；
- 普通任务队列串行执行。

这些机制只能在性能改动后做回归验证，不能为了追求吞吐而删除或弱化。

## 3. 已验证性能基线

### 3.1 证据等级

主要数据来自 x86_64/WSL 的同源实验，不是真机：

- 12 核、约 23 GB 内存；
- 应用同源 C++、FFmpeg 6.1、LibRaw 0.21.4；
- 真实 4096×3072 DNG ZIP 子集和独立微核；
- 主机的绝对毫秒数不能直接外推到 ARM 手机，但瓶颈排序和相对趋势可用于确定下一轮实验。

真机决策必须重新测量，尤其是线程数、温控和内存峰值。

### 3.2 关键结果

| 项目 | 已验证结果 | 判断 |
|---|---:|---|
| `prores_ks` 端到端耗时占比 | 约 94–95% | 当前头号瓶颈 |
| `prores_ks` 编码器 1→2 线程 | 约 1.77× | 高优先级 |
| `prores_ks` 编码器 1→4 线程 | 约 2.97×（编码器隔离测试） | 首轮真机候选 |
| 1→12 线程 | 约 4.38×（主机上限参考） | 不应直接用于生产默认 |
| ProRes 切片线程 | 几乎无收益 | 优先帧级线程 |
| swscale 单线程→自动线程 | 108.0→23.6 ms/帧 | filtergraph 当前已具备自动并行 |
| `ushort[4]→ushort[3]`：`-O0`→`-O2/-O3` | 36.1→5.6 ms | Release 必须先建立 |
| inflate | 约 3–16 ms/帧 | 当前不是主要瓶颈 |
| setup 首帧重复解码 | 约 0.4 s/任务 | 长任务收益小、短任务可优化 |

### 3.3 对旧结论的修正

旧报告曾把 `graph->nb_threads=0` 解释为单线程；已验证报告确认 FFmpeg 中 `0` 表示自动线程数，当前 filtergraph/sws 已经自动并行。因此：

- 不把“给 filtergraph 设置线程”列为确定的 P0；
- 不在编码器线程优化之前强制限制或扩大 filtergraph 线程；
- 只有出现过度抢核时，才做 `auto / 2 / 4` 的上限 A/B。

## 4. 优先级总表

| 优先级 | 优化项 | 预期收益 | 风险 | 决策 |
|---|---|---|---|---|
| P0 | Release native 基线 | DNG 自有代码显著降耗，避免错误基线 | 低 | 立即作为所有测试前提 |
| P0 | `prores_ks` 帧级线程 2–4 | 端到端最大确定性收益 | 编码缓冲、延迟、码流一致性需验证 | 第一实际改动 |
| P1 | `dngthreads`/`dngdepth` 联动 | 编码器提速后释放 DNG 并行潜力 | 4K 内存线性增加 | 先测后调 |
| P1 | worker-local LibRaw 复用 | 减少大型分配/释放和 allocator 抖动 | LibRaw 状态复用语义 | 单独 A/B |
| P1/P2 | AVFrame/AVBuffer 池化、零拷贝 | 减少 72 MB 分配、清零和 memcpy | 引用生命周期复杂 | 在 depth 调参后做 |
| P2 | faststart 策略 | 长文件可能省一次近全文件读写 | 影响播放/上传兼容体验 | 产品策略 + 实测 |
| P2 | 首帧结果复用 | 每任务约省 0.4 s | 缓存失效与生命周期 | 低成本优化 |
| P3 | filtergraph 线程上限 | 仅在过度抢核时有收益 | 可能反而降低吞吐 | 条件触发 |
| P3 | 更换 inflate / 重编 FFmpeg | 当前证据不足 | 构建、ABI、体积风险 | 暂不投入 |

## 5. P0 阶段：建立可信基线并优化 ProRes 编码

### 5.1 P0-1：固定 Release native 性能基线

当前 [app/src/main/cpp/CMakeLists.txt](../app/src/main/cpp/CMakeLists.txt) 没有显式的 `CMAKE_BUILD_TYPE` 或 `-O` 设置；已验证实验在 app 自有代码中观察到 Debug/`-O0`。

建议：

1. 使用 Release variant 构建性能包；
2. 检查 `compile_commands.json`，确认 `transcode.cpp`、`dngzip.cpp`、`dngframeq.h` 使用 `-O2` 或 `-O3`；
3. 后续所有性能数字均标注 ABI、BuildType、CPU、温度和素材；
4. 分别比较 `-O2` 与 `-O3`，但不要在首轮加入 `-ffast-math`；
5. 保留现有显式 `pread64/pwrite64/lseek64` 大文件 I/O 方案，不用全局 `_FILE_OFFSET_BITS=64` 替代它，以免破坏 32 位 ABI 约束。

重点影响位置：

- [app/src/main/cpp/dngzip.cpp:830-902](../app/src/main/cpp/dngzip.cpp:830-902)：LibRaw 输出和 4→3 通道转换；
- [app/src/main/cpp/transcode.cpp:1865-1882](../app/src/main/cpp/transcode.cpp:1865-1882)：AVFrame 交接和逐行复制；
- [app/src/main/cpp/dngframeq.h:186-309](../app/src/main/cpp/dngframeq.h:186-309)：队列控制和 worker 胶水代码。

**验收条件：** Release 包和 Debug 包分别记录，不能把两者混成一条基线；输出内容、帧数和错误状态不变。

### 5.2 P0-2：为 `prores_ks` 设置有限帧级线程

当前 ProRes 分支只设置 profile，位置在 [app/src/main/cpp/transcode.cpp:1417-1450](../app/src/main/cpp/transcode.cpp:1417-1450)。FFmpeg 的 `AVCodecContext.thread_count` 默认值为 1，因此不设置就等于单线程；`0` 才表示自动，不建议直接把自动值作为生产默认。

建议的第一轮矩阵：

```text
thread_count = 1 / 2 / 3 / 4
thread_type  = FF_THREAD_FRAME
```

建议策略：

- 真机优先验证 2、3、4 线程；
- 初始生产候选从 3 或 4 中选择；
- 不使用 `thread_count=0` 作为第一版默认值，避免自动扩展到过多线程；
- 不优先使用切片线程，已有实验显示切片模式几乎没有收益；
- 只对软件 `prores_ks` 路径使用该策略，不套用 MediaCodec 路径；
- 保留现有任务串行调度，不并发启动多个 4K 转码。

**收益假设：** 编码器隔离测试 1→2 线程约 1.77×，1→4 线程约 2.97×；端到端实际收益会受 DNG、滤镜、I/O 和温控限制，不应直接相乘外推。

**主要风险：**

- 帧级编码产生编码延迟，需确认 `send_frame`/`receive_packet` 以及收尾逻辑正确；
- 编码器可能持有额外帧缓冲；
- 主机实验曾观察到输出逐字节相同，但不能把它当作所有 ARM 环境的无条件保证；
- 报告内存在不同实验口径的内存增量观察，必须以真机 `VmHWM` 为准。

## 6. P1 阶段：让 DNG 解码和主链重新平衡

### 6.1 P1-1：联动 worker 数与 `depth`

当前 [app/src/main/cpp/dngframeq.h:275-293](../app/src/main/cpp/dngframeq.h:275-293) 使用 `assigned-consumed < depth` 限制在飞帧。`depth=2` 时，在飞解码帧最多为 2，即使 worker 数为 4，也无法让 4 个 worker 长时间同时工作。

建议矩阵：

```text
worker = 2 / 3 / 4
depth  = 2 / 3 / 4
encoder = 2 / 3 / 4
```

建议原则：

- `depth` 至少从“不小于有效 worker 数”开始测试；
- 先让编码器提速，再调 depth；否则 DNG 解码加速可能被主线程编码完全掩盖；
- 不直接使用 `depth=2×worker` 或 `thread_count=auto`；
- 每档同时记录 `VmHWM`、队列等待比例和总吞吐；
- 4K 设备以现有约 500 MB 内存目标为硬约束。

历史数据表明纯 LibRaw 解码 2T→4T 有明显加速，但历史真机整帧 2T 与 4T 接近，符合 depth=2 的限制。该结论需要在 Release arm64 真机上重新确认。

### 6.2 P1-2：每个 worker 复用一个 LibRaw 实例

当前 [app/src/main/cpp/dngzip.cpp:830-902](../app/src/main/cpp/dngzip.cpp:830-902) 每帧创建 LibRaw、解码、`recycle()` 后销毁；这会重复触发大型内部缓冲的申请与释放。

候选设计：

- worker 线程拥有自己的 LibRaw 实例；
- 每帧使用明确的 reset/recycle 流程；
- 不跨 worker 共享实例；
- 失败、取消、尺寸异常后丢弃并重建实例，而不是复用不确定状态。

验证重点：

- WB、曝光、输出色彩、尺寸和 metadata 不跨帧污染；
- 连续 60 帧和失败后下一帧均正确；
- 内存不会随帧数增长；
- 复用的收益来自总 wall time 和 allocator 抖动，而不仅是单帧局部计时。

**不建议：** 为 LibRaw 再打开 OpenMP。当前已有帧级并行，叠加帧内并行可能过度订阅 CPU，破坏线程预算和温控。

### 6.3 P1-3：消除每帧 72 MB AVFrame 分配和复制

当前 [app/src/main/cpp/transcode.cpp:1865-1882](../app/src/main/cpp/transcode.cpp:1865-1882) 每帧执行：

1. `av_frame_unref()`；
2. `av_frame_get_buffer()`，分配/清零约 72 MB；
3. 把 worker RGB 数据逐行 memcpy 约 72 MB；
4. 送入 filtergraph。

建议分两步实施：

#### 第一步：池化和引用计数

- 先让 AVFrame/AVBuffer 跨帧复用；
- 明确 `av_buffersrc_add_frame_flags()` 的所有权；
- 仅在 graph 不再持有输入后回收槽位；
- 用 `AVBufferRef` 管理生命周期，不用裸指针约定。

#### 第二步：worker 直接写目标 buffer

- 让 worker 按目标 frame 的 `linesize` 写入；
- 消除中间 RGB memcpy；
- 只有在目标 buffer 已确定不会被 graph 继续读取时才归还槽位。

**风险：** 这是本方案中生命周期风险最高的优化之一。必须保持严格帧序、取消、失败传播和异常清理；不能为了零拷贝破坏 `DngFrameQueue` 的槽位复用语义。

**收益假设：** 主机 memcpy 约 4–6.6 ms/帧，另有分配、清零和 allocator 抖动；编码器提速后其占比会明显上升。

## 7. P2 阶段：I/O 与初始化优化

### 7.1 faststart 默认策略

当前默认开启 faststart：

- UI 默认值见 [app/src/main/java/com/proresforge/app/ui/HomeViewModel.kt:54-68](../app/src/main/java/com/proresforge/app/ui/HomeViewModel.kt:54-68)；
- 文件搬移逻辑见 [app/src/main/cpp/transcode.cpp:487-564](../app/src/main/cpp/transcode.cpp:487-564)。

长 4K ProRes 文件可能达到数 GB，faststart 会把 `moov` 前区域整体搬移，相当于一次额外的近全文件读写。

建议：

- 保留 UI 开关；
- 对本地批量交付场景评估默认关闭；
- 对需要在线播放/上传后立即播放的场景保留开启；
- 用相同成品分别测 `faststart=0/1` 的 finalize 时间、I/O 字节数和失败率；
- 不把 faststart 计入每帧编码吞吐，应单独报告收尾耗时。

### 7.2 复用首帧结果

DNG 任务启动时可能有三次首帧工作：

1. UI `nativeInfo` 探测；
2. 转码初始化确定尺寸；
3. worker `seq=0` 再次解码。

相关流程见 [MediaProbe.kt:24-49](../app/src/main/java/com/proresforge/app/data/MediaProbe.kt:24-49) 和 [app/src/main/cpp/transcode.cpp:1147-1235](../app/src/main/cpp/transcode.cpp:1147-1235)。

预期收益约为每任务 0.4 秒，长任务占比小，但短片或批量任务可受益。

建议优先级：

- 先复用已确认的尺寸和像素格式；
- 再评估是否安全地把已解出的首帧 buffer 交给队列；
- 明确定义 URI、文件长度和内容变化时的缓存失效条件；
- 不以 URI 永久缓存媒体内容。

## 8. P3 阶段：条件性优化与暂不建议项

### 8.1 filtergraph 线程上限只做条件 A/B

当前 filtergraph 的 `nb_threads=0` 已验证为自动并行。只有在启用 ProRes 帧级线程后观察到过度抢核时，才比较：

```text
filtergraph = auto / 2 / 4
```

触发条件包括：

- encoder 长时间等待 CPU；
- worker 频繁空闲而总吞吐不升；
- CPU 利用率接近满载但 wall time 不降；
- 温度升高导致频率下降；
- 4 线程编码比 3 线程更慢。

Apple Log/LUT 的 RGB/YUV 往返属于后续色彩管线优化，不应和首轮编码器线程改动混合验证。任何改变都必须做色卡、灰卡、白卡和颜色元数据回归。

### 8.2 暂不更换 inflate 库

当前 inflate 约 3–16 ms/帧，远小于编码器主耗时。因此暂不建议优先引入 zlib-ng、libdeflate 或新的解压线程模型。

允许的低风险微优化是复用 worker-local 输入 buffer，但必须先确认这不会增加内存驻留和错误清理复杂度。

### 8.3 暂不优先重编 FFmpeg

[scripts/build-ffmpeg-android.sh:427-461](../scripts/build-ffmpeg-android.sh:427-461) 使用 `--enable-small`。去掉它、启用 LTO、调整 CPU/NEON 参数或修改预编译库，均属于独立构建实验：

- 先完成 Release app native 和 ProRes 线程验证；
- 再单独比较速度、包体、启动和 ABI 兼容性；
- 不要把重编 FFmpeg 与零拷贝、depth 和编码器线程同时提交，否则无法归因。

## 9. 推荐实施顺序

### 阶段 A：基线

1. 构建 Release arm64 性能包；
2. 检查实际 `-O2/-O3`；
3. 固定一份 4096×3072 DNG ZIP 和一份 1080p 样本；
4. 跑 60 帧 steady-state + 一次 900 帧长跑；
5. 记录分段耗时、`VmHWM`、温度、频率、帧数和输出 hash。

### 阶段 B：最高收益

1. 只改变 `prores_ks` 的 `thread_count` 和 `FF_THREAD_FRAME`；
2. 比较 1/2/3/4 线程；
3. 选取不超过内存/温控预算的候选；
4. 完成 framemd5、帧数、时长和取消回归。

### 阶段 C：解码与内存

1. 在固定编码器线程后调 worker/depth；
2. 做 worker-local LibRaw 复用 A/B；
3. 再做 AVFrame 池化；
4. 最后尝试 worker 直写目标 buffer；
5. 每次只改变一项，避免无法归因。

### 阶段 D：产品策略与微优化

1. faststart 0/1 长文件对比；
2. 首帧结果复用；
3. 仅在过度抢核时限制 filtergraph；
4. 最后才评估 FFmpeg 构建参数和压缩库。

不要把理论收益直接相乘。每个阶段完成后都必须重新测量，因为编码器提速会改变整个流水线的瓶颈排序。

## 10. 线程与内存预算建议

首轮不要使用全自动线程叠加。建议把总资源拆成四部分：

```text
DNG worker 解码线程
+ ProRes 编码器帧线程
+ filtergraph 自动线程
+ 主循环 / 写盘 / Android 服务线程
```

初始实验可采用：

| 设备类型 | DNG worker | depth | ProRes encoder | filtergraph |
|---|---:|---:|---:|---|
| 低核/低内存 | 2 | 2 | 2 | auto |
| 中等设备 | 3 | 3 | 2–3 | auto |
| 高核设备 | 3–4 | 3–4 | 3–4 | auto，必要时限 4 |

以上只是测试起点，不是最终默认值。最终选择必须同时满足：

- 4K `VmHWM` 不逼近或超过约 500 MB；
- 1080p `VmHWM` 不逼近或超过约 400 MB；
- 温控不导致长跑频率明显下降；
- worker、encoder 和主线程均有可观有效工作；
- 取消和失败时没有线程泄漏、死锁或输出文件竞态。

## 11. 验收标准

### 11.1 性能指标

每次 A/B 必须固定：设备、ABI、BuildType、输入 URI、素材、输出参数、温度起点和后台负载。记录：

- 总 wall time；
- 首帧 setup 与 steady-state 分开耗时；
- P50/P95 每帧耗时；
- read/inflate/LibRaw/filter/encode/mux/finalize 分段耗时；
- `VmHWM`、峰值 RSS；
- CPU 利用率、温度和降频；
- worker、encoder、filtergraph 实际线程数。

### 11.2 输出正确性

至少校验：

- `framemd5` 或逐帧 hash；
- 首、中、尾各 5 帧；
- 输出帧数、时长、PTS 和尺寸；
- ProRes profile 和像素格式；
- Apple Log/LUT 的画面、色彩元数据和矩阵；
- faststart 开关下 MOV 的可播放性和 `moov` 位置。

如果线程化后不能保证逐字节相同，则至少使用 PSNR/SSIM、灰卡/白卡、色卡 ΔE、码率和人工目检建立新的容差口径，不能只以“播放器能打开”验收。

### 11.3 稳定性与正确性

性能变更不得削弱：

- DNG 严格帧序和尺寸一致性；
- 解码失败立即传播；
- cancel、Service 销毁和输出删除顺序；
- 普通容器解码、AAC/BSF 和 encoder flush 错误处理；
- 32 位 ABI 大文件读写；
- 低内存设备行为；
- 现有预览路径和单任务串行调度。

### 11.4 历史目标

项目契约记录的长期目标为：

- 1080p60、900 帧小于 20 分钟；
- 4K、900 帧小于 90 分钟；
- 1080p 峰值内存约不超过 400 MB；
- 4K 峰值内存约不超过 500 MB。

这些目标应先在 Release arm64 真机上重新确认，不能直接把历史 Debug 包或 x86_64 主机数字视为当前设备基线。

## 12. 最终建议

第一轮只做两件事：

1. **Release arm64 基线**；
2. **`prores_ks` 的 2/3/4 线程帧级 A/B**。

在这两项完成之前，不建议：

- 调大 DNG depth 并据此宣称加速；
- 把 filtergraph `nb_threads=0` 改成固定线程；
- 更换 inflate 库；
- 开启 LibRaw OpenMP；
- 重编整套 FFmpeg；
- 实施高风险零拷贝。

第一轮结果出来后，再按“编码器线程 → depth/worker → LibRaw 复用 → AVFrame 池化/零拷贝 → faststart/首帧 → 构建微调”的顺序推进。这样可以保持性能收益可归因，同时不牺牲 DNG 顺序、内存安全和输出正确性。

## 13. 证据索引

- [docs/PERF-RAW-TO-PRORES-VERIFIED-2026-10-06.md](PERF-RAW-TO-PRORES-VERIFIED-2026-10-06.md)：实测瓶颈排序、编码器线程矩阵、Release 微核、filtergraph 自动线程修正。
- [app/src/main/cpp/transcode.cpp:1417-1450](../app/src/main/cpp/transcode.cpp:1417-1450)：ProRes 编码器配置。
- [app/src/main/cpp/transcode.cpp:1647-1693](../app/src/main/cpp/transcode.cpp:1647-1693)：filtergraph 创建。
- [app/src/main/cpp/transcode.cpp:1865-1882](../app/src/main/cpp/transcode.cpp:1865-1882)：每帧 AVFrame 分配与 RGB 复制。
- [app/src/main/cpp/transcode.cpp:487-564](../app/src/main/cpp/transcode.cpp:487-564)：faststart 文件搬移。
- [app/src/main/cpp/dngframeq.h:275-309](../app/src/main/cpp/dngframeq.h:275-309)：depth 对在飞帧的限制。
- [app/src/main/cpp/dngzip.cpp:830-902](../app/src/main/cpp/dngzip.cpp:830-902)：LibRaw 实例生命周期与输出转换。
- [app/src/main/cpp/CMakeLists.txt:13-40](../app/src/main/cpp/CMakeLists.txt:13-40)：native 构建配置现状。
- [scripts/build-ffmpeg-android.sh:427-461](../scripts/build-ffmpeg-android.sh:427-461)：FFmpeg 构建参数。
- [docs/INTERFACES.md:523-529](INTERFACES.md:523-529)：历史性能和内存验收目标。

---

**结论：** 先用 Release 包建立可信基线，再给 `prores_ks` 设置有限的帧级线程；其余优化按新瓶颈和真机内存数据逐级推进。当前证据不支持优先改 filtergraph 线程或更换 inflate 库。
