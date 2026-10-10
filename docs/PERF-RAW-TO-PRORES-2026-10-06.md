# ProResForge 只读性能审查：raw → ProRes（DNG 序列 zip 路径）

审查日期：2026-10-06
审查范围：`read_frame → zlib inflate → LibRaw → rgb48le → 滤镜图(sws/lut) → prores_ks → mov` 全链路的
CPU 并行度、编译选项、每帧内存操作与编码器/滤镜线程配置。
审查方式：静态逐行复核 native 源码 + 预编译产物的编译命令 + FFmpeg n6.1.1 上游原始源码核实；
本轮不修改源码、不执行构建或测试（沿用用户「审查默认只读」口径）。

## 结论

现阶段 4096x3072 下每帧 ~256ms 的瓶颈**已不在 LibRaw 解码**，而在主线程的「滤镜 + ProRes 编码 + 写盘」
这一段；而这一整段（含 swscale/lutrgb）目前是**单线程**运行的，同时 app 自己的 native 源码全部以
**-O0** 编译。三个高杠杆项全部属于「能力已存在、只是没打开」：

1. `prores_ks` 明确声明支持 slice/frame 多线程，但代码只设了 `profile`，一个线程参数都没有（`transcode.cpp:1417-1424`）。
2. 滤镜图/swscale/lutrgb 全部支持分片多线程，但 `graph->nb_threads` 从未设置（默认 0 = 单线程）。
3. app 源码 -O0（对比 FFmpeg/LibRaw 都是 -O3）。
4. 附带一个结构性误区：解码并行度被 `depth=2` **硬顶成 2**，真机上 `nthreads=4` 有 2 个 worker 永远在等空位
   （这与实测「2T 258ms ≈ 4T 256ms」完全吻合）。

## 基线（2026-10-05 实测，4096x3072 BALANCED，真机样本 5.22GiB / 335 帧）

| 阶段 | 单线程 | 2 线程 | 4 线程 | 8 线程 |
|---|---|---|---|---|
| 每帧 read+decode（worker 内） | 407ms（read 56 / decode 351） | — | — | — |
| 纯 decode | 368ms | 212ms | 124ms | 117ms |
| 整帧总耗时 | 433ms | 258ms | **256ms** | — |
| depth=1（无重叠） | 424ms | — | — | — |

两个读数值得注意：① 2T 与 4T 的总耗时几乎相同 —— 这不是巧合，是 `depth=2` 封顶的结果（见 P2-01）；
② host bench 的「4T 124ms 纯解码」是在 depth≥4 下测的，**不代表真机默认配置**。
另：纯解码 4T→8T 只从 124ms 到 117ms，说明 4 路之后接近内存带宽/热设计上限，堆线程收益递减。

## 发现

### P1-01：app 自己的 native 源码以 -O0 编译（最高杠杆、零风险）

证据（`app/.cxx/Debug/4b6u6p29/arm64-v8a/`）：

- `compile_commands.json` 里 transcode.cpp 的编译命令为
  `-g -DANDROID -fdata-sections -ffunction-sections -funwind-tables -fstack-protector-strong -no-canonical-prefixes -D_FORTIFY_SOURCE=2 -Wformat -Werror=format-security -fno-limit-debug-info -fPIC -std=gnu++17`
  —— **没有任何 `-O` 标志**，也没有 `-DNDEBUG`。
- `CMakeCache.txt`：`CMAKE_BUILD_TYPE:STRING=Debug`、`CMAKE_CXX_FLAGS:STRING=`、`CMAKE_CXX_FLAGS_DEBUG:STRING=`（空）；
  只有 `RELEWITHDEBINFO=-O2 -g -DNDEBUG` / `MINSIZEREL=-Os -DNDEBUG` 有 `-O`。
- `app/src/main/cpp/CMakeLists.txt`（全文 103 行）只设 C++17、include 目录与 IMPORTED 链接，**未设 CMAKE_BUILD_TYPE、未设任何 -O**。
- 对照：`scripts/build-ffmpeg-android.sh:254` 的 `common_cflags="-fPIC -O3 -fno-strict-aliasing"`（FFmpeg/x264/x265/libvpx 全部 -O3）；
  LibRaw 走 CMake `-DCMAKE_BUILD_TYPE=Release`（默认 -O3）。

受影响的热点（全在 app 自己代码里，FFmpeg/LibRaw 已经 -O3）：

- `dngzip.cpp:890-898` `dst[3*x+0..2]=src[4*x+0..2]` 逐像素拷贝：12.6M 像素 × 3 通道，
  -O0 下索引算术、load/store 全在内存里走，无自动向量化；这段在 worker 内，直接拖长每帧 403ms。
- `transcode.cpp:1877-1881` 逐行 `memcpy` 循环及所有胶水代码（pts/rescale/日志/池操作）。

预期：worker 解码 -10~30ms/帧，主线程 -5~15ms/帧，整链约 -5~15%。改法是给外部 native 构建加
`-DCMAKE_BUILD_TYPE=Release`（或直接在 CMakeLists 里兜底 `-O2`），属零行为风险改动。

### P1-02：ProRes 编码器没有启用多线程（`prores_ks` 支持，但代码没设）

证据：

- `transcode.cpp:1417-1424` ProRes 分支只做 `av_opt_set(enc->priv_data,"profile","hq"/"4444")`；
  全文件 grep `thread_count|thread_type|nb_threads|FF_THREAD` 只有 `transcode.cpp:1287-1288`
  两行，且那是**非 zip 路径的解码器**（`dec->thread_count = 4; dec->thread_type = FF_THREAD_FRAME|FF_THREAD_SLICE`），
  编码器与滤镜图都没有任何线程设置。x265 用了 `row-mt`、x264 用 preset，唯独 ProRes 分支是裸的。
- 上游 n6.1.1 原始源码 `libavcodec/proresenc_kostya.c`（`prores_ks` 的真实文件）：
  `ff_prores_ks_encoder` 的 `.p.capabilities = AV_CODEC_CAP_SLICE_THREADS | AV_CODEC_CAP_FRAME_THREADS | AV_CODEC_CAP_ENCODER_REORDERED_OPAQUE`，
  `encode_init` 里 `ctx->tdata = av_calloc(avctx->thread_count, ...)`（按 thread_count 分配 trellis 线程数据）。
  → 422 HQ 的 trellis 量化天然按线程数分工。
- 编码器链确认是 `prores_ks`：`transcode.cpp:579` `{"prores", {"prores_ks", nullptr}, "mov"}`，
  `transcode.cpp:1363` `is_prores=(enc_name=="prores_ks")`。

预期：主线程三项（sws / encode / write）里 encode 通常最重，2~3 线程 slice/frame 并行预计
省 40~70ms/帧；这是把主线程 256ms 压下来的最大单项。
注意点：① 先试 `FF_THREAD_SLICE`（无额外帧延迟，适合逐帧同步 send/receive 的模式），再试 `FF_THREAD_FRAME`；
现有 `encode_out()` 的两处 EAGAIN 排空逻辑对两种都成立。② thread_count 必须与解码线程、滤镜线程分核
（见「建议执行顺序」的三方预算），不能无脑拉满。

### P1-03：滤镜图 / swscale / lutrgb 全部单线程（`graph->nb_threads` 未设）

证据（上游 n6.1.1 原始源码 + 本地调用方式）：

- 本地 `avfilter_graph_alloc()` 之后没有任何 `nb_threads` 赋值 → `ff_filter_get_nb_threads()` 返回 0，
  带 slice 线程能力的滤镜以「当前线程内联执行」处理。
- `libavfilter/vf_scale.c`：scale 滤镜自身的 option 列表没有 threads，但 `child_class_iterate` 返回
  `sws_get_class()`，且 `init()` 里
  `av_opt_get_int(scale->sws_opts,"threads",0,&threads); if(!threads) av_opt_set_int(scale->sws_opts,"threads", ff_filter_get_nb_threads(ctx),0)`
  → **滤镜图默认线程数会自动传导给 swscale**，无需改滤镜字符串。DNG 路径 RGB48LE→yuv422p10
  in!=out 格式必然新建 sws（`config_props` 的跳过条件不满足），正好是主线程里的一块常量。
- `libavfilter/vf_lut.c`（注意 `lutrgb` 就在这个文件里，另有 `lut`/`lutyuv` 共用同一实现）：
  `uint16_t lut[4][256*256]` 先把表达式对每个可能取值只算一次（65536×3 次 log/pow，建表一次、约十几 ms），
  之后每帧纯查表；`.flags = AVFILTER_FLAG_SLICE_THREADS` +
  `ff_filter_execute(..., FFMIN(in->height, ff_filter_get_nb_threads(ctx)))` → 可按行分片多线程。
  → 此前「applelog 曲线每像素算 log/pow 会拖垮」的猜测**不成立**（代价是 12.6M×3 = 37.7M 次 16-bit 查表，
  单线程几十 ms，且可并行）。`lut3d`（LUT 路径）同样是 slice-threaded。

改法与时机：`avfilter_graph_config()` **之前** `graph->nb_threads = N`（vf_scale 在 init 时读取，晚了不生效）。
预期：主线程 -30~80ms/帧。

### P2-01：解码并行度被 `depth=2` 封顶，`nthreads>2` 时是无效配置

证据（`app/src/main/cpp/dngframeq.h`）：

- `produce_one()`（L267-285）在锁内 `m_cv_prod.wait(lk, []{ ... || (m_assigned - m_consumed) < m_depth; })`，
  醒来后 `seq = m_assigned++`，**出锁后才解码**；`m_consumed` 只在主线程 `next()` 取走帧时 +1。
  → 任何时刻「已派未取」的帧数 ≤ depth，而这些帧各自由一个 worker 独占解码 →
  **同时处于解码态的帧数硬上限 = depth**，与 worker 数无关。
- `transcode.cpp:162-164`：`p.depth = spec_depth>0 ? spec_depth : 2`，`L117-118` 注释也称「预取深度固定 2」；
  而 L151-155 单独按内存/核数推 nthreads —— 两者没有联动。

推论：真机默认 4K（nthreads=4、depth=2）下只有 2 帧在飞，另外 2 个 worker 全程睡在 `m_cv_prod`；
日志里「4 线程 / 预取深度 2」是误导性数字。这与实测 2T 258ms ≈ 4T 256ms 完全一致。
喂饱 N 个 worker 所需在飞深度 ≈ N ×（主线程每帧时间 / worker 每帧时间）= 4 × 256/403 ≈ 2.5，
再叠加 read 56ms 的 I/O 抖动，合理起点是 `depth ≈ 2 × nthreads`（内存允许时）。
内存代价：每 +1 depth 多占一块槽 ≈ 15.7MB(raw) + 72MB(rgb)；
`dng_parallel_params` 的 1GB 预算公式（`kBudget/(rgb_bytes*2.95+16MB)`，4K 得 by_mem=4）本来就能复用来定 depth。
**注意**：在 P1-02/P1-03 完成前单独调大 depth 看不出收益——主线程 256ms 仍是长板，解码从 212ms 降到
124ms 不会缩短总时长。这两类改动必须一起上，顺序反了会误判为「无效」。

### P2-02：每帧重新分配 72MB（`av_frame_get_buffer`）+ 额外 72MB `memcpy`

证据（`transcode.cpp:1865-1881`）：每帧 `av_frame_unref(dframe)` →
`dframe->format/width/height/pts` 赋值 → `av_frame_get_buffer(dframe, 32)`（72MB 4096x3072x3x2，
含分配 + 清零 + 页错误）→ 再逐行 `memcpy` 72MB。每帧合计 ~215MB 内存流量 + 一次 72MB 级 scudo 分配，
335 帧重复 335 次；而队列另一侧的 buffer 已经做了 swap 复用（`dngframeq.h:359-364`），只有这一处没有池化。
改法（三选一，收益递增）：
(a) `push_decoded()` 改用 `AV_BUFFERSRC_FLAG_KEEP_REF`，主线程保留 buffer 所有权，下一帧直接覆盖同一块
（省掉 alloc+zero，memcpy 仍在）；
(b) 自己持有常驻 AVFrame + 把 unref 推迟到下一帧 push 之前（需确认图内滤镜不会改写输入帧）；
(c) 最彻底：让 worker 按目标 `dframe->linesize` 直接写（解码阶段就知道目标行距）→ 同时消灭 memcpy 与每帧分配，
并把 72MB 从 worker 峰值内存里挪走（顺带缓解 P2-01 的内存压力）。
预期：-20~50ms/帧，与是否 -O2/-O3 无关。

### P2-03：worker 内的 zlib inflate 未单独计时（可能是 403ms 里的第二块）

证据：`dngzip.cpp:658-674` `inflateInit2(&zs,-15)` 裸 deflate、1MB 块（`kInflateChunk`）、
`out.resize(ent.usize)` 直出整帧；同时每帧新建 `std::vector<uint8_t> inbuf(kInflateChunk)`（不跨帧复用）。
口径：每帧约 15.7MB 压缩态 → 25MB 解出（这才是 TIFF），随后 LibRaw 的 LJpeg 才把 25MB 扩成 100MB raw。
inflate 的量级估 80~150ms/帧，但现在被算在「decode 351ms」里，没有拆开测过，无法确认占比。
建议：先加计时（inflate / unpack / dcraw_process 三段），若 inflate 占大头再评估 zlib-ng 或 libdeflate
（drop-in `inflate`，带 NEON/AVX2 路径，通常 2~3×）；并把 `inbuf` 挪进槽位跨帧复用（零成本）。

### P3-01：LibRaw 只有单线程构建（记录，不建议动）

`scripts/build-ffmpeg-android.sh:583` `rm -f libraw_r.a libraw_r.pc`、L566-568
`-DCMAKE_DISABLE_FIND_PACKAGE_OpenMP=ON -DENABLE_OPENMP=OFF`。帧级并行已经覆盖了这个维度；
为 LibRaw 开 OpenMP 会与现有 4 路帧并行重复争核，并让线程数决策复杂化。仅记录，保持现状。

### P3-02：零头（仅记录，收益小）

- `read_frame` 每帧 1MB 块 `pread`（16 次 syscall，可忽略）；冷缓存下 56ms 基本是闪存读，压不动。
- DNG 路径的 `fps=N/D` 滤镜是 pass-through（pts 已是 1/fps），不产生复制。
- `av_interleaved_write_frame` 每包直写，大包绕过 AVIO 缓冲，无双拷贝。
- 构建脚本 `L491-501` 只校验 `CONFIG_PRORES_ENCODER`，而实际链是 `prores_ks`
  （应校验 `CONFIG_PRORES_KS_ENCODER`）；两者默认都编进 FFmpeg 6.1，不影响性能，只是校验项名不符实。

## 建议执行顺序

1. **先仪器化**（只加日志，不改逻辑）：worker 内 inflate/unpack/dcraw_process 三段 + 主线程 sws/encode/write
   三段；同时记录 `VmHWM`。没有这组数，后面的收益都只能靠猜。
2. **-O2/-O3**（app 源码）：零行为风险，先落地，再复测同一素材。
3. **打开主线程并行**：`prores_ks` 的 `thread_count/thread_type` + 滤镜图 `nb_threads`。
   按核数做一份三方预算，例如 8 核机：解码 3 线程 / depth 6、滤镜图 2、编码 2，主循环独占 1 核。
4. **消掉每帧 72MB**：KEEP_REF 或池化；中期按 (c) 让 worker 直写 dframe。
5. **联动 depth 与 nthreads**（至少 depth ≥ nthreads），用 `VmHWM` 校验不逼近 LMK。
6. 视第 1 步结果决定是否引入 zlib-ng/libdeflate。

## 验证方法

- 同一台机、同一 5.22GiB/335 帧样本，前后各跑一轮；对比每帧总耗时、各阶段耗时、`VmHWM`、发热档位与是否触发 LMK。
- 输出一致性：线程数/线程类型变化**不应**改变任何一帧的码流，用
  `ffmpeg -i out.mov -f framemd5 -`（或 `-f md5`）对比改动前后；prores_ks 的 trellis/量化与线程数无关，
  这条断言应当成立。
- 回归面：契约 §10.3（DNG 序列语义：不静默跳帧、尺寸一致性）与现有 host 单测保持不动；
  改动后补一轮「取消/失败路径」手测。

## 证据索引

- `app/.cxx/Debug/4b6u6p29/arm64-v8a/compile_commands.json`、`.../CMakeCache.txt`（编译选项核实）
- `app/src/main/cpp/CMakeLists.txt`（全文，无 -O）
- `app/src/main/cpp/transcode.cpp:110-166`（dng_parallel_params）、`:802-898`（encode_out/drain_graph/push_decoded）、
  `:1287-1288`（唯一的线程设置，属解码器）、`:1417-1424`（ProRes 分支）、`:1770-1826`（队列启动）、`:1865-1881`（每帧分配+memcpy）
- `app/src/main/cpp/dngframeq.h:261-316`（worker_loop/produce_one）、`:318-371`（next）
- `app/src/main/cpp/dngzip.cpp:658-674`（inflate）、`:890-898`（逐像素拷贝）
- `scripts/build-ffmpeg-android.sh:254`（-O3）、`:427-462`（FFmpeg configure）、`:560-583`（LibRaw 单线程）、`:491-501`（校验项）
- 上游 FFmpeg n6.1.1 原始源码：`libavcodec/proresenc_kostya.c`、`libavfilter/vf_scale.c`、`libavfilter/vf_lut.c`
