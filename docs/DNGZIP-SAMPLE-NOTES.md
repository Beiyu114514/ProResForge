# DNG 序列 zip 样本核对记录(M0)

> 计划:`docs/DNGZIP-PROGRES-PLAN.md` 里程碑 M0。
> 目的:在写死任何 LibRaw 依赖开关之前,先用真实 MotionCam 输出核对
> 「DNG 序列 zip」的结构与压缩方式,决定 zlib / libjpeg / libjasper 谁需要。

## 1. 容器:zip,不是 tar,也不是单帧 DNG

MotionCam(开源,mirsadm/motioncam)的「延时/DNG 序列」导出是一个 **zip**:

```
DSC_0123.zip
├── frame-00000.dng
├── frame-00001.dng
├── ...
└── frame-00899.dng      # 文件名严格 frame-%05d.dng(小写)
```

计划与实现(`app/src/main/cpp/dngzip.cpp`)按以下约定处理:

| 项 | 约定 | 依据 |
|---|---|---|
| 入口识别 | 前 4 字节 `50 4B 03 04`(`PK\x03\x04`) | 本地文件头签名,`DngZip::is_zip` |
| 帧名过滤 | `^frame[_-]?\d+\.dng$`(大小写不敏感,**位数不写死**);一个都没命中时回退成 zip 内全部 `.dng` 条目,仍按自然序 | 历史约定是 MotionCam 的 5 位零填充,但导出工具/版本会给 `frame_000000.dng`(下划线 6 位)、`frame-1.dng`、`raw/frame-00001.dng`(带目录);命中过任意 `frame-*` 就只用这些,不掺入其它 `.dng`(契约 §10.2) |
| 排序 | **按数字自然序**(`frame-00002` 在 `frame-00010` 前) | 零填充字典序在本例恰好一致,但不得依赖;混合填充时自然序 + UI 警告 |
| 定位 | 只用中央目录(支持 ZIP64)+ local header 算真实数据偏移 | 不依赖 zip 工具生成的其他元数据 |
| 帧数 | 中央目录里过滤后的条目数 | 容器本身没有 fps、没有时长 |

> 采样说明:本节记录的是 MotionCam 公开导出格式与 zip 规范(APPNOTE.TXT)的核对结果。
> 帧名匹配规则已于 2026-10-04 修订(位数不写死 + 回退全部 `.dng`),若再遇到新变体,
> 改 `dngzip.cpp` 的 `is_dng_frame_name()` 或回退分支,不要改调用方;识别不到时
> `av_log` 会打前 8 个条目名 + 命中/回退计数(契约 §10.2)。

## 2. DNG 压缩方式 → LibRaw 依赖开关

DNG 的 `Compression` 标签(TIFF tag 259)决定要不要外部库:

| Compression | 含义 | LibRaw 解码路径 | 需要外部库 |
|---|---|---|---|
| 1 | 无压缩 | 内建 | 无 |
| 7 | **无损 JPEG(LJpeg)** | LibRaw 内建 LJpeg 解码器 | **无**(libjpeg 用不上) |
| 8 / 32946 | deflate / old-style deflate | zlib inflate | zlib |
| 6 / 7(老 Kodak 变体) | 有损 baseline JPEG | libjpeg | libjpeg-turbo |
| 32773 | PackBits | 内建 | 无 |

**核对结论(计划已按此定稿)**:

* MotionCam 走 **Compression=7(LJpeg)** 或 **8(deflate)**;
* Compression=7 → `LIBRAW_JPEG=OFF`(默认),不引入 libjpeg-turbo;
* Compression=8 → NDK sysroot 的 zlib(两 ABI 均已验证
  `${SYSROOT}/usr/lib/<triplet>/libz.so` 存在),不自建;
* lcms / Jasper / OpenMP / rawspeed 全关:色彩矩阵走 DNG 自带的
  ColorMatrix + LibRaw 内置表,足够把帧转成 sRGB。

脚本开关(见 `scripts/build-ffmpeg-android.sh` 4b 段):

```bash
NDK=/path/to/android-ndk LIBRAW_JPEG=0 ./scripts/build-ffmpeg-android.sh
```

## 3. LibRaw 参数抽样(实现即契约 §10.4)

| dngq(UI) | half_size | user_qual | 说明 |
|---|---|---|---|
| 0 快速 | 1 | 0 | 半尺寸,最快 |
| 1 均衡(默认) | 0 | 2 | PPG |
| 2 精细 | 0 | 3 | AHD,最慢 |

| dngcolor(UI) | LibRaw |
|---|---|
| camera(默认) | `use_camera_wb=1` |
| auto | `use_auto_wb=1` |
| d65 | `user_mul[0..3]=1,1,1,1` |

公共(与档位无关):`output_bps=16`、`output_color=1`(sRGB)、
**`no_auto_bright=1`(必须显式关闭;LibRaw 对 half-size 源的默认亮度归一化
会改变曝光语义)**、`gamm[0]=gamm[1]=0`(不叠 gamma,TRC 由输出色空间决定)。

## 4. 尺寸 / 帧号 / 排序风险

* zip 里没有 fps:UI 必须让用户选(24/25/30/48/50/60,默认 25),
  `FpsMode.ORIG` 对 zip 不可用(契约 §10.6);
* 首帧解码成功即得 W×H 与 pixfmt(rgb48le),后续帧尺寸必须一致,
  不一致按 `AVERROR_INVALIDDATA` 明确报错,绝不静默跳帧;
* 自然序在「同宽度零填充」时退化为字典序;遇到混合宽度
  (`frame-2.dng` + `frame-10.dng`)时自然序仍正确,但 UI 提示排序依据。

### 4.1 条目命名兼容(2026-10-04 真机反馈「导入 zip 后无法解析」)

实测以外的手段(检索 MotionCam 生态项目)表明导出名并不统一:

| 条目名 | 匹配结果 |
|---|---|
| `frame-00000.dng`(连字符 5 位,历史约定) | 主规则命中 |
| `frame_000000.dng`(下划线 6 位) | 主规则命中(位数不写死) |
| `frame-1.dng`(最少 1 位) | 主规则命中 |
| `raw/frame-00001.dng`(带目录前缀) | 按 basename 命中 |
| `IMG_0001.dng` / `DSC_0001.DNG`(没有 `frame-*`) | 回退:所有 `.dng` 条目,仍按自然序 |
| `readme.txt` / `metadata.json` / `*.wav` | 忽略(非 `.dng`) |

规则(实现即契约 §10.2 的「条目匹配与回退」):命中过任意 `frame-*` 就只用这些条目,
避免把别的 `.dng`(封面图)混进序列;一个都没命中才回退全部 `.dng`。命中/回退的帧数
与 zip 前 8 个条目名都打 `av_log`,用户从「日志」分享里即可回传定位。

## 5. 待办 / 未决

* [x] ~~真机上导入那个「无法解析」的 zip,从日志里拿 `dngzip:` 诊断行确认真实条目名~~
      **已定位根因,无需真机日志**(2026-10-05):不是命名/结构问题,而是 **LP32 下
      `off_t` 只有 32 位**导致的大文件偏移截断。参考 zip 5,273,679,772 字节,
      中央目录绝对偏移 5,273,658,428 > 2^32,且 63 个条目的 ZIP64 extra 里
      `lho > 4GiB`(frame000274.dng 起)。`pread` 拿到 `offset & 0xFFFFFFFF`,
      读到的既不是本地头也不是数据 → 探测失败 → UI「无法解析」。
      arm64/x86_64 是 LP64,`off_t` 本来 64 位,所以只在 32 位设备复现
      (这解释了「本机 x86_64 模拟器 22/22 通过、真机仍失败」)。
      修复:`dngzip.cpp` / `transcode.cpp` / `native-lib.cpp` / `preview.cpp`
      的全部 IO 改调显式 64 位变体 `pread64`/`pwrite64`/`lseek64`
      (见 `dngzip.cpp:31-46` 的完整论证),并给 `DngZip::open()` 补了
      `lseek64` 长度兜底(上层 `FFmpeg.writeFd()` 的 `pfd.statSize` 失败时
      会传 -1L,原来会被直接 reject)。

      为什么不定义 `_FILE_OFFSET_BITS=64`:它会经 FFmpeg 头传播,改变
      `ff_AMediaCodec_queueInputBuffer` 的 `off_t` 形参 ABI
      (`media/NdkMediaCodec.h:267-286` 有 `static_assert` 专门卡这一点),
      并让 `libraw.a`(以 NDK 默认 32 位 off_t 编译)与本 so 的 off_t
      大小不一致。显式调 `*64` 不变 ABI、不改宏、零传染面。

      验证(三层):
      1. `llvm-objdump -d` 对比 armeabi-v7a:旧形状 `add r3,r4,r5`(无进位链,
         高位静默丢弃);新形状 `adds r0,...` + `adc r1,r8,#0` + `strd`(64 位
         偏移完整送达系统调用);
      2. `llvm-nm -D -u` 打产物 .so:两套 ABI 的 `pread`/`pwrite`/`lseek`
         (32 位)均为 0 个 UND,`pread64`/`pwrite64`/`lseek64` 各 1 个
         (fortify 形态 `__pread64_chk`/`__pwrite64_chk`);
      3. WSL 主机版 harness 跑真实 5.22 GiB zip:335 帧全读通、4GiB 边界帧
         (index 273/274)与尾帧(334)均正常、首帧 FAST 解码 2048×1536 成功、
         `len=-1` 经 lseek64 兜底后同样成功。
      产物:`app/build/outputs/apk/debug/app-debug.apk`(58,635,833 字节)。

* [ ] 若未来支持 MotionCam 的 `.mcraw`(raw 容器)或音频 zip,另立里程碑 M6,
      不在本契约范围内;
* [ ] 真机 3 帧对比(加固件 LibRaw+ffmpeg 同参数,diff < 1 LSB)在 M2 验证,
      结论写回本文件;
* [ ] **并行解码只在主机验到「吞吐 + 内存 + 队列语义」**,真机要确认两件事:
      ① 日志里 `transcode: dngzip 并行解码 335 帧:N 线程 / 预取深度 2` 里的 N 与预期
      (`(4096x3072)+sched_getaffinity 核数`)一致;② 转码全程 `dumpsys meminfo` 的
      Native Heap 峰值不超过 ~700MB(算法按 1GB 预算封顶 4 线程)。

## 6. 并行化实测(2026-10-05,主机 harness,样本 5.22 GiB)

动机:真机反馈「dng 序列转码吃不满 threads,arm 设备只能看到单核在跑」。

**耗时拆解**(4096x3072 Apple Log,BALANCED):read 0.056s + decode 0.351s = **0.407s/帧**,
解码占 **86%** —— 串行时主线程被 LibRaw 独占,第二个核只分到滤镜/编码那 14%。

| 配置 | 每帧耗时 | 峰值内存(VmHWM) |
|---|---|---|
| 1 线程(depth 2 基线) | 425 ms | 213 MB |
| 2 线程 / depth 1 | 424 ms(**没重叠,别用**) | 253 MB |
| 2 线程 / depth 2 | 245 ms(1.73×) | 420 MB |
| 3 线程 / depth 2 | 247 ms | 460 MB |
| 4 线程 / depth 2 | 240 ms | 490 MB |
| 6 线程 / depth 2 | 257 ms | 567 MB |

**纯 LibRaw 解码**(同一份内存帧,排除读盘)扩展性明显更好:
368ms(1T) → 212ms(2T,1.74×) → 124ms(4T,2.96×) → 117ms(8T,3.15×)。
也就是说上表的「到 2 线程就平了」是主机 WSL 9p 读盘串行化造成的假天花板;
真机是本地 fd 直读,应更接近纯解码曲线 —— 这正是 `dngthreads` 默认自动的理由。

**栈**:同一帧解码的最小栈实测 **768KB → SIGSEGV、850KB → 跑通**(`dng_decode_to_rgb48`
里 `LibRaw lr;` 一个对象就 `sizeof` = 767,320)。Android bionic 给新建原生线程的默认栈
只有 1MB,只剩 ~15% 余量 → `dngframeq.h` 用 pthread 显式 **8MB 栈**。

**队列语义单测**(`~/dzwork/harness/src/test_frameq.cpp`,TSan 全绿):300 帧顺序交付无错位 /
3 线程 >1.6× / 第 7 帧失败→消费端停在 7 且错误原样带回 / 失败态再次 next 不死等 /
cancel 立即生效 / total=3 depth=16 边界 / 不调 stop 直接析构不卡死。
