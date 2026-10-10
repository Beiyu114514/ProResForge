# ProResForge 性能优化执行后只读审查报告

审查日期：2026-10-06  
审查范围：性能优化方案、实施报告、当前 native 实现、LibRaw/FFmpeg 构建脚本、Gradle/CMake 配置、契约与验证记录。  
审查约束：本轮只读；未修改源码、构建配置或既有文档，未运行构建、测试、脚本或真机验证。  
审查基线：以当前工作区文件为准；历史报告中的实验结果只作为证据记录，不视为本轮重新验证。

## 结论

P0 的 Release 配置和 `prores_ks` 帧级线程、P1 的 LibRaw 复用、P2 的首帧 seed 以及队列安全修复均已在当前源码中落地，未从静态代码中发现新的明显帧序破坏、槽位越界或异常路径资源释放缺陷。

但当前版本仍不宜把“最终门禁全部通过”当作可发布结论，主要原因是：

1. **线程安全 LibRaw 的构建门禁可在脏缓存上跳过重建，导致旧的 `LIBRAW_NOTHREADS` 库重新进入产物。** 这是直接影响多 worker 解码正确性的发布风险。
2. **AVFrame 池仍默认开启，但现有 host 证据显示墙钟慢 1.4–1.7 倍、RSS 增加约 220 MB，真机判定协议尚未执行。** 4K 设备上存在性能回退和 OOM 风险。
3. **faststart 的失败结果被调用方忽略；在搬移过程中发生 I/O 错误时没有回滚，可能把一个原本可播放的 MOV 留成部分搬移状态，同时任务仍继续成功收尾。**
4. **Release 优化配置不是硬保证：`-O0` 也会匹配 `MATCHES "-O"`，外部 cache 注入 `-O0` 时可能跳过 `-O3`。**
5. **当前文档对 ProRes 输出确定性和 P2 完成状态存在互相矛盾的口径，后续门禁容易被错误的 hash/容差规则误判。**
6. **本轮没有独立证明 arm64 Release 真机的 4K/900 帧、内存、温控、色彩和取消目标。** host harness 和 Gradle 记录不能替代设备验收。

## Findings

### P1-01：LibRaw raw_r 重建门禁可被旧产物绕过

位置：

- [scripts/build-ffmpeg-android.sh](../scripts/build-ffmpeg-android.sh:552-615)
- [app/src/main/cpp/prebuilt/BUILDINFO](../app/src/main/cpp/prebuilt/BUILDINFO:19-26)
- [app/src/main/cpp/CMakeLists.txt](../app/src/main/cpp/CMakeLists.txt:91-110)

脚本只用下面的存在性条件决定是否执行 LibRaw 构建：

```bash
if [[ ! -f "${prefix}/lib/libraw.a" ]]; then
    # configure/build raw_r/install
fi
```

当 prefix 中已经存在旧版 `libraw.a` 时，整个 `raw_r` 构建段直接跳过。后续只检查文件存在，不检查它是否来自 `raw_r`、是否没有 `LIBRAW_NOTHREADS`、是否匹配当前 `BUILDINFO` 或当前源码/脚本 hash。CMake 同样只检查 `libraw.a` 和头文件存在，无法补上这个 provenance 缺口。

这与当前运行时默认的多 worker 解码直接相关：旧单线程版的进程级 static 缓存会导致并发解码结果不稳定。`BUILDINFO` 还明确记录本次是“手工替换、未重跑整个脚本”，说明当前产物与后续完整构建路径并非同一证据链。

影响：在已有 prefix 的开发机、CI 缓存或发布机上，重新执行脚本可能看似成功却继续使用旧库；最终 APK 的正确性和可复现性依赖人工清理缓存。

建议：把 LibRaw 线程模型纳入机器可验证的构建门禁，至少采用以下一种方案：

- 为 `raw_r` 产物和安装产物生成并比较 SHA-256，同时把源码 pin、脚本 SHA、线程模型写入 manifest；
- 当 manifest 不匹配或缺失时强制删除并重建 `libraw.a`，不能只看文件存在；
- 在构建收尾用 `llvm-nm`/符号探针检查旧版 `LIBRAW_NOTHREADS` 产生的 static 符号差集为零；
- Gradle/CMake 侧至少校验 manifest，而不是只校验静态库文件存在。

### P1-02：AVFrame 池默认开启，但当前证据不支持默认值为 1

位置：

- [app/src/main/cpp/transcode.cpp](../app/src/main/cpp/transcode.cpp:181-213)
- [app/src/main/cpp/transcode.cpp](../app/src/main/cpp/transcode.cpp:250-276)
- [app/src/main/cpp/transcode.cpp](../app/src/main/cpp/transcode.cpp:1997-2066)
- [docs/PERF-OPTIMIZATION-IMPLEMENTATION-2026-10-06.md](../docs/PERF-OPTIMIZATION-IMPLEMENTATION-2026-10-06.md:85-91)
- [docs/PERF-OPTIMIZATION-IMPLEMENTATION-2026-10-06.md](../docs/PERF-OPTIMIZATION-IMPLEMENTATION-2026-10-06.md:137-144)

当前默认宏为：

```cpp
#ifndef PRF_AVFRAME_POOL
#define PRF_AVFRAME_POOL 1
#endif
```

实施报告记录的 host A/B 结果是池化墙钟慢约 1.6–1.7 倍、RSS 增加约 220 MB；报告的真机判定标准则是池化必须至少快 10%，且 `VmHWM` 不高于 nopool 才能保留默认 1。当前记录没有 arm64 真机结果可以满足这个条件。

池化的引用计数保护本身是合理的：只有 `av_buffer_get_ref_count(...) == 1` 时原地复用，否则重新分配。但这只能证明内存生命周期路径有防护，不能证明默认策略适合设备。当前默认值会让未完成真机验证的 4K 用户直接承担较高常驻内存和 host 已观测到的吞吐回退。

建议：在真机 A/B 完成前将默认值改为 0，或把默认值保留为 1 但阻止其进入发布变体并明确标记为实验开关。无论选择哪种方式，都应把 `PRF_AVFRAME_POOL=1/0` 的 arm64 Release 结果、4K `VmHWM`、温度、墙钟和取消/失败 soak 作为发布门禁。

### P1-03：faststart 失败被静默忽略，部分搬移可能破坏输出

位置：

- [app/src/main/cpp/transcode.cpp](../app/src/main/cpp/transcode.cpp:616-693)
- [app/src/main/cpp/transcode.cpp](../app/src/main/cpp/transcode.cpp:2380-2385)
- [app/src/main/cpp/transcode.cpp](../app/src/main/cpp/transcode.cpp:2444-2448)

`faststart_move_moov()` 返回 `bool`，会在扫描、读取或写入失败时返回 `false`。但正常收尾和错误收尾都以 `faststart_move_moov(&out_ctx); // best effort` 调用并丢弃返回值。

函数在 `:683-689` 处会先逐块右移 A 区，再写入 moov；如果中途某次 `pwrite_exact()` 失败，之前的块已经完成搬移，没有回滚机制。调用方随后无法区分“faststart 未完成”与“faststart 已完成”，也没有日志、结果码或最终 box 结构校验。这样一个原本已写完 trailer 的文件，在存储空间不足、I/O 错误或设备异常时可能被留在部分搬移状态，任务仍可能被上层当作成功。

建议：

- 把 faststart 视为可观测的收尾阶段，失败至少写 `AV_LOG_WARNING` 并把最终状态标为 warning/failed，不能静默吞掉；
- 更稳妥的实现是写入临时文件或先复制到安全区域，验证 `[ftyp][moov]` 结构后再原子替换；
- 若产品坚持 best-effort，就必须明确契约为“可能不前置 moov”，并在失败后验证文件仍可播放且不发生部分搬移。

### P2-01：Release 优化条件会把 `-O0` 当成“已经有优化级别”

位置：[app/src/main/cpp/CMakeLists.txt](../app/src/main/cpp/CMakeLists.txt:19-28)

当前逻辑是：

```cmake
if(CMAKE_CXX_FLAGS_RELEASE MATCHES "-O")
  # 已带优化,不重复追加
else()
  set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
endif()
```

正则 `-O` 同时匹配 `-O0`、`-O1`、`-O2` 和 `-O3`。因此只要外部 toolchain、Gradle cache 或命令行把 `CMAKE_CXX_FLAGS_RELEASE` 设成 `-O0`，本段就不会强制 Release 优化。当前实施报告中记录的某次 Release compile command 不能证明所有干净机、缓存机和外部配置都满足该保证。

建议：用明确的优化级别匹配或 target 级编译选项验证/覆盖 `-O0`，并把 `compile_commands.json` 检查设为构建门禁。至少应让 `-O0` 不满足“已带优化”的判断。

### P2-02：构建产物 provenance 与当前源码/脚本状态仍可能漂移

位置：

- [app/src/main/cpp/prebuilt/BUILDINFO](../app/src/main/cpp/prebuilt/BUILDINFO:1-26)
- [app/src/main/cpp/prebuilt/README.md](../app/src/main/cpp/prebuilt/README.md:64-77)
- [scripts/build-ffmpeg-android.sh](../scripts/build-ffmpeg-android.sh:552-615)

`BUILDINFO` 写明 LibRaw 是手工替换、未重跑整个脚本；同时构建脚本当前已改为 `raw_r` 路径。只要 BUILDINFO 没有在每次产物变更后自动刷新，发布者无法仅凭它判断“所有 ABI 的库是否由当前脚本和当前 pin 生成”。这与 P1-01 是同一供应链问题的证据层表现，但即使修复 skip，也需要把 manifest 作为 APK 发布前的可核对输入。

建议：将 ABI、LibRaw target、源码 commit、脚本 SHA、NDK/API、每个静态库 SHA-256 和生成时间作为机器生成 manifest；构建时校验 manifest 与实际文件，手工替换不应被当作最终发布路径。

### P2-03：输出确定性门禁在文档间互相矛盾

位置：

- [docs/INTERFACES.md](../docs/INTERFACES.md:553-606)
- [docs/INTERFACES.md](../docs/INTERFACES.md:623-628)
- [docs/PERF-OPTIMIZATION-IMPLEMENTATION-2026-10-06.md](../docs/PERF-OPTIMIZATION-IMPLEMENTATION-2026-10-06.md:23-30)
- [docs/PERF-OPTIMIZATION-IMPLEMENTATION-2026-10-06.md](../docs/PERF-OPTIMIZATION-IMPLEMENTATION-2026-10-06.md:117-124)
- [docs/PERF-RAW-TO-PRORES-VERIFIED-2026-10-06.md](../docs/PERF-RAW-TO-PRORES-VERIFIED-2026-10-06.md:124-127)

`INTERFACES.md` 和旧性能复核报告仍记录“同配置每轮输出字节不同、采用 ±0.5%/PSNR 容差”；实施报告又称切换线程安全 LibRaw 后同二进制同输入 SHA-256 稳定，并把精确 SHA 作为最终门禁。两套结论可能分别对应旧单线程 LibRaw 和新 raw_r 产物，但文档没有清楚标出版本边界，且实施报告第 1 节仍把 P2 faststart/seed 标为“待 D 段数据”，与后文已完成的结果矛盾。

影响：后续维护者可能用旧容差掩盖新库的非确定性，也可能拿旧库和新库的 hash 直接比较；“最终门禁全通过”的可审计性不足。

建议：把证据分成“旧库历史基线”和“raw_r 后当前基线”，明确适用的 ABI、构建宏、LibRaw SHA 和码控参数；对 raw_r 后稳定性再做一次可复现的独立确认。同步修正实施报告状态表，并在契约中明确：哪些配置允许精确 hash，哪些配置只能用帧数/PTS/PSNR/大小容差。

### P2-04：arm64 Release 真机验收证据缺失

位置：

- [docs/PERF-OPTIMIZATION-IMPLEMENTATION-2026-10-06.md](../docs/PERF-OPTIMIZATION-IMPLEMENTATION-2026-10-06.md:137-144)
- [docs/INTERFACES.md](../docs/INTERFACES.md:727-733)

当前实施记录主要是 WSL x86_64 host、host harness、静态符号检查和 Gradle 产物记录。真机协议仍要求 arm64 Release、60 帧以上 interleaved A/B、`VmHWM`、温度/负载、LibRaw 并发画面检查和长期目标验证。当前没有本轮独立证据证明：

- `PRF_PRORES_THREADS=2/3/4` 在大小核调度下仍有收益；
- `PRF_AVFRAME_POOL=1/0` 满足 10% 且不增内存的门槛；
- raw_r、seed、池化组合在 arm64 上没有坏块、色偏或长跑 OOM；
- 4K/900 帧、1080p/900 帧、取消和温控目标满足契约；
- PSNR/灰卡/白卡与桌面参考达到 40 dB。

因此“host 正确且构建产物存在”只能作为合并前门禁，不能作为稳定发布验收。

## 已确认的正面项

- `DngFrameQueue::thread_count()` 当前同时统计 `m_workers` 和 POSIX `m_threads`，符合历史死锁修复要求。
- `DngFrameQueue::start()` 当前对 `pthread_attr_init`、栈大小和 detach state 做失败处理，栈属性失败会返回 false；`next()` 有 `m_started` 守卫和 15 秒诊断等待。
- seed 的落地顺序、`seq=0`、`m_assigned=1` 和 `total_frames==1` 处理与契约说明一致，未静态发现首帧重复交付或单帧死等路径。
- LibRaw reuse 使用 thread-local 实例、构造默认参数快照，并以“失败丢弃、成功保留”守卫覆盖异常出口；当前实现比单纯调用 `recycle()` 更能避免参数跨帧污染。
- ProRes 线程设置只在 `is_prores` 分支生效，没有套用到 MediaCodec 路径；Release CMake 参数也没有引入 `-ffast-math`。

## 本轮未重新验证的既有发布阻断

以下问题来自既有审查记录，本轮未修改、未重新执行测试，也没有把它们重复升级为本轮性能代码 finding：

- Kotlin 文件探测批次竞态、取消状态被成功收尾覆盖、`buildSpec()` 异常降级继续转码；
- 音频 FIFO/BSF/解码器/编码器 flush 的错误传播；
- DNG 预览固定 FAST/CAMERA 与实际转码参数不一致；
- 4K 内存口径、备份恢复、通知 ID、契约文档其他漂移；
- ZIP64 异常边界、日志开销和构建依赖的其他卫生问题。

这些条目见 [docs/REVIEW-REPORT-2026-10-06.md](../docs/REVIEW-REPORT-2026-10-06.md) 和 [docs/AUDIT-2026-10-06.md](../docs/AUDIT-2026-10-06.md)，仍应视为整体发布评估的一部分。

## 建议的发布前顺序

1. 先修复 raw_r 的 manifest/强制重建门禁，并对两个 ABI 做静态符号和 SHA 校验。
2. 在 arm64 Release 上完成 ProRes 线程、AVFrame 池和 seed 的交错 A/B；在真机不满足池化门槛前不要默认开启池化。
3. 让 faststart 失败可观测且不可留下部分搬移文件。
4. 收紧 Release 优化配置判断，门禁检查 compile commands 中没有 `-O0`。
5. 统一 raw_r 后的输出确定性和容差口径，更新实施报告状态表。
6. 最后执行 900 帧、内存、温控、取消、色彩和预览/普通容器回归，再决定是否可发布。

## 审查结论

本轮没有对源码提出“立即回滚所有优化”的结论：队列、LibRaw 参数复位、seed 和编码器线程的静态实现方向可以保留。当前不通过的是发布证据链和两个默认策略：**raw_r provenance 必须从人工约定变成构建门禁，AVFrame 池必须先经过真机正向证据再作为默认优化。**
