# raw(DNG zip) → ProRes 性能优化点复核报告（只读实测版）

- 日期: 2026-10-06
- 目的: 回答「raw→ProRes 这条流水线还能不能继续优化性能」。**本次审查不改任何仓库代码**；所有实验都在 WSL `/tmp` 下的源码副本或独立探针中进行，仓库内只新增本文件。
- 与前一份文档的关系: `docs/PERF-RAW-TO-PRORES-2026-10-06.md` 已列出 P1/P2/P3 清单。本报告用**实测**重新排序，并**修正其中一条错误结论**（见 §5）。

---

## 1. 结论（TL;DR）

1. **当前端到端瓶颈是 prores_ks 编码器，占 94–95% 的时间；而它在 app 里是「单线程」在跑。**
   原因不是「没开线程」这句话本身，而是 FFmpeg 的 `AVCodecContext.thread_count` **默认值就是 1**（不是 0=auto）：
   `app/src/main/cpp/prebuilt/arm64-v8a/include/libavcodec/options_table.h:220`
   `{"threads", "set the number of threads", OFFSET(thread_count), AV_OPT_TYPE_INT, {.i64 = 1 }, 0, INT_MAX, V|A|E|D, "threads"}`
   （`armeabi-v7a` 同址同样内容；`:221` 才是 `auto = 0`。）
   `transcode.cpp` 只给**输入解码器**设过线程（`:1287-1288 dec->thread_count = 4`），编码器侧全文件没有任何 `thread_count` 赋值。
   实测后果（应用同源 harness，`avcodec_open2` 之后打印）：`ENC thread_count=1 active_thread_type=0`。
2. **一处改动（编码器线程数）实测带来 ~1.8× 端到端提升，且几乎不涨内存**：同源 harness 跑真实 4 帧 zip，wall 从 **23.7–24.0 s（`thread_count=1`，现状）降到 12.3–15.0 s（=4）**；`thread_count=2` 就已经到位（13.3 s），8/12 线程不再更快（12.9 s）→ **设 2–4，不要留默认 1，也不要用 0=auto**。MaxRSS 778.6 MB(1) → 778.9–779.7 MB(2/4/12)，**增量 ≤1.1 MB**（编码器只持有输入帧引用，不复制帧数据）。详见 §3.6 与 §4-P0-1。
3. **第二条独立优化：app 自身 native 代码目前是 `-O0` 构建**（`app/.cxx` 下只有 `Debug` 目录，`app/build/outputs` 下只有 `app-debug.apk`）。实测一个 12.58 MPix 的 `ushort[4]→ushort[3]` 转换循环：`-O0` **36.1 ms** vs `-O2/-O3` **5.6 ms**（6.4×）。这段循环每帧都在解码路径里（`dngzip.cpp:890-898`）。
4. **不要再花时间在滤镜图线程上**：`nb_threads=0` 已经是「自动」，`avpriv_slicethread_create()` 会取 `min(ncpus+1,16)`，`vf_scale` 又把它转发给 swscale 的 `threads` 选项。实测 swscale 单线程 108.0 ms/帧 vs 自动 23.6 ms/帧，harness 实测滤镜链只有 ~25–56 ms/帧 —— **它已经是并行的**（A/B harness 在 app 自己的建图代码之后直接打印：`graph nb_threads=13 thread_type=1(AVFILTER_THREAD_SLICE) filters=6`）。既有文档 P1-03「滤镜图默认单线程」不成立。
5. 编码器修好之后，下一个天花板才是：**解码并行度被队列 depth=2 硬顶**（`dngframeq.h:275-278`）、**每帧 72 MB 分配+拷贝**（`transcode.cpp:1865-1881`）、**每帧重建 LibRaw 与 ~125 MB 内部缓冲**（`dngzip.cpp:830/:902`）。

| 编号 | 优化点 | 预期收益 | 改动量 | 风险 |
|---|---|---|---|---|
| P0-1 | 编码器 `thread_count`（建议 **2–4**，不要 auto） | **端到端 1.78×(1→2 实测)**；编码器隔离口径 4.4×(1→12) | 1–2 行 | 内存增量实测 ≤1.1 MB；输出不可逐字节复现，需 PSNR/容差口径 |
| P0-2 | 用 Release(-O3) 构建 native | 解码路径每帧省 ~30 ms(主机口径)，真机 ARM 上更大 | 构建配置 | 无（只是别用 Debug 出性能数） |
| P1-3 | 队列 depth 从 2 提高（让 4 个 worker 真的在解码） | 解码 235 ms→~120–150 ms/帧（上限受内存） | 1 个常量/参数 | 内存线性增长 |
| P1-4 | 每个 worker 复用一个 LibRaw 实例 | 未实测，量级数十 ms/帧 + 内存churn | 小 | 需确认 LibRaw 复用语义 |
| P2-5 | 去掉每帧 72 MB 分配 + memcpy（零拷贝 AVFrame） | 主机实测 memcpy 4.2–6.6 ms；分配/清零另计 | 中 | 生命周期/所有权 |
| P2-6 | `faststart` 默认改为关闭（大文件时） | 省一次整文件重写 I/O | 1 行 | 兼容性（moov 前置） |
| P3-7 | 去掉首帧重复解码（3 次） | ~0.4 s/任务 | 小 | 无 |
| P3-8 | 调 FFmpeg 构建（去 `--enable-small` 等） | 未实测，预计 0–5% | 构建脚本 | 体积 |

---

## 2. 测量方法与证据等级

所有数据都来自 x86_64 主机的**只读复现**（12 核 / 23 GB / WSL2），不是真机：

- **[A] 我方独立 C 探针**：直接链仓库同版本静态 FFmpeg 6.1（`libavutil 58.29.100 / libavcodec 60.31.102 / libavfilter 9.12.100`），分别测 swscale 直调、prores_ks 编码线程矩阵、微内核 `-O0/-O2/-O3`。
- **[B] 应用同源 harness（插桩，源码副本在 `/tmp`）**：把 `app/src/main/cpp/{transcode.cpp,dngzip.cpp}` + `scripts/transcode_harness.cpp` 用 host 版 FFmpeg/LibRaw 编译，逐帧打印 consumer 链各段耗时。
- **[C] 应用同源 harness（端到端，另一条独立路径）**：同样源码副本编到 `/tmp/prfbench/th`，跑真实 4 帧 DNG zip（stored，4096×3072，15.7 MB/帧），同进程内插桩分段。
- **[B+] 端到端 A/B（本报告最关键的新证据）**：`/tmp/prfplus/{base,thr,tc2,tc12}` 同一份源码副本，唯一差异是编码器 `thread_count`；同一份 4 帧 zip、同一编译参数，用 `/usr/bin/time -v` 取 wall + MaxRSS。§3.6 与 §4-P0-1 的实测均来自它，并在 app 自己的代码里打印了 `graph nb_threads` / `enc thread_count/active/type`。
- **[D] 历史数据**：仓库既有文档/表格里的设备侧数字（VmHWM、并行表）。

⚠️ 口径警告：
- 主机绝对值**不能当设备预期**（ARM 大核更少更弱），但「谁占 94%」「开线程能快几倍」这类**结构性结论**可以移植。
- [B] 与 [C] 的编码单线程绝对耗时差了约 4×（1.4 s/帧 vs 5.6–7.0 s/帧），原因是输入内容、zip 制式不同，且当时主机上有多个并行编译/基准在抢 CPU。**只应引用它们的比例与结论，不要引用绝对毫秒数。** 本报告新增的 [B+]（同源 harness、单线程、真实 4 帧）为 **5.9 s/帧**，与 [C] 一致 → [B] 的 1.4 s/帧应视为异常值。
- [C] 还发现一个测量陷阱：harness 的 `av_log` 行尾换行会被 FFmpeg 吃掉，按行读 stderr 打时间戳会把两行粘在一起，得到「init 阶段 10.4 s」的假象（同进程插桩显示 setup 只有 0.42 s）。**分段耗时必须在同进程内计时。**

---

## 3. 瓶颈定位（数据）

### 3.1 端到端分段 [C]，4 帧真实 zip，wall = 23.5 / 24.8 / 29.5 s

| 阶段 | 时间 | 占比 |
|---|---|---|
| setup（开 zip + 首帧解码 + 编码器/滤镜图初始化） | 0.42 s | ~2% |
| LibRaw 解码（4 线程池，被流水线掩盖） | 0.48–0.50 s / 4 帧（≈0.36–0.42 s/帧） | — |
| `av_buffersrc_add_frame_flags` | 0.000 s | 0 |
| 滤镜图 sink 拉取（含 swscale 转换） | 0.225 s / 4 帧（≈56 ms/帧） | ~1% |
| **encode + mux（prores_ks）** | **22.2–28.1 s / 4 帧 = 5.6–7.0 s/帧** | **94–95%** |

> [B+] 用同一份 4 帧 zip（`tmp/dngzip-test-4f.zip`）独立复现：`thread_count=1` 时 wall = **23.74 / 24.03 s**，与本节 [C] 的 23.5 / 24.8 / 29.5 s 互相印证（两者单线程编码都落在 ~5.9–7.0 s/帧）。

### 3.2 consumer 链逐帧拆分 [B]（同源 harness，4 帧真实 DNG）

| 段 | 每帧耗时 |
|---|---|
| `av_frame_get_buffer` + 逐行 memcpy（72 MB） | 46–51 ms |
| 滤镜图（buffersrc→buffersink，含 RGB48→YUV422P10） | 24.6–25.6 ms |
| `avcodec_send_frame` | **~1430 ms** |
| `avcodec_receive_packet` | 0.002 ms |
| `av_interleaved_write_frame`（mux） | 4.4 ms |
| 合计 push | ~1460 ms/帧 |

→ **mux 免费；encoder 吃掉 ~96%。** 另：`avcodec_open2` 后打印 `thread_count=1 active_thread_type=0`。

### 3.3 编码器线程矩阵 [A]（prores_ks，4096×3072，yuv422p10le，HQ）

| thread_count | ms/帧 | 相对 1 线程 |
|---|---|---|
| 1 | 950.8 | 1.00× |
| 2 | 536.2 | 1.77× |
| 4 | 320.4 | 2.97× |
| 8 | 285.0 | 3.34× |
| 12（auto） | 216.9 | **4.38×** |

输出**逐字节相同**（每次运行 131,401,800 B）。而 `slices=4` 的**切片**线程基本无收益（1 线程+slices=4 = 478.6 ms vs 1 线程 490.6 ms）——与上游「只有 quant 搜索走 `execute2`」一致。**所以要用帧级线程，不要用切片线程。**

### 3.4 滤镜图/ swscale 已经是并行的 [A] + 源码

- `libavfilter/pthread.c:83-108`（`HAVE_THREADS` 实现）：`nb_threads == 1` 才关闭线程；否则 `thread_init_internal()` → `avpriv_slicethread_create()`。
- `libavutil/slicethread.c:107-113`：`nb_threads == 0`（app 的默认）→ `FFMIN(nb_cpus+1, MAX_AUTO_THREADS=16)`（`slicethread.c:27`）。
- `libavfilter/avfilter.c:786-791` `ff_filter_get_nb_threads()`；`libavfilter/vf_scale.c:283-288,353-357`：把该值写进 swscale 的 `threads` 选项（`libswscale/options.c:82-83`，默认 1，0=auto）。
- 实测: RGB48→YUV422P10、4096×3072：sws `threads=1` = **108.0 ms/帧**；`threads=0(auto，回读 13)` = **23.6 ms/帧**；`threads=12` = 23.5 ms/帧。
- `transcode.cpp:1647` 起建图，全文件没有 `vp.g->nb_threads` / `AVFilterContext.nb_threads` 赋值 → 保持 0 → 自动并行。

### 3.5 app 自身 native 是 -O0 [A] + 仓库证据

- `app/.cxx` 下只有 `Debug/{214y1y33,4b6u6p29}/{arm64-v8a,armeabi-v7a}`；每个 `CMakeCache.txt:49` 均 `CMAKE_BUILD_TYPE:STRING=Debug`，`:58/:61/:67` 的 `CMAKE_CXX_FLAGS*` 全为空；`app/build/outputs` 下只有 `app-debug.apk`（60.8 MB，2026-10-06 04:57）。
- 微内核实测（12.58 MPix，`ushort[4]→ushort[3]`，即 `dngzip.cpp:890-898` 的形状）：`-O0` **36.1 ms**、`-O2` **5.6 ms**、`-O3` **5.6 ms**。逐行 memcpy 72 MB：4.0–4.3 ms（与 `-O` 无关）；`memset` 100 MB：1.5–1.7 ms。

### 3.6 端到端 A/B：编码器线程数 [B+]（同源 harness，真实 4 帧 zip，4096×3072）

输入 `/tmp/in4.zip`（= 仓库 `tmp/dngzip-test-4f.zip`，4 帧 stored，62,936,710 B）。四个二进制来自同一份源码副本，**唯一差异**是在 `transcode.cpp:1450` 的 `avcodec_open2` 之前插入 `enc->thread_count = N; enc->thread_type = FF_THREAD_FRAME;`，编译参数完全相同（`g++ -O2`，host FFmpeg 6.1 + LibRaw 0.21.4 静态库）：

| `thread_count` | 实测 open2 后打印 | wall | MaxRSS | 相对 1 线程 |
|---|---|---|---|---|
| 1（现状） | `tc=1 active=0 type=3 slices=0 delay=0 caps=0x103000` | 23.74 / 24.03 s | 778.6 / 778.8 MB | 1.00× |
| 2 | `tc=2 active=1 type=1` | 13.34 s | 778.9 MB | **1.78×** |
| 4 | `tc=4 active=1 type=1` | 12.25 / 14.96 s | 779.0 / 779.0 MB | 1.61–1.94× |
| 12 | `tc=12 active=1 type=1` | 12.94 s | 779.7 MB | 1.83× |

同一次运行里 app 自己的代码还打印了滤镜图状态（插在 `transcode.cpp:1700` 的 `avfilter_graph_config` 之后）：

```
PRFDIAG path=base graph nb_threads=13 thread_type=1 filters=6
```

即 **`nb_threads=13`、`thread_type=AVFILTER_THREAD_SLICE`** —— 滤镜图确实自动并行了（13 = 12 核 + 1，来自 `avpriv_slicethread_create()`），与 §3.4 的源码推断一致，彻底否掉「滤镜图默认单线程」。

三点结论：
1. **2 线程就到拐点**（13.3 → 12.9 s 是平台），说明编码器不再是关键路径；编码器隔离口径（§3.3）里 1→12 还有 4.4×，端到端只剩 1.8×，差距就是「剩下那段串行链」。
2. **内存几乎零代价**：MaxRSS 增量 0.4–1.1 MB（帧级线程只持有输入帧引用 + 每线程上下文，不复制帧数据）。
3. ⚠️ **不可逐字节复现**：同一配置连跑两次输出大小就不同（`tc=1`: 17,593,280 vs 17,595,310 B；`tc=4`: 17,585,415 vs 17,595,248 B；`tc=2`: 17,597,117 B；`tc=12`: 17,593,849 B，全部 ±0.06% 以内）。原因候选：`rc=bitrate` 的码控反馈顺序 + 13 线程滤镜图/swscale 的分片边界。**所以「输出字节相同」不能当回归门禁**（§6）。[A] 的固定量化器探针（不设 bit_rate）则是逐字节相同的，说明这是码控/线程调度的现象，不是编码器缺陷。

4 帧样本里含 ~0.4 s 固定 setup（首帧解码等），因此**编码段的实际比例收益比 wall 比值更大**。

---

## 4. 逐项优化点

### P0-1 给 prores_ks 设线程数（最高优先级）

**现状**（`transcode.cpp:1417-1424` 只设 profile，`:1450` 直接 `avcodec_open2`）：
```cpp
} else if (is_prores) {
    const char *p = "hq"; // 422 HQ
    ...
    av_opt_set(enc->priv_data, "profile", p, 0);
}
ret = avcodec_open2(enc, ecodec, &enc_opts);   // thread_count 仍是默认 1
```
**建议改法（示例，未改仓库）**：
```cpp
// 仅软件编码器路径(is_prores / libx264 / libx265 / vpx)，不要碰 is_mc(MediaCodec)
enc->thread_count = 2;                      // 实测拐点=2(1.78×)，2~4 均可；0 = auto(≤16) 不建议
enc->thread_type  = FF_THREAD_FRAME;        // prores_ks caps=0x103000 支持 FRAME|SLICE
// slices 保持 0：切片线程实测几乎无收益(1 线程: 478.6 → 490.6 ms)
```
**为什么 1 不是 0**：FFmpeg 的 `thread_count` 选项默认值是 **1**（`options_table.h:220`，仓库自带 arm64/armv7 头文件同址可查），所以「不设置」= 单线程；`0` 才是 auto（`pthread_frame.c:863`：`FFMIN(nb_cpus+1, MAX_AUTO_THREADS=16)`）。
**实测（§3.6，端到端，真实 4 帧 zip）**：1→2 线程 **1.78×**（23.74 → 13.34 s），1→4 为 1.61–1.94×，1→12 为 1.83× —— **2 线程即拐点，再加线程只是空转**（12.9–13.3 s 平台说明剩余 ~13 s 的关键路径已经不是编码器）。编码器隔离口径（[A]，合成帧、不设 bit_rate）：1→4 ≈2.97×、1→12 ≈4.38×，输出逐字节相同。
**内存代价（实测，比原先估计小得多）**：帧级线程**不复制帧数据**，只多出 N-1 份编码器上下文与在飞引用。端到端 MaxRSS：778.6 MB(1) / 778.9(2) / 779.0(4) / 779.7(12) → **增量 0.4–1.1 MB**。[A] 里 auto(12 线程) 曾见 ~92 MB 增量，那是「20 帧连续编码、输出包全压在内存里」的口径，不代表本流水线。结论：内存预算不再是限制线程数的理由，**拐点在 2 线程**，建议 `thread_count = 2~4`（给真机大小核调度留余量），仍**不建议 `0=auto`**（8 核机会开 9 个编码线程，与 13 线程滤镜图抢核）。
**风险/回归**：帧级线程会让编码器产生 `thread_count-1` 帧的延迟（批量转码无影响；`preview.cpp` 走的是另一条路，不受影响）。⚠️ **本流水线在 `rc=bitrate` 下输出不可逐字节复现**（§3.6：同配置两次运行差 0.01–0.06%），所以**不要用「字节相同 / framemd5 相同」当门禁**；请用 PSNR/SSIM（或抽帧目视）+ 输出大小容差（±0.5%）+ 帧数/时长一致。
**验证方法**：端到端 A/B 已在 §3.6 完成（复现命令见 §7）；剩下的是**真机复测** —— Release 包跑一次 60 帧，记录 wall 与 `VmHWM`，确认 2–4 线程在大小核调度下的收益与内存（不要用 framemd5 当门禁，见 §6）。

### P0-2 用 Release 构建 native 代码

**现状**：所有 native 构建产物都是 Debug（§3.5），即 app 自己的 C++（`dngzip.cpp` 的 4→3 循环、`transcode.cpp` 的逐行 memcpy、`dngframeq.h`）以 `-O0` 运行；LibRaw/FFmpeg 是预编译 `-O3` 静态库，不受影响。
**改法**：用 `assembleRelease` 出包，或在 `app/build.gradle.kts` 的 `externalNativeBuild.cmake { arguments += "-DCMAKE_BUILD_TYPE=Release" }` 固定优化级别（AGP 默认会给非 debuggable 变体传 Release；请在真机确认 `.cxx/Release` 出现）。
**预期**：解码路径每帧省 ~30 ms（主机口径的 4→3 循环），ARM 上 `-O3` 还能自动向量化，收益更大；此收益在 P0-1 修好、解码重新成为瓶颈后更重要。
**注意**：以后所有性能数字都必须在 Release 包上取；历史「设备侧」数字可能全部来自 Debug 包。

### P1-3 提高 DNG 队列 depth（让 4 个 worker 真的同时在解码）

**现状**：`dngframeq.h:275-278` 的等待条件是 `(m_assigned - m_consumed) < m_depth`，而 depth 固定为 2 → **在飞帧最多 2**，所以 4 个 worker 里只有 2 个在解码。实测吻合：2 线程 235 ms/帧 ≈ 4 线程 239 ms/帧（历史设备侧表 [D]）。
**改法**：把 depth 提到「≈ worker 数」（或 2×worker 数），并按 worker 数×72 MB 重新核算内存。
**预期**：纯 LibRaw 解码 2 线程 212 ms → 4 线程 124 ms（历史数据 [D]），在编码器修好后可直接转化为端到端收益。
**风险**：每加一档 depth 就多一份 72 MB rgb 缓冲，4096×3072 下立刻撞 500 MB 预算 → 建议与「去掉 72 MB 拷贝」(P2-5) 一起做。

### P1-4 每个 worker 复用一个 LibRaw 实例

**现状**：`dngzip.cpp:830` 每帧 `std::make_unique<LibRaw>()`（对象本身 767 KB），`:833 open_buffer` → `:850 unpack` → `:857 dcraw_process` → `:889 resize(75.5 MB)` → `:902 recycle()` 之后就析构。也就是说**每帧都重新申请/释放约 125 MB 内部缓冲**（25 MB CFA + 100 MB image），`recycle()` 省下的复用完全被析构丢掉了。
**改法**：LibRaw 实例随 worker 线程走（线程局部/池化），帧间只 `recycle()` 不析构；既然是副本实验，先量收益再决定是否落地。
**预期**：未实测（本报告不臆造百分比）。量级参考：每帧一次 100 MB+ 的分配/清零与释放（memset 100 MB ≈ 1.5 ms 主机口径，真机显著更贵），外加 mmap/munmap 抖动。

### P2-5 去掉每帧 72 MB 分配 + 逐行 memcpy

**现状**：`transcode.cpp:1865-1870` 每帧 `av_frame_unref()` + `av_frame_get_buffer(dframe,32)`（72 MB 分配并清零），`:1877-1881` 逐行 `memcpy` 72 MB。
**改法**：让 LibRaw 的输出缓冲直接挂在 AVFrame 上（`av_buffer_create()` 自定义释放回调，或 worker 侧维护可复用的缓冲池），图表入口用该缓冲；`av_buffersrc_add_frame_flags(..., KEEP_REF)` 视所有权选择。
**预期**：主机实测 memcpy 4.2–6.6 ms/帧 + 每帧 72 MB 的分配/清零；在编码器修好后约占端到端 10% 量级。

### P2-6 `faststart` 默认值

**现状**：`HomeViewModel.kt:62 val faststart: Boolean = true`（默认开），`FFmpeg.kt:228` 写进 spec，`transcode.cpp:487-564` 在收尾时把 moov 之前的整个区域分段重写（>256 MB 直接放弃）。
**影响**：4K ProRes HQ 的成品大约 5–6 MB/帧 → 900 帧约 5 GB；faststart 相当于对 ~5 GB 做一次「读+写」全量搬运。对「本机/批量交付」场景这笔 I/O 纯属浪费，但对需要流式播放/上传的场景是必要的。
**建议**：把默认值改成关（或按输出大小自动关闭），并在 UI 里保留开关；用 harness 的 0/1 对比实测一次收益。

### P3-7 首帧被解码 3 次

`transcode.cpp:1227-1235`（按实际档位整帧试解一次）+ worker `seq=0` 再解一次 + `native-lib.cpp:319-335` 的 `nativeInfo` 再解一次 ≈ 0.4 s/任务（[C] setup 0.42 s 基本就是它）。900 帧任务里可忽略，但修起来便宜。

### P3-8 构建与其它微调（低优先级）

- `scripts/build-ffmpeg-android.sh:429-461` 用了 `--enable-small`（体积优先于速度），未开 LTO；`scripts/build-ffmpeg-android.sh:566` 关闭 OpenMP 并 `rm -f libraw_r.a`（`:581-583`）→ LibRaw 无法帧内并行。这些都是「要不要为速度重编一次预编译库」的取舍，收益未实测。
- 线程亲和/优先级（把解码 worker 与大核绑定）未测；在大小核手机上可能有价值。
- 仓库里有一个编辑器临时文件 `app/src/main/cpp/transcode.cpp~RF55b9c91.TMP`，建议清理（本次未动）。

---

## 5. 对既有文档 `docs/PERF-RAW-TO-PRORES-2026-10-06.md` 的修正

| 既有结论 | 复核结果 |
|---|---|
| P1-01 app native `-O0` | **成立**，本报告升级为 P0-2（有 `-O0` vs `-O2` 实测 36.1 ms vs 5.6 ms）。 |
| P1-02「prores_ks 没有启用多线程」 | **成立，而且是头号问题**（本报告 P0-1）。根因写清楚：`thread_count` 默认值 1，不是 0=auto。**已 A/B 实测：1→2 线程端到端 1.78×，1→4 为 1.61–1.94×，内存增量 ≤1.1 MB（§3.6）。** |
| P1-03「滤镜图 `nb_threads` 从未设置 → 默认 0 = 单线程」 | **不成立**。0 是「自动」：`slicethread.c:107-113` → `min(ncpus+1,16)`，`vf_scale.c:353-357` 又转发给 swscale；实测 swscale auto 23.6 ms/帧 vs 单线程 108.0 ms/帧，harness 实测滤镜链 ~25–56 ms/帧，A/B harness 里 app 自己代码打印 `graph nb_threads=13 thread_type=1(SLICE)`。**不要在这里花工时。** |
| P2-01 depth=2 硬顶并行度 | **成立**，但只有在编码器修好后才会成为主瓶颈（本报告 P1-3）。 |
| P2-02 每帧 72 MB 分配+拷贝 | **成立**（本报告 P2-5）。 |
| P2-03 inflate 未单独计时 | 参考价值不大：`read_frame` 实测 0.003–0.016 s/帧，zip 解压从来不是瓶颈。 |

---

## 6. 验收与回归

- **契约/验收口径不变**（`docs/INTERFACES.md` §10.7）：1080p60 900 帧 < 20 min；4K 900 帧 < 90 min；峰值内存 ≤400 MB(1080p) / ≤500 MB(4K)。
- 每次改动都要跑：
  1. 输出等价性：**该流水线不可逐字节复现**（§3.6：同配置两次运行就差 0.01–0.06%）→ 用 PSNR/SSIM ≥ 阈值 + 输出大小容差 ±0.5% + 帧数/时长一致；`framemd5` 只适合「同一次构建内的确定性对照」（例如只改滤镜参数）。
  2. 性能：同源 harness（`/tmp` 副本，`-O2`）跑 60 帧子集，记录每帧 ms 与 maxrss；真机再跑一次并记录 `VmHWM`。
  3. 内存：编码器线程数几乎不增内存（§3.6 实测 +≤1.1 MB），预算主要看「解码 worker/depth × 72 MB 帧缓冲」。
- 建议的参数组合（真机再调）：编码器 `thread_count=2~4`（实测拐点=2）、解码 worker 3~4 且 depth≈worker 数、滤镜图保持 auto（若发现线程总数过多，可把 `vp.g->nb_threads` 限定为 4 以给编码器让核）。

---

## 7. 复现命令与证据索引

主机侧（全部在 WSL `/tmp`，仓库只读；`/mnt/zshare/ProResForge` = 工作区共享）：

```bash
# 同源 harness（应用同款代码，host FFmpeg 6.1 + LibRaw 0.21.4 静态库）
g++ -std=c++17 -O2 -pthread \
  -I/tmp/prf-bench/src -I/home/ubuntu/ffx86/inst/include -I/home/ubuntu/dzwork/libraw_host/include \
  src/transcode.cpp src/dngzip.cpp src/transcode_harness.cpp -o bin/tcharness \
  -Wl,--start-group /home/ubuntu/ffx86/inst/lib/libav{format,codec,filter}.a \
     /home/ubuntu/ffx86/inst/lib/libsw{scale,resample}.a /home/ubuntu/ffx86/inst/lib/libavutil.a \
  -Wl,--end-group /home/ubuntu/dzwork/libraw_host/lib/libraw.a -lz -lm -lpthread -ldl
```

端到端 A/B（§3.6 的实测来源；仓库只读，全部在 `/tmp`）：

```bash
# 1) 拷贝源码到 /tmp（仓库路径在 WSL 下是 /mnt/zshare/ProResForge）
mkdir -p /tmp/prfplus/base /tmp/prfplus/thr
cp /mnt/zshare/ProResForge/app/src/main/cpp/{transcode.cpp,dngzip.cpp,dngframeq.h,dngzip.h,transcode.h,strspec.h,preview.h} /tmp/prfplus/base/
cp /mnt/zshare/ProResForge/scripts/transcode_harness.cpp /tmp/prfplus/base/
cp /tmp/prfplus/base/* /tmp/prfplus/thr/
# 2) 唯一改动：在 thr/transcode.cpp 的 `ret = avcodec_open2(enc, ecodec, &enc_opts);`(约 :1450) 之前插入
#      enc->thread_count = N;  enc->thread_type = FF_THREAD_FRAME;
#    如需诊断日志，再在 avcodec_open2 之后 / avfilter_graph_config(约 :1700) 之后加一行 av_log 打印
#      enc->thread_count / active_thread_type / thread_type ；vp.g->nb_threads / thread_type
# 3) 编译（g++ 命令见上，把 -I 与源文件换成 /tmp/prfplus/<变体>）
# 4) 跑 A/B
cp /mnt/zshare/ProResForge/tmp/dngzip-test-4f.zip /tmp/in4.zip
/usr/bin/time -v /tmp/prfplus/base/tcharness /tmp/in4.zip /tmp/out_base.mov src=dngzip 2>&1 | grep -E "PRFDIAG|Elapsed|Maximum resident"
/usr/bin/time -v /tmp/prfplus/thr/tcharness  /tmp/in4.zip /tmp/out_thr.mov  src=dngzip 2>&1 | grep -E "PRFDIAG|Elapsed|Maximum resident"
# 本次实测：1 线程 23.74/24.03 s、778.6–778.8 MB；2 线程 13.34 s；4 线程 12.25/14.96 s；12 线程 12.94 s；MaxRSS 差 ≤1.1 MB
```

关键源码/证据位置：

| 事项 | 位置 |
|---|---|
| 编码器 `thread_count` 默认 1 | `app/src/main/cpp/prebuilt/{arm64-v8a,armeabi-v7a}/include/libavcodec/options_table.h:220`（`:221` auto=0，`:366-368` thread_type 默认 FRAME\|SLICE） |
| app 未设编码器线程 | `app/src/main/cpp/transcode.cpp:1417-1424`（prores 分支）、`:1450`（open2）；对比 `:1287-1288`（解码器）；`app/src/main/cpp/preview.cpp:260-261` |
| 每帧 72 MB 分配+拷贝 | `app/src/main/cpp/transcode.cpp:1865-1870`、`:1877-1881`、`:1882 push_decoded` |
| 每帧重建 LibRaw / 4→3 循环 | `app/src/main/cpp/dngzip.cpp:830`、`:889-898`、`:902` |
| 队列 depth 谓词 | `app/src/main/cpp/dngframeq.h:275-278`、`:284`、`:290-293` |
| 建图处（未设 nb_threads） | `app/src/main/cpp/transcode.cpp:1647`、`:1665-1693` |
| faststart 默认开 | `app/src/main/java/com/proresforge/app/ui/HomeViewModel.kt:62`、`engine/FFmpeg.kt:228`、`transcode.cpp:487-564` |
| native 构建为 Debug | `app/.cxx/Debug/*/*/CMakeCache.txt:49,58,61,67`；`app/build/outputs/apk/debug/app-debug.apk` |
| 上游线程实现 | FFmpeg 6.1 `libavfilter/pthread.c:83-108`、`libavutil/slicethread.c:27,107-113`、`libavfilter/avfilter.c:786-791`、`libavfilter/vf_scale.c:283-288,353-357`、`libswscale/options.c:82-83`、`libavcodec/pthread_frame.c:863` |

---

## 8. 仍未验证（不要当成结论引用）

1. 真机（arm64，大核更少）上编码器线程数的拐点位置与内存增量 —— 主机（x86_64 / 12 核 / WSL2）实测拐点 = 2 线程、端到端 1.78×、MaxRSS +≤1.1 MB；真机核数更少、内存口径不同，需在 Release 包上复测（但「`thread_count` 默认 1 = 单线程」这一事实与加速比量级可移植）。
2. LibRaw 实例复用、零拷贝 AVFrame、depth 提高各自在**端到端**口径下的收益（本次都是结构分析 + 局部实测）。
3. `faststart=0` 对 4K/900 帧成品的确切节省（一次 ~5 GB 的读+写）。
4. 去掉 `--enable-small` / 开 LTO 对编码速度的影响。
5. 设备侧基线：本报告的结构性结论可移植，但「当前真机每帧多少毫秒」仍需在 Release 包上重测一次（历史数字可能来自 Debug 包，见 P0-2）。
6. 编码器修好后剩下的 ~13 s/4 帧（≈3.2 s/帧）里，解码 / 72 MB 拷贝 / 滤镜 / 编码各占多少 —— 需要重新插桩 profile（本次未做）。这决定 P1-3（depth）、P2-5（零拷贝）、P0-2（-O3）的优先顺序。
