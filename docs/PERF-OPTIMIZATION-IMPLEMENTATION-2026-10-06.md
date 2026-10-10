# ProResForge 性能优化实施报告(2026-10-06)

> 依据:docs/PERF-OPTIMIZATION-PLAN-2026-10-06.md(方案)
> 依据:docs/PERF-RAW-TO-PRORES-VERIFIED-2026-10-06.md(复核报告,实测数字出处)
> 本文记录:方案各项的落地结论、host 侧证据、延后项与真机复测协议。
> 结论口径:一切 host(x86_64 WSL)绝对毫秒不可外推真机;真机数字必须按 §6 协议复测后回填。
> **口径更新(2026-10-06 整改轮 A–F 段)**:§1–§8 的历史内容一律保留原文作为历史证据,
> 其中与现状矛盾的结论已就地加「口径更新」标注;**当前有效口径以第 9 章为准**。
> 本轮纪律:凡未在设备上执行的验证一律写「未验证/待真机」,编译通过不得写成功能验证。

## 1. 总览

| 项 | 状态 | 默认值变化 | 说明 |
|---|---|---|---|
| P0-1 Release native 基线 | ✅ 已落地(口径已更新) | CMake 默认 Release/-O3 -DNDEBUG;release buildType 追加 `-DCMAKE_BUILD_TYPE=Release` | app/src/main/cpp/CMakeLists.txt、app/build.gradle.kts;**当前口径:Release flags 归一化(-O0 漏洞已修,强制 -O3 -DNDEBUG),见第 9 章 D 节** |
| P0-2 prores_ks 帧级线程 | ✅ 已落地 | `PRF_PRORES_THREADS=3`(仅 prores_ks 编码器,`FF_THREAD_FRAME`) | app/src/main/cpp/transcode.cpp |
| P1-1 depth 与 worker 联动 | ✅ 已取证,**不联动** | 无变化(默认 `dngdepth` 0=2) | 见 §4.1 |
| P1-2 worker-local LibRaw 复用 | ✅ 已落地 | `PRF_LIBRAW_REUSE=1` | 见 §4.2 |
| P1-3 AVFrame 跨帧池(第一步) | ✅ 已落地(口径已更新) | **默认 0(2026-10-06 整改)**;host 收益为负,开启条件=真机 ≥10% 且 VmHWM 不增,待验收 | 见 §4.3(历史证据)+ 第 9 章 B 节(当前口径) |
| P1-3 第二步(worker 直写 pool frame,零拷贝) | ⬜ 延后 | — | 见 §5.2 |
| P2 faststart 策略 | ✅ host 取证已完成(2026-10-06),真机待验收 | 默认不改(产品决策);失败路径已改为可观测/任务显式失败 | 见 §4.4 + 第 9 章 C 节 |
| P2 首帧复用(seed) | ✅ host 取证已完成(2026-10-06),真机待验收 | — | 见 §4.4 |
| 附:LibRaw 线程安全版重编 | ✅ 已落地(口径已更新) | prebuilt `libraw.a` 换 `raw_r` 构建;**本轮已加 provenance manifest 门禁(段A),当前 prebuilt 无 manifest 属预期拒绝** | 见 §4.5 + 第 9 章 A 节 |
| P3 filtergraph/其他 | ⬜ 不做(方案本身划为 P3 且条件性) | — | 见 §5 |

## 2. 环境与验证口径

- host 验证环境:WSL Ubuntu-24.04,12 vCPU(存在 SMT 拥挤负载噪声),FFmpeg 6.1 自编译(/home/ubuntu/ffx86/inst),LibRaw 0.21.4。
- 输入样本:`tmp/dngzip-test-4f.zip`(62,936,710 B,4 帧,复核报告 A/B 同款)与 60 帧 zip(944,050,462 B,4096×3072,每帧 15,734,066 B)。
- 性能判定纪律:**所有性能结论必须 interleaved round-robin 重复 ≥2 轮**。host 单次跑在负载 2.4-3.1 下同配置 60 帧曾出现 39s~137s 的抖动,单次数字会得出反向结论(复核报告与 P1-1/P1-3 实施均因此踩过坑)。
- 正确性判定口径:复核报告记录「prores_ks 输出不可逐字节复现」,后证实为旧 LibRaw 单线程版竞态导致解码像素逐次不同(见 §4.5);**换成线程安全版后,同二进制同输入输出 sha256 稳定(帧级 prores 线程 3 下仍稳定)**。因此本轮 host 门禁可恢复精确 sha 对比(方案阶段的 PSNR/SSIM+大小容差口径保留作旧产物比对用)。旧二进制/旧库产物不得与新库产物比对。
- 已记录的既有问题(非本方案引入,未改):① 容器 `counted_video_packets` 比输入帧数少 1;② 偶发 rc=183 LibRaw unpack I/O 错误(负载 2.5+ 的宿主噪声,pool/nopool 同错同码)。(复核报告记录的「输出不可复现」已归因于旧 LibRaw 竞态并随 §4.5 修复。)
- 泄漏口径:混合静态 libav 进程中 LeakSanitizer 不可用(三种方式证实),ASan 只覆盖内存错误类;不得把「ASan 零报告」写成「零泄漏」。

## 3. P0(已落地)

### 3.1 P0-1 Release native 基线

改动:
- app/src/main/cpp/CMakeLists.txt:16-33:默认 `CMAKE_BUILD_TYPE=Release`;Release/RelWithDebInfo 下 `CMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG"`(已含 -O 则不重复追加);显式禁 `-ffast-math`(会改颜色/编码一致性);Debug 不升优化。
- app/build.gradle.kts:108-119:release buildType 内 `externalNativeBuild.cmake.arguments += "-DCMAKE_BUILD_TYPE=Release"`。

证据(host):
- 此前 native 全 Debug/-O0(复核报告 §3.5);实测 4→3 通道循环 -O0 36.1ms vs -O2/-O3 5.6ms(结构结论,真机复测)。
- `assembleRelease` BUILD SUCCESSFUL;`.cxx/Release/*/{arm64-v8a,armeabi-v7a}/compile_commands.json` 每编译单元 `-O3`×4、`-DNDEBUG`×4;`assembleDebug` 的 `-O3` 计数=0(互不影响)。
- host harness(src=dngzip faststart=0,4 帧)rc=0,17.59MB .mov,duration 0.120s,4096×3072。

> **口径更新(2026-10-06 段D)**:本节是 P0-1 的原始落地记录。上面「(已含 -O 则不重复追加)」
> 即审查项 P2-01 的漏洞(`-O0` 也含 `-O`,外部/缓存注入 `-O0` 会被当成「已优化」原样放行),
> 已按第 9 章 D 节改为「先剥掉所有 -O 级别与重复 -DNDEBUG,再统一强制 `-O3 -DNDEBUG`」,
> 并新增 `scripts/check-native-flags.sh` 与 Gradle native-flags-manifest 门禁。
> 本节历史证据(compile_commands `-O3`×4 等)保留原文,但**不再单独作为所有机器/缓存机的保证**。

### 3.2 P0-2 prores_ks 帧级线程

改动:app/src/main/cpp/transcode.cpp:1431-1438(约,以现行文件为准):`is_prores` 分支设 `enc->thread_count = PRF_PRORES_THREADS`(宏默认 3,:17-21)+ `FF_THREAD_FRAME`;不碰解码器、不设 thread_count=0、不碰 flush 语义。

证据(host,4 帧 zip):
- 复核报告 A/B:1T 23.74/24.03s → 2T 13.34s(1.78×)→ 4T 12.25/14.96s → 12T 12.94s;MaxRSS 778.6→779.7MB(+≤1.1MB);拐点=2 线程。
- 实施后复测:threads=1 21.75s vs threads=3 11.69/12.01s(≈1.8×),MaxRSS 779,156→779,540/779,728KB(+0.6MB),两边输出 packets/decoded_frames 一致(3)。
- 码率不可逐字节复现的说明见 §2 口径。

## 4. P1 与附修(已落地)

### 4.1 P1-1 dngthreads/dngdepth 联动 → 取证后**不联动**

host 60 帧矩阵(`/usr/bin/time -v`,4096×3072):

| dngthreads | depth | Elapsed(s) | user(s) | CPU% | MaxRSS(KB) |
|---|---|---|---|---|---|
| 2 | 2 | 137.08 | 318.52 | 238% | 935,552 |
| 2 | 3 | 103.49 | 275.68 | 274% | 1,064,956 |
| 2 | 4 | 118.92 | 306.16 | 264% | 1,207,480 |
| 2 | 6 | 131.98 | 355.35 | 275% | 1,379,640 |
| 4 | 2 | 48.14 | 141.90 | 309% | 969,428 |
| 4 | 3 | 39.18 | 140.01 | 375% | 1,047,016 |
| 4 | 4 | 107.75 | 250.84 | 240% | 1,335,332 |
| 4 | 6 | 146.00 | 372.39 | 260% | 1,515,160 |

12 帧受控 round-robin 4 轮:T=2 depth2 = 18.5/16.3/23.5/23.7s vs depth3 = 40.7/39.1/37.2/37.1s;**T=4 depth2 = 23.9/15.7/25.1/23.1s vs depth4 = 30.0/33.5/30.0/33.5s**。/proc 线程级采样证明 depth 增大带来的 CPU 100% 落在 `prores_ks` 帧线程编码内部,主线程 + 每 worker 仅 ~4.5s;每深度槽位 +130-160MB RSS。

结论:不满足 ≥10% 门槛,**`dng_parallel_params` 零功能改动**(默认 depth=2,spec 强制优先/钳[2,8]语义不变),仅 transcode.cpp:123-132 补证据注释;明细见 docs/INTERFACES.md §10.7.2。

### 4.2 P1-2 worker-local LibRaw 实例复用

改动(仅 app/src/main/cpp/dngzip.cpp):`PRF_LIBRAW_REUSE` 默认 1;:846-897 `thread_local std::unique_ptr<LibRaw> tlr` + 「默认丢弃、成功才 keep」RAII 守卫 + **构造默认 params 快照**(每帧先恢复再 `apply_libraw_params`);:971-974 成功路径 `keep=true`;`#else` 保留每帧新建/析构的原行为。

关键机理(源码依据):
- `LibRaw::recycle()` 只释放 image/raw_alloc/thumbnail/meta/... 并 ZERO imgdata 的 sizes/idata/...,**不重置 imgdata.params**。
- `apply_libraw_params()` 只设置当前档位字段(half_size 仅 FAST 置 1 等),复用实例会把上一帧的值带进后续帧 → 实测:FAST 帧后跟 BALANCED/FINE 帧残留 half_size=1 走半尺寸解算,尺寸与内容漂。构造默认快照 + 每帧先恢复,修复后档位轮换 24/24 PASS(修复前 16/24 FAIL)。

host 证据:48/48 sha 逐字节一致(distinct=1);失败注入 36/36 rc=-1、12/12 重解=基线;内存 50 轮 VmHWM +240kB(0.1%);耗时 372.9 vs 375.6 ms/帧(噪声内)。真机收益才是目标(消除每帧 ~125MB malloc/free 抖动)。

### 4.3 P1-3 AVFrame 跨帧池(第一步)

改动(仅 transcode.cpp + docs):`PRF_AVFRAME_POOL` 默认 1;:174-289 `struct AvFramePool`(RAII;复用判据 `f->buf[0] && w/h 同 && format==RGB48LE && av_buffer_get_ref_count(f->buf[0])==1` → 只更新 pts;否则 unref+get_buffer)+ `avframe_pool_count`(N=depth+2,按 1GB 预算 25% 封顶:4096×3072→3、1080p→4、8K→1);:1009-1030 `push_frame(Pipe*, AVFrame*, bool keep)` + `AV_BUFFERSRC_FLAG_KEEP_REF`;:1968-2071 zip 主循环 `#if` 分支。

所有权结论(基于 buffersrc.h:53 `AV_BUFFERSRC_FLAG_KEEP_REF=8`):flags=0 走 `av_frame_move_ref`(壳重置,老路径随后的 unref 是 no-op);KEEP_REF 走 `av_frame_ref`(计数+1),故推帧后计数 2、`drain_graph` 消费完回 1、此时才可覆写。**N=depth+2 是余量不是正确性条件(引用计数兜底)**。

host A/B(60 帧 interleaved 2 轮):nopool 74.01s/975,980KB 与 38.93s/963,792KB;pool 126.33s/1,207,160KB 与 132.99s/1,202,952KB(池计数「原地复用 57/重新分配 3」);minor faults -22%(572k vs 737k)但墙钟慢 1.6-1.7×、RSS +220MB,多出的 CPU 100% 在编码线程;内存微基准 45.8ms → 复用 6.2ms,每帧边际 ~39.6ms 撑不起观察差。**按任务契约保留默认 1**,翻宏条件与真机协议见 INTERFACES §10.7.2 与本文 §6。

> **口径更新(2026-10-06 段B)**:本节是整改前的落地记录。文中「`PRF_AVFRAME_POOL` 默认 1」
> 与「按任务契约保留默认 1」是**历史决定,已被第 9 章 B 节取代**——发布默认现为 **0**
> (`transcode.cpp:216-218`);上面的 host A/B 负收益数字(墙钟慢 1.6~1.7×、RSS +220MB)
> 原文保留,正是把默认改为 0 的依据。`#if/#else` 两路径已按 NDK Release arm64-v8a
> 编译验证(仅编译,不等于功能/性能验证)。docs/INTERFACES.md §10.7.2 的默认值口径已同步更新。

### 4.4 P2 faststart + 首帧 seed(✅ 已落地)

**faststart:不修改任何东西,默认 1 保留。** 实测取证(host,harness + ffprobe):
- 4 帧任务 faststart 阶段 0.041–0.042s,墙钟与 `faststart=0` 无统计学差异;
- 合成大文件 moov 前置扫描全文件近似一遍:512MB→0.098s、1GB→0.175s、2GB→0.378s、4GB→1.083s、5GB→1.201s、8GB→2.065s(且 moov 大小几乎不影响,成本≈成品字节数×2/磁盘带宽)+ ffprobe: `fs=1` 时 box 顺序 `[ftyp][moov][wide][mdat]`,前置正确。
- 结论:不存在「大文件关 faststart」的拐点,默认可交互(已写出)场景收益 > 成本,**保留 `kFastStart` 默认 1**。(HomeViewModel `faststart` 默认 true 不动。)

**首帧 seed(✅ 已落地)**:zip 输入 init 段为拿尺寸必然解一次 seq=0,seed 把该帧搬进解码队列 slot0(换走 dng_rgb/dng_buf,72MB 无拷贝),worker 从 seq=1 派活。
- 锚点:`app/src/main/cpp/dngframeq.h` seed() 声明 :129 / 实现 :208-214 / start() 内唯一落地 :241-248(seq=0、ready=true、m_assigned 从 1)/ stop() 复位 :445-453;`app/src/main/cpp/transcode.cpp` seed_frame 声明 :1293-1294、swap :1382-1386、`pool.seed()` :1980-1985(在 start() 前,被拒仅 WARNING 降级全量解码)。
- 正确性:pts 序列容器层 stts 取证 `[0,512,1024]` 与 base 完全一致;单帧 `total=1` 不卡(worker 见 `m_assigned>=m_total` 立即退出);取消矩阵 cancel 注入 4 帧 delay 0/50/200/500/1500/3000ms → rc=1(0.03/0.89/0.82/2.42s)/ rc=0 正常完成;`dngthreads=1 dngdepth=1` 输出与 base 逐字节相同(md5 e6d99d883ceeb75c760aa3f3c4e02524)。
- 收益:MaxRSS 每轮稳定 **-87MB**(消除 init 帧与 worker 首帧两份 72MB+15.7MB 并存);`dngthreads=1` 端到端 -12.5%(3.61→3.12s);默认参数下墙钟在噪声内(4 帧任务编码占 ~86%,增量被并行余量吃掉——帧级越多越明显)。
- `docs/INTERFACES.md` 新增 §10.7.3 记录上述取证。

**HomeViewModel:评估后不改。** `faststart` 默认 true 保留;`dngthreads/dngdepth` 不上 UI(不能新增 spec 键,§10.7.2 已证自动策略最优,专家旋钮风险大于收益)。

### 4.5 附修:LibRaw 线程安全版(✅ 已落地)

B 段实施 P1-2 时发现的正确性阻断:仓库自带三份 libraw.a(host + app prebuilt 两 ABI)均为 `LIBRAW_NOTHREADS` 单线程版,LibRaw 每实例 TLS 退化为进程级函数内 static(decoders_dcraw.cpp:22-31;命中符号 `_ZZN6LibRaw10getbithuffEiPtE5{reset,vbits,bitbuf}`、ph1_bithuff、cielab{cbrt,xyz_cam}、sony_decrypt{p,pad}、pana_data{buf,vpos});生产默认 `dng_parallel_params` nthreads=min(4,cores-1) 并发解帧会踩同一进程级缓存。scripts/build-ffmpeg-android.sh:581-583 主动 `rm -f libraw_r.a` 只留单线程版。用户已批准重编线程安全版并替换 prebuilt。

修复内容(用户批准,超出原方案):
- `app/src/main/cpp/prebuilt/{arm64-v8a,armeabi-v7a}/lib/libraw.a` 换成 LibRaw-cmake `raw_r` target(同源码同参数,唯一差别是不加 `LIBRAW_NOTHREADS`);app 侧链接名不变(契约不变)。
- `scripts/build-ffmpeg-android.sh` LibRaw 段(:503-611)改为只构建/安装 `raw_r` 并把产物复制成 `libraw.a`,新增 sha256 交叉校验(装出的 libraw.a 必须等于 raw_r 产物);`:66 LIBRAW_CMAKE_SHA` 与 verify_pin 未动。
- `app/src/main/cpp/prebuilt/BUILDINFO`(新建)与 `README.md` 记录本次修订与重建注意。

host 实测证据(Lead 已独立复核符号差集):
- 符号差集:旧→新恰好消失 11 个 static 局部符号、0 新增;arm64 未定义符号集 391/391、armv7 405/405 完全相同;归档成员 75/75 同名;重定位画像与旧 PIC prebuilt 同分布;zlib 依赖不变。Lead 复核:两个 ABI 新 libraw.a 中上述 11 符号命中数=0,旧备份中均在。
- 竞态探针(同一 DNG 帧,fnv64 over 16bit 图):旧库 2T(6 次)/4T(12 次)输出全不同 → FAIL;新库 2T×5/4T×5 全部 `332e8641377d662d` → PASS。
- 单线程输出同一性:9 组档位 × 色模(Fast 1536×2048 / Balanced、Fine 3072×4096)逐字节 BYTE_IDENTICAL;帧 1/2/3 × FAST 同样一致;新库 2T/4T 输出 == 新库串行 == 旧库串行。
- tcharness 端到端(4 帧 dngzip,src=dngzip faststart=0):rc=0,prores/yuv422p10le/4096×3072,duration 0.1200s。
- **e2e 决定性**:旧库同 zip 跑 3 次 sha256 全不同(9dceb667…/7d150f3e…/d566b361…),新库 3 次全等(dea33c1248e1a61fccfed36f00d392a9af7268d6bb47f755ae65860c3acb7c48,17,567,843 B)。

**重大口径修正**:复核报告与 C/B 段记录的「prores_ks 不可逐字节复现」实为 LibRaw 进程级 static 竞态导致解码像素逐次不同。换成线程安全版后,同二进制同输入输出 sha256 稳定可复现(帧级 prores 线程 3 下仍稳定)。后续门禁**可以恢复精确 sha 对比**(但旧二进制/旧库结果不可与新库比对)。

历史偏差(不阻塞):仓库 prebuilt 的 8 个 libraw 头文件为全行 CRLF(历史上 Windows/SMB 往返所致,连 x265_config.h 等公共头同样),新装头为 LF;`sed 's/\r$//'` 后内容逐字节相同,无回归,未替换头文件。建议后续统一全 prebuilt 行尾。

备份(旧单线程版):`\\192.168.100.232\win\tmp\prf-libraw-backup\{arm64-v8a,armeabi-v7a}-libraw-OLD-nothreads.a`(sha 34d2226a…/314793a9…);新库耐用副本在 `/home/ubuntu/.prf-ffmpeg-build/raw_r_install_<abi>/lib/libraw.a`,host 线程安全版在 `/home/ubuntu/dzwork/libraw_host_r/`。

下次完整构建注意:脚本 `:552` 的 skip 守卫在 prefix 已有 libraw.a 时会 `[skip]` 跳过 LibRaw 段(旧逻辑幂等),要真重建须清 prefix 或至少 rm libraw.a;BUILDINFO 的脚本 sha 以重跑后为准;gradle/APK 侧与真机端到端由 Lead 统一验证。

## 5. 延后与不做

- P1-3 第二步(worker 直写 pool frame,零拷贝):方案自称生命周期风险最高。第一步的引用计数证据(KEEP_REF + refcount==1 判据)已为它铺路,但直写要求证明「帧 buffer 在被 graph 读取期间不被覆写」,host A/B 亦显示当前收益结构,延后到真机协议给出正向信号后再评估。
- P3(filtergraph nb_threads 条件 A/B、不换 inflate、不重编 FFmpeg):方案本身列 P3;复核报告已证「滤镜单线程」结论错误(nb_threads=0=自动),无明确收益面,不做。

## 6. 真机复测协议(所有项共用)

1. 构建:arm64 **Release**(Merge-`-O3`)APK,装真机。
2. 输入:≥60 帧 4096×3072 子集(每项 A/B ≥4×N 轮),负载 <1.0(飞行模式、无后台)。
3. 记 VmHWM 与墙钟;**interleaved round-robin** 交替跑两个配置,不做单次对比。
4. P1-3 池化去留判据:池化 ≥10% 更快且 VmHWM 不涨,才保留 `PRF_AVFRAME_POOL=1`,否则宏改 0 重新出包。
5. 验收对齐 docs/INTERFACES.md §10.7/§10.8:1080p60/900 帧 <20min;4K/900 帧 <90min;峰值 ≤400MB(1080p)/≤500MB(4K)。
6. LibRaw 线程安全版真机重点:多 worker 并发解帧下确认无画面异常(坏块/色偏),与 seed/池化组合验证。

## 7. 变更文件清单(实施后回填 md5)

| 文件 | 段 | md5 |
|---|---|---|
| app/src/main/cpp/transcode.cpp | A/C/D | c3e40f4f843e74d25166fbeab7a3c173(基线 f2fd0e82…,A/C/D 三段叠加) |
| app/src/main/cpp/dngzip.cpp | B | e13fb8439757bbc47777aa834d4fe611 |
| app/src/main/cpp/dngframeq.h | D | 5c7b493f4dd018bff7515065ee67cf7e(基线 c2862454…) |
| app/src/main/cpp/CMakeLists.txt | A | 9091d5a009d864e1f70b5bf650f57773 |
| app/build.gradle.kts | A | f8accf35f00b63a4870c59bb0befc8be |
| app/src/main/cpp/prebuilt/{arm64-v8a,armeabi-v7a}/lib/libraw.a | E | c48fa6cc…/8bbadf84…(旧:79a6e06d…/2f152fd0…,已备份) |
| scripts/build-ffmpeg-android.sh | E | 58c736bb03954659bd0a69c7eccc1a7b(基线 9ee92405…) |
| app/src/main/cpp/prebuilt/BUILDINFO | E(新建) | b5c71182fd5f61854d5ff0c84861d2a8 |
| app/src/main/cpp/prebuilt/README.md | E | 94d34172c1c1ebd2a950b52a06c15fb0(基线 5c1b5d0b…) |
| docs/INTERFACES.md | C/D | 1ad0943e8244382e88a7ef2d02fe1589(基线 997ea41f…,新增 §10.7.2/§10.7.3、depth 口径) |
| docs/PERF-OPTIMIZATION-IMPLEMENTATION-2026-10-06.md | Lead | 本报告 |

**本轮(2026-10-06 整改轮 A–D)追加清单**——代码文件 md5 已由 Lead 合并验证后回填(2026-10-06 21:2x,合并验证定稿时的最终值):

| 文件 | 段 | md5 / 备注 |
|---|---|---|
| scripts/build-ffmpeg-android.sh | A | `6c4f881969ccf02b2c9a23cd3ecaed87`(948 行;函数区 L209-433 + build_abi 内 L782-1007,per-ABI manifest 门禁) |
| scripts/verify-prebuilt.sh | A(新建) | `653644f6e1af103b031a411df0653ee0`(399 行;独立复验 manifest 与 libraw.a;真 prebuilt 上 rc=1) |
| scripts/check-native-flags.sh | D(新建) | `a4e81130644f90043a8fa1683f51bfc7`(145 行;Release flags 门禁,14 PASS) |
| app/src/main/cpp/CMakeLists.txt | A/D | `057333ca5480cd13c0f00a1fe80a48e2`(241 行;L130-262 prf-libraw-gate 段;L16-50 prf-release-flags 段) |
| app/build.gradle.kts | A/D | `1363d2e73fd7543f18b48525e1654b55`(440 行;L54-357 门禁 + L349-357 挂 release variants;L166 prfReleaseCmakeArgs / L173 prfCleanStaleNativeCxx / L301 prfWriteNativeFlagsManifest;Lead 追加 java.* import 修复) |
| app/src/main/cpp/transcode.cpp | B/C | `8b30ebf51abac6b108bc01c12cb935cb`(2441 行;:216-218 宏默认 0;:621-754 自检+具名失败点;:2449-2475 成功路径升级;:2542-2561 loop_error 仅告警) |
| app/src/main/cpp/prebuilt/README.md | A | `54d0dbc9a7a02b00355fcbd820a59010`(160 行;重写,写清「已有 libraw.a 不能证明线程安全」) |
| docs/INTERFACES.md | E | `507877445208c2ebd53ec39c1c89f113`(与 E 段自算值一致,E 改 §10.7.2/§10.7.3 及 §6 LibRaw provenance 口径) |
| docs/PERF-OPTIMIZATION-IMPLEMENTATION-2026-10-06.md | E | 本报告;自身 md5 属自引用(写入即失效),最终以「合并验证报告」章节为准 |

**未改(核过)**:native-lib.cpp / preview.cpp / preview.h / transcode.h / dngzip.h / strspec.h / check-contracts.sh / FFmpeg.kt / HomeViewModel.kt / 两 ABI libav*.so;app 安装态 spec 键数仍 27(契约一致)。删除编辑器残留 `transcode.cpp~RF55b9c91.TMP`(89,720B,无独有内容)。

## 8. Lead 合并验证(最终门禁,全部通过)

| # | 项 | 结果 |
|---|---|---|
| 1 | 写集纪律 | md5 对基线逐文件 diff:仅 A/B/C/D/E 声明的 7 文件 + BUILDINFO 变动,其余 same |
| 2 | 契约 check-contracts | RC=0(JNI 6 / CMake 4 / 库 7 / spec 27 键三方一致) |
| 3 | host harness 重编(链线程安全版 libraw_host_r) | rc=0,仅剩既有 deprecated 警告 transcode.cpp:502 |
| 4 | 4 帧 e2e × 2(并行默认规格) | 两次 rc=0、sha256 全等 **dea33c1248e1a61fccfed36f00d392a9af7268d6bb47f755ae65860c3acb7c48**(17,567,843B),与 §4.5 单测库结果一致 |
| 5 | P0 A/B(受控编译宏 1T vs 2T,interleaved×3) | 1T 5.33/5.24/5.29s vs 2T 3.76/3.72/3.72s(≈1.42×,宿主当前负载下;方向与 §3.2 一致),6 个输出 sha 全部相同 |
| 6 | gradle 全量合并构建 | **BUILD SUCCESSFUL in 7m32s**:buildCMake Debug/Release × arm64-v8a/armeabi-v7a 全部真编译,testDebugUnitTest 22 用例 0 失败,DngSpecMappingTest/FormatCatalogTest/HistoryItemSerializationTest 全过 |
| 7 | 产物 | app-debug.apk 56.1MB、app-release-unsigned.apk 52.2MB(内含 raw_r libraw.a 两 ABI) |

遗留环境说明:旧 `.cxx/` 缓存按 Y: 映射路径生成,须删 `app/.cxx` 后用稳定盘符映射(Z:)重跑 gradle;cmd 对 UNC cwd 默认落到 Windows 目录,勿直接 `\\...` 跑 gradlew。

> **当前口径指针**:本节「全部通过」是 2026-10-06 首轮(A/B/C/D 段)的 Lead 合并验证证据,
> 作为历史记录保留。2026-10-06 整改轮(A–F 段)的落地记录、未覆盖项与 F 段验收协议见
> **第 9 章**;发布结论以第 9 章的未覆盖项与 F 段真机验收为准。

## 9. 2026-10-06 整改轮(A–F 段)落地记录

> 本章为**当前有效口径**,依据 docs/PERF-OPTIMIZATION-REPAIR-PLAN-2026-10-06.md 的
> A–F 分段与 docs/PERF-OPTIMIZATION-AUDIT-2026-10-06.md 的 P2-03/P2-04。
> 纪律:① 每节都必须显式列出**未覆盖项**,凡未在设备上执行的一律写「未验证/待真机」;
> ② 编译通过、宿主持久通过、门禁通过都不等于功能/性能验证;③ 历史取证数字保留原文,
> 只作「历史/宿主证据」,不外推真机。

### 9.0 本轮共用的 host 验证证据

- 宿主编译:通过(NDK Release arm64-v8a;`#if/#else` 两条池化路径均可编译)。
- 门禁矩阵:**13/13**(A 段 provenance 注入场景)与 **10/10**(D 段 flags 场景)通过。
- `scripts/check-native-flags.sh` 门禁 **14 PASS**。
- `bash scripts/check-contracts.sh` → **RC=0**(JNI 6 / CMake 源 4 / runtime 库 7 /
  **spec 键 27**,strspec.h 注释 `kAll[27]` 与 docs/INTERFACES.md §3 三方一致)。
- `scripts/verify-prebuilt.sh` 对**真实 prebuilt** → **rc=1**(该副本无有效 manifest,
  提示重跑 `scripts/build-ffmpeg-android.sh`),证明门禁会拒绝无 provenance 产物。

### 9.1 A 段:LibRaw provenance 与强制重建门禁

**改动文件与行锚点**:

- `scripts/build-ffmpeg-android.sh`:全量函数区 **L209-433** + `build_abi` 内 **L782-1007**。
  「有 `libraw.a` 就 skip」已改为 per-ABI manifest 门禁:manifest 缺 / 字段不符 /
  `libraw.a` SHA-256 不符 / 脚本自身 sha 变 → 删掉该 ABI 的 `lib/libraw.a` + manifest,
  强制从源码重建 `raw_r`,重建成功才写 manifest(**16 字段**:`abi` / `api_level` /
  `ndk_revision` / `libraw_tag` / `libraw_cmake_sha` / `raw_r_target=raw_r` /
  `libraw_nothreads=absent` / `raw_r_cmake_flags`(不含 `LIBRAW_NOTHREADS`)/ `threads_lib` /
  `script_sha256` / `libraw_sha256` / `symbols_sha256` / `members_sha256` /
  `build_time_utc` / `manual=false`);符号/成员 sha 由 NDK `llvm-nm --defined-only` /
  `llvm-ar t` 生成。
- `scripts/verify-prebuilt.sh`(**新增**):可独立复验 manifest 与 `libraw.a`(真实 prebuilt
  无 manifest → rc=1,提示重跑脚本)。
- `app/src/main/cpp/CMakeLists.txt` **L130-262**(`prf-libraw-gate` 段):CMake configure
  校验 manifest,缺失/不符 → FATAL_ERROR 并提示重跑脚本。
- `app/build.gradle.kts` **L54-357** 门禁 + **L349-357** 挂 release variants:release
  变体消费前校验 manifest;`manual=true` → `GradleException`「BUILDINFO 不再允许手工替换,
  必须重跑 scripts/build-ffmpeg-android.sh」。
- `app/src/main/cpp/prebuilt/README.md`:重写,明确「已有 `libraw.a` 不能证明线程安全」,
  必须通过 manifest 和符号检查。
- docs/INTERFACES.md §6 LibRaw 段已追加 provenance 门禁口径(见该文件)。

**未覆盖项(显式)**:

- 两 ABI 从源码完整重建 `raw_r` 并生成有效 manifest:**未执行/待执行**(当前副本无有效
  manifest,需在联网机器重跑 `scripts/build-ffmpeg-android.sh`,约 20~40 分钟/ABI)。
- 因此 **CMake configure 与 Gradle release 构建在当前副本上失败是预期**,不是回归;
  修法 = 联网重跑脚本。文档(含 INTERFACES §6)已写清,避免误认为回归。
- manifest 只证 provenance(来源/线程模型),**不证行为**:真机/宿主 2T/4T 多轮回合同帧
  解码输出 sha 恒定 —— **仍未覆盖/待真机**。

### 9.2 B 段:AVFrame 池发布默认改为 0

**改动文件与行锚点**:

- `app/src/main/cpp/transcode.cpp` **:216-218**:`PRF_AVFRAME_POOL` 发布默认 **1 → 0**。
  依据 = host A/B 池化墙钟慢 1.4~1.7×、RSS +220MB,arm64 真机证据缺失。
- 开启(=1)准入条件:同设备同负载下 ≥10% 吞吐提升且 `VmHWM` 不增、interleaved 4 轮方向
  一致;回退 = 改一个宏。`#if/#else` 两路径均已 NDK Release arm64-v8a **编译**验证。
- 文档同步:docs/INTERFACES.md §10.7.2(L553 默认值口径、L580 判定依据处的醒目整改口径、
  L614 起真机协议标注「把宏改成 0」已执行);本文 §1/§4.3 已加口径更新标注。

**未覆盖项(显式)**:

- 真机 A/B(`PRF_AVFRAME_POOL=1/0`,interleaved ≥4 轮,记 `VmHWM` 与墙钟):**未验证/待真机**。
- 池化路径的取消/解码失败/filtergraph flush/输出写入失败 soak:**未验证/待真机**。
- NDK 编译通过只证明两路径可编译,**不等于功能或性能验证**。

### 9.3 C 段:faststart 可观测失败

**改动文件与行锚点**(`app/src/main/cpp/transcode.cpp`):

- **:621-754** `faststart_move_moov()`:新增搬移后重扫顶层 box 自检([ftyp][moov]、moov
  偏移 = 搬移起点、尺寸不变、box 数/总长度)与具名失败点(`FaststartFail.point` / `err`)。
- **:2449-2475**(成功路径/正常收尾):失败 = `AV_LOG_ERROR(stage=moov-move / point / errno /
  out_len / out_ctx.size)` + `result=AVERROR(EIO)` → 任务显式失败,由既有 done 错误清理
  `vp.rep(-1, "error")` 上报,Kotlin `deleteOutputDocument` 删输出(不算伪装成功);
  不补写 trailer(`trailer_written` 守卫,已写过且仅此一次)。
- **:2542-2561**(`loop_error` 路径):保留 best-effort 但只写 `AV_LOG_WARNING`、不改
  `result`、不重复 `av_write_trailer`。
- 文档同步:docs/INTERFACES.md §10.7.3 新增「A′. 当前实现口径(2026-10-06 整改)」,
  并把整改前行锚点(`:616-693` / `:2361` / `:2425`)标注为历史。

**未覆盖项(显式)**:

- 段C 第二步(staging/临时 fd 上 faststart 后验证再提交)**未实施**,需要输出层契约变更
  (Kotlin `createFile`/`detachFd` 流程提供可回滚临时输出),**列为后续项**。
- 真机 faststart 失败注入测试未做(建议注入 `ENOSPC`/`EBADF` 或破坏 staging 副本 moov);
  运行时 log 与 `result` 联动**未在设备验证**。

### 9.4 D 段:Release -O3 硬化

**改动文件与行锚点**:

- `app/src/main/cpp/CMakeLists.txt` **L16-50**:旧漏洞
  `if(CMAKE_CXX_FLAGS_RELEASE MATCHES "-O")`(连 `-O0` 都放行)已删;现在先剥掉所有
  `-O0/-O1/-O2/-Os/-Oz/-Ofast` 与重复 `-DNDEBUG`,再统一强制 `-O3 -DNDEBUG`
  (命中 `-ffast-math`/`-Ofast` → FATAL_ERROR)。
- `scripts/check-native-flags.sh`(**新增**)。
- `app/build.gradle.kts`:`prfCleanStaleNativeCxx`(L173)/ `prfWriteNativeFlagsManifest`
  (L301,写 `app/build/native-flags-manifest.txt`:`buildType` / `abi` / `cmake_arguments` /
  `release_cxx_flags` / `PRF_PRORES_THREADS` / `PRF_AVFRAME_POOL` 及来源)+
  `prfReleaseCmakeArgs`(L166);release 变体显式 `-DCMAKE_BUILD_TYPE=Release` 不变。
- NDK 真实验证:注入 `-O0` 被归一吃掉,`check-native-flags` rc=0;不带该 block 的对照
  配置 `-O0` 生效 rc=1。

**未覆盖项(显式)**:

- 真机构建与 `compile_commands.json` 复查:以 F 段 arm64 Release APK 构建为准,**待真机**。

### 9.5 E 段:文档口径统一(本节所属)

- 本文:§1 总览逐行核改;§3.1、§4.3 历史结论加「口径更新」标注(数字保留原文);§7 追加
  本轮文件清单(代码文件 md5 待 Lead 回填);§8 加指针指回本章;新增本章。
- docs/INTERFACES.md:§10.7.2 默认值口径(默认 1 → 当前代码默认 0,保留 host 负收益原文);
  §10.7.3 新增「A′. 当前实现口径(2026-10-06 整改)」并标注第二步未实施、失败注入未验证;
  §6 LibRaw 段追加 per-ABI manifest provenance 口径。
- **spec 键口径(27)**:本仓库当前真实基线为 **27** —— `app/src/main/cpp/strspec.h`
  L84 注释「共 27 个」、L85 `kAll[27]`、L306 定义,`scripts/check-contracts.sh` 输出
  「spec 键 27 个……三方一致」。本文档 §7「未改(核过)」与 §8 表格中记录的 27 即为真实值;
  docs/INTERFACES.md §10.7.3 C 的 27 键契约说明同样正确。
  `docs/PERF-OPTIMIZATION-PLAN-2026-10-06.md` 中的「26」是过期数字,**该文件不在本轮
  写集内(未改)**,已在此登记差异,由 Lead 知悉后另行处理。
- 未新增 spec 键、未改 §1/§3 契约表格与任何计数。

### 9.6 F 段:arm64 Release 真机验收(未执行,本机无设备)

**状态:未执行**。以下为完整验收协议,作为发布前唯一设备门禁:

1. **构建**:arm64 **Release** APK(干净 `.cxx`、稳定盘符工作目录,避免旧 Y:/P:/UNC 路径
   缓存);构建 manifest 完整(含 `PRF_PRORES_THREADS` / `PRF_AVFRAME_POOL`、优化级别)。
2. **输入**:同一份 4096×3072 DNG ZIP 子集,帧数 ≥60;另备 1080p60 与 4K 900 帧样本。
3. **纪律**:每项 A/B **≥4×N 轮 interleaved** round-robin 交替跑(不许先跑完一组再跑另一组),
   负载 <1.0(飞行模式、无后台);记录设备型号 / Android 版本 / 温度起点 / 电池 / 可用内存,
   记 `VmHWM`(procstat)与墙钟、P50/P95、CPU、温度/降频、输出帧数、PTS、时长、文件大小、
   抽帧质量与错误码。
4. **性能门禁**:1080p60 900 帧 **< 20min**;4K 900 帧 **< 90min**(P50 档)。
5. **内存门禁**:峰值 ≤ **400MB**(1080p)/ ≤ **500MB**(4K);任一 900 帧长跑 OOM 或系统
   杀进程即不通过。
6. **色彩门禁**:灰卡/白卡与桌面同参数参考 **PSNR ≥ 40dB**,并检查 Apple Log 元数据与画面映射。
7. **LibRaw 竞态探针**:同一 DNG 帧 2T/4T 多轮回合,输出 sha 恒定(host 已见旧库 FAIL /
   `raw_r` PASS,真机**未验证/待真机**);实际画面无坏块/色偏;失败帧错误带帧号且不静默跳帧。
8. **pool 去留判据**:默认 0 现状下,仅当 `PRF_AVFRAME_POOL=1` 在同设备同负载较 nopool
   ≥10% 吞吐提升且 `VmHWM` 不增、interleaved 4 轮方向一致,才可把默认改回 1;否则维持 0。
9. **ProRes 线程档位**:1/2/3/4 中选择不超过内存与温控预算、且相对 1T 稳定有收益的档位
   (默认候选优先 2 或 3,不使用 auto)。
10. **失败/取消/回归**:注入解码失败、存储失败、取消时机 0/50/200/500ms/晚取消;faststart
    失败注入(`ENOSPC`/`EBADF`/破坏 staging 副本 moov)后任务不得报告成功、目录中无可误认为
    成品的半文件;普通 `.mov/.mxf` 路径回归不受 DNG 优化影响。
11. **结论要求**:任一门禁不通过即不发布;不得用 Gradle 成功或 host harness 成功替代。
