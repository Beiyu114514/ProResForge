# PreResForge —— 代码审查记录与修复清单

> 本文档记录 2026-10-04 对 ProResForge 全仓代码的审查结论,以及本轮已完成的修复。
> 编号 S/M/L = 严重 / 中等 / 轻微;括号内为源位置(修复后行号可能有偏移)。
> **修复状态:严重(S1-S4)/ 中等(M5-M10)/ 轻微(L11-L23)全部已落地;P3 单测已起步
> (FormatCatalog + HistoryItem 序列化两个纯 JVM 测试类)。已完成编译 + 单元测试 +
> x86_64 模拟器冒烟验证(见文末「编译与验证情况」);真机 native 转码仍未验证——
> native 改动(M5/L17/L18/L19)需要用 arm64 真机跑一次 assembleDebug + 端到端转码。**

## 严重(必须修)

### S1 重复入队:两次点击 → 同一批任务入队两遍

- 位置:`ui/HomeViewModel.kt:319-380` `startTranscode()`
- 原因:`busy` 的判定在协程外、置位在 `viewModelScope.launch(Dispatchers.IO)` 执行体内。
  两次点击间隔小于一个 IO 线程派发延迟时,两帧都读到 `busy=false` → 同一个 Snapshot 各入队一次。
- 后果:输出文件被追加 " (2)",时间与存储翻倍。
- 修复:引入 `AtomicBoolean(enqueueGuard)`,`compareAndSet(false,true)` 与判定原子完成;
  `busy=true` 同步置(不等协程启动);`try { } finally { enqueueGuard.set(false) }` 保证复位。
  另加 `preflightError()` 预检:未授权输出目录 / 探测失败的文件直接挡住并给可操作文案。

### S2 服务被杀后 native 转码仍在跑,但收尾(通知/历史/fd)全部丢失

- 位置:`engine/TranscodeService.kt:100-118` `onDestroy()`
- 原因:`onDestroy()` 只有 `serviceScope.cancel()`。协程取消对阻塞 JNI (`nativeRun`) 无效,
  服务销毁后 FFmpeg 继续写输出文件,而 `JobBus.finish` / `JobStore` / 通知均不再执行,
  wakeLock 也被提前释放。
- 修复:`onDestroy()` 先 `FFmpeg.nativeCancel()`(若 `JobBus.currentId.value != null`)再 cancel scope。

### S3 JobStore 双实例 → 互相覆盖 / 丢更新

- 位置:`data/JobStore.kt:40-60`、`ui/HistoryScreen.kt:52`
- 原因:Service 与 UI 各 `JobStore(context)` 一个实例,每个实例各自持 `Mutex` 写同一
  `filesDir/jobs.json`,进程内两把不同的锁 → 互相覆盖(经典双实例单文件问题)。
- 修复:构造私有化 + `companion object get(context)` 双检锁单例;
  `writeLocked()` 删掉 `rename` 前的 `file.delete()`(POSIX rename 本身原子替换,删了反而放大崩溃窗口)。

### S4 未授权输出目录时 UI 仍显示默认目录名(「Movies/ProResForge」)

- 位置:`ui/HomeScreen.kt:378-392`、`data/OutputDir.kt:41`、`ui/HomeViewModel.kt:310-316`
- 原因:`outputDirName == null` 被当成 `R.string.output_dir_default` 展示,实际 SAF 树未授权,
  首启必败且无引导。
- 修复:FactRow 的 `value` 改为 `state.outputDirName ?: R.string.output_dir_unset`,
  `warn = treeMissing`,按钮文案随之切换(pick/change);
  `OutputDir.initialTreeUri()` 返回可空 URI:先按 MediaStore.Video 查询真实存在的
  `Movies/ProResForge` 目录树(不存在则返回 null,让系统选择器自行处理)。

## 中等

### M5 `av_opt_set(...,"bitrate_mode","vbr",0)` 字符串写 INT 选项

- 位置:`cpp/transcode.cpp:1159`
- 原因:`bitrate_mode` 是 AV_OPT_TYPE_INT;若无 `"vbr"` 命名常量,`av_opt_set` 返回负值并被忽略,
  编码器退回默认 CQ 模式,`bit_rate` 形同虚设。
- 修复:`av_opt_set_int(enc->priv_data, "bitrate_mode", 1 /* VBR */, 0)` 并检查返回值,
  失败时 `AV_LOG_WARNING`(不 fatal)。

### M6 native 抛 Throwable 时 in/out fd 泄漏

- 位置:`engine/TranscodeService.kt:220-240`
- 原因:`catch (t: Throwable)` 只打日志;`inFd`(已 detach,归 native)与 `outFd` 无人 close。
- 修复:异常分支补 `closeDetachedFd(...)`(与 `catch (e: Exception)` 分支的既有写法一致)。

### M7 LUT 强度滑块一拖一帧 → 每档两次 seek+解码(+LUT)

- 位置:`ui/HomeScreen.kt:96-119`
- 原因:`LaunchedEffect(previewKey, state.lutIntensity)`,steps=19,拖动时每档全量重取前后两图。
- 修复:`beforeBitmap == null` 时才取两张;之后只重取后图;`previewAfter(intensity)` 支持外部传值。
  `preview()` 去掉 `runBlocking`(调用方已在 IO 线程),`@Throws` 语义保留在文档注释。

### M8 大量硬编码中文 → 无法切换英文、且无法复用

- 位置:`ui/HomeScreen.kt`、`ui/JobsScreen.kt`、`ui/HistoryScreen.kt`、`data/model.kt`
- 修复:`OutFormat` / `ScaleMode` / `FpsMode` 的 `label: String` 全部改为 `labelRes: Int`
  (`fmt_*` / `scale_*` / `fps_*`);三页全部外提到 `res/values/strings.xml` +
  `res/values-en/strings.xml`(59+ 个新增/恢复键,双语一一对应);
  数值类文案统一 `"%.2f fps".format(Locale.US, ...)`。

### M9 `POST_NOTIFICATIONS` 每次旋转/重建都重弹

- 位置:`MainActivity.kt:44-70`
- 原因:`LaunchedEffect(Unit)` 无 guard。
- 修复:进程内 `AskedFlag`(`@Volatile` 布尔),只问一次;用户拒绝后不再骚扰。

### M10 入队前无预检

- 位置:`ui/HomeViewModel.kt:306-316, 319-347`
- 修复:`preflightError()` 返回 `err_no_output_dir` / `err_unprobeable`;
  `HomeUiState.canStart` 把「探测结束但 facts==null」的文件算为不可开始,按钮直接禁用。

## 轻微

### L11 fitLongEdge / P50/P25 极短边算出 0 尺寸

- 位置:`engine/FFmpeg.kt:262-272`
- 修复:`fitLongEdge` 两侧都 `coerceAtLeast(2)`;`even(v) = v<=0 ? 0 : v-and 1`(0 仍表示「源尺寸」);
  P50/P25 补 `coerceAtLeast(2)`。

### L12 runQueue 二次 QUEUED 确认死代码

- 位置:`engine/TranscodeService.kt:215-230`(已删)

### L13 通知小图标复用启动图标 / notify 静默失败

- 位置:`engine/Notifications.kt:69,92,128,139-152`
- 修复:新建 `res/drawable/ic_stat_transcode.xml`(白色 24dp 胶片+播放键)三处替换;
  `notify()` 的 catch 改为 `AppLog.w(TAG, ...)`(含 SecurityException 分支)。

### L14 LUT 头解析依赖固定键长 + 并发 tmp 竞态

- 位置:`data/LutFile.kt:60-120, 158-190`
- 修复:`parseHeader` 按空白切 key/value(`takeWhile{!it.isWhitespace()}`)
  替代 `drop(11)` / `substring(6)`;`stage()` 删掉 rename 前的 `dest.delete()`;
  tmp 名改 `${dest.name}.${System.nanoTime()}.tmp`;KDoc 注明契约 §8.2 的 `.dat/.sha1` 与实现不一致。

### L15 resolveName ↔ createDocument 之间 TOCTOU;take(120) 可能切断代理对

- 位置:`data/OutputDir.kt:100-170`
- 修复:进程内 `pendingNames` 与 `resolveName` 求并(先吃自带改名);`outputUriFor` 返回
  `OutputDoc(uri, name)(name 取 provider 实际采用名)`;`fileName` 上限 `MAX_BASE_CHARS = 60`
  (120 CJK = 360B 会 ENAMETOOLONG)且新增 `String.truncateSafe(n)` 剔除尾随高代理。

### L16 枚举按 name 序列化 → 改名即旧历史反序列化失败

- 位置:`data/model.kt:40-70`
- 修复:KDoc 注明该风险(需配 `@SerialName` 或迁移),暂无数据迁移需求故不改序列化格式。

### L17 native 若干正确性细节

- 位置:`cpp/transcode.cpp:651,662-668,1159,1279,1491`
- 修复:`Pipe::last_pts` 初值 `AV_NOPTS_VALUE`(旧代码首帧 pts=0 会被兜底改成 1);
  AAC `ac->sample_fmts[0]` 补空列表保护;音频直通 bsf send 失败不再静默;
  删除死变量 `bool loop_error`。

### L18 jni_progress 全程持锁回调 Kotlin → 死锁风险

- 位置:`cpp/native-lib.cpp:200-260`
- 修复:只从 `g_progress`/`g_progress_method` 快照后即出锁再回调;
  新增 `std::atomic<int> g_cb_inflight` + `std::atomic<bool> g_cb_drop_pending`;
  最后一位回调负责 `DeleteGlobalRef`。Kotlin 侧阻塞/再入 native 均不会死锁。

### L19 预览路径未 autorotate / 未 flush / 未 drain / thread_count 注释错

- 位置:`cpp/preview.cpp`
- 修复:`dec->thread_count = 1`(0 在 FFmpeg 语义里是「自动」,与注释相悖);
  seek 后 `avcodec_flush_buffers(dec)`;EOF `avcodec_send_packet(dec,nullptr)` 冲刷并 drain;
  新增 `int rot_deg = detect_rotation_deg(vst,"auto")`,按旋转交换宽高再比 `max_w`,
  描述串按 `transpose=clock,` / `transpose=cclock,` / `hflip,vflip,` 前置(与转码 rotate=auto 同序)。

### L20 CMakeLists 注释自相矛盾

- 位置:`cpp/CMakeLists.txt:27`
- 修复:改为「3 个 .cpp」表述。

### L21 转码 tab 旋转后弹回

- 位置:`ui/AppNav.kt:36` → `rememberSaveable`

### L22 各类 UI 细节

- 位置:`ui/HomeScreen.kt:89-94`、`ui/components.kt:183`
- 修复:Snackbar 改由 `LaunchedEffect(state.messageSeq)` 驱动(同文案也能再弹);
  `HomeViewModel.postMessage()` 带原子序号;`ProgressRow` 的 `"…"` → `stringResource`;
  预览区删除多余的本地 `OutputDir` 实例(与 `HomeScreen` 里已删的 `FormatCatalog` 误删一起修正);
  `queryNameSize` 兜底名走字符串资源。

### L23 文档夸大 / 缺 .gitignore

- 位置:`README.md:23,134-136`、`app/src/main/cpp/prebuilt/README.md:40`
- 修复:删除「队列断点续传」表述,改为「队列可取消」并显式声明队列仅存内存;
  补仓库根 `.gitignore`(prebuilt 产物、build 目录、local.properties、.gradle、签名文件)。

## 测试现状(P3,进行中)

- 仓库已有 gradle wrapper(Gradle 8.9,与 AGP 8.7.2 匹配);`app/src/test` 现已新增
  `junit` 4.13.2 依赖(`testImplementation`,纯 JVM,不牵动 NDK/设备构建)。
- 已落地的纯 JVM 单测:
  - `app/src/test/java/com/proresforge/app/data/FormatCatalogTest.kt`
    锁住契约 §8.1/§3.2 的参数表:四个格式的顺序、preferred vcodec、pixFmt(ProRes 恒 yuv422p10le)、
    supports10Bit 只对 H265 开、CRF/CQ 三档各自单调且「一个格式只用一套画质方案」、
    ProRes 三档必须全 0、audioModes 恒含 COPY+AAC 且不含 DROP、labelRes 互不相同、ext/mime 自洽。
  - `app/src/test/java/com/proresforge/app/data/HistoryItemSerializationTest.kt`
    锁住 jobs.json 线格式(枚举按 **name** 而非 ordinal 序列化),逐 OutFormat × JobState round-trip,
    含 null 可选字段;谁改枚举名/顺序,这里先红(L16 的回归护栏)。
- 仍待补:`OutputDir.fileName` / `resolveName` 去重与 `LutFile.parseHeader`(需要 Robolectric 或
  把纯函数抽成无 Context 的工具对象后再测);C 端 `strspec.h` 可用 gtest 覆盖。

## 编译与验证情况

### 环境

- NDK `27.0.12077973`(与 `app/build.gradle.kts` 的 `ndkVersion` 一致)、CMake `3.22.1`、
  build-tools `35.0.0`、platforms `android-35`;JDK 21(Microsoft Build of OpenJDK 21.0.9)。
- 仓库在 UNC 共享上时 PowerShell 直跑 Gradle 会踩沙箱/UNC 兼容问题,稳妥做法是先
  `robocopy` 一份到本地盘(排除 `.git`/`build`/`.gradle`/`local.properties`)再构建。

### 编译暴露并已修掉的 3 个问题

1. `cpp/transcode.cpp:1608 error: use of undeclared identifier 'loop_error'` —— L17 删
   `bool loop_error` 死变量时漏了 `goto loop_error` 标签体里的 `loop_error = true;`
   (共 7 处 goto)。标签现在只剩注释与 `if (result == 0) result = ret;`。
2. `data/OutputDir.kt:77` 把 `MediaStore.Video.Media.EXTERNAL_CONTENT_URI`(Uri)传给了
   只收 authority 字符串的 `DocumentsContract.buildTreeDocumentUri` → 改为显式
   `"com.android.externalstorage.documents"` 常量。
3. `ui/HomeViewModel.kt:301` L14 去掉 `runBlocking` 后,`preview()` 直接裸调
   `suspend fun previewJpeg(...)` → `preview()` 改为 suspend / 在协程内调用。

编译后 `app/build/outputs/apk/debug/app-debug.apk` 约 57 MB(3 个 ABI:arm64-v8a /
armeabi-v7a 为真 FFmpeg prebuilt,x86_64 为上面的桩)。仅剩 1 条 `av_stream_get_side_data`
弃用警告(FFmpeg 6.1 建议改用 `av_packet_side_data_get`,但 prebuilt 头里没有该 API,
保留不动)。

### 单元测试

`:app:testDebugUnitTest` 全绿:`FormatCatalogTest` 12 个、`HistoryItemSerializationTest` 4 个,
0 failures / 0 errors / 0 skipped。

### x86_64 模拟器冒烟(UI/Kotlin 全链路)

`app/build.gradle.kts` 的 `abiFilters` 只有 `arm64-v8a`/`armeabi-v7a`,x86_64 镜像里
真 FFmpeg `.so` 装不上,所以做了一次「JNI 桩 + x86_64 镜像」的冒烟:

- 桩 = `cpp/stub_native_x86.cpp`(仅本地副本,不入库):`nativeRun` 按 init→decode→audio→
  finalize 五步 `emit()` 进度后 sleep 返回 0;读 `persist.pf.stub.fail` 属性决定是否返回
  `RC_UNSUPPORTED(2)` 走失败分支;`nativePreview` 返回一张内置 JPEG;native 侧负责
  `close(inFd)`/`close(outFd)`(契约 §2 的 fd 所有权)。
- `cpp/CMakeLists.txt` 里 `if(ANDROID_ABI STREQUAL "x86_64")` 早退分支只编桩,
  `abiFilters` 追加 `x86_64`,装 `system-images;android-35;google_apis;x86_64` 的 AVD。
- 因为模拟器没网/没 arm 转码,这一轮只验证 **Kotlin + UI + 进程生命周期**,已在模拟器上
  逐条跑通的路径:
  1. 首启 `POST_NOTIFICATIONS` 弹窗(M9 一次性守卫生效),英文 locale 走 `values-en`;
  2. DocumentsUI 选文件(网格 + 搜索),MediaProbe facts 全字段渲染(含桩返回的 `rot=90`);
  3. **S4/M10**:未授权输出目录点 Start → Snackbar "Select an output folder first",不入队;
     授权 Movies 树后 "Current folder: Movies"、按钮变 "Change folder"(L15 初始目录定位生效);
  4. 入队 → Jobs 页进度(阶段 + 百分比 + facts)→ DONE → 通知 `Done: sample.mp4`
     (L13 新小图标 `ic_stat_transcode` 在通知栏生效);
  5. **S3**:History 页立即出条目,不再先闪「暂无历史记录」;`files/jobs.json` 每条含
     `lutName/lutIntensity/outputUriString`;
  6. **失败分支**:`persist.pf.stub.fail=1` → `nativeRun rc=2` → `job end state=FAILED
     err=Input unsupported or not decodable`,Jobs 页显示错误行,通知
     `Failed: sample.mp4`(category=err);
  7. **LUT 全链路**:选 `smoke.cube`(TITLE/LUT_3D_SIZE)→ `LUT 暂存: PF smoke size=2`
     (L14 解析修好)→ UI File/Size → 强度滑条 15% → `lut_intensity=0.15` 即时落盘 →
     带 LUT 转码 → History `LUT: PF smoke · 15%`;
  8. **重启恢复**:force-stop 后重进,输出目录/LUT/格式全部恢复,无文件时预览瓦片显示
     "No preview";history 仍在;
  9. 分享:History 行 Share 与 Logs 分享均能拉起系统分享(FileProvider 正常)。
- **残余风险(S1 之外的更高要求,需产品决策)**:`enqueueGuard` 只挡住「同一帧内」的第二次点击;
  间隔 >~250ms 的再次点击会合法再入队一份(输出被 `OutputDir.resolveName` 去重成
  ` (2)`/` (3)`)。若希望「转码期间禁止再入队」,需要把 busy 生命周期延伸到 JobBus 队列清空,
  这会改变现有交互语义,故未动。

### 仍未验证(原有项)

- 真机(arm64)上的 FFmpeg 6.1 真实转码:`lut3d` 滤镜链、`bitrate_mode`(M5)、
  pts 单调化(L17)、音频直通 bsf(L17)、预览 autorotate/flush/drain(L19)、
  `nativeCancel` 中断(S2)、fd 泄漏(M6)。这些都需要 `install -r` 到 arm64 真机跑一遍。

### DNG 序列 zip(契约 §10,本轮新增)

实现范围(M1–M4):

| 层 | 文件 | 内容 |
|---|---|---|
| 构建 | `scripts/build-ffmpeg-android.sh` | 4b 段新增 LibRaw 0.21.4(LibRaw.git 源码 + LibRaw-cmake 脚本,NDK toolchain,`c++_static`,静态 `libraw.a`);依赖 NDK sysroot zlib;`LIBRAW_JPEG=ON` 才编 libjpeg-turbo(默认 OFF);§5 校验加 `raw`;幂等(有 `libraw.a` 就 skip) |
| CMake | `app/src/main/cpp/CMakeLists.txt` | `NATIVE_SOURCES` 加 `dngzip.cpp`;新增 `IMPORTED STATIC ffmpeg-raw`(缺 `libraw.a`/`libraw.h` 时 `FATAL_ERROR`),链接 `z` |
| native | `dngzip.h` / `dngzip.cpp`(新) | 手写 ZIP(中央目录 + ZIP64 + raw inflate + 自然序排序,零临时文件/零整包 mmap);`dng_decode_to_rgb48`(LibRaw → rgb48le,档位表见 §10.4) |
| native | `transcode.cpp` | `src=dngzip` 分支:跳过 avformat,首帧定尺寸,buffer `rgb48le`/`1/fps`/`sar=1`,每帧 `read_frame → dng_decode_to_rgb48 → pts=i → push_decoded`,复用既有 drain_graph/encode_out/faststart/进度/取消;不静默跳帧 |
| native | `preview.cpp` | 入口探 `PK\x03\x04`;`atUs` 语义变帧号;FAST 档解单帧后复用既有缩放/mjpeg 流程;JNI 签名不变 |
| native | `native-lib.cpp` | `collect_media_info` 识别 zip:`codec=dng`/`container=zip`/`pixfmt=rgb48le`/`frames=N`;非 DNG zip 返回空串(既有「不可解析」终态) |
| Kotlin | `model.kt`/`MediaProbe.kt`/`FFmpeg.kt` | `MediaFacts.frames` + `isDngZip`;`FpsMode` 加 F25/F48/F50 + `standard`;新增 `DngQuality`/`DngColorMode`;`buildSpec` 追加 `src/dngq/dngcolor` |
| Kotlin | `HomeViewModel.kt`/`HomeScreen.kt`/`JobStore.kt`/`SettingsStore.kt` | picker mime 加 zip;PROBEABLE_EXTENSIONS 加 `zip`;zip 专属 fps(24–60,默认 25)/质量/白平衡三组 chips;帧数·尺寸·耗时预估 + 大尺寸警告;音频卡对 zip 隐藏;预览/fps estimate |
| 测试 | `app/src/test/.../DngSpecMappingTest.kt`(新) | 钉住 `dngq` 序号(0/1/2)、`dngcolor` 字符串(camera/auto/d65)、`FpsMode.standard`/zip 档位不变量 |

### 编译与验证情况(DNG zip 专项,2026-10-04)

**环境**:本机有 Android SDK(`C:\Users\Administrator\AppData\Local\Android\Sdk`,NDK
27.0.12077973 + CMake 3.22.1 自带 ninja)、JDK 21.0.9、git、python 3.12。
仓库在 UNC 共享上,PowerShell 直接跑 Gradle 会踩 UNC 兼容问题,故
`robocopy` 到 `C:\pfbuild\ProResForge` 再构建(`local.properties` 的 `sdk.dir`
指向本机 SDK)。

**LibRaw 真编出来了(不是占位)**:cmake + NDK toolchain,`-DANDROID_PLATFORM=android-24
-DANDROID_STL=c++_static -DBUILD_SHARED_LIBS=OFF -DCMAKE_DISABLE_FIND_PACKAGE_JPEG=ON
-DCMAKE_DISABLE_FIND_PACKAGE_Jasper=ON -DCMAKE_DISABLE_FIND_PACKAGE_OpenMP=ON
-DENABLE_{EXAMPLES,OPENMP,LCMS,RAWSPEED,JASPER,X3FTOOLS,6BY9RPI}=OFF -DLIBRAW_PATH=<源码>`,
arm64 `libraw.a` 8,482,312 B(aarch64)、armeabi-v7a 5,975,616 B(arm),头文件
`include/libraw/` 9 个,已拷进 `prebuilt/<abi>/`。LibRaw-cmake 没有 `ENABLE_JPEG`
选项,JPEG 只能用 `CMAKE_DISABLE_FIND_PACKAGE_JPEG` 关。

**编译期发现并修掉的 3 个错误**(均已落盘):

1. `cpp/preview.cpp:166 error: use of undeclared label 'cleanup'` —— 插 `zip_ready:`
   标签时吃掉了一个 `{`,在原 decode block 与其闭合 `}` 之间补回,括号深度归零。
2. `cpp/dngzip.cpp`:`extern "C" { #include <libraw/libraw.h> }` 会把 libc++ 的
   `<fstream>/<filesystem>` 也变成 C 链接,报一串
   `functions that differ only in their return type cannot be overloaded`
   (perms.h vs copy_options.h)。LibRaw 是 C++ 库,**必须按 C++ 包含,不能包
   extern "C"**。
3. `cpp/dngzip.cpp` 重复定义 `frame_count()` / `entry()`(头文件里已 inline)→ 删。

**`:app:assembleDebug` BUILD SUCCESSFUL**(arm64-v8a + armeabi-v7a 双 ABI,
仅剩 `transcode.cpp:254 av_stream_get_side_data` 弃用警告 1 条),产物
`dist/ProResForge-1.0.0-debug-dngzip.apk` 58,619,449 B。

**符号验证**(arm64 obj 的 `libffmpeg-transcode.so`):`DngZip::open/read_frame/is_zip`、
`dng_decode_to_rgb48` 全部在;`LibRaw::` 符号 469 个(静态链进去了);5 个 JNI 入口
`Java_com_proresforge_app_engine_FFmpeg_{nativeRun,nativeCancel,nativeInfo,nativePreview,initNativeLogging}`
与契约 §2 逐字一致。

**`:app:testDebugUnitTest` BUILD SUCCESSFUL**,XML 汇总:
`DngSpecMappingTest` 4/4、`FormatCatalogTest` 12/12、`HistoryItemSerializationTest` 4/4,
0 failures / 0 errors / 0 skipped(共 20 个)。

**设备端 native 自测(x86_64 模拟器,`adb shell` 真跑)**:arm64 镜像在这台 x86_64 主机上
跑不了(QEMU2 明确不支持 arm64 镜像),改用 x86_64 镜像 AVD `medium_phone`;把
`dngzip.cpp` 单独编成可执行文件(链接 x86_64 版 libraw.a + NDK sysroot zlib)推上去跑。
两个测试二进制与合成 DNG 生成器都在本机会话目录(`C:\pfbuild\...`),**未进仓库**
(契约 §1 只允许 CMake 引用那 5 个文件名)。注意:直接链接的可执行文件依赖
`libc++_shared.so`,要一并 push 并设 `LD_LIBRARY_PATH`。

| 自测 | 用例 | 结果 |
|---|---|---|
| `dngzip_test`(ZIP 解析) | 32 帧 stored+deflate 混排 + 无关 extra + 64B 注释 / 混合零填充自然序 / ZIP64 / 非 DNG 过滤 / 截断报错 / 越界下标 | **15/15 通过** |
| `dng_decode_test`(真 DNG 解码) | 8 帧 BGGR CFA(64x64 16bit,Compression=1,Photometric=32803,ColorMatrix1,AsShotNeutral=1,1,1) | **17/17 通过** |

解码侧关键观测(与契约 §10.4 完全对上):

- FINE(AHD)→ 64x64;BALANCED(PPG)→ 64x64;**FAST(half_size)→ 32x32**(半尺寸档位真的生效);
- 源 CFA 的 R:B = 3:1,经相机 WB + 色彩矩阵后输出 spread = 0.002(被拉平,说明
  `use_camera_wb` + `output_color=1` + `ColorMatrix1` 整条都走到了);
- 横向 0→65535 渐变原样保留(左半均值 20630 vs 右半 51614),说明 demosaic
  真的跑了而不是常量填充;帧 0 与帧 3 内容不同(不是同帧循环);
- AUTO / D65 白平衡档、以及**垃圾字节输入明确返回错误不崩**。

### 仍未验证(需要真 arm64 机)

- 真机(arm64)上完整跑一次:900 帧 zip 的耗时/内存(契约 §10.7:1080p60 < 20 min、
  4K < 90 min、≤400MB、无 OOM)、取消、与桌面 LibRaw+ffmpeg 的 PSNR ≥ 40dB;
- APK 安装后的 UI 链路(picker zip → 探测 → 参数卡 → 转码)本机无 arm 设备,只验到
  native 层;x86_64 只有 `.so`、没有对应 ABI 的应用包;
- 既有 .mov/.mxf 回归用例本次未跑(§10.7 要求不改动地全过);
- `strspec.h` 增补 3 个键(src/dngq/dngcolor)只做了编译 + 运行验证,无 gtest 覆盖;
- `scripts/build-ffmpeg-android.sh` 是 Linux 脚本,本机 `bash -n` 与端到端未跑;
  LibRaw 段是手工按脚本同款参数用 cmake 复现的。

### 修复「导入 zip 后无法解析」(m00934,2026-10-04)

**现象**: 设备上导入 MotionCam 导出的 DNG 序列 zip,App 显示「无法解析」。

**定位**: Kotlin→native 链路本身没断(`HomeViewModel.onFilesPicked` → `MediaProbe.probe`
→ `FFmpeg.writeFd` → `nativeInfo` → `collect_media_info` zip 分支 → `parseInfo` 失败态),
问题在 zip 分支内部两个「无声拒绝」:

1. `is_dng_frame_name` 写死 `frame-` + 恰好 5 位数字(整名 15 字符)。不同导出工具/版本
   用的是 `frame_000000.dng`(下划线 6 位)之类 → 0 帧,而 `open()` 仍返回成功
   (`frame_count()==0`)→ native 返回空串 → UI 「不可解析」。
2. 探测失败路径(open 失败 / 0 帧 / 首帧解不开)**一条日志都不打**,用户无法反馈原因,
   开发无法远程定位。

**修复**(`dngzip.cpp` + `native-lib.cpp` + `preview.cpp`,契约 §10.2 已同步):
- 帧名主规则放宽为 `^frame[_-]?\d+\.dng$`(大小写不敏感、位数不写死、按 basename 判定);
- 一个 `frame-*` 都没命中时回退成「zip 内所有 `.dng` 条目」(仍自然序);
  命中过 `frame-*` 就只用它们,不掺别的 `.dng`;
- 全链路 `av_log` 诊断:open 失败、0 帧(附前 8 个条目名)、命中帧数、回退帧数、
  单帧解压失败(带帧号)都打印,经 `pf_av_log` 落 logcat + app.log;
- 探测段/预览段失败也有明确日志,不再是静默空串。

**编译期新增踩坑(已修)**: `libavutil/log.h` **没有 `extern "C"` 保护**(FFmpeg 的
libavutil 头是例外),C++ 里直接 include 会把 `av_log` 链成 C++ 符号 →
`ld.lld: undefined symbol: av_log(void*, int, char const*, ...)`。dngzip.cpp /
preview.cpp 都改用 `extern "C" { #include <libavutil/log.h> }` 包
(libraw/libraw.h 反之不能包,它是 C++ 库)。

**验证结果**:
- `dngzip_test`(x86_64 模拟器 `adb shell` 实跑): **22/22 通过**(原 15/15;新增:
  下划线 6 位命名识别成 6 帧、无 `frame-*` 时回退 3 个 `.dng`(大小写不敏感)、
  命中 `frame-*` 时只用 2 帧不掺 `IMG_*`、带目录前缀 + 1 位数字也认、非 DNG zip 0 帧);
- `dng_decode_test`:**17/17 通过**(FINE 64x64 / BALANCED 64x64 / FAST 32x32、
  AUTO/D65 WB、R/B 3:1 拉平到 spread 0.002、左右渐变、帧间差异、垃圾输入报错);
- `:app:testDebugUnitTest`:20/20(DngSpecMappingTest 4 + FormatCatalogTest 12 +
  HistoryItemSerializationTest 4);
- `:app:assembleDebug` **BUILD SUCCESSFUL**,arm64 `libffmpeg-transcode.so`
  6,420,176 B,UTF-8 串检查确认新诊断串已进 .so(「回退成 zip 内全部 .dng」等);
  APK 60,454,945 B → `dist/ProResForge-1.0.0-debug-dngzipfix.apk`。

**仍未验证(需真机)**: 用户手里那个 MotionCam zip 的实际条目名/压缩方式;900 帧耗时/内存;
取消;`.mov/.mxf` 老路径回归。复测后请把「日志」分享内容发回来,里面应有 `dngzip:` 开头的
诊断行(命中/回退/帧数,或前 8 个条目名)。

### DNG 转码并行化(m00836,2026-10-05)

**现象**: dng 序列转码吃不满线程,arm 设备只能看到单核在跑。

**定位**: 不是编码器/滤镜不并行,而是 **DNG zip 分支本身是串行的单线程循环** ——
`read_frame + dng_decode_to_rgb48` 全在主线程,实测解码占整条链 86%
(4096x3072 BALANCED:read 0.056s + decode 0.351s = 0.407s/帧)。主线程被 LibRaw 独占,
第二个核只拿到滤镜/编码/写盘那 14%。

**修复**(新增 `app/src/main/cpp/dngframeq.h`,改 `transcode.cpp` / `strspec.h`):
- 新增 `DngFrameQueue`:N 条工作线程并行 `read_frame + dng_decode_to_rgb48`,
  主线程按帧号顺序 `next()` 取帧 → 滤镜图 → 编码 → 写盘。**帧号严格有序**,
  输出与串行版逐字节等价(pts/进度/写盘顺序不变)。
- 环形槽位(深度 = depth);空位判定用「未完成帧数 `assigned-consumed < depth`」,
  解码中的帧也占槽;depth 固定 2(depth=1 实测 424ms,与串行 433ms 无差别 ——
  帧 i+1 必须等帧 i 被取走才开始解,没有重叠)。
- 失败/取消语义与串行版一致:任一帧失败 → `next()` 立刻 -1 → `AVERROR_INVALIDDATA` + 帧号
  (**不静默跳帧**);`cancel()` 唤醒两侧 cv,`stop()` 先 cancel 再 join,**不存在忘 cancel
  就 join 卡死**的路径;析构也调 stop。
- 线程数按内存预算反推(1GB / 单帧峰值 ≈ rgb48le 字节数 × 2.95 + 16MB),封顶 4 且
  ≤ cores-1(给主线程留一个核);spec 新增 `dngthreads`/`dngdepth`(默认 0=自动)可强制覆盖。
- **线程栈必须显式 8MB**:Android 新建原生线程默认栈只有 1MB
  (bionic `PTHREAD_STACK_SIZE_DEFAULT`=1MB),而 `dng_decode_to_rgb48()` 里
  `LibRaw lr;` 实测 `sizeof` = 767,320;同一帧解码实测 **768KB 栈 SIGSEGV、850KB 才跑通**
  → 1MB 只剩 ~15% 余量,这是只有真机才炸的段错误。因此用 pthread 而非 std::thread。

**实测(主机 x86_64,4096x3072,335 帧样本)**:

| 配置 | 每帧耗时 | 峰值内存 |
|---|---|---|
| 1 线程基线 | 425 ms | 213 MB |
| 2 线程 / depth 1 | 424 ms(无重叠) | 253 MB |
| 2 线程 / depth 2 | 245 ms(1.73×) | 420 MB |
| 3 线程 / depth 2 | 247 ms | 460 MB |
| 4 线程 / depth 2 | 240 ms | 490 MB |

纯 LibRaw 解码(排除 IO)扩展性更好:368ms(1T) → 212ms(2T) → 124ms(4T) → 117ms(8T),
即上表「2 线程后持平」是 WSL 9p 读盘串行化造成的假天花板,真机本地 fd 应接近纯解码曲线。

**buffer 复用(实现细节,别改回 move)**:槽位里 `raw`(15.7MB)/`rgb`(72MB)的 capacity 跨轮
复用,`next()` 用 **swap** 把 buffer 还给槽位;335 帧少 670 次大块 mmap/munmap,峰值内存更稳。
`dng_decode_to_rgb48()` 内部只 `clear()`+`resize()`,本来就承诺不缩容量,正好接上。

**验证**:
- 队列单测 `test_frameq.cpp`(host,不依赖 FFmpeg/LibRaw):**全部通过** —— 300 帧顺序交付
  无错位 / 3 线程 >1.6× / 第 7 帧失败→消费端停在 7 且错误原样带回 / 失败态再次 next 不死等 /
  cancel 立即生效 / total=3 depth=16 边界 / 不调 stop 直接析构不卡死;ThreadSanitizer 全绿。
- 真实 5.22 GiB zip 主机 harness:335 帧全通,4096x3072 rgb48le,3T/depth2 = 262ms/帧。
- NDK 直编 `transcode.cpp`:arm64 / armeabi-v7a / x86_64 三 ABI 均 COMPILE 通过;
- 产物 .so 两 ABI:`pthread_create`/`pthread_join`/`pthread_attr_setstacksize`/
  `sched_getaffinity`/`sysconf` UND 各 1,`dngthreads`/`dngdepth` 诊断串在位;
  `pread64`/`pwrite64`/`lseek64` 仍各 1(off_t 修复未被破坏)。
- `:app:assembleDebug` **BUILD SUCCESSFUL**(全 39 task)。

**仍未验证(需真机)**: ① 真机多核上的实际加速比(主机 9p 低估了它);② 日志
`transcode: dngzip 并行解码 335 帧:N 线程 / 预取深度 2` 里的 N 与
`(4096x3072 + sched_getaffinity 核数)` 的预期一致;③ 全程 `dumpsys meminfo` Native Heap
峰值 ≤ ~700MB;④ `.mov/.mxf` 老路径回归。

### L21 并行化交付后真机「卡在转码中(初始化)」—— 线程数数错容器导致死锁(2026-10-05)
用户(m01204):「审查一遍刚才的更改,实机测试进程卡在转码中(初始化)阶段」。

**根因**:`dngframeq.h:97 int thread_count() const { return (int)m_workers.size(); }`
POSIX 路径下线程全在 `m_threads`,`m_workers`(std::thread 退路)恒空 → `thread_count()` 恒 0
→ `start()` 里 `if (thread_count() == 0) m_no_workers = true;` **恒真**
→ `next()` 每次都先在消费线程上跑 `produce_one()`。环满时它阻塞在 `m_cv_prod` 等空位,
而 ready 的队首帧本该由它经 `m_cv_cons` 取走;worker 同样在等空位
→ **全部线程睡在 cv_prod,死锁**。第一个 `next()` 就挂,所以进度永远停在 init。

**为什么测试没抓到**:单测的 decode 只有几 ms(消费线程总能立刻拿到空位);bench 用的
WSL 侧头文件副本没跟仓库同步,跑的是老版本。

**修复**(3 处,`dngframeq.h`):① `thread_count()` = `m_workers.size() + m_threads.size()`;
② depth 钳制补回 `if (m_total && m_depth > m_total) m_depth = m_total;`(原行是 no-op);
③ `next()` 改 `wait_for(15s)` + 谓词循环,静默挂死时每 15s 打一条带现场数据的诊断。

**验证**:修前 nt∈{2,3,4,8}×2 = **8/8 死锁**(`consumed=0`、2 帧 ready 无人取、看门狗
3s 见 `thread_count()=0`);修后 nt∈{1,2,3,4,8} 全 `DONE`,12 次重复零挂,depth=8 正常,
单测 + TSan 全绿;真实 5.22GiB zip bench 1T 418ms / 2T 235ms / 4T 239ms(VmHWM 213/421/496MB),
与修前一致。`:app:assembleDebug` BUILD SUCCESSFUL,产物 APK 60,622,689 字节(10-05 12:32),
两 ABI `.so` 含 `DngFrameQueue::thread_count()` 符号与 `dngframeq: ` 诊断串。

**教训**:① 头文件里「数线程」必须同时覆盖 pthread 与 std::thread 两条路径,否则降级分支
被永久打开;② host 全绿 ≠ 真机可用 —— 慢解码(350ms/帧)才会暴露只在特定 interleaving 下
出现的死锁;③ host 侧复刻用的头文件要和仓库保持同步。
