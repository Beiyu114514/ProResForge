# ProResForge 性能优化执行后修复方案

版本：2026-10-06  
状态：待执行  
依据：[PERF-OPTIMIZATION-AUDIT-2026-10-06.md](PERF-OPTIMIZATION-AUDIT-2026-10-06.md)  
适用范围：DNG ZIP → LibRaw → filtergraph → `prores_ks`/MOV 性能优化链路及其发布门禁

> 本文是修复方案，不是执行记录。本轮只生成方案文档，不修改源码、构建配置或既有报告，不运行构建、测试、脚本或真机验证。

## 1. 目标与原则

### 1.1 目标

1. 确保 Android 两个 ABI 使用经过机器校验的线程安全 LibRaw `raw_r` 产物，杜绝旧 `LIBRAW_NOTHREADS` 库被缓存复用。
2. 在没有 arm64 真机正向证据前，避免把 host 已显示负收益的 AVFrame 池作为发布默认路径。
3. 使 faststart 失败可观测、可判定，并避免原地搬移失败留下损坏或半搬移的最终文件。
4. 让 Release native 优化级别成为可验证的构建门禁，不允许 `-O0` 混入 Release 产物。
5. 统一实施报告、接口契约和历史性能报告中的输出确定性、池化状态及 P2 完成状态。
6. 补齐 arm64 Release 的性能、内存、温控、正确性和长跑验收，之后再决定是否发布。

### 1.2 原则

- 先修复发布正确性和产物 provenance，再做性能取舍。
- 每次只改变一个性能变量，使用 interleaved round-robin，禁止用单次 host 数字决定默认值。
- host 结果用于判断结构性趋势，不直接替代 ARM 真机结果。
- 旧单线程 LibRaw 产物不得与 `raw_r` 产物进行 hash 或像素结果对比。
- 所有“通过”必须绑定 ABI、BuildType、编译宏、LibRaw SHA、输入素材和验证命令。
- 不实施 worker 直写 AVFrame 的零拷贝第二步，直到第一步在真机上满足正向门槛并完成生命周期验证。

## 2. 当前问题与优先级

| 优先级 | 问题 | 主要位置 | 发布影响 | 处理策略 |
|---|---|---|---|---|
| P0 | `libraw.a` 存在性检查可跳过 `raw_r` 重建 | `scripts/build-ffmpeg-android.sh:552-615` | 旧库可能重新进入 APK，造成并发解码结果不可靠 | 强制 provenance 校验；不满足即重建并阻断发布 |
| P0 | faststart 失败后可能留下部分搬移文件且仍成功 | `transcode.cpp:616-693,2380-2385,2444-2448` | 可能损坏用户最终 MOV | 先改为失败可观测；最终采用临时文件/可清理 staging |
| P1 | AVFrame 池默认开启，但 host 慢 1.4–1.7 倍且 RSS 增约 220 MB | `transcode.cpp:211-213` | 4K 性能回退或 OOM | 真机证据前默认关闭，保留 A/B 开关 |
| P1 | Release 判断将 `-O0` 视为已有优化 | `CMakeLists.txt:19-28` | 性能包可能实际以 `-O0` 编译 | 修正匹配并增加 compile command 门禁 |
| P1 | arm64 Release 验收证据缺失 | 实施报告 §6、`INTERFACES.md §10.8` | 不能证明设备目标满足 | 完成设备矩阵和 900 帧长跑 |
| P2 | 输出 hash/容差和 P2 状态在文档中冲突 | `INTERFACES.md`、两份性能报告 | 回归门禁可能误判 | 分层记录旧库与 `raw_r` 证据，统一现行规则 |

本方案不重复修复既有整体审查中已列出的 Kotlin 状态机、音频错误传播、预览参数不一致、ZIP64 边界和通知等问题；这些问题仍是独立发布阻断，见 [REVIEW-REPORT-2026-10-06.md](REVIEW-REPORT-2026-10-06.md) 与 [AUDIT-2026-10-06.md](AUDIT-2026-10-06.md)。

## 3. 实施分段

### 3.1 A 段：LibRaw provenance 与强制重建门禁

**目标**：只要当前产物不是明确由 `raw_r`、当前源码 pin、当前脚本和当前 NDK 生成，就不能走 `[skip]` 路径。

**涉及文件**：

- `scripts/build-ffmpeg-android.sh`
- `app/src/main/cpp/prebuilt/BUILDINFO`
- `app/src/main/cpp/prebuilt/README.md`
- 必要时新增 `scripts/verify-prebuilt.sh` 或等价校验脚本
- `app/src/main/cpp/CMakeLists.txt`、`app/build.gradle.kts` 只承担消费前检查，不承担编译逻辑

**实施步骤**：

1. 为每个 ABI 生成 LibRaw manifest，至少包含：
   - ABI、API level、NDK revision；
   - LibRaw 源码 tag/commit；
   - LibRaw-cmake SHA；
   - `raw_r` target 和 `LIBRAW_NOTHREADS=absent` 声明；
   - 构建脚本 SHA；
   - `libraw.a` SHA-256；
   - 生成时间和构建机信息。
2. 把当前 `if [[ ! -f "${prefix}/lib/libraw.a" ]]` 改成“产物和 manifest 同时有效才允许跳过”。以下任一条件成立时必须删除旧库并重建：
   - `libraw.a` 缺失；
   - manifest 缺失、格式错误或字段不匹配；
   - manifest 中记录的 SHA 与实际 SHA 不同；
   - 脚本 SHA、源码 pin、LibRaw-cmake SHA、NDK/API 或 ABI 改变；
   - 线程安全特征检查失败。
3. 重建前清理对应 ABI 的 `raw_build`，避免旧的 `raw`/`raw_r` object 被误复用；只构建 `raw_r`，再复制为应用契约要求的 `libraw.a`。
4. 安装后立即执行校验：
   - 安装库 SHA 等于 `raw_r` 产物 SHA；
   - 两个 ABI 都存在 `libraw.a` 和 `include/libraw/libraw.h`；
   - `llvm-nm`/归档成员探针确认旧版 `LIBRAW_NOTHREADS` 静态符号差集为零；
   - manifest 与实际文件重新计算后完全一致。
5. `BUILDINFO` 不再允许手工替换作为最终发布路径。若必须临时手工替换，manifest 标记为 `manual=true`，Gradle 发布门禁直接拒绝。
6. `prebuilt/README.md` 明确：已有 `libraw.a` 不能证明线程安全；必须通过 manifest 和符号检查。
7. Gradle/CMake 在消费 prebuilt 时检查 manifest 存在和 ABI 条目完整。检查失败应在构建开始处终止，而不是等链接或运行时失败。

**验收条件**：

- 用旧单线程库覆盖 prefix 后重新执行脚本，必须不能成功 skip，必须重建或明确失败。
- 删除 manifest 但保留新库，必须不能进入发布构建。
- 修改脚本 SHA 或 ABI 条目，必须触发重建/阻断。
- arm64-v8a 和 armeabi-v7a 的 manifest、库 SHA、线程安全符号检查均通过。
- 同一输入下，2T/4T race probe 重复至少 5 次输出 FNV/像素摘要一致。
- `raw_r` 失败时不得回退到旧库继续发布；应阻断构建。

### 3.2 B 段：AVFrame 池默认策略与回滚

**目标**：在真机正向证据出现前，不让已知 host 负收益路径成为默认发布路径；同时保留快速 A/B 和安全回滚能力。

**涉及文件**：

- `app/src/main/cpp/transcode.cpp:181-295,1997-2100`
- `docs/INTERFACES.md §10.7.2`
- `docs/PERF-OPTIMIZATION-IMPLEMENTATION-2026-10-06.md §4.3/§6`

**实施步骤**：

1. 将发布默认值设为 `PRF_AVFRAME_POOL=0`，保留 `#if/#else` 两条实现用于实验构建。若产品决定暂时保留默认 1，必须禁止该配置进入 release 产物，并通过编译 manifest 标记实验状态。
2. 不实施第二步 worker 直写目标 buffer；先保持当前 `refcount == 1` 才复用、否则重新分配的安全判据。
3. 为 A/B 构建记录以下固定条件：ABI、Release 编译参数、ProRes 线程数、DNG worker/depth、输入帧数与分辨率、设备温度和后台负载。
4. 真机使用 60 帧以上 4096×3072 子集，`PRF_AVFRAME_POOL=0/1` 交错运行至少 4 轮；记录 wall time、`VmHWM`、CPU 利用率、温度、失败/取消结果和输出正确性。
5. 对 pool 开启路径增加取消、解码失败、filtergraph flush、输出写入失败的 soak；ASan 只能作为 use-after-free/heap-overflow 证据，不能把 LSan 不可用写成零泄漏。

**保留默认 1 的唯一条件**：池化相对 nopool 在相同设备和相同负载下至少快 10%，且 `VmHWM` 不高于 nopool，4 轮结果方向一致；任一条件不满足，默认保持 0。

**回滚**：只需将发布构建宏设为 0；不得通过修改引用计数判据或绕过 RAII 来“抢回”性能。

### 3.3 C 段：faststart 事务化与失败处理

**目标**：`faststart=1` 要么生成经过结构验证的前置 `moov`，要么明确失败，不能静默成功，也不能把部分搬移文件暴露为最终成品。

**涉及文件**：

- `app/src/main/cpp/transcode.cpp:616-693,2361-2449`
- 输出文件创建和清理相关的 Kotlin/SAF 路径
- `docs/INTERFACES.md` faststart 契约

**推荐实现顺序**：

1. **第一步，立即补齐可观测错误**：调用方检查 `faststart_move_moov()` 返回值；失败时写入 `AV_LOG_WARNING/ERROR`，带 errno、文件长度和阶段信息，并让最终任务状态不能伪装成“faststart 已完成”。
2. **第二步，取消危险的原地部分搬移**：优先把正常 MOV 写入 staging 文件/临时 fd；在 staging 上完成 faststart，再验证顶层 box 顺序为 `[ftyp][moov]` 且长度一致。
3. **第三步，提交最终输出**：验证成功后再复制/提交到 SAF 输出 URI；提交失败时删除不完整的目标文件并返回失败。若 SAF 无原子 rename 能力，必须把“最终文件尚未提交”作为输出层契约，而不是直接改写用户已可见文件。
4. 如果短期无法完成 staging，临时策略必须是：关闭 faststart 默认值或在失败时明确失败并删除输出；禁止继续返回成功。
5. 对 `loop_error` 收尾路径单独覆盖：原始任务已经失败时，faststart 不得覆盖原始错误；faststart 失败也不能触发二次异常或重复写 trailer。

**验收条件**：

- 正常文件 `faststart=1` 的顶层结构为 `[ftyp][moov][wide?][mdat]`，`faststart=0` 结构保持原语义。
- 注入 `pread64/pwrite64` 失败、只写、空间不足和 fd 错误时，任务不报告成功，最终目录中没有可被误认为完整成品的半文件。
- 大于 4 GiB、`largesize`、moov 超过阈值、没有 moov、已有前置 moov 等路径均有明确日志和结果。
- 取消发生在 finalize/faststart 期间时，输出清理和任务状态与原取消契约一致。

### 3.4 D 段：Release 优化级别硬化

**目标**：Release native 产物必须可证明使用 `-O3 -DNDEBUG`，不能被外部 `-O0` 或旧 `.cxx` 缓存污染。

**涉及文件**：

- `app/src/main/cpp/CMakeLists.txt:19-28`
- `app/build.gradle.kts:101-119`
- 新增构建门禁脚本或 Gradle verification task

**实施步骤**：

1. 移除 `CMAKE_CXX_FLAGS_RELEASE MATCHES "-O"` 这种会匹配 `-O0` 的宽松判断。
2. 用明确的优化级别处理：清理/覆盖 Release 配置中的 `-O0`，再显式加入 `-O3 -DNDEBUG`；保留 `-ffast-math` 禁用约束。
3. 让 release variant 继续显式传入 `-DCMAKE_BUILD_TYPE=Release`，并在配置前清理与旧盘符/旧工作目录绑定的 `.cxx` 缓存。
4. 新增 `compile_commands.json` 检查：
   - Release 的四个 native ABI 编译单元均含 `-O3` 和 `-DNDEBUG`；
   - 不含 `-O0`；
   - Debug 不得被误改为 Release 优化；
   - `PRF_PRORES_THREADS` 和 `PRF_AVFRAME_POOL` 的值记录在构建 manifest。
5. 构建失败时不要根据历史 APK 或旧 `.cxx` 输出宣称通过。

**验收条件**：

- 干净 `.cxx` 目录执行 Release configure，所有目标命令满足上述规则。
- 人为注入 `CMAKE_CXX_FLAGS_RELEASE=-O0` 时，门禁必须失败或最终命令被纠正为无 `-O0` 的 `-O3`。
- `assembleDebug` 与 `assembleRelease` 的 native flags 明确分离。

### 3.5 E 段：文档与门禁口径统一

**目标**：让当前报告只保留一套可执行的状态和正确性判定。

**涉及文件**：

- `docs/PERF-OPTIMIZATION-IMPLEMENTATION-2026-10-06.md`
- `docs/INTERFACES.md`
- `docs/PERF-RAW-TO-PRORES-VERIFIED-2026-10-06.md`
- `docs/PERF-OPTIMIZATION-PLAN-2026-10-06.md`

**实施步骤**：

1. 实施报告总览中把 P2 faststart 和 seed 从“待 D 段数据”改为与正文一致的“已完成 host 取证，真机待验收”。
2. 将旧单线程 LibRaw 证据明确标为“历史失败基线”，将 `raw_r` 证据标为“当前候选基线”；两者不能跨版本比较。
3. 统一正确性门禁：
   - 固定 LibRaw pin、ABI、编译宏、固定输入和固定码控参数的 `raw_r` 回归可使用精确 SHA；
   - 一般 bitrate/线程调度组合不得只靠 SHA，必须同时检查帧数、PTS、时长、分辨率、profile、pixfmt、抽帧 PSNR/SSIM 和文件大小容差；
   - 若精确 SHA 失败，报告必须写明是旧库竞态、码控调度还是未分类原因，不能笼统写“不可复现”。
4. `INTERFACES.md §10.7.2/§10.7.3` 增加“当前默认值”和“发布前待验收”标识，避免把 host 实验结果写成设备保证。
5. 所有报告统一记录：输入、ABI、BuildType、LibRaw SHA、宏值、设备/host、重复轮次、失败样本和结论。

### 3.6 F 段：arm64 Release 真机验收

**目标**：用设备证据决定默认值和发布状态。

**准备要求**：

- 使用干净 `.cxx` 和稳定盘符工作目录，避免旧 Y:/P:/UNC 路径缓存；
- arm64 Release APK，构建 manifest 完整；
- 飞行模式或等效低负载，记录设备型号、Android 版本、温度起点、电池状态和可用内存；
- 使用同一份 4096×3072 DNG ZIP 子集，至少 60 帧；另准备 900 帧 4K 与 1080p 样本。

**测试矩阵**：

| 组 | 变量 | 配置 |
|---|---|---|
| A | ProRes 编码线程 | 1 / 2 / 3 / 4，pool=0，seed 开启 |
| B | AVFrame 池 | pool=0 / 1，固定最佳 ProRes 线程 |
| C | seed | seed 开启 / 测试构建关闭，固定 pool=0 |
| D | LibRaw 并发正确性 | worker 1 / 2 / 4，固定输入和参数 |
| E | 长跑 | 1080p 900 帧、4K 900 帧，使用最终候选默认值 |
| F | 失败与取消 | 解码失败、存储失败、取消时机 0/50/200/500ms/晚取消 |

每个 A/B 组使用 interleaved round-robin 至少 4 轮，不能先跑完一组再跑另一组。记录 wall time、P50/P95、VmHWM、CPU、温度/降频、输出帧数、PTS、时长、文件大小、抽帧质量和错误码。

**设备验收门禁**：

- ProRes 线程：选择不超过内存和温控预算、且相对 1T 稳定有收益的档位；默认候选优先 2 或 3，不使用 auto。
- AVFrame 池：仅当比 nopool 至少快 10% 且 VmHWM 不增加时保留 1，否则发布默认 0。
- LibRaw：多 worker race probe、实际画面无坏块/色偏；失败帧错误带帧号且不静默跳帧。
- 内存：1080p ≤400 MB，4K ≤500 MB；任一 900 帧长跑 OOM 或系统杀进程即不通过。
- 性能：1080p60 900 帧 <20 分钟，4K 900 帧 <90 分钟；报告必须注明 P50/均衡档和实际设备。
- 色彩：灰卡/白卡与桌面同参数参考 PSNR ≥40 dB，并检查 Apple Log 的元数据和画面映射。
- 取消：结果状态、输出清理和后续任务均正确，不出现卡死、幽灵 RUNNING 或错误 DONE。
- 普通 `.mov/.mxf` 路径回归不受 DNG 优化影响。

## 4. 推荐执行顺序与依赖

```text
A LibRaw provenance
  ↓
D Release flags hardening
  ↓
C faststart failure/staging
  ↓
B AVFrame pool default rollback
  ↓
E documentation and gate unification
  ↓
F arm64 Release matrix and long-run acceptance
```

说明：

- A 必须先于所有并发 DNG 正确性测试，否则测试可能误用旧库。
- D 必须先于性能测试，否则 Debug/`-O0` 数字不可比较。
- C 可以与 B 并行开发，但最终验收必须分别注入失败。
- E 只能在 A–D 的实际结果确定后回填，不能提前把 host 结果写成发布结论。
- F 是发布决定的最后门禁，不得被 Gradle 成功或 host harness 成功替代。

## 5. 变更与验收记录模板

每个分段完成后追加以下记录，不直接修改历史实验数字：

```text
分段：A/B/C/D/E/F
日期：
代码版本或变更标识：
ABI：
BuildType：
NDK/API：
LibRaw 源码 pin：
LibRaw libraw.a SHA-256：
脚本 SHA-256：
编译宏：PRF_PRORES_THREADS= / PRF_AVFRAME_POOL= / PRF_LIBRAW_REUSE=
输入素材与帧数：
重复策略：interleaved round-robin，轮数=
结果：
VmHWM / wall / CPU / 温度：
帧数 / PTS / duration / profile / pixfmt：
PSNR/SSIM/大小容差或精确 SHA：
失败样本与日志：
结论：通过 / 有条件通过 / 不通过
回滚动作：
```

## 6. 回滚策略

| 场景 | 回滚动作 | 禁止动作 |
|---|---|---|
| `raw_r` 构建或 provenance 校验失败 | 阻断发布；修复构建链后重建 | 不得直接使用旧 `LIBRAW_NOTHREADS` 库发布多 worker 版本 |
| AVFrame 池真机负收益或内存超标 | `PRF_AVFRAME_POOL=0` 重新出包 | 不得绕过 refcount 判据强行复用 |
| faststart 在设备上失败 | 临时默认 `faststart=0`，或阻断任务并清理 staging | 不得忽略返回值继续报告成功 |
| Release flags 校验失败 | 清理 `.cxx`，重新 configure/build | 不得使用历史 APK 充当当前 Release 证据 |
| raw_r 后输出仍不稳定 | 暂停精确 SHA 门禁，改用 PSNR/PTS/帧数分析并定位根因 | 不得把旧库、新库 hash 混比 |
| 真机长跑 OOM/温控失败 | 降低 worker 或固定安全线程档位，重新测量 | 不得仅用 host 内存数字放宽设备门槛 |

## 7. 完成定义

本修复方案只有在以下条件全部满足后才可标记为“完成”：

- 两 ABI 的 raw_r provenance、SHA 和线程安全符号门禁通过；
- Release compile commands 无 `-O0` 且包含 `-O3 -DNDEBUG`；
- faststart 失败不会静默成功，也不会暴露半搬移最终文件；
- AVFrame 池默认值由 arm64 A/B 数据决定，且满足 10%/VmHWM 门槛；
- 实施报告、接口契约和性能复核报告的当前口径一致；
- arm64 60 帧 A/B、900 帧长跑、取消、失败、色彩、普通容器回归全部有可追溯记录；
- 1080p/4K 内存和耗时目标满足当前契约；
- `check-contracts`、单元测试、Gradle Debug/Release 构建和最终 APK 产物核对均通过；
- 所有仍未修复的整体发布阻断在发布说明中明确列出，不能以“性能优化已完成”掩盖。

## 8. 方案结论

当前最先执行的是 A 段构建 provenance 修复和 D 段 Release 门禁硬化；这两项决定后续测试是否可信。AVFrame 池暂不应凭 host 负收益直接删除，但在 arm64 正向证据出现前不应作为发布默认。faststart 必须从“best effort 且静默失败”升级为可观测、可清理、可验证的输出阶段。完成这些修复和真机验收后，才能对性能优化方案给出“可发布”结论。
